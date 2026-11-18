// Gridline — K-89: DAWproject import and export. See DawProject.h. Written from the format's
// published schema; the mapping is Gridline's own.
#include "DawProject.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include "Json.h"
#include "NoteBend.h"
#include "Params.h"
#include "PluginInfo.h"
#include "Project.h"
#include "StepNotes.h"
#include "Zip.h"

namespace gl::dawproject {

namespace fs = std::filesystem;

std::string Report::text() const {
  std::string s;
  for (const auto& l : lines) s += l + "\n";
  return s;
}

namespace {

// Counts of each kind of loss, reported as one line each.
struct Losses {
  std::map<std::string, int> n;
  std::vector<std::string> order;
  void add(const std::string& what, int k = 1) {
    if (k <= 0) return;
    if (!n.count(what)) order.push_back(what);
    n[what] += k;
  }
  void into(Report* r) const {
    if (!r) return;
    for (const auto& w : order) r->lines.push_back(std::to_string(n.at(w)) + " " + w);
  }
};

std::string colorText(Rgb c) {
  char b[8];
  std::snprintf(b, sizeof b, "#%06x", static_cast<unsigned>(c & 0xFFFFFF));
  return b;
}
Rgb colorFrom(const std::string& s, Rgb dflt) {
  if (s.size() < 7 || s[0] != '#') return dflt;
  return static_cast<Rgb>(std::strtoul(s.c_str() + 1, nullptr, 16) & 0xFFFFFF);
}

std::string safeName(std::string s) {
  for (char& c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) c = '_';
  return s.empty() ? std::string("file") : s;
}

// ------------------------------------------------------------------ export

struct Exporter {
  const Song& s;
  Losses loss;
  Parts parts;
  int nextId = 1;
  std::map<ObjectId, std::string> chanOfMixer;              // mixer track -> its Channel id
  std::map<ObjectId, std::string> trackOfChannel;           // rack channel -> its Track id
  std::map<std::pair<ObjectId, uint32_t>, std::string> paramIds;  // (owner, index) -> Volume / Pan id
  std::map<std::string, std::string> audioNames;            // source path -> archive name

  explicit Exporter(const Song& song) : s(song) {}
  std::string id() { return "id" + std::to_string(nextId++); }
  double beats(Tick t) const { return static_cast<double>(t) / s.ppq; }

  void param(xml::Node& ch, const char* el, ObjectId owner, uint32_t index, double value, double mn, double mx, const char* unit) {
    xml::Node& p = ch.add(el);
    p.set("id", paramIds[{owner, index}] = id());
    p.set("max", mx);
    p.set("min", mn);
    p.set("unit", std::string(unit));
    p.set("value", value);
  }

  void device(xml::Node& devs, const PluginState& st, const char* role, bool enabled, const std::string& key) {
    if (st.type.empty()) return;
    const PluginInfo* info = findPlugin(st.type);
    const std::string name = info ? info->name : st.type;
    const char* el = "BuiltinDevice";
    std::string devId = st.type;
    if (st.type.rfind("vst3:", 0) == 0) el = "Vst3Plugin", devId = st.type.substr(5);
    else if (st.type.rfind("clap:", 0) == 0) el = "ClapPlugin", devId = st.type.substr(5);
    else if (st.type.rfind("vst2:", 0) == 0) el = "Vst2Plugin", devId = st.type.substr(5);
    else if (st.type.rfind("au:", 0) == 0) el = "AuPlugin", devId = st.type.substr(3);
    else if (st.type.rfind("lv2:", 0) == 0) loss.add("LV2 plugins kept as Gridline devices (the format has no LV2 entry)");
    xml::Node& d = devs.add(el);
    d.set("deviceID", devId);
    d.set("deviceName", name);
    d.set("deviceRole", std::string(role));
    if (!isHostedType(st.type)) d.set("deviceVendor", std::string("Gridline"));
    d.setFlag("loaded", true);
    d.set("id", id());
    d.set("name", name);
    xml::Node& en = d.add("Enabled");
    en.set("id", id());
    en.setFlag("value", enabled);
    // The state in Gridline's own form: a Gridline import restores it exactly; other apps load
    // the plugin with its default sound.
    const std::string file = "plugins/" + safeName(key) + ".gridline-state.json";
    parts.blobs.push_back({file, jsonStringify(pluginStateToJson(st), 0)});
    d.add("State").set("path", file);
  }

  void sends(xml::Node& ch, const MixerTrack& m) {
    bool first = true;
    xml::Node* sendsEl = nullptr;
    for (const Send& sd : m.sends) {
      if (sd.sidechain || sd.stageRoom) {
        loss.add(sd.sidechain ? "sidechain routes left out" : "Stage room sends left out");
        continue;
      }
      auto dest = chanOfMixer.find(sd.to);
      if (dest == chanOfMixer.end()) continue;
      if (first && std::fabs(sd.level - 1.0f) < 1e-6f && !sd.preFader) {
        ch.set("destination", dest->second);
        first = false;
        continue;
      }
      if (!sendsEl) sendsEl = &ch.add("Sends");
      xml::Node& e = sendsEl->add("Send");
      e.set("destination", dest->second);
      e.set("type", std::string(sd.preFader ? "pre" : "post"));
      e.set("id", id());
      xml::Node& v = e.add("Volume");
      v.set("id", id());
      v.set("max", 2.0);
      v.set("min", 0.0);
      v.set("unit", std::string("linear"));
      v.set("value", static_cast<double>(sd.level));
    }
  }

  // The mixer tracks that matter: those a channel plays into, what they send to, and any with
  // effects or a name of their own.
  std::vector<const MixerTrack*> usedMixer() {
    std::set<ObjectId> used;
    std::function<void(ObjectId)> reach = [&](ObjectId t) {
      if (!used.insert(t).second) return;
      if (const MixerTrack* m = s.mixerTrack(t))
        for (const auto& sd : m->sends) reach(sd.to);
    };
    for (const auto& c : s.channels) reach(c.mixerTrack == kNoId ? s.masterId() : c.mixerTrack);
    for (size_t i = 1; i < s.mixer.size(); ++i) {
      const MixerTrack& m = s.mixer[i];
      bool fx = !m.name.empty();
      for (const auto& sl : m.slots) fx = fx || !sl.plugin.type.empty();
      if (fx) reach(m.id);
    }
    std::vector<const MixerTrack*> out;
    for (size_t i = 1; i < s.mixer.size(); ++i)
      if (used.count(s.mixer[i].id)) out.push_back(&s.mixer[i]);
    return out;
  }

