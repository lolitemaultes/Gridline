#include "SmoothLibrary.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Crash.h"
#include "../core/I18n.h"
#include "../core/LayoutShell.h"
#include "../core/MenuOps.h"
#include "../core/PluginInfo.h"
#include "../core/TrackModes.h"
#include "../engine/Render.h"
#include "../engine/Sample.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginGroups.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "CollabUi.h"
#include "Keys.h"
#include "LayoutParts.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LogicNewTracks.h"
#include "LogicPlugins.h"
#include "PluginUI.h"
#include "Theme.h"
#include "TrackIcons.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::smoothlib {

using theme::pal;

namespace {

constexpr const char* kRegion = "logic.library";

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trimmed(const std::string& s) {
  const size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
  return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

// The kind of sound a patch is, by its instrument and its name (as the browser's Sounds list sorts them).
const char* soundKind(const PluginInfo& p, const std::string& preset) {
  const std::string n = lower(preset);
  auto has = [&n](const char* w) { return n.find(w) != std::string::npos; };
  if (p.type == "gl.pads") return "Drum kit";
  if (p.category == "Drums") return "Drums";
  if (p.type == "gl.acid" || has("bass") || has("808") || has("sub ")) return "Bass";
  if (p.type == "gl.keys" || has("piano") || has("organ") || has("keys") || has("rhodes")) return "Keys";
  if (has("bell") || has("mallet") || has("marimba") || has("vibe")) return "Bells and mallets";
  if (p.type == "gl.pluck" || has("pluck") || has("nylon") || has("harp") || has("guitar")) return "Plucked";
  if (has("pad") || has("strings") || has("choir")) return "Pads and strings";
  if (has("lead")) return "Leads";
  if (p.type == "gl.granular") return "Textures";
  return "Synth";
}

bool installedListed(const App& a, const PluginInfo& p) {
  std::string fmt, uid;
  if (host::splitType(p.type, fmt, uid) && !host::formatEnabled(fmt)) return false;
  return host::listedInBrowser(p.type) && collabui::pluginAllowed(a, p.type);
}

int gNav = 1;  // Instruments, as a new project's Library opens
bool gDrill = true;  // D4 (R10-05): a narrow Library shows one column at a time: false the list of kinds, true its patches
int gNavBeforeBus = -1;  // R10-07: the view before a bus was selected (the Library showed its effects meanwhile)
char gFilter[64] = "";
uint64_t gEpoch = ~0ull;
bool gShown = false;
bool gWasActive = false;
// D5 (R11-04): the slot each strip's Library browsing holds, as the reference's Library loads a setting
// that replaces the one before it: the next effect clicked goes into that slot again (one undo step
// back), so browsing never piles up inserts. Kept while the slot still holds something the Library put
// there (an undo, or the user replacing it, ends it); this run only, forgotten with the project.
// D6 (R12-10): every slot the Library used on the strip, the latest last: an undo that empties the
// latest one (an Option-click add taken back) hands browsing back to the slot used before it.
struct LibUse {
  int slot = -1;
  std::vector<std::string> types;  // what the Library put in it (an undo goes back to an earlier one)
};
struct LibSlot {
  ObjectId strip = kNoId;
  std::vector<LibUse> uses;
};
std::vector<LibSlot> gLibSlots;

// A row's identity across rebuilds of the list (the selection and the loaded mark).
std::string itemKey(const Item& it) { return (it.effect ? "fx|" : "in|") + it.type + "|" + std::to_string(it.preset) + "|" + it.name; }
// The selected row (a click or Up / Down; empty: none). The list scrolls to it once after a key moves it.
std::string gSel;
bool gScrollToSel = false;
// The row each sound was last loaded from, with the settings it got: the row shows as the track's sound
// while the sound still has them, even when the sound's name is the user's own (it then does not follow
// the patch's name).
struct Loaded {
  std::string key, type;
  std::vector<float> params;
};
std::map<ObjectId, Loaded> gLoaded;

LibSlot* libSlotOf(ObjectId strip) {
  for (LibSlot& l : gLibSlots)
    if (l.strip == strip) return &l;
  return nullptr;
}
// The strip's latest Library slot that still holds one of the Library's effects, else -1.
int browsedSlot(const MixerTrack& st) {
  const LibSlot* l = libSlotOf(st.id);
  if (!l) return -1;
  for (auto u = l->uses.rbegin(); u != l->uses.rend(); ++u) {
    if (u->slot < 0 || u->slot >= kEffectSlots) continue;
    const std::string& t = st.slots[static_cast<size_t>(u->slot)].plugin.type;
    if (!t.empty() && std::find(u->types.begin(), u->types.end(), t) != u->types.end()) return u->slot;
  }
  return -1;
}

bool libraryShown(const App& a) {
  return layout::visible(layout::shell(layout::Preset::Logic), layoutui::regionState(a, layout::Preset::Logic), kRegion);
}

// The track the Library loads onto, and its sound.
int targetTrack(const App& a) { return logicplug::focusTrack(a); }
// R10-07: the selected track is a bus (its row shows a bus strip's automation): its strip, else kNoId.
ObjectId busOf(const App& a, int track) {
  const logicplug::AutoFold& af = logicplug::autoFold(a);
  return af.isBus(track) ? af.busStrip[static_cast<size_t>(track)] : kNoId;
}
const Channel* targetSound(const App& a, int track) {
  const Song& s = a.doc.song();
  const Channel* c = logicplug::trackSound(s, track, nullptr);
  if (!c) return nullptr;
  const PluginInfo* p = findPlugin(c->plugin.type);
  if (!p || p->kind != PluginKind::Generator) return nullptr;
  // An audio track's sound (the sample player holding the track's audio regions) is not an instrument to
  // replace. R09-02: told by the track, not by the instrument: an instrument that can also play a sample
  // (Tercet, the New tracks default) on an instrument track is the track's instrument.
  const TrackMode mode = trackmodes::modeOf(s, track);
  if (mode == TrackMode::Audio) return nullptr;
  if (mode != TrackMode::Instrument)
    for (const Clip& k : s.playlist)
      if (k.track == track && k.kind == ClipKind::Audio && k.source == c->id) return nullptr;
  return c;
}

// The Library lists instruments that make sound on their own: sample players are left out (a patch
// without its sample is silent), except Tercet, whose sample oscillator is one of its sources.
bool listedInstrument(const PluginInfo& p) { return !p.usesSample || p.type == "gl.synth"; }


}  // namespace

const std::vector<std::string>& kinds() {
  static const std::vector<std::string> k = {"Bass", "Drum kit", "Drums", "Keys", "Plucked", "Bells and mallets", "Pads and strings", "Leads", "Synth", "Textures"};
  return k;
}

std::vector<Item> items(const App& a, int nav, const std::string& filterIn) {
  std::vector<Item> out;
  const std::string f = lower(trimmed(filterIn));
  auto keep = [&f](const Item& it) { return f.empty() || lower(it.name + " " + it.detail).find(f) != std::string::npos; };
  const bool wantInst = nav == 0 || nav == 1 || nav >= 3, wantFx = nav == 0 || nav == 2;
  const std::string onlyKind = nav >= 3 && nav - 3 < static_cast<int>(kinds().size()) ? kinds()[static_cast<size_t>(nav - 3)] : std::string();
  if (wantInst) {
    for (const PluginInfo& p : pluginCatalog()) {
      if (p.hidden || p.kind != PluginKind::Generator || !listedInstrument(p) || !collabui::pluginAllowed(a, p.type)) continue;
      if (p.presets.empty()) {
        Item it{p.type, -1, p.name, p.name, soundKind(p, p.name), false};
        if ((onlyKind.empty() || it.kind == onlyKind) && keep(it)) out.push_back(it);
      }
      for (size_t i = 0; i < p.presets.size(); ++i) {
        Item it{p.type, static_cast<int>(i), p.presets[i].name, p.name, soundKind(p, p.presets[i].name), false};
        if ((onlyKind.empty() || it.kind == onlyKind) && keep(it)) out.push_back(it);
      }
    }
    if (onlyKind.empty())
      for (const PluginInfo* p : dynamicPlugins()) {
        if (p->kind != PluginKind::Generator || !installedListed(a, *p)) continue;
        Item it{p->type, -1, p->name, p->category.empty() ? std::string(i18n::tr("Installed")) : p->category, "Installed", false};
        if (keep(it)) out.push_back(it);
      }
  }
  if (wantFx) {
    for (const PluginInfo& p : pluginCatalog()) {
      if (p.hidden || p.kind != PluginKind::Effect || !collabui::pluginAllowed(a, p.type)) continue;
      Item it{p.type, -1, p.name, p.category, p.category, true};
      if (keep(it)) out.push_back(it);
    }
    for (const PluginInfo* p : dynamicPlugins()) {
      if (p->kind != PluginKind::Effect || !installedListed(a, *p)) continue;
      Item it{p->type, -1, p->name, p->category.empty() ? std::string(i18n::tr("Installed")) : p->category, "Installed", true};
      if (keep(it)) out.push_back(it);
    }
  }
  // Sorted by name, as the reference's results list (numbers and letters, case ignored).
  std::stable_sort(out.begin(), out.end(), [](const Item& x, const Item& y) {
    if (x.effect != y.effect) return !x.effect;
    return lower(x.name) < lower(y.name);
  });
  // Each patch name once, as the reference lists it: where two instruments ship a patch of the same
  // name, the first (catalogue order) stays; the other is still reached from its instrument's presets.
  out.erase(std::unique(out.begin(), out.end(), [](const Item& x, const Item& y) { return x.effect == y.effect && lower(x.name) == lower(y.name); }),
            out.end());
  return out;
}

namespace {
// The strip an effect from the Library goes onto: the selected track's (a bus's own, R10-07), else the master.
const MixerTrack* effectStrip(const App& a) {
  const Song& s = a.doc.song();
  const int track = targetTrack(a);
  const ObjectId bus = busOf(a, track);
  const MixerTrack* st = bus != kNoId ? s.mixerTrack(bus) : track >= 0 ? logicplug::trackStrip(s, track) : nullptr;
  if (!st && !s.mixer.empty()) st = &s.mixer[0];
  return st;
}

// D6 (R12-05): the sound's own strip takes the patch's name as the track header does, when the strip
// still had the old name (or none) and no other sound plays into it. A name the user gave stays.
void stripFollows(App& a, ObjectId channel, const std::string& oldName, const std::string& name) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(channel);
  const MixerTrack* m = c ? s.mixerTrack(c->mixerTrack) : nullptr;
  if (!m || s.mixerIndex(m->id) <= 0 || m->name == name) return;
  if (!m->name.empty() && m->name != oldName) return;
  for (const Channel& o : s.channels) {
    if (o.mixerTrack == m->id && o.id != channel) return;
  }
  a.doc.editMixerTrack(m->id, "name mixer track", [&name](MixerTrack& x) { x.name = name; });
}

// `add` (Option-click): a new insert even while browsing (the Library's slot is then that new one).
bool loadItem(App& a, const Item& it, bool add) {
  const PluginInfo* info = findPlugin(it.type);
  if (!info) return false;
  const Song& s0 = a.doc.song();
  const int track = targetTrack(a);
  const ObjectId bus = busOf(a, track);
  if (it.effect) {
    // D5 (R11-04): into the strip's Library slot when browsing holds one (the effect there is replaced,
    // one undo step), else its first free slot.
    const MixerTrack* st = effectStrip(a);
    if (!st) return false;
    int at = add ? -1 : browsedSlot(*st);
    if (at >= 0 && st->slots[static_cast<size_t>(at)].plugin.type == it.type) {
      a.selMixer = st->id;
      return true;  // already there: nothing to change
    }
    for (int k = 0; k < kEffectSlots && at < 0; ++k)
      if (st->slots[static_cast<size_t>(k)].plugin.type.empty()) at = k;
    if (at < 0) {
      a.notify(i18n::tr("Every effect slot of this track is taken"));
      return false;
    }
    const ObjectId id = st->id;
    a.selMixer = id;
    const bool replacing = !st->slots[static_cast<size_t>(at)].plugin.type.empty();
    if (!a.doc.setSlotPlugin(id, at, it.type)) return false;
    if (replacing) pui::pluginReplaced(PluginTarget{id, at});
    LibSlot* l = libSlotOf(id);
    if (!l) {
      gLibSlots.push_back(LibSlot{id, {}});
      l = &gLibSlots.back();
    }
    // the slot used now goes last (a new one when the slot was free: what the Library put there before is gone)
    LibUse u{at, {}};
    for (size_t k = 0; k < l->uses.size(); ++k) {
      if (l->uses[k].slot != at) continue;
      if (replacing) u = l->uses[k];
      l->uses.erase(l->uses.begin() + static_cast<std::ptrdiff_t>(k));
      break;
    }
    if (std::find(u.types.begin(), u.types.end(), it.type) == u.types.end()) u.types.push_back(it.type);
    l->uses.push_back(std::move(u));
    if (l->uses.size() > static_cast<size_t>(kEffectSlots)) l->uses.erase(l->uses.begin());
    return true;
  }
  if (bus != kNoId) {
    // R10-07: a bus plays no instrument. The reference's Library offers a bus channel-strip settings
    // only; a patch is not put on a new track behind the user's back: it is refused, and said.
    a.notify(playlistTrackName(s0, static_cast<size_t>(track)) + i18n::tr(" is a bus: select an instrument track for a patch, or pick an effect for the bus"));
    return false;
  }
  PluginState ps = it.preset >= 0 && static_cast<size_t>(it.preset) < info->presets.size() ? presetState(*info, static_cast<size_t>(it.preset))
                                                                                          : defaultPluginState(it.type);
  const std::string name = it.name;
  if (const Channel* c = targetSound(a, track)) {
    // The track's sound takes the patch: its instrument and settings; a name that was the old patch's
    // (or the instrument's own) follows, and so does the track's.
    const ObjectId id = c->id;
    const std::string oldName = c->name;
    const PluginInfo* oldInfo = findPlugin(c->plugin.type);
    bool autoName = channelNameIsAutomatic(*c) || (oldInfo && oldName == oldInfo->name);
    if (oldInfo)
      for (const auto& pr : oldInfo->presets) autoName = autoName || pr.name == oldName;
    const PlaylistTrack& pt = s0.tracks[static_cast<size_t>(track)];
    const bool trackFollows = pt.name.empty() || pt.name == oldName;
    const bool sameType = c->plugin.type == it.type;
    a.doc.beginGroup("load patch");
    if (!sameType) a.doc.setChannelPlugin(id, it.type);
    a.doc.editChannel(id, "load patch", [&ps, &name, autoName](Channel& ch) {
      ch.plugin.params = ps.params;
      ch.plugin.data = ps.data;
      if (!ps.ins.empty()) ch.plugin.ins = ps.ins;
      if (autoName) ch.name = name;
    });
    if (autoName && trackFollows) a.doc.editPlaylistTrack(track, "name track", [&name](PlaylistTrack& x) { x.name = name; });
    if (autoName) stripFollows(a, id, oldName, name);
    a.doc.endGroup();
    if (!sameType) pui::pluginReplaced(PluginTarget{id, -1});
    a.selectChannel(id);
    if (const Channel* nc = a.doc.song().channel(id)) gLoaded[id] = Loaded{itemKey(it), nc->plugin.type, nc->plugin.params};
    return true;
  }
  // No instrument on the selected track: a new instrument track for the patch, as the reference does
  // on an empty or audio track (the first unused track, else a new one at the end).
  a.doc.beginGroup("load patch");
  const ObjectId id = a.addInstrument(it.type);
  if (id == kNoId) {
    a.doc.endGroup();
    return false;
  }
  a.doc.editChannel(id, "load patch", [&ps, &name](Channel& ch) {
    ch.plugin.params = ps.params;
    ch.plugin.data = ps.data;
    if (!ps.ins.empty()) ch.plugin.ins = ps.ins;
    ch.name = name;
  });
  const Song& s = a.doc.song();
  auto unused = [&s](int k) {
    bool used = trackmodes::modeOf(s, k) != TrackMode::Unassigned;
    for (const Clip& c : s.playlist) used = used || c.track == k;
    return !used;
  };
  // R09-02: the selected track itself when it is empty (the patch loads onto the selected track)
  int t = track >= 0 && track < static_cast<int>(s.tracks.size()) && unused(track) ? track : -1;
  for (int k = 0; k < static_cast<int>(s.tracks.size()) && t < 0; ++k)
    if (unused(k)) t = k;
  if (t < 0 && ops::insertPlaylistTrack(a.doc, static_cast<int>(s.tracks.size()))) t = static_cast<int>(a.doc.song().tracks.size()) - 1;
  if (t >= 0) {
    trackmodes::makeInstrumentTrack(a.doc, t, id);
    a.doc.editPlaylistTrack(t, "name track", [&name](PlaylistTrack& x) { x.name = name; });
    a.playlist.trackSel = {t};
  }
  if (const PluginInfo* pi = findPlugin(it.type)) stripFollows(a, id, pi->name, name);
  a.doc.endGroup();
  a.selectChannel(id);
  if (const Channel* nc = a.doc.song().channel(id)) gLoaded[id] = Loaded{itemKey(it), nc->plugin.type, nc->plugin.params};
  return true;
}

// Whether row `it` is what the sound `c` plays now (the loaded mark).
bool isLoaded(const Channel* c, const Item& it) {
  if (!c || it.effect || c->plugin.type != it.type) return false;
  const auto l = gLoaded.find(c->id);
  if (l != gLoaded.end() && l->second.type == c->plugin.type && l->second.params == c->plugin.params) return l->second.key == itemKey(it);
  return c->name == it.name;
}

}  // namespace

