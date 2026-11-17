// Gridline - K-112b: plugin windows in the Tracks layout (see LogicPlugins.h).
#include "LogicPlugins.h"
#include "ExportUi.h"
#include "Keys.h"
#include "../packs/PacksUi.h"
#include "LayoutStyleUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/WindowPlacement.h"
#include "../core/ClipSources.h"
#include "../core/MenuOps.h"
#include "../core/Meter.h"
#include "../core/TrackModes.h"
#include "../core/Automation.h"
#include "../engine/ClipOps.h"
#include "../engine/PlaylistTools.h"
#include "../engine/Render.h"
#include "../packs/edison/edison.h"
#include "App.h"
#include "EditorZoomUi.h"
#include "HostedBodies.h"
#include "HostedUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "TypeInMenu.h"
#include "Widgets.h"

namespace gl::logicplug {

namespace {
AutoFold gFold;  // K-112B (checkpoint 46): the automation view, worked out by autoFold
}  // namespace

// R01-10: the sound a track plays: its linked channel, else the sound of the given region (when it
// lies on the track), else of the track's first region (an audio region's channel, a pattern
// region's first channel with notes or steps). A free track is never "nothing" while it holds regions.
const Channel* trackSound(const Song& s, int track, const Clip* clip) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return nullptr;
  if (const Channel* c = trackmodes::linkedChannel(s, track)) return c;
  auto soundOf = [&s](const Clip& k) -> const Channel* {
    if (k.kind == ClipKind::Audio) return s.channel(k.source);
    if (k.kind == ClipKind::Pattern)
      if (const Pattern* p = s.pattern(k.pattern))
        for (const Channel& c : s.channels)
          if (p->notes.count(c.id) || p->steps.count(c.id)) return &c;
    return nullptr;
  };
  if (clip && clip->track == track)
    if (const Channel* c = soundOf(*clip)) return c;
  const Clip* first = nullptr;
  for (const Clip& k : s.playlist)
    if (k.track == track && (!first || k.start < first->start) && soundOf(k)) first = &k;
  if (first) return soundOf(*first);
  return s.channel(s.tracks[static_cast<size_t>(track)].sound);  // R08-07: the sound it kept
}

namespace {
struct TrackMemo {
  const Song* song = nullptr;
  uint64_t rev = ~0ull;
  std::vector<const Channel*> sound;
  std::vector<const MixerTrack*> strip;
  std::vector<char> soundDone, stripDone;
};
TrackMemo& trackMemo(const App& a) {
  static TrackMemo m;
  const Song& s = a.doc.song();
  if (m.song != &s || m.rev != a.doc.revision() || m.sound.size() != s.tracks.size()) {
    m.song = &s, m.rev = a.doc.revision();
    m.sound.assign(s.tracks.size(), nullptr), m.strip.assign(s.tracks.size(), nullptr);
    m.soundDone.assign(s.tracks.size(), 0), m.stripDone.assign(s.tracks.size(), 0);
  }
  return m;
}
}  // namespace

const Channel* trackSound(const App& a, int track) {
  TrackMemo& m = trackMemo(a);
  if (track < 0 || track >= static_cast<int>(m.sound.size())) return nullptr;
  const size_t t = static_cast<size_t>(track);
  if (!m.soundDone[t]) m.sound[t] = trackSound(a.doc.song(), track, nullptr), m.soundDone[t] = 1;
  return m.sound[t];
}

const MixerTrack* trackStrip(const App& a, int track) {
  TrackMemo& m = trackMemo(a);
  if (track < 0 || track >= static_cast<int>(m.strip.size())) return nullptr;
  const size_t t = static_cast<size_t>(track);
  if (!m.stripDone[t]) m.strip[t] = trackStrip(a.doc.song(), track), m.stripDone[t] = 1;
  return m.strip[t];
}

void keepTrackSounds(App& a, const std::set<ObjectId>& clips) {
  if (!active(a) || clips.empty()) return;
  const Song& s = a.doc.song();
  const int n = static_cast<int>(s.tracks.size());
  std::map<int, ObjectId> keep;
  for (const Clip& c : s.playlist) {
    const int t = c.track;
    if (!clips.count(c.id) || c.kind == ClipKind::Automation || t < 0 || t >= n || keep.count(t)) continue;
    if (s.tracks[static_cast<size_t>(t)].sound != kNoId || trackmodes::linkedChannel(s, t) || trackmodes::linkedMixer(s, t)) continue;
    bool stays = false;  // a region of the track that is not going
    for (const Clip& k : s.playlist) stays = stays || (k.track == t && k.kind != ClipKind::Automation && !clips.count(k.id));
    if (stays) continue;
    if (const Channel* ch = trackSound(s, t, &c)) keep[t] = ch->id;
  }
  for (const auto& [t, id] : keep) {
    const ObjectId keepId = id;
    a.doc.editPlaylistTrack(t, "keep the track's sound", [keepId](PlaylistTrack& x) { x.sound = keepId; });
  }
}

bool deleteTrack(App& a, int track) {
  const Song& s = a.doc.song();
  if (track < 0 || track >= static_cast<int>(s.tracks.size()) || s.tracks.size() <= 1) return false;
  const ObjectId master = s.mixer.empty() ? kNoId : s.mixer[0].id;
  const MixerTrack* st = trackStrip(s, track);
  ObjectId strip = st && st->id != master ? st->id : kNoId;
  // The sounds the track plays, and the ones anything else plays (other tracks, other arrangements).
  std::set<ObjectId> mine, others;
  auto sounds = [&s](const Clip& k, std::set<ObjectId>& into) {
    if (k.kind == ClipKind::Audio) into.insert(k.source);
    else if (k.kind == ClipKind::Pattern)
      if (const Pattern* p = s.pattern(k.pattern))
        for (const Channel& c : s.channels)
          if (p->notes.count(c.id) || p->steps.count(c.id)) into.insert(c.id);
  };
  if (const Channel* lc = trackmodes::linkedChannel(s, track)) mine.insert(lc->id);
  if (const Channel* kc = s.channel(s.tracks[static_cast<size_t>(track)].sound)) mine.insert(kc->id);  // R08-07
  for (const Clip& k : s.playlist) sounds(k, k.track == track ? mine : others);
  for (int i = 0; i < static_cast<int>(s.arrangements.size()); ++i)
    if (i != s.currentArrangement)
      for (const Clip& k : s.arrangements[static_cast<size_t>(i)].playlist) sounds(k, others);
  std::vector<ObjectId> drop;
  for (const ObjectId id : mine) {
    const Channel* c = s.channel(id);
    const int linkedTo = trackmodes::instrumentTrackOf(s, id);
    if (c && !others.count(id) && (linkedTo < 0 || linkedTo == track)) drop.push_back(id);
  }
  // The strip goes only when nothing but the dropped sounds plays or sends into it, and no other track uses it.
  if (strip != kNoId) {
    for (const Channel& c : s.channels)
      if (c.mixerTrack == strip && std::find(drop.begin(), drop.end(), c.id) == drop.end()) strip = kNoId;
    for (const MixerTrack& m : s.mixer)
      for (const Send& sd : m.sends)
        if (sd.to == strip && m.id != strip) strip = kNoId;
    for (int t = 0; t < static_cast<int>(s.tracks.size()) && strip != kNoId; ++t)
      if (t != track)
        if (const MixerTrack* o = trackStrip(s, t); o && o->id == strip) strip = kNoId;
  }
  std::set<ObjectId> owners(drop.begin(), drop.end());
  if (strip != kNoId) owners.insert(strip);
  std::vector<int> shownHere;  // R08-03: automation-only tracks out of sight (lanes or the folder) that may empty
  {
    const AutoFold& f = autoFold(a);
    for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t)
      if (t != track && (f.isHidden(t) || f.isMember(t))) shownHere.push_back(t);
  }
  a.doc.beginGroup("delete track");
  ops::deletePlaylistTrack(a.doc, track, true);
  // Their automation: the data aimed at the dropped strip or sounds, and the regions that play it
  // (only data no other arrangement plays).
  std::set<ObjectId> autos;
  for (const AutomationClip& ac : a.doc.song().automations)
    if (owners.count(ac.owner)) {
      int here = 0;
      for (const Clip& k : a.doc.song().playlist) here += k.kind == ClipKind::Automation && k.source == ac.id ? 1 : 0;
      if (ops::clipSourceUses(a.doc.song(), ClipKind::Automation, ac.id) == here) autos.insert(ac.id);
    }
  if (!autos.empty())
    a.doc.editSong("delete track automation", [&autos](Song& x) {
      x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(),
                                      [&](const Clip& k) { return k.kind == ClipKind::Automation && autos.count(k.source); }),
                       x.playlist.end());
      x.automations.erase(std::remove_if(x.automations.begin(), x.automations.end(), [&](const AutomationClip& ac) { return autos.count(ac.id) > 0; }),
                          x.automations.end());
    });
  if (!drop.empty()) ops::removeChannels(a.doc, drop);
  if (strip != kNoId) a.doc.removeMixerTrack(strip);
  // R08-03: an automation-only track shown as this track's lane (or in the folder) that held only
  // the automation just deleted goes too: nothing is left behind as an empty row.
  for (auto it = shownHere.rbegin(); it != shownHere.rend(); ++it) {
    const int t = *it > track ? *it - 1 : *it;
    const Song& now = a.doc.song();
    if (t < 0 || t >= static_cast<int>(now.tracks.size()) || now.tracks.size() <= 1) continue;
    bool used = false;
    for (const Clip& k : now.playlist) used = used || k.track == t;
    for (const Arrangement& ar : now.arrangements)
      for (const Clip& k : ar.playlist) used = used || k.track == t;
    if (!used) ops::deletePlaylistTrack(a.doc, t, true);
  }
  a.doc.endGroup();
  return true;
}

const MixerTrack* trackStrip(const Song& s, int track) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size()) || s.mixer.empty()) return nullptr;
  const ObjectId master = s.mixer[0].id;
  if (const MixerTrack* mt = trackmodes::linkedMixer(s, track); mt && mt->id != master) return mt;
  std::vector<ObjectId> outs;  // the mixer tracks the track's sounds play into, in order
  auto add = [&](const Channel* c) {
    if (c && c->mixerTrack != master && s.mixerTrack(c->mixerTrack) && std::find(outs.begin(), outs.end(), c->mixerTrack) == outs.end()) outs.push_back(c->mixerTrack);
  };
  if (const Channel* c = trackmodes::linkedChannel(s, track)) add(c);
  if (outs.empty())
    for (const Clip& k : s.playlist) {
      if (k.track != track) continue;
      if (k.kind == ClipKind::Audio) add(s.channel(k.source));
      else if (k.kind == ClipKind::Pattern)
        if (const Pattern* p = s.pattern(k.pattern))
          for (const Channel& c : s.channels)
            if (p->notes.count(c.id) || p->steps.count(c.id)) add(&c);
    }
  if (outs.empty()) add(s.channel(s.tracks[static_cast<size_t>(track)].sound));  // R08-07: the sound it kept
  if (outs.empty()) {  // checkpoint 48: a bus track (automation of a bus) shows the bus's strip
    if (gFold.on && gFold.busStrip.size() == s.tracks.size() && gFold.busStrip[static_cast<size_t>(track)] != kNoId) {
      const ObjectId bus = gFold.busStrip[static_cast<size_t>(track)];
      bool theirs = false;  // (the view is from this song: the track holds that bus's automation)
      for (const Clip& k : s.playlist)
        if (k.track == track && k.kind == ClipKind::Automation)
          if (const AutomationClip* ac = s.automation(k.source); ac && ac->owner == bus) theirs = true;
      if (theirs) return s.mixerTrack(bus);
    }
    return nullptr;
  }
  if (outs.size() == 1) return s.mixerTrack(outs.front());
  // Several: the first mixer track (not the master) every one of them sends its sound into.
  const MixerTrack* first = s.mixerTrack(outs.front());
  for (const Send& sd : first->sends) {
    if (sd.sidechain || sd.to == master) continue;
    bool all = true;
    for (const ObjectId o : outs) {
      const MixerTrack* m = s.mixerTrack(o);
      bool sends = false;
      for (const Send& q : m->sends) sends = sends || (q.to == sd.to && !q.sidechain);
      all = all && sends;
    }
    if (all)
      if (const MixerTrack* bus = s.mixerTrack(sd.to)) return bus;
  }
  return first;
}

// The track the layout is about: the first selected region's, else the first selected track, else
// (as the reference always has one track selected) the track that plays the selected channel, else
// the first track.
int focusTrack(const App& a) {
  const Song& s = a.doc.song();
  for (const Clip& c : s.playlist)
    if (a.playlist.selected.count(c.id)) return c.track;
  if (!a.playlist.trackSel.empty()) return *a.playlist.trackSel.begin();
  const int n = static_cast<int>(s.tracks.size());
  if (a.selChannel != kNoId)
    for (int t = 0; t < n; ++t)
      if (const Channel* c = trackSound(a, t); c && c->id == a.selChannel) return t;
  return n > 0 ? 0 : -1;
}

bool midiRecordable(const App& a, int track) {
  const Song& s = a.doc.song();
  if (!active(a) || track < 0 || track >= static_cast<int>(s.tracks.size())) return false;
  if (s.tracks[static_cast<size_t>(track)].mode == TrackMode::Audio || autoFold(a).takesNoRegions(track)) return false;
  return trackSound(a, track) != nullptr;
}