  void mixerChannel(xml::Node& ch, const MixerTrack& m) {
    xml::Node& devs = ch.add("Devices");
    int k = 0;
    for (const auto& sl : m.slots) {
      device(devs, sl.plugin, "audioFX", sl.enabled, "mixer-" + std::to_string(m.id) + "-" + std::to_string(k++));
      if (!sl.plugin.type.empty() && sl.mix < 0.999f) loss.add("effect mix settings left at full (the format has no per-device mix)");
    }
    if (devs.children.empty()) ch.children.pop_back();
    xml::Node& mute = ch.add("Mute");
    mute.set("id", id());
    mute.setFlag("value", m.muted);
    param(ch, "Pan", m.id, static_cast<uint32_t>(MixerParam::Pan), (m.pan + 1.0) / 2.0, 0, 1, "normalized");
    param(ch, "Volume", m.id, static_cast<uint32_t>(MixerParam::Volume), m.volume, 0, 2, "linear");
    if (m.stereoSep != 0 || m.depth != 0 || m.focus != 0 || m.invertPolarity || m.swapLR) loss.add("mixer tracks' stereo, Stage and polarity settings left out");
  }

  void noteContent(xml::Node& notes, const std::vector<Note>& list, Tick from, Tick to) {
    notes.set("id", id());
    for (const Note& n : list) {
      if (n.start < from || n.start >= to) continue;
      if (n.muted) {
        loss.add("muted notes left out");
        continue;
      }
      xml::Node& x = notes.add("Note");
      x.set("time", beats(n.start - from));
      x.set("duration", beats(std::max<Tick>(1, n.length)));
      x.set("channel", 0.0);
      x.set("key", static_cast<double>(n.pitch));
      x.set("vel", n.velocity / 127.0);
      x.set("rel", n.release / 127.0);
      if (n.slide || n.porta) loss.add("slide and portamento notes became plain notes");
      if (n.pan != 0 || n.fine != 0 || n.modX != kNoteModCentre || n.modY != kNoteModCentre) loss.add("notes' pan, fine pitch and mod X / Y left out");
      auto lane = [&](const std::vector<BendPoint>& pts, const char* expr, double offset) {
        if (pts.empty()) return;
        xml::Node& p = x.add("Points");
        p.set("id", id());
        p.set("unit", std::string(std::string(expr) == "transpose" ? "semitones" : "normalized"));
        xml::Node& t = p.add("Target");
        t.set("expression", std::string(expr));
        for (const BendPoint& b : pts) {
          xml::Node& r = p.add("RealPoint");
          r.set("time", b.tick / s.ppq);
          r.set("value", static_cast<double>(b.semis) + offset);
          r.set("interpolation", std::string("linear"));
        }
      };
      lane(n.bend, "transpose", 0);
      lane(n.pressure, "pressure", 0);
      lane(n.timbre, "timbre", 0.5);
    }
  }

  void automation(xml::Node& lanes, const Clip& c, const AutomationClip& a, Losses& lost) {
    auto key = paramIds.find({a.owner, a.index});
    const bool tempo = a.owner == kSongId && a.index == static_cast<uint32_t>(SongParam::Tempo);
    if (key == paramIds.end() && !tempo) {
      lost.add("automation clips of other parameters left out (only volume, pan and tempo map)");
      return;
    }
    const auto desc = describeParam(s, {a.owner, a.index});
    if (!desc) return;
    xml::Node* pts = nullptr;
    if (tempo) {
      pts = parts.project.child("Arrangement")->child("TempoAutomation");
      if (!pts) {
        pts = &parts.project.child("Arrangement")->add("TempoAutomation");
        pts->set("id", id());
        pts->set("unit", std::string("bpm"));
        xml::Node& t = pts->add("Target");
        t.set("parameter", parts.project.child("Transport")->child("Tempo")->get("id"));
      }
    } else {
      pts = &lanes.add("Points");
      pts->set("id", id());
      const bool pan = (a.owner != kSongId) && ((s.channel(a.owner) && a.index == static_cast<uint32_t>(ChannelParam::Pan)) ||
                                                 (s.mixerTrack(a.owner) && a.index == static_cast<uint32_t>(MixerParam::Pan)));
      pts->set("unit", std::string(pan ? "normalized" : "linear"));
      pts->add("Target").set("parameter", key->second);
    }
    const bool pan = pts->get("unit") == "normalized";
    for (const AutoPoint& p : a.points) {
      const Tick at = c.start + p.pos - c.offset;
      if (p.pos < c.offset || at > c.start + c.length) continue;
      const double norm = a.min + p.value * (a.max - a.min);
      double v = desc->fromNormalized(norm);
      if (pan) v = (v + 1.0) / 2.0;
      xml::Node& r = pts->add("RealPoint");
      r.set("time", beats(at));
      r.set("value", v);
      r.set("interpolation", std::string(p.shape == AutoShape::Hold ? "hold" : "linear"));
      if (p.shape != AutoShape::Hold && p.tension != 0) lost.add("automation curve tensions became straight lines");
    }
    if (c.loop) lost.add("looping automation clips played once");
  }

