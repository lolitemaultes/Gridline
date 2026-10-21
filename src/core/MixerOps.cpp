// Gridline — mixer track operations (J-12). See MixerOps.h.
#include "MixerOps.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include "Json.h"
#include "Params.h"
#include "Project.h"

namespace gl::mixops {

namespace {

std::string trackLabel(const Song& s, ObjectId id) {
  int i = s.mixerIndex(id);
  return i < 0 ? std::string("?") : mixerTrackName(s, static_cast<size_t>(i));
}

const Send* findSend(const MixerTrack& t, ObjectId to) {
  for (const auto& sd : t.sends)
    if (sd.to == to) return &sd;
  return nullptr;
}

// Applies `fn` to each listed track as one undo step; true if any changed.
bool editEach(Document& d, const std::vector<ObjectId>& tracks, const std::string& label,
              const std::function<void(MixerTrack&)>& fn) {
  bool any = false;
  d.beginGroup(label);
  for (ObjectId id : tracks) {
    const MixerTrack* t = d.song().mixerTrack(id);
    if (!t) continue;
    MixerTrack copy = *t;
    fn(copy);
    if (copy == *t) continue;
    any |= d.editMixerTrack(id, label, fn);
  }
  d.endGroup();
  return any;
}

Rgb lerpRgb(Rgb a, Rgb b, double t) {
  auto ch = [&](int sh) {
    double x = ((a >> sh) & 0xFF) + (static_cast<double>((b >> sh) & 0xFF) - ((a >> sh) & 0xFF)) * t;
    return static_cast<Rgb>(std::clamp(static_cast<int>(std::lround(x)), 0, 255)) << sh;
  };
  return ch(16) | ch(8) | ch(0);
}

// True when (owner, index) addresses a parameter of slot `slot` on `track`
// (including its mix and enable switches).
bool slotParamOf(ObjectId owner, uint32_t index, ObjectId track, int& slot) {
  if (owner != track || index < kSlotBase) return false;
  slot = static_cast<int>((index - kSlotBase) / kSlotStride);
  return slot < kEffectSlots;
}

}  // namespace

// ---------------------------------------------------------------- routing ----

std::vector<ObjectId> cyclePath(const Song& s, ObjectId from, ObjectId to) {
  if (!s.mixerTrack(from) || !s.mixerTrack(to)) return {};
  if (from == to) return {from};
  // Depth-first search for a route chain to → … → from.
  std::vector<ObjectId> path;
  std::set<ObjectId> seen;
  std::function<bool(ObjectId)> dfs = [&](ObjectId x) {
    path.push_back(x);
    if (x == from) return true;
    if (seen.insert(x).second)
      if (const MixerTrack* t = s.mixerTrack(x))
        for (const auto& sd : t->sends)
          if (dfs(sd.to)) return true;
    path.pop_back();
    return false;
  };
  return dfs(to) ? path : std::vector<ObjectId>{};
}

std::string routeProblem(const Song& s, ObjectId from, ObjectId to) {
  if (!s.mixerTrack(from) || !s.mixerTrack(to)) return "That track no longer exists";
  if (from == to) return "A track can't route into itself";
  if (from == s.masterId()) return "The master feeds the audio output only";
  if (const MixerTrack* t = s.mixerTrack(from); t && findSend(*t, to)) return "";
  std::vector<ObjectId> loop = cyclePath(s, from, to);
  if (loop.empty()) return "";
  std::string msg = "Routing " + trackLabel(s, from) + " to " + trackLabel(s, to) + " would make a feedback loop: ";
  msg += trackLabel(s, from);
  for (ObjectId id : loop) msg += " \xE2\x86\x92 " + trackLabel(s, id);
  return msg;
}

int route(Document& d, const std::vector<ObjectId>& from, const std::vector<ObjectId>& to, RouteMode mode, std::string* err) {
  static const char* labels[] = {"route mixer tracks", "route mixer tracks only", "sidechain mixer tracks",
                                 "sidechain mixer tracks only"};
  const char* label = labels[static_cast<int>(mode)];
  int made = 0;
  bool reported = false;
  d.beginGroup(label);
  for (ObjectId f : from)
    for (ObjectId t : to) {
      if (f == t) continue;
      std::string why = routeProblem(d.song(), f, t);
      if (!why.empty()) {
        if (err && !reported) *err = why;
        reported = true;
        continue;
      }
      const bool sc = mode == RouteMode::Sidechain || mode == RouteMode::SidechainOnly;
      auto inCall = [&](ObjectId x) { return std::find(to.begin(), to.end(), x) != to.end(); };
      const bool ok = d.editMixerTrack(f, label, [&](MixerTrack& m) {
        // "Only" keeps nothing but the destinations of this call (so several
        // destinations can be routed "only" together); "sidechain only" does
        // the same for the sidechain routes.
        m.sends.erase(std::remove_if(m.sends.begin(), m.sends.end(),
                                     [&](const Send& x) {
                                       if (inCall(x.to)) return false;
                                       return mode == RouteMode::Only || (mode == RouteMode::SidechainOnly && x.sidechain);
                                     }),
                      m.sends.end());
        auto it = std::find_if(m.sends.begin(), m.sends.end(), [&](const Send& x) { return x.to == t; });
        if (it == m.sends.end())
          m.sends.push_back({t, 1.0f, sc, false});
        else
          it->sidechain = sc;
      });
      made += ok ? 1 : 0;
    }
  d.endGroup();
  return made;
}

ObjectId createSubmix(Document& d, const std::vector<ObjectId>& tracks, const std::string& name) {
  const Song& s = d.song();
  int last = 0;
  for (ObjectId id : tracks) last = std::max(last, s.mixerIndex(id));
  if (last < 1 || static_cast<int>(s.mixer.size()) > kMaxMixerInserts) return kNoId;
  d.beginGroup("create submix");
  ObjectId sub = insertTracks(d, last + 1, 1).front();
  d.editMixerTrack(sub, "create submix", [&](MixerTrack& m) { m.name = name; });
  std::vector<ObjectId> srcs;
  for (ObjectId id : tracks)
    if (id != d.song().masterId() && d.song().mixerTrack(id)) srcs.push_back(id);
  int n = route(d, srcs, {sub}, RouteMode::Only);
  d.endGroup();
  if (n == 0) {
    d.undo();
    return kNoId;
  }
  return sub;
}

int resetRouting(Document& d, const std::vector<ObjectId>& tracks) {
  const ObjectId m = d.song().masterId();
  int n = 0;
  d.beginGroup("reset mixer routing");
  for (ObjectId id : tracks) {
    const MixerTrack* t = d.song().mixerTrack(id);
    if (!t || id == m) continue;
    if (t->sends.size() == 1 && t->sends[0].to == m && !t->sends[0].sidechain && !t->sends[0].preFader &&
        t->sends[0].level == 1.0f)
      continue;
    n += d.editMixerTrack(id, "reset mixer routing", [&](MixerTrack& x) { x.sends = {{m, 1.0f, false, false}}; }) ? 1 : 0;
  }
  d.endGroup();
  return n;
}

int unrouteIncoming(Document& d, ObjectId dest) {
  const ObjectId m = d.song().masterId();
  if (dest == m || !d.song().mixerTrack(dest)) return 0;  // the master's inputs are never unrouted
  std::vector<ObjectId> srcs = routedTo(d.song(), dest);
  int n = 0;
  d.beginGroup("unroute tracks");
  for (ObjectId id : srcs)
    n += d.editMixerTrack(id, "unroute tracks", [&](MixerTrack& x) {
      x.sends.erase(std::remove_if(x.sends.begin(), x.sends.end(), [&](const Send& sd) { return sd.to == dest; }), x.sends.end());
      if (x.sends.empty() && dest != m) x.sends.push_back({m, 1.0f, false, false});
    }) ? 1 : 0;
  d.endGroup();
  return n;
}

bool setSendPreFader(Document& d, ObjectId from, ObjectId to, bool pre) {
  const MixerTrack* t = d.song().mixerTrack(from);
  const Send* sd = t ? findSend(*t, to) : nullptr;
  if (!sd || sd->preFader == pre) return false;
  return d.editMixerTrack(from, pre ? "pre-fader send" : "post-fader send", [&](MixerTrack& m) {
    for (auto& x : m.sends)
      if (x.to == to) x.preFader = pre;
  });
}

std::vector<ObjectId> routedFrom(const Song& s, ObjectId track) {
  std::vector<ObjectId> out;
  const MixerTrack* t = s.mixerTrack(track);
  if (!t) return out;
  for (const auto& m : s.mixer)
    if (findSend(*t, m.id)) out.push_back(m.id);
  return out;
}

std::vector<ObjectId> routedTo(const Song& s, ObjectId track) {
  std::vector<ObjectId> out;
  for (const auto& m : s.mixer)
    if (findSend(m, track)) out.push_back(m.id);
  return out;
}

double sendGain(const Song& s, ObjectId from, ObjectId to) {
  const MixerTrack* t = s.mixerTrack(from);
  const Send* sd = t ? findSend(*t, to) : nullptr;
  if (!sd) return 0.0;
  if (sd->preFader) return sd->level;
  const double x = t->volume / 0.8;  // fader taper (dsp::faderGain) and balance pan law, left channel
  const double l = t->pan > 0 ? 1.0 - t->pan : 1.0;
  return sd->level * x * x * l;
}

RouteLayout routeLayout(const Song& s) {
  RouteLayout L;
  const size_t n = s.mixer.size();
  L.column.assign(n, 0);
  for (size_t i = 0; i < n; ++i)
    for (const auto& sd : s.mixer[i].sends) {
      int j = s.mixerIndex(sd.to);
      if (j >= 0) L.edges.push_back({static_cast<int>(i), j, sd.level, sd.sidechain, sd.preFader});
    }
  // Longest path from the sources, in processing order (sources first).
  for (ObjectId id : mixerOrder(s)) {
    int i = s.mixerIndex(id);
    if (i < 0) continue;
    for (const auto& sd : s.mixer[static_cast<size_t>(i)].sends) {
      int j = s.mixerIndex(sd.to);
      if (j >= 0) L.column[static_cast<size_t>(j)] = std::max(L.column[static_cast<size_t>(j)], L.column[static_cast<size_t>(i)] + 1);
    }
  }
  int maxCol = 0;
  for (size_t i = 1; i < n; ++i) maxCol = std::max(maxCol, L.column[i]);
  if (n) L.column[0] = std::max(L.column[0], maxCol + 1);  // the master is always rightmost
  L.columns = n ? L.column[0] + 1 : 1;
  return L;
}

// ------------------------------------------------------------------ strip ----

bool setMono(Document& d, const std::vector<ObjectId>& tracks, bool mono) {
  return editEach(d, tracks, mono ? "make track mono" : "make track stereo",
                  [mono](MixerTrack& m) { m.stereoSep = mono ? -1.0f : 0.0f; });
}

bool setSwap(Document& d, const std::vector<ObjectId>& tracks, bool swap) {
  return editEach(d, tracks, "swap left and right", [swap](MixerTrack& m) { m.swapLR = swap; });
}

bool setPolarity(Document& d, const std::vector<ObjectId>& tracks, bool invert) {
  return editEach(d, tracks, "reverse polarity", [invert](MixerTrack& m) { m.invertPolarity = invert; });
}

bool setDisabled(Document& d, const std::vector<ObjectId>& tracks, bool disabled) {
  return editEach(d, tracks, disabled ? "disable mixer track" : "enable mixer track",
                  [disabled](MixerTrack& m) { m.disabled = disabled; });
}

bool setThreaded(Document& d, const std::vector<ObjectId>& tracks, bool threaded) {
  return editEach(d, tracks, "threaded processing", [threaded](MixerTrack& m) { m.threaded = threaded; });
}

bool setLatencyOffset(Document& d, ObjectId track, int samples) {
  samples = std::clamp(samples, 0, kMaxManualLatency);
  return editEach(d, {track}, "set track latency", [samples](MixerTrack& m) { m.latencyOffset = samples; });
}

int resetManualLatency(Document& d) {
  std::vector<ObjectId> ids;
  for (const auto& m : d.song().mixer)
    if (m.latencyOffset) ids.push_back(m.id);
  editEach(d, ids, "reset manual latency", [](MixerTrack& m) { m.latencyOffset = 0; });
  return static_cast<int>(ids.size());
}

int latencyFromMs(double ms, double sampleRate) {
  return std::clamp(static_cast<int>(std::lround(ms * sampleRate / 1000.0)), 0, kMaxManualLatency);
}

int latencyFromBeats(double beats, double tempo, double sampleRate) {
  return tempo > 0 ? latencyFromMs(beats * 60000.0 / tempo, sampleRate) : 0;
}

int resetTracks(Document& d, const std::vector<ObjectId>& tracks) {
  int n = 0;
  d.beginGroup("reset mixer tracks");
  for (ObjectId id : tracks)
    n += editEach(d, {id}, "reset mixer track", [](MixerTrack& m) {
      const MixerTrack def;
      m.volume = def.volume, m.pan = def.pan, m.stereoSep = def.stereoSep;
      m.muted = def.muted, m.solo = def.solo, m.invertPolarity = def.invertPolarity, m.swapLR = def.swapLR;
      m.disabled = def.disabled, m.latencyOffset = def.latencyOffset, m.threaded = def.threaded;
      m.eqGain = def.eqGain, m.eqFreq = def.eqFreq, m.eqQ = def.eqQ;
    }) ? 1 : 0;
  d.endGroup();
  return n;
}

// -------------------------------------------------- layout, groups, looks ----

bool setDock(Document& d, const std::vector<ObjectId>& tracks, MixerDock dock) {
  std::vector<ObjectId> ids;
  for (ObjectId id : tracks)
    if (id != d.song().masterId()) ids.push_back(id);
  return editEach(d, ids, "dock mixer track", [dock](MixerTrack& m) { m.dock = static_cast<int>(dock); });
}

bool setIcon(Document& d, const std::vector<ObjectId>& tracks, int icon) {
  return editEach(d, tracks, "change mixer track icon", [icon](MixerTrack& m) { m.icon = std::clamp(icon, 0, 255); });
}

bool setColor(Document& d, const std::vector<ObjectId>& tracks, Rgb color) {
  return editEach(d, tracks, "change mixer track colour", [color](MixerTrack& m) { m.color = color; });
}

bool gradientColor(Document& d, const std::vector<ObjectId>& tracks) {
  std::vector<ObjectId> order = inMixerOrder(d.song(), std::set<ObjectId>(tracks.begin(), tracks.end()));
  if (order.size() < 3) return false;
  const Rgb a = d.song().mixerTrack(order.front())->color, b = d.song().mixerTrack(order.back())->color;
  d.beginGroup("gradient colour");
  for (size_t i = 0; i < order.size(); ++i) {
    Rgb c = lerpRgb(a, b, static_cast<double>(i) / static_cast<double>(order.size() - 1));
    d.editMixerTrack(order[i], "gradient colour", [c](MixerTrack& m) { m.color = c; });
  }
  d.endGroup();
  return true;
}

bool setGroup(Document& d, const std::vector<ObjectId>& tracks, const std::string& group) {
  return editEach(d, tracks, group.empty() ? "ungroup mixer tracks" : "group mixer tracks",
                  [&group](MixerTrack& m) { m.group = group; });
}

std::vector<std::string> groupNames(const Song& s) {
  std::vector<std::string> out;
  for (const auto& m : s.mixer)
    if (!m.group.empty() && std::find(out.begin(), out.end(), m.group) == out.end()) out.push_back(m.group);
  return out;
}

std::vector<ObjectId> groupMembers(const Song& s, const std::string& group) {
  std::vector<ObjectId> out;
  if (group.empty()) return out;
  for (const auto& m : s.mixer)
    if (m.group == group) out.push_back(m.id);
  return out;
}

std::string newGroupName(const Song& s) {
  std::vector<std::string> used = groupNames(s);
  for (int k = 1;; ++k) {
    std::string n = "Group " + std::to_string(k);
    if (std::find(used.begin(), used.end(), n) == used.end()) return n;
  }
}

std::vector<size_t> displayOrder(const Song& s) {
  std::vector<size_t> out;
  if (s.mixer.empty()) return out;
  out.push_back(0);
  for (int dock : {-1, 0, 1})
    for (size_t i = 1; i < s.mixer.size(); ++i)
      if (std::clamp(s.mixer[i].dock, -1, 1) == dock) out.push_back(i);
  return out;
}

std::vector<ObjectId> inMixerOrder(const Song& s, const std::set<ObjectId>& tracks) {
  std::vector<ObjectId> out;
  for (const auto& m : s.mixer)
    if (tracks.count(m.id)) out.push_back(m.id);
  return out;
}

// ----------------------------------------------------------------- tracks ----

std::vector<ObjectId> insertTracks(Document& d, int index, int count) {
  std::vector<ObjectId> out;
  const Song& s = d.song();
  if (s.mixer.empty() || count < 1) return out;
  count = std::min(count, kMaxMixerInserts + 1 - static_cast<int>(s.mixer.size()));
  if (count < 1) return out;
  index = std::clamp(index, 1, static_cast<int>(s.mixer.size()));
  d.editSong(count == 1 ? "insert mixer track" : "insert mixer tracks", [&](Song& x) {
    for (int k = 0; k < count; ++k) {
      MixerTrack t;
      t.id = x.allocateId();
      t.sends.push_back({x.masterId(), 1.0f, false, false});
      x.mixer.insert(x.mixer.begin() + index + k, t);
      out.push_back(t.id);
    }
  });
  return out;
}

int growTo(Document& d, int inserts) {
  inserts = std::min(inserts, kMaxMixerInserts);
  const int have = static_cast<int>(d.song().mixer.size()) - 1;
  if (inserts <= have) return 0;
  return static_cast<int>(insertTracks(d, have + 1, inserts - have).size());
}

// ------------------------------------------------------------------ slots ----

int stepSlot(Document& d, ObjectId track, int slot, int steps) {
  const MixerTrack* t = d.song().mixerTrack(track);
  if (!t || slot < 0 || slot >= kEffectSlots || t->slots[static_cast<size_t>(slot)].plugin.empty()) return slot;
  const int to = std::clamp(slot + steps, 0, kEffectSlots - 1);
  if (to == slot) return slot;
  d.endGesture();  // each wheel notch is its own undo step
  return d.moveSlot(track, slot, to) ? to : slot;
}

bool moveSlotTo(Document& d, ObjectId fromTrack, int fromSlot, ObjectId toTrack, int toSlot, bool copy) {
  const Song& s = d.song();
  const MixerTrack* src = s.mixerTrack(fromTrack);
  const MixerTrack* dst = s.mixerTrack(toTrack);
  if (!src || !dst || fromSlot < 0 || toSlot < 0 || fromSlot >= kEffectSlots || toSlot >= kEffectSlots) return false;
  if (src->slots[static_cast<size_t>(fromSlot)].plugin.empty()) return false;
  if (fromTrack == toTrack && !copy) return fromSlot != toSlot && d.moveSlot(fromTrack, fromSlot, toSlot);
  // Target: free slot, or shift the slots from toSlot down when the last one is free.
  const bool shift = !dst->slots[static_cast<size_t>(toSlot)].plugin.empty();
  if (shift && !dst->slots.back().plugin.empty()) return false;
  if (fromTrack == toTrack && shift) return false;  // copying onto a taken slot of the same track
  d.editSong(copy ? "copy effect" : "move effect", [&](Song& x) {
    MixerTrack* a = x.mixerTrack(fromTrack);
    MixerTrack* b = x.mixerTrack(toTrack);
    EffectSlot moved = a->slots[static_cast<size_t>(fromSlot)];
    if (!copy) a->slots[static_cast<size_t>(fromSlot)] = EffectSlot{};
    if (shift) {
      b->slots.pop_back();
      b->slots.insert(b->slots.begin() + toSlot, moved);
    } else {
      b->slots[static_cast<size_t>(toSlot)] = moved;
    }
    // Parameter references follow: shifted slots of the target track, and
    // (when moving) the moved effect itself.
    auto remap = [&](ObjectId& owner, uint32_t& index) {
      int sl = 0;
      if (!copy && slotParamOf(owner, index, fromTrack, sl) && sl == fromSlot) {
        owner = toTrack;
        index = kSlotBase + static_cast<uint32_t>(toSlot) * kSlotStride + (index - kSlotBase) % kSlotStride;
        return;
      }
      if (shift && slotParamOf(owner, index, toTrack, sl) && sl >= toSlot)
        index = kSlotBase + static_cast<uint32_t>(sl + 1) * kSlotStride + (index - kSlotBase) % kSlotStride;
    };
    for (auto& l : x.modLinks) {
      remap(l.owner, l.index);
      if (!copy && l.sourceOwner == fromTrack && l.sourceSlot == fromSlot) {
        l.sourceOwner = toTrack;
        l.sourceSlot = toSlot;
      } else if (shift && l.sourceOwner == toTrack && l.sourceSlot >= toSlot) {
        ++l.sourceSlot;
      }
    }
    for (auto& l : x.links) remap(l.owner, l.index);
    for (auto& au : x.automations) remap(au.owner, au.index);
  });
  return true;
}

// ------------------------------------------------------------ track state ----

std::string trackStateText(const MixerTrack& t) {
  Json j = mixerTrackToJson(t);
  j.object.erase("id");
  j.object.erase("sends");
  j.object.erase("solo");
  j.object.erase("dock");
  j.object.erase("group");
  j.object.erase("armed");
  j.object["format"] = Json::makeString(kTrackStateTag);
  j.object["version"] = Json::makeNumber(1);
  return jsonStringify(j) + "\n";
}

bool applyTrackState(Document& d, ObjectId track, const std::string& text, std::string* err) {
  const MixerTrack* cur = d.song().mixerTrack(track);
  if (!cur) {
    if (err) *err = "No such mixer track";
    return false;
  }
  Json j;
  if (!jsonParse(text, j) || j.text("format") != kTrackStateTag) {
    if (err) *err = "Not a mixer track state file";
    return false;
  }
  MixerTrack loaded;
  mixerTrackFromJson(j, loaded);
  loaded.id = cur->id;
  loaded.sends = cur->sends;
  loaded.solo = cur->solo;
  loaded.dock = cur->dock;
  loaded.group = cur->group;
  loaded.armed = cur->armed;
  d.beginGroup("load mixer track state");
  for (int k = 0; k < kEffectSlots; ++k)  // links into the replaced effects no longer apply
    if (cur->slots[static_cast<size_t>(k)].plugin.type != loaded.slots[static_cast<size_t>(k)].plugin.type)
      d.setSlotPlugin(track, k, "");
  bool ok = d.editMixerTrack(track, "load mixer track state", [&](MixerTrack& m) { m = loaded; });
  d.endGroup();
  return ok;
}

bool saveTrackState(const std::string& path, const MixerTrack& t, std::string* err) {
  return writeFileAtomic(path, trackStateText(t), err);
}

bool loadTrackState(Document& d, ObjectId track, const std::string& path, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "Cannot read " + path;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return applyTrackState(d, track, ss.str(), err);
}

// ------------------------------------------------------------------ stems ----

Song stemSong(const Song& s, ObjectId track) {
  Song x = s;
  const ObjectId master = x.masterId();
  if (track == master || !x.mixerTrack(track)) return x;
  // Everything that feeds `track` (by sends, sidechains or send effects).
  std::set<ObjectId> feeds{track};
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto& m : x.mixer) {
      if (feeds.count(m.id)) continue;
      bool in = false;
      for (const auto& sd : m.sends) in = in || feeds.count(sd.to);
      for (const auto& sl : m.slots)  // send effects: parameter 0 is the destination track number
        if (sl.plugin.type == "gl.send" && !sl.plugin.params.empty()) {
          int to = static_cast<int>(std::lround(sl.plugin.params[0]));
          in = in || (to >= 0 && static_cast<size_t>(to) < x.mixer.size() && feeds.count(x.mixer[static_cast<size_t>(to)].id));
        }
      if (in) grew = feeds.insert(m.id).second || grew;
    }
  }
  for (auto& m : x.mixer) {
    m.solo = false;
    if (m.id == master) {  // neutral master: unity, no effects, no EQ
      MixerTrack def;
      m.slots.assign(kEffectSlots, EffectSlot{});
      m.volume = def.volume, m.pan = def.pan, m.stereoSep = def.stereoSep;
      m.muted = false, m.invertPolarity = false, m.swapLR = false, m.disabled = false, m.latencyOffset = 0;
      m.eqGain = def.eqGain;
    } else if (m.id == track) {
      m.sends = {{master, 1.0f, false, false}};
    } else if (!feeds.count(m.id)) {
      m.muted = true;
    }
  }
  // J-20: only the stem's own signal. Tracks feeding it keep just the routes
  // that lead into it (a direct send to the master would leak), and channels
  // that play into tracks outside the stem — or straight into the master —
  // are silent.
  for (auto& m : x.mixer)
    if (m.id != master && m.id != track && feeds.count(m.id))
      m.sends.erase(std::remove_if(m.sends.begin(), m.sends.end(), [&](const Send& sd) { return !feeds.count(sd.to); }), m.sends.end());
  for (auto& c : x.channels)
    if (!feeds.count(c.mixerTrack == kNoId ? master : c.mixerTrack)) c.muted = true;
  // Links and automation that drive the master would colour every stem.
  x.modLinks.erase(std::remove_if(x.modLinks.begin(), x.modLinks.end(), [&](const ModLink& l) { return l.owner == master; }),
                   x.modLinks.end());
  return x;
}

std::vector<StemJob> stemJobs(const Song& s, const std::vector<ObjectId>& tracks, const std::string& dir, const std::string& base) {
  std::vector<StemJob> out;
  std::set<std::string> used;
  auto safe = [](std::string n) {
    for (char& c : n)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.')) c = '_';
    while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
    return n.empty() ? std::string("track") : n;
  };
  for (ObjectId id : inMixerOrder(s, std::set<ObjectId>(tracks.begin(), tracks.end()))) {
    const int i = s.mixerIndex(id);
    StemJob j;
    j.track = id;
    j.name = mixerTrackName(s, static_cast<size_t>(i));
    char num[8];
    std::snprintf(num, sizeof num, "%02d", i);
    std::string stem = (base.empty() ? std::string() : safe(base) + "_") + (i == 0 ? std::string("Master") : std::string(num) + "_" + safe(j.name));
    std::string f = stem;
    for (int k = 2; used.count(f); ++k) f = stem + "_" + std::to_string(k);
    used.insert(f);
    j.file = (dir.empty() ? std::string() : dir + "/") + f + ".wav";
    out.push_back(std::move(j));
  }
  return out;
}

}  // namespace gl::mixops
