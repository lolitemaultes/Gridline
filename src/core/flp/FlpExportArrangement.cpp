// Gridline — FLP export: arrangements (playlist records, markers, tracks)
// and the mixer (inserts, slots, routing, 225 parameter records) (J-34).
//
// Playlist record (233), 80 bytes in FL 25 (see FlpArrangement.cpp):
//   u32 position, u16 0x5000, u16 item (0x5000 + pattern, else channel),
//   u32 length, u16 499 − track, u16 group, u16 0x78, u16 flags (0x40, +0x2000
//   muted), 4 bytes, @24 / @28 start / end offset (i32 ticks for patterns,
//   f32 beats for automation, f32 ms of the sample for audio), u32 counter,
//   @36 f32 fade in (ms), @40 fade-in tension, @44 f32 fade out (ms), @48
//   f32 pitch (cents), @52 f32 gain, @64 f64 time stretch.
// Track (238), 70 bytes: u32 index (1-based), u32 colour, u32 icon, u8 on,
//   f32 height, …, live settings @22 (motion, press, trigger sync, queue,
//   tolerant, position sync), @46 u8 grouped under the track above.
// Mixer parameter record (225): {u32 0, u8 id, u8 kind, u16 (base + insert)
//   << 6 | slot, i32 value}; FL 25 base 448.
#include <algorithm>
#include <cmath>
#include <limits>
#include "FlpExportInternal.h"
#include "FlpFx.h"