  void run() {
    xml::Node& pr = parts.project;
    pr.children.reserve(8);
    pr.name = "Project";
    pr.set("version", std::string("1.0"));
    xml::Node& app = pr.add("Application");
    app.set("name", std::string("Gridline"));
    app.set("version", std::string("1"));
    xml::Node& tr = pr.add("Transport");
    xml::Node& tempo = tr.add("Tempo");
    tempo.set("id", id());
    tempo.set("max", 999.0);
    tempo.set("min", 20.0);
    tempo.set("unit", std::string("bpm"));
    tempo.set("value", s.tempo);
    xml::Node& sig = tr.add("TimeSignature");
    sig.set("id", id());
    sig.set("denominator", static_cast<double>(s.timeSigDen));
    sig.set("numerator", static_cast<double>(s.timeSigNum));
    if (s.swing > 0) loss.add("song swing left out");

    xml::Node& st = pr.add("Structure");
    // Ids first, so routing can point forward.
    const MixerTrack* master = s.mixer.empty() ? nullptr : &s.mixer[0];
    const auto used = usedMixer();
    if (master) chanOfMixer[master->id] = id();
    for (const MixerTrack* m : used) chanOfMixer[m->id] = id();
    // Mixer inserts: a track each, with its effects.
    int ins = 0;
    for (const MixerTrack* m : used) {
      ++ins;
      xml::Node& t = st.add("Track");
      t.set("contentType", std::string("audio"));
      t.setFlag("loaded", true);
      t.set("id", id());
      t.set("name", m->name.empty() ? "Insert " + std::to_string(ins) : m->name);
      t.set("color", colorText(m->color));
      xml::Node& ch = t.add("Channel");
      ch.set("audioChannels", 2.0);
      bool isReturn = false;
      for (const auto& other : s.mixer)
        for (const auto& sd : other.sends)
          if (sd.to == m->id && std::fabs(sd.level - 1.0f) > 1e-6f) isReturn = true;
      ch.set("role", std::string(isReturn ? "effect" : "regular"));
      ch.setFlag("solo", m->solo);
      ch.set("id", chanOfMixer[m->id]);
      mixerChannel(ch, *m);
      sends(ch, *m);
    }
    // Rack channels: an instrument track each.
    for (const Channel& c : s.channels) {
      xml::Node& t = st.add("Track");
      t.set("contentType", std::string("notes"));
      t.setFlag("loaded", true);
      t.set("id", trackOfChannel[c.id] = id());
      t.set("name", c.name);
      t.set("color", colorText(c.color));
      xml::Node& ch = t.add("Channel");
      ch.set("audioChannels", 2.0);
      ch.set("role", std::string("regular"));
      ch.setFlag("solo", false);
      ch.set("id", id());
      const ObjectId dest = c.mixerTrack == kNoId ? s.masterId() : c.mixerTrack;
      if (chanOfMixer.count(dest)) ch.set("destination", chanOfMixer[dest]);
      xml::Node& devs = ch.add("Devices");
      device(devs, c.plugin, "instrument", true, "channel-" + std::to_string(c.id));
      int k = 0;
      for (const auto& fx : c.noteFx) device(devs, fx, "noteFX", true, "channel-" + std::to_string(c.id) + "-note-" + std::to_string(k++));
      if (devs.children.empty()) ch.children.pop_back();
      xml::Node& mute = ch.add("Mute");
      mute.set("id", id());
      mute.setFlag("value", c.muted);
      param(ch, "Pan", c.id, static_cast<uint32_t>(ChannelParam::Pan), (c.pan + 1.0) / 2.0, 0, 1, "normalized");
      param(ch, "Volume", c.id, static_cast<uint32_t>(ChannelParam::Volume), c.gain, 0, 2, "linear");
      if (c.pitchCents != 0 || c.fine != 0) loss.add("channels' pitch and fine tuning left out");
      if (!c.auxOuts.empty()) loss.add("channels' extra outputs left out");
    }
    if (master) {
      xml::Node& ch = st.add("Channel");
      ch.set("audioChannels", 2.0);
      ch.set("role", std::string("master"));
      ch.setFlag("solo", false);
      ch.set("id", chanOfMixer[master->id]);
      ch.set("name", std::string("Master"));
      mixerChannel(ch, *master);
    }

    // The arrangement.
    xml::Node& arr = pr.add("Arrangement");
    arr.children.reserve(8);  // the lanes below stay where they are while tempo and markers join
    arr.set("id", id());
    xml::Node& root = arr.add("Lanes");
    root.set("timeUnit", std::string("beats"));
    root.set("id", id());
    std::map<ObjectId, xml::Node*> laneOf;  // rack channel -> its Clips
    auto clipsFor = [&](ObjectId ch) -> xml::Node& {
      auto it = laneOf.find(ch);
      if (it != laneOf.end()) return *it->second;
      xml::Node& l = root.add("Lanes");
      l.set("track", trackOfChannel[ch]);
      l.set("id", id());
      xml::Node& clips = l.add("Clips");
      clips.set("id", id());
      // (re-find after the push: the node vector may have moved)
      laneOf.clear();
      for (auto& lane : root.children)
        if (lane.name == "Lanes")
          for (const auto& [cid, tid] : trackOfChannel)
            if (lane.get("track") == tid) laneOf[cid] = lane.child("Clips");
      return *laneOf[ch];
    };
    // Gather what each channel's lane holds first, then write it (stable node addresses).
    std::map<ObjectId, std::vector<const Clip*>> patternClips, audioClips;
    std::vector<const Clip*> autoClips;
    int live = 0;
    for (const Clip& c : s.playlist) {
      if (c.start < s.live.areaEnd) {
        ++live;
        continue;
      }
      if (c.kind == ClipKind::Pattern) {
        const Pattern* p = s.pattern(c.pattern);
        if (!p) continue;
        for (const Channel& ch : s.channels)
          if (!stepnotes::view(s, *p, ch.id).empty()) patternClips[ch.id].push_back(&c);
        if (!p->automation.empty()) loss.add("automation inside patterns left out");
      } else if (c.kind == ClipKind::Audio) {
        audioClips[c.source].push_back(&c);
      } else {
        autoClips.push_back(&c);
      }
    }
    loss.add("clips of the live area left out", live);
    for (const Channel& ch : s.channels) {
      if (!patternClips.count(ch.id) && !audioClips.count(ch.id)) continue;
      clipsFor(ch.id);
    }
    for (const Channel& ch : s.channels) {
      if (!laneOf.count(ch.id)) continue;
      xml::Node& clips = *laneOf[ch.id];
      for (const Clip* c : patternClips[ch.id]) {
        const Pattern* p = s.pattern(c->pattern);
        const Tick plen = std::max<Tick>(1, s.patternContentLength(*p));
        const Tick len = c->length > 0 ? c->length : plen;
        xml::Node& x = clips.add("Clip");
        x.set("time", beats(c->start));
        x.set("duration", beats(len));
        x.set("playStart", beats(c->offset));
        const bool loops = c->loop || (p->loop && len > plen - c->offset);
        if (loops) {
          const Tick ls = c->loopLength > 0 ? c->loopStart : 0, ll = c->loopLength > 0 ? c->loopLength : plen;
          x.set("loopStart", beats(ls));
          x.set("loopEnd", beats(ls + ll));
        }
        x.setFlag("enable", !c->muted);
        x.set("name", p->name);
        x.set("color", colorText(p->color));
        const std::vector<Note> notes = stepnotes::view(s, *p, ch.id);
        if (stepnotes::hasSteps(s, *p, ch.id)) loss.add("channel rack steps written as notes");
        noteContent(x.add("Notes"), notes, 0, std::max(plen, notes.empty() ? 0 : notes.back().start + 1));
      }
      for (const Clip* c : audioClips[ch.id]) {
        const std::string& src = ch.plugin.data;
        if (src.empty()) continue;
        std::string& name = audioNames[src];
        if (name.empty()) {
          name = "audio/" + std::to_string(audioNames.size()) + "_" + safeName(fs::path(src).filename().string());
          parts.files.push_back({name, src});
        }
        xml::Node& x = clips.add("Clip");
        x.set("time", beats(c->start));
        x.set("duration", beats(c->length));
        x.set("contentTimeUnit", std::string("seconds"));
        x.set("playStart", c->audioStart);
        if (c->fadeIn > 0 || c->fadeOut > 0) {
          x.set("fadeTimeUnit", std::string("seconds"));
          x.set("fadeInTime", c->fadeIn);
          x.set("fadeOutTime", c->fadeOut);
        }
        x.setFlag("enable", !c->muted);
        x.set("name", fs::path(src).stem().string());
        xml::Node& au = x.add("Audio");
        au.set("algorithm", std::string("raw"));
        au.set("channels", 2.0);
        au.set("duration", c->audioEnd > 0 ? c->audioEnd : 0.0);
        au.set("sampleRate", 44100.0);
        au.set("id", id());
        au.add("File").set("path", name);
        if (c->stretch != 1.0 || c->pitch != 0 || c->reverse || !c->warp.empty() || c->syncBpm > 0)
          loss.add("audio clips' stretch, pitch, reverse and warp markers left out");
        if (c->gain != 1.0f) loss.add("audio clip gains left out");
      }
    }
    // Automation: points in the lane of the track it belongs to (its own lane when it has none).
    Losses lost;
    for (const Clip* c : autoClips) {
      const AutomationClip* a = nullptr;
      for (const auto& x : s.automations)
        if (x.id == c->source) a = &x;
      if (!a) continue;
      if (a->owner == kSongId && a->index == static_cast<uint32_t>(SongParam::Tempo)) {
        automation(root, *c, *a, lost);
        continue;
      }
      std::string trackId;
      if (s.channel(a->owner)) trackId = trackOfChannel[a->owner];
      for (auto& t : pr.child("Structure")->children)
        if (t.name == "Track" && t.child("Channel") && chanOfMixer.count(a->owner) && t.child("Channel")->get("id") == chanOfMixer[a->owner])
          trackId = t.get("id");
      if (trackId.empty()) {
        automation(root, *c, *a, lost);  // the master's: on the arrangement itself
        continue;
      }
      xml::Node* lane = nullptr;
      for (auto& l : root.children)
        if (l.name == "Lanes" && l.get("track") == trackId) lane = &l;
      if (!lane) {
        lane = &root.add("Lanes");
        lane->set("track", trackId);
        lane->set("id", id());
      }
      automation(*lane, *c, *a, lost);
    }
    for (const auto& w : lost.order) loss.add(w, lost.n.at(w));
    // Markers.
    if (!s.markers.empty()) {
      xml::Node& mk = arr.add("Markers");
      mk.set("id", id());
      for (const Marker& m : s.markers) {
        if (m.isMeter()) loss.add("meter changes became plain markers");
        if (m.isKey()) loss.add("key markers became plain markers");
        xml::Node& x = mk.add("Marker");
        x.set("time", beats(m.pos));
        x.set("name", m.name);
      }
    }
    if (s.arrangements.size() > 1) loss.add("other arrangements left out (only the current one is written)", static_cast<int>(s.arrangements.size()) - 1);
    // Metadata.
    xml::Node& md = parts.metadata;
    md.name = "MetaData";
    md.add("Title").text = s.info.title;
    if (!s.info.author.empty()) md.add("Artist").text = s.info.author;
    if (!s.info.genre.empty()) md.add("Genre").text = s.info.genre;
    if (!s.info.comments.empty()) md.add("Comment").text = s.info.comments;
    if (!s.info.url.empty()) md.add("Website").text = s.info.url;
  }
};

// ------------------------------------------------------------------ import

struct Importer {
  const Source& src;
  Song& s;
  Losses loss;
  std::map<std::string, ObjectId> mixerOf;     // Channel id -> mixer track
  std::map<std::string, ObjectId> rackOf;      // Track id -> rack channel
  std::map<std::string, std::pair<ObjectId, uint32_t>> paramOf;  // Volume / Pan id -> (owner, index)
  std::map<std::string, std::string> trackName;
  std::map<std::string, Rgb> trackColor;
  std::set<ObjectId> audioSet;  // samplers that got their file

