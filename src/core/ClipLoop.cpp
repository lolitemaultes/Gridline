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
  if (c.kind != ClipKind::Pattern) return 0;
  const Pattern* p = s.pattern(c.pattern);
  if (!p || !p->loop) return 0;
  return std::max<Tick>(1, s.patternContentLength(*p));
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
