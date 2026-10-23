// Gridline — browser content in the app: content index, drag & drop of
// browser files, double-click actions and tempo-synced preview (BrowserUi.h).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Json.h"
#include "../core/TrackModes.h"
#include "../core/I18n.h"
#include "../core/MidiFile.h"
#include "../core/MidiImport.h"
#include "../core/Presets.h"
#include "../core/flp/Flp.h"
#include "../core/flp/FlpPresets.h"
#include "App.h"
#include "BrowserUi.h"
#include "ExternalDragUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::browserui {

namespace fs = std::filesystem;
using browse::DropAction;
using browse::DropItem;
using browse::DropTarget;
using browse::Kind;

// ------------------------------------------------------------------ index

browse::Index& index() {
  static browse::Index ix;
  return ix;
}

std::string flDataDir() {
  static std::string dir = [] {
    std::string base = flp::defaultFactoryData();
    std::error_code ec;
    if (!base.empty() && fs::is_directory(fs::path(base) / "Data" / "Patches", ec)) return (fs::path(base) / "Data").string();
    return std::string();
  }();
  return dir;
}

std::string contentDir(App& a, const char* name) { return (fs::path(a.configDir()) / name).string(); }

namespace {
bool indexStarted = false;

std::vector<std::pair<std::string, bool>> indexRoots(App& a) {
  std::vector<std::pair<std::string, bool>> roots;
  const std::string fl = flDataDir();
  if (!fl.empty()) {
    const fs::path p = fs::path(fl) / "Patches";
    roots.push_back({(p / "Packs").string(), true});
    roots.push_back({(p / "Impulses").string(), true});
    roots.push_back({(p / "Misc").string(), true});
    roots.push_back({(p / "Scores").string(), false});
    roots.push_back({(p / "Channel presets").string(), false});
    roots.push_back({(p / "Mixer presets").string(), false});
    roots.push_back({(p / "Plugin presets").string(), false});
    roots.push_back({(fs::path(fl) / "Demo projects").string(), false});
  }
  roots.push_back({presetRoot(a.configDir()), false});
  for (const char* d : {"Recorded", "Rendered", "Sliced audio", "Edited audio", "Seltmann loops", "Clipboard files"}) roots.push_back({contentDir(a, d), true});
  // User folders: only those added in the browser (never the default guesses).
  std::ifstream in(fs::path(a.configDir()) / "browser-folders.txt");
  for (std::string l; std::getline(in, l);)
    if (!l.empty()) roots.push_back({l, true});
  return roots;
}
}  // namespace

void ensureIndex(App& a) {
  if (indexStarted || thumbMode()) return;
  indexStarted = true;
  browse::Index& ix = index();
  ix.setCacheFile((fs::path(a.configDir()) / "browser-index.json").string());
  ix.load();
  ix.setRoots(indexRoots(a));
  ix.start();
}

namespace {
double lastPass = 0;
}

void tickIndex(App& a) {
  if (!indexStarted || index().busy()) return;
  if (lastPass <= 0) lastPass = a.now;
  if (a.now - lastPass > 240.0) {
    lastPass = a.now;
    refreshIndex(a);
  }
}

void shutdownIndex() {
  if (indexStarted) index().stop();
}

void refreshIndex(App& a) {
  if (!indexStarted) return ensureIndex(a);
  index().setRoots(indexRoots(a));
  index().start();
}

// ------------------------------------------------------------- drop items

namespace {

struct Decoded {
  DropItem item;
  PluginState state;
  ChainPreset chain;
  std::string name;
  bool ok = false;
  std::string error;
};

// Library preset (.json): its plugin type; FL preset (.fst): decoded.
const Decoded& decodeItem(const std::string& path) {
  static std::map<std::string, Decoded> cache;
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  if (cache.size() > 96) cache.clear();
  Decoded d;
  d.item.kind = browse::kindOf(path);
  d.name = fs::path(path).stem().string();
  if (d.item.kind == Kind::Preset) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    Json j;
    if (jsonParse(ss.str(), j)) {
      d.item.pluginType = j.text("type");
      if (const PluginInfo* info = findPlugin(d.item.pluginType)) {
        d.item.pluginKind = info->kind;
        d.ok = loadPreset(path, d.item.pluginType, d.state);
      }
    }
    if (!d.ok) d.item.kind = Kind::Other, d.error = "Unknown preset";
  } else if (d.item.kind == Kind::Chain) {
    d.ok = loadChain(path, d.chain);
    if (!d.ok) d.item.kind = Kind::Other;
  } else if (d.item.kind == Kind::Fst) {
    flp::PresetResult r;
    if (flp::importFlPreset(path, r, flp::defaultFactoryData())) {
      if (r.kind == flp::PresetResult::Plugin && findPlugin(r.state.type)) {
        d.item.kind = Kind::Preset;
        d.item.pluginType = r.state.type;
        d.item.pluginKind = findPlugin(r.state.type)->kind;
        d.state = r.state;
        d.ok = true;
      } else if (r.kind == flp::PresetResult::Chain) {
        d.item.kind = Kind::Chain;
        d.chain = r.chain;
        d.ok = true;
      } else {
        d.item.kind = Kind::Other;
        d.error = "No Gridline equivalent for this preset";
      }
    } else {
      d.item.kind = Kind::Other;
      d.error = r.error.empty() ? "Cannot read this FL Studio preset" : r.error;
      if (!r.unmapped.empty()) d.error = "No Gridline equivalent for " + r.unmapped.front();
    }
  } else {
    d.ok = browse::listable(d.item.kind);
  }
  return cache.emplace(path, std::move(d)).first->second;
}

