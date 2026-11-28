// Gridline: the audio clip editor's settings, one field at a time. See ClipFields.h.
#include "ClipFields.h"
#include <algorithm>
#include <cmath>
#include <set>
#include "../core/Automation.h"
#include "../core/ClipGain.h"
#include "../core/Fades.h"
#include "../core/PluginParams.h"
#include "ClipOps.h"
#include "ClipSource.h"

namespace gl::clipfields {

namespace {

bool isSwitch(Field f) {
  return f == Field::Mute || f == Field::Reverse || f == Field::FadeInShape || f == Field::FadeOutShape || f == Field::FollowTempo || f == Field::Loop;
}

// The channel's time mode keeps the pitch (the stretch or Rubber Band, not resampling).
bool keepsPitch(const Song& s, const Clip& c) {
  const Channel* ch = s.channel(c.source);
  return ch && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode) && ch->plugin.params[pp::SmpStretchMode] >= 0.5f;
}

double speedOf(const Clip& c) { return 100.0 / std::max(1e-6, c.stretch); }

// One clip's new state for `f` = v (a copy; `s` is the song before the edit).
void store(const Song& s, const TempoMap& tm, Clip& c, Field f, double v) {
  double lo = 0, hi = 0;
  range(f, lo, hi);
  if (!isSwitch(f)) v = std::clamp(v, lo, hi);
  switch (f) {
    case Field::GainDb: c.gain = clipgain::fromDb(v <= clipgain::kMinDb ? clipgain::kMinDb - 0.1 : v); break;
    case Field::Pan: c.pan = static_cast<float>(std::clamp(v / 100.0, -1.0, 1.0)); break;
    case Field::Mute: c.muted = v >= 0.5; break;
    case Field::Reverse:
      if ((v >= 0.5) != c.reverse) toggleClipReverse(s, c);
      break;
    case Field::FadeIn: c.fadeIn = v, c.fadeInManual = true; break;
    case Field::FadeOut: c.fadeOut = v, c.fadeOutManual = true; break;
    case Field::FadeInShape: c.fadeInShape = v >= 0.5 ? 1 : 0; break;
    case Field::FadeOutShape: c.fadeOutShape = v >= 0.5 ? 1 : 0; break;
    case Field::FadeInCurve: c.fadeInTension = fadeTensionFromUi(static_cast<float>(v)); break;
    case Field::FadeOutCurve: c.fadeOutTension = fadeTensionFromUi(static_cast<float>(v)); break;
    case Field::Semitones: {
      const int semis = static_cast<int>(std::lround(c.pitch / 100.0f));
      const float cents = c.pitch - static_cast<float>(semis) * 100.0f;
      c.pitch = static_cast<float>(std::lround(v)) * 100.0f + cents;
      break;
    }
    case Field::Cents: {
      const int semis = static_cast<int>(std::lround(c.pitch / 100.0f));
      c.pitch = static_cast<float>(semis) * 100.0f + static_cast<float>(v);
      break;
    }
    case Field::Stretch: {
      Song x = s;
      if (setClipStretch(x, c.id, v, keepsPitch(s, c)))
        if (const Clip* n = x.clip(c.id)) c = *n;
      break;
    }
    case Field::Speed: {
      // A record played faster or slower: the length follows the speed and so does the pitch.
      const double from = speedOf(c);
      Song x = s;
      if (setClipStretch(x, c.id, 100.0 / v, keepsPitch(s, c)))
        if (const Clip* n = x.clip(c.id)) c = *n;
      c.pitch = std::clamp(c.pitch + static_cast<float>(1200.0 * std::log2(v / from)), -4800.0f, 4800.0f);
      break;
    }
    case Field::FollowTempo: {
      Song x = s;
      if (setClipFollowsTempo(x, c.id, v >= 0.5))
        if (const Clip* n = x.clip(c.id)) c = *n;
      break;
    }
    case Field::Loop:
      if ((v >= 0.5) != c.loop) c = withClipLoop(s, tm, c, v >= 0.5);
      break;
    case Field::LoopStart: c.offset = std::max<Tick>(0, static_cast<Tick>(std::llround(v))); break;
    case Field::Count: break;
  }
}

}  // namespace

