// Gridline — the master chain (RN-09). See Mastering.h.
#include "Mastering.h"
#include <algorithm>
#include <cmath>
#include "Document.h"
#include "PluginInfo.h"
#include "PluginParams.h"

namespace gl::mastering {

namespace {

constexpr float kTiltLowHz = 220.0f, kTiltHighHz = 3500.0f, kSubCutHz = 25.0f, kMonoBassHz = 120.0f;

void setP(PluginState& p, int k, float v) {
  if (k >= 0 && static_cast<size_t>(k) < p.params.size()) p.params[static_cast<size_t>(k)] = v;
}
float getP(const PluginState& p, int k, float dflt = 0) {
  return k >= 0 && static_cast<size_t>(k) < p.params.size() ? p.params[static_cast<size_t>(k)] : dflt;
}

Macros clampMacros(Macros m) {
  m.tilt = std::clamp(m.tilt, -6.0f, 6.0f);
  m.glue = std::clamp(m.glue, 0.0f, 1.0f);
  m.width = std::clamp(m.width, 0.0f, 2.0f);
  m.drive = std::clamp(m.drive, 0.0f, 12.0f);
  m.ceiling = std::clamp(m.ceiling, -6.0f, 0.0f);
  return m;
}

// Compressor settings for a glue amount.
float glueThreshold(float g) { return -4.0f - 20.0f * g; }
float glueRatio(float g) { return 1.0f + 3.0f * g; }

void writeChain(MixerTrack& master, int first, const Macros& in) {
  using namespace pp;
  const Macros m = clampMacros(in);
  {  // EQ: tilt shelves + sub-sonic cut
    PluginState& p = master.slots[static_cast<size_t>(first)].plugin;
    setP(p, eqParam(0, EqType), EqLowShelf);
    setP(p, eqParam(0, EqFreq), kTiltLowHz);
    setP(p, eqParam(0, EqGain), -0.5f * m.tilt);
    setP(p, eqParam(6, EqType), EqHighShelf);
    setP(p, eqParam(6, EqFreq), kTiltHighHz);
    setP(p, eqParam(6, EqGain), 0.5f * m.tilt);
    setP(p, eqParam(1, EqType), EqLowCut);
    setP(p, eqParam(1, EqFreq), kSubCutHz);
    setP(p, eqFl(1, EqWidth), 0.55f);
    setP(p, eqFl(1, EqFlOrder), EqOrd2);
    for (int b = 2; b <= 5; ++b) setP(p, eqParam(b, EqGain), 0.0f);
  }
  {  // glue compressor: slow RMS, detector high-passed so the bass does not pump it
    PluginState& p = master.slots[static_cast<size_t>(first + 1)].plugin;
    const float thr = glueThreshold(m.glue), ratio = glueRatio(m.glue);
    setP(p, CmpThreshold, thr);
    setP(p, CmpRatio, ratio);
    setP(p, CmpAttack, 0.015f);
    setP(p, CmpRelease, 0.25f);
    setP(p, CmpKnee, 8.0f);
    setP(p, CmpMakeup, -thr * (1.0f - 1.0f / ratio) * 0.3f);
    setP(p, CmpDetector, 1);  // RMS
    setP(p, CmpKeyHpf, 100.0f);
    setP(p, CmpLimiter, 0);   // the limiter follows
    setP(p, CmpMix, 1.0f);
  }
  {  // width with mono bass
    PluginState& p = master.slots[static_cast<size_t>(first + 2)].plugin;
    setP(p, StClassic, 0);
    setP(p, StWidth, m.width);
    setP(p, StMonoBelow, kMonoBassHz);
  }
  {  // limiter
    PluginState& p = master.slots[static_cast<size_t>(first + 3)].plugin;
    setP(p, LimGain, m.drive);
    setP(p, LimCeiling, m.ceiling);
    setP(p, LimRelease, 0.1f);
  }
}

bool chainAt(const MixerTrack& t, int first) {
  const auto& types = chainTypes();
  if (first < 0 || first + kChainSlots > static_cast<int>(t.slots.size())) return false;
  for (int k = 0; k < kChainSlots; ++k)
    if (t.slots[static_cast<size_t>(first + k)].plugin.type != types[static_cast<size_t>(k)]) return false;
  return true;
}

}  // namespace

const std::vector<std::string>& chainTypes() {
  static const std::vector<std::string> t = {"gl.eq", "gl.comp", "gl.stereo", "gl.limiter", "gl.meter"};
  return t;
}

const std::vector<Preset>& presets() {
  static const std::vector<Preset> p = {
      {"Clean", {0.0f, 0.15f, 1.0f, 0.0f, -1.0f}},
      {"Glue and shine", {1.0f, 0.35f, 1.1f, 3.0f, -1.0f}},
      {"Warm and round", {-1.5f, 0.3f, 1.0f, 2.0f, -1.0f}},
      {"Loud and wide", {0.5f, 0.55f, 1.2f, 6.0f, -0.8f}},
  };
  return p;
}

int chainSlot(const Song& s) {
  if (s.mixer.empty()) return -1;
  const MixerTrack& m = s.mixer[0];
  for (int i = 0; i + kChainSlots <= static_cast<int>(m.slots.size()); ++i)
    if (chainAt(m, i)) return i;
  return -1;
}

bool readMasterChain(const Song& s, Macros& out) {
  using namespace pp;
  const int first = chainSlot(s);
  if (first < 0) return false;
  const MixerTrack& t = s.mixer[0];
  const PluginState& eq = t.slots[static_cast<size_t>(first)].plugin;
  const PluginState& cmp = t.slots[static_cast<size_t>(first + 1)].plugin;
  const PluginState& st = t.slots[static_cast<size_t>(first + 2)].plugin;
  const PluginState& lim = t.slots[static_cast<size_t>(first + 3)].plugin;
  Macros m;
  m.tilt = getP(eq, eqParam(6, EqGain)) - getP(eq, eqParam(0, EqGain));
  m.glue = (-4.0f - getP(cmp, CmpThreshold, -10.0f)) / 20.0f;
  m.width = getP(st, StWidth, 1.0f);
  m.drive = getP(lim, LimGain);
  m.ceiling = getP(lim, LimCeiling, -1.0f);
  out = clampMacros(m);
  return true;
}

bool applyMacros(Song& s, const Macros& m) {
  const int first = chainSlot(s);
  if (first < 0) return false;
  writeChain(s.mixer[0], first, m);
  return true;
}

bool installMasterChain(Song& s, const Macros& m, std::string* why) {
  if (applyMacros(s, m)) return true;
  if (s.mixer.empty()) ensureMixer(s, 0);
  MixerTrack& t = s.mixer[0];
  // The last free run of five slots after the last used one keeps the chain at the end.
  int lastUsed = -1;
  for (int i = 0; i < static_cast<int>(t.slots.size()); ++i)
    if (!t.slots[static_cast<size_t>(i)].plugin.empty()) lastUsed = i;
  const int first = std::max(lastUsed + 1, static_cast<int>(t.slots.size()) - kChainSlots);
  bool free = first + kChainSlots <= static_cast<int>(t.slots.size());
  for (int k = 0; free && k < kChainSlots; ++k) free = t.slots[static_cast<size_t>(first + k)].plugin.empty();
  if (!free) {
    if (why) *why = "The master needs five free effect slots after its last effect for the master chain.";
    return false;
  }
  for (int k = 0; k < kChainSlots; ++k) {
    EffectSlot& slot = t.slots[static_cast<size_t>(first + k)];
    slot = EffectSlot{};
    slot.plugin = defaultPluginState(chainTypes()[static_cast<size_t>(k)]);
  }
  writeChain(t, first, m);
  return true;
}

bool removeMasterChain(Song& s) {
  const int first = chainSlot(s);
  if (first < 0) return false;
  for (int k = 0; k < kChainSlots; ++k) s.mixer[0].slots[static_cast<size_t>(first + k)] = EffectSlot{};
  return true;
}

bool installMasterChain(Document& d, const Macros& m, std::string* why) {
  Song probe = d.song();
  if (!installMasterChain(probe, m, why)) return false;
  d.editSong("master chain", [&](Song& s) { installMasterChain(s, m); });
  return true;
}

bool removeMasterChain(Document& d) {
  if (chainSlot(d.song()) < 0) return false;
  d.editSong("remove master chain", [](Song& s) { removeMasterChain(s); });
  return true;
}

bool setMacros(Document& d, const Macros& m, uint64_t mergeKey) {
  const int first = chainSlot(d.song());
  if (first < 0 || d.song().mixer.empty()) return false;
  return d.editMixerTrack(d.song().mixer[0].id, "master chain", [&](MixerTrack& t) { writeChain(t, first, m); }, mergeKey);
}

float driveForTarget(const Macros& m, double measuredLufs, double targetLufs) {
  if (!(measuredLufs > -100)) return m.drive;
  return std::clamp(static_cast<float>(m.drive + (targetLufs - measuredLufs)), 0.0f, 12.0f);
}

}  // namespace gl::mastering
