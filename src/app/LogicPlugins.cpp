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
      if (const Channel* c = trackSound(s, t, nullptr); c && c->id == a.selChannel) return t;
  return n > 0 ? 0 : -1;
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
  if (!own.empty()) return own.size() == 1 ? own.front() : -1;
  return routed.size() == 1 ? routed.front() : -1;
}
}  // namespace

int ownerTrack(const Song& s, ObjectId owner) { return ownerTrackK(s, owner, trackKinds(s)); }

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
};
bool gSmartMaster = false;  // the master track's chain instead of the selected track's
int gSmartView = 0;         // 0 the knobs, 1 the EQ

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
    out.push_back({t, k});
    ++n;
  }
}
}  // namespace

void smartControls(App& a) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  const int tr = focusTrack(a);
  const MixerTrack* m = gSmartMaster ? (s.mixer.empty() ? nullptr : &s.mixer[0]) : mixerOf(s, tr);
  const Channel* inst = gSmartMaster ? nullptr : trackSound(s, tr, nullptr);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  {  // the bar: the target, the view, the master switch
    // Checkpoint 28: as 174, 173 and 050 order it: Track | Master and Compare at the left, the view
    // (Controls | EQ) centred, the track's name at the right end (the references title the window with it).
    const std::string name = m ? mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(m->id)))) : std::string(i18n::tr("No track selected"));
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY(), W = ImGui::GetContentRegionAvail().x;
    const char* targets[] = {i18n::tr("Track"), i18n::tr("Master")};
    const char* thints[] = {i18n::tr("The selected track's controls"), i18n::tr("The master track's controls")};
    int target = gSmartMaster ? 1 : 0;
    if (ui::Segmented("##smtarget", targets, 2, &target, 64, 0, thints)) gSmartMaster = target == 1;
    float leftEnd = x0 + 128;
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
    if (room > 40) {
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
  // The knobs: the instrument's main controls, then the first effects'. Grouped in boxes, one per
  // plugin; a box's name opens that plugin's window.
  std::vector<SmartKnob> knobs;
  if (inst && !inst->plugin.type.empty()) mapPlugin(s, PluginTarget{inst->id, -1}, 4, knobs);
  for (int k = 0; k < kEffectSlots && knobs.size() < 8; ++k)
    if (!m->slots[static_cast<size_t>(k)].plugin.type.empty()) mapPlugin(s, PluginTarget{m->id, k}, knobs.empty() ? 4 : 2, knobs);
  if (knobs.empty()) {
    ui::EmptyState(dl, p0, p1, i18n::tr("No controls yet"), i18n::tr("Put an instrument or an effect on this track: its main controls show here"));
    return;
  }
  // K-112b: the panel fills the pane (every reference shows it across the whole pane): a tinted
  // panel, one titled box per plugin, the knobs sized to the room, in two rows when it is tall.
  const float gap = theme::kS2, pad = 10, headH = 22;
  dl->AddRectFilled(p0, p1, theme::mix(P.surface800, theme::rgb(0x2E5E92), 0.30f), 6.0f);
  struct Box {
    size_t i, j;
    int cols, rows;
  };
  std::vector<Box> boxes;
  for (size_t i = 0; i < knobs.size();) {
    size_t j = i;
    while (j < knobs.size() && knobs[j].t.key() == knobs[i].t.key()) ++j;
    boxes.push_back({i, j, 0, 0});
    i = j;
  }
  const float innerH = (p1.y - p0.y) - pad * 2;
  const bool twoRows = innerH >= headH + 2 * 86.0f;
  int totalCols = 0;
  for (Box& b : boxes) {
    const int n = static_cast<int>(b.j - b.i);
    b.rows = twoRows && n > 2 ? 2 : 1;
    b.cols = (n + b.rows - 1) / b.rows;
    totalCols += b.cols;
  }
  const float roomW = (p1.x - p0.x) - pad * 2 - gap * static_cast<float>(boxes.size() - 1) - 16.0f * static_cast<float>(boxes.size());
  // Checkpoint 29: the knobs grow with the pane (174 and 173 fill it): up to 40 px radius, wider cells.
  const float cellW = std::clamp(roomW / static_cast<float>(std::max(1, totalCols)), 64.0f, 190.0f);
  const float rowRoom = (innerH - headH - 22) / (twoRows ? 2.0f : 1.0f);
  const float knobR = std::clamp(std::min(cellW * 0.28f, (rowRoom - 34) * 0.5f), 12.0f, 40.0f);
  const float cellH = std::min(rowRoom, std::round(knobR * 2 + 30));  // name (18) above, the knob, 12 px to the next row
  float used = 0;
  for (const Box& b : boxes) used += cellW * static_cast<float>(b.cols) + 16;
  used += gap * static_cast<float>(boxes.size() - 1);
  float x = std::round(p0.x + std::max(pad, ((p1.x - p0.x) - used) * 0.5f));  // the boxes centred in the panel
  // Checkpoint 28: each plugin's name stands over its box at title size, as 174, 173 and 193 set it
  // (it was a caption inside the box's top edge); the box holds the knobs with a little more room.
  const float boxH = headH + cellH * static_cast<float>(twoRows ? 2 : 1) + 12;
  const float y0 = std::round(p0.y + std::max(pad, ((p1.y - p0.y) - boxH) * 0.5f));
  for (const Box& b : boxes) {
    const float bw = cellW * static_cast<float>(b.cols) + 16;
    if (x + bw > p1.x - 2 && &b != &boxes.front()) break;  // no room for another box
    const ImVec2 b0(x, y0), b1(x + bw, y0 + headH + cellH * static_cast<float>(b.rows) + 12);  // checkpoint 29: 14 px under the last knobs, as above the first names
    dl->AddRect(ImVec2(b0.x, b0.y + headH), b1, theme::withAlpha(P.text, 0.35f), 4.0f);
    const PluginInfo* info = knobs[b.i].t.info(s);
    ImGui::PushID(static_cast<int>(b.i));
    {  // the title: the plugin's name, a click opens its window
      ImGui::SetCursorScreenPos(b0);
      if (ImGui::InvisibleButton("##title", ImVec2(bw, headH))) togglePluginWindow(a, knobs[b.i].t);
      const bool shown = pluginWindowShown(a, knobs[b.i].t), hov = ImGui::IsItemHovered();
      if (hov) ui::Hint("Open this plugin's window");
      std::string title = info ? info->name : std::string("?");
      for (char& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      const float tf = theme::kFontTitle;
      const float tw = std::min(a.fontRegular->CalcTextSizeA(tf, 1e9f, 0, title.c_str()).x, bw - 12);
      theme::textEllipsis(dl, a.fontRegular, tf, ImVec2(std::round(b0.x + (bw - tw) * 0.5f), theme::textTop(a.fontRegular, tf, b0.y - 2, headH)), bw - 12,
                          shown ? P.hot : hov ? P.text : theme::mix(P.text, P.textDim, 0.3f), title.c_str());
    }
    for (size_t k = b.i; k < b.j; ++k) {
      const int idx = static_cast<int>(k - b.i), col = idx % b.cols, row = idx / b.cols;
      const ParamId pid = knobs[k].t.pid(knobs[k].k);
      const auto d = describeParam(s, pid);
      const float cx = b0.x + 8 + cellW * (static_cast<float>(col) + 0.5f);
      const float cy0 = std::round(b0.y + headH + 10 + cellH * static_cast<float>(row));
      // The control's name above its knob (as the references show), small capitals, centred.
      if (d) {
        std::string label = d->name;
        for (char& ch : label) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const float lf = theme::kFontCaption;
        const float lw = std::min(a.fontRegular->CalcTextSizeA(lf, 1e9f, 0, label.c_str()).x, cellW - 8);
        theme::textEllipsis(dl, a.fontRegular, lf, ImVec2(std::round(cx - lw * 0.5f), cy0), cellW - 8, P.textDim, label.c_str());
      }
      ImGui::SetCursorScreenPos(ImVec2(std::round(cx - knobR), cy0 + 18));
      ImGui::PushID(static_cast<int>(k));
      ui::ParamKnob("##sk", pid, knobR, nullptr);
      ImGui::PopID();
    }
    ImGui::PopID();
    x = b1.x + gap;
  }
  ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
  ImGui::Dummy(ImVec2(av.x, 0));
}

}  // namespace gl::logicplug