bool load(App& a, const Item& it) { return loadItem(a, it, false); }

// ------------------------------------------------------------------------ preview ----
//
// A click on a patch plays one C5 note of it, so a sound can be heard while browsing without touching
// the song. The note is rendered by a separate instance of the patch (a one-note song through the
// offline engine, so a patch sounds as it will on a track) on a worker thread, then played on the
// engine's preview output (straight to the speakers, as the browser's sample preview: not recorded,
// not in the song, no undo step, nothing sent to collaborators). Each preview cuts the one before. One
// worker at a time: a click while one renders cancels it and queues the newest; a render that ends
// after the selection moved on is not played. Built-in patches are kept (a few) so a row clicked
// again plays at once; a hosted plugin is loaded each time (its calls that must run on the interface
// thread are served there each frame, so the window stays alive) and its row says Loading meanwhile.

namespace {

constexpr int kPreviewPitch = 60;      // C5 (MIDI 60)
constexpr int kPreviewVelocity = 100;  // of 127, the piano roll's default
constexpr size_t kPreviewCacheSize = 12;

struct PreviewRequest {
  uint64_t serial = 0;
  std::string key;  // the row (itemKey)
  PluginState state;
  int sampleRate = 44100;
};

struct PreviewJob {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::mutex mu;
  PreviewRequest req;  // what the worker renders (written before it starts, read-only meanwhile)
  SamplePtr result;    // under mu
};
PreviewJob gPv;
bool gPvQueued = false;
PreviewRequest gPvNext;
uint64_t gPvSerial = 0;  // the latest request: only its render is played
int gPvSince = 0;        // frame the latest request was made (the Loading mark waits a moment)
struct CachedPreview {
  std::string key;
  int sampleRate = 0;
  std::vector<float> params;
  std::string data;
  SamplePtr sample;
};
std::vector<CachedPreview> gPvCache;
// Dev / tests: previews played, the last one's row and sound.
int gPvPlayed = 0;
std::string gPvLastKey;
SamplePtr gPvLast;

bool previewOn(const App& a) { return a.settings.getBool("smooth.libraryPreview"); }

// The one-note song: the patch on a channel into the master, one C5 note half a second long, then
// a second and a half for its release. The silence after the sound is cut off; a short fade ends it.
SamplePtr renderPreview(const PreviewRequest& r, const std::atomic<bool>* cancel) {
  Song s = makeEmptySong();
  s.tempo = 120;
  Channel c;
  c.id = s.allocateId();
  c.name = "Preview";
  c.plugin = r.state;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  Pattern& p = s.patterns[0];
  p.bars = 1;
  Note n;
  n.start = 0;
  n.length = s.ppq;  // a beat: half a second at 120
  n.pitch = kPreviewPitch;
  n.velocity = kPreviewVelocity;
  p.notes[c.id] = {n};
  Clip k;
  k.id = s.allocateId();
  k.pattern = p.id;
  k.length = s.ppq;
  s.playlist.push_back(k);
  RenderOptions o;
  o.sampleRate = r.sampleRate;
  o.tail = s.ppq * 3;
  o.cancel = cancel;
  const AudioBuffer b = renderSong(s, o);
  if (cancel && cancel->load()) return nullptr;
  size_t len = b.frames;
  const float* L = b.channels > 0 ? b.plane(0) : nullptr;
  const float* R = b.channels > 1 ? b.plane(1) : L;
  if (!L || len == 0) return nullptr;
  while (len > 0 && std::fabs(L[len - 1]) < 1e-4f && std::fabs(R[len - 1]) < 1e-4f) --len;
  if (len == 0) return nullptr;
  auto out = std::make_shared<Sample>();
  out->path = "library-preview";
  out->sampleRate = r.sampleRate;
  out->left.assign(L, L + len);
  out->right.assign(R, R + len);
  const size_t fade = std::min(len, static_cast<size_t>(r.sampleRate / 20));  // 50 ms
  for (size_t i = 0; i < fade; ++i) {
    const float g = static_cast<float>(i) / static_cast<float>(fade);
    out->left[len - 1 - i] *= g;
    out->right[len - 1 - i] *= g;
  }
  return out;
}

void startWorker(const PreviewRequest& r) {
  gPv.req = r;
  gPv.cancel = false;
  gPv.finished = false;
  gPv.running = true;
  gPv.worker = std::thread([]() {
    host::UiThreadRenderScope scope;  // a hosted plugin's interface-thread calls are served there
    SamplePtr smp;
    try {  // an exception must not leave this thread
      smp = renderPreview(gPv.req, &gPv.cancel);
    } catch (...) {
      crash::reportCurrentException("Previewing a Library sound");
      smp = nullptr;
    }
    {
      std::lock_guard<std::mutex> lk(gPv.mu);
      gPv.result = std::move(smp);
    }
    gPv.finished = true;
    gPv.running = false;
  });
}

void play(App& a, const std::string& key, SamplePtr smp) {
  a.engine.preview(smp);
  ++gPvPlayed;
  gPvLastKey = key;
  gPvLast = std::move(smp);
}

// Once a frame: a finished render is played when it is still the latest request; the queued one starts.
void pollPreview(App& a) {
  if (gPv.running) {
    host::runUiThreadTasks();
  }
  if (gPv.finished.exchange(false)) {
    if (gPv.worker.joinable()) {
      gPv.worker.join();
    }
    SamplePtr smp;
    {
      std::lock_guard<std::mutex> lk(gPv.mu);
      smp = std::move(gPv.result);
    }
    const PreviewRequest& r = gPv.req;
    if (smp && !isHostedType(r.state.type)) {
      gPvCache.push_back(CachedPreview{r.key, r.sampleRate, r.state.params, r.state.data, smp});
      if (gPvCache.size() > kPreviewCacheSize) {
        gPvCache.erase(gPvCache.begin());
      }
    }
    if (smp && r.serial == gPvSerial && !gPvQueued && previewOn(a)) {
      play(a, r.key, smp);
    }
  }
  if (gPvQueued && !gPv.running && !gPv.worker.joinable()) {
    gPvQueued = false;
    startWorker(gPvNext);
  }
}

// Whether the row's preview is still on its way (its Loading mark).
bool previewPending(const std::string& key) {
  const bool queued = gPvQueued && gPvNext.key == key && gPvNext.serial == gPvSerial;
  const bool rendering = gPv.running && gPv.req.key == key && gPv.req.serial == gPvSerial;
  return queued || rendering;
}

void requestPreview(App& a, const Item& it) {
  if (it.effect || !previewOn(a)) {
    return;
  }
  const PluginInfo* info = findPlugin(it.type);
  if (!info || info->kind != PluginKind::Generator) {
    return;
  }
  PreviewRequest r;
  r.serial = ++gPvSerial;
  r.key = itemKey(it);
  r.state = it.preset >= 0 && static_cast<size_t>(it.preset) < info->presets.size() ? presetState(*info, static_cast<size_t>(it.preset))
                                                                                  : defaultPluginState(it.type);
  r.sampleRate = std::max(8000, static_cast<int>(std::lround(a.engine.sampleRate())));
  a.stopPreview();  // the note before stops now, whatever comes next
  gPvQueued = false;
  if (gPv.running) {
    gPv.cancel = true;  // its result would not be played
  }
  for (const CachedPreview& c : gPvCache) {
    if (c.key == r.key && c.sampleRate == r.sampleRate && c.params == r.state.params && c.data == r.state.data) {
      play(a, r.key, c.sample);
      return;
    }
  }
  gPvNext = std::move(r);
  gPvQueued = true;
  gPvSince = ImGui::GetFrameCount();
  pollPreview(a);  // starts at once when no render runs
}

// The selection moves to `it` (a click, Up / Down): a patch is heard, nothing else changes.
void selectItem(App& a, const Item& it) {
  gSel = itemKey(it);
  requestPreview(a, it);
}

// A double-click or Enter: the row is loaded (a patch replaces the selected track's sound).
void openItem(App& a, const Item& it, bool add) {
  gSel = itemKey(it);
  loadItem(a, it, add);
}

}  // namespace

