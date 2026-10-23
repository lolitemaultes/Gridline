#include "Params.h"
#include <atomic>

namespace gl {

namespace {

ParamDesc lin(const char* n, const char* u, double mn, double mx, double d) {
  return ParamDesc{n, u, mn, mx, d, ParamKind::Continuous, ParamScale::Linear, {}};
}
ParamDesc tog(const char* n) { return ParamDesc{n, "", 0, 1, 0, ParamKind::Toggle, ParamScale::Linear, {}}; }

// Resolved storage for a ParamId: exactly one pointer is set.
struct Target {
  double* d = nullptr;
  float* f = nullptr;
  bool* b = nullptr;
  int* i = nullptr;
  ParamDesc desc;
};

// Shared resolver for const and non-const access.
bool resolve(Song& s, ParamId id, Target& t) {
  if (id.owner == kSongId) {
    switch (static_cast<SongParam>(id.index)) {
      case SongParam::Tempo: t.d = &s.tempo; t.desc = lin("Tempo", "BPM", 10.0, 522.0, 140.0); return true;
      case SongParam::Swing: t.f = &s.swing; t.desc = lin("Swing", "%", 0, 1, 0); return true;
      case SongParam::MasterPitch: t.f = &s.masterPitch; t.desc = lin("Master pitch", "st", -12, 12, 0); return true;
      default: return false;
    }
  }
  if (Channel* c = s.channel(id.owner)) {
    if (id.index >= kNoteFxBase) {
      uint32_t fx = (id.index - kNoteFxBase) / kNoteFxStride, k = (id.index - kNoteFxBase) % kNoteFxStride;
      if (fx >= c->noteFx.size()) return false;
      PluginState& ps = c->noteFx[fx];
      const PluginInfo* p = findPlugin(ps.type);
      if (!p || k >= p->params.size() || k >= ps.params.size()) return false;
      t.f = &ps.params[k];
      t.desc = p->params[k];
      return true;
    }
    if (id.index >= kChannelPluginBase) {
      const PluginInfo* p = findPlugin(c->plugin.type);
      size_t k = id.index - kChannelPluginBase;
      if (!p || k >= p->params.size() || k >= c->plugin.params.size()) return false;
      t.f = &c->plugin.params[k];
      t.desc = p->params[k];
      return true;
    }
    switch (static_cast<ChannelParam>(id.index)) {
      case ChannelParam::Volume: t.f = &c->gain; t.desc = lin("Volume", "%", 0.0, 1.25, 0.78); return true;
      case ChannelParam::Pan: t.f = &c->pan; t.desc = lin("Pan", "pan", -1.0, 1.0, 0.0); return true;
      case ChannelParam::Mute: t.b = &c->muted; t.desc = tog("Mute"); return true;
      case ChannelParam::Pitch: t.i = &c->pitch; t.desc = lin("Pitch", "st", -24, 24, 0); return true;
      default: return false;
    }
  }
  if (MixerTrack* m = s.mixerTrack(id.owner)) {
    if (id.index >= kSlotBase) {
      uint32_t slot = (id.index - kSlotBase) / kSlotStride, k = (id.index - kSlotBase) % kSlotStride;
      if (slot >= m->slots.size()) return false;
      EffectSlot& es = m->slots[slot];
      if (k == kSlotMix) {
        t.f = &es.mix;
        t.desc = lin("Mix", "%", 0, 1, 1);
        return true;
      }
      if (k == kSlotEnable) {
        t.b = &es.enabled;
        t.desc = tog("Effect on");
        return true;
      }
      const PluginInfo* p = findPlugin(es.plugin.type);
      if (!p || k >= p->params.size() || k >= es.plugin.params.size()) return false;
      t.f = &es.plugin.params[k];
      t.desc = p->params[k];
      return true;
    }
    if (id.index >= kSendBase && id.index < kSlotBase) {  // send level to a destination track
      size_t dest = id.index - kSendBase;
      if (dest >= s.mixer.size()) return false;
      for (auto& sd : m->sends)
        if (sd.to == s.mixer[dest].id && !sd.sidechain) {
          t.f = &sd.level;
          t.desc = lin("Send level", "%", 0.0, 1.25, 1.0);
          return true;
        }
      return false;
    }
    {
      const uint32_t ix = id.index, g0 = static_cast<uint32_t>(MixerParam::EqLowGain);
      if (ix >= g0 && ix < g0 + 9) {
        const uint32_t which = (ix - g0) / 3, band = (ix - g0) % 3;
        static const char* names[3][3] = {{"EQ low gain", "EQ mid gain", "EQ high gain"},
                                          {"EQ low freq", "EQ mid freq", "EQ high freq"},
                                          {"EQ low Q", "EQ mid Q", "EQ high Q"}};
        if (which == 0) t.f = &m->eqGain[band], t.desc = lin(names[0][band], "dB", -18.0, 18.0, 0.0);
        if (which == 1) {
          t.f = &m->eqFreq[band];
          t.desc = lin(names[1][band], "Hz", 20.0, 20000.0, kStripEqDefFreq[band]);
          t.desc.scale = ParamScale::Log;
        }
        if (which == 2) {
          t.f = &m->eqQ[band];
          t.desc = lin(names[2][band], "", 0.1, 10.0, kStripEqDefQ);
          t.desc.scale = ParamScale::Log;
        }
        return true;
      }
    }
    switch (static_cast<MixerParam>(id.index)) {
      case MixerParam::Volume: t.f = &m->volume; t.desc = lin("Volume", "%", 0.0, 1.25, 0.8); return true;
      case MixerParam::Pan: t.f = &m->pan; t.desc = lin("Pan", "pan", -1.0, 1.0, 0.0); return true;
      case MixerParam::StereoSep: t.f = &m->stereoSep; t.desc = lin("Stereo separation", "", -1.0, 1.0, 0.0); return true;
      case MixerParam::Mute: t.b = &m->muted; t.desc = tog("Mute"); return true;
      default: return false;
    }
  }
  return false;
}

}  // namespace

namespace {
std::atomic<ParamReadHook> readHook{nullptr};
}  // namespace

void setParamReadHook(ParamReadHook hook) { readHook.store(hook, std::memory_order_relaxed); }

std::optional<ParamDesc> describeParam(const Song& song, ParamId id) {
  if (ParamReadHook h = readHook.load(std::memory_order_relaxed)) h(id);
  Target t;
  if (!resolve(const_cast<Song&>(song), id, t)) return std::nullopt;
  return t.desc;
}

std::optional<double> getParam(const Song& song, ParamId id) {
  if (ParamReadHook h = readHook.load(std::memory_order_relaxed)) h(id);
  Target t;
  if (!resolve(const_cast<Song&>(song), id, t)) return std::nullopt;  // read-only use
  if (t.d) return *t.d;
  if (t.f) return *t.f;
  if (t.b) return *t.b ? 1.0 : 0.0;
  return static_cast<double>(*t.i);
}

bool setParam(Song& song, ParamId id, double value) {
  Target t;
  if (!resolve(song, id, t)) return false;
  double v = t.desc.clamp(value);
  if (t.d) *t.d = v;
  else if (t.f) *t.f = static_cast<float>(v);
  else if (t.b) *t.b = v >= 0.5;
  else *t.i = static_cast<int>(v < 0 ? v - 0.5 : v + 0.5);
  return true;
}

std::string paramDisplayName(const Song& song, ParamId id) {
  auto d = describeParam(song, id);
  if (!d) return "?";
  if (id.owner == kSongId) return d->name;
  if (const Channel* c = song.channel(id.owner)) {
    if (id.index >= kNoteFxBase) {
      uint32_t fx = (id.index - kNoteFxBase) / kNoteFxStride;
      const PluginInfo* p = fx < c->noteFx.size() ? findPlugin(c->noteFx[fx].type) : nullptr;
      return c->name + " › " + (p ? p->name : std::string("Note FX")) + " › " + d->name;
    }
    return c->name + " › " + d->name;
  }
  int mi = song.mixerIndex(id.owner);
  if (mi >= 0) {
    std::string n = mixerTrackName(song, static_cast<size_t>(mi));
    if (id.index >= kSlotBase) {
      uint32_t slot = (id.index - kSlotBase) / kSlotStride;
      const PluginInfo* p = findPlugin(song.mixer[static_cast<size_t>(mi)].slots[slot].plugin.type);
      n += " › " + (p ? p->name : std::string("Slot ") + std::to_string(slot + 1));
    }
    return n + " › " + d->name;
  }
  return d->name;
}

std::vector<ParamId> listParams(const Song& song) {
  std::vector<ParamId> out;
  for (uint32_t i = 0; i < static_cast<uint32_t>(SongParam::Count); ++i) out.push_back({kSongId, i});
  for (const auto& c : song.channels) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(ChannelParam::Count); ++i) out.push_back({c.id, i});
    if (const PluginInfo* p = findPlugin(c.plugin.type))
      for (size_t k = 0; k < p->params.size(); ++k) out.push_back(channelPluginParam(c.id, static_cast<int>(k)));
    for (size_t fx = 0; fx < c.noteFx.size(); ++fx)
      if (const PluginInfo* p = findPlugin(c.noteFx[fx].type))
        for (size_t k = 0; k < p->params.size(); ++k)
          out.push_back(noteFxParam(c.id, static_cast<int>(fx), static_cast<int>(k)));
  }
  for (const auto& m : song.mixer) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(MixerParam::Count); ++i) out.push_back({m.id, i});
    for (const auto& sd : m.sends)
      if (!sd.sidechain && song.mixerIndex(sd.to) >= 0) out.push_back(sendParam(m.id, song.mixerIndex(sd.to)));
    for (size_t s = 0; s < m.slots.size(); ++s) {
      const PluginInfo* p = findPlugin(m.slots[s].plugin.type);
      if (!p) continue;
      out.push_back(slotMixParam(m.id, static_cast<int>(s)));
      out.push_back(slotEnableParam(m.id, static_cast<int>(s)));
      for (size_t k = 0; k < p->params.size(); ++k)
        out.push_back(slotParam(m.id, static_cast<int>(s), static_cast<int>(k)));
    }
  }
  return out;
}

