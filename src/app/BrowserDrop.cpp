// Gridline — browser content in the app: content index, drag & drop of
// browser files, double-click actions and tempo-synced preview (BrowserUi.h).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Json.h"
#include "../core/MidiFile.h"
#include "../core/Presets.h"
#include "../core/flp/Flp.h"
#include "../core/flp/FlpPresets.h"
#include "App.h"
#include "BrowserUi.h"
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
  for (const char* d : {"Recorded", "Rendered", "Sliced audio", "Clipboard files"}) roots.push_back({contentDir(a, d), true});
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
  if (target == DropTarget::MixerSlot)
    if (const MixerTrack* m = s.mixerTrack(place.mixer); m && place.slot >= 0 && place.slot < kEffectSlots)
      return findPlugin(m->slots[static_cast<size_t>(place.slot)].plugin.type);
  return nullptr;
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

}  // namespace

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
  return browse::dropAction(d.item, target, ti ? ti->type : "", ti && ti->usesSample);
}

bool acceptFileDrop(App& a, DropTarget target, const DropPlace& place) {
  bool done = false;
  for (const char* type : {"GL_FILE", "GL_ITEM"}) {
    const ImGuiPayload* peek = ImGui::AcceptDragDropPayload(type, ImGuiDragDropFlags_AcceptPeekOnly | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if (!peek) continue;
    const std::string path = payloadPath(peek);
    const DropAction act = dropActionFor(a, path, target, place);
    if (act == DropAction::None) {
      const Decoded& d = decodeItem(path);
      if (!d.ok && !d.error.empty()) ImGui::SetTooltip("%s", d.error.c_str());
      continue;
    }
    ImGui::SetTooltip("%s", browse::dropActionLabel(act));
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(type)) {
      (void)pl;
      done = performDrop(a, path, target, place);
    }
  }
  return done;
}

void pluginWindowDropTarget(App& a, const PluginTarget& t) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (!w || !ImGui::GetDragDropPayload()) return;
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl->IsDataType("GL_FILE") && !pl->IsDataType("GL_ITEM")) return;
  if (ImGui::BeginDragDropTargetCustom(w->Rect(), w->ID)) {
    DropPlace place;
    place.owner = t.owner;
    place.pluginSlot = t.slot;
    acceptFileDrop(a, DropTarget::PluginWindow, place);
    ImGui::EndDragDropTarget();
  }
}

bool performDrop(App& a, const std::string& path, DropTarget target, const DropPlace& place) {
  const Decoded& d = decodeItem(path);
  if (!d.ok) {
    a.notify(d.error.empty() ? "Cannot use " + fs::path(path).filename().string() : d.error, true);
    return false;
  }
  const PluginInfo* ti = targetInfo(a, target, place);
  const DropAction act = browse::dropAction(d.item, target, ti ? ti->type : "", ti && ti->usesSample);
  const std::string fname = fs::path(path).filename().string();
  switch (act) {
    case DropAction::None:
      return false;
    case DropAction::NewSamplerChannel:
      a.settingsChannel = a.addInstrument("gl.sampler", path);
      return a.settingsChannel != kNoId;
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
        sg.playlist.push_back(c);
        a.playlist.selected = {c.id};
      });
      a.doc.endGroup();
      return true;
    }
    case DropAction::LoadSample: {
      PluginTarget t{place.owner, place.pluginSlot};
      if (const PluginState* st = t.state(a.doc.song())) {
        PluginState ns = *st;
        ns.data = path;
        setPluginState(a, t, ns, "load sample");
        if (t.isChannel()) a.doc.renameChannel(t.owner, fs::path(path).stem().string());
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
      // Impulse source: the "File" choice (the default is a generated space).
      if (const PluginInfo* ci = findPlugin("gl.convolver"))
        for (size_t i = 0; i < ci->params.size() && i < ir.params.size(); ++i)
          if (ci->params[i].kind == ParamKind::Choice)
            for (size_t c = 0; c < ci->params[i].choices.size(); ++c)
              if (ci->params[i].choices[c] == "File") ir.params[i] = static_cast<float>(c);
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
      ObjectId id = a.addInstrument(d.item.pluginType);
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
      a.importMidi(path);
      return true;
    case DropAction::ImportMidiNotes: {
      MidiFileData m;
      std::string err;
      if (!readMidiFile(path, m, &err)) {
        a.notify("MIDI import failed: " + err, true);
        return false;
      }
      const Song& s = a.doc.song();
      ObjectId pat = a.curPattern, ch = a.selChannel;
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
    case DropAction::ImportFlp:
      a.guardUnsaved([&a, path]() { a.importFlpProject(path); });
      return true;
  }
  return false;
}

void openFile(App& a, const std::string& path) {
  const Decoded& d = decodeItem(path);
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