  Importer(const Source& source, Song& song) : src(source), s(song) {}
  Tick ticks(double beats) const { return static_cast<Tick>(std::llround(beats * s.ppq)); }

  PluginState deviceState(const xml::Node& d, std::string* hostedType) {
    PluginState st;
    const std::string state = d.child("State") ? d.child("State")->get("path") : "";
    std::string bytes;
    if (!state.empty() && src.readBlob && src.readBlob(state, bytes) && state.find(".gridline-state.json") != std::string::npos) {
      Json j;
      if (jsonParse(bytes, j)) return pluginStateFromJson(j);
    }
    const std::string devId = d.get("deviceID");
    std::string type;
    if (d.name == "Vst3Plugin") type = "vst3:" + devId;
    else if (d.name == "ClapPlugin") type = "clap:" + devId;
    else if (d.name == "Vst2Plugin") type = "vst2:" + devId;
    else if (d.name == "AuPlugin") type = "au:" + devId;
    if (hostedType) *hostedType = type;
    if (!type.empty()) {
      st.type = type;
      if (!state.empty()) loss.add("plugin states in another app's form left for the plugin to start fresh");
      return st;
    }
    if (findPlugin(devId)) return defaultPluginState(devId);
    loss.add("devices of another app replaced (instruments by Gridline's synth, effects left out)");
    return st;
  }

