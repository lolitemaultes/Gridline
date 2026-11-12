// Gridline - K-12: the playlist track "+" edits. See TrackPlus.h.
#include "TrackPlus.h"
#include <algorithm>
#include "Document.h"
#include "MenuOps.h"
#include "Meter.h"
#include "TrackModes.h"

namespace gl::trackplus {

namespace {

void setWhy(std::string* why, const std::string& text) {
  if (why) *why = text;
}

bool trackHasClips(const Song& s, int track) {
  for (const auto& c : s.playlist)
    if (c.track == track) return true;
  return false;
}

// The pattern + clip, inside the caller's group.
Started startPattern(Document& d, int track, Tick at, std::string* why) {
  Started out;
  const Song& s = d.song();
  const Channel* ch = trackmodes::linkedChannel(s, track);
  if (!ch) {
    setWhy(why, "Give the track an instrument first.");
    return out;
  }
  out.channel = ch->id;
  const std::string name = patternName(s, ch->name);
  const Rgb color = ch->color;
  out.pattern = d.addPattern(name);
  if (out.pattern == kNoId) {
    setWhy(why, "The project has as many patterns as it can hold.");
    return out;
  }
  d.editPattern(out.pattern, "pattern colour", [color](Pattern& p) { p.color = color; });
  const Pattern* p = d.song().pattern(out.pattern);
  const Tick len = p ? d.song().patternLength(*p) : d.song().barTicks();
  out.clip = d.addClip(out.pattern, track, freeSpot(d.song(), track, std::max<Tick>(0, at), len));
  if (out.clip == kNoId) setWhy(why, "No room for the pattern on the track.");
  return out;
}

}  // namespace

std::string patternName(const Song& s, const std::string& base) {
  if (base.empty()) return ops::nextPatternName(s);
  auto used = [&](const std::string& n) {
    for (const auto& p : s.patterns)
      if (p.name == n) return true;
    return false;
  };
  if (!used(base)) return base;
  for (int n = 2;; ++n) {
    std::string name = base + " " + std::to_string(n);
    if (!used(name)) return name;
  }
}

Tick freeSpot(const Song& s, int track, Tick at, Tick len) {
  const MeterMap mm(s);  // K-193: bar lines of the meter in force
  len = std::max<Tick>(1, len);
  Tick start = std::max<Tick>(0, at);
  for (int guard = 0; guard < 100000; ++guard) {
    Tick blockEnd = -1;
    for (const auto& c : s.playlist) {
      if (c.track != track) continue;
      const Tick c0 = c.start, c1 = c.start + std::max<Tick>(1, s.clipLength(c));
      if (c0 < start + len && c1 > start) blockEnd = std::max(blockEnd, c1);
    }
    if (blockEnd < 0) return start;
    start = mm.roundUpToBar(blockEnd);
  }
  return start;
}

Tick placeTick(const Song& s, Tick songPos, Tick viewStart, Tick viewEnd) {
  const MeterMap mm(s);  // K-193: bar lines of the meter in force
  if (songPos >= viewStart && songPos < viewEnd && songPos >= 0) return mm.barStartAt(songPos);
  return mm.roundUpToBar(std::max<Tick>(0, viewStart));
}

Started newPatternOnTrack(Document& d, int track, Tick at, std::string* why) {
  if (trackmodes::modeOf(d.song(), track) != TrackMode::Instrument) {
    setWhy(why, "Give the track an instrument first.");
    return {};
  }
  d.beginGroup("new pattern on track");
  Started out = startPattern(d, track, at, why);
  if (out.clip == kNoId) {
    d.cancelGroup();
    return {};
  }
  d.endGroup();
  return out;
}

Started assignChannel(Document& d, int track, ObjectId channel, bool startPat, Tick at, std::string* why) {
  const bool empty = !trackHasClips(d.song(), track);
  d.beginGroup("track instrument");
  if (!trackmodes::makeInstrumentTrack(d, track, channel, why)) {
    d.cancelGroup();
    return {};
  }
  Started out;
  out.channel = channel;
  if (startPat && empty) {
    Started p = startPattern(d, track, at, why);
    if (p.clip == kNoId) {
      d.cancelGroup();
      return {};
    }
    out = p;
  }
  d.endGroup();
  return out;
}

Started assignNewInstrument(Document& d, int track, const std::string& pluginType, bool startPat, Tick at, std::string* why) {
  const bool empty = !trackHasClips(d.song(), track);
  d.beginGroup("track instrument");
  const ObjectId ch = trackmodes::newInstrumentTrack(d, track, pluginType, why);
  if (ch == kNoId) {
    d.cancelGroup();
    return {};
  }
  Started out;
  out.channel = ch;
  if (startPat && empty) {
    Started p = startPattern(d, track, at, why);
    if (p.clip == kNoId) {
      d.cancelGroup();
      return {};
    }
    out = p;
  }
  d.endGroup();
  return out;
}

}  // namespace gl::trackplus
