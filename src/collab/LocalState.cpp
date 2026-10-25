// Gridline - J-111 collaboration: session-only state. See LocalState.h.
#include "LocalState.h"
#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include "../core/PluginInfo.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../core/Update.h"
#include "../engine/AudioEngine.h"
#include "Crypto.h"

namespace gl::collab {

namespace {

// The performance gestures, listening aids and display settings of Gridline's
// own plugins (reviewed one by one, J-111). Everything else is a sound
// setting and is shared.
struct Rule {
  const char* type;
  const char* name;  // exact name; "* solo" = any name ending in " solo"
};
const Rule kRules[] = {
    {"gl.pads", "* solo"},              // Abacus: pad solo
    {"gl.membranekit", "* solo"},       // Membrane Kit: channel solo
    {"gl.splitter", "* solo"},          // Band Splitter: band solo
    {"gl.eq", "Listen"},                // Parametric EQ: band listen
    {"gl.eq", "Band solo on Shift"},    // ... and how Shift behaves (a preference)
    {"gl.maximizer", "Solo band"},
    {"gl.master", "Solo"},              // Crest: band solo
    {"gl.master", "Solo (ref.)"},       // ... the reference track
    {"gl.enhance", "Program: Solo"},
    {"gl.enhance", "Solo (ref.)"},
    {"gl.octafilter", "Solo unit"},
    {"gl.emphasis", "Delta"},           // Loudness: hear what it adds
    {"gl.tideline", "Band listen"},     // Tideline: listen to what is ducked
    {"gl.scduck", "Listen to key"},     // Sidechain Ducker: hear the key
    {"gl.vocoder", "Listen"},           // Cipher: listen to one signal
    {"gl.vocoder", "Hold"},             // ... hold (freeze) the carrier: a gesture
    {"gl.timegate", "Hold"},            // Shuttle: the free-run hold
    {"gl.hitloop", "Hold loops"},       // Hit Repeater: hold the loop
    {"gl.scratcher", "Hold platter"},   // Turntable: hand on the platter
    {"gl.arp", "Latch"},                // Arpeggiator: latch (held keys)
    {"gl.edison", "Loop playback"},     // Loupe: its own playback loops
    {"gl.analyzer", "Peak hold"},       // displays
    {"gl.analyzer", "Freeze"},
    {"gl.scope", "Freeze"},
    {"gl.scope", "Infinite hold"},
    {"gl.scope", "Display engine"},
    {"gl.tuner", "Display"},
    {"gl.clock", "Display"},
};

bool ruleMatches(const Rule& r, const std::string& name) {
  const std::string n = r.name;
  if (n.rfind("* ", 0) == 0) {
    const std::string suffix = n.substr(1);
    return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
  }
  return name == n;
}

const std::map<std::string, std::vector<bool>>& table() {
  static std::map<std::string, std::vector<bool>> t;
  static std::once_flag once;
  std::call_once(once, [] {
    for (const auto& p : pluginCatalog()) {
      std::vector<bool> flags(p.params.size(), false);
      bool any = false;
      for (size_t i = 0; i < p.params.size(); ++i)
        for (const auto& r : kRules)
          if (p.type == r.type && ruleMatches(r, p.params[i].name)) {
            flags[i] = true;
            any = true;
          }
      if (any) t[p.type] = std::move(flags);
    }
  });
  return t;
}

void maskMixerJson(Json& m) {
  if (!m.is(Json::Type::Object)) return;
  if (m.find("solo")) m.object["solo"] = [] {
      Json b;
      b.type = Json::Type::Bool;
      b.boolean = false;
      return b;
    }();
  for (const char* k : {"soloAlone", "armed", "monitor", "input", "inputStereo", "recPoint"}) m.object.erase(k);
}

void maskClipJson(Json& c) {
  if (c.is(Json::Type::Object)) c.object.erase("launched");
}

void maskTrackJson(Json& t) {
  if (!t.is(Json::Type::Object)) return;
  t.object.erase("folded");
  t.object.erase("height");
}

void eachIn(Json& j, const char* key, const std::function<void(Json&)>& fn) {
  if (Json* a = j.is(Json::Type::Object) && j.find(key) ? &j.object[key] : nullptr; a && a->is(Json::Type::Array))
    for (auto& e : a->array) fn(e);
}

void maskPlugins(Json& j, int depth) {
  if (depth > 40) return;
  if (j.is(Json::Type::Object)) {
    const Json* t = j.find("type");
    Json* params = j.find("params") ? &j.object["params"] : nullptr;
    if (t && t->is(Json::Type::String) && params && params->is(Json::Type::Array)) {
      const auto& tab = table();
      auto it = tab.find(t->str);
      if (it != tab.end()) {
        const PluginInfo* pi = findPlugin(t->str);
        for (size_t i = 0; i < params->array.size() && i < it->second.size(); ++i)
          if (it->second[i]) params->array[i] = Json::makeNumber(pi && i < pi->params.size() ? pi->params[i].def : 0.0);
      }
    }
    for (auto& [k, v] : j.object) maskPlugins(v, depth + 1);
  } else if (j.is(Json::Type::Array)) {
    for (auto& v : j.array) maskPlugins(v, depth + 1);
  }
}

void keepPluginSession(PluginState& in, const PluginState& cur) {
  if (in.type != cur.type) return;
  const auto& tab = table();
  auto it = tab.find(in.type);
  if (it == tab.end()) return;
  for (size_t i = 0; i < it->second.size() && i < in.params.size() && i < cur.params.size(); ++i)
    if (it->second[i]) in.params[i] = cur.params[i];
}

}  // namespace

bool isSessionParam(const std::string& type, size_t index) {
  const auto& tab = table();
  auto it = tab.find(type);
  return it != tab.end() && index < it->second.size() && it->second[index];
}

std::vector<std::string> sessionParamList() {
  std::vector<std::string> out;
  for (const auto& p : pluginCatalog())
    for (size_t i = 0; i < p.params.size(); ++i)
      if (isSessionParam(p.type, i)) out.push_back(p.type + ": " + p.params[i].name);
  return out;
}

void maskPluginJson(Json& anything) { maskPlugins(anything, 0); }

void maskElementJson(const std::string& list, Json& e) {
  if (list == "mixer") maskMixerJson(e);
  if (list == "playlist") maskClipJson(e);
  maskPlugins(e, 0);
}

void maskSessionJson(Json& song) {
  eachIn(song, "mixer", maskMixerJson);
  eachIn(song, "playlist", maskClipJson);
  eachIn(song, "tracks", maskTrackJson);
  eachIn(song, "arrangements", [](Json& a) {
    eachIn(a, "playlist", maskClipJson);
    eachIn(a, "tracks", maskTrackJson);
  });
  maskPlugins(song, 0);
}

void keepChannelSession(Channel& in, const Channel& cur) {
  keepPluginSession(in.plugin, cur.plugin);
  for (size_t i = 0; i < in.noteFx.size() && i < cur.noteFx.size(); ++i) keepPluginSession(in.noteFx[i], cur.noteFx[i]);
}

void keepMixerSession(MixerTrack& in, const MixerTrack& cur) {
  in.solo = cur.solo;
  in.soloAlone = cur.soloAlone;
  in.armed = cur.armed;
  in.monitor = cur.monitor;
  in.input = cur.input;
  in.inputStereo = cur.inputStereo;
  in.recPoint = cur.recPoint;
  for (size_t k = 0; k < in.slots.size() && k < cur.slots.size(); ++k) keepPluginSession(in.slots[k].plugin, cur.slots[k].plugin);
}

void keepClipSession(Clip& in, const Clip& cur) { in.launched = cur.launched; }

void keepSessionState(Song& in, const Song& cur) {
  for (auto& c : in.channels)
    if (const Channel* o = cur.channel(c.id)) keepChannelSession(c, *o);
  for (auto& m : in.mixer)
    if (const MixerTrack* o = cur.mixerTrack(m.id)) keepMixerSession(m, *o);
  for (auto& c : in.playlist)
    if (const Clip* o = cur.clip(c.id)) keepClipSession(c, *o);
  for (size_t i = 0; i < in.tracks.size() && i < cur.tracks.size(); ++i) {
    in.tracks[i].folded = cur.tracks[i].folded;
    in.tracks[i].height = cur.tracks[i].height;
  }
}

OwnPlaySig ownPlaySignature(const AudioEngine& e, const Song& s) {
  OwnPlaySig sig;
  const AudioEngine::OwnPlayback o = e.pluginPlayback();
  sig.kind = static_cast<int>(o.kind);
  sig.id = o.id;
  sig.slot = o.slot;
  if (o.kind == AudioEngine::OwnPlay::None) return sig;
  const PluginState* st = nullptr;
  if (o.kind == AudioEngine::OwnPlay::Effect) {
    if (const MixerTrack* m = s.mixerTrack(o.id); m && o.slot >= 0 && o.slot < static_cast<int>(m->slots.size()))
      st = &m->slots[static_cast<size_t>(o.slot)].plugin;
  } else if (const Channel* c = s.channel(o.id)) {
    st = &c->plugin;
  }
  if (st) {
    sig.type = st->type;
    const crypto::Key h = crypto::hash(st->data);
    for (int i = 0; i < 8; ++i) sig.dataHash = (sig.dataHash << 8) | h[static_cast<size_t>(i)];
  } else {
    sig.type = "\x01gone";
  }
  return sig;
}

bool stopOwnPlaybackIfChanged(AudioEngine& e, const Song& s, const OwnPlaySig& before) {
  if (before.kind == 0) return false;
  // the same playback still running: compare what it plays from
  const AudioEngine::OwnPlayback o = e.pluginPlayback();
  if (o.kind == AudioEngine::OwnPlay::None) return false;
  if (static_cast<int>(o.kind) != before.kind || o.id != before.id || o.slot != before.slot) return false;
  const OwnPlaySig now = ownPlaySignature(e, s);
  if (now.type == before.type && now.dataHash == before.dataHash) return false;
  e.stopPluginPlayback();  // normal release; the editors' Play buttons go off (stop epoch)
  return true;
}

std::string sharedSongDigest(const Song& in) {
  Song s = in;
  s.nextId = 0;
  // project-relative file names: every folder refers to Audio/<sha256>.<ext>
  forEachFileRef(s, [](std::string& p) {
    const size_t a = p.find("/Audio/");
    if (a != std::string::npos) p = p.substr(a + 1);
  });
  Json j = songToJson(s);
  maskSessionJson(j);  // solo, arm, launch ... are each computer's own (J-111)
  return update::sha256Hex(jsonCompact(j));
}

}  // namespace gl::collab
