// Gridline — FL origin capture and object fingerprints (see FlpOrigin.h).
#include "FlpOrigin.h"
#include <cstring>
#include "../Arrangements.h"
#include "Flp.h"

namespace gl::flp {

std::string originKey(const char* kind, ObjectId id) { return std::string(kind) + "/" + std::to_string(id); }
std::string pluginKey(ObjectId owner, int slot) { return "plug/" + std::to_string(owner) + "/" + std::to_string(slot); }

int IndexMap::owner(ObjectId id) const {
  if (id == kSongId) return -2;
  if (int c = ch(id); c >= 0) return c;
  if (int i = ins(id); i >= 0) return 0x10000 + i;
  return -1;
}

namespace {

// FNV-1a over a canonical byte stream of typed values.
struct Fp {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull;
  }
  Fp& i(int64_t v) {
    bytes(&v, sizeof v);
    return *this;
  }
  Fp& f(double v) {
    bytes(&v, sizeof v);
    return *this;
  }
  Fp& s(const std::string& v) {
    i(static_cast<int64_t>(v.size()));
    bytes(v.data(), v.size());
    return *this;
  }
  Fp& tag(const char* t) { return s(t); }
  Fp& plugin(const PluginState& p) {
    s(p.type).i(static_cast<int64_t>(p.params.size()));
    for (float x : p.params) f(x);
    return s(p.data);
  }
  Fp& point(const AutoPoint& p) { return i(p.pos).f(p.value).f(p.tension).i(static_cast<int>(p.shape)); }
};

}  // namespace

uint64_t fingerprintPlugin(const PluginState& p) {
  Fp h;
  h.tag("plugin").plugin(p);
  return h.h;
}

uint64_t fingerprintChannel(const Channel& c, const IndexMap& m) {
  Fp h;
  h.tag("channel").s(c.name).i(c.color).f(c.gain).f(c.pan).i(c.muted).i(c.pitch).i(m.ins(c.mixerTrack));
  h.i(c.flIcon);  // J-39
  // A plugin channel's instrument has its own origin entry; a sampler's
  // settings are channel events.
  if (c.plugin.type == "gl.sampler" || c.plugin.type.empty()) h.plugin(c.plugin);
  else h.s(c.plugin.type);
  h.i(static_cast<int64_t>(c.noteFx.size()));
  for (const auto& n : c.noteFx) h.plugin(n);
  h.i(c.fine).f(c.swingMix).f(c.timeShift).i(c.polyphony).i(c.mono).i(c.porta).f(c.portaTime).i(c.cutGroup).i(c.cutBy);
  h.i(c.echo.count).f(c.echo.feedback).f(c.echo.time).f(c.echo.pan).i(c.echo.pitch).i(c.echo.pingPong).f(c.echo.modX).f(c.echo.modY);
  h.i(static_cast<int64_t>(c.layer.size()));
  for (ObjectId k : c.layer) h.i(m.ch(k));
  h.i(c.layerMode).i(c.group);
  h.i(static_cast<int64_t>(c.auxOuts.size()));
  for (ObjectId k : c.auxOuts) h.i(m.ins(k));
  h.i(c.midiOutPort).i(c.midiInPort);
  return h.h;
}

uint64_t fingerprintPattern(const Pattern& p, const Song& s, const IndexMap& m) {
  Fp h;
  h.tag("pattern").s(p.name).i(p.color).i(p.stepsPerBar).i(p.bars).i(s.barTicks());
  for (const auto& [ch, v] : p.steps) {
    h.i(m.ch(ch)).i(static_cast<int64_t>(v.size()));
    for (uint8_t x : v) h.i(x);
  }
  for (const auto& [ch, v] : p.notes) {
    h.i(m.ch(ch)).i(static_cast<int64_t>(v.size()));
    for (const Note& n : v)
      h.i(n.start).i(n.length).i(n.pitch).i(n.velocity).i(n.pan).i(n.fine).i(n.release).i(n.modX).i(n.modY).i(n.color).i(n.slide).i(n.porta).i(n.muted);
  }
  for (const auto& l : p.automation) {
    h.i(m.owner(l.owner)).i(l.index).i(static_cast<int64_t>(l.points.size()));
    for (const auto& pt : l.points) h.point(pt);
  }
  for (const auto& [ch, v] : p.stepProps) {
    h.i(m.ch(ch)).i(static_cast<int64_t>(v.size()));
    for (const auto& sp : v) h.i(sp.pitch).i(sp.pan).i(sp.release).i(sp.fine).i(sp.modX).i(sp.modY).i(sp.shift);
  }
  return h.h;
}

