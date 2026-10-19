// Gridline — FLP import: FL generator plugin states → Gridline instruments.
// The decoders live in FlpGen*.cpp (see FlpGen.h); this file registers them
// and handles channels without a generator plugin (sampler / layer channels).
#include "FlpGen.h"

namespace gl::flp {

namespace {

// Sampler (type 0) and layer (type 3) channels carry an empty plugin name. Their
// instrument was already built by the channel importer, so keep it as it is.
bool keepChannelInstrument(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  for (const auto& [idx, evs] : c.sec.channels) {
    if (&evs != &events) continue;
    auto type = c.channelTypes.find(idx);
    auto id = c.channelIds.find(idx);
    if (type == c.channelTypes.end() || id == c.channelIds.end()) return false;
    if (type->second != 0 && type->second != 3) return false;
    const Channel* ch = c.song.channel(id->second);
    if (!ch || ch->plugin.type.empty()) return false;
    m.glType = ch->plugin.type;
    m.state = ch->plugin;
    m.exact = true;
    return true;
  }
  return false;  // an effect slot or an unknown external plugin
}

}  // namespace

void registerGeneratorDecoders(std::map<std::string, PluginDecoder>& d) {
  gen::registerOscDecoders(d);
  gen::registerSimpleDecoders(d);
  gen::registerSynthDecoders(d);
  gen::registerSamplerDecoders(d);
  d[""] = keepChannelInstrument;
}

}  // namespace gl::flp