int armedTrack(const App& a) {
  if (a.playlist.midiArm.empty() || !active(a)) return -1;
  const Song& s = a.doc.song();
  const int focus = focusTrack(a);  // the focus track first when it is armed (several may be)
  auto armed = [&](int t) {
    const Channel* c = midiRecordable(a, t) ? trackSound(a, t) : nullptr;
    return c && a.playlist.midiArm.count(c->id) > 0;
  };
  if (armed(focus)) return focus;
  for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t)
    if (armed(t)) return t;
  return -1;
}

namespace {
// R11-01: where the next Cmd+V goes while the playhead stays where the last paste left it: right
// after what was pasted, so repeated pastes lay copies end to end (the reference's paste moves on).
struct PasteRun {
  size_t count = 0;        // the clipboard (its size and first clip) when it pasted
  ObjectId first = kNoId;
  int target = -1;
  double playhead = -1;    // the playhead when it pasted (it has not moved since)
  Tick next = -1;          // where the next paste starts
};
PasteRun gPasteRun;
}  // namespace

bool pasteRegions(App& a) { return pasteRegionsAt(a, -1, -1); }

bool pasteRegionsAt(App& a, int atTrack, Tick atTick) {
  PlaylistState& st = a.playlist;
  if (st.clipboard.empty()) return false;
  const Song& s = a.doc.song();
  const int n = static_cast<int>(s.tracks.size());
  Tick lo = st.clipboard.front().start, hi = lo;
  int top = st.clipboard.front().track;
  for (const Clip& c : st.clipboard) lo = std::min(lo, c.start), top = std::min(top, c.track);
  for (const Clip& c : st.clipboard) hi = std::max(hi, c.start + std::max<Tick>(1, s.clipLength(c)));
  const int target = atTrack >= 0 ? atTrack : !st.trackSel.empty() ? *st.trackSel.begin() : top;
  const AutoFold& fold = autoFold(a);
  for (const Clip& c : st.clipboard) {
    const int t = target + (c.track - top);
    const bool bus = c.kind != ClipKind::Automation && fold.takesNoRegions(t);
    if (t < 0 || t >= n || bus || !trackmodes::clipAllowed(s, c.kind, t)) {
      a.notify(bus || fold.takesNoRegions(target) ? i18n::tr("A bus track holds its automation only, no regions: select another track to paste onto")
               : t >= n                           ? i18n::tr("The regions would go below the last track")
                                                  : i18n::tr("Instrument tracks take MIDI regions only"),
               true);
      return false;
    }
  }
  const double playhead = a.engine.position();
  Tick at = atTick >= 0 ? atTick : std::max<Tick>(0, static_cast<Tick>(std::llround(playhead)));
  PasteRun& run = gPasteRun;
  const bool again = atTick < 0 && run.next >= 0 && run.playhead == playhead && run.target == target &&
                     run.count == st.clipboard.size() && run.first == st.clipboard.front().id;
  if (again) at = run.next;  // R11-01: the same paste again: after the last copy, never on top of it
  // R11-01: each region plays the sound of the track it lands on (a region belongs to its track),
  // from a pattern of its own; in No Overlap the regions under it give way.
  std::vector<pltools::PasteItem> items;
  items.reserve(st.clipboard.size());
  for (const Clip& c : st.clipboard) {
    pltools::PasteItem it;
    it.clip = c;
    it.clip.track = target + (c.track - top);
    it.clip.start = at + (c.start - lo);
    if (c.kind == ClipKind::Pattern && it.clip.track != c.track) {
      const Channel* from = c.track >= 0 && c.track < n ? trackSound(s, c.track, &c) : nullptr;
      const Channel* to = trackSound(a, it.clip.track);
      if (from && to) it.fromSound = from->id, it.toSound = to->id;
    }
    items.push_back(it);
  }
  const bool noOverlap = static_cast<int>(a.settings.getNumber("ui.logicDragMode")) == 1;
  std::vector<ObjectId> added;
  a.doc.editSong("paste regions", [&](Song& x) { added = pltools::pasteOwnCopies(x, items, noOverlap); });
  st.selected = std::set<ObjectId>(added.begin(), added.end());
  if (added.empty()) return false;
  if (atTick < 0) {
    run.count = st.clipboard.size(), run.first = st.clipboard.front().id;
    run.target = target, run.playhead = playhead, run.next = at + (hi - lo);
  }
  return true;
}

ObjectId liveChannel(const App& a) {
  if (!active(a) || (a.playlist.midiArm.empty() && a.playlist.midiMonitor.empty())) return a.selChannel;
  const Song& s = a.doc.song();
  if (const int t = armedTrack(a); t >= 0)
    if (const Channel* c = trackSound(a, t)) return c->id;
  for (const ObjectId id : a.playlist.midiMonitor)
    if (s.channel(id)) return id;
  return a.selChannel;
}

std::vector<std::pair<int, ObjectId>> armedTracks(const App& a) {
  std::vector<std::pair<int, ObjectId>> out;
  if (a.playlist.midiArm.empty() || !active(a)) return out;
  auto add = [&](int t) {
    const Channel* c = midiRecordable(a, t) ? trackSound(a, t) : nullptr;
    if (!c || !a.playlist.midiArm.count(c->id)) return;
    for (const auto& [ot, oc] : out) {
      if (ot == t || oc == c->id) return;  // one take per sound
    }
    out.emplace_back(t, c->id);
  };
  if (const int first = armedTrack(a); first >= 0) add(first);
  for (int t = 0; t < static_cast<int>(a.doc.song().tracks.size()); ++t) add(t);
  return out;
}