uint64_t fingerprintInsert(const MixerTrack& t, const IndexMap& m) {
  Fp h;
  h.tag("insert").s(t.name).i(t.color).f(t.volume).f(t.pan).f(t.stereoSep).i(t.muted).i(t.solo).i(t.invertPolarity).i(t.swapLR);
  for (const auto& sl : t.slots) h.i(sl.enabled).f(sl.mix).i(sl.plugin.empty());
  for (const auto& sd : t.sends) h.i(m.ins(sd.to)).f(sd.level).i(sd.sidechain).i(sd.preFader);
  for (int b = 0; b < 3; ++b) h.f(t.eqGain[static_cast<size_t>(b)]).f(t.eqFreq[static_cast<size_t>(b)]).f(t.eqQ[static_cast<size_t>(b)]);
  h.i(t.icon).i(t.disabled).i(t.dock).i(t.latencyOffset).i(t.threaded).s(t.group).i(t.input).i(t.inputStereo).i(t.monitor).i(t.armed);
  h.i(t.separator).i(t.locked).i(t.flIcon);  // J-39
  return h.h;
}

uint64_t fingerprintAutomation(const AutomationClip& a, const IndexMap& m) {
  Fp h;
  h.tag("automation").s(a.name).i(a.color).i(m.owner(a.owner)).i(a.index).f(a.min).f(a.max).i(a.loop);
  for (const auto& p : a.points) h.point(p);
  return h.h;
}

uint64_t fingerprintClip(const Clip& c, const IndexMap& m) {
  Fp h;
  h.tag("clip").i(static_cast<int>(c.kind)).i(m.pat(c.pattern)).i(c.track).i(c.start).i(c.length);
  h.i(c.kind == ClipKind::Audio ? m.ch(c.source) : c.kind == ClipKind::Automation ? m.aut(c.source) : -1);
  h.i(c.offset).i(c.muted).f(c.audioStart).f(c.audioEnd).f(c.fadeIn).f(c.fadeOut).f(c.gain).f(c.pitch).f(c.stretch).f(c.syncBpm).i(c.reverse);
  h.i(c.group != kNoId).i(c.launched);  // J-39 (the group number is solved at export)
  return h.h;
}

uint64_t fingerprintArrangement(const std::vector<PlaylistTrack>& tracks, const std::vector<Marker>& markers, const LiveSettings& live,
                                const std::string& name) {
  Fp h;
  h.tag("arrangement").s(name).i(live.areaEnd).i(static_cast<int64_t>(tracks.size()));
  for (const auto& t : tracks)
    h.s(t.name).i(t.color).i(t.muted).f(t.height).i(static_cast<int>(t.launch)).i(static_cast<int>(t.pad)).i(static_cast<int>(t.follow))
        .i(static_cast<int>(t.syncStart)).i(t.groupSize).i(t.folded).i(t.locked).i(t.flIcon);
  for (const auto& mk : markers) h.i(mk.pos).s(mk.name).i(mk.timeSigNum).i(mk.timeSigDen).i(static_cast<int>(mk.action));
  return h.h;
}