// Plugin shown by a plugin window / in a mixer slot.
const PluginInfo* targetInfo(App& a, DropTarget target, const DropPlace& place) {
  const Song& s = a.doc.song();
  if (target == DropTarget::PluginWindow) return PluginTarget{place.owner, place.pluginSlot}.info(s);
  if (target == DropTarget::Rack && place.channel != kNoId) return PluginTarget{place.channel, -1}.info(s);  // J-75: a channel row
  if (target == DropTarget::MixerSlot)
    if (const MixerTrack* m = s.mixerTrack(place.mixer); m && place.slot >= 0 && place.slot < kEffectSlots)
      return findPlugin(m->slots[static_cast<size_t>(place.slot)].plugin.type);
  return nullptr;
}

// Whether the target plugin takes a dropped sample (J-97): channel rows only
// for the sample players, plugin windows for every plugin holding a sample.
bool takesSample(DropTarget target, const PluginInfo* ti) {
  if (!ti) return false;
  if (target == DropTarget::Rack) return xdrop::rowTakesSample(ti->type);
  if (target == DropTarget::PluginWindow) return xdrop::sampleSlot(ti->type) != xdrop::SampleSlot::None;
  return ti->usesSample;
}

// Convolver: the impulse source is the "File" choice (the default is a generated space).
void useFileSource(PluginState& st) {
  if (st.type != "gl.convolver") return;
  if (const PluginInfo* ci = findPlugin(st.type))
    for (size_t i = 0; i < ci->params.size() && i < st.params.size(); ++i)
      if (ci->params[i].kind == ParamKind::Choice)
        for (size_t c = 0; c < ci->params[i].choices.size(); ++c)
          if (ci->params[i].choices[c] == "File") st.params[i] = static_cast<float>(c);
}

int freeSlot(const MixerTrack& m) {
  for (int k = 0; k < kEffectSlots; ++k)
    if (m.slots[static_cast<size_t>(k)].plugin.empty()) return k;
  return -1;
}

ObjectId mixerOf(App& a, const DropPlace& place) {
  if (place.mixer != kNoId) return place.mixer;
  if (a.selMixer != kNoId) return a.selMixer;
  return a.doc.song().mixer.empty() ? kNoId : a.doc.song().mixer.front().id;
}

std::string payloadPath(const ImGuiPayload* pl) {
  return pl && pl->Data ? std::string(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize)) : std::string();
}

// J-97: replaces channel `ch`'s content in one undo step: its instrument
// (`type`; FL "Replace"), optionally with a preset's state and / or a sample.
// Routing, volume, pan, colour, steps and notes stay (they belong to the
// channel, not its instrument). The channel takes `newName` only when its old
// name was automatic (xdrop::autoChannelName).
bool replaceChannel(App& a, ObjectId ch, const std::string& type, const PluginState* preset, const std::string& sample, const std::string& newName,
                    const std::string& label) {
  const Channel* c = a.doc.song().channel(ch);
  if (!c || !findPlugin(type)) return false;
  const PluginInfo* oldInfo = findPlugin(c->plugin.type);
  const bool rename = !newName.empty() && newName != c->name &&
                      xdrop::autoChannelName(c->name, oldInfo ? oldInfo->name : std::string(),
                                             xdrop::sampleSlot(c->plugin.type) != xdrop::SampleSlot::None ? c->plugin.data.substr(0, c->plugin.data.find('\n'))
                                                                                                         : std::string());
  const bool newType = c->plugin.type != type;
  a.doc.beginGroup(label);
  bool ok = !newType || a.doc.setChannelPlugin(ch, type);
  if (ok && (preset || !sample.empty() || rename))
    ok = a.doc.editChannel(ch, label, [&](Channel& x) {
      if (preset) {
        const std::string keep = x.plugin.data;
        x.plugin = *preset;
        if (x.plugin.data.empty()) x.plugin.data = keep;  // presets without their own data keep the sample
      }
      if (!sample.empty()) x.plugin.data = xdrop::withSample(x.plugin.type, x.plugin.data, sample);
      if (rename) x.name = newName;
    });
  a.doc.endGroup();
  return ok;
}

// Adds a channel (instrument `type`, optional sample) at channel index `at`
// (< 0: after the selected channel, as the add-channel button does).
ObjectId addChannelAt(App& a, const std::string& type, const std::string& sample, int at) {
  a.doc.beginGroup("add channel");
  const ObjectId id = a.addInstrument(type, sample);
  if (id != kNoId && at >= 0) a.doc.moveChannel(id, std::min(at, static_cast<int>(a.doc.song().channels.size()) - 1));
  a.doc.endGroup();
  return id;
}

}  // namespace

bool loadChannelSample(App& a, ObjectId channel, const std::string& path, const char* label, bool keepSampleInstrument) {
  const Channel* c = a.doc.song().channel(channel);
  if (!c || path.empty()) return false;
  const bool keep = keepSampleInstrument ? xdrop::sampleSlot(c->plugin.type) != xdrop::SampleSlot::None : xdrop::rowTakesSample(c->plugin.type);
  const std::string type = keep ? c->plugin.type : std::string("gl.sampler");
  return replaceChannel(a, channel, type, nullptr, path, fs::path(path).stem().string(), label ? label : "replace sample");
}

