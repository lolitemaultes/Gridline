// Gridline — FLP import: plugin states → Gridline instruments and effects.
#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include "Flp.h"
#include "FlpJ22.h"
#include "FlpWrapper.h"

namespace gl::flp {

std::map<std::string, PluginDecoder>& decoders() {
  static std::map<std::string, PluginDecoder> d = [] {
    std::map<std::string, PluginDecoder> m;
    registerEffectDecoders(m);
    registerGeneratorDecoders(m);
    registerEffectDecoders2(m);     // second wave; never overrides the first
    registerGeneratorDecoders2(m);
    registerDecoders3(m);
    registerDecoders4(m);
    registerDecodersJ22(m);  // J-22: replaces earlier approximate instrument decoders
    registerWrapperDecoder(m);  // third-party plugins (FlpWrapper.cpp)
    return m;
  }();
  return d;
}

const Event* pluginState(const std::vector<Event>& events) { return find(events, 213); }

const PluginDecoder* findDecoder(const std::string& flName) {
  auto& d = decoders();
  if (auto it = d.find(flName); it != d.end()) return &it->second;
  auto lower = [](std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
  };
  static std::map<std::string, const PluginDecoder*> folded = [&] {
    std::map<std::string, const PluginDecoder*> m;
    for (const auto& [k, v] : d) m.emplace(lower(k), &v);
    return m;
  }();
  auto it = folded.find(lower(flName));
  return it == folded.end() ? nullptr : it->second;
}

void setRawRanges(PluginMapping& m, std::vector<RawRange> ranges) {
  auto table = std::make_shared<std::map<int, RawRange>>();
  for (const RawRange& r : ranges)
    if (r.hi != r.lo) (*table)[r.fl] = r;
  m.rawNorm = [table](int fl, int32_t raw) {
    auto it = table->find(fl);
    if (it == table->end()) return std::numeric_limits<double>::quiet_NaN();
    return std::clamp((raw - it->second.lo) / (it->second.hi - it->second.lo), 0.0, 1.0);
  };
}

namespace {

bool decode(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  const Event* nameEv = find(evs, 201);
  if (!nameEv) return false;
  m.flName = nameEv->text();
  if (m.flName.empty()) return false;  // samplers / audio clips carry an empty plugin name
  const PluginDecoder* dec = findDecoder(m.flName);
  if (!dec || !(*dec)(c, evs, m) || m.glType.empty()) {
    c.report.unmapped[m.flName]++;
    return false;
  }
  normalizePluginState(m.state);
  c.report.counts[m.exact ? "plugins mapped exactly" : "plugins mapped approximately"]++;
  return true;
}

}  // namespace

void importPlugins(Context& c) {
  Song& s = c.song;
  // Instruments: replace the placeholder generator of native / plugin channels.
  for (const auto& [idx, evs] : c.sec.channels) {
    auto id = c.channelIds.find(idx);
    if (id == c.channelIds.end()) continue;
    PluginMapping m;
    if (!decode(c, evs, m)) continue;
    if (Channel* ch = s.channel(id->second)) {
      if (m.state.type.empty()) m.state.type = m.glType;
      ch->plugin = m.state;
    }
    c.channelPlugins[idx] = std::move(m);
  }
  // Effects: one Gridline slot per FL slot.
  for (size_t i = 0; i < c.sec.inserts.size(); ++i) {
    auto tid = c.insertIds.find(static_cast<int>(i));
    MixerTrack* t = tid == c.insertIds.end() ? nullptr : s.mixerTrack(tid->second);
    for (size_t k = 0; k < c.sec.inserts[i].slots.size(); ++k) {
      const auto& evs = c.sec.inserts[i].slots[k].events;
      if (evs.empty()) continue;
      PluginMapping m;
      if (!decode(c, evs, m) || !t || k >= t->slots.size()) continue;
      if (m.state.type.empty()) m.state.type = m.glType;
      t->slots[k].plugin = m.state;
      c.slotPlugins[{static_cast<int>(i), static_cast<int>(k)}] = std::move(m);
    }
  }
}

}  // namespace gl::flp
