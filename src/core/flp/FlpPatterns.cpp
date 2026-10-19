// Gridline — FLP import: patterns and their notes (+ per-note properties).
//
// Note record (event 224), 24 bytes:
//   u32 position, u16 flags (0x08 = slide), u16 channel, u32 length, u16 key,
//   u16 group, u8 fine pitch (120 = 0, 10 cents per unit), u8 (unused),
//   u8 release velocity (0..128, 64 default; kept as is, 128 → 127), u8 colour / MIDI channel
//   (low 4 bits; +128 = imported from MIDI), u8 pan (0..128, 64 = centre),
//   u8 velocity (0..128), u8 mod X, u8 mod Y (0..255, 128 = neutral).
// Notes of length 0 were entered in the step sequencer.
#include <algorithm>
#include <cmath>
#include "Flp.h"

namespace gl::flp {

namespace {
Rgb flColor(uint32_t v) { return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF); }
// FL 0..128 scale → Gridline 0..127.
int to127(int v) { return std::clamp(static_cast<int>(std::lround(v * 127.0 / 128.0)), 0, 127); }
}  // namespace

void importPatterns(Context& c) {
  Song& s = c.song;
  int notes = 0, slides = 0, panned = 0, fine = 0, release = 0, modX = 0, modY = 0, colored = 0, steps = 0, fixedLengths = 0;
  const Tick stepLen = std::max<Tick>(1, c.tick(c.file.ppq / 4));
  // FL does not write patterns that hold nothing (no notes, name or colour),
  // but the playlist can still place them: such patterns are created empty.
  std::map<int, std::vector<Event>> all = c.sec.patterns;
  int placedEmpty = 0;
  for (const auto& [arr, aevs] : c.sec.arrangements)
    for (const auto& e : aevs) {
      if (e.id != 233) continue;
      for (uint32_t k : {80u, 60u, 32u}) {
        if (e.size % k) continue;
        bool ok = true;
        for (uint32_t o = 0; o + k <= e.size && ok; o += k) ok = e.u16(o + 4) == 0x5000;
        if (!ok) continue;
        for (uint32_t o = 0; o + k <= e.size; o += k) {
          int base = e.u16(o + 4), item = e.u16(o + 6);
          if (item > base && !all.count(item - base)) all[item - base], ++placedEmpty;
        }
        break;
      }
    }
  if (placedEmpty) c.report.counts["empty patterns (placed, not stored)"] = placedEmpty;
  for (const auto& [idx, evs] : all) {
    Pattern p;
    p.id = s.allocateId();
    p.name = "Pattern " + std::to_string(idx);
    Tick end = 0, fixedLen = 0;
    for (const auto& e : evs) {
      if (e.id == 193) p.name = e.text();
      if (e.id == 164) fixedLen = c.tick(e.value());  // fixed pattern length in ticks (0 = follows its notes)
      if (e.id == 150) p.color = flColor(e.value());
      if (e.id != 224) continue;
      for (uint32_t o = 0; o + 24 <= e.size; o += 24) {
        uint32_t pos = e.u32(o), len = e.u32(o + 8);
        int flags = e.u16(o + 4), chIdx = e.u16(o + 6), key = e.u16(o + 12);
        int finePitch = e.u8(o + 16), rel = e.u8(o + 18), mch = e.u8(o + 19), pan = e.u8(o + 20), vel = e.u8(o + 21);
        int mx = e.u8(o + 22), my = e.u8(o + 23);
        auto it = c.channelIds.find(chIdx);
        if (it == c.channelIds.end()) continue;
        Note n;
        n.start = c.tick(pos);
        if (len == 0) ++steps;  // step-sequencer note: plays one step
        n.length = len == 0 ? stepLen : std::max<Tick>(1, c.tick(len));
        n.pitch = std::clamp(key, 0, 127);
        n.velocity = to127(vel);
        n.pan = std::clamp(pan - 64, -kNotePanMax, kNotePanMax);
        n.fine = std::clamp((finePitch - 120) * 10, -kNoteFineMax, kNoteFineMax);
        n.release = std::clamp(rel, 0, 127);  // same scale (FL 0..128, 64 = default)
        n.modX = std::clamp(mx, 0, kNoteModMax);
        n.modY = std::clamp(my, 0, kNoteModMax);
        n.color = mch & 0x0F;
        n.slide = (flags & 0x08) != 0;
        slides += n.slide;
        panned += n.pan != 0;
        fine += n.fine != 0;
        release += n.release != kNoteReleaseDefault;
        modX += n.modX != kNoteModCentre;
        modY += n.modY != kNoteModCentre;
        colored += n.color != 0;
        p.notes[it->second].push_back(n);
        end = std::max(end, n.start + n.length);
        ++notes;
      }
    }
    for (auto& [ch, ns] : p.notes) std::stable_sort(ns.begin(), ns.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    const Tick bar = std::max<Tick>(1, s.barTicks());
    p.bars = std::max<int>(1, static_cast<int>(((fixedLen > 0 ? fixedLen : end) + bar - 1) / bar));
    fixedLengths += fixedLen > 0;
    s.patterns.push_back(p);
    c.patternIds[idx] = p.id;
  }
  c.report.counts["patterns"] = static_cast<int>(s.patterns.size());
  c.report.counts["notes"] = notes;
  auto count = [&](const char* k, int v) {
    if (v) c.report.counts[k] = v;
  };
  count("notes: slide", slides);
  count("notes: pan", panned);
  count("notes: fine pitch", fine);
  count("notes: release velocity", release);
  count("notes: mod X", modX);
  count("notes: mod Y", modY);
  count("notes: colour", colored);
  count("notes: step (length 0)", steps);
  count("patterns with a fixed length", fixedLengths);
}

}  // namespace gl::flp