void fileDragSource(const std::string& path, const char* label) {
  if (!ImGui::BeginDragDropSource()) return;
  const bool audio = browse::kindOf(path) == Kind::Audio;
  ImGui::SetDragDropPayload(audio ? "GL_FILE" : "GL_ITEM", path.data(), path.size());
  const std::string name = label ? std::string(label) : fs::path(path).filename().string();
  const Kind k = browse::kindOf(path);
  const ui::Icon icon = audio ? ui::Icon::Wave
                        : k == Kind::Midi || k == Kind::Score ? ui::Icon::Piano
                        : k == Kind::Chain ? ui::Icon::Mixer
                        : k == Kind::Project || k == Kind::Flp ? ui::Icon::Song
                                                               : ui::Icon::Settings;
  ui::DragPreview(icon, name.c_str(), audio ? theme::rgb(theme::role::kPaint) : 0);
  ImGui::EndDragDropSource();
}

DropAction dropActionFor(App& a, const std::string& path, DropTarget target, const DropPlace& place) {
  const Decoded& d = decodeItem(path);
  if (!d.ok) return DropAction::None;
  const PluginInfo* ti = targetInfo(a, target, place);
  return browse::dropAction(d.item, target, ti ? ti->type : "", takesSample(target, ti));
}

// ------------------------------------------------- several files (J-75)

browse::DropItem dropItemFor(const xdrop::Item& it) {
  browse::DropItem d;
  if (it.dir) {
    d.kind = Kind::Folder;
  } else if (it.pending || it.path.empty()) {
    d.kind = xdrop::kindOf(it);
  } else {
    const Decoded& dec = decodeItem(it.path);
    d = dec.item;
    if (!dec.ok && d.kind != Kind::Other) d.kind = Kind::Other;
  }
  return d;
}

namespace {

struct HoverState {
  HoverPlan plan;
  int frame = -1;
} gHover;

// Seconds of audio files, from their headers (hover runs every frame).
double cachedSeconds(const std::string& path) {
  static std::map<std::string, double> cache;
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  if (cache.size() > 256) cache.clear();
  double s = xdrop::audioHeaderSeconds(path);
  if (s < 0) {
    browse::FileMeta meta;
    if (index().lookup(path, meta) && meta.seconds > 0) s = meta.seconds;
  }
  return cache[path] = s;
}

std::string targetName(App& a, DropTarget target, const DropPlace& place) {
  const Song& s = a.doc.song();
  if (target == DropTarget::Rack && place.channel != kNoId)
    if (const Channel* c = s.channel(place.channel)) return c->name;
  if (target == DropTarget::PluginWindow) {
    if (place.pluginSlot < 0) {
      if (const Channel* c = s.channel(place.owner)) return c->name;
    } else if (const PluginInfo* i = PluginTarget{place.owner, place.pluginSlot}.info(s)) {
      return i->name;
    }
  }
  return "";
}

std::vector<xdrop::Item> payloadItems(const ImGuiPayload* pl) {
  if (pl->IsDataType("GL_FILES") || xdragui::ownsPayload(pl)) return xdragui::items();
  xdrop::Item it;
  it.path = payloadPath(pl);
  it.name = fs::path(it.path).filename().string();
  return {it};
}

void drawTargetHighlight(const ImRect& r) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImRect b = r;
  b.Expand(-1.5f);
  dl->AddRectFilled(b.Min, b.Max, theme::withAlpha(P.hot, 0.08f), theme::kRadiusSm);
  dl->AddRect(b.Min, b.Max, theme::withAlpha(P.hot, 0.85f), theme::kRadiusSm, 0, 2.0f);
}

}  // namespace

xdrop::Mods currentMods() {
  const ImGuiIO& io = ImGui::GetIO();
  xdrop::Mods m;
  m.shift = io.KeyShift;
  m.alt = io.KeyAlt;
  m.ctrl = io.KeyCtrl;
  return m;
}

xdrop::Plan planFor(App& a, const std::vector<xdrop::Item>& items, DropTarget target, const DropPlace& place, const xdrop::Mods& mods) {
  const Song& s = a.doc.song();
  xdrop::Context ctx;
  ctx.target = target;
  if (const PluginInfo* ti = targetInfo(a, target, place)) {
    ctx.targetType = ti->type;
    ctx.targetUsesSample = takesSample(target, ti);
  }
  ctx.onChannel = target == DropTarget::Rack && place.channel != kNoId;
  ctx.insertHere = target == DropTarget::Rack && place.channel == kNoId && place.insertAt >= 0;
  ctx.targetName = targetName(a, target, place);
  ctx.track = place.track;
  ctx.tick = place.tick;
  ctx.barTicks = std::max<Tick>(1, s.barTicks());
  ctx.trackCount = std::max(1, static_cast<int>(s.tracks.size()));
  ctx.fallbackLength = ctx.barTicks;
  std::vector<browse::DropItem> kinds;
  for (const xdrop::Item& it : items) {
    kinds.push_back(dropItemFor(it));
    Tick len = 0;
    if (target == DropTarget::Playlist && !it.pending && kinds.back().kind == Kind::Audio) {
      const double sec = cachedSeconds(it.path);
      browse::FileMeta meta;
      const double bpm = previewSync() ? (index().lookup(it.path, meta) && meta.bpm > 0 ? meta.bpm : browse::bpmFromName(it.name)) : 0;
      if (sec > 0) len = std::max<Tick>(1, static_cast<Tick>(std::llround(sec * (bpm > 0 ? bpm : s.tempo) / 60.0 * s.ppq)));
    }
    ctx.lengths.push_back(len);
  }
  return xdrop::plan(items, kinds, ctx, mods);
}

const HoverPlan* hoverPlan(DropTarget target) {
  if (gHover.frame != ImGui::GetFrameCount() || gHover.plan.target != target) return nullptr;
  return &gHover.plan;
}