std::vector<ObjectId> liveChannels(const App& a) {
  std::vector<ObjectId> out{liveChannel(a)};
  if (!active(a) || (a.playlist.midiArm.empty() && a.playlist.midiMonitor.empty())) return out;
  const Song& s = a.doc.song();
  auto add = [&](ObjectId id) {
    if (id != kNoId && s.channel(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  };
  for (const auto& [t, id] : armedTracks(a)) add(id);  // an armed track is heard, as the reference's
  for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) {
    const Channel* c = midiRecordable(a, t) ? trackSound(a, t) : nullptr;
    if (c && a.playlist.midiMonitor.count(c->id)) add(c->id);
  }
  return out;
}

namespace {
std::vector<std::pair<ObjectId, NoteRecorder>> gLayers;  // R11-03: the extra takes, by sound
}  // namespace

void syncRecordSteps(App& a) {
  if (gLayers.empty()) return;
  const uint64_t cur = a.doc.history().stateSerial();
  bool ours = cur != 0 && a.recorder.step() == cur;
  for (const auto& [c, r] : gLayers) ours = ours || (cur != 0 && r.step() == cur);
  if (!ours) return;  // (something else was edited since: each goes on with a step of its own)
  a.recorder.joinStep(cur);
  for (auto& [c, r] : gLayers) r.joinStep(cur);
}

void startLayerTakes(App& a, const RecordOptions& o, const RecordSetup& base, ObjectId pattern) {
  gLayers.clear();
  const auto armed = armedTracks(a);
  for (size_t i = 1; i < armed.size(); ++i) {
    RecordSetup st = base;
    st.track = armed[i].first;
    gLayers.emplace_back(armed[i].second, NoteRecorder{});
    gLayers.back().second.beginSong(a.doc, pattern, o, st);
  }
}

void endLayerTakes(App& a, double tick) {
  for (auto& [c, r] : gLayers) {
    r.end(a.doc, tick);
    syncRecordSteps(a);
  }
  gLayers.clear();
}

void followLayerTakes(App& a, double tick) {
  for (auto& [c, r] : gLayers) {
    if (a.playlist.selEnd > a.playlist.selStart) {
      r.setSongLoop(a.playlist.selStart, a.playlist.selEnd);
    } else {
      r.setSongLoop(0, 0);
    }
    r.position(a.doc, tick);
    syncRecordSteps(a);
  }
}

bool layerNote(App& a, ObjectId channel, int pitch, int velocity, bool on, double tick) {
  for (auto& [c, r] : gLayers) {
    if (c != channel || !r.active()) continue;
    if (on) {
      r.noteOn(a.doc, channel, pitch, velocity, tick);
    } else {
      r.noteOff(a.doc, channel, pitch, tick);
    }
    syncRecordSteps(a);
    return true;
  }
  return false;
}

bool layerTakesActive() { return !gLayers.empty(); }

bool recordsChannel(const App& a, ObjectId channel) {
  if (!active(a) || a.playlist.midiArm.empty()) return true;
  for (const auto& [t, id] : armedTracks(a)) {
    if (id == channel) return true;
  }
  return false;
}

int newTrackSameSettings(App& a, int track, bool withContent) {
  const Song& s0 = a.doc.song();
  if (track < 0 || track >= static_cast<int>(s0.tracks.size()) || static_cast<int>(s0.tracks.size()) >= kMaxPlaylistTracks) return -1;
  const bool audioTrack = s0.tracks[static_cast<size_t>(track)].mode == TrackMode::Audio;
  // The instrument to copy: the linked channel, else the first pattern region's sound (a free
  // track). A track of audio regions only has no instrument to copy.
  const Channel* src = trackmodes::linkedChannel(s0, track);
  if (!src && !audioTrack) {
    bool audioOnly = true;
    for (const Clip& k : s0.playlist)
      if (k.track == track && k.kind == ClipKind::Pattern) audioOnly = false;
    if (!audioOnly) src = trackSound(s0, track, nullptr);
  }
  const ObjectId srcId = src ? src->id : kNoId;
  a.doc.beginGroup("new track with the same settings");
  const bool ok = ops::clonePlaylistTrack(a.doc, track, withContent);
  if (ok && srcId != kNoId && !audioTrack) {
    const Channel copy = *a.doc.song().channel(srcId);
    const ObjectId srcMixer = copy.mixerTrack;
    const int at = a.doc.song().channelIndex(srcId);
    const ObjectId nid = a.doc.addChannel(copy.name, copy.plugin.type, at + 1);
    if (nid != kNoId) {
      a.doc.editChannel(nid, "new track with the same settings", [&](Channel& c) {
        const ObjectId keep = c.id;
        c = copy;
        c.id = keep;
      });
      std::string why;
      if (trackmodes::makeInstrumentTrack(a.doc, track + 1, nid, &why)) {
        // Its own insert (makeInstrumentTrack moved it off a shared one) gets the same plugins.
        const Song& s = a.doc.song();
        const Channel* nc = s.channel(nid);
        const MixerTrack* from = s.mixerTrack(srcMixer);
        if (nc && from && nc->mixerTrack != srcMixer && s.mixerTrack(nc->mixerTrack) && !s.mixer.empty() && srcMixer != s.mixer[0].id) {
          const auto slots = from->slots;
          const float vol = from->volume, pan = from->pan;
          a.doc.editMixerTrack(nc->mixerTrack, "new track with the same settings", [&](MixerTrack& m) {
            m.slots = slots;
            m.volume = vol;
            m.pan = pan;
          });
        }
      }
    }
  }
  a.doc.endGroup();
  if (!ok) return -1;
  a.playlist.trackSel = {track + 1};
  return track + 1;
}

namespace {

const theme::Palette& pal() { return theme::pal(); }

const MixerTrack* mixerOf(const Song& s, int track) {
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return nullptr;
  if (const MixerTrack* m = trackmodes::linkedMixer(s, track)) return m;
  if (const Channel* c = trackSound(s, track, nullptr)) {
    if (const MixerTrack* m = s.mixerTrack(c->mixerTrack)) return m;
    if (!s.mixer.empty()) return &s.mixer[0];  // routed straight to the master
  }
  return nullptr;
}

// Installed plugins whose own windows are open (instruments and effect slots).
std::vector<PluginTarget> openNative(const Song& s) {
  std::vector<PluginTarget> out;
  for (const Channel& c : s.channels)
    if (isHostedType(c.plugin.type) && hostedEditorOpen(PluginTarget{c.id, -1})) out.push_back(PluginTarget{c.id, -1});
  for (const MixerTrack& m : s.mixer)
    for (int k = 0; k < kEffectSlots; ++k)
      if (isHostedType(m.slots[static_cast<size_t>(k)].plugin.type) && hostedEditorOpen(PluginTarget{m.id, k})) out.push_back(PluginTarget{m.id, k});
  return out;
}

// Multi: the open windows follow the selected strip, row by row (each effect slot, the
// instrument). A built-in row with nothing on the new strip stays open, empty, naming the track
// and the slot; an installed plugin's window closes and its row comes back with a strip that has
// a plugin there.
int gEmptyRow = -1;            // the built-in effect window's row while it shows the empty state
ObjectId gEmptyTrack = kNoId;  // and the mixer track it names
std::vector<int> gNativeRows;  // installed plugins' rows waiting for a strip with a plugin there

void follow(App& a, int track) {
  const Song& s = a.doc.song();
  const MixerTrack* to = mixerOf(s, track);
  const Channel* inst = trackmodes::linkedChannel(s, track);
  std::vector<int> builtinRows, nativeRows = gNativeRows;
  bool instrument = a.settingsChannel != kNoId;
  if (a.fxEditor.first != kNoId) builtinRows.push_back(a.fxEditor.second);
  if (gEmptyRow >= 0) builtinRows.push_back(gEmptyRow);
  for (const PluginTarget& t : openNative(s)) {
    if (t.isChannel()) {
      if (inst && t.owner == inst->id) continue;  // already the new strip's
      instrument = true;
    } else {
      if (to && t.owner == to->id) continue;
      nativeRows.push_back(t.slot);
    }
    closeHostedEditor(a, t, "the selected strip changed (linked plugin windows)");
  }
  gNativeRows.clear();
  if (instrument && inst && a.settingsChannel != inst->id) a.settingsChannel = inst->plugin.type.empty() ? kNoId : inst->id;
  auto has = [&](int k) { return to && k >= 0 && k < kEffectSlots && !to->slots[static_cast<size_t>(k)].plugin.type.empty(); };
  for (int k : nativeRows) {
    if (has(k) && isHostedType(to->slots[static_cast<size_t>(k)].plugin.type)) {
      a.fxEditor = {to->id, k};
      routePluginWindows(a);  // opens its own window
    } else if (!has(k)) {
      gNativeRows.push_back(k);  // back when a strip has a plugin in this row
    }
  }
  for (int k : builtinRows) {
    if (has(k)) {
      a.fxEditor = {to->id, k};
      gEmptyRow = -1;
      if (isHostedType(to->slots[static_cast<size_t>(k)].plugin.type)) routePluginWindows(a);
    } else {
      a.fxEditor = {kNoId, -1};
      gEmptyRow = k;
      gEmptyTrack = to ? to->id : kNoId;
    }
  }
}

// Single: one window, which switches to each plugin opened. Built-in effects already share one
// window; an installed plugin's window opening closes the others (and the built-in window).
std::vector<uint64_t> gNativeSeen;
std::pair<ObjectId, int> gLastFx{kNoId, -1};

void single(App& a) {
  const Song& s = a.doc.song();
  const std::vector<PluginTarget> open = openNative(s);
  const PluginTarget* fresh = nullptr;
  for (const PluginTarget& t : open)
    if (std::find(gNativeSeen.begin(), gNativeSeen.end(), t.key()) == gNativeSeen.end()) fresh = &t;
  const bool fxChanged = a.fxEditor != gLastFx && a.fxEditor.first != kNoId;
  if (fresh) {  // the newest native window stays
    const PluginTarget keep = *fresh;
    for (const PluginTarget& t : open)
      if (t.key() != keep.key()) closeHostedEditor(a, t, "another plugin was opened (single plugin window)");
    if (a.fxEditor.first != kNoId && !(keep.owner == a.fxEditor.first && keep.slot == a.fxEditor.second)) {
      const MixerTrack* m = s.mixerTrack(a.fxEditor.first);
      if (m && a.fxEditor.second >= 0 && a.fxEditor.second < kEffectSlots && !isHostedType(m->slots[static_cast<size_t>(a.fxEditor.second)].plugin.type))
        a.fxEditor = {kNoId, -1};
    }
  } else if (fxChanged) {  // a built-in effect window opened: the native ones close
    for (const PluginTarget& t : open)
      if (!(t.owner == a.fxEditor.first && t.slot == a.fxEditor.second)) closeHostedEditor(a, t, "another plugin was opened (single plugin window)");
  }
  gLastFx = a.fxEditor;
  gNativeSeen.clear();
  for (const PluginTarget& t : openNative(s)) gNativeSeen.push_back(t.key());
}

// ---- hide / show ----
// The built-in windows skip drawing while hidden (their targets stay). Installed plugins' own
// windows go through the shared, layout-neutral hide / show the Live branch (k112c-live) is
// writing; this branch does not have it yet, so nativeHide is a stub wired to it at the merge.
bool gHidden = false;
ObjectId gHiddenChannel = kNoId;
std::pair<ObjectId, int> gHiddenFx{kNoId, -1};

// At the merge with k112c-live (482f48e3, HostedUi.h), the body becomes:
//   for (const PluginTarget& t : openHostedEditorTargets()) setHostedEditorHidden(t, hide, kHideAll);
// and frame() calls setPluginWindowPolicy every frame while the layout is on screen:
//   {managed = true, openOnLoad = true, several = link != 1, selectedOnly = false,
//    channel / mixer = the focus track's}
// so Single's "opening one closes the others" for installed plugins moves to the policy (single()
// keeps the built-in window's side), and Follow keeps its row-by-row rule here.
void nativeHide(App& a, bool hide) {
  (void)a;
  (void)hide;
}

void show(App& a) {
  gHidden = false;
  nativeHide(a, false);
}

// ---- the saved states (Compare) ----
std::map<uint64_t, PluginState> gSaved;  // by PluginTarget::key()
uint64_t gSavedRev = ~0ull;
const Song* gSavedSong = nullptr;
// Compare runs on the A/B machinery (D4): the spare slot holds the saved state, and "on B" means
// the saved state is showing.

void snapshot(const App& a) {
  const Song& s = a.doc.song();
  gSaved.clear();
  for (const Channel& c : s.channels) gSaved[PluginTarget{c.id, -1}.key()] = c.plugin;
  for (const MixerTrack& m : s.mixer)
    for (int k = 0; k < kEffectSlots; ++k)
      if (!m.slots[static_cast<size_t>(k)].plugin.type.empty()) gSaved[PluginTarget{m.id, k}.key()] = m.slots[static_cast<size_t>(k)].plugin;
}

// ---- the plugin clipboard ----
PluginState gClip;
bool gHasClip = false;

std::vector<std::string> controlsTypes(const App& a) {
  std::vector<std::string> out;
  std::istringstream in(a.settings.getString("ui.logicControlsTypes"));
  for (std::string l; std::getline(in, l);)
    if (!l.empty()) out.push_back(l);
  return out;
}

void setControlsView(App& a, const std::string& type, bool on) {
  std::vector<std::string> v = controlsTypes(a);
  v.erase(std::remove(v.begin(), v.end(), type), v.end());
  if (on) v.push_back(type);
  std::string joined;
  for (const std::string& t : v) joined += t + "\n";
  a.settings.set("ui.logicControlsTypes", joined);
}

}  // namespace

bool active(const App& a) { return layoutui::active() == layout::Preset::Logic && layoutui::shellBuilt(a, layout::Preset::Logic); }

int link(const App& a) { return std::clamp(static_cast<int>(a.settings.getNumber("ui.logicPluginLink")), 0, 2); }
void setLink(App& a, int mode) { a.settings.set("ui.logicPluginLink", std::clamp(mode, 0, 2)); }

// Off (D5): each built-in effect stays in a window of its own. The effect window shows the newest
// one (App::fxEditor, as in every layout); the ones it showed before stay open here. Never in
// another layout (Studio keeps exactly one effect window).
namespace {
std::vector<std::pair<ObjectId, int>> gMoreFx;
std::pair<ObjectId, int> gPrevFx{kNoId, -1};
// K-297 / R02-09: where each window stood as the effect window, so it stays there (and as large)
// when the next effect takes the effect window; it jumped up over the control bar and grew.
std::map<std::pair<ObjectId, int>, ImVec2> gMorePos;

void trackMoreFx(App& a, int mode) {
  if (mode != 0) {
    gMoreFx.clear(), gMorePos.clear();
    gPrevFx = a.fxEditor;
    return;
  }
  const Song& s = a.doc.song();
  auto builtIn = [&](const std::pair<ObjectId, int>& f) {
    const MixerTrack* m = s.mixerTrack(f.first);
    return m && f.second >= 0 && f.second < kEffectSlots && !m->slots[static_cast<size_t>(f.second)].plugin.type.empty() &&
           !isHostedType(m->slots[static_cast<size_t>(f.second)].plugin.type);
  };
  if (a.fxEditor.first != kNoId && gPrevFx.first != kNoId && a.fxEditor != gPrevFx && builtIn(gPrevFx) &&
      std::find(gMoreFx.begin(), gMoreFx.end(), gPrevFx) == gMoreFx.end()) {
    gMoreFx.push_back(gPrevFx);
    // K-297: it stays where the effect window showed it (K-301 R02-09: it jumped to the centre and up over the control bar).
    if (ImGuiWindow* w = ImGui::FindWindowByName("###fxeditor"); w && w->WasActive && !w->Hidden) {
      gMorePos[gPrevFx] = w->Pos;
      ImGui::SetWindowPos(w, ImVec2(w->Pos.x + 28, w->Pos.y + 28), ImGuiCond_Always);  // the new one cascades (G-218)
    } else {
      gMorePos.erase(gPrevFx);
    }
  }
  gMoreFx.erase(std::remove_if(gMoreFx.begin(), gMoreFx.end(), [&](const auto& f) { return f == a.fxEditor || !builtIn(f); }), gMoreFx.end());
  gPrevFx = a.fxEditor;
}
}  // namespace

void drawMoreFx(App& a) {
  if (gMoreFx.empty() || gHidden || !active(a) || link(a) != 0) return;
  const Song& s = a.doc.song();
  for (size_t i = 0; i < gMoreFx.size();) {
    const auto f = gMoreFx[i];
    const MixerTrack* m = s.mixerTrack(f.first);
    const PluginInfo* info = m ? findPlugin(m->slots[static_cast<size_t>(f.second)].plugin.type) : nullptr;
    if (!info) {
      gMoreFx.erase(gMoreFx.begin() + static_cast<long>(i));
      continue;
    }
    const std::string title = mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(m->id)))) + "###fxmore" + std::to_string(m->id) + "_" +
                              std::to_string(f.second);
    float width = info->editorWidth > 0 ? info->editorWidth : (info->type == "gl.eq" ? 780.0f : 600.0f);
    if (info->editorMinWidth > 0) {  // R02-09: fitted to the main window as the effect window fits it
      const ImVec2 area = ui::EditorArea();
      width = std::floor(place::fitEditor(width, info->editorMinWidth, 24.0f + ImGui::GetStyle().ScrollbarSize, area.x, area.y).content);
    }
    // R02-09: drawn at the editor size the effect window had (it grew to full size once it left)
    const std::string zkey = "fxmore" + std::to_string(m->id) + "_" + std::to_string(f.second);
    const bool zoomable = !isHostedType(info->type);
    edzoomui::prepare(a, zkey.c_str(), info->type, width, zoomable);
    const int fitFlags = zoomable ? edzoomui::nextWindowSize(zkey.c_str()) : 0;
    bool open = true;
    if (const auto at = gMorePos.find(f); at != gMorePos.end()) {
      ImGui::SetNextWindowPos(at->second, ImGuiCond_Appearing);  // K-297: where it was
    } else {
      const ImGuiViewport* vp = ImGui::GetMainViewport();
      const float step = 28.0f * static_cast<float>(i + 1);
      ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f - step, vp->WorkPos.y + vp->WorkSize.y * 0.42f - step), ImGuiCond_Appearing,
                              ImVec2(0.5f, 0.5f));
    }
    // An earlier window goes behind the effect window (the newest stays in front), cascaded.
    const bool appearing = ImGui::FindWindowByName(title.c_str()) == nullptr || !ImGui::FindWindowByName(title.c_str())->WasActive;
    bool vis = false;
    {  // K-112b: the title bar's underline (drawn by Begin from the frame outline) stays as in every layout
      const layoutui::BaseBorderScope border;
      vis = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | fitFlags);
    }
    if (vis) {
      ui::KeepWindowInViewport();  // K-297: between the control bar and the window's bottom, as the effect window
      BodyFrame bf{width, zoomable, ""};
      bf.zoomKey = zkey.c_str();
      drawEffectEditorBody(a, f.first, f.second, bf);
    }
    gMorePos[f] = ImGui::GetWindowPos();  // K-297: hidden (V) and shown again, it comes back where it was left
    ImGui::End();
    if (appearing)
      if (ImGuiWindow* front = ImGui::FindWindowByName("###fxeditor")) ImGui::BringWindowToDisplayFront(front);
    if (!open) {
      gMorePos.erase(f);
      gMoreFx.erase(gMoreFx.begin() + static_cast<long>(i));
      continue;
    }
    ++i;
  }
}

void closeMoreFx() { gMoreFx.clear(), gMorePos.clear(); }

