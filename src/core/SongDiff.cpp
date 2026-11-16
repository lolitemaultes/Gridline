// Gridline — K-92: song differences for people. See SongDiff.h.
#include "SongDiff.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "PluginInfo.h"
#include "Platform.h"

namespace gl {

namespace {

std::string num(double v) { return platform::formatDouble("%.6g", v); }
std::string q(const std::string& s) { return "'" + s + "'"; }

std::string mixName(const Song& s, const MixerTrack& m) {
  if (m.id == s.masterId()) return "Master";
  if (!m.name.empty()) return m.name;
  for (size_t i = 0; i < s.mixer.size(); ++i)
    if (s.mixer[i].id == m.id) return "Insert " + std::to_string(i);
  return "Insert";
}

std::string plugName(const std::string& type) {
  if (type.empty()) return "nothing";
  const PluginInfo* p = findPlugin(type);
  return p ? p->name : type;
}

struct Lines {
  std::vector<std::string> v;
  size_t max;
  size_t dropped = 0;
  void add(const std::string& l) {
    if (v.size() < max) v.push_back(l);
    else ++dropped;
  }
};

// Notes compared as a multiset: added and removed counts.
std::pair<int, int> noteDelta(const std::vector<Note>& a, const std::vector<Note>& b) {
  std::vector<bool> used(b.size(), false);
  int removed = 0;
  for (const Note& n : a) {
    bool found = false;
    for (size_t i = 0; i < b.size() && !found; ++i)
      if (!used[i] && b[i] == n) used[i] = true, found = true;
    if (!found) ++removed;
  }
  int added = 0;
  for (bool u : used)
    if (!u) ++added;
  return {added, removed};
}

std::string chName(const Song& a, const Song& b, ObjectId id) {
  if (const Channel* c = b.channel(id)) return c->name;
  if (const Channel* c = a.channel(id)) return c->name;
  return "?";
}

}  // namespace

std::vector<std::string> songDiff(const Song& a, const Song& b, size_t maxLines) {
  Lines L{{}, maxLines};
  if (a.tempo != b.tempo) L.add("Tempo " + num(a.tempo) + " -> " + num(b.tempo));
  if (a.timeSigNum != b.timeSigNum || a.timeSigDen != b.timeSigDen)
    L.add("Meter " + std::to_string(a.timeSigNum) + "/" + std::to_string(a.timeSigDen) + " -> " + std::to_string(b.timeSigNum) + "/" + std::to_string(b.timeSigDen));
  if (a.swing != b.swing) L.add("Swing " + num(a.swing) + " -> " + num(b.swing));
  if (a.masterPitch != b.masterPitch) L.add("Master pitch " + num(a.masterPitch) + " -> " + num(b.masterPitch));
  if (!(a.info == b.info)) L.add("Project info changed");
  // Channels.
  for (const Channel& c : a.channels)
    if (!b.channel(c.id)) L.add("Channel " + q(c.name) + " removed");
  for (const Channel& c : b.channels) {
    const Channel* o = a.channel(c.id);
    if (!o) {
      L.add("Channel " + q(c.name) + " added (" + plugName(c.plugin.type) + ")");
      continue;
    }
    if (o->name != c.name) L.add("Channel " + q(o->name) + " renamed " + q(c.name));
    if (o->plugin.type != c.plugin.type) L.add("Channel " + q(c.name) + ": instrument " + plugName(o->plugin.type) + " -> " + plugName(c.plugin.type));
    else if (!(o->plugin == c.plugin)) {
      int n = 0;
      for (size_t i = 0; i < std::min(o->plugin.params.size(), c.plugin.params.size()); ++i) n += o->plugin.params[i] != c.plugin.params[i];
      L.add("Channel " + q(c.name) + ": " + (n ? std::to_string(n) + " instrument settings changed" : std::string("instrument changed")));
    }
    if (o->gain != c.gain) L.add("Channel " + q(c.name) + ": volume " + num(o->gain) + " -> " + num(c.gain));
    if (o->pan != c.pan) L.add("Channel " + q(c.name) + ": pan " + num(o->pan) + " -> " + num(c.pan));
    if (o->muted != c.muted) L.add("Channel " + q(c.name) + (c.muted ? ": muted" : ": unmuted"));
    if (o->mixerTrack != c.mixerTrack) L.add("Channel " + q(c.name) + ": routed to another mixer track");
    if (o->noteFx != c.noteFx) L.add("Channel " + q(c.name) + ": note effects changed");
  }
  // Mixer.
  for (const MixerTrack& m : a.mixer)
    if (!b.mixerTrack(m.id)) L.add("Mixer track " + q(mixName(a, m)) + " removed");
  for (const MixerTrack& m : b.mixer) {
    const MixerTrack* o = a.mixerTrack(m.id);
    if (!o) {
      L.add("Mixer track " + q(mixName(b, m)) + " added");
      continue;
    }
    const std::string n = q(mixName(b, m));
    if (o->name != m.name) L.add("Mixer track " + q(mixName(a, *o)) + " renamed " + n);
    if (o->volume != m.volume) L.add("Mixer track " + n + ": volume " + num(o->volume) + " -> " + num(m.volume));
    if (o->pan != m.pan) L.add("Mixer track " + n + ": pan " + num(o->pan) + " -> " + num(m.pan));
    if (o->muted != m.muted) L.add("Mixer track " + n + (m.muted ? ": muted" : ": unmuted"));
    if (!(o->sends == m.sends)) L.add("Mixer track " + n + ": routing changed");
    for (size_t k = 0; k < std::min(o->slots.size(), m.slots.size()); ++k) {
      const auto& x = o->slots[k];
      const auto& y = m.slots[k];
      if (x.plugin.type != y.plugin.type)
        L.add("Mixer track " + n + ", slot " + std::to_string(k + 1) + ": " + plugName(x.plugin.type) + " -> " + plugName(y.plugin.type));
      else if (!(x == y))
        L.add("Mixer track " + n + ", slot " + std::to_string(k + 1) + ": " + plugName(y.plugin.type) + " settings changed");
    }
  }
  // Patterns and their notes.
  for (const Pattern& p : a.patterns)
    if (!b.pattern(p.id)) L.add("Pattern " + q(p.name) + " removed");
  for (const Pattern& p : b.patterns) {
    const Pattern* o = a.pattern(p.id);
    if (!o) {
      size_t n = 0;
      for (const auto& [ch, ns] : p.notes) n += ns.size();
      L.add("Pattern " + q(p.name) + " added" + (n ? " (" + std::to_string(n) + " notes)" : std::string()));
      continue;
    }
    if (o->name != p.name) L.add("Pattern " + q(o->name) + " renamed " + q(p.name));
    if (o->bars != p.bars) L.add("Pattern " + q(p.name) + ": " + std::to_string(o->bars) + " -> " + std::to_string(p.bars) + " bars");
    std::set<ObjectId> chans;
    for (const auto& [ch, ns] : o->notes) chans.insert(ch);
    for (const auto& [ch, ns] : p.notes) chans.insert(ch);
    static const std::vector<Note> none;
    for (ObjectId ch : chans) {
      auto x = o->notes.find(ch);
      auto y = p.notes.find(ch);
      const auto& xa = x == o->notes.end() ? none : x->second;
      const auto& ya = y == p.notes.end() ? none : y->second;
      if (xa == ya) continue;
      const auto [add, rem] = noteDelta(xa, ya);
      std::string l = "Pattern " + q(p.name) + ", channel " + q(chName(a, b, ch)) + ": ";
      if (add && rem && add == rem) l += std::to_string(add) + " notes changed";
      else {
        if (add) l += std::to_string(add) + " notes added";
        if (add && rem) l += ", ";
        if (rem) l += std::to_string(rem) + " removed";
      }
      L.add(l);
    }
    if (o->steps != p.steps) L.add("Pattern " + q(p.name) + ": steps changed");
    if (!(o->automation == p.automation)) L.add("Pattern " + q(p.name) + ": automation changed");
  }
  // Arrangement.
  {
    std::map<ObjectId, const Clip*> x, y;
    for (const auto& c : a.playlist) x[c.id] = &c;
    for (const auto& c : b.playlist) y[c.id] = &c;
    int added = 0, removed = 0, moved = 0, other = 0;
    for (const auto& [id, c] : x)
      if (!y.count(id)) ++removed;
    for (const auto& [id, c] : y) {
      auto it = x.find(id);
      if (it == x.end()) {
        ++added;
        continue;
      }
      const Clip& o = *it->second;
      if (o.start != c->start || o.track != c->track) ++moved;
      else if (o.length != c->length || o.muted != c->muted || o.offset != c->offset || o.loop != c->loop || o.gain != c->gain) ++other;
    }
    if (added) L.add("Arrangement: " + std::to_string(added) + " clips added");
    if (removed) L.add("Arrangement: " + std::to_string(removed) + " clips removed");
    if (moved) L.add("Arrangement: " + std::to_string(moved) + " clips moved");
    if (other) L.add("Arrangement: " + std::to_string(other) + " clips changed");
  }
  if (!(a.automations == b.automations)) {
    int added = 0, removed = 0, changed = 0;
    for (const auto& c : a.automations) {
      bool found = false;
      for (const auto& d : b.automations)
        if (d.id == c.id) found = true, changed += !(d == c);
      removed += !found;
    }
    for (const auto& d : b.automations) {
      bool found = false;
      for (const auto& c : a.automations) found = found || c.id == d.id;
      added += !found;
    }
    if (added) L.add("Automation: " + std::to_string(added) + " clips added");
    if (removed) L.add("Automation: " + std::to_string(removed) + " clips removed");
    if (changed) L.add("Automation: " + std::to_string(changed) + " clips changed");
  }
  if (!(a.markers == b.markers)) L.add("Markers: " + std::to_string(a.markers.size()) + " -> " + std::to_string(b.markers.size()));
  if (!(a.tracks == b.tracks)) L.add("Playlist tracks changed");
  if (L.dropped) L.v.push_back("... and " + std::to_string(L.dropped) + " more");
  if (L.v.empty() && !(a == b)) L.v.push_back("Other settings changed");
  return L.v;
}

}  // namespace gl