bool acceptFileDrop(App& a, DropTarget target, const DropPlace& place) { return acceptFileDrop(a, target, place, nullptr); }

bool acceptFileDrop(App& a, DropTarget target, const DropPlace& place, DropFeedback* fb) {
  const ImGuiPayload* cur = ImGui::GetDragDropPayload();
  if (!cur) return false;
  const char* type = cur->IsDataType("GL_FILE") ? "GL_FILE" : cur->IsDataType("GL_ITEM") ? "GL_ITEM" : cur->IsDataType("GL_FILES") ? "GL_FILES" : nullptr;
  if (!type) return false;
  const std::vector<xdrop::Item> items = payloadItems(cur);
  if (items.empty()) return false;
  const xdrop::Mods mods = currentMods();
  const xdrop::Plan p = planFor(a, items, target, place, mods);
  if (!p.accepted()) {
    if (!p.label.empty()) {
      ImGui::SetTooltip("%s", p.label.c_str());
      a.setHint(p.label);
      ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    }
    return false;
  }
  const ImRect rect = GImGui->DragDropTargetRect;
  const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(type, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery);
  if (!pl) return false;
  if (!pl->Delivery) {
    if (!fb || fb->highlight) drawTargetHighlight(rect);
    if (fb) fb->hovering = true;
    ImGui::SetTooltip("%s", p.label.c_str());
    a.setHint(p.label);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    gHover.plan = HoverPlan{target, p, items};
    gHover.frame = ImGui::GetFrameCount();
    return false;
  }
  xdragui::markConsumed();
  return performItems(a, items, target, place, mods);
}

void pluginWindowDropTarget(App& a, const PluginTarget& t) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (!w || !ImGui::GetDragDropPayload()) return;
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl->IsDataType("GL_FILE") && !pl->IsDataType("GL_ITEM") && !pl->IsDataType("GL_FILES") && !pl->IsDataType("GL_PLUGIN")) return;
  if (ImGui::BeginDragDropTargetCustom(w->Rect(), w->ID)) {
    DropPlace place;
    place.owner = t.owner;
    place.pluginSlot = t.slot;
    acceptFileDrop(a, DropTarget::PluginWindow, place);
    acceptPluginDrop(a, DropTarget::PluginWindow, place);  // J-97: replace the shown instrument / effect
    ImGui::EndDragDropTarget();
  }
}