void select(App& a, const Item& it) { selectItem(a, it); }

void shutdown() {
  gPvQueued = false;
  gPv.cancel = true;
  host::serveUiThreadTasksWhile(gPv.running);
  if (gPv.worker.joinable()) {
    gPv.worker.join();
  }
  gPvCache.clear();
  gPvLast = nullptr;
}

void frame(App& a) {
  pollPreview(a);  // (any layout: a render finishing after a switch is still collected)
  const bool on = logicplug::active(a) && layoutui::shellBuilt(a, layout::Preset::Logic);
  if (!on) {
    gWasActive = false;
    return;
  }
  const uint64_t ep = a.doc.projectEpoch();
  const bool shown = libraryShown(a);
  if (ep != gEpoch || !gWasActive) {  // a project opened or made, or Smooth just came on: as the project says
    if (ep != gEpoch) {
      gLibSlots.clear();
      gLoaded.clear();
      gSel.clear();
    }
    gEpoch = ep;
    gWasActive = true;
    const bool want = a.doc.song().smooth.library != "closed";
    if (want != shown) layoutui::setRegionVisible(a, kRegion, want);
    gShown = libraryShown(a);
    return;
  }
  if (shown != gShown) {  // shown or hidden here (B, the control bar, the View menu): noted in the project
    gShown = shown;
    const char* v = shown ? "open" : "closed";
    if (a.doc.song().smooth.library != v) a.doc.patchSong([v](Song& x) { x.smooth.library = v; });
  }
}