namespace {
uint64_t gFoldRev = ~0ull;
const Song* gFoldSong = nullptr;
int gFoldKey = -1;

// A track holding automation regions and nothing else, with no sound or strip of its own.
bool automationOnly(const Song& s, int tr, const std::vector<int>& kinds) {
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(tr)];
  return kinds[static_cast<size_t>(tr)] == 1 && t.groupSize == 0 && !trackmodes::linkedChannel(s, tr) && !trackmodes::linkedMixer(s, tr);
}
// Per track: 0 empty, 1 automation regions only, 2 anything else.
std::vector<int> trackKinds(const Song& s) {
  std::vector<int> k(s.tracks.size(), 0);
  for (const Clip& c : s.playlist)
    if (c.track >= 0 && static_cast<size_t>(c.track) < k.size()) {
      int& v = k[static_cast<size_t>(c.track)];
      v = c.kind == ClipKind::Automation ? std::max(v, 1) : 2;
    }
  return k;
}
int ownerTrackK(const Song& s, ObjectId owner, const std::vector<int>& kinds) {
  if (owner == kNoId || s.mixer.empty() || owner == s.mixer[0].id) return -1;  // never the master
  const int n = static_cast<int>(s.tracks.size());
  std::vector<std::set<ObjectId>> plays(static_cast<size_t>(n));
  for (const Clip& k : s.playlist) {
    if (k.track < 0 || k.track >= n) continue;
    if (k.kind == ClipKind::Audio) plays[static_cast<size_t>(k.track)].insert(k.source);
    else if (k.kind == ClipKind::Pattern)
      if (const Pattern* p = s.pattern(k.pattern))
        for (const Channel& c : s.channels)
          if (p->notes.count(c.id) || p->steps.count(c.id)) plays[static_cast<size_t>(k.track)].insert(c.id);
  }
  std::vector<int> own, routed;
  for (int tr = 0; tr < n; ++tr) {
    if (automationOnly(s, tr, kinds)) continue;
    std::set<ObjectId>& p = plays[static_cast<size_t>(tr)];
    if (const Channel* lc = trackmodes::linkedChannel(s, tr)) p.insert(lc->id);
    if (p.empty() && s.channel(s.tracks[static_cast<size_t>(tr)].sound)) p.insert(s.tracks[static_cast<size_t>(tr)].sound);  // R08-07
    const MixerTrack* strip = trackStrip(s, tr);
    if (p.count(owner) || (strip && strip->id == owner)) {
      own.push_back(tr);
      continue;
    }
    for (const ObjectId id : p)
      if (const Channel* c = s.channel(id); c && c->mixerTrack == owner) {
        routed.push_back(tr);
        break;
      }
  }
  if (own.size() > 1) {
    // R11-02: a sound heard on several tracks (a region pasted or moved onto another track) still
    // belongs to the one track it is the sound of (its first region's, or its own): its curves stay
    // on that track's lane instead of falling into the Automation folder.
    int mine = -1, count = 0;
    for (const int tr : own) {
      const Channel* c = trackSound(s, tr, nullptr);
      if (c && c->id == owner) mine = tr, ++count;
    }
    return count == 1 ? mine : -1;
  }
  if (!own.empty()) return own.front();
  return routed.size() == 1 ? routed.front() : -1;
}
}  // namespace

int ownerTrack(const Song& s, ObjectId owner) { return ownerTrackK(s, owner, trackKinds(s)); }

// K-112B (R12-05): one with regions, a mode, a name, links, a shown or chosen automation lane, a
// group, an icon, or a setting changed from a new track's; a group's tracks count with it.
int lastUsedTrack(const Song& s) {
  int last = -1;
  for (const Clip& c : s.playlist) {
    last = std::max(last, c.track);
  }
  const PlaylistTrack plain;
  for (int t = static_cast<int>(s.tracks.size()) - 1; t > last; --t) {
    const PlaylistTrack& x = s.tracks[static_cast<size_t>(t)];
    const bool used = x.mode != TrackMode::Unassigned || !x.name.empty() || x.channel != kNoId || x.mixer != kNoId || x.laneShown ||
                      x.laneSet || x.groupSize > 0 || x.icon != 0 || x.flIcon != 0 || x.locked || x.muted || x.color != plain.color ||
                      x.height != plain.height;
    if (used) {
      last = t;
      break;
    }
  }
  const int n = static_cast<int>(s.tracks.size());
  for (int t = 0; t <= last && t < n; ++t) {
    last = std::max(last, std::min(t + s.tracks[static_cast<size_t>(t)].groupSize, n - 1));
  }
  return std::min(last, n - 1);
}

const AutoFold& autoFold(const App& a) {
  const bool on = active(a) && a.settings.getBool("ui.logicAutoAsLanes");
  const bool open = a.settings.getBool("ui.logicAutoFolderOpen");
  const Song& s = a.doc.song();
  const int key = (on ? 1 : 0) | (open ? 2 : 0);
  if (key == gFoldKey && gFoldRev == a.doc.revision() && gFoldSong == &s && gFold.hidden.size() == s.tracks.size() &&
      gFold.clipTarget.size() == s.playlist.size())
    return gFold;
  gFoldKey = key, gFoldRev = a.doc.revision(), gFoldSong = &s;
  AutoFold f;
  f.on = on, f.open = open;
  f.hidden.assign(s.tracks.size(), 0);
  f.member.assign(s.tracks.size(), 0);
  f.clipTarget.assign(s.playlist.size(), -1);
  f.busStrip.assign(s.tracks.size(), kNoId);
  if (on) {
    const std::vector<int> kinds = trackKinds(s);
    std::map<ObjectId, int> byOwner;  // (owner -> track, worked out once each)
    std::map<ObjectId, int> busTrack;  // checkpoint 48: bus -> the track that shows its automation
    for (int tr = 0; tr < static_cast<int>(s.tracks.size()); ++tr) {
      if (!automationOnly(s, tr, kinds)) continue;
      bool all = true;
      for (size_t ci = 0; ci < s.playlist.size(); ++ci) {
        const Clip& c = s.playlist[ci];
        if (c.track != tr) continue;
        const AutomationClip* ac = s.automation(c.source);
        int to = -1;
        if (ac) {
          auto it = byOwner.find(ac->owner);
          if (it == byOwner.end()) it = byOwner.emplace(ac->owner, ownerTrackK(s, ac->owner, kinds)).first;
          to = it->second;
        }
        f.clipTarget[ci] = to;
        all = all && to >= 0;
      }
      if (all) {
        f.hidden[static_cast<size_t>(tr)] = 1;
        continue;
      }
      // Checkpoint 48: automation of one bus (or an effect on it) that no single track owns shows on
      // that bus's track, as the reference shows a bus's automation on its aux track: the first such
      // automation-only track becomes the bus's track (a row with the bus's strip and the curves on
      // its lane); the bus's other automation-only tracks show on that one's lane.
      ObjectId bus = kNoId;
      bool one = true;
      for (const Clip& c : s.playlist) {
        if (c.track != tr) continue;
        const AutomationClip* ac = s.automation(c.source);
        const ObjectId o = ac ? ac->owner : kNoId;
        if (bus == kNoId) bus = o;
        one = one && o == bus;
      }
      if (one && bus != kNoId && s.mixerTrack(bus) && !s.mixer.empty() && bus != s.mixer[0].id) {
        if (auto it = busTrack.find(bus); it == busTrack.end()) {
          busTrack.emplace(bus, tr);
          f.busStrip[static_cast<size_t>(tr)] = bus;
          for (size_t ci = 0; ci < s.playlist.size(); ++ci)
            if (s.playlist[ci].track == tr) f.clipTarget[ci] = -1;
        } else {
          f.hidden[static_cast<size_t>(tr)] = 1;
          for (size_t ci = 0; ci < s.playlist.size(); ++ci)
            if (s.playlist[ci].track == tr) f.clipTarget[ci] = it->second;
        }
        continue;
      }
      f.member[static_cast<size_t>(tr)] = 1, f.folder.push_back(tr);
    }
    for (size_t ci = 0; ci < s.playlist.size(); ++ci)  // (a folder track's curves stay its own)
      if (const int tr = s.playlist[ci].track; tr >= 0 && static_cast<size_t>(tr) < f.member.size() && f.member[static_cast<size_t>(tr)])
        f.clipTarget[ci] = -1;
    f.spare.assign(s.tracks.size(), 0);
    for (size_t tr = static_cast<size_t>(std::max(0, lastUsedTrack(s) + 1)); tr < s.tracks.size(); ++tr) {
      f.spare[tr] = tr > 0 ? 1 : 0;
    }
  }
  gFold = std::move(f);
  return gFold;
}

void setAutoFolderOpen(App& a, bool open) { a.settings.set("ui.logicAutoFolderOpen", open); }

Tick projectEnd(const App& a) {
  static uint64_t rev = ~0ull;
  static const Song* song = nullptr;
  static Tick end = 0;
  const Song& s = a.doc.song();
  if (rev == a.doc.revision() && song == &s) return end;
  rev = a.doc.revision(), song = &s;
  if (s.projectEnd > 0) return end = s.projectEnd;  // checkpoint 47: where the user put it
  const MeterMap mm(s);
  const Tick len = s.lengthTicks();
  const Tick b = mm.barStartAt(len);
  if (len <= 0) {  // an empty project: bar 129, the reference's new project's end
    Tick t = 0;
    for (int i = 0; i < 128; ++i) t = mm.nextBar(t);
    end = t;
  } else {
    end = std::max(mm.nextBar(0), b == len ? b : mm.nextBar(len));
  }
  return end;
}

bool laneClip(const Song& s, const Clip& c) {
  if (c.kind != ClipKind::Automation || c.track < 0 || static_cast<size_t>(c.track) >= s.tracks.size()) return false;
  // K-112B (checkpoint 46): the curves of an automation-only track shown on their targets' lanes, and
  // those of a closed Automation folder, are not regions either.
  if (gFold.on && s.tracks.size() == gFold.hidden.size() &&
      (gFold.isHidden(c.track) || (gFold.isMember(c.track) && !gFold.open) || gFold.isBus(c.track)))
    return true;
  return s.tracks[static_cast<size_t>(c.track)].laneSet;
}

ObjectId joinAudioRegions(Song& song, const std::set<ObjectId>& ids, const std::string& dir, double sr) {
  namespace ed = packs::edison;
  const Clip* first = nullptr;
  Tick a = 0, b = 0;
  for (const Clip& c : song.playlist) {
    if (!ids.count(c.id) || c.kind != ClipKind::Audio) continue;
    const Tick e = c.start + song.clipLength(c);
    if (!first || c.start < first->start) first = &c;
    b = std::max(b, e);
  }
  if (!first || sr <= 0) return kNoId;
  a = first->start;
  if (b <= a) return kNoId;
  const Clip head = *first;
  const Channel* ch0 = song.channel(head.source);
  if (!ch0) return kNoId;
  const std::string name = !head.name.empty() ? head.name : ch0->name;
  // The mix: these regions alone, each sound straight into a neutral master.
  Song r = song;
  std::vector<Clip> kept;
  std::set<ObjectId> sources;
  for (const Clip& c : r.playlist)
    if (ids.count(c.id) && c.kind == ClipKind::Audio && !c.muted) {
      Clip k = c;
      k.start -= a;
      kept.push_back(k);
      sources.insert(c.source);
    }
  if (kept.empty()) return kNoId;
  r.playlist = std::move(kept);
  r.live.areaEnd = 0;
  r.projectEnd = 0;
  for (auto& t : r.tracks) t.muted = false;
  const ObjectId master = r.mixer.empty() ? kNoId : r.mixer[0].id;
  for (Channel& c : r.channels)
    if (sources.count(c.id)) c.gain = 0.78f, c.pan = 0, c.muted = false, c.mixerTrack = master;  // (0.78: the channel's unity)
  if (!r.mixer.empty()) {
    MixerTrack& m = r.mixer[0];
    m.volume = 0.8f, m.pan = 0, m.stereoSep = 0, m.depth = 0, m.focus = 0, m.muted = false;
    m.eqGain = {0.0f, 0.0f, 0.0f};
    for (auto& e : m.slots) e.enabled = false;
  }
  RenderOptions opt;
  opt.sampleRate = static_cast<int>(sr);
  const AudioBuffer buf = renderSong(r, opt);
  TempoMap map;
  map.build(r);
  const auto n = static_cast<int64_t>(std::llround(map.secondsAt(static_cast<double>(b - a)) * sr));
  if (n <= 0) return kNoId;
  ed::Audio au = ed::makeAudio(sr, 2, n);
  for (int64_t i = 0; i < n && static_cast<size_t>(i) < buf.frames; ++i) {
    au.L[static_cast<size_t>(i)] = buf.at(0, static_cast<size_t>(i));
    au.R[static_cast<size_t>(i)] = buf.channels > 1 ? buf.at(1, static_cast<size_t>(i)) : buf.at(0, static_cast<size_t>(i));
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path = ed::renderPath(dir, cleanFileName(name), ed::audioHash(au));
  if (!ed::writeWavFile(path, ed::makeContent(au), 32)) return kNoId;
  // The new sound plays through the first region's strip, at its level and pan.
  Channel nc;
  nc.id = song.allocateId();
  nc.name = name;
  nc.color = ch0->color;
  nc.gain = ch0->gain, nc.pan = ch0->pan, nc.mixerTrack = ch0->mixerTrack;
  nc.plugin = defaultPluginState("gl.sampler");
  nc.plugin.data = path;
  song.channels.push_back(nc);
  song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) { return ids.count(c.id) && c.kind == ClipKind::Audio; }),
                      song.playlist.end());
  Clip c;
  c.id = song.allocateId();
  c.kind = ClipKind::Audio;
  c.source = nc.id;
  c.track = head.track;
  c.start = a;
  c.length = b - a;
  song.playlist.push_back(c);
  return c.id;
}

