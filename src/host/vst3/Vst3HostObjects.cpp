#include "Vst3HostObjects.h"
#include "../Backend.h"
#include <algorithm>

namespace gl::vst3 {

// ---------------------------------------------------------------- helpers

std::string toUtf8(const sv::TChar* s, size_t maxLen) {
  std::string out;
  for (size_t i = 0; i < maxLen && s[i]; ++i) {
    uint32_t c = static_cast<uint16_t>(s[i]);
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < maxLen && s[i + 1]) {  // surrogate pair
      uint32_t lo = static_cast<uint16_t>(s[i + 1]);
      if (lo >= 0xDC00 && lo < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    // Typographic spaces (no-break, thin, narrow…) become plain spaces: UI fonts lack them.
    if (c == 0xA0 || (c >= 0x2000 && c <= 0x200B) || c == 0x202F || c == 0x205F) c = ' ';
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

void toUtf16(const std::string& s, sv::TChar* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < s.size() && o + 1 < cap;) {
    unsigned char b = static_cast<unsigned char>(s[i]);
    uint32_t c = b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    if (extra) c = b & (0x3F >> extra);
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) c = (c << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    if (c >= 0x10000) {
      if (o + 2 >= cap) break;
      c -= 0x10000;
      out[o++] = static_cast<sv::TChar>(0xD800 + (c >> 10));
      out[o++] = static_cast<sv::TChar>(0xDC00 + (c & 0x3FF));
    } else {
      out[o++] = static_cast<sv::TChar>(c);
    }
  }
  if (cap) out[o] = 0;
}

namespace {
// Class ids are written in the platform-independent form (FUID's four 32-bit words, most
// significant byte first). Windows keeps a TUID in COM's GUID layout (COM_COMPATIBLE: the first
// word and the two 16-bit halves of the second little-endian): the same class id has other bytes
// there, so the bytes are read in this order.
int uidByte(int i) {
#if COM_COMPATIBLE
  static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  return order[i];
#else
  return i;
#endif
}
}  // namespace

std::string uidToHex(const sb::TUID uid) {
  static const char* hex = "0123456789ABCDEF";
  std::string s;
  for (int i = 0; i < 16; ++i) {
    unsigned char b = static_cast<unsigned char>(uid[uidByte(i)]);
    s += hex[b >> 4];
    s += hex[b & 15];
  }
  return s;
}

bool hexToUid(const std::string& h, sb::TUID out) {
  if (h.size() != 32) return false;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (int i = 0; i < 16; ++i) {
    int a = nib(h[static_cast<size_t>(2 * i)]), b = nib(h[static_cast<size_t>(2 * i + 1)]);
    if (a < 0 || b < 0) return false;
    out[uidByte(i)] = static_cast<char>((a << 4) | b);
  }
  return true;
}

std::string base64Encode(const std::string& in) { return host::base64Encode(in); }

bool base64Decode(const std::string& in, std::string& out) { return host::base64Decode(in, out); }

// ---------------------------------------------------------------- MemoryStream

sb::tresult PLUGIN_API MemoryStream::read(void* buffer, sb::int32 numBytes, sb::int32* numBytesRead) {
  if (numBytes < 0 || (!buffer && numBytes > 0)) return sb::kInvalidArgument;
  int64_t avail = static_cast<int64_t>(data_.size()) - pos_;
  int32_t n = static_cast<int32_t>(std::max<int64_t>(0, std::min<int64_t>(numBytes, avail)));
  if (n > 0) std::memcpy(buffer, data_.data() + pos_, static_cast<size_t>(n));
  pos_ += n;
  if (numBytesRead) *numBytesRead = n;
  return sb::kResultOk;
}

sb::tresult PLUGIN_API MemoryStream::write(void* buffer, sb::int32 numBytes, sb::int32* numBytesWritten) {
  if (numBytes < 0 || (!buffer && numBytes > 0)) return sb::kInvalidArgument;
  size_t end = static_cast<size_t>(pos_) + static_cast<size_t>(numBytes);
  if (end > data_.size()) data_.resize(end);
  if (numBytes > 0) std::memcpy(&data_[static_cast<size_t>(pos_)], buffer, static_cast<size_t>(numBytes));
  pos_ = static_cast<int64_t>(end);
  if (numBytesWritten) *numBytesWritten = numBytes;
  return sb::kResultOk;
}

sb::tresult PLUGIN_API MemoryStream::seek(sb::int64 pos, sb::int32 mode, sb::int64* result) {
  int64_t base = mode == sb::IBStream::kIBSeekSet ? 0 : mode == sb::IBStream::kIBSeekCur ? pos_ : static_cast<int64_t>(data_.size());
  int64_t np = base + pos;
  if (np < 0) return sb::kInvalidArgument;
  pos_ = np;
  if (result) *result = pos_;
  return sb::kResultOk;
}

sb::tresult PLUGIN_API MemoryStream::tell(sb::int64* pos) {
  if (!pos) return sb::kInvalidArgument;
  *pos = pos_;
  return sb::kResultOk;
}

sb::tresult PLUGIN_API MemoryStream::getStreamSize(sb::int64& size) {
  size = static_cast<sb::int64>(data_.size());
  return sb::kResultOk;
}

sb::tresult PLUGIN_API MemoryStream::setStreamSize(sb::int64 size) {
  if (size < 0) return sb::kInvalidArgument;
  data_.resize(static_cast<size_t>(size));
  return sb::kResultOk;
}

// ---------------------------------------------------------------- AttributeList

sb::tresult PLUGIN_API AttributeList::setInt(AttrID id, sb::int64 value) {
  if (!id) return sb::kInvalidArgument;
  Value& v = values_[id];
  v.kind = Value::Int;
  v.i = value;
  return sb::kResultOk;
}
sb::tresult PLUGIN_API AttributeList::getInt(AttrID id, sb::int64& value) {
  auto it = id ? values_.find(id) : values_.end();
  if (it == values_.end() || it->second.kind != Value::Int) return sb::kResultFalse;
  value = it->second.i;
  return sb::kResultOk;
}
sb::tresult PLUGIN_API AttributeList::setFloat(AttrID id, double value) {
  if (!id) return sb::kInvalidArgument;
  Value& v = values_[id];
  v.kind = Value::Float;
  v.f = value;
  return sb::kResultOk;
}
sb::tresult PLUGIN_API AttributeList::getFloat(AttrID id, double& value) {
  auto it = id ? values_.find(id) : values_.end();
  if (it == values_.end() || it->second.kind != Value::Float) return sb::kResultFalse;
  value = it->second.f;
  return sb::kResultOk;
}
sb::tresult PLUGIN_API AttributeList::setString(AttrID id, const sv::TChar* string) {
  if (!id || !string) return sb::kInvalidArgument;
  Value& v = values_[id];
  v.kind = Value::String;
  v.s.clear();
  for (size_t i = 0; string[i]; ++i) v.s += static_cast<char16_t>(string[i]);
  return sb::kResultOk;
}
sb::tresult PLUGIN_API AttributeList::getString(AttrID id, sv::TChar* string, sb::uint32 sizeInBytes) {
  auto it = id ? values_.find(id) : values_.end();
  if (it == values_.end() || it->second.kind != Value::String || !string) return sb::kResultFalse;
  size_t cap = sizeInBytes / sizeof(sv::TChar);
  if (cap == 0) return sb::kResultFalse;
  size_t n = std::min(cap - 1, it->second.s.size());
  for (size_t i = 0; i < n; ++i) string[i] = static_cast<sv::TChar>(it->second.s[i]);
  string[n] = 0;
  return sb::kResultOk;
}
sb::tresult PLUGIN_API AttributeList::setBinary(AttrID id, const void* data, sb::uint32 sizeInBytes) {
  if (!id || (!data && sizeInBytes)) return sb::kInvalidArgument;
  Value& v = values_[id];
  v.kind = Value::Binary;
  v.bin.assign(static_cast<const char*>(data), sizeInBytes);
  return sb::kResultOk;
}
sb::tresult PLUGIN_API AttributeList::getBinary(AttrID id, const void*& data, sb::uint32& sizeInBytes) {
  auto it = id ? values_.find(id) : values_.end();
  if (it == values_.end() || it->second.kind != Value::Binary) return sb::kResultFalse;
  data = it->second.bin.data();
  sizeInBytes = static_cast<sb::uint32>(it->second.bin.size());
  return sb::kResultOk;
}

// ---------------------------------------------------------------- Message

Message::Message() : attrs_(new AttributeList) { attrs_->setHeapOwned(); }
Message::~Message() { attrs_->release(); }

// ---------------------------------------------------------------- HostApplication

sb::tresult PLUGIN_API HostApplication::getName(sv::String128 name) {
  toUtf16("Gridline", name, 128);
  return sb::kResultOk;
}

sb::tresult PLUGIN_API HostApplication::createInstance(sb::TUID cid, sb::TUID iid, void** obj) {
  if (!obj) return sb::kInvalidArgument;
  *obj = nullptr;
  auto is = [](const sb::TUID a, const sb::FUID& b) { return std::memcmp(a, b.toTUID(), sizeof(sb::TUID)) == 0; };
  if (is(cid, sv::IMessage::iid) && is(iid, sv::IMessage::iid)) {
    auto* m = new Message;
    m->setHeapOwned();
    *obj = static_cast<sv::IMessage*>(m);
    return sb::kResultOk;
  }
  if (is(cid, sv::IAttributeList::iid) && is(iid, sv::IAttributeList::iid)) {
    auto* l = new AttributeList;
    l->setHeapOwned();
    *obj = static_cast<sv::IAttributeList*>(l);
    return sb::kResultOk;
  }
  return sb::kResultFalse;
}

sb::tresult PLUGIN_API HostApplication::isPlugInterfaceSupported(const sb::TUID iid) {
  const sb::FUID* supported[] = {&sv::IComponent::iid,         &sv::IAudioProcessor::iid,  &sv::IEditController::iid,
                                 &sv::IConnectionPoint::iid,   &sv::IComponentHandler::iid, &sv::IComponentHandler2::iid,
                                 &sv::INoteExpressionController::iid, &sb::IPlugView::iid, &sb::IPlugFrame::iid};
  for (const sb::FUID* f : supported)
    if (std::memcmp(iid, f->toTUID(), sizeof(sb::TUID)) == 0) return sb::kResultTrue;
  return sb::kResultFalse;
}

HostApplication* hostContext() {
  static HostApplication* h = new HostApplication;  // shared by every instance, never freed
  return h;
}

// ---------------------------------------------------------------- parameter changes

sb::tresult PLUGIN_API ParamValueQueue::getPoint(sb::int32 index, sb::int32& sampleOffset, sv::ParamValue& value) {
  if (index < 0 || index >= count_) return sb::kInvalidArgument;
  sampleOffset = points_[index].offset;
  value = points_[index].value;
  return sb::kResultOk;
}

sb::tresult PLUGIN_API ParamValueQueue::addPoint(sb::int32 sampleOffset, sv::ParamValue value, sb::int32& index) {
  // Points stay ordered by offset; a point at an existing offset replaces it.
  for (sb::int32 i = 0; i < count_; ++i)
    if (points_[i].offset == sampleOffset) {
      points_[i].value = value;
      index = i;
      return sb::kResultOk;
    }
  if (count_ >= kMaxPoints) return sb::kResultFalse;
  sb::int32 at = count_;
  while (at > 0 && points_[at - 1].offset > sampleOffset) {
    points_[at] = points_[at - 1];
    --at;
  }
  points_[at] = {sampleOffset, value};
  ++count_;
  index = at;
  return sb::kResultOk;
}

void ParameterChanges::resize(int maxQueues) {
  queues_.clear();
  for (int i = 0; i < maxQueues; ++i) queues_.push_back(std::make_unique<ParamValueQueue>());
  used_ = 0;
}

sv::IParamValueQueue* PLUGIN_API ParameterChanges::getParameterData(sb::int32 index) {
  return index >= 0 && index < used_ ? queues_[static_cast<size_t>(index)].get() : nullptr;
}

sv::IParamValueQueue* PLUGIN_API ParameterChanges::addParameterData(const sv::ParamID& id, sb::int32& index) {
  for (int i = 0; i < used_; ++i)
    if (queues_[static_cast<size_t>(i)]->getParameterId() == id) {
      index = i;
      return queues_[static_cast<size_t>(i)].get();
    }
  if (full()) return nullptr;
  index = used_;
  ParamValueQueue* q = queues_[static_cast<size_t>(used_++)].get();
  q->reset(id);
  return q;
}

ParamValueQueue* ParameterChanges::queueFor(sv::ParamID id) {
  sb::int32 index = 0;
  return static_cast<ParamValueQueue*>(addParameterData(id, index));
}

// ---------------------------------------------------------------- events

sb::tresult PLUGIN_API EventList::getEvent(sb::int32 index, sv::Event& e) {
  if (index < 0 || index >= count_) return sb::kInvalidArgument;
  e = events_[index];
  return sb::kResultOk;
}

sb::tresult PLUGIN_API EventList::addEvent(sv::Event& e) {
  if (count_ >= kMaxEvents) return sb::kResultFalse;
  sb::int32 at = count_;
  while (at > 0 && events_[at - 1].sampleOffset > e.sampleOffset) {
    events_[at] = events_[at - 1];
    --at;
  }
  events_[at] = e;
  ++count_;
  return sb::kResultOk;
}

}  // namespace gl::vst3