// ------------------------------------------------------------------------ drawing ----

namespace {

// The header: the selected track's sound as a large icon and its patch name (248, 264).
void header(App& a, float w) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const int track = targetTrack(a);
  const Channel* c = targetSound(a, track);
  const bool bus = busOf(a, track) != kNoId;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // K-112D (audit H2): the pane's name centred on top, then the sound's picture (about 56 px) and its
  // patch name (248, 264).
  const float h = 104;
  ImGui::Dummy(ImVec2(w, h));
  const float cx = std::round(p.x + w * 0.5f);
  {
    // D9 (R14-08): the pane headers' one style (the inspector's: bold, the body size, the text colour) on
    // the arrange toolbar's centre line (it was 12 px dim, 1.5 px above that line).
    const char* lt = i18n::tr("Library");
    const float lfs = ImGui::GetFontSize();
    const float lw = a.fontBold->CalcTextSizeA(lfs, 1e9f, 0, lt).x;
    dl->AddText(a.fontBold, lfs, ImVec2(std::round(cx - lw * 0.5f), layoutparts::paneTitleY(lfs)), P.text, lt);
    // The preview speaker at the right of the title line: on, a click on a patch plays it; off, a click
    // only selects. A plain glyph on the pane, lit when on, as the header's other marks.
    const bool on = previewOn(a);
    const float bs = 20, bx = std::round(p.x + w - bs - 2), by = std::round(layoutparts::paneTitleY(lfs) + lfs * 0.5f - bs * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    const bool click = ImGui::InvisibleButton("##smlibpreview", ImVec2(bs, bs));
    const bool hov = ImGui::IsItemHovered();
    if (hov) {
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bs, by + bs), theme::mix(P.surface800, P.text, 0.08f), 5.0f);
      ui::Hint(on ? "Preview on: a click on a patch plays a C5 note of it (double-click loads it). Click to turn the preview off"
                  : "Preview off: a click on a patch only selects it (double-click loads it). Click to turn the preview on");
    }
    ui::drawIcon(dl, on ? ui::Icon::SpeakerSmall : ui::Icon::SpeakerSmallOff, ImVec2(bx + bs * 0.5f, by + bs * 0.5f), 13,
                 on ? (hov ? P.text : P.textDim) : P.textFaint);
    if (click) {
      a.settings.set("smooth.libraryPreview", !on);
      if (on) {
        a.stopPreview();
      }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(0, 0));
  }
  std::string busTitle = bus ? playlistTrackName(s, static_cast<size_t>(track)) : std::string();
  // D7 (R13-04): a track that plays several sounds through a bus (a kit) is that bus here, as the
  // inspector and Smart Controls show it, not its first sound.
  if (c && !bus) {
    const MixerTrack* st = logicplug::trackStrip(s, track);
    const int si = st ? s.mixerIndex(st->id) : -1;
    if (st && si > 0 && st->id != c->mixerTrack) {
      busTitle = mixerTrackName(s, static_cast<size_t>(si));
      c = nullptr;
    }
  }
  const bool kit = !c && !busTitle.empty() && !bus;
  const int ic = c && c->icon > 0 ? c->icon
                                  : trackicons::guess(c ? c->name : busTitle, c ? "Synth" : kit ? "Drums" : bus ? "Bus" : "Audio");
  const ImVec2 b0(cx - 28, p.y + 22), b1(cx + 28, p.y + 78);
  dl->AddRectFilled(b0, b1, theme::mix(P.surface800, P.text, 0.07f), 10.0f);
  if (ic > 0) trackicons::draw(dl, ic, ImVec2(cx, p.y + 50), 38, c ? theme::mix(theme::content(c->color), P.text, 0.30f) : P.textDim);
  const char* title = c ? c->name.c_str() : !busTitle.empty() ? busTitle.c_str() : track >= 0 && track < static_cast<int>(s.tracks.size()) ? i18n::tr("No instrument") : "";
  const float fs = theme::kFontBody - 1;  // audit X6: a name at 13 px
  const float tw = std::min(a.fontBold->CalcTextSizeA(fs, 1e9f, 0, title).x, w - 12);
  theme::textEllipsis(dl, a.fontBold, fs, ImVec2(std::round(cx - tw * 0.5f), p.y + 84), w - 12, P.text, title);
}