void frame(App& a) {
  autoFold(a);  // K-112B (checkpoint 46): off again as soon as another layout is on screen
  {  // R06-09: a round trip through the other layouts keeps the selected track and regions (the
     // project window's view, as the reference keeps it); the automation view is in the project.
    static bool wasOn = false;
    static int keepTrack = -1;
    static std::set<ObjectId> keepRegions;
    static uint64_t keepEpoch = 0;
    const bool on = active(a);
    const Song& s = a.doc.song();
    if (on && !wasOn && keepTrack >= 0 && keepEpoch == a.doc.projectEpoch() && keepTrack < static_cast<int>(s.tracks.size())) {
      std::set<ObjectId> regions;
      for (const ObjectId id : keepRegions)
        if (s.clip(id)) regions.insert(id);
      a.playlist.selected = regions;
      a.playlist.trackSel = {keepTrack};
    }
    if (on) keepTrack = focusTrack(a), keepRegions = a.playlist.selected, keepEpoch = a.doc.projectEpoch();
    wasOn = on;
  }
  // R04-04: the lower pane shows one editor. When the piano roll, the step sequencer or the mixer
  // takes it (a click on a MIDI region, a key), the audio editor leaves it: still pinned it went on
  // drawing as a big floating window over the tracks, and took the next W.
  {
    static bool wasPiano = true, wasRack = true, wasMixer = true;
    const bool opened = (a.showPiano && !wasPiano) || (a.showRack && !wasRack) || (a.showMixer && !wasMixer);
    wasPiano = a.showPiano, wasRack = a.showRack, wasMixer = a.showMixer;
    if (opened && active(a) && a.settings.getBool("ui.loupePinned")) {
      a.settings.set("ui.loupePinned", false);
      packs::closeAudioEditorSampleWindows();  // (its own window, opened while it followed the selection)
    }
  }
  {  // R01-02: the tracks area's tool is the layout's own. The Tracks layout opens with the pointer
     // (the reference's default tool: a click on empty space deselects, regions are drawn with the
     // pencil); the other layouts get back the tool they had.
    static bool was = false;
    static Tool tracksTool = Tool::Select, otherTool = Tool::Draw;
    // R05-12: the piano roll's left tool is the Pointer too (notes come from the Pencil or the
    // Command-click tool); the other layouts get theirs back.
    static Tool tracksPiano = Tool::Select, otherPiano = Tool::Draw;
    const bool on = active(a);
    if (on != was) {
      if (on) otherTool = a.playlist.tool, a.playlist.tool = tracksTool, otherPiano = a.piano.tool, a.piano.tool = tracksPiano;
      else tracksTool = a.playlist.tool, a.playlist.tool = otherTool, tracksPiano = a.piano.tool, a.piano.tool = otherPiano;
      was = on;
    }
  }
  // R01-09: Escape closes an open menu or pop-up (the tracks area's local menus, the tool and snap
  // pop-ups, a region's menu), as the reference's do; it left them open, so the next click on the
  // menu's own button only closed it. Modal dialogs and typing keep the key.
  if (active(a) && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput &&
      ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::GetTopMostPopupModal()) {
    ImGui::ClosePopupsExceptModals();
    keys::claimEscape();  // (the key is used: nothing after this closes a window with it)
  }
  // K-112D D6 (R12-06): Escape closes the built-in plugin window that has the keys (one opened from
  // Smart Controls, the mixer or the inspector), as the reference's plugin windows close. (The shared
  // handler leaves editor windows open on Escape in the other layouts.)
  if (active(a) && !keys::escapeBusy() && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
    const ImGuiWindow* nav = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
    const char* id = nav ? std::strstr(nav->Name, "###") : nullptr;
    if (id && !std::strcmp(id, "###fxeditor") && a.fxEditor.first != kNoId) {
      a.fxEditor = {kNoId, -1};
      keys::claimEscape();
    } else if (id && !std::strcmp(id, "###channelsettings") && a.settingsChannel != kNoId) {
      a.settingsChannel = kNoId;
      keys::claimEscape();
    }
  }
  // K-297: the area with key focus (the tracks area, the inspector, the lower pane's editor, a list)
  // carries a quiet accent frame, as the reference marks where the keys go: the shared frame
  // (ui::drawKeyFocusFrame, drawn for every layout but Studio at the end of the frame in main.cpp;
  // K-112D audit I5: in Smooth on the dark faces it takes the reference's blue, passed in there).
  // (R02-19, R04-07: Escape closing the dialog or tool window in front, Render and About included,
  // is the shared handler's in every layout: K-297, handleGlobalShortcuts. One Escape closes one window.)
  {  // R06-08: Stop ends recording and the Record button goes out, as the reference's does
    static bool wasPlaying = false;
    const bool playing = a.engine.playing();
    if (active(a) && wasPlaying && !playing && a.recordArmed) a.recordArmed = false;
    wasPlaying = playing;
  }
  {  // K-112B (checkpoint 46): playback stops at the Project End marker when no cycle is on, as the
     // reference's does (a recording runs on past it, and a start past it plays on).
    static double last = -1;
    const bool cycle = a.playlist.selEnd > a.playlist.selStart;
    const double pos = a.engine.position();
    if (active(a) && a.engine.playing() && a.engine.mode() == PlayMode::Song && !cycle && !a.recordArmed) {
      const double end = static_cast<double>(projectEnd(a));
      if (last >= 0 && last < end && pos >= end) {
        a.engine.stop();
        a.engine.setPosition(static_cast<Tick>(end));
      }
      last = pos;
    } else {
      last = a.engine.playing() ? pos : -1;
    }
  }
  {  // R03-06: a new or opened project closes the old one's render window, as the reference closes its windows
    static uint64_t epoch = 0;
    if (active(a) && a.doc.projectEpoch() != epoch && epoch != 0) {
      a.showExport = false;
      // R05-08: the Tracks layout has no pattern mode; a new or opened project plays its timeline
      if (a.engine.mode() == PlayMode::Pattern) a.engine.setMode(PlayMode::Song);
    }
    epoch = a.doc.projectEpoch();
  }
  if (!active(a)) {  // another layout: nothing of this runs (hidden windows come back)
    if (gHidden) show(a);
    gMoreFx.clear(), gMorePos.clear();
    gPrevFx = a.fxEditor;
    return;
  }
  // The saved states: taken whenever the project is in its saved state (opened, saved, or undone back to it).
  if (!a.doc.modified() && (a.doc.revision() != gSavedRev || &a.doc.song() != gSavedSong)) {
    snapshot(a);
    gSavedRev = a.doc.revision();
    gSavedSong = &a.doc.song();
  }
  if (gHidden && (a.settingsChannel != gHiddenChannel || a.fxEditor != gHiddenFx)) show(a);  // a plugin was opened: windows back
  const int mode = link(a);
  trackMoreFx(a, mode);
  if (mode != 2) gEmptyRow = -1, gNativeRows.clear();
  if (mode == 1 && !gHidden) single(a);
  static int last = -2;
  const int tr = focusTrack(a);
  if (tr == last) return;
  const bool first = last == -2;
  last = tr;
  if (first || tr < 0 || mode != 2 || gHidden) return;
  follow(a, tr);
}

void drawEmptyRow(App& a) {
  if (gEmptyRow < 0 || a.fxEditor.first != kNoId || gHidden || !active(a) || link(a) != 2) return;
  const Song& s = a.doc.song();
  const MixerTrack* m = s.mixerTrack(gEmptyTrack);
  const std::string track = m ? mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(m->id)))) : std::string(i18n::tr("No track"));
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(360, 160), ImGuiCond_Appearing);
  if (ImGui::Begin((track + "###fxeditor").c_str(), &open, ImGuiWindowFlags_NoDocking)) {
    char line[160];
    std::snprintf(line, sizeof line, i18n::tr("Insert %d of %s is empty"), gEmptyRow + 1, track.c_str());
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 80.0f)), line,
                   "Linked window: select a track with a plugin in this slot, or close the window");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 80.0f)));
  }
  ImGui::End();
  if (!open) gEmptyRow = -1;
}

bool hidden() { return gHidden; }

void toggleHidden(App& a) {
  if (gHidden) {
    show(a);
    return;
  }
  nativeHide(a, true);
  gHidden = true;
  gHiddenChannel = a.settingsChannel;
  gHiddenFx = a.fxEditor;
}

std::string fxWindowTitle(const Song& s, const MixerTrack& t) {
  return mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(t.id)))) + "###fxeditor";
}

void compareButton(App& a, const PluginTarget& t) {
  const Song& s = a.doc.song();
  const PluginState* st = t.state(s);
  const auto it = gSaved.find(t.key());
  const bool known = st && it != gSaved.end() && it->second.type == st->type;
  const bool differs = known && !(it->second == *st);
  bool on = known && pui::compareOnB(t);
  if (on && differs) {  // edited while showing the saved state: that is the edit now
    pui::compareSetSpare(a, t, it->second);
    on = false;
  }
  ImGui::BeginDisabled(!known || (!differs && !on));
  if (ui::TextButton(i18n::tr("Compare"), differs || on, 0,
                     on ? "Showing the settings as saved with the project: click to go back to your edit"
                        : "Lit when the settings differ from the project as saved: click to hear the saved settings")) {
    if (!on) pui::compareSetSpare(a, t, it->second);  // the other slot: the saved state
    pui::compareFlip(a, t);
  }
  ImGui::EndDisabled();
}

// K-112b: Compare for a whole track (the Smart controls bar): every plugin on it flips together
// between the edit and the project as saved. Lit when any of them differs from the saved state.
void compareTrackButton(App& a, const std::vector<PluginTarget>& ts, float width) {
  const Song& s = a.doc.song();
  bool anyKnown = false, anyDiffers = false, anyOn = false;
  for (const PluginTarget& t : ts) {
    const PluginState* st = t.state(s);
    const auto it = gSaved.find(t.key());
    if (!st || it == gSaved.end() || it->second.type != st->type) continue;
    anyKnown = true;
    const bool differs = !(it->second == *st);
    bool on = pui::compareOnB(t);
    if (on && differs) {  // edited while showing the saved state: that is the edit now
      pui::compareSetSpare(a, t, it->second);
      on = false;
    }
    anyDiffers = anyDiffers || differs;
    anyOn = anyOn || on;
  }
  ImGui::BeginDisabled(!anyKnown || (!anyDiffers && !anyOn));
  if (ui::TextButton(i18n::tr("Compare"), anyDiffers || anyOn, width,
                     anyOn ? "Showing this track's plugins as saved with the project: click to go back to your edits"
                           : "Lit when this track's plugins differ from the project as saved: click to hear them as saved")) {
    for (const PluginTarget& t : ts) {
      const PluginState* st = t.state(s);
      const auto it = gSaved.find(t.key());
      if (!st || it == gSaved.end() || it->second.type != st->type) continue;
      if (anyOn) {
        if (pui::compareOnB(t)) pui::compareFlip(a, t);
      } else if (!(it->second == *st)) {
        pui::compareSetSpare(a, t, it->second);
        pui::compareFlip(a, t);
      }
    }
  }
  ImGui::EndDisabled();
}

void copyPasteButtons(App& a, const PluginTarget& t) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return;
  if (ui::TextButton(i18n::tr("Copy"), false, 0, "Copy these settings (whole plugin state) to the plugin clipboard")) gClip = *st, gHasClip = true;
  ImGui::SameLine(0, 2);
  const bool fits = gHasClip && gClip.type == st->type;
  ImGui::BeginDisabled(!fits);
  if (ui::TextButton(i18n::tr("Paste"), false, 0, "Paste the copied settings (the same plugin only)") && fits) {
    PluginState ns = gClip;
    if (ns.data.empty()) ns.data = st->data;
    setPluginState(a, t, ns, "paste plugin settings");
  }
  ImGui::EndDisabled();
}

void viewButton(App& a, const PluginTarget& t) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return;
  const bool hosted = isHostedType(st->type);
  const bool ctl = controlsView(a, st->type);
  if (ui::TextButton(ctl ? i18n::tr("Controls") : i18n::tr("Editor"), false, 0, "The window shows the plugin's editor or a plain list of its controls; its size")) ImGui::OpenPopup("##lgview");
  if (ImGui::BeginPopup("##lgview")) {
    if (!hosted) {
      if (ImGui::MenuItem(i18n::tr("Editor"), nullptr, !ctl)) setControlsView(a, st->type, false);
      if (ImGui::MenuItem(i18n::tr("Controls"), nullptr, ctl)) setControlsView(a, st->type, true);
      ImGui::Separator();
      edzoomui::sizeMenu(a, st->type, t.isChannel() ? "channelsettings" : "fxeditor");
    } else {
      ImGui::TextDisabled("%s", i18n::tr("An installed plugin shows its own interface"));
    }
    ImGui::EndPopup();
  }
}

void linkButton(App& a) {
  const int m = link(a);
  const char* names[] = {i18n::N_("Link: off"), i18n::N_("Link: single"), i18n::N_("Link: follow")};
  if (ui::TextButton(i18n::tr(names[m]), m != 0, 0,
                     "Single: one plugin window, showing each plugin you open. Off: each plugin stays in its window. Follow (right-click): "
                     "the windows follow the selected track, slot by slot"))
    setLink(a, m == 1 ? 0 : 1);
  if (ImGui::BeginPopupContextItem("##lglink")) {
    if (ImGui::MenuItem(i18n::tr("Off"), nullptr, m == 0)) setLink(a, 0);
    if (ImGui::MenuItem(i18n::tr("Single window"), nullptr, m == 1)) setLink(a, 1);
    if (ImGui::MenuItem(i18n::tr("Follow the selected track"), nullptr, m == 2)) setLink(a, 2);
    ImGui::EndPopup();
  }
}

bool controlsView(const App& a, const std::string& type) {
  if (!active(a) || isHostedType(type)) return false;
  const std::vector<std::string> v = controlsTypes(a);
  return std::find(v.begin(), v.end(), type) != v.end();
}

static void drawControlsIn(App& a, const PluginTarget& t, float width);
void drawControls(App& a, const PluginTarget& t, float width) {
  typein::begin(a);  // K-311: right-click a row: reset, type a value
  drawControlsIn(a, t, width);
  typein::end(a);
}