uint64_t fingerprintProject(const Song& s) {
  Fp h;
  h.tag("project").i(s.ppq).f(s.tempo).i(s.timeSigNum).i(s.timeSigDen).f(s.swing).f(s.masterPitch);
  h.s(s.info.title).s(s.info.author).s(s.info.genre).s(s.info.url).s(s.info.comments).i(s.info.created).i(s.info.workTime);
  for (const auto& g : s.channelGroups) h.s(g);
  return h.h;
}

uint64_t fingerprintLinks(const Song& s, const IndexMap& m) {
  Fp h;
  h.tag("links");
  for (const auto& l : s.links)
    h.i(l.cc).i(l.midiChannel).i(m.owner(l.owner)).i(l.index).f(l.min).f(l.max).i(static_cast<int>(l.source)).i(l.port).i(static_cast<int>(l.curve))
        .s(l.formula).i(l.invert).i(static_cast<int>(l.takeover)).i(static_cast<int>(l.encoder)).f(l.sensitivity).i(static_cast<int>(l.button))
        .i(static_cast<int>(l.action));
  return h.h;
}

uint64_t fingerprintModLinks(const Song& s, const IndexMap& m) {
  Fp h;
  h.tag("modlinks");
  for (const auto& l : s.modLinks)
    h.i(m.owner(l.sourceOwner)).i(l.sourceSlot).i(l.sourceOutput).i(m.owner(l.owner)).i(l.index).f(l.min).f(l.max).i(static_cast<int>(l.curve))
        .s(l.formula).f(l.smoothMs).i(l.enabled);
  return h.h;
}

EventList originEvents(const Song& s, const std::string& key) {
  auto it = s.flOrigin.blobs.find(key);
  return it == s.flOrigin.blobs.end() ? EventList{} : decodeEvents(it->second);
}

int originIndex(const EventList& ev) {
  const RawEvent* e = findRaw(ev, kFlIndex);
  return e && e->data.size() >= 4 ? static_cast<int>(e->value()) : -1;
}

bool originFingerprint(const EventList& ev, uint64_t& out) {
  const RawEvent* e = findRaw(ev, kFingerprint);
  if (!e || e->data.size() != 8) return false;
  out = 0;
  for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(e->data[static_cast<size_t>(i)]) << (8 * i);
  return true;
}

std::string originGlType(const EventList& ev) {
  const RawEvent* e = findRaw(ev, kGlType);
  return e ? std::string(e->data.begin(), e->data.end()) : std::string();
}

EventList stripPseudo(const EventList& ev) {
  EventList out;
  out.reserve(ev.size());
  for (const auto& e : ev)
    if (e.id != kFlIndex && e.id != kFingerprint && e.id != kGlType) out.push_back(e);
  return out;
}

namespace {

RawEvent pseudoIndex(int idx) {
  RawEvent e;
  e.id = kFlIndex;
  ByteWriter w;
  w.u32(static_cast<uint32_t>(idx));
  e.data = w.b;
  return e;
}
RawEvent pseudoFp(uint64_t v) {
  RawEvent e;
  e.id = kFingerprint;
  ByteWriter w;
  w.u64(v);
  e.data = w.b;
  return e;
}
RawEvent pseudoType(const std::string& t) {
  RawEvent e;
  e.id = kGlType;
  e.data.assign(t.begin(), t.end());
  return e;
}

// Section events without the link events the splitter files under whatever
// section is open (226 / 227 are written between the pattern and channel runs).
EventList sectionEvents(const std::vector<Event>& evs) {
  EventList out;
  for (const Event& e : evs)
    if (e.id != 226 && e.id != 227) {
      RawEvent r;
      r.id = e.id;
      r.data.assign(e.data, e.data + e.size);
      out.push_back(std::move(r));
    }
  return out;
}

EventList pluginEvents(const std::vector<Event>& evs) {
  EventList out;
  for (const Event& e : evs)
    if (e.id == 201 || e.id == 212 || e.id == 213) {
      RawEvent r;
      r.id = e.id;
      r.data.assign(e.data, e.data + e.size);
      out.push_back(std::move(r));
    }
  return out;
}

}  // namespace