// A navigation row: an icon and a name, the chosen one on a rounded fill.
bool navRow(App& a, const char* id, ui::Icon icon, const char* label, bool on, float w, bool chevron = false) {
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 22;
  const bool click = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (on || hov) dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + w, p.y + h - 1), on ? theme::mix(P.surface800, P.text, 0.12f) : theme::mix(P.surface800, P.text, 0.05f), 5.0f);
  const float fs = theme::kFontCaption;
  // D7 polish: the icon's left edge on the same 8 px line as the plain rows' names and the "Sounds"
  // heading (it sat at 6), its name 6 px after it.
  if (icon != ui::Icon::Count) ui::drawIcon(dl, icon, ImVec2(p.x + 14, p.y + h * 0.5f), 12, on ? P.text : P.textDim);
  const float tx = icon != ui::Icon::Count ? p.x + 26 : p.x + 8;
  if (chevron) ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(p.x + w - 10, p.y + h * 0.5f), 9, P.textFaint);  // D4: opens its list (264)
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(tx, std::round(p.y + (h - fs) * 0.5f) - 1), p.x + w - tx - (chevron ? 20 : 4), on ? P.text : P.textDim, label);
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  ImGui::Dummy(ImVec2(0, 0));
  return click;
}

void navHeading(App& a, const char* label, float w) {
  const auto& P = pal();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // D9 (R14-15): a row of the list's own 22 px pitch, its name centred as the rows' are (it sat at the foot
  // of a 20 px row: 32 px from Effects, 19 to Bass, against the list's 22).
  const float h = 22;
  ImGui::Dummy(ImVec2(w, h));
  const float fs = theme::kFontLabel;
  ImGui::GetWindowDrawList()->AddText(a.fontBold, fs, ImVec2(p.x + 8, std::round(p.y + (h - fs) * 0.5f) - 1), P.textFaint, label);
}

void navLine(float w) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(w, 8));
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 6, std::round(p.y + 4)), ImVec2(p.x + w - 6, std::round(p.y + 4)), theme::withAlpha(pal().line, 0.9f));
}

// A row's badge: a small rounded square, its colour and glyph by the kind of sound (bass, keys, pads,
// drums ...), effects and installed plugins their own; muted tones that sit on the graphite.
void kindBadge(ImDrawList* dl, const Item& it, ImVec2 c) {
  struct K {
    const char* kind;
    uint32_t col;
    ui::Icon icon;
  };
  static const K kinds[] = {{"Bass", 0xC9902E, ui::Icon::Wave},          {"Drum kit", 0x4F8FD8, ui::Icon::CatDrums},
                            {"Drums", 0x4F8FD8, ui::Icon::CatDrums},       {"Keys", 0x3FA35B, ui::Icon::Piano},
                            {"Plucked", 0xC9902E, ui::Icon::CatPitch},       {"Bells and mallets", 0x2FA6A0, ui::Icon::Piano},
                            {"Pads and strings", 0x8A67D6, ui::Icon::CatModulation}, {"Leads", 0xD9644A, ui::Icon::CatSynth},
                            {"Synth", 0x6E62D9, ui::Icon::CatSynth},       {"Textures", 0x8A67D6, ui::Icon::CatSpectral},
                            {"Installed", 0x5C6470, ui::Icon::Plug}};
  uint32_t col = 0x3FA35B;
  ui::Icon icon = ui::Icon::Piano;
  if (it.effect) col = 0x5C6470, icon = ui::Icon::CatFilter;
  else
    for (const K& k : kinds)
      if (it.kind == k.kind) col = k.col, icon = k.icon;
  dl->AddRectFilled(ImVec2(c.x - 7, c.y - 7), ImVec2(c.x + 7, c.y + 7), theme::rgb(col), 3.0f);
  ui::drawIcon(dl, icon, c, 9, theme::contrastText(theme::rgb(col)));
}

}  // namespace

