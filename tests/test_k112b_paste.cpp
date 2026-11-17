// Gridline - K-112B (R11-01): Smooth's paste (pltools::pasteOwnCopies), rendered before and after.
// A pad region copied onto the keys track must play the keys' sound (the pad is muted here, so a
// pasted region that still played the pad would be silent), from a pattern of its own (an edit of
// it leaves the original alone), and in No Overlap the region under it gives way.
// R12-01: the paste keeps its full length (the tester's 16-bar Pad Intro onto Felt Keys at bar 9,
// pasted three times): the regions under it are trimmed, split or removed, and the copies lie
// back to back, each sounding to its last bar.
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include <set>
#include "../src/core/Document.h"
#include "../src/core/Song.h"
#include "../src/engine/PlaylistTools.h"
#include "../src/engine/Render.h"
#include "Check.h"

using namespace gl;

namespace {

float rms(const AudioBuffer& b, size_t from, size_t to) {
  double acc = 0;
  size_t n = 0;
  for (size_t c = 0; c < b.channels; ++c) {
    for (size_t i = from; i < to && i < b.frames; ++i, ++n) acc += static_cast<double>(b.at(c, i)) * b.at(c, i);
  }
  return n ? static_cast<float>(std::sqrt(acc / static_cast<double>(n))) : 0.0f;
}

AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.tail = 0;
  return renderSong(s, o);
}

// Frames of song ticks [t0, t1) at 120 BPM, 44.1 kHz.
size_t frameAt(const Song& s, Tick t) { return static_cast<size_t>(static_cast<double>(t) / s.ppq * 0.5 * 44100.0); }

}  // namespace