static void drawControlsIn(App& a, const PluginTarget& t, float width) {
  const Song& s = a.doc.song();
  const PluginInfo* info = t.info(s);
  if (!info) return;
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float rowH = 22, nameW = std::floor(width * 0.36f), valW = 84;
  const float barW = std::max(60.0f, width - nameW - valW - 16);
  for (int k = 0; k < static_cast<int>(info->params.size()); ++k) {
    const ParamId pid = t.pid(k);
    const auto d = describeParam(s, pid);
    const auto v = getParam(s, pid);
    if (!d || !v) continue;
    ImGui::PushID(k);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float fs = ImGui::GetFontSize(), ty = std::round(p.y + (rowH - fs) * 0.5f);
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(p.x, ty), nameW - 8, P.textDim, d->name.c_str());
    const ImVec2 b0(p.x + nameW, p.y + 4);
    ImGui::SetCursorScreenPos(b0);
    ImGui::InvisibleButton("##bar", ImVec2(barW, rowH - 8));
    const bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    double n = d->toNormalized(*v);
    if (act && ImGui::GetIO().MouseDelta.x != 0) {
      n = std::clamp(n + ImGui::GetIO().MouseDelta.x / barW * (ImGui::GetIO().KeyShift ? 0.15 : 1.0), 0.0, 1.0);
      a.doc.setParam(pid, d->fromNormalized(n), true);
    }
    if (hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.doc.setParam(pid, d->def, false);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    const ImVec2 b1(b0.x + barW, b0.y + rowH - 8);
    dl->AddRectFilled(b0, b1, theme::mix(P.surface800, P.ink, 0.4f), 3.0f);
    dl->AddRectFilled(b0, ImVec2(b0.x + barW * static_cast<float>(n), b1.y), hov || act ? theme::withAlpha(P.hot, 0.8f) : theme::withAlpha(P.hot, 0.55f), 3.0f);
    const std::string val = d->format(*v);
    theme::textEllipsis(dl, a.fontMono, fs, ImVec2(b1.x + 8, ty), valW, act ? P.hot : P.text, val.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(width, 0));
    ImGui::PopID();
  }
}

// ---- Smart controls (G7) ----

namespace {
struct SmartKnob {
  PluginTarget t;
  int k;
  // D6 (R12-04): the plugin whose box the position belongs to (the pane's own pick there). A control
  // mapped by hand stays in that box, in its place, and only its label changes, as the reference's do.
  PluginTarget home;
  bool byHand = false;
};
bool gSmartOut = false;  // D5 (R11-06): the track's output bus (the reference's second tab) instead of the track
int gSmartView = 0;      // 0 the knobs, 1 the EQ
// D5 (R11-06): learn in place, as the reference's two Learn buttons: 1 Parameter Mapping (the next
// parameter moved on this track's plugins becomes the control in focus), 2 External Assignment (the next
// hardware control moved drives the control in focus). Both stay lit until clicked again (or Escape).
int gLearn = 0;
uint64_t gLearnSerial = 0;  // the document's touch count when Parameter Mapping learning started
ParamId gFocus{};           // the control last touched here (the column shows it)
ObjectId gPendStrip = kNoId;  // D6: a Learn waiting for the mouse to be let go (strip, position, parameter)
int gPendPos = -1;
ParamId gPendParam{};

// Up to `most` of a plugin's controls (built-ins: the continuous ones, in their own order;
// installed plugins: the first ones, which are the main ones in most plugins). A stand-in until
// the merge with the Live branch: there, built-ins take devview::mainParams and installed plugins
// their shown-parameter list (PluginState::shown) when it is set (decision D8).
void mapPlugin(const Song& s, const PluginTarget& t, int most, std::vector<SmartKnob>& out) {
  const PluginInfo* info = t.info(s);
  if (!info) return;
  const bool hosted = isHostedType(info->type);
  int n = 0;
  for (int k = 0; k < static_cast<int>(info->params.size()) && n < most && out.size() < 8; ++k) {
    if (!hosted && info->params[static_cast<size_t>(k)].kind != ParamKind::Continuous) continue;
    out.push_back({t, k, t, false});
    ++n;
  }
}

// D5: a plugin parameter as a screen control (the instrument's or an effect slot's), false for anything else.
bool knobOf(const Song& s, ParamId p, SmartKnob& out) {
  if (s.mixerTrack(p.owner)) {
    if (p.index < kSlotBase) return false;
    const uint32_t r = p.index - kSlotBase, slot = r / kSlotStride, k = r % kSlotStride;
    if (slot >= static_cast<uint32_t>(kEffectSlots) || k >= kSlotEnable) return false;
    out.t = PluginTarget{p.owner, static_cast<int>(slot)};
    out.k = static_cast<int>(k);
  } else if (s.channel(p.owner)) {
    if (p.index < kChannelPluginBase || p.index >= kNoteFxBase) return false;
    out.t = PluginTarget{p.owner, -1};
    out.k = static_cast<int>(p.index - kChannelPluginBase);
  } else {
    return false;
  }
  out.home = out.t;
  out.byHand = false;
  const PluginInfo* info = out.t.info(s);
  return info && out.k < static_cast<int>(info->params.size());
}

// The screen controls of a strip and its sound, in their positions: the pane's own picks (the
// instrument's main controls, then the first effects'), with the ones mapped by hand in their place.
std::vector<SmartKnob> smartKnobs(const Song& s, const MixerTrack& m, const Channel* inst) {
  std::vector<SmartKnob> knobs;
  if (inst && !inst->plugin.type.empty()) mapPlugin(s, PluginTarget{inst->id, -1}, 4, knobs);
  for (int k = 0; k < kEffectSlots && knobs.size() < 8; ++k)
    if (!m.slots[static_cast<size_t>(k)].plugin.type.empty()) mapPlugin(s, PluginTarget{m.id, k}, knobs.empty() ? 4 : 2, knobs);
  for (const SmartControlMap& mp : s.smooth.controls) {
    SmartKnob sk;
    if (mp.strip != m.id || mp.pos < 0 || mp.pos >= 8 || !knobOf(s, ParamId{mp.owner, mp.index}, sk)) continue;
    if (sk.t.owner != m.id && !(inst && sk.t.owner == inst->id)) continue;
    sk.byHand = true;
    if (static_cast<size_t>(mp.pos) < knobs.size()) {  // D6 (R12-04): in its place, in its box
      sk.home = knobs[static_cast<size_t>(mp.pos)].home;
      knobs[static_cast<size_t>(mp.pos)] = sk;
    } else {
      knobs.push_back(sk);  // (a position the pane no longer fills: a box of the mapped plugin's own)
    }
  }
  return knobs;
}
bool mappedByHand(const Song& s, ObjectId strip, int pos) {
  for (const SmartControlMap& mp : s.smooth.controls)
    if (mp.strip == strip && mp.pos == pos) return true;
  return false;
}
// Maps (or with an invalid param, unmaps) screen control `pos` of `strip`. D6 (R12-02, R12-03): song
// data, as the reference keeps it: one undo step that marks the project modified, shared in collab.
void setSmartMap(App& a, ObjectId strip, int pos, ParamId p) {
  a.doc.editSong(p.owner != kNoId ? "map smart control" : "reset smart control", [&](Song& x) {
    auto& v = x.smooth.controls;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const SmartControlMap& c) { return c.strip == strip && c.pos == pos; }), v.end());
    if (p.owner != kNoId) v.push_back(SmartControlMap{strip, pos, p.owner, p.index});
  });
}
// D6 (R12-07): the strip Smart Controls shows for a track is the inspector's (trackStrip: a kit's bus
// for a track that plays several sounds), and its sound only when that sound plays into that strip.
const MixerTrack* smartStrip(const Song& s, int tr, const Channel** inst) {
  const MixerTrack* m = trackStrip(s, tr);
  if (!m) m = mixerOf(s, tr);
  const Channel* c = trackSound(s, tr, nullptr);
  if (c && m && c->mixerTrack != m->id && !(s.mixerIndex(m->id) == 0 && !s.mixerTrack(c->mixerTrack))) c = nullptr;
  if (inst) *inst = c;
  return m;
}
// D6 (R12-06): a plugin window opened from the pane takes the keys, so Escape (and Cmd+W) close it,
// as a window opened from the mixer does.
void openFromSmart(App& a, const PluginTarget& t) {
  const bool was = pluginWindowShown(a, t);
  togglePluginWindow(a, t);
  const PluginInfo* info = t.info(a.doc.song());
  if (!was && !(info && isHostedType(info->type))) a.focusWindow = t.isChannel() ? "###channelsettings" : "###fxeditor";
}
}  // namespace