void draw(App& a) {
  const auto& P = pal();
  const float w = ImGui::GetContentRegionAvail().x;
  {  // R10-07: a bus selected: the Library shows the effects it can take (the reference offers a bus's
     // channel-strip settings); the view comes back when an instrument or audio track is selected again
    const bool bus = busOf(a, targetTrack(a)) != kNoId;
    if (bus && gNavBeforeBus < 0) gNavBeforeBus = gNav, gNav = 2, gDrill = true;
    else if (!bus && gNavBeforeBus >= 0) gNav = gNavBeforeBus, gNavBeforeBus = -1;
  }
  header(a, w);
  // The search field, full width, a rounded dark field as 111 and 252 draw it.
  ImGui::SetNextItemWidth(-1);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(P.field));
  ImGui::InputTextWithHint("##smlibfind", i18n::tr("Search"), gFilter, sizeof gFilter);
  ImGui::PopStyleColor();
  ui::Hint("Search every patch and effect: type part of a name");
  ImGui::Dummy(ImVec2(0, 2));
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float avH = ImGui::GetContentRegionAvail().y;
  const float footH = 30;
  // Two columns: the navigation, then the results (111, 252); the navigation takes about a third.
  // D4 (R10-05): a narrow Library (under 230 px, as at 1440) shows one column at a time, the reference's
  // column browser in a narrow pane (264): the kinds with a chevron, then the chosen kind's patches under
  // a back row, so each name has the pane's whole width; a search shows its results at once.
  const bool oneCol = w < 230.0f, search = gFilter[0] != 0;
  const bool showNav = !oneCol || (!gDrill && !search), showList = !oneCol || gDrill || search;
  const float navW = oneCol ? w : std::clamp(std::floor(w * 0.38f), 84.0f, 150.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (!oneCol) dl->AddLine(ImVec2(o.x + navW + 2, o.y), ImVec2(o.x + navW + 2, o.y + avH - footH), theme::withAlpha(P.line, 0.9f));
  if (showNav) {
  ImGui::BeginChild("##smlibnav", ImVec2(navW, avH - footH), ImGuiChildFlags_None);
  {
    const float nw = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    struct Nav {
      ui::Icon icon;
      const char* name;
    };
    const Nav top[] = {{ui::Icon::ListBullets, i18n::N_("All")}, {ui::Icon::Piano, i18n::N_("Instruments")}, {ui::Icon::CatFilter, i18n::N_("Effects")}};
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i);
      if (navRow(a, "##nav", top[i].icon, i18n::tr(top[i].name), gNav == i, nw, oneCol)) gNav = i, gDrill = true;
      ImGui::PopID();
    }
    navLine(nw);
    navHeading(a, i18n::tr("Sounds"), nw);
    for (size_t k = 0; k < kinds().size(); ++k) {
      ImGui::PushID(static_cast<int>(10 + k));
      if (navRow(a, "##kind", ui::Icon::Count, i18n::tr(kinds()[k].c_str()), gNav == static_cast<int>(3 + k), nw, oneCol)) gNav = static_cast<int>(3 + k), gDrill = true;
      ImGui::PopID();
    }
    ImGui::PopStyleVar();
  }
  ImGui::EndChild();
  }
  float listX = o.x + navW + 5, listW = w - navW - 5, listY = o.y;
  if (oneCol) {
    listX = o.x, listW = w;
    if (showList && !search) {  // the back row: the kind shown, a click goes back to the kinds
      static const char* const topNames[3] = {i18n::N_("All"), i18n::N_("Instruments"), i18n::N_("Effects")};
      const char* nm = gNav < 3 ? i18n::tr(topNames[gNav]) : i18n::tr(kinds()[static_cast<size_t>(gNav - 3)].c_str());
      ImGui::SetCursorScreenPos(ImVec2(o.x, o.y));
      const bool back = ImGui::InvisibleButton("##smlibback", ImVec2(w, 24));
      const bool hov = ImGui::IsItemHovered();
      if (hov) dl->AddRectFilled(ImVec2(o.x, o.y + 1), ImVec2(o.x + w, o.y + 23), theme::mix(P.surface800, P.text, 0.05f), 5.0f);
      ui::drawIcon(dl, ui::Icon::ChevronLeft, ImVec2(o.x + 10, o.y + 12), 10, hov ? P.text : P.textDim);
      const float fs = theme::kFontCaption;
      theme::textEllipsis(dl, a.fontBold, fs, ImVec2(o.x + 22, std::round(o.y + (24 - fs) * 0.5f) - 1), w - 28, P.text, nm);
      if (hov) ui::Hint("Back to the kinds of sound");
      if (back) gDrill = false;
      dl->AddLine(ImVec2(o.x + 4, o.y + 24), ImVec2(o.x + w - 4, o.y + 24), theme::withAlpha(P.line, 0.9f));
      listY = o.y + 26;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(listX, listY));
  // The rows are made again only when the view or the search changes, or once a second (plugins
  // installed or allowed meanwhile): no list is built per frame.
  static std::vector<Item> rows;
  static int rowsNav = -1;
  static std::string rowsFilter;
  static int rowsFrame = -1000;
  const int fr = ImGui::GetFrameCount();
  if (rowsNav != gNav || rowsFilter != gFilter || fr - rowsFrame >= 60 || fr < rowsFrame) {
    rows = items(a, gNav, gFilter);
    rowsNav = gNav;
    rowsFilter = gFilter;
    rowsFrame = fr;
  }
  // Up / Down move the selection (a patch is heard), Enter loads it, while the Library has the focus
  // and no field takes the typing. A key the Library uses is locked for the frame, so no shortcut also
  // acts on it.
  int keyMove = 0;
  bool keyOpen = false;
  {
    ImGuiIO& io = ImGui::GetIO();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && !rows.empty() && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift && !keys::capturing() &&
        ImGui::GetActiveID() == 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
      const ImGuiID owner = ImGui::GetID("##smlibkeys");
      auto hit = [owner](ImGuiKey k) {
        if (!ImGui::IsKeyPressed(k, true)) {
          return false;
        }
        ImGui::SetKeyOwner(k, owner, ImGuiInputFlags_LockThisFrame);
        return true;
      };
      if (hit(ImGuiKey_DownArrow)) {
        keyMove = 1;
      } else if (hit(ImGuiKey_UpArrow)) {
        keyMove = -1;
      } else if (hit(ImGuiKey_Enter) || hit(ImGuiKey_KeypadEnter)) {
        keyOpen = true;
      }
    }
  }
  int selRow = -1;
  for (size_t i = 0; i < rows.size() && selRow < 0 && !gSel.empty(); ++i) {
    if (itemKey(rows[i]) == gSel) {
      selRow = static_cast<int>(i);
    }
  }
  int doSelect = -1, doOpen = -1;
  bool openAdd = false;
  if (keyMove != 0) {
    doSelect = selRow < 0 ? 0 : std::clamp(selRow + keyMove, 0, static_cast<int>(rows.size()) - 1);
    if (doSelect == selRow) {
      doSelect = -1;  // at an end: nothing new to hear
    }
    gScrollToSel = true;
  }
  if (keyOpen && selRow >= 0) {
    doOpen = selRow;
  }
  if (showList) {
  ImGui::BeginChild("##smlibrows", ImVec2(listW, std::max(40.0f, avH - footH - (listY - o.y))), ImGuiChildFlags_None);
  // D8: the rows on the navigation's 22 px pitch, so a patch sits level with the kind beside it (the
  // item spacing made them 27 px, drifting a row lower every four)
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  {
    const int track = targetTrack(a);
    const Channel* cur = targetSound(a, track);
    // D5 (R11-04): the effect the strip's Library slot holds shows as loaded, as a patch does
    const MixerTrack* fxStrip = effectStrip(a);
    const int fxAt = fxStrip ? browsedSlot(*fxStrip) : -1;
    const std::string* fxLoaded = fxAt >= 0 ? &fxStrip->slots[static_cast<size_t>(fxAt)].plugin.type : nullptr;
    const float rw = ImGui::GetContentRegionAvail().x, rh = 22;
    ImDrawList* ld = ImGui::GetWindowDrawList();
    // A key moved the selection: the list scrolls just enough to show it.
    const int showRow = doSelect >= 0 ? doSelect : selRow;
    if (gScrollToSel && showRow >= 0) {
      const float y0 = static_cast<float>(showRow) * rh, y1 = y0 + rh, top = ImGui::GetScrollY(), viewH = ImGui::GetWindowHeight();
      if (y0 < top) {
        ImGui::SetScrollY(y0);
      } else if (y1 > top + viewH) {
        ImGui::SetScrollY(y1 - viewH);
      }
    }
    gScrollToSel = false;
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(rows.size()), rh);
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
        const Item& it = rows[static_cast<size_t>(i)];
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##row", ImVec2(rw, rh));
        const bool hov = ImGui::IsItemHovered();
        // A press selects (and previews a patch); the second press of a double-click loads the row.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          if (ImGui::GetIO().MouseClickedCount[ImGuiMouseButton_Left] >= 2) {
            doOpen = i, openAdd = it.effect && ImGui::GetIO().KeyAlt;
          } else {
            doSelect = i;
          }
        }
        const bool loaded = it.effect ? fxLoaded && *fxLoaded == it.type : isLoaded(cur, it);
        const bool sel = i == (doSelect >= 0 ? doSelect : selRow);
        // Alternate rows a shade apart, the loaded patch on the selection fill (111, 252); the selected
        // row (the one heard) outlined, on a lighter fill when it is not the loaded one.
        if (i % 2 == 1) ld->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), theme::withAlpha(P.ink, 0.10f));
        if (loaded) ld->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), theme::withAlpha(P.accent, 0.38f), 4.0f);
        else if (sel) ld->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), theme::withAlpha(P.accent, 0.16f), 4.0f);
        else if (hov) ld->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), theme::withAlpha(P.text, 0.06f), 4.0f);
        if (sel) {
          ld->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f), ImVec2(p.x + rw - 0.5f, p.y + rh - 0.5f), theme::withAlpha(P.accent, 0.70f), 4.0f);
        }
        // R09 (visual gap 10): each row's badge by its kind of sound, as 111 tells its kinds apart by the
        // badge's colour and glyph (every patch had the same green one).
        kindBadge(ld, it, ImVec2(p.x + 13, p.y + rh * 0.5f));
        const float fs = theme::kFontCaption, tx = p.x + 26;
        // A preview on its way (a hosted plugin loading, or a long render): "Loading" at the right.
        float nameW = rw - (tx - p.x) - 6;
        if (sel && previewPending(itemKey(it)) && (isHostedType(it.type) || ImGui::GetFrameCount() - gPvSince > 12)) {
          const char* lt = i18n::tr("Loading...");
          const float lw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, lt).x;
          ld->AddText(a.fontRegular, fs, ImVec2(std::round(p.x + rw - 6 - lw), std::round(p.y + (rh - fs) * 0.5f) - 1), P.textFaint, lt);
          nameW -= lw + 8;
        }
        theme::textEllipsis(ld, a.fontRegular, fs, ImVec2(tx, std::round(p.y + (rh - fs) * 0.5f) - 1), nameW, P.text, it.name.c_str());
        if (hov)
          ui::Hint(it.effect ? it.name + " \xC2\xB7 " + it.detail + "   (double-click: load it on the selected track, in place of the one the Library put there; Option-double-click: add another)"
                             : it.name + " \xC2\xB7 " + it.detail + "   (click: hear it; double-click or Enter: load it onto the selected track)");
        ImGui::PopID();
      }
    if (rows.empty()) ImGui::TextDisabled("%s", i18n::tr("Nothing has that name"));
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  }
  // What the rows asked for, after they are drawn (a load changes the song the rows read).
  if (doOpen >= 0 && doOpen < static_cast<int>(rows.size())) {
    const Item it = rows[static_cast<size_t>(doOpen)];
    openItem(a, it, openAdd);
  } else if (doSelect >= 0 && doSelect < static_cast<int>(rows.size())) {
    const Item it = rows[static_cast<size_t>(doSelect)];
    selectItem(a, it);
  }
  // The footer under the results: how many items (252, 111), then Revert and Save for the selected
  // track's patch (audit H2: the reference's footer buttons). Revert loads the patch's factory
  // settings again; Save writes the sound as a preset file.
  {
    // across the whole pane (the navigation column ends above it), as the reference's footer
    const ImVec2 f(o.x, o.y + avH - footH);
    dl->AddRectFilled(f, ImVec2(f.x + w, f.y + footH), P.surface900);
    dl->AddLine(ImVec2(f.x, f.y), ImVec2(f.x + w, f.y), theme::withAlpha(P.line, 0.9f));
    char b[48];
    std::snprintf(b, sizeof b, i18n::tr("%d items"), static_cast<int>(rows.size()));
    const float fs = theme::kFontLabel;
    // D9 (R14-15): the footer's line of text on the inspector strips' name line, the window's last line
    // (it sat 3 px higher: 1056.5 against 1059.5 at 1920). The strips' name plates are 18 px and end 2 px
    // above the pane's foot, their names 2 px into them; the buttons (20 px) are centred on that plate.
    const float plateTop = f.y + footH - 20;
    dl->AddText(a.fontRegular, fs, ImVec2(f.x + 8, plateTop + 2), P.textFaint, b);
    const int track = targetTrack(a);
    const Channel* cur = targetSound(a, track);
    const Item* factory = nullptr;
    for (const Item& it : rows)
      if (cur && !it.effect && it.preset >= 0 && it.type == cur->plugin.type && it.name == cur->name) factory = &it;
    const float bh = 20, gap = 4, by = plateTop - 2;
    const char* labels[2] = {i18n::tr("Revert"), i18n::tr("Save")};
    float bw[2], total = gap;
    for (int k = 0; k < 2; ++k) total += (bw[k] = std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, labels[k]).x) + 16);
    float bx = f.x + w - total - 1;  // D8: the last button ends on the search field's right edge, as its last pixel (it stopped 3 px short)
    for (int k = 0; k < 2; ++k) {
      const bool en = k == 0 ? factory != nullptr : cur != nullptr;
      ImGui::SetCursorScreenPos(ImVec2(bx, by));
      ImGui::PushID(k);
      const bool click = ImGui::InvisibleButton("##libfoot", ImVec2(bw[k], bh)) && en;
      const bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw[k], by + bh), hov && en ? theme::mix(P.surface700, P.text, 0.10f) : P.surface700, 5.0f);
      const float lw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, labels[k]).x;
      dl->AddText(a.fontRegular, fs, ImVec2(std::round(bx + (bw[k] - lw) * 0.5f), std::round(by + (bh - fs) * 0.5f) - 1), en ? P.text : P.textFaint, labels[k]);
      if (hov) ui::Hint(k == 0 ? "Revert: the selected track's patch back to its factory settings" : "Save: the selected track's sound as a preset file");
      if (click && k == 0) load(a, *factory);
      if (click && k == 1) savePresetDialog(a, cur->id, -1);
      bx += bw[k] + gap;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + avH));
  ImGui::Dummy(ImVec2(0, 0));
}

