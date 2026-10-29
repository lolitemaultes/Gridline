#include "PatternAutoOps.h"
#include <algorithm>
#include "ClipLoop.h"
#include "PatternAutomation.h"

namespace gl::patauto {

Tick patternTickAt(const Song& s, ObjectId pattern, Tick songTick) {
  for (const auto& c : s.playlist) {
    if (c.kind != ClipKind::Pattern || c.pattern != pattern) continue;
    if (songTick < c.start || songTick >= c.start + s.clipLength(c)) continue;
    for (const ClipPass& ps : clipPasses(s, c))
      if (songTick >= c.start + ps.at && songTick < c.start + ps.at + ps.len) return ps.content + (songTick - c.start - ps.at);
  }
  return -1;
}

const char* moveResultText(MoveResult r) {
  switch (r) {
    case MoveResult::Done: return "";
    case MoveResult::NoSource: return "That automation clip no longer exists";
    case MoveResult::NoPattern: return "Select a pattern first";
    case MoveResult::TargetTaken: return "The pattern has an automation clip for that control already";
    case MoveResult::NoTarget: return "The automation clip drives no control yet";
  }
  return "";
}

MoveResult placementIntoPattern(Document& doc, ObjectId placement, ObjectId pattern, bool keep) {
  const Song& s = doc.song();
  const Clip* pl = s.clip(placement);
  const AutomationClip* src = pl && pl->kind == ClipKind::Automation ? s.automation(pl->source) : nullptr;
  if (!src) return MoveResult::NoSource;
  const Pattern* p = s.pattern(pattern);
  if (!p) return MoveResult::NoPattern;
  if (src->owner == kNoId) return MoveResult::NoTarget;
  if (findPatternLane(*p, src->owner, src->index)) return MoveResult::TargetTaken;
  const Tick at = std::max<Tick>(0, patternTickAt(s, pattern, pl->start));
  const PatternAutomation c = patternClipFromPlaylist(s, *src, pl, at);
  const ObjectId autoId = src->id;
  doc.editSong(keep ? "copy automation into pattern" : "move automation into pattern", [&](Song& x) {
    if (Pattern* q = x.pattern(pattern)) addPatternClip(*q, c);
    if (keep) return;
    x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(), [&](const Clip& k) { return k.id == placement; }), x.playlist.end());
    bool used = false;
    for (const auto& k : x.playlist) used = used || (k.kind == ClipKind::Automation && k.source == autoId);
    for (const auto& ar : x.arrangements)
      for (const auto& k : ar.playlist) used = used || (k.kind == ClipKind::Automation && k.source == autoId);
    if (!used)
      x.automations.erase(std::remove_if(x.automations.begin(), x.automations.end(), [&](const AutomationClip& q) { return q.id == autoId; }),
                          x.automations.end());
  });
  return MoveResult::Done;
}

MoveResult automationIntoPattern(Document& doc, ObjectId automation, ObjectId pattern, Tick at) {
  const Song& s = doc.song();
  const AutomationClip* src = s.automation(automation);
  if (!src) return MoveResult::NoSource;
  const Pattern* p = s.pattern(pattern);
  if (!p) return MoveResult::NoPattern;
  if (src->owner == kNoId) return MoveResult::NoTarget;
  if (findPatternLane(*p, src->owner, src->index)) return MoveResult::TargetTaken;
  const PatternAutomation c = patternClipFromPlaylist(s, *src, nullptr, at);
  doc.editPattern(pattern, "copy automation into pattern", [&](Pattern& q) { addPatternClip(q, c); });
  return MoveResult::Done;
}

}  // namespace gl::patauto
