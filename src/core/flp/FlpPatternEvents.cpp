// Gridline — FLP import: pattern events → pattern automation.
//
// Event 223 (inside a pattern) holds control changes recorded into the
// pattern, 12 bytes each (observed on the supplied projects, see
// format notes):
//   u32 position (pattern ticks, ascending), u32 target, i32 value.
// The target uses the same numbering as automation links (event 227, see
// FlpArrangement.cpp): channel (high word) + setting (low word), mixer
// insert/slot above 0x2000 (0x7000 in FL 2025), 0x8000 + n = plugin
// parameter n. The value is the control's own integer value (not 0..1):
// channel volume / pan / mixer level 0..12800, channel filter 0..256, on/off
// 0..1, plugin parameters in the plugin's own range.
//
// Notes placed on automation channels trigger that automation in FL; they
// become pattern automation too (the curve copied at each note).
#include <algorithm>
#include <cmath>
#include <limits>
#include "../PluginParams.h"
#include "Flp.h"

namespace gl::flp {

namespace {

constexpr double kUnknown = std::numeric_limits<double>::quiet_NaN();

// Lane for a target in a pattern (created on demand).
PatternAutomation& laneFor(Pattern& p, ParamId id) {
  for (auto& l : p.automation)
    if (l.owner == id.owner && l.index == id.index) return l;
  PatternAutomation l;  // J-173: an unnamed pattern automation clip with the earlier curves (steps)
  l.name.clear();
  l.owner = id.owner;
  l.index = id.index;
  l.flCurves = false;
  p.automation.push_back(std::move(l));
  return p.automation.back();
}

void addPoint(PatternAutomation& l, Tick pos, double value) {
  AutoPoint pt;
  pt.pos = pos;
  pt.value = static_cast<float>(std::clamp(value, 0.0, 1.0));
  pt.shape = AutoShape::Hold;  // recorded values step
  l.points.push_back(pt);
}

}  // namespace

void importPatternEvents(Context& c) {
  Song& s = c.song;
  int records = 0, mapped = 0, lanes = 0, triggers = 0;
  std::map<std::string, int> unmapped;
  for (const auto& [idx, evs] : c.sec.patterns) {
    auto pid = c.patternIds.find(idx);
    Pattern* pat = pid == c.patternIds.end() ? nullptr : s.pattern(pid->second);
    if (!pat) continue;
    // --- recorded control changes (223) ---
    for (const auto& e : evs) {
      if (e.id != 223) continue;
      for (uint32_t o = 0; o + 12 <= e.size; o += 12) {
        ++records;
        const uint32_t target = e.u32(o + 4), hi = target >> 16, lo = target & 0xFFFF;
        const int32_t raw = e.i32(o + 8);
        const Tick pos = c.tick(e.u32(o));
        ParamId id;
        double norm = kUnknown;
        std::string what;
        if (hi < 0x2000) {  // channel setting
          auto ch = c.channelIds.find(static_cast<int>(hi));
          const Channel* chan = ch == c.channelIds.end() ? nullptr : s.channel(ch->second);
          what = "channel setting " + std::to_string(lo);
          if (!chan) {
            what = "channel " + std::to_string(hi) + " (not imported)";
          } else if (lo == 0x07) {  // mute (1 = muted [H])
            id = channelParam(chan->id, ChannelParam::Mute);
            norm = raw != 0 ? 1.0 : 0.0;
          } else if ((lo == 0x02 || lo == 0x03) && chan->plugin.type == "gl.sampler") {  // channel filter → sampler filter
            id = channelPluginParam(chan->id, lo == 0x02 ? pp::SmpCutoff : pp::SmpReso);
            if (auto d = describeParam(s, id)) {
              double x = std::clamp(raw / 256.0, 0.0, 1.0);
              norm = d->toNormalized(d->clamp(lo == 0x02 ? 20.0 * std::pow(1000.0, x) : x));
            }
          } else if (lo == 0x00 || lo == 0x01 || lo == 0x10 || lo == 0x11 || lo >= 0x8000) {
            double fl = kUnknown;
            if (lo < 0x8000) {
              fl = std::clamp(raw / 12800.0, 0.0, 1.0);
            } else {
              auto pm = c.channelPlugins.find(static_cast<int>(hi));
              what = "plugin parameter " + std::to_string(lo - 0x8000);
              if (pm != c.channelPlugins.end()) {
                what += " (" + pm->second.flName + ")";
                if (pm->second.rawNorm) fl = pm->second.rawNorm(static_cast<int>(lo - 0x8000), raw);
              }
            }
            FlTarget t;
            if (std::isfinite(fl) && resolveFlTarget(c, target, t)) {
              id = t.id;
              norm = t.toNormalized(fl);
            }
          }
        } else if (hi != 0x4000) {  // mixer insert / slot
          what = "mixer setting " + std::to_string(lo);
          double fl = kUnknown;
          if (lo == 0x1F00) fl = raw != 0 ? 1.0 : 0.0;                         // slot on/off
          else if (lo == 0x1F01 || (lo >= 0x1FC0 && lo <= 0x1FC2)) fl = std::clamp(raw / 12800.0, 0.0, 1.0);  // mix, volume, pan, separation
          else if (lo >= 0x8000) {
            uint32_t rel = hi - (hi >= 0x7000 ? 0x7000u : 0x2000u);
            auto pm = c.slotPlugins.find({static_cast<int>(rel >> 6), static_cast<int>(rel & 0x3F)});
            what = "effect parameter " + std::to_string(lo - 0x8000);
            if (pm != c.slotPlugins.end()) {
              what += " (" + pm->second.flName + ")";
              if (pm->second.rawNorm) fl = pm->second.rawNorm(static_cast<int>(lo - 0x8000), raw);
            }
          }
          FlTarget t;
          if (std::isfinite(fl) && resolveFlTarget(c, target, t)) {
            id = t.id;
            norm = t.toNormalized(fl);
          }
        } else {
          what = "song setting " + std::to_string(lo);
        }
        if (!std::isfinite(norm) || id.owner == kNoId || id.owner == kSongId) {
          ++unmapped[what];
          continue;
        }
        size_t before = pat->automation.size();
        addPoint(laneFor(*pat, id), pos, norm);
        lanes += pat->automation.size() > before;
        ++mapped;
      }
    }
    // --- notes on automation channels: the automation plays from each note ---
    for (const auto& e : evs) {
      if (e.id != 224) continue;
      for (uint32_t o = 0; o + 24 <= e.size; o += 24) {
        int chIdx = e.u16(o + 6);
        auto ty = c.channelTypes.find(chIdx);
        if (ty == c.channelTypes.end() || ty->second != 5) continue;
        auto aid = c.automationIds.find(chIdx);
        const AutomationClip* a = aid == c.automationIds.end() ? nullptr : s.automation(aid->second);
        if (!a || a->owner == kNoId || a->owner == kSongId || a->points.empty()) {
          ++unmapped["notes on automation channels without a target"];
          continue;
        }
        const Tick start = c.tick(e.u32(o));
        size_t before = pat->automation.size();
        PatternAutomation& l = laneFor(*pat, ParamId{a->owner, a->index});
        if (pat->automation.size() > before) {  // J-173: the clip is named and coloured after the automation channel
          ++lanes;
          l.name = a->name;
          l.color = a->color;
        }
        for (const auto& p : a->points) {
          AutoPoint q = p;
          q.pos = start + p.pos;
          q.value = static_cast<float>(std::clamp(a->min + p.value * (a->max - a->min), 0.0f, 1.0f));
          l.points.push_back(q);
        }
        ++triggers;
      }
    }
    for (auto& l : pat->automation)
      std::stable_sort(l.points.begin(), l.points.end(), [](const AutoPoint& x, const AutoPoint& y) { return x.pos < y.pos; });
  }
  if (records) c.report.counts["pattern events"] = records;
  if (mapped) c.report.counts["pattern events imported"] = mapped;
  if (lanes) c.report.counts["pattern automation lanes"] = lanes;
  if (triggers) c.report.counts["notes on automation channels (pattern automation)"] = triggers;
  for (const auto& [what, n] : unmapped)
    c.report.warn("pattern events not imported: " + what + " (" + std::to_string(n) + ")");
}

}  // namespace gl::flp
