// Gridline — song and piano roll helpers (J-31). See SongTools.h.
#include "SongTools.h"
#include <algorithm>
#include <cmath>
#include "ChordTools.h"
#include "NoteTools.h"

namespace gl::ops {

// ---------------------------------------------------------------- key markers ----

KeyAt keyAt(const Song& s, Tick t) {
  KeyAt k;
  for (size_t i = 0; i < s.markers.size(); ++i) {
    const Marker& m = s.markers[i];
    if (!m.isKey() || m.pos > t) continue;
    if (k.marker < 0 || m.pos >= s.markers[static_cast<size_t>(k.marker)].pos) {
      k.root = m.keyRoot;
      k.scale = m.keyScale;
      k.marker = static_cast<int>(i);
    }
  }
  return k;
}

std::string keyName(int root, int scale) {
  const auto& sc = tools::scales();
  const std::string name = scale > 0 && scale < static_cast<int>(sc.size()) ? sc[static_cast<size_t>(scale)].name : "Chromatic";
  return chord::noteName(root) + " " + name;
}

int addKeyMarker(Document& d, Tick pos, int root, int scale) {
  Marker m;
  m.pos = std::max<Tick>(0, pos);
  m.keyRoot = ((root % 12) + 12) % 12;
  m.keyScale = std::clamp(scale, 0, static_cast<int>(tools::scales().size()) - 1);
  m.name = keyName(m.keyRoot, m.keyScale);
  return addMarker(d, m);
}

Tick patternSongPosition(const Song& s, ObjectId pattern) {
  Tick best = -1;
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Pattern && c.pattern == pattern && (best < 0 || c.start < best)) best = c.start;
  return std::max<Tick>(0, best);
}

KeyAt patternKey(const Song& s, ObjectId pattern) { return keyAt(s, patternSongPosition(s, pattern)); }

// --------------------------------------------------------- quantized jumps ----

Tick jumpGrid(const Song& s, int mode) {
  switch (mode) {
    case 1: return s.beatTicks();
    case 2: return s.barTicks();
    case 3: return s.barTicks() * 2;
    case 4: return s.barTicks() * 4;
    case 5: return s.barTicks() * 8;
    default: return 0;
  }
}

const char* jumpGridName(int mode) {
  static const char* const n[] = {"At once", "Next beat", "Next bar", "Next 2 bars", "Next 4 bars", "Next 8 bars"};
  return mode >= 0 && mode < kJumpGridCount ? n[mode] : "";
}

// ------------------------------------------------------------- MIDI snapping ----

int snapIncoming(int pitch, int root, int scale, bool on) {
  if (!on || scale <= 0) return pitch;
  return tools::snapPitchToScale(pitch, root, scale);
}

// ------------------------------------------------------------- ghost notes ----

GhostHit ghostNoteAt(const Pattern& p, ObjectId exclude, const std::set<ObjectId>& only, Tick t, int pitch) {
  GhostHit hit;
  Tick bestLen = 0;
  for (const auto& [ch, notes] : p.notes) {
    if (ch == exclude || (!only.empty() && !only.count(ch))) continue;
    for (size_t i = 0; i < notes.size(); ++i) {
      const Note& n = notes[i];
      if (n.pitch != pitch || t < n.start || t >= n.start + n.length) continue;
      if (hit.channel == kNoId || n.length < bestLen) {
        hit = {ch, static_cast<int>(i)};
        bestLen = n.length;
      }
    }
  }
  return hit;
}

// ---------------------------------------------------------- background waveform ----

std::vector<std::pair<float, float>> waveformColumns(const float* L, const float* R, size_t frames, double sampleRate,
                                                     double secPerTick, double offsetSec, double t0, double ticksPerCol,
                                                     int cols) {
  std::vector<std::pair<float, float>> out(static_cast<size_t>(std::max(0, cols)), {0.0f, 0.0f});
  if (!L || frames == 0 || sampleRate <= 0 || secPerTick <= 0 || ticksPerCol <= 0) return out;
  for (int k = 0; k < cols; ++k) {
    const double a = (offsetSec + (t0 + ticksPerCol * k) * secPerTick) * sampleRate;
    const double b = (offsetSec + (t0 + ticksPerCol * (k + 1)) * secPerTick) * sampleRate;
    if (b <= 0 || a >= static_cast<double>(frames)) continue;
    const size_t i0 = static_cast<size_t>(std::max(0.0, std::floor(a)));
    const size_t i1 = std::min(frames, static_cast<size_t>(std::max(std::ceil(b), std::floor(a) + 1)));
    // Long columns read a bounded number of frames (evenly spaced).
    const size_t span = i1 > i0 ? i1 - i0 : 0;
    const size_t step = std::max<size_t>(1, span / 256);
    float lo = 0, hi = 0;
    for (size_t i = i0; i < i1; i += step) {
      const float v = R ? 0.5f * (L[i] + R[i]) : L[i];
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    out[static_cast<size_t>(k)] = {lo, hi};
  }
  return out;
}

}  // namespace gl::ops
