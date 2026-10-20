// Gridline — FLP import: mixer inserts (names, colours, flags, levels, strip
// EQ, slot enable/mix, routing and send levels).
//
// Format notes (observed in saved projects; see analysis/demos/notes_gamma.md):
// * Inserts are written in index order: master, 1…N, then the "current" track
//   (FL's extra per-selection track). FL ≤ 21 always writes 127 inserts
//   (master + 125 + current); FL 25 writes only master + used range + current
//   and stores that count in event 103.
// * Event 236 (12 bytes): u32 ?, u32 flags, u32 ?. Flags: 1 polarity, 2 swap
//   L/R, 4 effects enabled, 8 track enabled (off = muted), 0x40/0x80 docking,
//   0x400 separator, 0x800 locked, 0x1000 solo.
// * Event 235: one byte per destination insert, 1 = routed there.
// * Event 225: 12-byte records {u32 0, u8 id, u8 kind, u16 where, i32 value}.
//   `where` = (insertBase + insert) << 6 | slot. kind 31: id 0 slot enabled,
//   1 slot mix (0..12800), 64+k route level to insert k (FL ≤ 21), 192 volume,
//   193 pan, 194 stereo separation, 208-210 EQ gain, 216-218 EQ freq,
//   224-226 EQ bandwidth. kind 32 (FL 25): id k = route level to insert k.
//   A routed destination with level 0 is a sidechain-only route.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "FlpFx.h"

namespace gl::flp {

namespace {
// FL colours are 0x00BBGGRR.
Rgb flColor(uint32_t v) { return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF); }

enum InsertFlag : uint32_t {
  kPolarity = 1u << 0,
  kSwapLR = 1u << 1,
  kFxEnabled = 1u << 2,
  kEnabled = 1u << 3,
  kSolo = 1u << 12,
};

struct InsertParams {
  int volume = 12800, pan = 0, sep = 0;
  int eqGain[3] = {0, 0, 0};
  int eqFreq[3] = {-1, -1, -1};  // -1: not stored (defaults)
  int eqBw[3] = {-1, -1, -1};
  int slotEnabled[kEffectSlots] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  int slotMix[kEffectSlots] = {12800, 12800, 12800, 12800, 12800, 12800, 12800, 12800, 12800, 12800};
  std::map<int, int> routeLevel;  // destination insert → level (0..16000)
  bool touched = false;           // any non-default value
};

struct Record {
  int id, kind, where, value;
};

std::vector<Record> records(const std::vector<Event>& evs) {
  std::vector<Record> out;
  for (const Event& e : evs)
    for (uint32_t i = 0; i + 12 <= e.size; i += 12)
      out.push_back({e.u8(i + 4), e.u8(i + 5), e.u16(i + 6), e.i32(i + 8)});
  return out;
}

}  // namespace

// Decodes all 225 records into per-insert parameters (keyed by FL insert index).
static std::map<int, InsertParams> decodeParams(const std::vector<Event>& evs) {
  std::vector<Record> recs = records(evs);
  // The insert field carries a format-dependent base: the master's records
  // (the lowest index) define it.
  int base = -1;
  for (const Record& r : recs)
    if (r.kind == 31 || r.kind == 32) base = base < 0 ? (r.where >> 6) : std::min(base, r.where >> 6);
  std::map<int, InsertParams> out;
  if (base < 0) return out;
  for (const Record& r : recs) {
    if (r.kind != 31 && r.kind != 32) continue;
    int ins = (r.where >> 6) - base, slot = r.where & 0x3F;
    if (ins < 0) continue;
    InsertParams& p = out[ins];
    if (r.kind == 32) {
      p.routeLevel[r.id] = r.value;
      continue;
    }
    if (r.id == 0 && slot < kEffectSlots) p.slotEnabled[slot] = r.value;
    else if (r.id == 1 && slot < kEffectSlots) p.slotMix[slot] = r.value;
    else if (r.id >= 64 && r.id < 192) p.routeLevel[r.id - 64] = r.value;
    else if (r.id == 192) p.volume = r.value;
    else if (r.id == 193) p.pan = r.value;
    else if (r.id == 194) p.sep = r.value;
    else if (r.id >= 208 && r.id <= 210) p.eqGain[r.id - 208] = r.value;
    else if (r.id >= 216 && r.id <= 218) p.eqFreq[r.id - 216] = r.value;
    else if (r.id >= 224 && r.id <= 226) p.eqBw[r.id - 224] = r.value;
  }
  return out;
}

