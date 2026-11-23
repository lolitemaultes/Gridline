// Gridline - K-245: macro knobs for every plugin (see Macros.h).
#include "Macros.h"
#include <algorithm>
#include <set>
#include "Document.h"

namespace gl::macro {

bool isMacroParam(ParamId pid) {
  return pid.owner != kSongId && pid.index >= kLayerMacroBase && pid.index < kLayerMacroBase + static_cast<uint32_t>(kLayerMacros);
}

LayerMacro* find(Song& s, ObjectId owner, int k) {
  if (k < 0 || k >= kLayerMacros || owner == kSongId || owner == kNoId) return nullptr;
  if (Channel* c = s.channel(owner)) return &c->macros[static_cast<size_t>(k)];
  if (MixerTrack* m = s.mixerTrack(owner)) return &m->macros[static_cast<size_t>(k)];
  return nullptr;
}

const LayerMacro* find(const Song& s, ObjectId owner, int k) { return find(const_cast<Song&>(s), owner, k); }

bool hasMacros(const Song& s, ObjectId owner) { return find(s, owner, 0) != nullptr; }

std::string name(const Song& s, ObjectId owner, int k) {
  const LayerMacro* m = find(s, owner, k);
  if (m && !m->name.empty()) return m->name;
  return "Macro " + std::to_string(k + 1);
}

ObjectId ownerFor(const Song& s, ParamId target) {
  if (target.owner == kSongId || target.owner == kNoId) return kNoId;
  return hasMacros(s, target.owner) ? target.owner : kNoId;
}

std::vector<const ModLink*> links(const Song& s, ObjectId owner, int k) {
  std::vector<const ModLink*> out;
  for (const auto& l : s.modLinks)
    if (l.sourceOwner == owner && l.sourceSlot == kLayerMacroSlot && l.sourceOutput == k) out.push_back(&l);
  return out;
}

bool linkedMacro(const Song& s, ParamId target, ObjectId* owner, int* k) {
  const ModLink* l = findModLink(s, target.owner, target.index);
  if (!l || l->sourceSlot != kLayerMacroSlot || !find(s, l->sourceOwner, l->sourceOutput)) return false;
  if (owner) *owner = l->sourceOwner;
  if (k) *k = l->sourceOutput;
  return true;
}

bool assigned(const Song& s, ObjectId owner, int k) {
  const LayerMacro* m = find(s, owner, k);
  if (!m) return false;
  if (!m->name.empty()) return true;
  for (const auto& l : s.modLinks)
    if (l.sourceOwner == owner && l.sourceSlot == kLayerMacroSlot && l.sourceOutput == k) return true;
  return false;
}

std::vector<ParamId> assignedMacros(const Song& s) {
  std::vector<ParamId> out;
  for (const auto& c : s.channels)
    for (int k = 0; k < kLayerMacros; ++k)
      if (assigned(s, c.id, k)) out.push_back(layerMacroParam(c.id, k));
  for (const auto& m : s.mixer)
    for (int k = 0; k < kLayerMacros; ++k)
      if (assigned(s, m.id, k)) out.push_back(layerMacroParam(m.id, k));
  return out;
}

int firstFree(const Song& s, ObjectId owner) {
  if (!hasMacros(s, owner)) return -1;
  for (int k = 0; k < kLayerMacros; ++k)
    if (!assigned(s, owner, k)) return k;
  return -1;
}

bool wouldLoop(const Song& s, ObjectId owner, int k, ParamId target) {
  // Follows the macros `target` leads to (a macro drives the macros it is linked to): a loop when one
  // of them is the source macro itself.
  std::vector<ParamId> todo{target};
  std::set<uint64_t> seen;
  const ParamId source = layerMacroParam(owner, k);
  while (!todo.empty()) {
    const ParamId p = todo.back();
    todo.pop_back();
    if (!isMacroParam(p)) continue;
    if (p == source) return true;
    if (!seen.insert(p.packed()).second) continue;
    for (const ModLink* l : links(s, p.owner, static_cast<int>(p.index - kLayerMacroBase))) todo.push_back({l->owner, l->index});
  }
  return false;
}

void defaultRange(const Song& s, ParamId target, float& min, float& max) {
  min = 0, max = 1;
  const auto d = describeParam(s, target);
  const auto v = getParam(s, target);
  if (!d || !v || d->kind != ParamKind::Continuous) return;
  const float n = static_cast<float>(std::clamp(d->toNormalized(*v), 0.0, 1.0));
  min = n;
  max = n >= 0.99f ? 0.0f : 1.0f;
}

bool link(Document& d, ParamId target, ObjectId owner, int k) {
  const Song& s = d.song();
  if (!find(s, owner, k) || !describeParam(s, target) || wouldLoop(s, owner, k, target)) return false;
  ModLink l;
  if (const ModLink* cur = findModLink(s, target.owner, target.index); cur && cur->sourceSlot == kLayerMacroSlot) l = *cur;  // keeps its range and curve
  else defaultRange(s, target, l.min, l.max);
  l.sourceOwner = owner;
  l.sourceSlot = kLayerMacroSlot;
  l.sourceOutput = k;
  l.owner = target.owner;
  l.index = target.index;
  l.enabled = true;
  return d.setModLink(l);
}

bool unlink(Document& d, ParamId target) {
  if (!linkedMacro(d.song(), target, nullptr, nullptr)) return false;
  return d.removeModLink(target);
}

bool clear(Document& d, ObjectId owner, int k) {
  if (links(d.song(), owner, k).empty()) return false;
  d.editSong("clear macro", [&](Song& s) {
    s.modLinks.erase(std::remove_if(s.modLinks.begin(), s.modLinks.end(),
                                    [&](const ModLink& l) { return l.sourceOwner == owner && l.sourceSlot == kLayerMacroSlot && l.sourceOutput == k; }),
                     s.modLinks.end());
  });
  return true;
}

bool rename(Document& d, ObjectId owner, int k, const std::string& nm) {
  const LayerMacro* m = find(d.song(), owner, k);
  const std::string n = nm.substr(0, 24);
  if (!m || m->name == n) return false;
  if (d.song().channel(owner)) return d.editChannel(owner, "rename macro", [&](Channel& c) { c.macros[static_cast<size_t>(k)].name = n; });
  return d.editMixerTrack(owner, "rename macro", [&](MixerTrack& t) { t.macros[static_cast<size_t>(k)].name = n; });
}

}  // namespace gl::macro
