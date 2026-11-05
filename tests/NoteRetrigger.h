// Two clips of one pattern back to back, with a long note ending where the next clip's same note
// starts: the new note must sound in a hosted instrument, whatever the instrument goes by. Most
// plugins go by the key, so a note-off arriving after the new note-on ends the new note.
// Shared by the host tests of every format. Set GRIDLINE_NOTE_TRACE to print what the instrument
// received.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Check.h"
#include "NoteLog.h"
#include "core/Document.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Render.h"

namespace gl::test {

constexpr int kRetrigRate = 48000;
constexpr int kRetrigBarFrames = 96000;  // one bar at 120 BPM, on the edge of 64, 512 and 1000 frame blocks
constexpr int kRetrigKey = 69;

enum class RetrigScene {
  EndsAtClipEnd,   // the note fills the clip exactly
  RunsPastClip,    // the note is longer than the clip (its clip ends it)
  EndsBeforeClip,  // the note stops short of the clip's end
  OtherPitch,      // the second clip plays another pitch
  OtherChannel,    // the second clip plays the same pitch on another channel
  LoopedClip,      // one clip of two passes over the pattern
  PatternLoop,     // the pattern alone, looping
  OverlapInside,   // same pitch, the first note still held when the second starts
  StackedSamePlace // two of the same note at the same place (both stay, as they always did)
};

struct RetrigResult {
  std::vector<float> audio;
  GridlineNoteLog log;
};

inline const char* retrigName(RetrigScene s) {
  switch (s) {
    case RetrigScene::EndsAtClipEnd: return "note ends at the clip end";
    case RetrigScene::RunsPastClip: return "note runs past the clip end";
    case RetrigScene::EndsBeforeClip: return "note ends before the clip end";
    case RetrigScene::OtherPitch: return "second clip another pitch";
    case RetrigScene::OtherChannel: return "second clip another channel";
    case RetrigScene::LoopedClip: return "one clip looping its pattern";
    case RetrigScene::PatternLoop: return "pattern loop";
    case RetrigScene::OverlapInside: return "same pitch overlapping inside the pattern";
    case RetrigScene::StackedSamePlace: return "same note stacked";
  }
  return "";
}

inline Song retrigSong(const std::string& type, RetrigScene scene) {
  Document d(makeEmptySong());
  const Tick bar = d.song().barTicks();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = d.addChannel("hosted", type);
  const auto note = [](Tick start, Tick len, int key) { return Note{start, len, key, 100}; };
  switch (scene) {
    case RetrigScene::EndsAtClipEnd:
      d.addNote(pat, ch, note(0, bar, kRetrigKey));
      d.addClip(pat, 0, 0), d.addClip(pat, 0, bar);
      break;
    case RetrigScene::RunsPastClip:
      d.addNote(pat, ch, note(0, 2 * bar, kRetrigKey));
      d.setPatternLength(pat, 1, 16);
      d.addClip(pat, 0, 0, bar), d.addClip(pat, 0, bar, bar);
      break;
    case RetrigScene::EndsBeforeClip:
      d.addNote(pat, ch, note(0, bar - bar / 16, kRetrigKey));
      d.addClip(pat, 0, 0), d.addClip(pat, 0, bar);
      break;
    case RetrigScene::OtherPitch: {
      const ObjectId p2 = d.addPattern("other");
      d.addNote(pat, ch, note(0, bar, kRetrigKey));
      d.addNote(p2, ch, note(0, bar, kRetrigKey + 3));
      d.addClip(pat, 0, 0), d.addClip(p2, 0, bar);
      break;
    }
    case RetrigScene::OtherChannel: {
      const ObjectId p2 = d.addPattern("other");
      const ObjectId ch2 = d.addChannel("hosted 2", type);
      d.addNote(pat, ch, note(0, bar, kRetrigKey));
      d.addNote(p2, ch2, note(0, bar, kRetrigKey));
      d.addClip(pat, 0, 0), d.addClip(p2, 0, bar);
      break;
    }
    case RetrigScene::LoopedClip:
      d.addNote(pat, ch, note(0, bar, kRetrigKey));
      d.addClip(pat, 0, 0, 2 * bar);
      break;
    case RetrigScene::PatternLoop:
      d.addNote(pat, ch, note(0, bar, kRetrigKey));
      break;
    case RetrigScene::OverlapInside:
      d.addNote(pat, ch, note(0, bar + bar / 2, kRetrigKey));
      d.addNote(pat, ch, note(bar, bar, kRetrigKey));
      d.setPatternLength(pat, 2, 16);
      d.addClip(pat, 0, 0);
      break;
    case RetrigScene::StackedSamePlace:
      d.addNote(pat, ch, note(0, bar, kRetrigKey));
      d.addNote(pat, ch, note(0, bar, kRetrigKey));
      d.addClip(pat, 0, 0), d.addClip(pat, 0, bar);
      break;
  }
  Song s = d.song();
  s.tempo = 120;
  return s;
}

// The engine as the app runs it, `frames` frames in blocks of `block`.
inline RetrigResult retrigRealtime(const std::string& type, RetrigScene scene, int block, int frames, GridlineNoteLog* log) {
  const Song s = retrigSong(type, scene);
  RetrigResult r;
  if (log) *log = GridlineNoteLog{};
  ProgramBuilder builder;
  builder.setSampleRate(kRetrigRate);
  {
    AudioEngine eng{kRetrigRate};
    eng.setProgram(builder.build(s));
    eng.setMode(scene == RetrigScene::PatternLoop ? PlayMode::Pattern : PlayMode::Song);
    if (scene == RetrigScene::PatternLoop) eng.setPattern(s.patterns[0].id);
    eng.play();
    r.audio.resize(static_cast<size_t>(frames));
    std::vector<float> R(static_cast<size_t>(block));
    for (int pos = 0; pos < frames; pos += block) {
      const int len = std::min(block, frames - pos);
      eng.process(r.audio.data() + pos, R.data(), len);
    }
    if (log) r.log = *log;
  }
  return r;
}

inline RetrigResult retrigOffline(const std::string& type, RetrigScene scene, GridlineNoteLog* log) {
  const Song s = retrigSong(type, scene);
  RetrigResult r;
  if (log) *log = GridlineNoteLog{};
  RenderOptions o;
  o.sampleRate = kRetrigRate;
  o.channels = 1;
  o.tail = 0;
  if (scene == RetrigScene::PatternLoop) o.patternMode = true, o.pattern = s.patterns[0].id;
  AudioBuffer out = renderSong(s, o);
  r.audio.assign(out.plane(0), out.plane(0) + out.frames);
  if (log) r.log = *log;
  return r;
}

// Whether an instrument that goes by the key alone holds `key` at sample `at`.
inline bool retrigKeyHeld(const GridlineNoteLog& l, int key, int64_t at) {
  bool held = false;
  for (int i = 0; i < l.count; ++i)
    if (l.ev[i].key == key && l.ev[i].at <= at) held = l.ev[i].on;
  return held;
}
inline int64_t retrigFirstOn(const GridlineNoteLog& l) {
  for (int i = 0; i < l.count; ++i)
    if (l.ev[i].on) return l.ev[i].at;
  return -1;
}
inline float retrigPeak(const std::vector<float>& v, int from, int to) {
  float p = 0;
  for (int i = std::max(0, from); i < to && i < static_cast<int>(v.size()); ++i) p = std::max(p, std::fabs(v[static_cast<size_t>(i)]));
  return p;
}
inline std::string retrigTrace(const GridlineNoteLog& l) {
  std::string t;
  const int64_t base = retrigFirstOn(l);
  for (int i = 0; i < l.count; ++i)
    t += std::string(i ? " " : "") + (l.ev[i].on ? "on" : "off") + "@" + std::to_string(l.ev[i].at - base) + "/k" + std::to_string(l.ev[i].key) + "/id" + std::to_string(l.ev[i].id);
  return t;
}

// Every scene, in blocks that put the junction on a block edge and in the middle of one, in the
// engine as it plays and in an offline render. `log`: what the plugin received (nullptr when the
// format gives no way to see it: only the sound is checked). `minLevel`: the level the note must
// keep (a window well after the note's attack, past any release of a note that was cut at once).
inline void checkRetrigger(const char* format, const std::string& type, GridlineNoteLog* log, float minLevel = 0.01f) {
  const bool trace = std::getenv("GRIDLINE_NOTE_TRACE") != nullptr;
  const int bar = kRetrigBarFrames;
  const RetrigScene scenes[] = {RetrigScene::EndsAtClipEnd, RetrigScene::RunsPastClip,  RetrigScene::EndsBeforeClip,
                                RetrigScene::OtherPitch,    RetrigScene::OtherChannel,  RetrigScene::LoopedClip,
                                RetrigScene::PatternLoop,   RetrigScene::OverlapInside, RetrigScene::StackedSamePlace};
  const auto label = [&](RetrigScene s, const std::string& how, const char* what) {
    return std::string(format) + ", " + retrigName(s) + ", " + how + ": " + what;
  };
  const auto judge = [&](RetrigScene s, const std::string& how, const RetrigResult& r, bool looped) {
    const std::string tr = log ? retrigTrace(r.log) : std::string();
    if (trace) std::printf("[%s] %s, %s: %s\n", format, retrigName(s), how.c_str(), tr.c_str());
    // The second note's body, from well after its start.
    const int key = s == RetrigScene::OtherPitch ? kRetrigKey + 3 : kRetrigKey;
    const float second = retrigPeak(r.audio, bar + bar / 4, bar + 3 * bar / 4);
    expect(second > minLevel, label(s, how, ("the second clip's note sounds, level " + std::to_string(second) + " [" + tr + "]").c_str()).c_str());
    if (!log || s == RetrigScene::OtherChannel) return;  // (two instances share one log)
    const int64_t base = retrigFirstOn(r.log);
    expect(base >= 0, label(s, how, "the instrument received notes").c_str());
    expect(retrigKeyHeld(r.log, key, base + bar + bar / 2), label(s, how, ("the new note is still held for an instrument that goes by the key [" + tr + "]").c_str()).c_str());
    if (s == RetrigScene::EndsBeforeClip)
      expect(!retrigKeyHeld(r.log, key, base + bar - 1), label(s, how, "the gap before the next clip stays").c_str());
    if (s == RetrigScene::OtherPitch) expect(!retrigKeyHeld(r.log, kRetrigKey, base + bar), label(s, how, "the first pitch has ended").c_str());
    if (s == RetrigScene::PatternLoop || looped) {
      expect(retrigKeyHeld(r.log, kRetrigKey, base + 2 * bar + bar / 4), label(s, how, ("the note after the loop's wrap is held [" + tr + "]").c_str()).c_str());
    }
    // Every note the plugin was told about ends once: nothing stuck, nothing ended twice.
    if (!looped && s != RetrigScene::PatternLoop) {
      int ons = 0, offs = 0;
      for (int i = 0; i < r.log.count; ++i) (r.log.ev[i].on ? ons : offs) += 1;
      expect(ons == offs, label(s, how, ("as many note-offs as note-ons [" + tr + "]").c_str()).c_str());
      expect(!retrigKeyHeld(r.log, key, base + 3 * bar), label(s, how, "nothing left held after the end").c_str());
    }
  };
  for (RetrigScene s : scenes) {
    for (int block : {64, 512, 1000, 333, 441}) {
      const RetrigResult r = retrigRealtime(type, s, block, 2 * bar + bar / 2, log);
      judge(s, "realtime block " + std::to_string(block), r, true);
    }
    const RetrigResult r = retrigOffline(type, s, log);
    if (s != RetrigScene::PatternLoop) judge(s, "offline", r, false);  // (a pattern renders one pass)
  }
}

}  // namespace gl::test