  // Volume / Pan / Mute of a Channel element onto (owner, kinds).
  void levels(const xml::Node& ch, ObjectId owner, bool rack) {
    if (const xml::Node* v = ch.child("Volume")) {
      double val = v->num("value", 1);
      if (v->get("unit") == "decibel") val = std::pow(10.0, val / 20.0);
      if (v->get("unit") == "normalized") val = v->num("min", 0) + val * (v->num("max", 2) - v->num("min", 0));
      const float g = static_cast<float>(std::clamp(val, 0.0, 1.25));
      if (rack) s.channel(owner)->gain = g;
      else s.mixerTrack(owner)->volume = g;
      if (val > 1.25) loss.add("volumes above +2 dB lowered to Gridline's top");
      paramOf[v->get("id")] = {owner, static_cast<uint32_t>(rack ? static_cast<uint32_t>(ChannelParam::Volume) : static_cast<uint32_t>(MixerParam::Volume))};
    }
    if (const xml::Node* p = ch.child("Pan")) {
      double val = p->num("value", 0.5);
      if (p->get("unit") != "normalized") val = (val - p->num("min", -1)) / std::max(1e-9, p->num("max", 1) - p->num("min", -1));
      const float pan = static_cast<float>(std::clamp(val * 2 - 1, -1.0, 1.0));
      if (rack) s.channel(owner)->pan = pan;
      else s.mixerTrack(owner)->pan = pan;
      paramOf[p->get("id")] = {owner, rack ? static_cast<uint32_t>(ChannelParam::Pan) : static_cast<uint32_t>(MixerParam::Pan)};
    }
    if (const xml::Node* m = ch.child("Mute")) {
      if (rack) s.channel(owner)->muted = m->flag("value");
      else s.mixerTrack(owner)->muted = m->flag("value");
    }
  }

  void effects(const xml::Node& ch, MixerTrack& m) {
    const xml::Node* devs = ch.child("Devices");
    if (!devs) return;
    size_t k = 0;
    for (const auto& d : devs->children) {
      if (d.get("deviceRole") == "instrument") continue;
      PluginState st = deviceState(d, nullptr);
      if (st.type.empty()) continue;
      if (k >= m.slots.size()) {
        loss.add("effects past the tenth on a track left out");
        break;
      }
      m.slots[k].plugin = st;
      if (const xml::Node* en = d.child("Enabled")) m.slots[k].enabled = en->flag("value", true);
      ++k;
    }
  }

  void collect(const xml::Node& n, std::vector<const xml::Node*>& tracks) {
    for (const auto& c : n.children)
      if (c.name == "Track") {
        tracks.push_back(&c);
        collect(c, tracks);  // nested (group) tracks
      }
  }

  bool run(const xml::Node& pr, const xml::Node* md, std::string* err) {
    if (pr.name != "Project") {
      if (err) *err = "not a DAWproject file (no Project element)";
      return false;
    }
    s = Song();
    ensureMixer(s, 0);
    s.tracks = std::vector<PlaylistTrack>(kDefaultPlaylistTracks);
    if (const xml::Node* tr = pr.child("Transport")) {
      if (const xml::Node* t = tr->child("Tempo")) s.tempo = std::clamp(t->num("value", 120), 10.0, 999.0);
      if (const xml::Node* t = tr->child("TimeSignature")) {
        s.timeSigNum = std::clamp(static_cast<int>(t->num("numerator", 4)), 1, 32);
        s.timeSigDen = std::clamp(static_cast<int>(t->num("denominator", 4)), 1, 32);
      }
    }
    const xml::Node* st = pr.child("Structure");
    std::vector<const xml::Node*> tracks;
    if (st) collect(*st, tracks);
    // Which Channels are routing targets.
    std::set<std::string> targets;
    auto noteTargets = [&](const xml::Node& ch) {
      if (ch.attr("destination")) targets.insert(ch.get("destination"));
      if (const xml::Node* sd = ch.child("Sends"))
        for (const auto* x : sd->all("Send")) targets.insert(x->get("destination"));
    };
    for (const auto* t : tracks)
      if (const xml::Node* ch = t->child("Channel")) noteTargets(*ch);
    // The master.
    const xml::Node* masterEl = nullptr;
    if (st)
      for (const auto& c : st->children)
        if (c.name == "Channel" && c.get("role") == "master") masterEl = &c;
    for (const auto* t : tracks)
      if (const xml::Node* ch = t->child("Channel"); ch && ch->get("role") == "master") masterEl = ch;
    if (masterEl) {
      mixerOf[masterEl->get("id")] = s.masterId();
      levels(*masterEl, s.masterId(), false);
      effects(*masterEl, s.mixer[0]);
    }
    // Buses: routing targets, effect returns and group tracks become mixer inserts.
    for (const auto* t : tracks) {
      const xml::Node* ch = t->child("Channel");
      if (!ch || ch == masterEl) continue;
      const std::string ct = t->get("contentType");
      const bool bus = targets.count(ch->get("id")) || ch->get("role") == "effect" || ct == "tracks" ||
                       (ct == "audio" && !hasClips(pr, t->get("id")));
      if (!bus) continue;
      MixerTrack m;
      m.id = s.allocateId();
      m.name = t->get("name");
      m.color = colorFrom(t->get("color"), m.color);
      s.mixer.push_back(m);
      mixerOf[ch->get("id")] = m.id;
      levels(*ch, m.id, false);
      effects(*ch, s.mixer.back());
      if (ch->flag("solo")) s.mixer.back().solo = true;
    }
    // Routing of the buses.
    for (const auto* t : tracks) {
      const xml::Node* ch = t->child("Channel");
      if (!ch || !mixerOf.count(ch->get("id")) || ch == masterEl) continue;
      MixerTrack* m = s.mixerTrack(mixerOf[ch->get("id")]);
      route(*ch, *m);
    }
    // Instrument and audio tracks become rack channels.
    for (const auto* t : tracks) {
      const xml::Node* ch = t->child("Channel");
      if (!ch || mixerOf.count(ch->get("id")) || ch == masterEl) continue;
      Channel c;
      c.id = s.allocateId();
      c.name = t->get("name", "Track");
      c.color = colorFrom(t->get("color"), c.color);
      c.mixerTrack = s.masterId();
      const xml::Node* devs = ch->child("Devices");
      bool inst = false, fx = false;
      if (devs)
        for (const auto& d : devs->children) {
          const std::string role = d.get("deviceRole");
          if (role == "instrument" && !inst) {
            c.plugin = deviceState(d, nullptr);
            inst = !c.plugin.type.empty();
          } else if (role == "noteFX") {
            PluginState nf = deviceState(d, nullptr);
            if (!nf.type.empty()) c.noteFx.push_back(nf);
          } else if (role != "instrument") {
            fx = true;
          }
        }
      if (!inst) c.plugin = defaultPluginState(t->get("contentType") == "audio" ? "gl.sampler" : "gl.synth");
      const std::string dest = ch->get("destination");
      if (mixerOf.count(dest)) c.mixerTrack = mixerOf[dest];
      s.channels.push_back(c);
      rackOf[t->get("id")] = c.id;
      trackName[t->get("id")] = c.name;
      trackColor[t->get("id")] = c.color;
      levels(*ch, c.id, true);
      if (fx || ch->child("Sends")) {  // the track's own effects and sends: an insert of its own
        MixerTrack m;
        m.id = s.allocateId();
        m.name = c.name;
        m.color = c.color;
        s.mixer.push_back(m);
        effects(*ch, s.mixer.back());
        route(*ch, s.mixer.back());
        s.channel(c.id)->mixerTrack = m.id;
      }
    }
    // The arrangement.
    if (const xml::Node* arr = pr.child("Arrangement")) {
      if (const xml::Node* lanes = arr->child("Lanes")) {
        const double unitScale = lanes->get("timeUnit", "beats") == "seconds" ? s.tempo / 60.0 : 1.0;
        lanesOf(*lanes, "", unitScale);
      }
      if (const xml::Node* ta = arr->child("TempoAutomation")) tempoAutomation(*ta);
      if (const xml::Node* mk = arr->child("Markers"))
        for (const auto* m : mk->all("Marker")) {
          Marker x;
          x.pos = ticks(m->num("time"));
          x.name = m->get("name");
          s.markers.push_back(x);
        }
      std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
    }
    placePatterns();
    if (s.patterns.empty()) {
      Pattern p;
      p.id = s.allocateId();
      p.name = "Pattern 1";
      s.patterns.push_back(p);
    }
    if (pr.child("Scenes") && !pr.child("Scenes")->children.empty()) loss.add("scenes left out");
    if (md) {
      if (const xml::Node* x = md->child("Title"); x && !x->text.empty()) s.info.title = x->text;
      if (const xml::Node* x = md->child("Artist")) s.info.author = x->text;
      if (const xml::Node* x = md->child("Genre")) s.info.genre = x->text;
      if (const xml::Node* x = md->child("Comment")) s.info.comments = x->text;
      if (const xml::Node* x = md->child("Website")) s.info.url = x->text;
    }
    return true;
  }