void captureOrigin(Context& c) {
  Song& s = c.song;
  auto put = [&](const std::string& key, EventList ev) { s.flOrigin.blobs[key] = eventsBlob(ev); };
  IndexMap m;
  for (const auto& [fl, id] : c.channelIds) m.channel[id] = fl;
  for (const auto& [fl, id] : c.automationIds) m.automation[id] = fl;
  for (const auto& [fl, id] : c.patternIds) m.pattern[id] = fl;
  for (const auto& [fl, id] : c.insertIds) m.insert[id] = fl;

  {
    EventList ev = toRaw(c.sec.project);
    ev.push_back(pseudoFp(fingerprintProject(s)));
    put("proj", std::move(ev));
  }
  {
    EventList links, mods;
    for (const Event& e : c.file.events) {
      if (e.id == 226) links.push_back({e.id, Bytes(e.data, e.data + e.size)});
      if (e.id == 227 && e.size >= 2 && (e.u16(0) & 0x8000)) mods.push_back({e.id, Bytes(e.data, e.data + e.size)});
    }
    links.push_back(pseudoFp(fingerprintLinks(s, m)));
    mods.push_back(pseudoFp(fingerprintModLinks(s, m)));
    put("links", std::move(links));
    put("modlinks", std::move(mods));
  }
  // Channels, automation channels and their plugins.
  for (const auto& [fl, evs] : c.sec.channels) {
    if (auto a = c.automationIds.find(fl); a != c.automationIds.end()) {
      EventList ev = sectionEvents(evs);
      for (const Event& e : c.file.events)  // its target links
        if (e.id == 227 && e.size >= 4 && !(e.u16(0) & 0x8000) && e.u16(2) == fl) ev.push_back({e.id, Bytes(e.data, e.data + e.size)});
      ev.push_back(pseudoIndex(fl));
      if (const AutomationClip* ac = s.automation(a->second)) ev.push_back(pseudoFp(fingerprintAutomation(*ac, m)));
      put(originKey("auto", a->second), std::move(ev));
      continue;
    }
    auto id = c.channelIds.find(fl);
    if (id == c.channelIds.end()) continue;
    const Channel* ch = s.channel(id->second);
    if (!ch) continue;
    EventList ev = sectionEvents(evs);
    ev.push_back(pseudoIndex(fl));
    ev.push_back(pseudoFp(fingerprintChannel(*ch, m)));
    put(originKey("ch", ch->id), std::move(ev));
    if (c.channelPlugins.count(fl)) {
      EventList pe = pluginEvents(evs);
      pe.push_back(pseudoType(ch->plugin.type));
      pe.push_back(pseudoFp(fingerprintPlugin(ch->plugin)));
      put(pluginKey(ch->id, -1), std::move(pe));
    }
  }
  // Patterns (also the ones created for placed-but-unsaved FL patterns).
  for (const auto& [fl, id] : c.patternIds) {
    const Pattern* p = s.pattern(id);
    if (!p) continue;
    EventList ev;
    if (auto it = c.sec.patterns.find(fl); it != c.sec.patterns.end()) ev = sectionEvents(it->second);
    ev.push_back(pseudoIndex(fl));
    ev.push_back(pseudoFp(fingerprintPattern(*p, s, m)));
    put(originKey("pat", id), std::move(ev));
  }
  // Arrangements and their clips.
  for (const auto& [fl, evs] : c.sec.arrangements) {
    auto gi = c.arrangementIndex.find(fl);
    if (gi == c.arrangementIndex.end()) continue;
    EventList ev;
    for (const Event& e : evs)
      if (e.id != 233 && e.id != 226 && e.id != 227) ev.push_back({e.id, Bytes(e.data, e.data + e.size)});
    ev.push_back(pseudoIndex(fl));
    const bool current = s.arrangements.empty() || gi->second == s.currentArrangement;
    const Arrangement* a = s.arrangements.empty() ? nullptr : &s.arrangements[static_cast<size_t>(gi->second)];
    const std::string name = a ? a->name : "Arrangement";
    ev.push_back(pseudoFp(current ? fingerprintArrangement(s.tracks, s.markers, s.live, name)
                                  : fingerprintArrangement(a->tracks, a->markers, a->live, name)));
    put("arr/" + std::to_string(gi->second), std::move(ev));
  }
  forEachArrangementClip(s, [&](const Clip& cl) {
    auto r = c.clipRecords.find(cl.id);
    if (r == c.clipRecords.end()) return;
    EventList ev;
    ev.push_back({233, Bytes(r->second.begin(), r->second.end())});
    ev.push_back(pseudoFp(fingerprintClip(cl, m)));
    put(originKey("clip", cl.id), std::move(ev));
  });
  // Mixer: insert events + their 225 records, slot plugins.
  std::map<int, EventList> recs;  // FL insert → its 225 records (one event each)
  EventList loose;
  {
    int base = -1;
    for (const Event& e : c.sec.mixerParams)
      for (uint32_t i = 0; i + 12 <= e.size; i += 12) {
        const int kind = e.u8(i + 5);
        if (kind == 31 || kind == 32) base = base < 0 ? (e.u16(i + 6) >> 6) : std::min(base, e.u16(i + 6) >> 6);
      }
    for (const Event& e : c.sec.mixerParams)
      for (uint32_t i = 0; i + 12 <= e.size; i += 12) {
        const int kind = e.u8(i + 5);
        const int ins = base >= 0 ? (e.u16(i + 6) >> 6) - base : -1;
        RawEvent r{225, Bytes(e.data + i, e.data + i + 12)};
        if ((kind == 31 || kind == 32) && ins >= 0) recs[ins].push_back(std::move(r));
        else loose.push_back(std::move(r));
      }
    if (base >= 0) loose.push_back(pseudoIndex(base));  // the base the records were written with
  }
  put("mxp", std::move(loose));
  for (const auto& [fl, id] : c.insertIds) {
    const MixerTrack* t = s.mixerTrack(id);
    if (!t || fl < 0 || static_cast<size_t>(fl) >= c.sec.inserts.size()) continue;
    const auto& in = c.sec.inserts[static_cast<size_t>(fl)];
    EventList ev = toRaw(in.events);
    for (auto& r : recs[fl]) ev.push_back(std::move(r));
    ev.push_back(pseudoIndex(fl));
    ev.push_back(pseudoFp(fingerprintInsert(*t, m)));
    put(originKey("mx", id), std::move(ev));
    for (size_t k = 0; k < in.slots.size() && k < t->slots.size(); ++k) {
      if (in.slots[k].events.empty()) continue;
      EventList pe = toRaw(in.slots[k].events);
      const PluginState& ps = t->slots[k].plugin;
      pe.push_back(pseudoType(ps.type));
      pe.push_back(pseudoFp(fingerprintPlugin(ps)));
      put(pluginKey(id, static_cast<int>(k)), std::move(pe));
    }
  }
  // Inserts the importer trimmed (unused): their events as they were, by FL index.
  for (size_t fl = 0; fl + 1 < c.sec.inserts.size(); ++fl) {
    if (c.insertIds.count(static_cast<int>(fl))) continue;
    EventList ev = toRaw(c.sec.inserts[fl].events);
    for (auto& r : recs[static_cast<int>(fl)]) ev.push_back(std::move(r));
    ev.push_back(pseudoIndex(static_cast<int>(fl)));
    put("mxfl/" + std::to_string(fl), std::move(ev));
  }
  if (c.sec.inserts.size() > c.insertIds.size() && !c.sec.inserts.empty()) {
    EventList ev = toRaw(c.sec.inserts.back().events);
    ev.push_back(pseudoIndex(static_cast<int>(c.sec.inserts.size()) - 1));
    put("mxcur", std::move(ev));
  }
}

}  // namespace gl::flp