void importMixer(Context& c) {
  Song& s = c.song;
  s.mixer.clear();
  const auto& ins = c.sec.inserts;
  // The last written insert is FL's "current" track (not a real insert).
  size_t n = ins.size();
  {
    int count103 = -1;
    for (const Event& e : c.file.events)
      if (e.id == 103) count103 = static_cast<int>(e.value());
    bool lastIsCurrent = count103 >= 0 ? static_cast<int>(n) == count103 : n >= 64;
    if (lastIsCurrent && n > 1) {
      bool hasFx = false;
      for (const auto& sl : ins.back().slots) hasFx |= !sl.events.empty();
      if (hasFx) c.report.warn("mixer: effects on FL's \"current\" track are not imported");
      --n;
    }
  }
  std::map<int, InsertParams> params = decodeParams(c.sec.mixerParams);

  // Which inserts are used: anything non-default, a routing other than the
  // master, an incoming route, or a channel feeding it. Unused trailing
  // inserts are trimmed (indices of the kept ones stay FL's indices).
  std::set<int> channelTargets;
  for (const auto& [idx, evs] : c.sec.channels)
    for (const Event& e : evs)
      if (e.id == 22 || e.id == 104) channelTargets.insert(static_cast<int>(e.id == 22 ? e.value() : static_cast<int16_t>(e.value())));
  // True when insert i holds an effect that processes audio (a sidechain
  // input is only useful there).
  auto hasFx = [&](int i) {
    if (i < 0 || static_cast<size_t>(i) >= n) return false;
    for (const auto& sl : ins[static_cast<size_t>(i)].slots) {
      const Event* nm = find(sl.events, 201);
      if (!nm) continue;
      std::string name = nm->text();
      if (name != "Control Surface" && name != "Fruity NoteBook" && name != "Fruity NoteBook 2" && name != "Fruity HTML NoteBook")
        return true;
    }
    return false;
  };
  // FL ≤ 21 projects (127 inserts written) route every insert to its default
  // send inserts 100–103 at level 0. Files that leave those levels out (FL's
  // render tests) mean the same: a missing level there is 0, not full level
  // (full level would add four extra copies of every insert to the master).
  const bool defaultSends = n >= 104;
  auto routeLevel = [&](size_t i, int to) {
    const int missing = defaultSends && to >= 100 && to <= 103 && static_cast<int>(i) != to ? 0 : 12800;
    auto pit = params.find(static_cast<int>(i));
    if (pit == params.end()) return missing;
    auto lv = pit->second.routeLevel.find(to);
    return lv == pit->second.routeLevel.end() ? missing : lv->second;
  };
  // Send buses (templates such as "Empty with 4 sends"): an insert without
  // effects that several inserts route to at level 0 is a send whose knobs
  // are down, not a sidechain; it is kept with those level-0 sends.
  std::set<int> sendBuses;
  {
    std::map<int, int> zeroIn;
    for (size_t i = 1; i < n; ++i)
      if (const Event* e = find(ins[i].events, 235))
        for (uint32_t k = 1; k < e->size && k < n; ++k)
          if (e->data[k] && k != i && routeLevel(i, static_cast<int>(k)) == 0) ++zeroIn[static_cast<int>(k)];
    // Default send inserts 100–103 (see routeLevel) stay trimmed unless used otherwise.
    for (const auto& [k, cnt] : zeroIn)
      if (cnt >= 2 && !hasFx(k) && !(defaultSends && k >= 100 && k <= 103)) sendBuses.insert(k);
  }
  // Destinations of insert i. Sidechain-only routes (level 0) into inserts
  // without effects carry nothing and are dropped (except sends into send buses).
  auto routes = [&](size_t i) {
    std::vector<int> r;
    if (const Event* e = find(ins[i].events, 235))
      for (uint32_t k = 0; k < e->size; ++k) {
        if (!e->data[k] || k == i) continue;
        const bool zero = k != 0 && routeLevel(i, static_cast<int>(k)) == 0;
        const bool toBus = sendBuses.count(static_cast<int>(k)) && !sendBuses.count(static_cast<int>(i));
        if (zero && sendBuses.count(static_cast<int>(k)) && !toBus) continue;  // between send buses
        if (!(zero && !hasFx(static_cast<int>(k)) && !toBus)) r.push_back(static_cast<int>(k));
      }
    return r;
  };
  size_t keep = std::min<size_t>(n, 1);
  for (size_t i = 0; i < n; ++i) {
    bool used = i == 0 || channelTargets.count(static_cast<int>(i)) || sendBuses.count(static_cast<int>(i));
    const auto& in = ins[i];
    used = used || find(in.events, 204) || find(in.events, 149);
    for (const auto& sl : in.slots) used = used || !sl.events.empty();
    if (auto it = params.find(static_cast<int>(i)); it != params.end()) {
      const InsertParams& p = it->second;
      used = used || p.volume != 12800 || p.pan != 0 || p.sep != 0 || p.eqGain[0] || p.eqGain[1] || p.eqGain[2];
    }
    if (const Event* e = find(in.events, 236); e && e->size >= 8) {
      uint32_t f = e->u32(4);
      used = used || (f & (kPolarity | kSwapLR | kSolo)) || !(f & kEnabled) || !(f & kFxEnabled);
    }
    if (used) keep = std::max(keep, i + 1);
    for (int to : routes(i))  // a used source keeps its destinations
      if (to > 0 && static_cast<size_t>(to) < n && used) keep = std::max(keep, static_cast<size_t>(to) + 1);
  }
  // Second pass: an insert that routes into a kept one is itself in use only
  // when it is in use for another reason; destinations of kept inserts stay.
  for (size_t i = 0; i < keep; ++i)
    for (int to : routes(i))
      if (to > 0 && static_cast<size_t>(to) < n) keep = std::max(keep, static_cast<size_t>(to) + 1);
  if (n == 0) keep = 1;
  if (keep < n) c.report.counts["mixer inserts trimmed"] = static_cast<int>(n - keep);

  for (size_t i = 0; i < keep; ++i) {
    MixerTrack t;
    t.id = s.allocateId();
    if (i < ins.size()) {
      const auto& in = ins[i];
      if (const Event* e = find(in.events, 204)) t.name = e->text();
      if (const Event* e = find(in.events, 149)) t.color = flColor(e->value());
      uint32_t flags = kFxEnabled | kEnabled;
      if (const Event* e = find(in.events, 236); e && e->size >= 8) flags = e->u32(4);
      t.invertPolarity = flags & kPolarity;
      t.swapLR = flags & kSwapLR;
      t.muted = !(flags & kEnabled);
      t.solo = flags & kSolo;
      const bool fxOn = flags & kFxEnabled;
      auto pit = params.find(static_cast<int>(i));
      if (pit != params.end()) {
        const InsertParams& p = pit->second;
        t.volume = static_cast<float>(fx::glFaderFromGain(fx::flMixerGain(p.volume)));
        t.pan = static_cast<float>(std::clamp(p.pan / 6400.0, -1.0, 1.0));
        // FL: negative = knob left = more separation; Gridline: +1 = wide.
        t.stereoSep = static_cast<float>(std::clamp(-p.sep / 64.0, -1.0, 1.0));
        for (size_t b = 0; b < 3; ++b) {
          t.eqGain[b] = static_cast<float>(std::clamp(p.eqGain[b] / 100.0, -18.0, 18.0));
          static const int defFreq[3] = {5777, 33145, 55825};
          if (p.eqFreq[b] >= 0 && p.eqFreq[b] != defFreq[b])
            t.eqFreq[b] = static_cast<float>(std::clamp(fx::flEqFreqHz(p.eqFreq[b]), 20.0, 20000.0));
          if (p.eqBw[b] >= 0 && p.eqBw[b] != 17500) t.eqQ[b] = static_cast<float>(std::clamp(fx::flInsertEqQ(p.eqBw[b]), 0.1, 10.0));
        }
        for (size_t k = 0; k < t.slots.size() && k < static_cast<size_t>(kEffectSlots); ++k) {
          t.slots[k].enabled = p.slotEnabled[k] != 0 && fxOn;
          t.slots[k].mix = static_cast<float>(std::clamp(p.slotMix[k] / 12800.0, 0.0, 1.0));
        }
      } else if (!fxOn) {
        for (auto& sl : t.slots) sl.enabled = false;
      }
    }
    if (i == 0 && t.name.empty()) t.name = "Master";
    s.mixer.push_back(t);
    c.insertIds[static_cast<int>(i)] = t.id;
  }

  // Routing: FL lists every destination (the master included); an insert
  // without a routing event feeds the master.
  int sidechains = 0, sends = 0;
  for (size_t i = 1; i < s.mixer.size(); ++i) {
    MixerTrack& t = s.mixer[i];
    const Event* re = i < ins.size() ? find(ins[i].events, 235) : nullptr;
    if (!re) {
      t.sends.push_back({s.mixer[0].id, 1.0f, false});
      continue;
    }
    for (int to : routes(i)) {
      if (to == static_cast<int>(i)) continue;
      if (static_cast<size_t>(to) >= s.mixer.size()) {
        c.report.warn("mixer: insert " + std::to_string(i) + " routes to unused insert " + std::to_string(to) + " (dropped)");
        continue;
      }
      ObjectId dest = s.mixer[static_cast<size_t>(to)].id;
      if (!canRoute(s, t.id, dest)) {
        c.report.warn("mixer: route " + std::to_string(i) + " → " + std::to_string(to) + " would form a cycle (dropped)");
        continue;
      }
      int level = routeLevel(i, to);
      if (to != 0 && level == 0 && sendBuses.count(to)) {
        t.sends.push_back({dest, 0.0f, false});  // send to a send bus, knob down
        ++sends;
      } else if (to != 0 && level == 0) {
        t.sends.push_back({dest, 1.0f, true});  // sidechain-only route
        ++sidechains;
      } else {
        t.sends.push_back({dest, static_cast<float>(std::clamp(fx::flMixerGain(level), 0.0, 1.25)), false});
        ++sends;
      }
    }
  }
  c.report.counts["mixer inserts"] = static_cast<int>(s.mixer.size());
  c.report.counts["mixer routes"] = sends;
  if (sidechains) c.report.counts["mixer sidechain routes"] = sidechains;
}

}  // namespace gl::flp