  static bool hasClipsIn(const xml::Node& n, const std::string& track) {
    for (const auto& c : n.children) {
      if (c.name == "Lanes" && c.get("track") == track && c.child("Clips") && !c.child("Clips")->children.empty()) return true;
      if (hasClipsIn(c, track)) return true;
    }
    return false;
  }
  static bool hasClips(const xml::Node& pr, const std::string& track) {
    const xml::Node* arr = pr.child("Arrangement");
    return arr && hasClipsIn(*arr, track);
  }

  void route(const xml::Node& ch, MixerTrack& m) {
    m.sends.clear();
    const std::string dest = ch.get("destination");
    m.sends.push_back({mixerOf.count(dest) ? mixerOf[dest] : s.masterId(), 1.0f, false});
    if (const xml::Node* sd = ch.child("Sends"))
      for (const auto* x : sd->all("Send")) {
        const std::string d = x->get("destination");
        if (!mixerOf.count(d) || mixerOf[d] == m.id) continue;
        Send e;
        e.to = mixerOf[d];
        e.level = x->child("Volume") ? static_cast<float>(std::clamp(x->child("Volume")->num("value", 1), 0.0, 1.25)) : 1.0f;
        e.preFader = x->get("type") == "pre";
        m.sends.push_back(e);
      }
  }

  // One note clip of one track, before patterns are made.
  struct NoteClip {
    ObjectId channel;
    Tick time, duration, playStart, loopStart, loopEnd;
    bool loop, enabled;
    std::string name;
    Rgb color;
    std::vector<Note> notes;
  };
  std::vector<NoteClip> noteClips;

  std::vector<BendPoint> pointsOf(const xml::Node& pts, double offset) {
    std::vector<BendPoint> out;
    for (const auto* r : pts.all("RealPoint")) out.emplace_back(static_cast<float>(r->num("time") * s.ppq), static_cast<float>(r->num("value") - offset));
    return out;
  }

  std::vector<Note> notesOf(const xml::Node& notes, double scale) {
    std::vector<Note> out;
    for (const auto* n : notes.all("Note")) {
      Note x(ticks(n->num("time") * scale), std::max<Tick>(1, ticks(n->num("duration") * scale)), std::clamp(static_cast<int>(n->num("key", 60)), 0, 127),
             std::clamp(static_cast<int>(std::lround(n->num("vel", 0.8) * 127)), 1, 127));
      if (n->attr("rel")) x.release = std::clamp(static_cast<int>(std::lround(n->num("rel") * 127)), 0, 127);
      if (n->num("channel", 0) != 0) loss.add("notes' MIDI channels merged");
      for (const auto* p : n->all("Points")) {
        const xml::Node* t = p->child("Target");
        const std::string e = t ? t->get("expression") : "";
        if (e == "transpose" || e == "pitchBend") x.bend = pointsOf(*p, 0);
        else if (e == "pressure" || e == "polyPressure") x.pressure = pointsOf(*p, 0);
        else if (e == "timbre") x.timbre = pointsOf(*p, 0.5);
        else loss.add("per-note expressions other than pitch, pressure and timbre left out");
      }
      for (auto* lane : {&x.bend, &x.pressure, &x.timbre}) {
        bend::normalize(*lane);
        if (lane->size() > static_cast<size_t>(bend::kMaxPoints)) {
          lane->resize(static_cast<size_t>(bend::kMaxPoints));
          loss.add("per-note expression lines thinned to 32 points");
        }
      }
      out.push_back(std::move(x));
    }
    std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
    return out;
  }

  ObjectId audioChannel(const std::string& track, const std::string& path) {
    const ObjectId base = rackOf.count(track) ? rackOf[track] : kNoId;
    Channel* c = s.channel(base);
    // The track's own sampler plays its first file (a Gridline export stored the file's old
    // place in the sampler's state; the archive's copy replaces it).
    if (c && c->plugin.type == "gl.sampler" && (!audioSet.count(c->id) || c->plugin.data == path)) {
      c->plugin.data = path;
      audioSet.insert(c->id);
      return c->id;
    }
    for (const auto& ch : s.channels)
      if (ch.plugin.type == "gl.sampler" && ch.plugin.data == path && (!c || ch.mixerTrack == c->mixerTrack)) return ch.id;
    Channel n;
    n.id = s.allocateId();
    n.name = fs::path(path).stem().string();
    n.plugin = defaultPluginState("gl.sampler");
    n.plugin.data = path;
    n.mixerTrack = c ? c->mixerTrack : s.masterId();
    n.color = c ? c->color : n.color;
    if (c) n.gain = c->gain, n.pan = c->pan;
    s.channels.push_back(n);
    if (c) loss.add("audio tracks with several files split into one sampler channel per file");
    return n.id;
  }

