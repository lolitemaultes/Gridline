// Gridline — FLP export: patterns (J-34).
//
// FL 25 writes each pattern twice: its notes (65 index, 224 note records,
// 223 controller events) before the channels, its metadata (65 index, 193
// name, 150 colour, 157, 158, 164 fixed length, 52, 171, 252, 253) after
// the first channel. Note record (24 bytes, see FlpPatterns.cpp): u32
// position, u16 flags (0x4000, +0x08 slide), u16 channel, u32 length (0 =
// step), u16 key, u16 group, u8 fine (120 = 0, 10 cents), u8, u8 release,
// u8 colour (low 4 bits), u8 pan (64 = centre), u8 velocity (0..128), u8 mod
// X, u8 mod Y. Step-sequencer steps are written as step notes (length 0).
#include <algorithm>
#include <cmath>
#include <limits>
#include "../Params.h"
#include "FlpExportInternal.h"

namespace gl::flp::xp {

namespace {
int to127(int v) { return std::clamp(static_cast<int>(std::lround(v * 127.0 / 128.0)), 0, 127); }
}  // namespace

Note decodeNoteRecord(const uint8_t* r, int, Tick stepLen) {
  auto u16 = [&](int o) { return static_cast<int>(r[o] | (r[o + 1] << 8)); };
  auto u32 = [&](int o) { return static_cast<uint32_t>(r[o]) | (static_cast<uint32_t>(r[o + 1]) << 8) | (static_cast<uint32_t>(r[o + 2]) << 16) | (static_cast<uint32_t>(r[o + 3]) << 24); };
  Note n;
  n.start = static_cast<Tick>(u32(0));
  const uint32_t len = u32(8);
  n.length = len == 0 ? stepLen : std::max<Tick>(1, static_cast<Tick>(len));
  n.pitch = std::clamp(u16(12), 0, 127);
  n.velocity = to127(r[21]);
  n.pan = std::clamp(static_cast<int>(r[20]) - 64, -kNotePanMax, kNotePanMax);
  n.fine = std::clamp((static_cast<int>(r[16]) - 120) * 10, -kNoteFineMax, kNoteFineMax);
  n.release = std::clamp(static_cast<int>(r[18]), 0, 127);
  n.modX = r[22];
  n.modY = r[23];
  n.color = r[19] & 0x0F;
  n.slide = (u16(4) & 0x08) != 0;
  n.group = u16(14);
  return n;
}

Bytes noteRecord(const Note& n, int flChannel, const Bytes* tmpl) {
  Bytes r(24, 0);
  if (tmpl && tmpl->size() == 24) r = *tmpl;
  else {
    putU16(r, 4, 0x4000);
    r[16] = 120;
    r[18] = 64;
    r[20] = 64;
    r[22] = 128;
    r[23] = 128;
  }
  putU32(r, 0, static_cast<uint32_t>(std::max<Tick>(0, n.start)));
  putU16(r, 4, static_cast<uint32_t>((getU16(r, 4) & ~0x08) | (n.slide ? 0x08 : 0)));
  putU16(r, 6, static_cast<uint32_t>(flChannel));
  putU32(r, 8, static_cast<uint32_t>(std::max<Tick>(1, n.length)));
  putU16(r, 12, static_cast<uint32_t>(std::clamp(n.pitch, 0, 127)));
  putU16(r, 14, static_cast<uint32_t>(std::clamp(n.group, 0, 65535)));  // J-39: note group
  r[16] = static_cast<uint8_t>(std::clamp(120 + static_cast<int>(std::lround(n.fine / 10.0)), 0, 240));
  r[18] = static_cast<uint8_t>(std::clamp(n.release, 0, 128));
  r[19] = static_cast<uint8_t>((r[19] & 0xF0) | (n.color & 0x0F));
  r[20] = static_cast<uint8_t>(std::clamp(n.pan + 64, 0, 128));
  {  // velocity 0..127 → 0..128 (the value the importer maps back exactly)
    int v = std::clamp(static_cast<int>(std::lround(n.velocity * 128.0 / 127.0)), 0, 128);
    for (int d : {0, -1, 1})
      if (v + d >= 0 && v + d <= 128 && to127(v + d) == n.velocity) {
        v += d;
        break;
      }
    r[21] = static_cast<uint8_t>(v);
  }
  r[22] = static_cast<uint8_t>(std::clamp(n.modX, 0, 255));
  r[23] = static_cast<uint8_t>(std::clamp(n.modY, 0, 255));
  return r;
}

namespace {

}  // namespace

void Exporter::writePatternNotes() {
  const Tick stepLen = std::max<Tick>(1, s.ppq / 4);
  for (const auto& [idx, pid] : patterns) {
    const Pattern* p = s.pattern(pid);
    if (!p) continue;
    const EventList orig = origin(originKey("pat", pid));
    const EventList pe = stripPseudo(orig);
    const bool same = unchanged(orig, fingerprintPattern(*p, s, im));
    EventList notes;
    if (same) {
      for (const auto& e : pe)
        if (e.id == 224 || e.id == 223) notes.push_back(e);
    } else {
      // Original records by (channel, position, key) to keep their unknown bytes.
      std::multimap<std::tuple<int, uint32_t, int>, Bytes> old;
      std::vector<Bytes> autoNotes;  // notes on automation channels (they trigger the automation)
      for (const auto& e : pe)
        if (e.id == 224)
          for (size_t o = 0; o + 24 <= e.data.size(); o += 24) {
            Bytes r(e.data.begin() + static_cast<std::ptrdiff_t>(o), e.data.begin() + static_cast<std::ptrdiff_t>(o) + 24);
            const int ch = getU16(r, 6);
            old.emplace(std::make_tuple(ch, getU32(r, 0), static_cast<int>(getU16(r, 12))), r);
          }
      // FL channel index each original record pointed at → still an automation channel?
      for (const auto& [k, r] : old) {
        const int ch = std::get<0>(k);
        for (const auto& [aid, afl] : im.automation)
          if (afl == ch) autoNotes.push_back(r);
      }
      struct N {
        Tick pos;
        int ch;
        int key;
        Bytes rec;
        int lane = -1;  // notes carrying pattern automation lane k: kept in lane order at one position
      };
      std::vector<N> all;
      for (const auto& [cid, ns] : p->notes) {
        const int fl = im.ch(cid);
        if (fl < 0) continue;
        for (const Note& n : ns) {
          const Bytes* tmpl = nullptr;
          auto range = old.equal_range(std::make_tuple(fl, static_cast<uint32_t>(std::max<Tick>(0, n.start)), n.pitch));
          for (auto it = range.first; it != range.second; ++it) {
            if (decodeNoteRecord(it->second.data(), s.ppq, stepLen) == n) {
              tmpl = &it->second;
              break;
            }
            if (!tmpl) tmpl = &it->second;
          }
          Bytes rec;
          if (tmpl && decodeNoteRecord(tmpl->data(), s.ppq, stepLen) == n) rec = *tmpl;
          else rec = noteRecord(n, fl, tmpl);
          if (n.porta) rep.counts["notes: porta flag not written"]++;
          if (n.muted) rep.counts["notes: muted notes not written as muted"]++;
          all.push_back({n.start, fl, n.pitch, std::move(rec)});
          if (tmpl)
            for (auto it = range.first; it != range.second; ++it)
              if (&it->second == tmpl) {
                old.erase(it);
                break;
              }
        }
      }
      // Step-sequencer rows → step notes (length 0).
      const Tick tps = s.ticksPerStep(*p);
      for (const auto& [cid, row] : p->steps) {
        const int fl = im.ch(cid);
        if (fl < 0 || tps <= 0) continue;
        auto props = p->stepProps.find(cid);
        for (size_t i = 0; i < row.size(); ++i) {
          if (!row[i]) continue;
          Note n;
          n.start = static_cast<Tick>(i) * tps;
          n.velocity = row[i];
          n.pitch = 60;
          if (props != p->stepProps.end() && i < props->second.size()) {
            const StepProps& sp = props->second[i];
            if (sp.pitch >= 0) n.pitch = sp.pitch;
            n.pan = sp.pan;
            n.release = sp.release;
            n.fine = sp.fine;
            n.modX = sp.modX;
            n.modY = sp.modY;
            n.start += tps * sp.shift / 100;
          }
          Bytes rec = noteRecord(n, fl, nullptr);
          putU32(rec, 8, 0);
          all.push_back({n.start, fl, n.pitch, std::move(rec)});
          rep.counts["pattern steps (as step notes)"]++;
        }
      }
      for (const Bytes& r : autoNotes) all.push_back({static_cast<Tick>(getU32(r, 0)), getU16(r, 6), getU16(r, 12), r});
      // J-39: lanes that are copies of an automation clip's curve (what notes on
      // an automation channel import as) → those notes again, when no origin
      // carries them: each copy starts where the clip's first point lands.
      std::set<size_t> laneAsNotes;
      if (autoNotes.empty())
        for (size_t li = 0; li < p->automation.size(); ++li) {
          const PatternAutomation& l = p->automation[li];
          for (const auto& [aid, afl] : im.automation) {
            const AutomationClip* a = s.automation(aid);
            if (!a || a->owner != l.owner || a->index != l.index || a->points.empty() || l.points.empty()) continue;
            const size_t n = a->points.size();
            std::vector<Tick> starts;
            bool ok = l.points.size() % n == 0;
            for (size_t i = 0; ok && i < l.points.size(); i += n) {
              const Tick st = l.points[i].pos - a->points[0].pos;
              for (size_t k = 0; ok && k < n; ++k) {
                const AutoPoint& q = l.points[i + k];
                const AutoPoint& p0 = a->points[k];
                const float v = std::clamp(a->min + p0.value * (a->max - a->min), 0.0f, 1.0f);
                ok = q.pos == st + p0.pos && q.value == v && q.tension == p0.tension && q.shape == p0.shape;
              }
              starts.push_back(st);
            }
            if (!ok) continue;
            for (Tick st : starts) {
              Note n;
              n.start = std::max<Tick>(0, st);
              n.pitch = 60;
              n.length = std::max<Tick>(1, a->points.back().pos);
              all.push_back({n.start, afl, n.pitch, noteRecord(n, afl, nullptr), static_cast<int>(li)});
            }
            laneAsNotes.insert(li);
            rep.counts["pattern automation as notes on automation channels"]++;
            break;
          }
        }
      std::stable_sort(all.begin(), all.end(), [](const N& a, const N& b) {
        if (a.pos != b.pos) return a.pos < b.pos;
        if ((a.lane >= 0) != (b.lane >= 0)) return a.lane >= 0;  // lane notes first, in lane order (the import makes lanes in note order)
        return a.lane >= 0 ? a.lane < b.lane : a.ch < b.ch;
      });
      if (!all.empty()) {
        ByteWriter b;
        for (const auto& n : all) b.bytes(n.rec);
        notes.push_back(RawEvent{224, b.b});
      }
      // Pattern automation → 223 controller events (lanes made from notes on
      // automation channels are carried by those notes).
      ByteWriter ev;
      for (size_t li = 0; li < p->automation.size(); ++li) {
        const PatternAutomation& l = p->automation[li];
        if (laneAsNotes.count(li)) continue;
        bool fromTrigger = false;
        for (const auto& [aid, afl] : im.automation)
          if (const AutomationClip* a = s.automation(aid); a && a->owner == l.owner && a->index == l.index && !autoNotes.empty()) fromTrigger = true;
        if (fromTrigger) continue;
        uint32_t target = 0;
        // Channel mute (0x07): only pattern events address it (1 = muted).
        if (const int ch = im.ch(l.owner); ch >= 0 && l.index == static_cast<uint32_t>(ChannelParam::Mute)) {
          for (const auto& pt : l.points) {
            ev.u32(static_cast<uint32_t>(std::max<Tick>(0, pt.pos)));
            ev.u32((static_cast<uint32_t>(ch) << 16) | 0x07);
            ev.i32(pt.value >= 0.5f ? 1 : 0);
          }
          continue;
        }
        if (!flTarget(ParamId{l.owner, l.index}, target)) {
          rep.counts["pattern automation lanes without FL target"]++;
          rep.warn("pattern '" + p->name + "': automation of " + paramDisplayName(s, ParamId{l.owner, l.index}) + " has no FL target");
          continue;
        }
        for (const auto& pt : l.points) {
          const double fl = flValue(target, pt.value);
          if (!std::isfinite(fl)) continue;
          int32_t raw;
          const uint32_t lo = target & 0xFFFF;
          if (lo >= 0x8000) {  // plugin parameter: its own integer range (rawNorm)
            const int ch = static_cast<int>(target >> 16);
            const PluginMapping* m = nullptr;
            if (target >> 16 < 0x2000) {
              for (const auto& [id, fi] : im.channel)
                if (fi == ch)
                  if (auto it = writtenPlugins.find({id, -1}); it != writtenPlugins.end()) m = &it->second;
            } else {
              const int rel = static_cast<int>(target >> 16) - kMixerBase * 64;
              for (const auto& [id, ins] : im.insert)
                if (ins == rel / 64)
                  if (auto it = writtenPlugins.find({id, rel % 64}); it != writtenPlugins.end()) m = &it->second;
            }
            if (!m || !m->rawNorm) continue;
            const int flp = static_cast<int>(lo - 0x8000);
            int64_t a = -(1 << 24), z = 1 << 24;
            double va = m->rawNorm(flp, static_cast<int32_t>(a)), vz = m->rawNorm(flp, static_cast<int32_t>(z));
            if (!std::isfinite(va) || !std::isfinite(vz)) continue;
            while (z - a > 1) {
              const int64_t mid = (a + z) / 2;
              const double v = m->rawNorm(flp, static_cast<int32_t>(mid));
              if ((v < fl) == (vz > va)) a = mid;
              else z = mid;
            }
            raw = static_cast<int32_t>(std::fabs(m->rawNorm(flp, static_cast<int32_t>(a)) - fl) <= std::fabs(m->rawNorm(flp, static_cast<int32_t>(z)) - fl) ? a : z);
          } else if (lo == 0x07 || lo == 0x1F00) {
            raw = fl >= 0.5 ? 1 : 0;
          } else {
            raw = static_cast<int32_t>(std::lround(fl * 12800.0));
          }
          ev.u32(static_cast<uint32_t>(std::max<Tick>(0, pt.pos)));
          ev.u32(target);
          ev.i32(raw);
        }
      }
      if (ev.size()) notes.push_back(RawEvent{223, ev.b});
    }
    if (notes.empty()) continue;
    w.word(65, static_cast<uint32_t>(idx));
    for (const auto& e : notes) w.raw(e);
  }
}

void Exporter::writePatternMeta() {
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick stepLen = std::max<Tick>(1, s.ppq / 4);
  for (const auto& [idx, pid] : patterns) {
    const Pattern* p = s.pattern(pid);
    if (!p) continue;
    const EventList orig = origin(originKey("pat", pid));
    const EventList pe = stripPseudo(orig);
    const bool same = unchanged(orig, fingerprintPattern(*p, s, im));
    EventList meta;
    for (const auto& e : pe)
      if (e.id != 224 && e.id != 223 && e.id != 65) meta.push_back(e);
    if (!same) {
      auto setOrAdd = [&](uint8_t id, const Bytes& b) {
        for (auto& e : meta)
          if (e.id == id) {
            e.data = b;
            return;
          }
        // keep FL's order: 193 150 157 158 164 …
        static const uint8_t order[] = {193, 150, 157, 158, 164, 52, 171, 252, 253};
        auto rank = [&](uint8_t x) {
          for (size_t i = 0; i < sizeof order; ++i)
            if (order[i] == x) return static_cast<int>(i);
          return 99;
        };
        auto it = std::find_if(meta.begin(), meta.end(), [&](const RawEvent& e) { return rank(e.id) > rank(id); });
        meta.insert(it, RawEvent{id, b});
      };
      auto dw = [](uint32_t v) {
        Bytes b(4);
        putU32(b, 0, v);
        return b;
      };
      const bool hadName = findRaw(meta, 193) != nullptr;
      if (hadName || p->name != "Pattern " + std::to_string(idx)) setOrAdd(193, utf16z(p->name));
      if (findRaw(meta, 150) || p->color != Pattern{}.color) setOrAdd(150, dw(flColor(p->color)));
      if (!findRaw(meta, 157)) setOrAdd(157, dw(0xFFFFFFFFu));
      if (!findRaw(meta, 158)) setOrAdd(158, dw(0xFFFFFFFFu));
      // Fixed length when the pattern is longer than its content.
      Tick end = 0;
      for (const auto& [c, ns] : p->notes)
        for (const Note& n : ns) end = std::max(end, n.start + n.length);
      const Tick tps = s.ticksPerStep(*p);
      for (const auto& [c, row] : p->steps)
        for (size_t i = 0; i < row.size(); ++i)
          if (row[i]) end = std::max(end, static_cast<Tick>(i) * tps + stepLen);
      const Tick content = std::clamp<Tick>((end + bar - 1) / bar, 1, kMaxPatternBars);
      Tick fixed = 0;
      if (const RawEvent* e = findRaw(meta, 164)) fixed = e->value();
      const Tick fromFixed = std::clamp<Tick>(((fixed > 0 ? fixed : end) + bar - 1) / bar, 1, kMaxPatternBars);
      if (fromFixed != p->bars) fixed = p->bars == content ? 0 : static_cast<Tick>(p->bars) * bar;
      setOrAdd(164, dw(static_cast<uint32_t>(fixed)));
    }
    w.word(65, static_cast<uint32_t>(idx));
    for (const auto& e : meta) w.raw(e);
  }
}

}  // namespace gl::flp::xp