namespace {

// One step of a drop. `made`: the clip an AudioClip / MidiPatternClip step added.
bool runAction(App& a, const std::string& path, DropAction act, DropTarget target, const DropPlace& place, bool direct, ObjectId* made) {
  const Decoded& d = decodeItem(path);
  const std::string fname = fs::path(path).filename().string();
  switch (act) {
    case DropAction::None:
      return false;
    case DropAction::NewSamplerChannel:
      a.settingsChannel = addChannelAt(a, "gl.sampler", path, target == DropTarget::Rack ? place.insertAt : -1);
      return a.settingsChannel != kNoId;
    case DropAction::ReplaceInstrument:  // J-97: another instrument's row becomes a sampler with the file
      if (!replaceChannel(a, place.channel, "gl.sampler", nullptr, path, fs::path(path).stem().string(), "replace with sampler")) return false;
      a.selectChannel(place.channel);
      return true;
    case DropAction::ReplaceFromPreset: {  // J-97: the row's instrument from a preset (FL "Replace")
      if (d.item.pluginType.empty() || !replaceChannel(a, place.channel, d.item.pluginType, &d.state, "", d.name, "replace instrument")) return false;
      a.selectChannel(place.channel);
      return true;
    }
    case DropAction::AudioClip: {
      SamplePtr s = sampleCache().get(path);
      if (!s || s->frames() == 0) {
        a.notify("Cannot decode " + fname, true);
        return false;
      }
      browse::FileMeta meta;
      const double bpm = index().lookup(path, meta) && meta.bpm > 0 ? meta.bpm : browse::bpmFromName(fname);
      a.doc.beginGroup("add audio clip");
      ObjectId ch = a.addInstrument("gl.sampler", path);
      const int track = std::clamp(place.track, 0, kMaxPlaylistTracks - 1);
      const Tick start = std::max<Tick>(0, place.tick);
      const bool sync = previewSync() && bpm > 0;
      a.doc.editSong("add audio clip", [&](Song& sg) {
        Clip c;
        c.id = sg.allocateId();
        c.kind = ClipKind::Audio;
        c.source = ch;
        c.track = track;
        c.start = start;
        const double beats = s->seconds() * (sync ? bpm : sg.tempo) / 60.0;
        c.length = std::max<Tick>(1, static_cast<Tick>(std::llround(beats * sg.ppq)));
        if (sync) c.syncBpm = bpm;
        if (!sg.tracks.empty()) c.track = std::min(track, static_cast<int>(sg.tracks.size()) - 1);
        c.track = trackmodes::placeAudioClip(sg, c.track, ch);  // PL-08: audio tracks route it, instrument tracks pass it on
        sg.playlist.push_back(c);
        a.playlist.selected = {c.id};
        if (made) *made = c.id;
      });
      a.doc.endGroup();
      return true;
    }
    case DropAction::ReplaceSample:
    case DropAction::LoadSample: {
      PluginTarget t = act == DropAction::ReplaceSample ? PluginTarget{place.channel, -1} : PluginTarget{place.owner, place.pluginSlot};
      if (t.isChannel()) {  // J-97: the channel keeps a name the user gave it
        if (!loadChannelSample(a, t.owner, path, act == DropAction::ReplaceSample ? "replace sample" : "load sample", act == DropAction::LoadSample)) return false;
        if (act == DropAction::ReplaceSample) a.selectChannel(t.owner);
        return true;
      }
      if (const PluginState* st = t.state(a.doc.song())) {
        PluginState ns = *st;
        ns.data = xdrop::withSample(ns.type, ns.data, path);  // J-97: the plugin's own settings after the path stay
        useFileSource(ns);
        setPluginState(a, t, ns, "load sample");
        return true;
      }
      return false;
    }
    case DropAction::ConvolverIR: {
      ObjectId mid = mixerOf(a, place);
      const MixerTrack* m = a.doc.song().mixerTrack(mid);
      if (!m) return false;
      int k = target == DropTarget::MixerSlot && place.slot >= 0 ? place.slot : freeSlot(*m);
      if (k < 0) {
        a.notify("No free effect slot on this track", true);
        return false;
      }
      PluginState ir = defaultPluginState("gl.convolver");
      if (ir.empty()) return false;
      ir.data = path;
      useFileSource(ir);
      a.doc.editMixerTrack(mid, "load impulse response", [&](MixerTrack& t) {
        t.slots[static_cast<size_t>(k)].plugin = ir;
        t.slots[static_cast<size_t>(k)].enabled = true;
      });
      a.fxEditor = {mid, k};
      a.selMixer = mid;
      return true;
    }
    case DropAction::ApplyPreset: {
      PluginTarget t{place.owner, place.pluginSlot};
      const PluginState* st = t.state(a.doc.song());
      if (!st) return false;
      PluginState ns = d.state;
      if (ns.data.empty()) ns.data = st->data;  // presets without their own data keep the sample
      setPluginState(a, t, ns, "load preset");
      a.notify("Loaded preset " + d.name);
      return true;
    }
    case DropAction::NewChannelFromPreset: {
      a.doc.beginGroup("add channel from preset");
      ObjectId id = addChannelAt(a, d.item.pluginType, "", target == DropTarget::Rack ? place.insertAt : -1);
      if (id != kNoId) {
        PluginState ns = d.state;
        a.doc.editChannel(id, "", [&](Channel& c) {
          c.plugin = ns;
          c.name = d.name;
        });
      }
      a.doc.endGroup();
      a.settingsChannel = id;
      return id != kNoId;
    }
    case DropAction::InsertEffectPreset: {
      ObjectId mid = mixerOf(a, place);
      const MixerTrack* m = a.doc.song().mixerTrack(mid);
      if (!m) return false;
      int k = target == DropTarget::MixerSlot && place.slot >= 0 ? place.slot : freeSlot(*m);
      if (k < 0) {
        a.notify("No free effect slot on this track", true);
        return false;
      }
      PluginState ns = d.state;
      a.doc.editMixerTrack(mid, "insert effect preset", [&](MixerTrack& t) {
        t.slots[static_cast<size_t>(k)].plugin = ns;
        t.slots[static_cast<size_t>(k)].enabled = true;
      });
      a.fxEditor = {mid, k};
      a.selMixer = mid;
      return true;
    }
    case DropAction::ApplyChain: {
      ObjectId mid = mixerOf(a, place);
      if (!a.doc.song().mixerTrack(mid)) return false;
      const ChainPreset c = d.chain;
      a.doc.editMixerTrack(mid, "load effect chain", [&c](MixerTrack& m) {
        m.slots.assign(kEffectSlots, EffectSlot{});
        for (size_t k = 0; k < c.slots.size() && k < m.slots.size(); ++k) {
          m.slots[k].plugin = c.slots[k];
          if (k < c.mix.size()) m.slots[k].mix = c.mix[k];
        }
      });
      a.selMixer = mid;
      a.notify("Loaded effect chain " + d.name);
      return true;
    }
    case DropAction::ImportMidiChannels:
    case DropAction::MidiPatternClip: {
      if (act == DropAction::ImportMidiChannels && !direct) {  // one file: the import options dialog
        a.importMidi(path);
        return true;
      }
      MidiFileData m;
      std::string err;
      if (!readMidiFile(path, m, &err)) {
        a.notify("MIDI import failed: " + err, true);
        return false;
      }
      if (m.format == 0 || m.tracks.size() == 1) splitTracksByChannel(m);
      MidiImportOptions o;
      o.placeInPlaylist = act == DropAction::ImportMidiChannels;
      o.tempo = o.timeSignature = o.markers = false;  // a dropped file adds a pattern; the song keeps its tempo and meter
      MidiImportResult r;
      a.doc.editSong("import MIDI", [&](Song& s) { r = importMidiIntoSong(s, m, fs::path(path).stem().string(), o); });
      if (!r.error.empty() || r.pattern == kNoId) {
        a.notify("MIDI import: " + (r.error.empty() ? std::string("no notes") : r.error), true);
        return false;
      }
      if (act == DropAction::MidiPatternClip) {
        const int tracks = std::max(1, static_cast<int>(a.doc.song().tracks.size()));
        const ObjectId id = a.doc.addClip(r.pattern, std::clamp(place.track, 0, tracks - 1), std::max<Tick>(0, place.tick));
        if (made) *made = id;
      }
      a.selectPattern(r.pattern);
      a.syncEngine();
      return true;
    }
    case DropAction::ImportMidiNotes: {
      MidiFileData m;
      std::string err;
      if (!readMidiFile(path, m, &err)) {
        a.notify("MIDI import failed: " + err, true);
        return false;
      }
      const Song& s = a.doc.song();
      const ObjectId pat = a.curPattern;
      const ObjectId ch = target == DropTarget::Rack && place.channel != kNoId ? place.channel : a.selChannel;  // J-97: a rack row
      const Pattern* p = s.pattern(pat);
      if (!p || !s.channel(ch)) {
        a.importMidi(path);
        return true;
      }
      std::vector<Note> notes;
      if (auto it = p->notes.find(ch); it != p->notes.end()) notes = it->second;
      size_t added = 0;
      for (const auto& tr : m.tracks)
        for (const auto& mn : tr.notes) {
          Note x;
          x.start = static_cast<Tick>(std::llround(static_cast<double>(mn.start) * s.ppq / std::max(1, m.division)));
          x.length = std::max<Tick>(1, static_cast<Tick>(std::llround(static_cast<double>(mn.length) * s.ppq / std::max(1, m.division))));
          x.pitch = mn.pitch;
          x.velocity = mn.velocity;
          notes.push_back(x);
          ++added;
        }
      std::stable_sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) { return x.start < y.start; });
      a.doc.setNotes(pat, ch, notes, "import MIDI notes");
      a.notify("Imported " + std::to_string(added) + " notes from " + fname);
      return added > 0;
    }
    case DropAction::OpenProject:
      a.guardUnsaved([&a, path]() { a.openProject(path); });
      return true;
    case DropAction::AddBrowserFolder:
      addBrowserFolder(a, path);
      return true;
  }
  return false;
}

}  // namespace