int main() {
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  const ObjectId pad = d.addChannel("pad", "gl.synth");
  const ObjectId keys = d.addChannel("keys", "gl.synth");
  const Song& s0 = d.song();
  const Tick bar = s0.barTicks();
  // The pad's region (one bar of notes) on track 0; the keys' own one-bar region on track 1 at bar 1.
  const ObjectId padPat = s0.patterns[0].id;
  d.addNote(padPat, pad, {0, bar / 2, 60, 110});
  d.addNote(padPat, pad, {bar / 2, bar / 2, 64, 110});
  const ObjectId keysPat = d.addPattern("keys part", 1, 16);
  d.addNote(keysPat, keys, {0, bar / 4, 72, 110});
  d.addClip(padPat, 0, 0);
  d.editPattern(keysPat, "no loop", [](Pattern& p) { p.loop = false; });  // (its second bar is silent)
  d.addClip(keysPat, 1, 0, 2 * bar);  // two bars long, so the paste at bar 2 lands on its tail
  d.editChannel(pad, "mute", [](Channel& c) { c.muted = true; });  // only the keys can sound
  // A last silent clip at bar 4 keeps the render length the same before and after.
  d.addClip(d.addPattern("end", 1, 16), 2, 4 * bar);

  const Song before = d.song();
  const AudioBuffer rb = render(before);
  const float bar2Before = rms(rb, frameAt(before, bar), frameAt(before, 2 * bar));
  expect(bar2Before < 1e-3f, "before the paste bar 2 is (nearly) silent");

  // The paste: the pad region onto the keys track at bar 2, No Overlap.
  const Clip* padClip = nullptr;
  for (const Clip& c : before.playlist) {
    if (c.pattern == padPat) padClip = &c;
  }
  expect(padClip != nullptr, "the pad region exists");
  pltools::PasteItem it;
  it.clip = *padClip;
  it.clip.track = 1;
  it.clip.start = bar;
  it.fromSound = pad, it.toSound = keys;
  Song after = before;
  const std::vector<ObjectId> added = pltools::pasteOwnCopies(after, {it}, true);
  expect(added.size() == 1, "one region pasted");
  const Clip* pc = after.clip(added.front());
  expect(pc && pc->track == 1 && pc->start == bar, "it lies on the keys track at bar 2");
  expect(pc && pc->pattern != padPat, "it has a pattern of its own");
  const Pattern* np = pc ? after.pattern(pc->pattern) : nullptr;
  expect(np && np->notes.count(keys) && !np->notes.count(pad), "its notes play the keys, not the pad");
  expect(np && np->name == before.pattern(padPat)->name, "it keeps the region's name");
  const Pattern* op = after.pattern(padPat);
  expect(op && op->notes.count(pad) && !op->notes.count(keys), "the original region still plays the pad");
  // No Overlap: the keys' two-bar region ends where the pasted one starts.
  for (const Clip& c : after.playlist) {
    if (c.pattern == keysPat) expect(c.start == 0 && after.clipLength(c) == bar, "the region under it ends where the paste starts");
  }

  const AudioBuffer ra = render(after);
  expect(ra.frames == rb.frames, "the same render length");
  const float bar2After = rms(ra, frameAt(after, bar), frameAt(after, 2 * bar));
  expect(bar2After > 1e-3f && bar2After > 10 * bar2Before, "after the paste bar 2 sounds: the pasted region plays the keys");
  const float bar1Before = rms(rb, 0, frameAt(before, bar)), bar1After = rms(ra, 0, frameAt(after, bar));
  expect(std::fabs(bar1Before - bar1After) < 1e-6f, "bar 1 sounds as before");

  // Independent: an edit of the pasted pattern leaves the original alone.
  if (Pattern* q = after.pattern(pc->pattern)) q->notes[keys].push_back({0, bar / 4, 48, 100});
  expect(after.pattern(padPat)->notes.at(pad).size() == 2, "editing the copy leaves the original's notes");

  // Same start, covered wholly: a region under the paste goes; a longer one goes on after it.
  {
    Song t = before;
    pltools::PasteItem i2 = it;
    i2.clip.start = 0;  // onto the keys region's start (it is two bars, the paste one)
    const std::vector<ObjectId> a2 = pltools::pasteOwnCopies(t, {i2}, true);
    const Clip* k = nullptr;
    for (const Clip& c : t.playlist) {
      if (c.pattern == keysPat) k = &c;
    }
    expect(a2.size() == 1 && k && k->start == bar && t.clipLength(*k) == bar, "a longer region at the same start goes on after the paste");
    Song u = before;
    pltools::PasteItem i3 = it;
    i3.clip.start = 0;
    i3.clip.length = 3 * bar;
    pltools::pasteOwnCopies(u, {i3}, true);
    bool gone = true;
    for (const Clip& c : u.playlist) {
      if (c.pattern == keysPat) gone = false;
    }
    expect(gone, "a region the paste covers wholly is replaced");
    Song v = before;
    pltools::pasteOwnCopies(v, {i3}, false);
    size_t onKeys = 0;
    for (const Clip& c : v.playlist) {
      if (c.track == 1) ++onKeys;
    }
    expect(onKeys == 2, "in Overlap nothing under it changes");
  }
  // R12-01: the tester's case. A 16-bar pad region pasted onto the keys track at bar 9, three
  // times (each Cmd+V advances by the copied span). The keys track holds its own 8-bar regions
  // back to back from bar 1; only the first one has notes, so a paste cut short would leave
  // silent bars. Rendered before and after.
  {
    Document e(makeEmptySong());
    e.setParam(songParam(SongParam::Tempo), 120.0, false);
    const ObjectId pad2 = e.addChannel("pad", "gl.synth");
    const ObjectId keys2 = e.addChannel("keys", "gl.synth");
    const Tick b = e.song().barTicks();
    const ObjectId intro = e.addPattern("Pad Intro", 16, 16);
    for (int k = 0; k < 16; ++k) {
      e.addNote(intro, pad2, {k * b, b / 2, 60 + (k % 5), 110});
    }
    e.addClip(intro, 0, 0, 16 * b);
    const ObjectId keysA = e.addPattern("Keys Intro", 8, 16);
    for (int k = 0; k < 8; ++k) {
      e.addNote(keysA, keys2, {k * b, b / 4, 72, 110});
    }
    const ObjectId keysB = e.addPattern("Keys rest", 8, 16);  // silent
    e.addClip(keysA, 1, 0, 8 * b);
    for (int k = 1; k < 8; ++k) {
      e.addClip(keysB, 1, k * 8 * b, 8 * b);  // bars 9 to 64
    }
    // A long region on track 2 that a paste lands inside of: it is split around the paste.
    e.addClip(keysB, 2, 0, 32 * b);
    e.editChannel(pad2, "mute", [](Channel& c) { c.muted = true; });  // only the keys can sound
    const Song b0 = e.song();
    const Clip* src = nullptr;
    for (const Clip& c : b0.playlist) {
      if (c.pattern == intro) {
        src = &c;
      }
    }
    expect(src && b0.clipLength(*src) == 16 * b, "the pad region is 16 bars");
    Song a1 = b0;
    std::vector<ObjectId> pasted;
    for (int n = 0; n < 3 && src; ++n) {
      pltools::PasteItem pi;
      pi.clip = *src;
      pi.clip.track = 1;
      pi.clip.start = 8 * b + n * 16 * b;  // bar 9, then advanced by the copied span
      pi.fromSound = pad2, pi.toSound = keys2;
      const std::vector<ObjectId> got = pltools::pasteOwnCopies(a1, {pi}, true);
      expect(got.size() == 1, "each Cmd+V pastes one region");
      pasted.insert(pasted.end(), got.begin(), got.end());
    }
    bool full = pasted.size() == 3;
    for (size_t n = 0; n < pasted.size(); ++n) {
      const Clip* c = a1.clip(pasted[n]);
      full = full && c && c->start == 8 * b + static_cast<Tick>(n) * 16 * b && a1.clipLength(*c) == 16 * b;
    }
    expect(full, "three full 16-bar copies, back to back from bar 9");
    // What is left on the keys track: Keys Intro (bars 1 to 8), the three copies, then the rest from bar 57.
    std::vector<std::pair<Tick, Tick>> rows;
    for (const Clip& c : a1.playlist) {
      if (c.track == 1) {
        rows.push_back({c.start, c.start + a1.clipLength(c)});
      }
    }
    std::sort(rows.begin(), rows.end());
    bool clean = !rows.empty() && rows.front() == std::make_pair(Tick(0), 8 * b) && rows.back().second == 64 * b;
    for (size_t k = 1; k < rows.size(); ++k) {
      clean = clean && rows[k].first == rows[k - 1].second;  // contiguous, no overlap, no gap
    }
    expect(clean && rows.size() == 5, "the keys track reads Keys Intro, three pastes, then the rest: no mosaic");
    // The split: an 8-bar paste inside the 32-bar region on track 2.
    Song sp = b0;
    if (src) {
      pltools::PasteItem pi;
      pi.clip = *src;
      pi.clip.track = 2;
      pi.clip.start = 8 * b;
      pi.clip.length = 8 * b;
      pi.fromSound = pad2, pi.toSound = keys2;
      pltools::pasteOwnCopies(sp, {pi}, true);
    }
    std::vector<std::pair<Tick, Tick>> t2;
    Tick rightOffset = -1;
    for (const Clip& c : sp.playlist) {
      if (c.track == 2) {
        t2.push_back({c.start, c.start + sp.clipLength(c)});
        if (c.pattern == keysB && c.start == 16 * b) {
          rightOffset = c.offset;
        }
      }
    }
    std::sort(t2.begin(), t2.end());
    expect(t2.size() == 3 && t2[0] == std::make_pair(Tick(0), 8 * b) && t2[1] == std::make_pair(8 * b, 16 * b) &&
               t2[2] == std::make_pair(16 * b, 32 * b),
           "a region spanning the paste is split around it");
    expect(rightOffset == 16 * b, "the part after the paste keeps its content in place");

    const AudioBuffer r0 = render(b0), r1 = render(a1);
    expect(r0.frames == r1.frames, "16-bar case: the same render length");
    expect(std::fabs(rms(r0, 0, frameAt(b0, 8 * b)) - rms(r1, 0, frameAt(a1, 8 * b))) < 1e-6f, "16-bar case: bars 1 to 8 as before");
    bool sounds = true;
    for (int k = 8; k < 56; ++k) {
      const float bb = rms(r0, frameAt(b0, k * b), frameAt(b0, (k + 1) * b));
      const float ba = rms(r1, frameAt(a1, k * b), frameAt(a1, (k + 1) * b));
      if (k == 23 || k == 39 || k == 55) {
        sounds = sounds && bb < 1e-3f && ba > 1e-3f;  // the last bar of each copy: silent before, sounding now
      } else {
        sounds = sounds && ba > 1e-3f;
      }
    }
    expect(sounds, "16-bar case: every bar of the three copies sounds, to the last bar of each");
  }
  return finish();
}