namespace gl::flp::xp {

namespace {

Bytes dwords(std::initializer_list<int64_t> v) {
  ByteWriter w;
  for (int64_t x : v) w.u32(static_cast<uint32_t>(x));
  return w.b;
}

// FL's default playlist record (fields set by the caller).
Bytes defaultClipRecord() {
  ByteWriter w;
  w.u32(0);
  w.u16(0x5000);
  w.u16(0);
  w.u32(0);
  w.u16(0);
  w.u16(0);
  w.u16(0x78);
  w.u16(0x40);
  w.u8(0x40);  // @20: four constant bytes in every FL 25 record
  w.u8(0x64);
  w.u8(0x80);
  w.u8(0x80);
  w.u32(0);
  w.u32(0);
  w.u32(0);      // @32 counter
  w.f32(0);      // @36 fade in
  w.f32(0);      // @40
  w.f32(0);      // @44 fade out
  w.f32(0);      // @48 pitch
  w.f32(1.0f);   // @52 gain
  w.u32(0);
  w.u32(0);
  w.f64(1.0);    // @64 stretch
  w.u32(0);
  w.u32(0);
  return w.b;
}

// FL's default playlist track record.
Bytes defaultTrack(int index) {
  ByteWriter w;
  w.u32(static_cast<uint32_t>(index + 1));
  w.u32(0x565148);  // colour (BGR)
  w.u32(0);         // icon
  w.u8(1);          // on
  w.f32(1.0f);      // height
  w.u8(0xF0);       // @17..21: constants in every FL 25 track record
  w.u8(0xFF);
  w.u8(0xFF);
  w.u8(0xFF);
  w.u8(0xFF);
  w.u32(0);  // @22 motion
  w.u32(0);  // @26 press
  w.u32(5);  // @30 trigger sync (4 beats)
  w.u32(0);  // @34 queue
  w.u32(1);  // @38 tolerant
  w.u32(0);  // @42 position sync
  w.u8(0);   // @46 grouped
  w.u32(0);
  w.u32(0);
  w.u8(0);
  for (int i = 0; i < 8; ++i) w.u8(0xFF);
  w.u8(1);
  w.u32(0);
  w.u32(0);
  while (w.size() < 70) w.u8(0);
  return w.b;
}

constexpr uint8_t kPostIds[] = {242, 100, 29, 39, 40, 31, 38, 103};
bool isPost(uint8_t id) { return std::find(std::begin(kPostIds), std::end(kPostIds), id) != std::end(kPostIds); }

}  // namespace

void Exporter::writeArrangements() {
  const int n = std::max<int>(1, static_cast<int>(s.arrangements.size()));
  std::vector<EventList> posts;  // 242 per arrangement
  EventList postGlobals;         // 29 39 40 31 38 from the origin
  uint32_t counter = 0;
  for (int ai = 0; ai < n; ++ai) {
    const bool cur = s.arrangements.empty() || ai == s.currentArrangement;
    const Arrangement* a = s.arrangements.empty() ? nullptr : &s.arrangements[static_cast<size_t>(ai)];
    const std::vector<Clip>& clips = cur ? s.playlist : a->playlist;
    const std::vector<PlaylistTrack>& tracks = cur ? s.tracks : a->tracks;
    const std::vector<Marker>& markers = cur ? s.markers : a->markers;
    const LiveSettings& live = cur ? s.live : a->live;
    const std::string name = a ? a->name : "Arrangement";
    const EventList orig = origin("arr/" + std::to_string(ai));
    const EventList oe = stripPseudo(orig);
    for (const auto& e : oe)
      if (e.id == 242) posts.push_back({e});
      else if (isPost(e.id) && e.id != 100 && e.id != 103 && postGlobals.size() < 5) postGlobals.push_back(e);
    const bool same = unchanged(orig, fingerprintArrangement(tracks, markers, live, name));
    w.word(99, static_cast<uint32_t>(ai));
    if (const RawEvent* e = findRaw(oe, 241); e && same) w.raw(*e);
    else w.text(241, name);
    w.byte(36, findRaw(oe, 36) ? findRaw(oe, 36)->value() : 0);
    // Playlist.
    ByteWriter pl;
    for (const Clip& c : clips) {
      const EventList co = origin(originKey("clip", c.id));
      const RawEvent* orec0 = findRaw(co, 233);
      // Records of older FL versions (32 / 60 bytes) are widened to FL 25's 80.
      RawEvent wide;
      const RawEvent* orec = nullptr;
      if (orec0 && orec0->data.size() <= 80) {
        wide.id = 233;
        wide.data = defaultClipRecord();
        // the fields older records share with FL 25's (bytes after @32 meant
        // something else or nothing; the importer only reads them at 80 bytes)
        const size_t keep = orec0->data.size() == 80 ? 80 : std::min<size_t>(orec0->data.size(), 32);
        std::copy(orec0->data.begin(), orec0->data.begin() + static_cast<std::ptrdiff_t>(keep), wide.data.begin());
        orec = &wide;
      }
      if (orec && unchanged(co, fingerprintClip(c, im))) {
        Bytes r = orec->data;
        putU16(r, 12, static_cast<uint32_t>(499 - std::clamp(c.track, 0, 499)));  // older files count from track 198
        pl.bytes(r);
        continue;
      }
      Bytes r = orec ? orec->data : defaultClipRecord();
      if (!orec) putU32(r, 32, ++counter);
      const Tick len = c.length > 0 ? c.length : s.clipLength(c);
      putU32(r, 0, static_cast<uint32_t>(std::max<Tick>(0, c.start)));
      putU32(r, 8, static_cast<uint32_t>(std::max<Tick>(1, len)));
      putU16(r, 12, static_cast<uint32_t>(499 - std::clamp(c.track, 0, 499)));
      putU16(r, 18, static_cast<uint32_t>((getU16(r, 18) & ~0x2000) | (c.muted ? 0x2000 : 0)));
      const double ppq = std::max(1, s.ppq);
      if (c.kind == ClipKind::Pattern) {
        const int pi = im.pat(c.pattern);
        if (pi < 0) continue;
        putU16(r, 6, static_cast<uint32_t>(0x5000 + pi));
        putI32(r, 24, static_cast<int32_t>(c.offset));
        putI32(r, 28, static_cast<int32_t>(c.offset + len));
      } else if (c.kind == ClipKind::Automation) {
        const int ci = im.aut(c.source);
        if (ci < 0) continue;
        putU16(r, 6, static_cast<uint32_t>(ci));
        putF32(r, 24, static_cast<float>(c.offset / ppq));
        putF32(r, 28, static_cast<float>((c.offset + len) / ppq));
      } else {
        const int ci = im.ch(c.source);
        if (ci < 0) continue;
        putU16(r, 6, static_cast<uint32_t>(ci));
        auto keepF = [&](size_t at, double v, double scale) {  // the original float when it decodes the same
          const float old = getF32(r, at);
          if (!(orec && std::isfinite(old) && static_cast<double>(old) / scale == v)) putF32(r, at, static_cast<float>(v * scale));
        };
        keepF(24, c.audioStart, 1000.0);
        if (c.audioEnd > 0) keepF(28, c.audioEnd, 1000.0);
        else putF32(r, 28, -1.0f);
        keepF(36, c.fadeIn, 1000.0);
        keepF(44, c.fadeOut, 1000.0);
        const auto cents = audioCents.find(c.source);
        const float pitch = c.pitch - static_cast<float>(cents == audioCents.end() ? 0 : cents->second);
        if (!(orec && getF32(r, 48) + static_cast<float>(cents == audioCents.end() ? 0 : cents->second) == c.pitch)) putF32(r, 48, pitch);
        if (!(orec && getF32(r, 52) == c.gain)) putF32(r, 52, c.gain);
        if (!(orec && getF64(r, 64) == c.stretch)) putF64(r, 64, c.stretch);
        if (c.reverse) rep.counts["audio clips: reverse not written"]++;
        if (c.syncBpm > 0) rep.counts["audio clips: tempo sync not written"]++;
      }
      pl.bytes(r);
    }
    w.data(233, pl.b);
    if (same) {
      for (const auto& e : oe)
        if (e.id != 99 && e.id != 241 && e.id != 36 && !isPost(e.id)) w.raw(e);
      continue;
    }
    // Markers: 148 position (+ kind << 24: 8 time signature, 5 song start), 33 / 34, 205 name.
    std::vector<std::pair<uint32_t, EventList>> oldMarkers;  // template events per marker
    for (const auto& e : oe) {
      if (e.id == 148) oldMarkers.push_back({e.value(), {}});
      else if (!oldMarkers.empty() && (e.id == 33 || e.id == 34 || e.id == 45 || e.id == 46 || e.id == 168 || e.id == 101))
        oldMarkers.back().second.push_back(e);
    }
    std::vector<Marker> ms = markers;
    if (live.areaEnd > 0) {  // the live area ends at FL's "song start" marker (kind 5)
      auto it = std::find_if(ms.begin(), ms.end(), [&](const Marker& m) { return m.pos == live.areaEnd && m.timeSigNum == 0; });
      if (it != ms.end()) {
        it->timeSigNum = -1;
      } else {
        Marker start;
        start.pos = live.areaEnd;
        start.timeSigNum = -1;
        ms.push_back(start);
        std::stable_sort(ms.begin(), ms.end(), [](const Marker& x, const Marker& y) { return x.pos < y.pos; });
      }
    }
    for (const Marker& m : ms) {
      const uint32_t kind = m.timeSigNum < 0 ? 5 : m.timeSigNum > 0 ? 8 : 0;
      const uint32_t v = (kind << 24) | static_cast<uint32_t>(m.pos & 0x00FFFFFF);
      w.dword(148, v);
      const EventList* extra = nullptr;
      for (const auto& [ov, ev] : oldMarkers)
        if (ov == v) extra = &ev;
      if (kind == 8) {
        w.byte(33, static_cast<uint32_t>(m.timeSigNum));
        w.byte(34, static_cast<uint32_t>(m.timeSigDen));
      }
      if (extra)
        for (const auto& e : *extra)
          if (e.id != 33 && e.id != 34) w.raw(e);
      w.text(205, m.name);
      if (m.action != MarkerAction::None) rep.counts["marker actions not written"]++;
    }
    // Tracks: FL keeps 500.
    std::map<int, Bytes> oldTracks;
    std::map<int, EventList> oldAfter;  // 43 (and others) after each track record
    int curT = -1;
    for (const auto& e : oe) {
      if (e.id == 238 && e.data.size() >= 4) {
        curT = static_cast<int>(getU32(e.data, 0)) - 1;
        oldTracks[curT] = e.data;
      } else if (curT >= 0 && e.id != 239 && !isPost(e.id) && e.id != 148 && e.id != 205) {
        oldAfter[curT].push_back(e);
      }
    }
    std::vector<bool> grouped(500, false);
    for (size_t t = 0; t < tracks.size() && t < 500; ++t)
      for (int k = 1; k <= tracks[t].groupSize && t + static_cast<size_t>(k) < 500; ++k) grouped[t + static_cast<size_t>(k)] = true;
    for (int t = 0; t < kMaxPlaylistTracks; ++t) {
      Bytes r = oldTracks.count(t) && oldTracks[t].size() >= 47 ? oldTracks[t] : defaultTrack(t);
      putU32(r, 0, static_cast<uint32_t>(t + 1));
      const PlaylistTrack def;
      const PlaylistTrack& pt = static_cast<size_t>(t) < tracks.size() ? tracks[static_cast<size_t>(t)] : def;
      putU32(r, 4, flColor(pt.color));
      putU8(r, 12, pt.muted ? 0 : 1);
      if (!(getF32(r, 13) == pt.height || (std::clamp(getF32(r, 13), 0.25f, 4.0f) == pt.height))) putF32(r, 13, pt.height);
      if (r.size() >= 47) {
        static const int kPad[3] = {0, 1, 3};
        putU32(r, 22, static_cast<uint32_t>(pt.follow));
        const uint32_t oldPad = getU32(r, 26);
        static const PadMode kPadBack[4] = {PadMode::Trigger, PadMode::Gate, PadMode::Gate, PadMode::Toggle};
        if (!(oldPad <= 3 && kPadBack[oldPad] == pt.pad)) putU32(r, 26, static_cast<uint32_t>(kPad[static_cast<int>(pt.pad) % 3]));
        putU32(r, 30, static_cast<uint32_t>(pt.launch));
        putU32(r, 42, static_cast<uint32_t>(pt.syncStart));
        putU8(r, 46, grouped[static_cast<size_t>(t)] ? 1 : 0);
      }
      w.data(238, r);
      if (oldAfter.count(t)) {
        for (const auto& e : oldAfter[t]) w.raw(e);
      } else {
        w.byte(43, 0);
      }
      if (!pt.name.empty()) w.text(239, pt.name);
      if (pt.locked) rep.counts["track locks not written"]++;
    }
  }
  // After all arrangements: 242 per arrangement, current arrangement, globals, insert count.
  for (int ai = 0; ai < n; ++ai) {
    if (static_cast<size_t>(ai) < posts.size()) w.raw(posts[static_cast<size_t>(ai)].front());
    else {
      ByteWriter b;
      b.u32(0);
      b.u32(0);
      b.u32(0);
      b.f64(6.4);
      b.u32(0);
      b.u32(16);
      w.data(242, b.b);
    }
  }
  w.word(100, static_cast<uint32_t>(s.arrangements.empty() ? 0 : s.currentArrangement));
  auto glob = [&](uint8_t id, uint32_t d) {
    const RawEvent* e = findRaw(postGlobals, id);
    w.byte(id, e ? e->value() : d);
  };
  glob(29, 1);
  glob(39, 1);
  glob(40, 0);
  glob(31, 0);
  glob(38, 1);
  w.word(103, static_cast<uint32_t>(insertCount));
}

// ------------------------------------------------------------ mixer

namespace {

// Integer stored value whose decoded value (float) is `want` (monotonic law), else the closest.
int64_t solveInt(int64_t lo, int64_t hi, double want, const std::function<double(int64_t)>& f) {
  double vlo = f(lo), vhi = f(hi);
  int64_t best = std::fabs(vlo - want) <= std::fabs(vhi - want) ? lo : hi;
  double bestErr = std::min(std::fabs(vlo - want), std::fabs(vhi - want));
  const bool inc = vhi >= vlo;
  int64_t a = lo, z = hi;
  while (z - a > 1) {
    const int64_t m = (a + z) / 2;
    const double v = f(m);
    if (std::fabs(v - want) < bestErr) best = m, bestErr = std::fabs(v - want);
    if (static_cast<float>(v) == static_cast<float>(want)) return m;
    if ((v < want) == inc) a = m;
    else z = m;
  }
  for (int64_t c : {a, z})
    if (std::fabs(f(c) - want) < bestErr) best = c, bestErr = std::fabs(f(c) - want);
  return best;
}

struct Rec {
  uint8_t id, kind;
  int slot;
  int32_t value;
};

}  // namespace

void Exporter::writeMixer() {
  std::vector<std::pair<int, Rec>> recs;
  for (int i = 0; i < insertCount; ++i) {
    const bool real = i < static_cast<int>(s.mixer.size());
    const MixerTrack* t = real ? &s.mixer[static_cast<size_t>(i)] : nullptr;
    EventList orig = real ? origin(originKey("mx", t->id)) : EventList{};
    if (!real && i == insertCount - 1) {
      const EventList cur = origin("mxcur");
      if (originIndex(cur) == i) orig = cur;
    } else if (!real) {  // an insert the import trimmed (unused): as it was
      orig = origin("mxfl/" + std::to_string(i));
    }
    const EventList oe = stripPseudo(orig);
    const bool same = real ? unchanged(orig, fingerprintInsert(*t, im)) : !orig.empty();
    // Template records of this insert.
    std::vector<Rec> tr;
    for (const auto& e : oe)
      if (e.id == 225 && e.data.size() == 12) tr.push_back({e.data[4], e.data[5], getU16(e.data, 6) & 0x3F, static_cast<int32_t>(getU32(e.data, 8))});
    EventList ev;
    for (const auto& e : oe)
      if (e.id != 225) ev.push_back(e);
    if (!same) {
      // Header events before 236, then 236, then the rest (235 165 166 49 154 147).
      EventList pre, post;
      const RawEvent* flags = findRaw(ev, 236);
      bool after = false;
      for (const auto& e : ev) {
        if (e.id == 236) {
          after = true;
          continue;
        }
        if (e.id == 149 || e.id == 204 || e.id == 235 || e.id == 154) continue;  // from the model
        (after ? post : pre).push_back(e);
      }
      EventList out;
      if (t && (t->color != MixerTrack{}.color || findRaw(ev, 149))) {
        Bytes b(4);
        putU32(b, 0, flColor(t->color));
        out.push_back({149, b});
      }
      for (const auto& e : pre) out.push_back(e);
      if (!findRaw(pre, 42)) out.push_back(scalarEvent(42, 0));
      if (t && !t->name.empty() && !(i == 0 && t->name == "Master" && !findRaw(ev, 204))) out.push_back({204, utf16z(t->name)});
      Bytes fb = flags && flags->data.size() == 12 ? flags->data : dwords({0, i == 0 ? 0x0C : 0x4C, 0});
      uint32_t f = getU32(fb, 4);
      if (t) {
        auto bit = [&](uint32_t b, bool on) { f = on ? (f | b) : (f & ~b); };
        bit(1, t->invertPolarity);
        bit(2, t->swapLR);
        bit(8, !t->muted);
        bit(0x1000, t->solo);
        bool anyOn = false;
        for (const auto& sl : t->slots) anyOn = anyOn || sl.enabled;
        if (anyOn) f |= 4;
        if (t->disabled) rep.counts["mixer: disabled inserts written enabled"]++;
      }
      putU32(fb, 4, f);
      out.push_back({236, fb});
      out.push_back({0, {}});  // slot marker (replaced below)
      // routing
      if (t) {
        Bytes route(1, 0);
        for (const auto& sd : t->sends) {
          const int d = im.ins(sd.to);
          if (d < 0) continue;
          if (route.size() <= static_cast<size_t>(d)) route.resize(static_cast<size_t>(d) + 1, 0);
          route[static_cast<size_t>(d)] = 1;
        }
        out.push_back({235, route});
      } else if (const RawEvent* r = findRaw(ev, 235)) {
        out.push_back(*r);
      } else if (i > 0 && oe.empty()) {
        out.push_back(RawEvent{235, {1}});
      }
      bool hasPost = false;
      for (const auto& e : post) hasPost = hasPost || e.id == 147;
      if (!hasPost) {
        out.push_back(scalarEvent(165, 3));
        out.push_back(scalarEvent(166, 1));
        out.push_back(scalarEvent(49, 0));
      } else {
        for (const auto& e : post)
          if (e.id == 154 || e.id == 147) continue;
          else out.push_back(e);
      }
      {
        Bytes b(4);
        putU32(b, 0, t && t->input >= 0 ? static_cast<uint32_t>(t->input) : 0xFFFFFFFFu);
        if (const RawEvent* e = findRaw(ev, 154); e && !(t && t->input >= 0)) b = e->data;
        out.push_back({154, b});
      }
      if (const RawEvent* e = findRaw(post, 147)) out.push_back(*e);
      else {
        Bytes b(4);
        putU32(b, 0, i == 0 ? 0u : 0xFFFFFFFFu);
        out.push_back({147, b});
      }
      ev = std::move(out);
    }
    // Write, with the slots right after 236.
    bool slotsDone = false;
    for (const auto& e : ev) {
      if (e.id == 0 && e.data.empty()) continue;
      w.raw(e);
      if (e.id == 236 && !slotsDone) {
        slotsDone = true;
        for (int k = 0; k < kEffectSlots; ++k) {
          if (t && static_cast<size_t>(k) < t->slots.size() && !t->slots[static_cast<size_t>(k)].plugin.empty()) {
            PluginOut po = encodePlugin(t->slots[static_cast<size_t>(k)].plugin, t->id, k, false,
                                        "insert " + std::to_string(i) + " slot " + std::to_string(k + 1));
            if (po.flPlugin) {
              w.text(201, po.flName);
              w.data(212, po.wrapper);
              const RawEvent* dn = findRaw(po.extras, 203);
              if (dn) w.raw(*dn);
              const RawEvent* ic = findRaw(po.extras, 155);
              if (ic) w.raw(*ic);
              else w.dword(155, 0);
              const RawEvent* co = findRaw(po.extras, 128);
              if (co) w.raw(*co);
              else w.dword(128, 0x565148);
              const RawEvent* f41 = findRaw(po.extras, 41);
              if (f41) w.raw(*f41);
              else w.byte(41, 0);
              if (po.hasState) w.data(213, po.state);
            }
          }
          w.word(98, static_cast<uint32_t>(k));
        }
      }
    }
    // Parameter records.
    auto find = [&](uint8_t id, uint8_t kind, int slot) -> int32_t* {
      for (auto& r : tr)
        if (r.id == id && r.kind == kind && r.slot == slot) return &r.value;
      return nullptr;
    };
    auto set = [&](uint8_t id, uint8_t kind, int slot, int32_t v) {
      if (int32_t* p = find(id, kind, slot)) *p = v;
      else tr.push_back({id, kind, slot, v});
    };
    if (tr.empty() || !same) {
      if (tr.empty()) {
        for (int k = 0; k < kEffectSlots; ++k) set(0, 31, k, 1), set(1, 31, k, 12800);
        set(192, 31, 0, 12800);
        set(193, 31, 0, 0);
        set(194, 31, 0, 0);
        for (int b = 0; b < 3; ++b) set(static_cast<uint8_t>(208 + b), 31, 0, 0);
        static const int kFreq[3] = {5777, 33145, 55825};
        for (int b = 0; b < 3; ++b) set(static_cast<uint8_t>(216 + b), 31, 0, kFreq[b]);
        for (int b = 0; b < 3; ++b) set(static_cast<uint8_t>(224 + b), 31, 0, 17500);
      }
      if (t && !same) {
        for (int k = 0; k < kEffectSlots && static_cast<size_t>(k) < t->slots.size(); ++k) {
          const EffectSlot& sl = t->slots[static_cast<size_t>(k)];
          set(0, 31, k, sl.enabled ? 1 : 0);
          int32_t* m = find(1, 31, k);
          if (!(m && static_cast<float>(std::clamp(*m / 12800.0, 0.0, 1.0)) == sl.mix)) set(1, 31, k, static_cast<int32_t>(std::lround(sl.mix * 12800.0)));
        }
        auto keep = [&](uint8_t id, double want, const std::function<double(int64_t)>& dec, int64_t lo, int64_t hi) {
          int32_t* p = find(id, 31, 0);
          if (p && static_cast<float>(dec(*p)) == static_cast<float>(want)) return;
          set(id, 31, 0, static_cast<int32_t>(solveInt(lo, hi, want, dec)));
        };
        keep(192, t->volume, [](int64_t v) { return static_cast<float>(fx::glFaderFromGain(fx::flMixerGain(static_cast<double>(v)))); }, 0, 16000);
        keep(193, t->pan, [](int64_t v) { return static_cast<float>(std::clamp(v / 6400.0, -1.0, 1.0)); }, -6400, 6400);
        keep(194, t->stereoSep, [](int64_t v) { return static_cast<float>(std::clamp(-v / 64.0, -1.0, 1.0)); }, -64, 64);
        static const int kDefFreq[3] = {5777, 33145, 55825};
        for (int b = 0; b < 3; ++b) {
          keep(static_cast<uint8_t>(208 + b), t->eqGain[static_cast<size_t>(b)], [](int64_t v) { return static_cast<float>(std::clamp(v / 100.0, -18.0, 18.0)); }, -1800, 1800);
          const int db = b;
          keep(static_cast<uint8_t>(216 + b), t->eqFreq[static_cast<size_t>(b)], [db](int64_t v) {
            if (v == kDefFreq[db]) return static_cast<double>(kStripEqDefFreq[db]);
            return static_cast<double>(static_cast<float>(std::clamp(fx::flEqFreqHz(static_cast<double>(v)), 20.0, 20000.0)));
          }, 0, 65536);
          keep(static_cast<uint8_t>(224 + b), t->eqQ[static_cast<size_t>(b)], [](int64_t v) {
            if (v == 17500) return static_cast<double>(kStripEqDefQ);
            return static_cast<double>(static_cast<float>(std::clamp(fx::flInsertEqQ(static_cast<double>(v)), 0.1, 10.0)));
          }, 0, 65536);
        }
        // Route levels (kind 32, id = destination): sidechain-only routes are level 0.
        std::vector<Rec> kept;
        for (const auto& r : tr)
          if (r.kind != 32 && !(r.kind == 31 && r.id >= 64 && r.id < 192)) kept.push_back(r);
        for (const auto& sd : t->sends) {
          const int d = im.ins(sd.to);
          if (d < 0) continue;
          int32_t v;
          if (sd.sidechain) v = 0;
          else v = static_cast<int32_t>(solveInt(0, 16000, sd.level, [](int64_t x) { return static_cast<float>(std::clamp(fx::flMixerGain(static_cast<double>(x)), 0.0, 1.25)); }));
          // the original record when it decodes to the same level
          for (const auto& r : tr)
            if ((r.kind == 32 && r.id == d) || (r.kind == 31 && r.id == 64 + d)) {
              const bool sameLevel = sd.sidechain ? r.value == 0 : static_cast<float>(std::clamp(fx::flMixerGain(r.value), 0.0, 1.25)) == sd.level && r.value != 0;
              if (sameLevel) v = r.value;
            }
          if (v != 12800) kept.push_back({static_cast<uint8_t>(d), 32, 0, v});
          if (sd.preFader) rep.counts["mixer: pre-fader sends written post-fader"]++;
        }
        tr = std::move(kept);
      }
    }
    for (const auto& r : tr) recs.push_back({i, r});
  }
  // 225: loose records first (as FL), then the inserts'.
  ByteWriter b;
  for (const auto& e : stripPseudo(origin("mxp")))
    if (e.id == 225 && e.data.size() == 12) {
      // keep its place relative to FL 25's base
      b.bytes(e.data);
    }
  for (const auto& [i, r] : recs) {
    b.u32(0);
    b.u8(r.id);
    b.u8(r.kind);
    b.u16(static_cast<uint32_t>(((kMixerBase + i) << 6) | (r.slot & 0x3F)));
    b.i32(r.value);
  }
  w.data(225, b.b);
}

}  // namespace gl::flp::xp