// K-112D (D3, R09 visual gap 4): Smart Controls' knob as the reference's screen controls (173, 174),
// in the graphite palette: a scale of ticks round a dark body with a soft rim and a light pointer line;
// a thin blue value arc between the ticks and the body. Nothing allocated.
namespace {
void smartKnobFace(ImDrawList* dl, ImVec2 c, float r, float norm, bool bipolar, bool hov, bool act) {
  const auto& P = pal();
  constexpr float kPi = 3.14159265f, a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  for (int k = 0; k <= 10; ++k) {  // the scale
    const float t = a0 + (a1 - a0) * static_cast<float>(k) / 10.0f, cx = std::cos(t), cy = std::sin(t);
    dl->AddLine(ImVec2(c.x + cx * (r - 3.5f), c.y + cy * (r - 3.5f)), ImVec2(c.x + cx * r, c.y + cy * r), theme::withAlpha(P.text, 0.45f), 1.0f);
  }
  const float ra = r - 6.0f, an = a0 + (a1 - a0) * std::clamp(norm, 0.0f, 1.0f), from = bipolar ? (a0 + a1) * 0.5f : a0;
  if (std::fabs(an - from) > 0.01f) {
    dl->PathArcTo(c, ra, std::min(from, an), std::max(from, an), 32);
    dl->PathStroke(act || hov ? theme::mix(P.accent, P.text, 0.25f) : P.accent, ImDrawFlags_None, 2.0f);
  }
  const float rb = std::max(4.0f, r - 9.0f);
  dl->AddCircleFilled(ImVec2(c.x, c.y + 1.5f), rb, theme::withAlpha(P.ink, 0.5f), 40);  // a soft drop shadow
  dl->AddCircleFilled(c, rb, act ? theme::mix(P.surface950, P.text, 0.12f) : hov ? theme::mix(P.surface950, P.text, 0.08f) : theme::mix(P.surface950, P.text, 0.04f), 40);
  dl->AddCircle(c, rb, theme::mix(P.surface950, P.text, 0.22f), 40, 1.0f);
  const float px = std::cos(an), py = std::sin(an);
  dl->AddLine(ImVec2(c.x + px * rb * 0.3f, c.y + py * rb * 0.3f), ImVec2(c.x + px * (rb - 2.0f), c.y + py * (rb - 2.0f)), P.text, std::max(1.5f, r * 0.06f));
}

// D4 (R10-09): Smart Controls' inspector column, as 050 and 262 have it at the pane's left: the sound's
// name (a click opens its window), the control in focus with its value, then two folding sections:
// Parameter Mapping (what the control in focus drives: the plugin, the parameter, its range and value;
// Learn and Open) and External Assignment (its hardware controller links; Learn).
// D5 (R11-06): one rhythm for every row (24 px fields, 26 px headings, 18 px lines), both sections with
// a Learn button that lights while learning in place (no dialog), and the name only here.
void smartColumn(App& a, ImVec2 c0, ImVec2 c1, const Channel* inst, const MixerTrack* m, const SmartKnob* fk, int focusPos) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(c0, c1, P.surface900);
  dl->AddLine(ImVec2(c1.x - 1, c0.y), ImVec2(c1.x - 1, c1.y), theme::withAlpha(P.line, 0.9f));
  const float x = c0.x + 10, w = c1.x - c0.x - 20, fs = theme::kFontCaption, rh = 24;
  float y = c0.y + 10;
  ImGui::PushID("##smcol");
  auto field = [&](const char* id, const std::string& left, const std::string& right, ImU32 face, const char* hint) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    const bool click = ImGui::InvisibleButton(id, ImVec2(w, rh));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + rh), hov && hint ? theme::mix(face, P.text, 0.06f) : face, 5.0f);
    const float rw = right.empty() ? 0.0f : a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, right.c_str()).x + 8;
    const float ty = std::round(y + (rh - fs) * 0.5f) - 1;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 8, ty), w - 16 - rw, P.text, left.c_str());
    if (!right.empty()) dl->AddText(a.fontRegular, fs, ImVec2(std::round(x + w - rw), ty), P.textDim, right.c_str());
    if (hov && hint) ui::Hint(hint);
    y += rh + 6;
    return click;
  };
  // the sound (or the strip, for a track with no instrument); the pane's only place for the name
  const std::string sound = inst ? inst->name : m ? mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(m->id)))) : std::string();
  if (field("##smsound", sound, "", theme::mix(P.surface800, P.text, 0.06f), inst ? "The track's sound: click to open its window" : nullptr) && inst)
    openFromSmart(a, PluginTarget{inst->id, -1});
  std::optional<ParamDesc> d;
  double v = 0;
  if (fk) {
    d = describeParam(s, fk->t.pid(fk->k));
    if (auto gv = getParam(s, fk->t.pid(fk->k))) v = *gv;
  }
  field("##smfocus", d ? d->name : std::string(i18n::tr("No control in focus")), d ? d->format(v) : std::string(), P.field, nullptr);
  dl->AddLine(ImVec2(c0.x, y), ImVec2(c1.x - 1, y), theme::withAlpha(P.line, 0.9f));
  y += 4;
  // A small button at a heading's right; lit while it learns.
  auto smallButton = [&](const char* id, const char* label, bool lit, bool enabled, ImVec2 b0, float bw, const char* hint) {
    const float bh = 20;
    ImGui::SetCursorScreenPos(b0);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(bw, bh)) && enabled;
    const bool hov = ImGui::IsItemHovered();
    const ImU32 face = lit ? (hov ? theme::mix(P.accent, P.text, 0.15f) : P.accent) : hov && enabled ? theme::mix(P.surface700, P.text, 0.10f) : P.surface700;
    dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), face, 5.0f);
    if (!lit) dl->AddLine(ImVec2(b0.x + 4, b0.y + 0.5f), ImVec2(b0.x + bw - 4, b0.y + 0.5f), theme::withAlpha(P.text, 0.10f));  // a raised top edge
    const float lw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, label).x;
    dl->AddText(a.fontRegular, fs, ImVec2(std::round(b0.x + (bw - lw) * 0.5f), std::round(b0.y + (bh - fs) * 0.5f) - 1),
                lit ? pal().paper : enabled ? P.text : P.textFaint, label);
    if (hov) ui::Hint(hint);
    return pressed;
  };
  auto bwOf = [&](const char* label) { return std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, label).x) + 18; };
  // A section's heading row: a chevron and the name; its buttons are placed by the caller.
  constexpr float kHeadH = 26;
  auto section = [&](const char* id, const char* name, bool* open, float buttonsW) {
    ImGui::SetCursorScreenPos(ImVec2(c0.x, y));
    if (ImGui::InvisibleButton(id, ImVec2(std::max(10.0f, c1.x - c0.x - buttonsW - 14), kHeadH))) *open = !*open;
    const bool hov = ImGui::IsItemHovered();
    ui::drawIcon(dl, *open ? ui::Icon::ChevronDown : ui::Icon::Chevron, ImVec2(c0.x + 14, y + kHeadH * 0.5f), 10, hov ? P.text : P.textDim);
    dl->AddText(a.fontRegular, theme::kFontBody - 1, ImVec2(c0.x + 26, std::round(y + (kHeadH - (theme::kFontBody - 1)) * 0.5f) - 1), P.text, name);
  };
  auto line = [&](const char* label, const std::string& value, ImU32 col = 0) {
    if (y + 18 > c1.y - 4) return;
    dl->AddText(a.fontRegular, fs, ImVec2(c0.x + 26, y), P.textDim, label);
    const float vx = c0.x + 26 + std::min(90.0f, (c1.x - c0.x) * 0.36f);
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(vx, y), c1.x - 10 - vx, col ? col : P.text, value.c_str());
    y += 18;
  };
  static bool mapOpen = true, extOpen = true;
  const bool alt = ImGui::GetIO().KeyAlt;
  {  // Parameter Mapping: Learn (the next parameter moved on this track becomes this control), Open
    const char* learn = i18n::tr("Learn");
    const char* open = i18n::tr("Open");
    const float lw = bwOf(learn), ow = bwOf(open), by = y + 3;
    section("##smmap", i18n::tr("Parameter Mapping"), &mapOpen, lw + ow + 4);
    if (smallButton("##maplearn", learn, gLearn == 1, fk != nullptr, ImVec2(c1.x - 10 - ow - 4 - lw, by), lw,
                    "Learn: move a parameter in one of this track's plugins (its window or here) to put it on the control in focus. "
                    "Click again to stop. Option-click: back to the automatic choice")) {
      if (alt && m && focusPos >= 0) {
        setSmartMap(a, m->id, focusPos, ParamId{});
        gLearn = 0;
      } else {
        gLearn = gLearn == 1 ? 0 : 1;
        gLearnSerial = a.doc.touchSerial();
      }
    }
    if (smallButton("##mapopen", open, false, fk != nullptr, ImVec2(c1.x - 10 - ow, by), ow, "Open the plugin the control in focus belongs to"))
      openFromSmart(a, fk->t);
    y += kHeadH;
  }
  if (mapOpen && fk && d) {
    const PluginInfo* info = fk->t.info(s);
    line(i18n::tr("Plugin"), info ? info->name : std::string("?"));
    line(i18n::tr("Parameter"), d->name);
    line(i18n::tr("Range"), d->format(d->min) + "  to  " + d->format(d->max));
    line(i18n::tr("Value"), d->format(v));
    if (gLearn == 1) line(i18n::tr("Learning"), i18n::tr("move a parameter of this track"), P.accent);
    else if (m && focusPos >= 0 && mappedByHand(s, m->id, focusPos)) line(i18n::tr("Mapping"), i18n::tr("by hand"));
    y += 6;
  }
  {  // External Assignment: Learn (the next hardware control moved drives this control)
    const char* learn = i18n::tr("Learn");
    const float lw = bwOf(learn);
    section("##smext", i18n::tr("External Assignment"), &extOpen, lw);
    if (smallButton("##extlearn", learn, gLearn == 2, fk != nullptr, ImVec2(c1.x - 10 - lw, y + 3), lw,
                    "Learn: move a control on your MIDI device to assign it to the control in focus. Click again to stop"))
      gLearn = gLearn == 2 ? 0 : 2;
    y += kHeadH;
  }
  if (extOpen && fk) {
    const ParamId pid = fk->t.pid(fk->k);
    int shown = 0;
    for (const ControllerLink& l : s.links) {
      if (l.action != RemoteAction::None || l.owner != pid.owner || l.index != pid.index) continue;
      char b[160];
      const char* kind = l.source == RemoteSource::CC ? "CC" : l.source == RemoteSource::Note ? "Note" : l.source == RemoteSource::Osc ? "OSC" : "Control";
      if (l.source == RemoteSource::Osc) std::snprintf(b, sizeof b, "%s %.120s", kind, l.oscAddress.c_str());
      else if (l.midiChannel >= 0) std::snprintf(b, sizeof b, "%s %d, channel %d", kind, l.cc, l.midiChannel + 1);
      else std::snprintf(b, sizeof b, "%s %d, any channel", kind, l.cc);
      line(i18n::tr("Controller"), b);
      ++shown;
    }
    if (shown == 0) line(i18n::tr("Controller"), i18n::tr("None"));
    if (gLearn == 2) line(i18n::tr("Learning"), i18n::tr("move a control on your device"), P.accent);
  }
  ImGui::PopID();
}
}  // namespace

