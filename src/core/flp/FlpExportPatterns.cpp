// Gridline — FLP export: patterns (J-34).
//
// FL 25 writes each pattern twice: its notes (65 index, 224 note records,
// 223 controller events) before the channels, its metadata (65 index, 193
// name, 150 colour, 157, 158, 164 fixed length, 52, 171, 252, 253) after
// the first channel. Note record (24 bytes, see FlpPatterns.cpp): u32
// position, u16 flags (0x4000, +0x08 slide), u16 channel, u32 length (0 =
// step), u16 key, u16 group, u8 fine (120 = 0, 10 cents), u8, u8 release,
// u8 colour (low 4 bits), u8 pan (64 = centre), u8 velocity (MIDI velocity), u8 mod
// X, u8 mod Y. Step-sequencer steps are written as step notes (length 0).
#include <algorithm>
#include <cmath>
#include <limits>
#include "../NoteBend.h"
#include "../Params.h"
#include "../PatternAutomation.h"
#include "../../packs/plasma/plasma.h"
#include "FlpExportInternal.h"

namespace gl::flp::xp {

namespace {
int velocity127(int v) { return std::clamp(v, 0, 127); }  // J-74: same law as FlpPatterns.cpp
}  // namespace

Note decodeNoteRecord(const uint8_t* r, int, Tick stepLen) {
  auto u16 = [&](int o) { return static_cast<int>(r[o] | (r[o + 1] << 8)); };
  auto u32 = [&](int o) { return static_cast<uint32_t>(r[o]) | (static_cast<uint32_t>(r[o + 1]) << 8) | (static_cast<uint32_t>(r[o + 2]) << 16) | (static_cast<uint32_t>(r[o + 3]) << 24); };
  Note n;
  n.start = static_cast<Tick>(u32(0));
  const uint32_t len = u32(8);
  n.length = len == 0 ? stepLen : std::max<Tick>(1, static_cast<Tick>(len));
  n.step = len == 0;  // K-22: as the import reads it
  n.pitch = std::clamp(u16(12), 0, 127);
  n.velocity = velocity127(r[21]);
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
  r[21] = static_cast<uint8_t>(velocity127(n.velocity));  // J-74: the MIDI velocity, as FL stores it
  r[22] = static_cast<uint8_t>(std::clamp(n.modX, 0, 255));
  r[23] = static_cast<uint8_t>(std::clamp(n.modY, 0, 255));
  return r;
}

namespace {

// K-28: key + fine pitch (FL's 10 cent steps) closest to `pitch` (MIDI, fractional); returns how
// far the written pitch is from it (semitones).
double setPitch(Note& n, double pitch) {
  n.pitch = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
  n.fine = std::clamp(static_cast<int>(std::lround((pitch - n.pitch) * 10.0)) * 10, -kNoteFineMax, kNoteFineMax);
  return std::fabs(n.pitch + n.fine / 100.0 - pitch);
}

// K-AB: within 5 cents where the ticks allow: half of FL's fine pitch step, the most a held value
// can be off anyway.
constexpr double kSlideTolerance = 0.05, kFinePitchStep = 0.1;

// K-AB: what colour means on a channel, beyond grouping slides: a note's colour reaches these
// instruments and they act on it, so a note can't change colour there. Hosted plugins and MIDI Out
// get it as the MIDI channel, the slicer picks deck and direction by it, the FM synth portamento
// and key tracking, Junction hands it to what it hosts, FL's voltage controller reads it, note
// processors can route by it, Plasma
// reads it as the channel in MPE mode.
bool colourSelects(const Song& s, const Channel& c, int depth = 0) {
  const std::string& t = c.plugin.type;
  if (isHostedType(t) || t == "gl.midiout" || t == "gl.slicer" || t == "gl.matrixfm" || t == "gl.patcher" || t == "gl.cvctl" || !c.noteFx.empty()) return true;
  const auto mpe = static_cast<size_t>(packs::plasma::G_MpeOn);
  if (t == "gl.plasma" && mpe < c.plugin.params.size() && c.plugin.params[mpe] >= 0.5f) return true;
  if (depth < 4)
    for (ObjectId k : c.layer)
      if (const Channel* kid = s.channel(k); kid && colourSelects(s, *kid, depth + 1)) return true;
  return false;
}

struct ColourUse {
  bool selects = false;    // colourSelects
  bool colour0Busy = false;  // the channel's step row sounds in colour 0 (its notes have that colour)
  uint32_t elsewhere = 0;  // colours of this channel's notes in other patterns (clips can overlap: taken last)
};

// K-28: FL notes have no bend line; FL's equivalent is the slide note, which glides every sounding
// note of its channel and colour by the interval from the last note of that colour started, over
// its length (fine pitch in 10 cent steps). A line becomes slide notes (bend::toSlides) when only
// its own note would glide: nothing else of its colour sounds during the slides or starts between
// the note and its last slide, or the others are a chord starting with it and carrying the same
// line (a chord bending as one: one set of slides glides them all, measured from the last of them).
// K-AB: otherwise the note and its slides move to a colour free for the note's whole length, where
// colour means nothing else on the channel; only without one is the line left out, and counted.
// The user's own slide notes keep what they glide: a line they play over or count from stays put.
// FL's file has no place for the old colour, so the note reads back in the new one. The notes come
// back without lines (converted ones starting at the line's start value); the slides go to `slides`.
std::vector<Note> bendsAsSlides(const std::vector<Note>& ns, std::vector<Note>& slides, ExportReport& rep, const ColourUse& use) {
  std::vector<Note> out = ns;
  for (Note& n : out) n.bend.clear();
  std::vector<bool> done(ns.size(), false);
  std::vector<int> colour;  // as written so far
  colour.reserve(ns.size());
  for (const Note& n : ns) colour.push_back(n.color);
  uint32_t moveTargets = 0;
  auto noteEnd = [](const Note& o) { return o.start + std::max<Tick>(1, o.length); };
  int asSlides = 0, moved = 0, noColour = 0, selects = 0, underSlide = 0, onSlides = 0, curves = 0, steps = 0, rounded = 0, cents = 0, relaxed = 0;
  for (size_t i = 0; i < ns.size(); ++i) {
    const Note& n = ns[i];
    if (n.bend.empty() || done[i]) continue;
    if (n.slide) {  // a slide note glides others and plays no line of its own
      ++onSlides;
      continue;
    }
    std::vector<size_t> group;
    Tick end = n.start;
    for (size_t j = 0; j < ns.size(); ++j)
      if (!ns[j].slide && ns[j].color == n.color && ns[j].start == n.start && ns[j].bend == n.bend) {
        group.push_back(j);
        end = std::max(end, ns[j].start + std::max<Tick>(1, ns[j].length));
        done[j] = true;
      }
    const int groupSize = static_cast<int>(group.size());
    auto inGroup = [&](size_t j) { return std::find(group.begin(), group.end(), j) != group.end(); };
    const Note& ref = ns[group.back()];
    const double refPitch = ref.pitch + ref.fine / 100.0;
    const bend::SlidePlan plan = bend::toSlides(n.bend, static_cast<double>(end - n.start), kSlideTolerance, kFinePitchStep, refPitch);
    // The ticks the slides take; a line that only starts off its pitch has none.
    const Tick wStart = plan.pieces.empty() ? n.start : n.start + static_cast<Tick>(plan.pieces.front().tick);
    const Tick wEnd = plan.pieces.empty() ? n.start : n.start + static_cast<Tick>(plan.pieces.back().tick + plan.pieces.back().length);
    int col = n.color;
    bool crowded = col == 0 && use.colour0Busy && !plan.pieces.empty();
    for (size_t j = 0; j < ns.size() && !crowded && !plan.pieces.empty(); ++j) {
      const Note& o = ns[j];
      if (colour[j] != col || o.slide || inGroup(j)) continue;
      crowded = o.start < wEnd && (noteEnd(o) > wStart || o.start >= n.start);  // glided, or the slides would count from it
    }
    // A slide note of the colour over this note takes it over, in place or moved it plays another
    // sound. One after it that counts from it would get another interval if it moved away.
    bool slideOver = false, countsFromIt = false;
    for (size_t j = 0; j < ns.size(); ++j) {
      const Note& o = ns[j];
      if (!o.slide || colour[j] != col) continue;
      if (o.start < end && noteEnd(o) > n.start) slideOver = true;
      if (o.start < n.start) continue;
      bool newer = false;  // an ordinary note of the colour started since: the slide counts from that
      for (size_t k = 0; k < ns.size() && !newer; ++k)
        newer = !ns[k].slide && colour[k] == col && !inGroup(k) && ns[k].start > n.start && ns[k].start <= o.start;
      if (!newer) countsFromIt = true;
    }
    if (slideOver || (crowded && countsFromIt)) {
      underSlide += groupSize;
      continue;
    }
    if (crowded && use.selects) {
      selects += groupSize;
      continue;
    }
    if (crowded) {
      int best = -1, bestRank = 0;
      for (int c = 0; c < kNoteColors; ++c) {
        if (c == col || (c == 0 && use.colour0Busy)) continue;
        bool free = true, usedHere = false;
        for (size_t j = 0; j < ns.size() && free; ++j) {
          if (colour[j] != c) continue;
          usedHere = true;
          free = !ns[j].slide && !(ns[j].start < end && noteEnd(ns[j]) > n.start);
        }
        if (!free) continue;
        // Colours this channel doesn't use elsewhere first (clips can overlap), 14 and 15 last (FL's
        // synths read those two), and a colour lines already moved to before one the user chose.
        const int rank = (((use.elsewhere >> c) & 1u) ? 4 : 0) + (c >= 14 ? 2 : 0) + (usedHere && !((moveTargets >> c) & 1u) ? 1 : 0);
        if (best < 0 || rank < bestRank) best = c, bestRank = rank;
      }
      if (best < 0) {
        noColour += groupSize;
        continue;
      }
      col = best;
      moveTargets |= 1u << best;
      for (size_t j : group) colour[j] = best, out[j].color = best;
      moved += groupSize;
    }
    bool off = plan.offGrid;
    for (size_t j : group) off = setPitch(out[j], ns[j].pitch + ns[j].fine / 100.0 + plan.start) > 0.005 || off;
    for (const bend::SlidePiece& pc : plan.pieces) {
      Note sl(n.start + static_cast<Tick>(pc.tick), std::max<Tick>(1, static_cast<Tick>(pc.length)), 60, n.velocity);
      off = setPitch(sl, refPitch + pc.target) > 0.005 || off;
      sl.color = col;
      sl.slide = true;
      slides.push_back(sl);
    }
    asSlides += groupSize;
    curves += plan.curves;
    steps += plan.steps;
    rounded += plan.rounded;
    cents += off;
    relaxed += plan.relaxed;
  }
  auto count = [&](const char* what, int v) {
    if (v > 0) rep.counts[what] += v;
  };
  count("notes: pitch bend lines written as slide notes", asSlides);
  count("notes: pitch bend curves written as several shorter slides (approximated)", curves);
  count("notes: pitch bend steps written as one-tick slides", steps);
  count("notes: pitch bend points moved to whole ticks", rounded);
  count("notes: pitch bend lines rounded to 10 cent fine pitch steps", cents);
  count("notes: pitch bend lines over the limit of 64 slides (a wider tolerance was used)", relaxed);
  count("notes: pitch bend lines moved to a free note colour (their slides would glide other notes of the old colour)", moved);
  count("notes: pitch bend lines not written (other notes of their colour sound with them and no note colour is free)", noColour);
  count("notes: pitch bend lines not written (other notes of their colour sound with them and this instrument reads note colours)", selects);
  count("notes: pitch bend lines not written (a slide note of their colour plays over them or counts from them)", underSlide);
  count("notes: pitch bend lines on slide notes not written (slide notes play no line)", onSlides);
  return out;
}

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
        std::vector<Note> slides;  // K-28: bend lines as slide notes
        ColourUse use;
        if (std::any_of(ns.begin(), ns.end(), [](const Note& x) { return !x.bend.empty(); })) {
          if (const Channel* c = s.channel(cid)) use.selects = colourSelects(s, *c);
          if (auto row = p->steps.find(cid); row != p->steps.end())
            use.colour0Busy = std::any_of(row->second.begin(), row->second.end(), [](uint8_t v) { return v != 0; });
          for (const Pattern& q : s.patterns) {
            if (q.id == pid) continue;
            if (auto it = q.notes.find(cid); it != q.notes.end())
              for (const Note& x : it->second) use.elsewhere |= 1u << (x.color & 15);
          }
        }
        const std::vector<Note> written = bendsAsSlides(ns, slides, rep, use);
        for (const Note& n : written) {
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
          // K-22: a step note one FL step long goes back as a step note (length 0); longer or
          // shorter it is an ordinary note there too.
          if (n.step && n.length == stepLen && s.ticksPerStep(*p) == stepLen) {
            putU32(rec, 8, 0);
            rep.counts["piano roll step notes (as step notes)"]++;
          }
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
        // After the channel's notes: at one position the sort keeps them first, so a slide that
        // starts with its note glides it.
        for (const Note& sl : slides) all.push_back({sl.start, fl, sl.pitch, noteRecord(sl, fl, nullptr)});
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
        // J-173: the clip as the parameter moves (range applied, loop unrolled over the pattern; FL's
        // events step, so curved segments are sampled every 1/32 note)
        const std::vector<AutoPoint> lp = patternClipSteps(l, s.patternContentLength(*p), std::max<Tick>(1, s.ppq / 8));
        // Channel mute (0x07): only pattern events address it (1 = muted).
        if (const int ch = im.ch(l.owner); ch >= 0 && l.index == static_cast<uint32_t>(ChannelParam::Mute)) {
          for (const auto& pt : lp) {
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
        for (const auto& pt : lp) {
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
  const Tick stepLen = std::max<Tick>(1, s.ppq / 4);
  for (const auto& [idx, pid] : patterns) {
    const Pattern* p = s.pattern(pid);
    if (!p) continue;
    const Tick bar = std::max<Tick>(1, s.patternBarTicks(*p));  // K-179: the pattern's own bar
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
        // keep FL's order: 193 150 157 158 (26 162: loop mode) 164 …
        static const uint8_t order[] = {193, 150, 157, 158, 26, 162, 164, 52, 171, 252, 253};
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
      // Q-1: FL's pattern loop mode: 26 (byte, 1) and 162 (1), as FL writes them.
      if (p->loop) {
        setOrAdd(26, Bytes{1});
        if (!findRaw(meta, 162)) setOrAdd(162, dw(1));
      } else {
        meta.erase(std::remove_if(meta.begin(), meta.end(), [](const RawEvent& e) { return e.id == 26 || e.id == 162; }), meta.end());
      }
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
      // K-179: the pattern's meter as FL's pattern time signature marker at its start (148 kind 8,
      // 33 / 34). One already there is updated; one is added only for a pattern with its own meter.
      const int num = s.patternMeterNum(*p), den = s.patternMeterDen(*p);
      bool found = false;
      for (size_t i = 0; i < meta.size(); ++i) {
        if (meta[i].id != 148 || meta[i].value() != (8u << 24)) continue;
        found = true;
        for (size_t j = i + 1; j < meta.size() && meta[j].id != 148; ++j) {
          if (meta[j].id == 33) meta[j].data = Bytes{static_cast<uint8_t>(num)};
          if (meta[j].id == 34) meta[j].data = Bytes{static_cast<uint8_t>(den)};
        }
        break;
      }
      if (!found && p->ownMeter()) {
        meta.push_back(RawEvent{148, dw(8u << 24)});
        meta.push_back(RawEvent{33, Bytes{static_cast<uint8_t>(num)}});
        meta.push_back(RawEvent{34, Bytes{static_cast<uint8_t>(den)}});
      }
    }
    w.word(65, static_cast<uint32_t>(idx));
    for (const auto& e : meta) w.raw(e);
  }
}

}  // namespace gl::flp::xp