bool performDrop(App& a, const std::string& path, DropTarget target, const DropPlace& place) {
  const Decoded& d = decodeItem(path);
  if (!d.ok) {
    a.notify(d.error.empty() ? "Cannot use " + fs::path(path).filename().string() : d.error, true);
    return false;
  }
  const PluginInfo* ti = targetInfo(a, target, place);
  const DropAction act = browse::dropAction(d.item, target, ti ? ti->type : "", takesSample(target, ti));
  return runAction(a, path, act, target, place, false, nullptr);
}

bool performItems(App& a, const std::vector<xdrop::Item>& items, DropTarget target, const DropPlace& place, const xdrop::Mods& mods) {
  for (const xdrop::Item& it : items)
    if (it.pending) {  // promised / downloading files: the drop happens once they are here
      xdragui::defer(a, items, target, place, mods);
      return true;
    }
  const xdrop::Plan p = planFor(a, items, target, place, mods);
  if (!p.unsupported.empty()) {
    std::string names;
    for (size_t k = 0; k < p.unsupported.size() && k < 3; ++k) names += (k ? ", " : "") + xdrop::displayName(items[p.unsupported[k]]);
    if (p.unsupported.size() > 3) names += ", …";
    a.notify(std::string(i18n::tr("Unsupported file type: ")) + names, true);
  }
  if (!p.accepted()) {
    if (p.unsupported.empty()) a.notify(p.label.empty() ? std::string(i18n::tr("Nothing here takes these files")) : p.label, true);
    return false;
  }
  if (p.steps.size() == 1 && p.steps[0].action == DropAction::OpenProject)
    return runAction(a, items[p.steps[0].item].path, DropAction::OpenProject, target, place, false, nullptr);
  int midi = 0;
  for (const xdrop::Step& s : p.steps) midi += s.action == DropAction::ImportMidiChannels;
  a.doc.beginGroup(p.steps.size() > 1 ? "drop files" : "drop file");
  bool any = false;
  std::set<ObjectId> clips;
  Tick prevEnd = -1;
  int inserted = 0;  // J-97: new channels of a rack drop keep the drop order
  for (const xdrop::Step& s : p.steps) {
    DropPlace pl = place;
    pl.track = s.track;
    pl.tick = s.tick;
    if (target == DropTarget::Rack && (s.action == DropAction::NewSamplerChannel || s.action == DropAction::NewChannelFromPreset)) {
      // Between rows: at the gap; extra files dropped on a row (or Shift): right below it.
      const int row = place.channel != kNoId ? a.doc.song().channelIndex(place.channel) : -1;
      if (place.insertAt >= 0) pl.insertAt = place.insertAt + inserted++;
      else if (row >= 0) pl.insertAt = row + 1 + inserted++;
    }
    // One after another: each clip starts where the one before really ends.
    if (mods.shift && prevEnd >= 0 && (s.action == DropAction::AudioClip || s.action == DropAction::MidiPatternClip)) pl.tick = prevEnd;
    ObjectId made = kNoId;
    any = runAction(a, items[s.item].path, s.action, target, pl, midi > 1, &made) || any;
    if (made != kNoId) {
      clips.insert(made);
      const Song& sg = a.doc.song();
      if (const Clip* c = sg.clip(made)) prevEnd = c->start + sg.clipLength(*c);
    }
  }
  a.doc.endGroup();
  if (!clips.empty()) a.playlist.selected = clips;
  if (any && items.size() > 1) a.notify(p.label.rfind(i18n::tr("Drop to "), 0) == 0 ? p.label.substr(std::strlen(i18n::tr("Drop to "))) : p.label);
  return any;
}

// ------------------------------------------------ plugins (GL_PLUGIN, J-97)