const char* undoLabel(Field f) {
  switch (f) {
    case Field::GainDb: return "clip gain";
    case Field::Pan: return "clip pan";
    case Field::Mute: return "mute clip";
    case Field::Reverse: return "reverse clip";
    case Field::FadeIn: return "clip fade in";
    case Field::FadeOut: return "clip fade out";
    case Field::FadeInShape: return "fade in shape";
    case Field::FadeOutShape: return "fade out shape";
    case Field::FadeInCurve:
    case Field::FadeOutCurve: return "fade curve";
    case Field::Semitones:
    case Field::Cents: return "clip pitch";
    case Field::Stretch: return "stretch clip";
    case Field::Speed: return "clip speed";
    case Field::FollowTempo: return "follow tempo";
    case Field::Loop: return "loop clip";
    case Field::LoopStart: return "loop start";
    case Field::Count: break;
  }
  return "edit clip";
}

void range(Field f, double& lo, double& hi) {
  lo = 0, hi = 1;
  switch (f) {
    case Field::GainDb: lo = clipgain::kMinDb - 0.1, hi = clipgain::kMaxDb; break;
    case Field::Pan: lo = -100.0, hi = 100.0; break;
    case Field::FadeIn:
    case Field::FadeOut: lo = 0, hi = 600; break;
    case Field::FadeInCurve:
    case Field::FadeOutCurve: lo = -1, hi = 1; break;
    case Field::Semitones: lo = -48, hi = 48; break;
    case Field::Cents: lo = -50, hi = 50; break;
    case Field::Stretch: lo = 0.0625, hi = 16; break;
    case Field::Speed: lo = 6.25, hi = 1600; break;
    case Field::LoopStart: lo = 0, hi = 1e12; break;
    default: break;
  }
}

double get(const Song& s, const Clip& c, Field f) {
  (void)s;
  switch (f) {
    case Field::GainDb: return clipgain::toDb(c.gain);
    case Field::Pan: return static_cast<double>(c.pan) * 100.0;
    case Field::Mute: return c.muted ? 1 : 0;
    case Field::Reverse: return c.reverse ? 1 : 0;
    case Field::FadeIn: return c.fadeIn;
    case Field::FadeOut: return c.fadeOut;
    case Field::FadeInShape: return c.fadeInShape;
    case Field::FadeOutShape: return c.fadeOutShape;
    case Field::FadeInCurve: return fadeTensionToUi(c.fadeInTension);
    case Field::FadeOutCurve: return fadeTensionToUi(c.fadeOutTension);
    case Field::Semitones: return std::lround(c.pitch / 100.0f);
    case Field::Cents: return c.pitch - static_cast<float>(std::lround(c.pitch / 100.0f)) * 100.0f;
    case Field::Stretch: return c.stretch;
    case Field::Speed: return speedOf(c);
    case Field::FollowTempo: return c.syncBpm > 0 ? 1 : 0;
    case Field::Loop: return c.loop ? 1 : 0;
    case Field::LoopStart: return static_cast<double>(c.offset);
    case Field::Count: break;
  }
  return 0;
}

bool set(Document& d, const std::vector<ObjectId>& ids, ObjectId anchor, Field f, double value, uint64_t mergeKey) {
  const Song& s = d.song();
  const Clip* a = s.clip(anchor);
  if (!a || a->kind != ClipKind::Audio) return false;
  const double delta = value - get(s, *a, f);
  std::set<ObjectId> targets(ids.begin(), ids.end());
  targets.insert(anchor);
  TempoMap tm;
  tm.build(s);
  if (f == Field::FollowTempo) {
    // K-431: keep the channel's SmpFitTempo readout in sync with clip tempo follow.
    Song trial = s;
    bool chChanged = false;
    for (const Clip& c : s.playlist) {
      if (!targets.count(c.id) || c.kind != ClipKind::Audio) continue;
      if (setClipFollowsTempo(trial, c.id, value >= 0.5)) chChanged = true;
    }
    if (!chChanged) return false;
    d.editSong(undoLabel(f), [&trial](Song& x) {
      x.playlist = trial.playlist;
      x.channels = trial.channels;
    });
    return true;
  }
  std::vector<Clip> next = s.playlist;
  bool changed = false;
  for (Clip& c : next) {
    if (!targets.count(c.id) || c.kind != ClipKind::Audio) continue;
    const Clip before = c;
    const double v = c.id == anchor || isSwitch(f) ? value : get(s, c, f) + delta;
    // (Speed is relative: every clip by the same ratio)
    store(s, tm, c, f, f == Field::Speed && c.id != anchor ? get(s, c, f) * (value / std::max(1e-6, get(s, *a, f))) : v);
    changed |= !(c == before);
  }
  if (!changed) return false;
  return d.editClips(undoLabel(f), [&next](std::vector<Clip>& v) { v = next; }, mergeKey);
}

}  // namespace gl::clipfields