  void clip(const xml::Node& x, const std::string& track, double scale, int laneIndex) {
    const xml::Node* audio = x.child("Audio");
    if (!audio)
      if (const xml::Node* w = x.child("Warps")) {
        audio = w->child("Audio");
        loss.add("warped audio played at its own speed");
      }
    const Tick time = ticks(x.num("time") * scale), duration = std::max<Tick>(1, ticks(x.num("duration") * scale));
    if (audio) {
      const xml::Node* f = audio->child("File");
      if (!f) return;
      const std::string path = src.audioPath ? src.audioPath(f->get("path")) : f->get("path");
      if (path.empty()) return;
      Clip c;
      c.id = s.allocateId();
      c.kind = ClipKind::Audio;
      c.source = audioChannel(track, path);
      c.track = std::clamp(laneIndex, 0, static_cast<int>(s.tracks.size()) - 1);
      c.start = time;
      c.length = duration;
      const bool secs = x.get("contentTimeUnit", "beats") == "seconds";
      c.audioStart = x.num("playStart") * (secs ? 1.0 : 60.0 / s.tempo);
      if (x.attr("fadeInTime") || x.attr("fadeOutTime")) {
        const bool fsecs = x.get("fadeTimeUnit", "beats") == "seconds";
        c.fadeIn = x.num("fadeInTime") * (fsecs ? 1.0 : 60.0 / s.tempo);
        c.fadeOut = x.num("fadeOutTime") * (fsecs ? 1.0 : 60.0 / s.tempo);
        c.fadeInManual = c.fadeIn > 0, c.fadeOutManual = c.fadeOut > 0;
      }
      c.muted = !x.flag("enable", true);
      if (x.attr("loopEnd")) loss.add("looping audio clips played once");
      s.playlist.push_back(c);
      return;
    }
    const xml::Node* notes = x.child("Notes");
    if (!notes) {
      if (const xml::Node* inner = x.child("Lanes"))
        for (const auto& c : inner->children)
          if (c.name == "Notes") notes = &c;
    }
    if (!notes || !rackOf.count(track)) {
      if (x.child("Clips") || x.child("Lanes")) loss.add("nested clips left out");
      return;
    }
    NoteClip n;
    n.channel = rackOf[track];
    n.time = time;
    n.duration = duration;
    n.playStart = ticks(x.num("playStart"));
    n.loop = x.attr("loopEnd") != nullptr;
    n.loopStart = ticks(x.num("loopStart"));
    n.loopEnd = ticks(x.num("loopEnd"));
    n.enabled = x.flag("enable", true);
    n.name = x.get("name", trackName[track]);
    n.color = colorFrom(x.get("color"), trackColor.count(track) ? trackColor[track] : 0x6E9F4B);
    n.notes = notesOf(*notes, 1.0);
    noteClips.push_back(std::move(n));
  }

  void points(const xml::Node& pts, double scale) {
    const xml::Node* t = pts.child("Target");
    if (!t) return;
    auto it = paramOf.find(t->get("parameter"));
    if (it == paramOf.end()) {
      loss.add("automation of other parameters left out");
      return;
    }
    const auto [owner, index] = it->second;
    const auto desc = describeParam(s, {owner, index});
    if (!desc) return;
    const auto rps = pts.all("RealPoint");
    if (rps.empty()) return;
    const bool pan = pts.get("unit") == "normalized" && index == 1;  // Pan is index 1 on both kinds
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = owner;
    a.index = index;
    a.name = std::string(pan ? "Pan" : "Volume");
    const Tick t0 = ticks(rps.front()->num("time") * scale);
    for (const auto* r : rps) {
      AutoPoint p;
      p.pos = ticks(r->num("time") * scale) - t0;
      double v = r->num("value");
      if (pan) v = v * 2 - 1;
      else if (pts.get("unit") == "normalized") v = v * 2;  // a normalized volume over 0..2
      p.value = static_cast<float>(desc->toNormalized(desc->clamp(v)));
      p.shape = r->get("interpolation") == "hold" ? AutoShape::Hold : AutoShape::Curve;
      a.points.push_back(p);
    }
    automationClip(a, t0);
  }

  void automationClip(AutomationClip& a, Tick t0) {
    const Tick len = std::max<Tick>(1, a.points.back().pos + 1);
    s.automations.push_back(a);
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Automation;
    c.source = a.id;
    c.start = t0;
    c.length = len;
    c.track = static_cast<int>(s.tracks.size()) - 1;  // the automation clips go together on the last track
    s.playlist.push_back(c);
  }

  void tempoAutomation(const xml::Node& ta) {
    const auto desc = describeParam(s, songParam(SongParam::Tempo));
    const auto rps = ta.all("RealPoint");
    if (!desc || rps.empty()) return;
    AutomationClip a;
    a.id = s.allocateId();
    a.owner = kSongId;
    a.index = static_cast<uint32_t>(SongParam::Tempo);
    a.name = "Tempo";
    const Tick t0 = ticks(rps.front()->num("time"));
    for (const auto* r : rps) {
      AutoPoint p;
      p.pos = ticks(r->num("time")) - t0;
      p.value = static_cast<float>(desc->toNormalized(desc->clamp(r->num("value"))));
      p.shape = r->get("interpolation") == "hold" ? AutoShape::Hold : AutoShape::Curve;
      a.points.push_back(p);
    }
    automationClip(a, t0);
  }

  void lanesOf(const xml::Node& lanes, const std::string& track, double scale) {
    const std::string tr = lanes.attr("track") ? lanes.get("track") : track;
    int laneIndex = 0;  // the playlist track: the channel's place in the rack
    for (size_t i = 0; i < s.channels.size(); ++i)
      if (rackOf.count(tr) && s.channels[i].id == rackOf[tr]) laneIndex = static_cast<int>(i);
    for (const auto& c : lanes.children) {
      if (c.name == "Lanes") lanesOf(c, tr, c.get("timeUnit", "") == "seconds" ? s.tempo / 60.0 : scale);
      else if (c.name == "Clips")
        for (const auto* x : c.all("Clip")) clip(*x, tr, scale, laneIndex);
      else if (c.name == "Points") points(c, scale);
      else if (c.name == "Notes" && rackOf.count(tr)) {  // notes straight on the timeline: one clip
        NoteClip n{rackOf[tr], 0, 0, 0, 0, 0, false, true, trackName[tr], trackColor[tr], notesOf(c, scale)};
        if (n.notes.empty()) continue;
        n.duration = n.notes.back().start + n.notes.back().length;
        noteClips.push_back(std::move(n));
      } else if (c.name == "Audio" || c.name == "Warps" || c.name == "Video") {
        loss.add("audio straight on a timeline (outside a clip) left out");
      }
    }
  }