bool devCommand(App& a, const std::vector<std::string>& args) {
  if (args.empty()) return false;
  const std::string& op = args[0];
  if (op == "nav" && args.size() > 1) {
    gNav = std::clamp(std::atoi(args[1].c_str()), 0, static_cast<int>(2 + kinds().size()));
    gDrill = true;
  } else if (op == "rows") {
    const std::vector<Item> v = items(a, gNav, gFilter);
    std::printf("smoothlib rows nav %d: %zu\n", gNav, v.size());
    for (size_t i = 0; i < v.size() && i < 400; ++i)
      std::printf("  %zu %s \"%s\" preset %d kind %s\n", i, v[i].type.c_str(), v[i].name.c_str(), v[i].preset, v[i].kind.c_str());
  } else if ((op == "load" || op == "loadadd") && args.size() > 1) {  // loadadd: Option-click
    const std::vector<Item> v = items(a, gNav, gFilter);
    const int i = std::atoi(args[1].c_str());
    const bool ok = i >= 0 && i < static_cast<int>(v.size()) && loadItem(a, v[static_cast<size_t>(i)], op == "loadadd");
    std::printf("smoothlib load %d: %s\n", i, ok ? "ok" : "no");
  } else if ((op == "select" || op == "open") && args.size() > 1) {  // a click / a double-click on row <n>
    const std::vector<Item> v = items(a, gNav, gFilter);
    const int i = std::atoi(args[1].c_str());
    if (i >= 0 && i < static_cast<int>(v.size())) {
      if (op == "select") {
        selectItem(a, v[static_cast<size_t>(i)]);
      } else {
        openItem(a, v[static_cast<size_t>(i)], false);
      }
    }
    std::printf("smoothlib %s %d: \"%s\"\n", op.c_str(), i, i >= 0 && i < static_cast<int>(v.size()) ? v[static_cast<size_t>(i)].name.c_str() : "");
  } else if ((op == "move" && args.size() > 1) || op == "enter") {  // Up / Down (move -1 / 1), Enter
    const std::vector<Item> v = items(a, gNav, gFilter);
    int cur = -1;
    for (size_t i = 0; i < v.size() && cur < 0; ++i) {
      if (itemKey(v[i]) == gSel) {
        cur = static_cast<int>(i);
      }
    }
    if (op == "enter") {
      if (cur >= 0) {
        openItem(a, v[static_cast<size_t>(cur)], false);
      }
    } else if (!v.empty()) {
      const int next = cur < 0 ? 0 : std::clamp(cur + std::atoi(args[1].c_str()), 0, static_cast<int>(v.size()) - 1);
      if (next != cur) {
        selectItem(a, v[static_cast<size_t>(next)]);
      }
    }
    std::printf("smoothlib %s: selected \"%s\"\n", op.c_str(), gSel.c_str());
  } else if (op == "wait") {  // until the preview on its way has been rendered (and played), at most 30 s
    for (int k = 0; k < 6000 && (gPvQueued || gPv.running || gPv.finished); ++k) {
      pollPreview(a);
      if (gPvQueued || gPv.running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    pollPreview(a);
  } else if (op == "previewon" && args.size() > 1) {
    a.settings.set("smooth.libraryPreview", std::atoi(args[1].c_str()) != 0);
  } else if (op == "preview") {
    // The previews played so far, the latest one's row and sound: its length, its peak and its pitch
    // (autocorrelation over 80 ms from 50 ms in), whether the engine still plays it; and the song's
    // revision and undo history, which a preview must not touch.
    double hz = 0, secs = 0;
    float peak = 0;
    if (gPvLast && gPvLast->frames() > 0) {
      const Sample& sm = *gPvLast;
      secs = sm.seconds();
      for (float x : sm.left) {
        peak = std::max(peak, std::fabs(x));
      }
      const size_t sr = static_cast<size_t>(sm.sampleRate), at = sr / 20, win = sr * 8 / 100;
      const size_t lo = sr / 2000, hi = sr / 40;
      if (sm.frames() > at + win + hi) {
        const float* x = sm.left.data() + at;
        std::vector<double> r(hi + 1, 0.0);
        double best = 0;
        for (size_t lag = lo; lag <= hi; ++lag) {
          double c = 0, e0 = 0, e1 = 0;
          for (size_t i = 0; i < win; ++i) {
            c += static_cast<double>(x[i]) * x[i + lag];
            e0 += static_cast<double>(x[i]) * x[i];
            e1 += static_cast<double>(x[i + lag]) * x[i + lag];
          }
          r[lag] = e0 > 0 && e1 > 0 ? c / std::sqrt(e0 * e1) : 0;
          best = std::max(best, r[lag]);
        }
        // the first lag (a local peak) within 90% of the best: the period, not a multiple of it
        for (size_t lag = lo + 1; lag < hi && hz == 0; ++lag) {
          if (r[lag] >= 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
            hz = static_cast<double>(sr) / static_cast<double>(lag);
          }
        }
      }
    }
    std::printf("smoothlib preview: on %d played %d pending %d last \"%s\" seconds %.2f peak %.3f hz %.1f playing %d notes 1 pitch %d velocity %d "
                "revision %llu undo %zu\n",
                previewOn(a) ? 1 : 0, gPvPlayed, gPvQueued || gPv.running ? 1 : 0, gPvLastKey.c_str(), secs, static_cast<double>(peak), hz,
                a.engine.previewing() ? 1 : 0, kPreviewPitch, kPreviewVelocity, static_cast<unsigned long long>(a.doc.revision()),
                a.doc.history().position());
  } else if (op == "newtrack") {  // R09-02 test: New tracks > Instrument, as the dialog's Create does
    newtracks::createDefault(a);
    const Song& s = a.doc.song();
    std::printf("smoothlib newtrack: tracks %zu channels %zu selected %d\n", s.tracks.size(), s.channels.size(), targetTrack(a));
  } else if (op == "tracks") {  // each used track: its mode and its sound
    const Song& s = a.doc.song();
    for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) {
      const Channel* c = logicplug::trackSound(s, t, nullptr);
      if (trackmodes::modeOf(s, t) == TrackMode::Unassigned && !c) continue;
      std::printf("smoothlib track %d \"%s\" mode %d sound \"%s\" type %s\n", t, playlistTrackName(s, static_cast<size_t>(t)).c_str(),
                  static_cast<int>(trackmodes::modeOf(s, t)), c ? c->name.c_str() : "", c ? c->plugin.type.c_str() : "");
    }
    std::printf("smoothlib tracks: channels %zu\n", s.channels.size());
  } else if (op == "selectbus") {  // R10-07 test: select the first bus track; its strip's effect count
    const Song& s = a.doc.song();
    int t = -1;
    for (int k = 0; k < static_cast<int>(s.tracks.size()) && t < 0; ++k)
      if (busOf(a, k) != kNoId) t = k;
    if (t >= 0) a.playlist.selected.clear(), a.playlist.trackSel = {t};
    std::printf("smoothlib selectbus: track %d\n", t);
  } else if (op == "busfx") {
    const Song& s = a.doc.song();
    const ObjectId b = busOf(a, targetTrack(a));
    int n = 0;
    std::string types;  // D5: the strip's effects in slot order (R11-04: replaced in place, not piled up)
    if (const MixerTrack* m = b != kNoId ? s.mixerTrack(b) : nullptr) {
      for (const EffectSlot& e : m->slots) {
        n += e.plugin.type.empty() ? 0 : 1;
        if (!e.plugin.type.empty()) types += (types.empty() ? "" : ",") + e.plugin.type;
      }
    }
    std::printf("smoothlib busfx: bus %d fx %d tracks %zu channels %zu slots %s\n", b != kNoId ? 1 : 0, n, s.tracks.size(), s.channels.size(),
                types.c_str());
  } else if (op == "state") {
    const int t = targetTrack(a);
    const Channel* c = targetSound(a, t);
    std::string marked;  // the row the list marks as the track's sound
    for (const Item& it : items(a, gNav, gFilter)) {
      if (marked.empty() && isLoaded(c, it)) {
        marked = it.name;
      }
    }
    std::printf("smoothlib state: shown %d saved \"%s\" nav %d track %d sound \"%s\" type %s channels %zu marked \"%s\" selected \"%s\"\n",
                libraryShown(a) ? 1 : 0, a.doc.song().smooth.library.c_str(), gNav, t, c ? c->name.c_str() : "", c ? c->plugin.type.c_str() : "",
                a.doc.song().channels.size(), marked.c_str(), gSel.c_str());
  } else {
    return false;
  }
  std::fflush(stdout);
  return true;
}

}  // namespace gl::smoothlib