namespace {

enum class PlugAct { None, ReplaceChannel, NewChannel, InsertChannel, FreeSlot, Slot };

struct PlugPlan {
  PlugAct act = PlugAct::None;
  ObjectId owner = kNoId;  // channel (ReplaceChannel) / mixer track (FreeSlot, Slot)
  int slot = -1;
  std::string label, why;
};

std::string fmtS(const char* f, const std::string& x) {
  char buf[512];
  std::snprintf(buf, sizeof buf, f, x.c_str());
  return buf;
}
std::string fmtSS(const char* f, const std::string& x, const std::string& y) {
  char buf[512];
  std::snprintf(buf, sizeof buf, f, x.c_str(), y.c_str());
  return buf;
}

PlugPlan planPlugin(App& a, const std::string& type, DropTarget target, const DropPlace& place) {
  PlugPlan p;
  const PluginInfo* info = findPlugin(type);
  if (!info) {
    p.why = i18n::tr("This plugin is not available");
    return p;
  }
  const Song& s = a.doc.song();
  const bool gen = info->kind == PluginKind::Generator, fx = info->kind == PluginKind::Effect;
  const char* genOnly = i18n::tr("Effects go into mixer slots");
  const char* fxOnly = i18n::tr("Instruments go into the channel rack");
  if (info->kind == PluginKind::NoteFx) {
    p.why = i18n::tr("Note effects go into a channel's note effect slots");
    return p;
  }
  auto slotTarget = [&](ObjectId track, int k) {
    const MixerTrack* m = s.mixerTrack(track);
    if (!fx || !m || k < 0 || k >= kEffectSlots) {
      if (!fx) p.why = fxOnly;
      return;
    }
    p.act = PlugAct::Slot;
    p.owner = track;
    p.slot = k;
    const PluginInfo* cur = findPlugin(m->slots[static_cast<size_t>(k)].plugin.type);
    p.label = cur ? fmtSS(i18n::tr("Drop to replace %s with %s"), cur->name, info->name)
                  : fmtS(i18n::tr("Drop to insert %s into this slot"), info->name);
  };
  switch (target) {
    case DropTarget::Rack:
    case DropTarget::Playlist:
    case DropTarget::Workspace:
      if (!gen) {
        p.why = genOnly;
        return p;
      }
      if (target == DropTarget::Rack && place.channel != kNoId) {
        if (const Channel* c = s.channel(place.channel)) {
          p.act = PlugAct::ReplaceChannel;
          p.owner = c->id;
          p.label = fmtSS(i18n::tr("Drop to replace %s with %s"), c->name, info->name);
          return p;
        }
      }
      if (target == DropTarget::Rack && place.insertAt >= 0) {
        p.act = PlugAct::InsertChannel;
        p.label = fmtS(i18n::tr("Drop to insert a %s channel here"), info->name);
        return p;
      }
      p.act = PlugAct::NewChannel;
      p.label = fmtS(i18n::tr("Drop to add a %s channel"), info->name);
      return p;
    case DropTarget::MixerTrack: {
      const MixerTrack* m = s.mixerTrack(mixerOf(a, place));
      if (!fx || !m) {
        if (!fx) p.why = fxOnly;
        return p;
      }
      const int k = freeSlot(*m);
      if (k < 0) {
        p.why = i18n::tr("All effect slots are in use");
        return p;
      }
      p.act = PlugAct::FreeSlot;
      p.owner = m->id;
      p.slot = k;
      p.label = fmtS(i18n::tr("Drop to insert %s into the first free slot"), info->name);
      return p;
    }
    case DropTarget::MixerSlot:
      slotTarget(place.mixer, place.slot);
      return p;
    case DropTarget::PluginWindow:
      if (place.pluginSlot >= 0) {
        slotTarget(place.owner, place.pluginSlot);
      } else if (const Channel* c = s.channel(place.owner)) {
        if (!gen) {
          p.why = genOnly;
          return p;
        }
        p.act = PlugAct::ReplaceChannel;
        p.owner = c->id;
        p.label = fmtSS(i18n::tr("Drop to replace %s with %s"), c->name, info->name);
      }
      return p;
    case DropTarget::PianoRoll:
    case DropTarget::Browser:
      return p;
  }
  return p;
}

}  // namespace

std::string pluginDropLabel(App& a, const std::string& type, DropTarget target, const DropPlace& place, std::string* why) {
  const PlugPlan p = planPlugin(a, type, target, place);
  if (why) *why = p.why;
  return p.act == PlugAct::None ? std::string() : p.label;
}

bool performPluginDrop(App& a, const std::string& type, DropTarget target, const DropPlace& place) {
  const PlugPlan p = planPlugin(a, type, target, place);
  const PluginInfo* info = findPlugin(type);
  switch (p.act) {
    case PlugAct::None:
      if (!p.why.empty()) a.notify(p.why, true);
      return false;
    case PlugAct::ReplaceChannel:
      if (!replaceChannel(a, p.owner, type, nullptr, "", info ? info->name : type, "replace instrument")) return false;
      a.selectChannel(p.owner);
      return true;
    case PlugAct::NewChannel: {
      const ObjectId id = a.addInstrument(type);
      if (target == DropTarget::Playlist) a.settingsChannel = id;
      return id != kNoId;
    }
    case PlugAct::InsertChannel:
      return addChannelAt(a, type, "", place.insertAt) != kNoId;
    case PlugAct::FreeSlot:
    case PlugAct::Slot:
      if (!a.doc.setSlotPlugin(p.owner, p.slot, type)) return false;
      a.selMixer = p.owner;
      return true;
  }
  return false;
}