namespace {
std::string sourcePrefix(const Song& song, ObjectId owner, int slot, const PluginInfo** info) {
  *info = nullptr;
  if (slot < 0) {
    const Channel* c = song.channel(owner);
    if (!c) return "";
    *info = findPlugin(c->plugin.type);
    return c->name;
  }
  int mi = song.mixerIndex(owner);
  if (mi < 0) return "";
  const MixerTrack& m = song.mixer[static_cast<size_t>(mi)];
  if (static_cast<size_t>(slot) >= m.slots.size()) return "";
  *info = findPlugin(m.slots[static_cast<size_t>(slot)].plugin.type);
  return mixerTrackName(song, static_cast<size_t>(mi));
}
}  // namespace

std::vector<ControllerOutput> listControllerOutputs(const Song& song) {
  std::vector<ControllerOutput> out;
  auto add = [&](ObjectId owner, int slot) {
    const PluginInfo* info = nullptr;
    std::string prefix = sourcePrefix(song, owner, slot, &info);
    if (!info) return;
    for (size_t k = 0; k < info->outputs.size(); ++k)
      out.push_back({owner, slot, static_cast<int>(k), prefix + " › " + info->name + " › " + info->outputs[k]});
  };
  for (const auto& c : song.channels) add(c.id, -1);
  for (const auto& m : song.mixer)
    for (size_t s = 0; s < m.slots.size(); ++s) add(m.id, static_cast<int>(s));
  return out;
}

std::string controllerOutputName(const Song& song, ObjectId owner, int slot, int output) {
  const PluginInfo* info = nullptr;
  std::string prefix = sourcePrefix(song, owner, slot, &info);
  if (!info || output < 0 || static_cast<size_t>(output) >= info->outputs.size()) return "?";
  return prefix + " › " + info->name + " › " + info->outputs[static_cast<size_t>(output)];
}

bool isModulated(const Song& song, ParamId id) {
  const ModLink* l = findModLink(song, id.owner, id.index);
  return l && l->enabled && isControllerSource(song, l->sourceOwner, l->sourceSlot);
}

}  // namespace gl
