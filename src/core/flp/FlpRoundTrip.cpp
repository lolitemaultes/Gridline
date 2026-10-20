// Gridline — FLP round trip: import → export → import, song comparison
// field by field, event-stream comparison (J-34).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include "FlpExport.h"

namespace gl::flp {

namespace {

struct Cmp {
  std::vector<std::string> out;
  size_t limit = 400;
  template <typename T>
  void v(const std::string& path, const T& a, const T& b) {
    if (a == b) return;
    if (out.size() < limit) {
      std::ostringstream os;
      os << path << ": " << a << " | " << b;
      out.push_back(os.str());
    } else if (out.size() == limit) {
      out.push_back("…");
    }
  }
  void f(const std::string& path, float a, float b) {
    if (a == b || (a != a && b != b)) return;
    v(path, a, b);
  }
  void d(const std::string& path, double a, double b) {
    if (a == b || (a != a && b != b)) return;
    v(path, a, b);
  }
  void plugin(const std::string& p, const PluginState& a, const PluginState& b) {
    v(p + ".type", a.type, b.type);
    v(p + ".data", a.data.size() > 80 ? "<" + std::to_string(a.data.size()) + " bytes>" : a.data,
      b.data.size() > 80 ? "<" + std::to_string(b.data.size()) + " bytes>" : b.data);
    if (a.data.size() > 80 && b.data.size() > 80 && a.data != b.data) v<std::string>(p + ".data", "(differs)", "");
    v(p + ".params.size", a.params.size(), b.params.size());
    // Parameter paths carry the plugin type ("….plugin.params<gl.sampler>[11]") so reports can group them.
    for (size_t k = 0; k < a.params.size() && k < b.params.size(); ++k) f(p + ".params<" + a.type + ">[" + std::to_string(k) + "]", a.params[k], b.params[k]);
  }
  void points(const std::string& p, const std::vector<AutoPoint>& a, const std::vector<AutoPoint>& b) {
    v(p + ".size", a.size(), b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const std::string q = p + "[" + std::to_string(i) + "]";
      v(q + ".pos", a[i].pos, b[i].pos);
      f(q + ".value", a[i].value, b[i].value);
      f(q + ".tension", a[i].tension, b[i].tension);
      v(q + ".shape", static_cast<int>(a[i].shape), static_cast<int>(b[i].shape));
    }
  }
  void clips(const std::string& p, const std::vector<Clip>& a, const std::vector<Clip>& b) {
    v(p + ".size", a.size(), b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const std::string q = p + "[" + std::to_string(i) + "]";
      const Clip &x = a[i], &y = b[i];
      v(q + ".kind", static_cast<int>(x.kind), static_cast<int>(y.kind));
      v(q + ".pattern", x.pattern, y.pattern);
      v(q + ".source", x.source, y.source);
      v(q + ".track", x.track, y.track);
      v(q + ".start", x.start, y.start);
      v(q + ".length", x.length, y.length);
      v(q + ".offset", x.offset, y.offset);
      v(q + ".muted", x.muted, y.muted);
      v(q + ".launched", x.launched, y.launched);  // J-39
      v(q + ".grouped", x.group != kNoId, y.group != kNoId);
      d(q + ".audioStart", x.audioStart, y.audioStart);
      d(q + ".audioEnd", x.audioEnd, y.audioEnd);
      d(q + ".fadeIn", x.fadeIn, y.fadeIn);
      d(q + ".fadeOut", x.fadeOut, y.fadeOut);
      v(q + ".fadeInManual", x.fadeInManual, y.fadeInManual);  // J-43
      v(q + ".fadeOutManual", x.fadeOutManual, y.fadeOutManual);
      v(q + ".fadeInShape", static_cast<int>(x.fadeInShape), static_cast<int>(y.fadeInShape));
      v(q + ".fadeOutShape", static_cast<int>(x.fadeOutShape), static_cast<int>(y.fadeOutShape));
      f(q + ".fadeInTension", x.fadeInTension, y.fadeInTension);
      f(q + ".fadeOutTension", x.fadeOutTension, y.fadeOutTension);
      v(q + ".fadeInDeclick", static_cast<int>(x.fadeInDeclick), static_cast<int>(y.fadeInDeclick));
      v(q + ".fadeOutDeclick", static_cast<int>(x.fadeOutDeclick), static_cast<int>(y.fadeOutDeclick));
      f(q + ".gain", x.gain, y.gain);
      f(q + ".pitch", x.pitch, y.pitch);
      d(q + ".stretch", x.stretch, y.stretch);
      v(q + ".id", x.id, y.id);
    }
  }
  void tracks(const std::string& p, const std::vector<PlaylistTrack>& a, const std::vector<PlaylistTrack>& b) {
    v(p + ".size", a.size(), b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const std::string q = p + "[" + std::to_string(i) + "]";
      v(q + ".name", a[i].name, b[i].name);
      v(q + ".color", a[i].color, b[i].color);
      v(q + ".muted", a[i].muted, b[i].muted);
      f(q + ".height", a[i].height, b[i].height);
      v(q + ".live", static_cast<int>(a[i].launch) * 100 + static_cast<int>(a[i].pad) * 10 + static_cast<int>(a[i].follow),
        static_cast<int>(b[i].launch) * 100 + static_cast<int>(b[i].pad) * 10 + static_cast<int>(b[i].follow));
      v(q + ".syncStart", static_cast<int>(a[i].syncStart), static_cast<int>(b[i].syncStart));
      v(q + ".groupSize", a[i].groupSize, b[i].groupSize);
      v(q + ".locked", a[i].locked, b[i].locked);
    }
  }
  void markers(const std::string& p, const std::vector<Marker>& a, const std::vector<Marker>& b) {
    v(p + ".size", a.size(), b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const std::string q = p + "[" + std::to_string(i) + "]";
      v(q + ".pos", a[i].pos, b[i].pos);
      v(q + ".name", a[i].name, b[i].name);
      v(q + ".timeSig", a[i].timeSigNum * 100 + a[i].timeSigDen, b[i].timeSigNum * 100 + b[i].timeSigDen);
      v(q + ".action", static_cast<int>(a[i].action), static_cast<int>(b[i].action));
    }
  }
};

}  // namespace

namespace {

// Comparable form of a song: object ids renumbered by position (channels,
// mixer tracks, patterns, automation clips; clip ids cleared), references
// remapped, and the parts the importer rebuilds by its own rules normalised:
// trailing unused mixer inserts and default playlist tracks are dropped.
Song canonical(const Song& in) {
  Song s = in;
  std::map<ObjectId, ObjectId> m;
  for (size_t i = 0; i < s.channels.size(); ++i) m[s.channels[i].id] = static_cast<ObjectId>(100000 + i);
  for (size_t i = 0; i < s.mixer.size(); ++i) m[s.mixer[i].id] = static_cast<ObjectId>(200000 + i);
  for (size_t i = 0; i < s.patterns.size(); ++i) m[s.patterns[i].id] = static_cast<ObjectId>(300000 + i);
  for (size_t i = 0; i < s.automations.size(); ++i) m[s.automations[i].id] = static_cast<ObjectId>(400000 + i);
  auto r = [&](ObjectId id) {
    auto it = m.find(id);
    return it == m.end() ? id : it->second;
  };
  for (auto& c : s.channels) {
    c.id = r(c.id);
    c.mixerTrack = r(c.mixerTrack);
    for (auto& l : c.layer) l = r(l);
    for (auto& o : c.auxOuts) o = r(o);
  }
  for (auto& t : s.mixer) {
    t.id = r(t.id);
    for (auto& sd : t.sends) sd.to = r(sd.to);
  }
  for (auto& p : s.patterns) {
    p.id = r(p.id);
    std::map<ObjectId, std::vector<Note>> notes;
    for (auto& [k, v] : p.notes)
      if (!v.empty()) notes[r(k)] = v;
    p.notes = std::move(notes);
    std::map<ObjectId, std::vector<uint8_t>> steps;
    for (auto& [k, v] : p.steps) steps[r(k)] = v;
    p.steps = std::move(steps);
    std::map<ObjectId, std::vector<StepProps>> props;
    for (auto& [k, v] : p.stepProps) props[r(k)] = v;
    p.stepProps = std::move(props);
    for (auto& l : p.automation) l.owner = r(l.owner);
  }
  for (auto& a : s.automations) a.id = r(a.id), a.owner = r(a.owner);
  auto clips = [&](std::vector<Clip>& v) {
    for (auto& c : v) c.id = kNoId, c.pattern = r(c.pattern), c.source = r(c.source), c.takeGroup = kNoId;
  };
  clips(s.playlist);
  for (auto& ar : s.arrangements) clips(ar.playlist);
  for (auto& l : s.links) l.owner = r(l.owner);
  for (auto& l : s.modLinks) l.owner = r(l.owner), l.sourceOwner = r(l.sourceOwner);
  // Trailing default playlist tracks.
  auto trimTracks = [](std::vector<PlaylistTrack>& t, const std::vector<Clip>& cl) {
    size_t used = 0;
    for (const auto& c : cl) used = std::max(used, static_cast<size_t>(c.track) + 1);
    while (t.size() > used && t.back() == PlaylistTrack{}) t.pop_back();
  };
  trimTracks(s.tracks, s.playlist);
  for (auto& ar : s.arrangements) trimTracks(ar.tracks, ar.playlist);
  // Trailing unused inserts (default settings, no effects, routed to the master only, nothing routed to them).
  while (s.mixer.size() > 1) {
    const MixerTrack& t = s.mixer.back();
    bool used = !t.name.empty() || t.volume != MixerTrack{}.volume || t.pan != 0 || t.muted;
    for (const auto& sl : t.slots) used = used || !sl.plugin.empty();
    for (const auto& sd : t.sends) used = used || sd.to != s.mixer[0].id;
    for (const auto& c : s.channels) used = used || c.mixerTrack == t.id;
    for (const auto& o : s.mixer)
      for (const auto& sd : o.sends) used = used || (sd.to == t.id && &o != &t);
    if (used) break;
    s.mixer.pop_back();
  }
  return s;
}

}  // namespace

std::vector<std::string> compareSongs(const Song& a0, const Song& b0, bool ignoreOrigin) {
  const Song a = canonical(a0), b = canonical(b0);
  Cmp c;
  c.v("info.title", a.info.title, b.info.title);
  c.v("info.author", a.info.author, b.info.author);
  c.v("info.genre", a.info.genre, b.info.genre);
  c.v("info.url", a.info.url, b.info.url);
  c.v("info.comments", a.info.comments, b.info.comments);
  c.v("info.created", a.info.created, b.info.created);
  c.v("info.workTime", a.info.workTime, b.info.workTime);
  c.v("ppq", a.ppq, b.ppq);
  c.d("tempo", a.tempo, b.tempo);
  c.v("timeSig", a.timeSigNum * 100 + a.timeSigDen, b.timeSigNum * 100 + b.timeSigDen);
  c.f("swing", a.swing, b.swing);
  c.f("masterPitch", a.masterPitch, b.masterPitch);
  c.v("autoCrossfades", a.autoCrossfades, b.autoCrossfades);  // J-43
  c.v("channelGroups.size", a.channelGroups.size(), b.channelGroups.size());
  for (size_t i = 0; i < a.channelGroups.size() && i < b.channelGroups.size(); ++i) c.v("channelGroups[" + std::to_string(i) + "]", a.channelGroups[i], b.channelGroups[i]);
  c.v("channels.size", a.channels.size(), b.channels.size());
  for (size_t i = 0; i < a.channels.size() && i < b.channels.size(); ++i) {
    const Channel &x = a.channels[i], &y = b.channels[i];
    const std::string p = "channels[" + std::to_string(i) + "]";
    c.v(p + ".id", x.id, y.id);
    c.v(p + ".name", x.name, y.name);
    c.v(p + ".color", x.color, y.color);
    c.plugin(p + ".plugin", x.plugin, y.plugin);
    c.f(p + ".gain", x.gain, y.gain);
    c.f(p + ".pan", x.pan, y.pan);
    c.v(p + ".muted", x.muted, y.muted);
    c.v(p + ".pitch", x.pitch, y.pitch);
    c.v(p + ".fine", x.fine, y.fine);
    c.v(p + ".mixerTrack", x.mixerTrack, y.mixerTrack);
    c.v(p + ".noteFx.size", x.noteFx.size(), y.noteFx.size());
    for (size_t k = 0; k < x.noteFx.size() && k < y.noteFx.size(); ++k) c.plugin(p + ".noteFx[" + std::to_string(k) + "]", x.noteFx[k], y.noteFx[k]);
    c.f(p + ".swingMix", x.swingMix, y.swingMix);
    c.f(p + ".timeShift", x.timeShift, y.timeShift);
    c.v(p + ".polyphony", x.polyphony, y.polyphony);
    c.v(p + ".mono", x.mono, y.mono);
    c.v(p + ".porta", x.porta, y.porta);
    c.f(p + ".portaTime", x.portaTime, y.portaTime);
    c.v(p + ".cutGroup", x.cutGroup, y.cutGroup);
    c.v(p + ".cutBy", x.cutBy, y.cutBy);
    c.v(p + ".echo", x.echo == y.echo, true);
    c.v(p + ".layer.size", x.layer.size(), y.layer.size());
    c.v(p + ".layer", x.layer == y.layer, true);
    c.v(p + ".layerMode", x.layerMode, y.layerMode);
    c.v(p + ".group", x.group, y.group);
    c.v(p + ".audioClip", x.audioClip, y.audioClip);
  }
  c.v("patterns.size", a.patterns.size(), b.patterns.size());
  for (size_t i = 0; i < a.patterns.size() && i < b.patterns.size(); ++i) {
    const Pattern &x = a.patterns[i], &y = b.patterns[i];
    const std::string p = "patterns[" + std::to_string(i) + "]";
    c.v(p + ".id", x.id, y.id);
    c.v(p + ".name", x.name, y.name);
    c.v(p + ".color", x.color, y.color);
    c.v(p + ".bars", x.bars, y.bars);
    c.v(p + ".stepsPerBar", x.stepsPerBar, y.stepsPerBar);
    c.v(p + ".steps", x.steps == y.steps, true);
    c.v(p + ".stepProps", x.stepProps == y.stepProps, true);
    c.v(p + ".notes.channels", x.notes.size(), y.notes.size());
    for (const auto& [ch, ns] : x.notes) {
      auto it = y.notes.find(ch);
      if (it == y.notes.end()) {
        c.v<std::string>(p + ".notes[" + std::to_string(ch) + "]", "present", "missing");
        continue;
      }
      c.v(p + ".notes[" + std::to_string(ch) + "].size", ns.size(), it->second.size());
      for (size_t k = 0; k < ns.size() && k < it->second.size(); ++k)
        if (!(ns[k] == it->second[k])) {
          const Note &n = ns[k], &m = it->second[k];
          std::ostringstream os;
          os << n.start << "/" << n.length << "/" << n.pitch << "/" << n.velocity << "/" << n.pan << "/" << n.fine << "/" << n.release << "/"
             << n.modX << "/" << n.modY << "/" << n.color << "/" << n.slide;
          std::ostringstream ms;
          ms << m.start << "/" << m.length << "/" << m.pitch << "/" << m.velocity << "/" << m.pan << "/" << m.fine << "/" << m.release << "/"
             << m.modX << "/" << m.modY << "/" << m.color << "/" << m.slide;
          c.v(p + ".notes[" + std::to_string(ch) + "][" + std::to_string(k) + "]", os.str(), ms.str());
        }
    }
    c.v(p + ".automation.size", x.automation.size(), y.automation.size());
    for (size_t k = 0; k < x.automation.size() && k < y.automation.size(); ++k) {
      const std::string q = p + ".automation[" + std::to_string(k) + "]";
      c.v(q + ".target", (static_cast<uint64_t>(x.automation[k].owner) << 32) | x.automation[k].index,
          (static_cast<uint64_t>(y.automation[k].owner) << 32) | y.automation[k].index);
      c.points(q + ".points", x.automation[k].points, y.automation[k].points);
    }
  }
  c.clips("playlist", a.playlist, b.playlist);
  c.tracks("tracks", a.tracks, b.tracks);
  c.markers("markers", a.markers, b.markers);
  c.v("live.areaEnd", a.live.areaEnd, b.live.areaEnd);
  c.v("arrangements.size", a.arrangements.size(), b.arrangements.size());
  c.v("currentArrangement", a.currentArrangement, b.currentArrangement);
  for (size_t i = 0; i < a.arrangements.size() && i < b.arrangements.size(); ++i) {
    const std::string p = "arrangements[" + std::to_string(i) + "]";
    c.v(p + ".name", a.arrangements[i].name, b.arrangements[i].name);
    c.clips(p + ".playlist", a.arrangements[i].playlist, b.arrangements[i].playlist);
    c.tracks(p + ".tracks", a.arrangements[i].tracks, b.arrangements[i].tracks);
    c.markers(p + ".markers", a.arrangements[i].markers, b.arrangements[i].markers);
  }
  c.v("mixer.size", a.mixer.size(), b.mixer.size());
  for (size_t i = 0; i < a.mixer.size() && i < b.mixer.size(); ++i) {
    const MixerTrack &x = a.mixer[i], &y = b.mixer[i];
    const std::string p = "mixer[" + std::to_string(i) + "]";
    c.v(p + ".id", x.id, y.id);
    c.v(p + ".name", x.name, y.name);
    c.v(p + ".color", x.color, y.color);
    c.f(p + ".volume", x.volume, y.volume);
    c.f(p + ".pan", x.pan, y.pan);
    c.f(p + ".stereoSep", x.stereoSep, y.stereoSep);
    c.v(p + ".flags", x.muted * 1 + x.solo * 2 + x.invertPolarity * 4 + x.swapLR * 8, y.muted * 1 + y.solo * 2 + y.invertPolarity * 4 + y.swapLR * 8);
    for (int b = 0; b < 3; ++b) {
      c.f(p + ".eqGain[" + std::to_string(b) + "]", x.eqGain[static_cast<size_t>(b)], y.eqGain[static_cast<size_t>(b)]);
      c.f(p + ".eqFreq[" + std::to_string(b) + "]", x.eqFreq[static_cast<size_t>(b)], y.eqFreq[static_cast<size_t>(b)]);
      c.f(p + ".eqQ[" + std::to_string(b) + "]", x.eqQ[static_cast<size_t>(b)], y.eqQ[static_cast<size_t>(b)]);
    }
    for (size_t k = 0; k < x.slots.size() && k < y.slots.size(); ++k) {
      const std::string q = p + ".slots[" + std::to_string(k) + "]";
      c.plugin(q + ".plugin", x.slots[k].plugin, y.slots[k].plugin);
      c.v(q + ".enabled", x.slots[k].enabled, y.slots[k].enabled);
      c.f(q + ".mix", x.slots[k].mix, y.slots[k].mix);
    }
    c.v(p + ".sends.size", x.sends.size(), y.sends.size());
    for (size_t k = 0; k < x.sends.size() && k < y.sends.size(); ++k) {
      const std::string q = p + ".sends[" + std::to_string(k) + "]";
      c.v(q + ".to", x.sends[k].to, y.sends[k].to);
      c.f(q + ".level", x.sends[k].level, y.sends[k].level);
      c.v(q + ".sidechain", x.sends[k].sidechain, y.sends[k].sidechain);
    }
  }
  c.v("automations.size", a.automations.size(), b.automations.size());
  for (size_t i = 0; i < a.automations.size() && i < b.automations.size(); ++i) {
    const AutomationClip &x = a.automations[i], &y = b.automations[i];
    const std::string p = "automations[" + std::to_string(i) + "]";
    c.v(p + ".id", x.id, y.id);
    c.v(p + ".name", x.name, y.name);
    c.v(p + ".color", x.color, y.color);
    c.v(p + ".target", (static_cast<uint64_t>(x.owner) << 32) | x.index, (static_cast<uint64_t>(y.owner) << 32) | y.index);
    c.f(p + ".min", x.min, y.min);
    c.f(p + ".max", x.max, y.max);
    c.points(p + ".points", x.points, y.points);
  }
  c.v("links.size", a.links.size(), b.links.size());
  for (size_t i = 0; i < a.links.size() && i < b.links.size(); ++i) c.v("links[" + std::to_string(i) + "]", a.links[i] == b.links[i], true);
  c.v("modLinks.size", a.modLinks.size(), b.modLinks.size());
  for (size_t i = 0; i < a.modLinks.size() && i < b.modLinks.size(); ++i) c.v("modLinks[" + std::to_string(i) + "]", a.modLinks[i] == b.modLinks[i], true);
  if (!ignoreOrigin) c.v("flOrigin", a.flOrigin == b.flOrigin, true);
  return c.out;
}

bool roundTrip(const std::string& path, const ExportOptions& optIn, RoundTrip& rt, std::string* err) {
  Song s1;
  Report r1;
  if (!importFlp(path, s1, r1, err, optIn.factoryData)) return false;
  ExportOptions opt = optIn;
  if (opt.projectDir.empty()) opt.projectDir = std::filesystem::path(path).parent_path().string();
  if (!exportFlp(s1, rt.exported, rt.report, opt)) {
    if (err) *err = "export failed";
    return false;
  }
  Song s2;
  Report r2;
  if (!importFlpBytes(rt.exported, s2, r2, err, opt.factoryData, opt.projectDir, false)) return false;
  rt.songDiffs = compareSongs(s1, s2, true);
  rt.original = s1;
  rt.reimported = s2;
  for (const auto& d : rt.songDiffs) {
    std::string cat = d.substr(0, d.find_first_of(".[:"));
    rt.songDiffCounts[cat]++;
  }
  // Event streams.
  File f1, f2;
  if (!readFile(path, f1, err) || !readBytes(rt.exported, f2, err)) return false;
  rt.originalEvents = static_cast<int>(f1.events.size());
  rt.exportedEvents = static_cast<int>(f2.events.size());
  std::map<std::pair<int, std::string>, int> pool;
  for (const Event& e : f1.events) {
    pool[{e.id, std::string(reinterpret_cast<const char*>(e.data), e.size)}]++;
    rt.events[e.id].original++;
  }
  for (const Event& e : f2.events) {
    auto& d = rt.events[e.id];
    d.exported++;
    auto it = pool.find({e.id, std::string(reinterpret_cast<const char*>(e.data), e.size)});
    if (it != pool.end() && it->second > 0) {
      --it->second;
      d.same++;
      rt.identicalEvents++;
    } else {
      d.changed++;
    }
  }
  rt.bytesIdentical = f1.bytes == f2.bytes;
  for (const auto& p : rt.report.plugins) {
    ++rt.plugins;
    if (p.quality == "verbatim") ++rt.pluginsVerbatim;
    else if (p.quality == "exact") ++rt.pluginsExact;
    else if (p.quality == "approx") ++rt.pluginsApprox;
    else if (p.quality == "defaults") ++rt.pluginsDefaults;
    else ++rt.pluginsNone;
  }
  return true;
}

std::string roundTripText(const RoundTrip& rt) {
  std::ostringstream os;
  os << "song differences: " << rt.songDiffs.size() << "\n";
  for (const auto& [k, v] : rt.songDiffCounts) os << "  " << k << ": " << v << "\n";
  for (size_t i = 0; i < rt.songDiffs.size(); ++i) os << "    " << rt.songDiffs[i] << "\n";
  os << "events: original " << rt.originalEvents << ", exported " << rt.exportedEvents << ", identical " << rt.identicalEvents
     << (rt.bytesIdentical ? " (file byte-identical)" : "") << "\n";
  for (const auto& [id, d] : rt.events)
    if (d.original != d.same || d.exported != d.same)
      os << "  id " << id << ": original " << d.original << ", exported " << d.exported << ", identical " << d.same << "\n";
  os << "plugins: " << rt.plugins << " (verbatim " << rt.pluginsVerbatim << ", exact " << rt.pluginsExact << ", approx " << rt.pluginsApprox
     << ", defaults " << rt.pluginsDefaults << ", placeholder/none " << rt.pluginsNone << ")\n";
  os << reportText(rt.report);
  return os.str();
}

std::string roundTripJson(const RoundTrip& rt) {
  auto esc = [](const std::string& s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\') o += '\\', o += c;
      else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
      else o += c;
    }
    return o;
  };
  std::ostringstream os;
  os << "{\"songDiffs\":" << rt.songDiffs.size() << ",\"songDiffCounts\":{";
  bool first = true;
  for (const auto& [k, v] : rt.songDiffCounts) os << (first ? "" : ",") << "\"" << esc(k) << "\":" << v, first = false;
  os << "},\"diffSamples\":[";
  for (size_t i = 0; i < rt.songDiffs.size() && i < 40; ++i) os << (i ? "," : "") << "\"" << esc(rt.songDiffs[i]) << "\"";
  os << "],\"originalEvents\":" << rt.originalEvents << ",\"exportedEvents\":" << rt.exportedEvents << ",\"identicalEvents\":" << rt.identicalEvents
     << ",\"bytesIdentical\":" << (rt.bytesIdentical ? "true" : "false") << ",\"events\":{";
  first = true;
  for (const auto& [id, d] : rt.events)
    os << (first ? "" : ",") << "\"" << id << "\":[" << d.original << "," << d.exported << "," << d.same << "]", first = false;
  os << "},\"plugins\":[" << rt.plugins << "," << rt.pluginsVerbatim << "," << rt.pluginsExact << "," << rt.pluginsApprox << "," << rt.pluginsDefaults
     << "," << rt.pluginsNone << "],\"pluginDetails\":[";
  first = true;
  for (const auto& p : rt.report.plugins)
    if (p.quality != "verbatim" && p.quality != "exact")
      os << (first ? "" : ",") << "[\"" << esc(p.flName) << "\",\"" << esc(p.glType) << "\",\"" << p.quality << "\",\"" << esc(p.note) << "\"]", first = false;
  os << "],\"warnings\":[";
  for (size_t i = 0; i < rt.report.warnings.size() && i < 50; ++i) os << (i ? "," : "") << "\"" << esc(rt.report.warnings[i]) << "\"";
  os << "]}";
  return os.str();
}

}  // namespace gl::flp