bool acceptPluginDrop(App& a, DropTarget target, const DropPlace& place, DropFeedback* fb) {
  const ImGuiPayload* cur = ImGui::GetDragDropPayload();
  if (!cur || !cur->IsDataType("GL_PLUGIN")) return false;
  const std::string type = payloadPath(cur);
  std::string why;
  const std::string label = pluginDropLabel(a, type, target, place, &why);
  if (label.empty()) {
    if (!why.empty()) {
      ImGui::SetTooltip("%s", why.c_str());
      a.setHint(why);
      ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
    }
    return false;
  }
  const ImRect rect = GImGui->DragDropTargetRect;
  const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PLUGIN", ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery);
  if (!pl) return false;
  if (!pl->Delivery) {
    if (!fb || fb->highlight) drawTargetHighlight(rect);
    if (fb) fb->hovering = true;
    ImGui::SetTooltip("%s", label.c_str());
    a.setHint(label);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return false;
  }
  return performPluginDrop(a, type, target, place);
}

void openFile(App& a, const std::string& path) {
  const Decoded& d = decodeItem(path);
  if (fs::is_directory(path)) {
    addBrowserFolder(a, path);
    return;
  }
  if (!d.ok) {
    a.notify(d.error.empty() ? "Cannot open " + fs::path(path).filename().string() : d.error, true);
    return;
  }
  // Presets go to the open plugin window when it shows the same plugin.
  if (d.item.kind == Kind::Preset) {
    DropPlace pw;
    if (a.fxEditor.first != kNoId && a.fxEditor.second >= 0) pw.owner = a.fxEditor.first, pw.pluginSlot = a.fxEditor.second;
    else if (a.settingsChannel != kNoId) pw.owner = a.settingsChannel, pw.pluginSlot = -1;
    if (pw.owner != kNoId && dropActionFor(a, path, DropTarget::PluginWindow, pw) == DropAction::ApplyPreset) {
      performDrop(a, path, DropTarget::PluginWindow, pw);
      return;
    }
    if (d.item.pluginKind == PluginKind::Effect) {
      performDrop(a, path, DropTarget::MixerTrack, DropPlace{});
      return;
    }
  }
  if (d.item.kind == Kind::Chain) {
    performDrop(a, path, DropTarget::MixerTrack, DropPlace{});
    return;
  }
  performDrop(a, path, DropTarget::Workspace, DropPlace{});
}

// Several files dropped where no window takes them: each the way a double
// click in the browser opens it (projects open, samples become channels, …).
void openFiles(App& a, const std::vector<xdrop::Item>& items) {
  std::vector<xdrop::Item> rest;
  for (const xdrop::Item& it : items) {
    if (it.pending) {
      xdragui::defer(a, items, DropTarget::Workspace, DropPlace{}, currentMods());
      return;
    }
    const browse::DropItem d = dropItemFor(it);
    if (d.kind == Kind::Project || d.kind == Kind::Flp) {
      openFile(a, it.path);
      return;
    }
    rest.push_back(it);
  }
  if (rest.size() == 1 && !rest[0].dir) {
    if (dropItemFor(rest[0]).kind == Kind::Other) a.notify(std::string(i18n::tr("Unsupported file type: ")) + rest[0].name, true);
    else openFile(a, rest[0].path);
    return;
  }
  performItems(a, rest, DropTarget::Workspace, DropPlace{}, currentMods());
}

// ----------------------------------------------------------------- preview

bool& previewSync() {
  static bool on = false;
  return on;
}

void previewFile(App& a, const std::string& path) {
  SamplePtr s = sampleCache().get(path);
  if (!s) {
    a.notify("Cannot decode " + fs::path(path).filename().string(), true);
    return;
  }
  // Remember what we learnt (files outside the indexed folders too).
  browse::FileMeta meta;
  if (!index().lookup(path, meta) || !meta.analysed) {
    meta = browse::Index::describe(path, false);
    meta.analysed = true;
    meta.seconds = s->seconds();
    meta.rate = s->sampleRate;
    meta.channels = s->left == s->right ? 1 : 2;
    meta.peaks = browse::encodePeaks(browse::computePeaks(s->left, s->right, 64));
    if (meta.bpm <= 0) meta.bpm = browse::wavAcidTempo(path);
    if (meta.bpm <= 0) {
      auto tags = browse::autoTags(path);
      if (std::find(tags.begin(), tags.end(), "loop") != tags.end()) meta.bpm = browse::bpmFromLength(meta.seconds);
    }
    index().put(meta);
  }
  const double ratio = previewSync() && meta.bpm > 0 ? browse::syncRatio(meta.bpm, a.doc.song().tempo) : 1.0;
  if (std::fabs(ratio - 1.0) < 1e-4) {
    a.engine.preview(s);
    return;
  }
  // Tempo sync: resample (speed follows the song tempo, like a synced loop).
  static std::string lastKey;
  static SamplePtr last;
  const std::string key = path + "@" + std::to_string(ratio);
  if (key != lastKey || !last) {
    auto r = std::make_shared<Sample>();
    r->path = s->path;
    r->sampleRate = s->sampleRate;
    const size_t n = static_cast<size_t>(static_cast<double>(s->frames()) / ratio);
    r->left.resize(n);
    r->right.resize(n);
    for (size_t i = 0; i < n; ++i) {
      const double x = static_cast<double>(i) * ratio;
      const size_t i0 = std::min(static_cast<size_t>(x), s->frames() - 1), i1 = std::min(i0 + 1, s->frames() - 1);
      const float f = static_cast<float>(x - static_cast<double>(i0));
      r->left[i] = s->left[i0] + (s->left[i1] - s->left[i0]) * f;
      r->right[i] = s->right[i0] + (s->right[i1] - s->right[i0]) * f;
    }
    last = r;
    lastKey = key;
  }
  a.engine.preview(last);
}

}  // namespace gl::browserui