  // Note clips that sit together (same time, length, start and name) become one pattern with
  // each track's notes, and identical ones share it.
  void placePatterns() {
    struct Key {
      Tick time, duration, playStart;
      std::string name;
      bool operator<(const Key& o) const {
        return std::tie(time, duration, playStart, name) < std::tie(o.time, o.duration, o.playStart, o.name);
      }
    };
    std::map<Key, std::vector<const NoteClip*>> groups;
    std::vector<Key> order;
    for (const auto& n : noteClips) {
      Key k{n.time, n.duration, n.playStart, n.name};
      if (!groups.count(k)) order.push_back(k);
      groups[k].push_back(&n);
    }
    for (const Key& k : order) {
      const auto& g = groups[k];
      std::map<ObjectId, std::vector<Note>> content;
      Tick contentLen = 0;
      for (const NoteClip* n : g) {
        auto& dst = content[n->channel];
        dst.insert(dst.end(), n->notes.begin(), n->notes.end());
        for (const Note& x : n->notes) contentLen = std::max(contentLen, x.start + x.length);
        if (n->loop) contentLen = std::max(contentLen, n->loopEnd);
      }
      const NoteClip& first = *g.front();
      if (!first.loop) contentLen = std::max(contentLen, first.playStart + first.duration);
      Pattern* pat = nullptr;
      for (auto& p : s.patterns)
        if (p.name == k.name && p.notes == content) pat = &p;
      if (!pat) {
        Pattern p;
        p.id = s.allocateId();
        p.name = k.name.empty() ? "Pattern " + std::to_string(s.patterns.size() + 1) : k.name;
        p.color = first.color;
        p.notes = content;
        const Tick bar = s.barTicks();
        const Tick need = first.loop ? first.loopEnd : contentLen;
        p.bars = std::max(1, static_cast<int>((need + bar - 1) / bar));
        s.patterns.push_back(p);
        pat = &s.patterns.back();
        if (first.loop && (first.loopStart != 0 || first.loopEnd % bar != 0)) loss.add("clip loops that do not span whole bars loop the pattern's bars");
      }
      Clip c;
      c.id = s.allocateId();
      c.kind = ClipKind::Pattern;
      c.pattern = pat->id;
      c.start = k.time;
      c.length = k.duration;
      c.offset = k.playStart;
      c.loop = first.loop;
      c.muted = !first.enabled;
      int lane = 0;
      for (size_t i = 0; i < s.channels.size(); ++i)
        if (s.channels[i].id == first.channel) lane = static_cast<int>(i);
      c.track = std::clamp(lane, 0, static_cast<int>(s.tracks.size()) - 2);
      s.playlist.push_back(c);
    }
  }
};

}  // namespace

Parts toParts(const Song& song, Report* report) {
  Exporter e(song);
  e.run();
  e.loss.into(report);
  return std::move(e.parts);
}

bool fromParts(const xml::Node& project, const xml::Node* metadata, const Source& src, Song& out, Report* report, std::string* err) {
  Song s;
  Importer im(src, s);
  if (!im.run(project, metadata, err)) return false;
  im.loss.into(report);
  out = std::move(s);
  return true;
}

bool isDawProject(const std::string& path) {
  std::string e = fs::path(path).extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e == ".dawproject";
}

bool exportFile(const Song& song, const std::string& path, Report* report, std::string* err) {
  Parts p = toParts(song, report);
  ZipWriter z;
  if (!z.open(path, err)) return false;
  auto bytes = [](const std::string& t) { return std::vector<uint8_t>(t.begin(), t.end()); };
  bool ok = z.add("project.xml", bytes(xml::write(p.project))) && z.add("metadata.xml", bytes(xml::write(p.metadata)));
  for (const auto& [name, data] : p.blobs) ok = ok && z.add(name, bytes(data));
  for (const auto& [name, file] : p.files) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
      if (report) report->lines.push_back("1 missing audio file left out: " + fs::path(file).filename().string());
      continue;
    }
    ok = ok && z.addFile(name, file, err);
  }
  if (!ok) {
    if (err && err->empty()) *err = "cannot write the archive";
    return false;
  }
  return z.close(err);
}

bool importFile(const std::string& path, const std::string& audioDir, Song& out, Report* report, std::string* err) {
  std::vector<ZipEntry> entries;
  if (!readZipDirectory(path, entries, err)) return false;
  auto read = [&](const std::string& name, std::string& text) {
    for (const auto& e : entries)
      if (e.name == name) {
        std::vector<uint8_t> b;
        if (!readZipEntry(path, e, b, err)) return false;
        text.assign(b.begin(), b.end());
        return true;
      }
    return false;
  };
  std::string projText, metaText;
  if (!read("project.xml", projText)) {
    if (err && err->empty()) *err = "no project.xml in the archive";
    return false;
  }
  xml::Node project, metadata;
  if (!xml::parse(projText, project, err)) return false;
  const bool hasMeta = read("metadata.xml", metaText) && xml::parse(metaText, metadata, nullptr);
  std::map<std::string, std::string> extracted;
  Source src;
  src.readBlob = [&](const std::string& name, std::string& bytes) { return read(name, bytes); };
  src.audioPath = [&](const std::string& name) -> std::string {
    if (auto it = extracted.find(name); it != extracted.end()) return it->second;
    for (const auto& e : entries) {
      if (e.name != name) continue;
      std::vector<uint8_t> b;
      if (!readZipEntry(path, e, b, nullptr)) return "";
      std::error_code ec;
      fs::create_directories(audioDir, ec);
      const fs::path dst = fs::path(audioDir) / safeName(fs::path(name).filename().string());
      std::ofstream o(dst, std::ios::binary);
      o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
      if (!o) return "";
      return extracted[name] = dst.string();
    }
    if (report) report->lines.push_back("1 audio file the archive does not hold left out: " + name);
    return "";
  };
  return fromParts(project, hasMeta ? &metadata : nullptr, src, out, report, err);
}

}  // namespace gl::dawproject
