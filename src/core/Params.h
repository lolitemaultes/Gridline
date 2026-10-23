// Gridline — parameter addressing (AU-01). Every automatable value in a project
// is reachable through a ParamId, so automation, controller links, undo and the
// UI share one path to it.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "PluginInfo.h"
#include "Song.h"

namespace gl {

// Owner object + parameter index within that owner.
struct ParamId {
  ObjectId owner = kNoId;
  uint32_t index = 0;

  uint64_t packed() const { return (static_cast<uint64_t>(owner) << 32) | index; }
  static ParamId unpack(uint64_t v) { return {static_cast<ObjectId>(v >> 32), static_cast<uint32_t>(v)}; }
  bool operator==(const ParamId& o) const { return owner == o.owner && index == o.index; }
  bool operator!=(const ParamId& o) const { return !(*this == o); }
};

// Parameter index layout per owner kind:
//   song         SongParam
//   channel      ChannelParam, then generator plugin params at kChannelPluginBase + k,
//                note processor params at kNoteFxBase + fx*kNoteFxStride + k
//   mixer track  MixerParam, then slot params at kSlotBase + slot*kSlotStride + k,
//                slot mix at kSlotBase + slot*kSlotStride + kSlotMix
enum class SongParam : uint32_t { Tempo = 0, Swing, MasterPitch, Count };
enum class ChannelParam : uint32_t { Volume = 0, Pan, Mute, Pitch, Count };
enum class MixerParam : uint32_t {
  Volume = 0, Pan, StereoSep, Mute,
  // Strip EQ (MixerTrack::eqGain / eqFreq / eqQ), bands low · mid · high.
  EqLowGain, EqMidGain, EqHighGain, EqLowFreq, EqMidFreq, EqHighFreq, EqLowQ, EqMidQ, EqHighQ,
  Count
};
inline constexpr uint32_t kChannelPluginBase = 100;
inline constexpr uint32_t kSlotBase = 1000;
inline constexpr uint32_t kSlotStride = 1000;  // schema v6 (was 100: effects with 99+ params aliased)
inline constexpr uint32_t kSlotMix = 999;
inline constexpr uint32_t kSlotEnable = 998;  // slot on/off (toggle)
inline constexpr uint32_t kSendBase = 200;    // mixer track: send level to mixer index d at kSendBase + d
inline constexpr uint32_t kNoteFxBase = 5000;
inline constexpr uint32_t kNoteFxStride = 200;
inline constexpr int kMaxNoteFx = 8;  // note processors per channel

inline ParamId songParam(SongParam p) { return {kSongId, static_cast<uint32_t>(p)}; }
inline ParamId channelParam(ObjectId ch, ChannelParam p) { return {ch, static_cast<uint32_t>(p)}; }
inline ParamId channelPluginParam(ObjectId ch, int k) { return {ch, kChannelPluginBase + static_cast<uint32_t>(k)}; }
inline ParamId noteFxParam(ObjectId ch, int fx, int k) {
  return {ch, kNoteFxBase + static_cast<uint32_t>(fx) * kNoteFxStride + static_cast<uint32_t>(k)};
}
inline ParamId mixerParam(ObjectId t, MixerParam p) { return {t, static_cast<uint32_t>(p)}; }
inline ParamId slotParam(ObjectId t, int slot, int k) {
  return {t, kSlotBase + static_cast<uint32_t>(slot) * kSlotStride + static_cast<uint32_t>(k)};
}
inline ParamId slotMixParam(ObjectId t, int slot) { return slotParam(t, slot, static_cast<int>(kSlotMix)); }
inline ParamId slotEnableParam(ObjectId t, int slot) { return slotParam(t, slot, static_cast<int>(kSlotEnable)); }
// Level of the send from mixer track t to the track at mixer index `destIndex`.
inline ParamId sendParam(ObjectId t, int destIndex) { return {t, kSendBase + static_cast<uint32_t>(destIndex)}; }

// Descriptor for a parameter, or nullopt if the ParamId does not resolve in `song`.
std::optional<ParamDesc> describeParam(const Song& song, ParamId id);
std::optional<double> getParam(const Song& song, ParamId id);
// Sets a clamped value. Returns false if the ParamId does not resolve.
bool setParam(Song& song, ParamId id, double value);
// J-92 parameter audit (UI thread, dev runs): called with every ParamId that
// describeParam / getParam resolve, so the editor audit can record which
// parameters an editor's controls read. nullptr (the default) = off.
using ParamReadHook = void (*)(ParamId);
void setParamReadHook(ParamReadHook hook);
// Display name including the owner, e.g. "Kick › Volume".
std::string paramDisplayName(const Song& song, ParamId id);
// All parameters currently present in the song (song params, then per channel).
std::vector<ParamId> listParams(const Song& song);

// --- internal controller links (UI queries) ---------------------------------
// One controller output that links can use as a source.
struct ControllerOutput {
  ObjectId owner = kNoId;  // channel or mixer track
  int slot = -1;           // -1 = channel instrument, else mixer slot
  int output = 0;
  std::string label;       // e.g. "Insert 1 › Peak Follower › Peak"
};
// Every controller output present in the song (channels first, then mixer slots).
std::vector<ControllerOutput> listControllerOutputs(const Song& song);
// Display name of a link source ("?" when it does not resolve).
std::string controllerOutputName(const Song& song, ObjectId owner, int slot, int output);
// True when an enabled internal controller link drives the parameter (knobs
// draw a marker and show the live value instead of the stored one).
bool isModulated(const Song& song, ParamId id);

// Linear ramp to the latest target over a fixed number of samples, so value
// jumps don't click. Realtime-safe (no allocation).
class Smoother {
 public:
  void reset(double value) { cur_ = target_ = value; left_ = 0; }
  void setRampSamples(int n) { ramp_ = n > 0 ? n : 1; }
  void setTarget(double v) {
    if (v == target_) return;
    target_ = v;
    left_ = ramp_;
    step_ = (target_ - cur_) / ramp_;
  }
  double next() {
    if (left_ > 0) {
      cur_ += step_;
      if (--left_ == 0) cur_ = target_;
    }
    return cur_;
  }
  double current() const { return cur_; }
  bool ramping() const { return left_ > 0; }

 private:
  double cur_ = 0.0, target_ = 0.0, step_ = 0.0;
  int ramp_ = 64, left_ = 0;
};

// Single-producer/single-consumer lock-free queue of parameter changes from
// the UI/control thread to the audio thread (the "value bus").
struct ParamChange {
  ParamId id;
  double value = 0.0;
};

class ParamBus {
 public:
  explicit ParamBus(size_t capacityPow2 = 1024) : buf_(capacityPow2), mask_(capacityPow2 - 1) {}
  bool push(const ParamChange& c) {  // producer thread
    size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= buf_.size()) return false;  // full
    buf_[h & mask_] = c;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(ParamChange& out) {  // consumer (audio) thread
    size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    out = buf_[t & mask_];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

 private:
  std::vector<ParamChange> buf_;
  size_t mask_;
  std::atomic<size_t> head_{0}, tail_{0};
};

}  // namespace gl
