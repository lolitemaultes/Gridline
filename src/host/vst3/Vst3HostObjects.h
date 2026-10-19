// Gridline — VST3 hosting: objects the host hands to plugins (streams,
// messages, host context, parameter change and event lists).
// The parameter/event lists are pre-allocated: filling and reading them on the
// audio thread never allocates.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Vst3Common.h"

namespace gl::vst3 {

// Growable in-memory stream (state chunks).
class MemoryStream final : public HostObject<sb::IBStream, sb::ISizeableStream> {
 public:
  MemoryStream() = default;
  explicit MemoryStream(std::string data) : data_(std::move(data)) {}
  sb::tresult PLUGIN_API read(void* buffer, sb::int32 numBytes, sb::int32* numBytesRead) override;
  sb::tresult PLUGIN_API write(void* buffer, sb::int32 numBytes, sb::int32* numBytesWritten) override;
  sb::tresult PLUGIN_API seek(sb::int64 pos, sb::int32 mode, sb::int64* result) override;
  sb::tresult PLUGIN_API tell(sb::int64* pos) override;
  sb::tresult PLUGIN_API getStreamSize(sb::int64& size) override;
  sb::tresult PLUGIN_API setStreamSize(sb::int64 size) override;
  const std::string& data() const { return data_; }
  void rewind() { pos_ = 0; }

 private:
  std::string data_;
  int64_t pos_ = 0;
};

class AttributeList final : public HostObject<sv::IAttributeList> {
 public:
  sb::tresult PLUGIN_API setInt(AttrID id, sb::int64 value) override;
  sb::tresult PLUGIN_API getInt(AttrID id, sb::int64& value) override;
  sb::tresult PLUGIN_API setFloat(AttrID id, double value) override;
  sb::tresult PLUGIN_API getFloat(AttrID id, double& value) override;
  sb::tresult PLUGIN_API setString(AttrID id, const sv::TChar* string) override;
  sb::tresult PLUGIN_API getString(AttrID id, sv::TChar* string, sb::uint32 sizeInBytes) override;
  sb::tresult PLUGIN_API setBinary(AttrID id, const void* data, sb::uint32 sizeInBytes) override;
  sb::tresult PLUGIN_API getBinary(AttrID id, const void*& data, sb::uint32& sizeInBytes) override;

 private:
  struct Value {
    enum Kind { Int, Float, String, Binary } kind = Int;
    sb::int64 i = 0;
    double f = 0;
    std::u16string s;
    std::string bin;
  };
  std::map<std::string, Value> values_;
};

class Message final : public HostObject<sv::IMessage> {
 public:
  Message();
  ~Message() override;
  sb::FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
  void PLUGIN_API setMessageID(sb::FIDString id) override { id_ = id ? id : ""; }
  sv::IAttributeList* PLUGIN_API getAttributes() override { return attrs_; }

 private:
  std::string id_;
  AttributeList* attrs_;
};

// Host context passed to IPluginBase::initialize.
class HostApplication final : public HostObject<sv::IHostApplication, sv::IPlugInterfaceSupport> {
 public:
  sb::tresult PLUGIN_API getName(sv::String128 name) override;
  sb::tresult PLUGIN_API createInstance(sb::TUID cid, sb::TUID iid, void** obj) override;
  sb::tresult PLUGIN_API isPlugInterfaceSupported(const sb::TUID iid) override;
};
// Process-wide host context (never destroyed).
HostApplication* hostContext();

// One parameter's value changes within a block (fixed capacity).
class ParamValueQueue final : public HostObject<sv::IParamValueQueue> {
 public:
  static constexpr int kMaxPoints = 32;
  sv::ParamID PLUGIN_API getParameterId() override { return id_; }
  sb::int32 PLUGIN_API getPointCount() override { return count_; }
  sb::tresult PLUGIN_API getPoint(sb::int32 index, sb::int32& sampleOffset, sv::ParamValue& value) override;
  sb::tresult PLUGIN_API addPoint(sb::int32 sampleOffset, sv::ParamValue value, sb::int32& index) override;
  void reset(sv::ParamID id) {
    id_ = id;
    count_ = 0;
  }

 private:
  sv::ParamID id_ = 0;
  sb::int32 count_ = 0;
  struct Point {
    sb::int32 offset;
    sv::ParamValue value;
  } points_[kMaxPoints] = {};
};

// Parameter changes of one block: pre-allocated queues, no allocation in use.
class ParameterChanges final : public HostObject<sv::IParameterChanges> {
 public:
  explicit ParameterChanges(int maxQueues = 0) { resize(maxQueues); }
  void resize(int maxQueues);  // UI thread only
  sb::int32 PLUGIN_API getParameterCount() override { return used_; }
  sv::IParamValueQueue* PLUGIN_API getParameterData(sb::int32 index) override;
  sv::IParamValueQueue* PLUGIN_API addParameterData(const sv::ParamID& id, sb::int32& index) override;
  void clear() { used_ = 0; }
  // Host side: queue for `id` (found or added); nullptr when full.
  ParamValueQueue* queueFor(sv::ParamID id);
  bool full() const { return used_ >= static_cast<int>(queues_.size()); }

 private:
  std::vector<std::unique_ptr<ParamValueQueue>> queues_;
  int used_ = 0;
};

// Events of one block, kept sorted by sample offset on insertion (stable).
class EventList final : public HostObject<sv::IEventList> {
 public:
  static constexpr int kMaxEvents = 1024;
  sb::int32 PLUGIN_API getEventCount() override { return count_; }
  sb::tresult PLUGIN_API getEvent(sb::int32 index, sv::Event& e) override;
  sb::tresult PLUGIN_API addEvent(sv::Event& e) override;
  void clear() { count_ = 0; }

 private:
  sv::Event events_[kMaxEvents] = {};
  sb::int32 count_ = 0;
};

}  // namespace gl::vst3
