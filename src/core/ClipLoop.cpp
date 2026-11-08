// Gridline — looping pattern clips (Q-1). See ClipLoop.h.
#include "ClipLoop.h"
#include <algorithm>

namespace gl {

Tick wrapLoopOffset(Tick offset, Tick loop) {
  if (loop <= 0) return offset;
  const Tick r = offset % loop;
  return r < 0 ? r + loop : r;
}

std::vector<ClipPass> loopPasses(Tick offset, Tick len, Tick loop) {
  std::vector<ClipPass> out;
  if (len <= 0) return out;
  if (loop <= 0) {
    out.push_back({0, len, offset, 0});
    return out;
  }
  Tick content = wrapLoopOffset(offset, loop);
  Tick at = 0;
  int index = 0;
  while (at < len) {
    const Tick n = std::min(loop - content, len - at);
    out.push_back({at, n, content, index++});
    at += n;
    content = 0;
  }
  return out;
}

Tick clipLoopLength(const Song& s, const Clip& c) {
  if (!c.loop) return 0;
  if (c.kind == ClipKind::Automation) {
    const AutomationClip* a = s.automation(c.source);
    return a && a->loop > 0 ? a->loop : 0;
  }
  if (c.kind != ClipKind::Pattern) return 0;
  const Pattern* p = s.pattern(c.pattern);
  if (!p) return 0;
  return std::max<Tick>(1, s.patternContentLength(*p));
}

bool clipPastContent(const Song& s, const Clip& c) {
  if (c.kind != ClipKind::Pattern) return false;
  const Pattern* p = s.pattern(c.pattern);
  if (!p) return false;
  const Tick pl = std::max<Tick>(1, s.patternContentLength(*p));
  const Tick off = c.loop ? wrapLoopOffset(c.offset, pl) : c.offset;
  return off + s.clipLength(c) > pl;
}

bool patternLoopMode(const Song& s, const Pattern& p) {
  bool looped = false, plain = false;
  auto scan = [&](const std::vector<Clip>& list) {
    for (const Clip& c : list)
      if (c.kind == ClipKind::Pattern && c.pattern == p.id && clipPastContent(s, c)) (c.loop ? looped : plain) = true;
  };
  scan(s.playlist);
  for (const Arrangement& a : s.arrangements) scan(a.playlist);
  if (looped) return true;
  if (plain) return false;
  return p.loop;
}

namespace {
template <typename Fn>
void eachPlaylist(Song& s, Fn fn) {
  fn(s.playlist);
  for (Arrangement& a : s.arrangements) fn(a.playlist);
}
}  // namespace

void adoptOldClipLoops(Song& s) {
  eachPlaylist(s, [&](std::vector<Clip>& list) {
    for (Clip& c : list) {
      c.loop = false;
      if (c.kind == ClipKind::Automation) {
        const AutomationClip* a = s.automation(c.source);
        c.loop = a && a->loop > 0;  // every placement wrapped at the curve's loop length
      } else if (c.kind == ClipKind::Pattern) {
        const Pattern* p = s.pattern(c.pattern);
        if (p && p->loop) c.loop = c.offset + s.clipLength(c) > std::max<Tick>(1, s.patternContentLength(*p));
      }
    }
  });
}

void adoptPatternLoopModes(Song& s) {
  eachPlaylist(s, [&](std::vector<Clip>& list) {
    for (Clip& c : list)
      if (c.kind == ClipKind::Pattern)
        if (const Pattern* p = s.pattern(c.pattern)) c.loop = p->loop;
  });
}

std::vector<ClipPass> clipPasses(const Song& s, const Clip& c) { return loopPasses(c.offset, s.clipLength(c), clipLoopLength(s, c)); }

bool clipRepeats(const Song& s, const Clip& c) {
  const Tick loop = clipLoopLength(s, c);
  return loop > 0 && wrapLoopOffset(c.offset, loop) + s.clipLength(c) > loop;
}

std::vector<Note> clipChannelNotes(const Song& s, const Clip& c, const std::vector<Note>& patternNotes, bool keepMuted) {
  std::vector<Note> out;
  const Tick len = s.clipLength(c);
  for (const ClipPass& p : clipPasses(s, c))
    for (const Note& n : patternNotes) {
      if ((n.muted && !keepMuted) || n.start < p.content || n.start >= p.content + p.len) continue;
      Note m = n;
      m.start = p.at + (n.start - p.content);
      m.length = std::max<Tick>(1, std::min(m.start + n.length, len) - m.start);
      out.push_back(m);
    }
  std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
  return out;
}

}  // namespace gl
