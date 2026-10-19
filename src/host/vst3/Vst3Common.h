// Gridline — VST3 hosting: shared includes and small helpers.
// Interfaces come from Steinberg's VST 3 SDK interface headers (MIT licence),
// vendored unmodified in third_party/vst3/pluginterfaces.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstpluginterfacesupport.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace gl::vst3 {

namespace sb = Steinberg;
namespace sv = Steinberg::Vst;

// Reference-counted COM-style object implementing the listed interfaces.
// Objects owned by a host class (members) may ignore the count: `owned`
// objects are never deleted through release().
template <class... Ifaces>
class HostObject : public Ifaces... {
 public:
  virtual ~HostObject() = default;
  sb::tresult PLUGIN_API queryInterface(const sb::TUID iid, void** obj) override {
    if (queryAll<Ifaces...>(iid, obj)) return sb::kResultOk;
    if (std::memcmp(iid, sb::FUnknown::iid.toTUID(), sizeof(sb::TUID)) == 0) {
      *obj = static_cast<sb::FUnknown*>(static_cast<First*>(this));
      addRef();
      return sb::kResultOk;
    }
    return queryExtra(iid, obj);
  }
  sb::uint32 PLUGIN_API addRef() override { return ++refs_; }
  sb::uint32 PLUGIN_API release() override {
    sb::uint32 r = --refs_;
    if (r == 0 && heap_) delete this;
    return r;
  }
  // Heap objects handed to plugins delete themselves when the count drops to 0.
  void setHeapOwned() { heap_ = true; }

 protected:
  // Additional interfaces (e.g. platform-specific); default none.
  virtual sb::tresult queryExtra(const sb::TUID iid, void** obj) {
    *obj = nullptr;
    (void)iid;
    return sb::kNoInterface;
  }

 private:
  template <class... Ts>
  struct FirstOf;
  template <class T, class... Ts>
  struct FirstOf<T, Ts...> {
    using type = T;
  };
  using First = typename FirstOf<Ifaces...>::type;
  template <class I, class... Rest>
  bool queryAll(const sb::TUID iid, void** obj) {
    if (std::memcmp(iid, I::iid.toTUID(), sizeof(sb::TUID)) == 0) {
      *obj = static_cast<I*>(this);
      addRef();
      return true;
    }
    if constexpr (sizeof...(Rest) > 0) return queryAll<Rest...>(iid, obj);
    return false;
  }
  std::atomic<sb::uint32> refs_{1};
  bool heap_ = false;
};

// Owning smart pointer for interfaces obtained from a plugin (release on reset).
template <class I>
class Ptr {
 public:
  Ptr() = default;
  explicit Ptr(I* p) : p_(p) {}  // adopts one reference
  Ptr(const Ptr&) = delete;
  Ptr& operator=(const Ptr&) = delete;
  Ptr(Ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  Ptr& operator=(Ptr&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  ~Ptr() { reset(); }
  void reset() {
    if (p_) p_->release();
    p_ = nullptr;
  }
  I* get() const { return p_; }
  I* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  // Queries interface J from any FUnknown; empty when unsupported.
  template <class J>
  static Ptr<J> query(sb::FUnknown* from) {
    void* out = nullptr;
    if (from && from->queryInterface(J::iid.toTUID(), &out) == sb::kResultOk && out) return Ptr<J>(static_cast<J*>(out));
    return Ptr<J>();
  }

 private:
  I* p_ = nullptr;
};

// UTF-16 (String128 etc.) to UTF-8.
std::string toUtf8(const sv::TChar* s, size_t maxLen = 128);
// UTF-8 to a zero-terminated UTF-16 buffer of `cap` units.
void toUtf16(const std::string& s, sv::TChar* out, size_t cap);
// 32 upper-case hex digits of a class id (byte order as stored).
std::string uidToHex(const sb::TUID uid);
bool hexToUid(const std::string& hex, sb::TUID out);

// Base64 (RFC 4648) for state chunks stored in PluginState::data.
std::string base64Encode(const std::string& bytes);
bool base64Decode(const std::string& text, std::string& bytes);

}  // namespace gl::vst3