void smartControls(App& a) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  const int tr = focusTrack(a);
  // D5 (R11-06): the second tab is the track's output (its bus, else the master), named so, as 050's
  // second tab names the track's aux.
  const Channel* trackInst = nullptr;
  const MixerTrack* trackM = smartStrip(s, tr, &trackInst);
  const MixerTrack* outM = nullptr;
  if (trackM && s.mixerIndex(trackM->id) > 0)
    for (const Send& sd : trackM->sends)
      if (!sd.sidechain && !outM) outM = s.mixerTrack(sd.to);
  if (!outM && !s.mixer.empty() && !(trackM && s.mixerIndex(trackM->id) == 0)) outM = &s.mixer[0];
  if (!outM) gSmartOut = false;
  const MixerTrack* m = gSmartOut ? outM : trackM;
  const Channel* inst = gSmartOut ? nullptr : trackInst;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float paneW = ImGui::GetContentRegionAvail().x;
  const bool column = paneW >= 620;  // the inspector column (which names the sound) fits
  {  // the bar: the target, the view
    // Checkpoint 28: as 174, 173 and 050 order it: Track | output and Compare at the left, the view
    // (Controls | EQ) centred; the track's name at the right end only when the column (which has it) is out.
    const std::string name = m ? mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(m->id)))) : std::string(i18n::tr("No track selected"));
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY(), W = ImGui::GetContentRegionAvail().x;
    std::string outName = outM ? (s.mixerIndex(outM->id) == 0 ? std::string(i18n::tr("Master")) : mixerTrackName(s, static_cast<size_t>(s.mixerIndex(outM->id))))
                               : std::string(i18n::tr("Output"));
    const char* targets[] = {i18n::tr("Track"), outName.c_str()};
    const std::string outHint = outM ? std::string(i18n::tr("The controls of the track's output: ")) + outName : std::string(i18n::tr("This track has no output bus"));
    const char* thints[] = {i18n::tr("The selected track's controls"), outHint.c_str()};
    const float segW = std::clamp(std::ceil(ImGui::CalcTextSize(outName.c_str()).x) + 24.0f, 64.0f, 120.0f);
    int target = gSmartOut ? 1 : 0;
    if (ui::Segmented("##smtarget", targets, 2, &target, segW, 0, thints)) gSmartOut = target == 1 && outM;
    float leftEnd = x0 + segW * 2;
    if (m) {  // Compare after the target, for every plugin on the track
      std::vector<PluginTarget> ts;
      if (inst && !inst->plugin.type.empty()) ts.push_back(PluginTarget{inst->id, -1});
      for (int k = 0; k < kEffectSlots; ++k)
        if (!m->slots[static_cast<size_t>(k)].plugin.type.empty()) ts.push_back(PluginTarget{m->id, k});
      const float cw = 84.0f;
      ImGui::SameLine(0, theme::kS2);
      compareTrackButton(a, ts, cw);
      leftEnd += theme::kS2 + cw;
    }
    const char* views[] = {i18n::tr("Controls"), i18n::tr("EQ")};
    const char* hints[] = {i18n::tr("The track's main controls as knobs"), i18n::tr("The track's EQ, editable here")};
    const float vw = 72 * 2;
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPos(ImVec2(std::max(leftEnd + theme::kS3, std::round(x0 + (W - vw) * 0.5f)), y0));
    ui::Segmented("##smview", views, 2, &gSmartView, 72, 0, hints);
    const float nameX = ImGui::GetItemRectMax().x + theme::kS3, right = ImGui::GetWindowPos().x + x0 + W;
    const float fs = ImGui::GetFontSize(), room = right - nameX;
    if ((!column || gSmartView == 1) && room > 40) {
      const float tw = std::min(room, ImGui::CalcTextSize(name.c_str()).x);
      const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(std::round(right - tw), theme::textTop(a.fontRegular, fs, r0.y, r1.y - r0.y)), room, P.textDim,
                          name.c_str());
    }
  }
  ImGui::Spacing();
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const ImVec2 p1(p0.x + av.x, p0.y + std::max(av.y, 60.0f));
  if (!m) {
    ui::EmptyState(dl, p0, p1, i18n::tr("No track selected"), i18n::tr("Select a track: its controls show here"));
    return;
  }
  if (gSmartView == 1) {  // the EQ view: the track's first EQ, its own display
    int eq = -1;
    for (int k = 0; k < kEffectSlots && eq < 0; ++k)
      if (m->slots[static_cast<size_t>(k)].plugin.type == "gl.eq") eq = k;
    if (eq < 0) {
      ui::EmptyState(dl, p0, ImVec2(p1.x, p0.y + std::min(av.y, 90.0f)), i18n::tr("No EQ on this track"), i18n::tr("Add one: it goes in the first free effect slot"));
      ImGui::SetCursorScreenPos(ImVec2(std::round(p0.x + av.x * 0.5f - 50), p0.y + std::min(av.y, 90.0f)));
      if (ui::TextButton(i18n::tr("Add an EQ"), false, 100, "Put an EQ in this track's first free effect slot")) {
        int free = -1;
        for (int k = 0; k < kEffectSlots && free < 0; ++k)
          if (m->slots[static_cast<size_t>(k)].plugin.type.empty()) free = k;
        if (free < 0) a.notify("No free effect slot for an EQ", true);
        else a.doc.setSlotPlugin(m->id, free, "gl.eq");
      }
      return;
    }
    if (a.fxEditor == std::make_pair(m->id, eq)) {  // one body, one place: the window has it now
      ui::EmptyState(dl, p0, ImVec2(p1.x, p0.y + std::min(av.y, 90.0f)), i18n::tr("The EQ is open in its window"), i18n::tr("Close the window to edit it here"));
      ImGui::SetCursorScreenPos(ImVec2(std::round(p0.x + av.x * 0.5f - 60), p0.y + std::min(av.y, 90.0f)));
      if (ui::TextButton(i18n::tr("Edit it here"), false, 120, "Closes the EQ's window and shows it in this pane")) a.fxEditor = {kNoId, -1};
      return;
    }
    ImGui::BeginChild("##smeq", ImVec2(0, 0));
    drawEffectEditorBody(a, m->id, eq, BodyFrame{ImGui::GetContentRegionAvail().x, false, "Smart controls"});
    ImGui::EndChild();
    return;
  }
  // The knobs in their positions (the instrument's main controls, then the first effects', and the ones
  // mapped by hand), shown grouped in boxes, one per plugin in the track's order; a box's name opens
  // that plugin's window.
  std::vector<SmartKnob> knobs = smartKnobs(s, *m, inst);
  int fi = -1;
  auto findFocus = [&] {
    fi = -1;
    for (size_t k = 0; k < knobs.size() && fi < 0; ++k)
      if (knobs[k].t.pid(knobs[k].k) == gFocus) fi = static_cast<int>(k);
    if (fi < 0 && !knobs.empty()) fi = 0, gFocus = knobs[0].t.pid(knobs[0].k);
  };
  findFocus();
  // D5 (R11-06): learning in place. Parameter Mapping: the next parameter moved on this track's plugins
  // (in their windows, or here) takes the focused control's position. External Assignment: the app links
  // the next hardware control moved that drives nothing yet (App::processMidi).
  if (gLearn != 0 && (fi < 0 || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) gLearn = 0;
  if (gLearn == 1 && a.doc.touchSerial() != gLearnSerial) {
    gLearnSerial = a.doc.touchSerial();
    const ParamId p = a.doc.lastTouchedParam();
    SmartKnob sk;
    bool onShown = false;  // a control already on the band is focused by the touch, not mapped
    for (const SmartKnob& k : knobs) onShown = onShown || k.t.pid(k.k) == p;
    if (!onShown && knobOf(s, p, sk) && (sk.t.owner == m->id || (inst && sk.t.owner == inst->id))) {
      // D6 (R12-02): recorded once the move is over (the button let go), so the mapping is its own
      // undo step after the move's: Cmd+Z takes back the mapping first, then the move.
      gPendStrip = m->id, gPendPos = fi, gPendParam = p;
    } else if (onShown) {
      gFocus = p;
    }
  }
  if (gPendStrip != kNoId && !ImGui::IsAnyMouseDown()) {
    const ParamId p = gPendParam;
    if (gPendStrip == m->id && describeParam(a.doc.song(), p)) {
      setSmartMap(a, gPendStrip, gPendPos, p);
      gFocus = p;
      a.notify(std::string(i18n::tr("Smart control mapped: ")) + paramDisplayName(a.doc.song(), p));
      knobs = smartKnobs(a.doc.song(), *m, inst);
      findFocus();
    }
    gPendStrip = kNoId;
  }
  if (gLearn == 2 && fi >= 0) {
    a.smartLearn = gFocus;
    a.smartLearnFrame = ImGui::GetFrameCount();
  } else {
    a.smartLearn = ParamId{};
  }
  // D4 (R10-09): the pane as 050 and 262 lay it out: an inspector column at the left (the sound's name,
  // the control in focus, Parameter Mapping and External Assignment), and the controls on a band across
  // the rest. D5 (R11-06): the band fills the area (no dark strip under it), and every box has the same
  // height (one row each, or two rows each), so the boxes read as one even row.
  const float W = p1.x - p0.x;
  const float colW = column ? std::clamp(std::round(W * 0.24f), 230.0f, 330.0f) : 0.0f;
  if (colW > 0) smartColumn(a, p0, ImVec2(p0.x + colW, p1.y), inst, m, fi >= 0 ? &knobs[static_cast<size_t>(fi)] : nullptr, fi);
  const ImVec2 q0(p0.x + colW, p0.y);
  {  // the band: the whole area right of the column, a soft top-lit graphite face (050, 262)
    const ImU32 top = theme::mix(P.surface800, P.text, 0.07f), bot = theme::mix(P.surface900, P.ink, 0.10f);
    dl->AddRectFilledMultiColor(q0, p1, top, top, bot, bot);
    dl->AddLine(q0, ImVec2(p1.x, q0.y), theme::withAlpha(P.text, 0.10f));
  }
  if (knobs.empty()) {
    ui::EmptyState(dl, q0, p1, i18n::tr("No controls yet"), i18n::tr("Put an instrument or an effect on this track: its main controls show here"));
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
    ImGui::Dummy(ImVec2(av.x, 0));
    return;
  }
  // The boxes: the knobs grouped by plugin, the instrument first, then the effects in slot order.
  struct Box {
    PluginTarget t;
    std::vector<int> idx;  // positions in `knobs`
    int cols = 0, rows = 0;
  };
  std::vector<Box> boxes;
  for (size_t k = 0; k < knobs.size(); ++k) {
    Box* b = nullptr;
    for (Box& x : boxes)
      if (x.t.key() == knobs[k].home.key()) b = &x;  // D6 (R12-04): a mapped control stays in its box
    if (!b) {
      boxes.push_back(Box{knobs[k].home, {}, 0, 0});
      b = &boxes.back();
    }
    b->idx.push_back(static_cast<int>(k));
  }
  std::stable_sort(boxes.begin(), boxes.end(), [](const Box& x, const Box& y) { return x.t.slot < y.t.slot; });
  const float areaW = p1.x - q0.x, areaH = p1.y - q0.y;
  const float gap = 14, inPad = 14, headH = 26, bandPad = 14;
  size_t most = 0;
  for (const Box& b : boxes) most = std::max(most, b.idx.size());
  const int rows = areaH >= headH + 2 * bandPad + 2 * 92.0f + 20 && most > 2 ? 2 : 1;  // every box the same
  // D6 (R12-08): the boxes fill the band, as 050's do: full height (less a margin), and across its
  // width up to a comfortable cell; the knobs grow with the room.
  int totalCols = 0;
  for (Box& b : boxes) {
    const int n = static_cast<int>(b.idx.size());
    b.rows = std::min(rows, n);
    b.cols = (n + rows - 1) / rows;
    totalCols += b.cols;
  }
  const float nb = static_cast<float>(boxes.size());
  const float roomW = areaW - 2 * bandPad - gap * (nb - 1) - 2 * inPad * nb;
  const float cellW = std::clamp(std::floor(roomW / static_cast<float>(std::max(1, totalCols))), 70.0f, 240.0f);
  const float boxH = std::max(60.0f, areaH - 2 * bandPad);
  const float cellH = std::floor((boxH - headH - 14) / static_cast<float>(rows));
  const float knobR = std::clamp(std::floor(std::min(cellW * 0.30f, (cellH - 30) * 0.5f)), 14.0f, 46.0f);
  const float cellTop = std::max(0.0f, std::round((cellH - (knobR * 2 + 22)) * 0.5f));  // name and knob centred in the cell
  float used = 0;
  for (const Box& b : boxes) used += cellW * static_cast<float>(b.cols) + 2 * inPad;
  used += gap * (nb - 1);
  float x = std::round(q0.x + std::max(bandPad, (areaW - used) * 0.5f));  // the boxes centred on the band
  const float y0 = std::round(q0.y + std::max(bandPad, (areaH - boxH) * 0.5f));
  for (const Box& b : boxes) {
    const float bw = cellW * static_cast<float>(b.cols) + 2 * inPad;
    if (x + bw > p1.x - 2 && &b != &boxes.front()) break;  // no room for another box
    const ImVec2 b0(x, y0), b1(x + bw, y0 + boxH);
    // a plugin's box: a hairline frame under its name, as the reference groups its controls
    dl->AddRect(ImVec2(b0.x, b0.y + headH), b1, theme::withAlpha(P.text, 0.32f), 3.0f);
    const PluginInfo* info = b.t.info(s);
    ImGui::PushID(static_cast<int>(b.t.key() & 0x7fffffff));
    {  // the title: the plugin's name in capitals over the box; a click opens its window
      ImGui::SetCursorScreenPos(b0);
      if (ImGui::InvisibleButton("##title", ImVec2(bw, headH))) openFromSmart(a, b.t);
      const bool shown = pluginWindowShown(a, b.t), hov = ImGui::IsItemHovered();
      if (hov) ui::Hint("Open this plugin's window");
      std::string title = info ? info->name : std::string("?");
      for (char& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      const float tf = theme::kFontTitle;
      const float tw = std::min(a.fontRegular->CalcTextSizeA(tf, 1e9f, 0, title.c_str()).x, bw - 12);
      theme::textEllipsis(dl, a.fontRegular, tf, ImVec2(std::round(b0.x + (bw - tw) * 0.5f), theme::textTop(a.fontRegular, tf, b0.y - 2, headH)), bw - 12,
                          shown ? P.accent : hov ? P.text : theme::mix(P.textDim, P.text, 0.4f), title.c_str());
    }
    // A box with fewer knobs than its rows (one knob on a two-row band) centres them in its height.
    const float rowOff = std::round(cellH * static_cast<float>(rows - b.rows) * 0.5f);
    for (size_t j = 0; j < b.idx.size(); ++j) {
      const size_t k = static_cast<size_t>(b.idx[j]);
      const int col = static_cast<int>(j) % b.cols, row = static_cast<int>(j) / b.cols;
      const ParamId pid = knobs[k].t.pid(knobs[k].k);
      const auto d = describeParam(s, pid);
      const float cx = std::round(b0.x + inPad + cellW * (static_cast<float>(col) + 0.5f));
      const float cy0 = std::round(b0.y + headH + 4 + cellTop + rowOff + cellH * static_cast<float>(row));
      const bool focused = pid == gFocus;
      if (d) {  // the control's name above its knob, in capitals (173, 050)
        std::string label = d->name;
        for (char& ch : label) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const float lf = theme::kFontCaption;
        const float lw = std::min(a.fontRegular->CalcTextSizeA(lf, 1e9f, 0, label.c_str()).x, cellW - 8);
        theme::textEllipsis(dl, a.fontRegular, lf, ImVec2(std::round(cx - lw * 0.5f), cy0), cellW - 8, focused ? P.text : P.textDim, label.c_str());
      }
      const ImVec2 kc(cx, cy0 + 18 + knobR);
      if (focused) {  // the control in focus: a soft ring of the accent round it (050); learning, a full one
        const float pulse = gLearn != 0 ? 0.55f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f) : 0.45f;
        dl->AddCircle(kc, knobR - 7.0f, theme::withAlpha(P.accent, pulse), 48, 3.0f);
      }
      ImGui::SetCursorScreenPos(ImVec2(cx - knobR, cy0 + 18));
      ImGui::PushID(static_cast<int>(k));
      ui::SetNextKnobFace(smartKnobFace);
      ui::ParamKnob("##sk", pid, knobR, nullptr);
      ImGui::PopID();
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
          ImGui::IsMouseHoveringRect(ImVec2(cx - cellW * 0.5f, cy0), ImVec2(cx + cellW * 0.5f, cy0 + knobR * 2 + 22)))
        gFocus = pid;  // (its own touch, a drag, maps nothing: it is on the band)
    }
    ImGui::PopID();
    x = b1.x + gap;
  }
  ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
  ImGui::Dummy(ImVec2(av.x, 0));
}

// D5: parts smart focus <position> | learn off|param|ext | cc <number> <value> | touch <slot> <param> | state
bool smartDevCommand(App& a, const std::vector<std::string>& args) {
  if (args.empty()) return false;
  const Song& s = a.doc.song();
  const int tr = focusTrack(a);
  const Channel* inst = nullptr;
  const MixerTrack* m = smartStrip(s, tr, &inst);
  if (gSmartOut && m) {  // the output tab, as the pane shows it
    const MixerTrack* o = nullptr;
    for (const Send& sd : m->sends) {
      if (!sd.sidechain && !o && s.mixerIndex(m->id) > 0) o = s.mixerTrack(sd.to);
    }
    if (!o && !s.mixer.empty() && s.mixerIndex(m->id) > 0) o = &s.mixer[0];
    if (o) {
      m = o;
      inst = nullptr;
    }
  }
  const std::string& op = args[0];
  if (op == "focus" && args.size() > 1 && m) {
    const std::vector<SmartKnob> knobs = smartKnobs(s, *m, inst);
    const size_t i = static_cast<size_t>(std::max(0, std::atoi(args[1].c_str())));
    if (i < knobs.size()) gFocus = knobs[i].t.pid(knobs[i].k);
  } else if (op == "learn" && args.size() > 1) {
    gLearn = args[1] == "param" ? 1 : args[1] == "ext" ? 2 : 0;
    gLearnSerial = a.doc.touchSerial();
  } else if (op == "cc" && args.size() > 2 && a.midi) {  // a controller message, as from a device
    const std::vector<unsigned char> msg = {0xB0, static_cast<unsigned char>(std::clamp(std::atoi(args[1].c_str()), 0, 127)),
                                            static_cast<unsigned char>(std::clamp(std::atoi(args[2].c_str()), 0, 127))};
    a.midi->onMessage("dev", 0, msg);
  } else if (op == "touch" && args.size() > 2 && m) {  // a parameter of an effect slot moved (its window)
    const ParamId p = slotParam(m->id, std::atoi(args[1].c_str()), std::atoi(args[2].c_str()));
    if (const auto v = getParam(s, p)) {
      const auto d = describeParam(s, p);
      a.doc.setParam(p, d ? std::clamp(*v + (d->max - d->min) * 0.1, d->min, d->max) : *v, false);
    }
  } else if (op == "state") {
    std::string ks, links;
    if (m) {
      for (const SmartKnob& k : smartKnobs(s, *m, inst)) {
        const ParamId p = k.t.pid(k.k);
        ks += (ks.empty() ? "" : ",") + std::to_string(p.owner) + ":" + std::to_string(p.index);
      }
    }
    for (const ControllerLink& l : s.links)
      if (l.action == RemoteAction::None) links += (links.empty() ? "" : ",") + std::to_string(l.cc) + ">" + std::to_string(l.owner) + ":" + std::to_string(l.index);
    std::printf("smart state: learn %d focus %u:%u out %d knobs %s mapped %zu links %s\n", gLearn, gFocus.owner, gFocus.index, gSmartOut ? 1 : 0, ks.c_str(),
                s.smooth.controls.size(), links.c_str());
  } else if (op == "out" && args.size() > 1) {
    gSmartOut = args[1] == "1";
  } else {
    return false;
  }
  std::fflush(stdout);
  return true;
}

}  // namespace gl::logicplug
