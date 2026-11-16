// Gridline - Objective-C classes made at run time (macOS), under names that belong to this
// loaded copy of the code.
//
// Classes compiled in (@implementation) are global in a process: two plugin bundles that carry
// the same binary (Plasma.vst3 and Loupe.vst3 are both the suite's engine) would register the same
// class names, the runtime logs "Class X is implemented in both" and picks one of them at random.
// A class made here is named "<stem>_<tag>", the tag taken from an address inside this image, so
// every loaded copy has its own classes. Methods are plain functions (id self, SEL, args...); the
// class is disposed of when the image unloads.
//
// Only classes found by name from outside (an Audio Unit's view factory, which hosts look up with
// -[NSBundle classNamed:]) must stay compiled in; those get a per-bundle name when packaged.
#pragma once
#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string>

namespace gl::objc {

// "<stem>_<hex>": the same in one image, different in every other loaded image.
std::string uniqueName(const char* stem);

template <typename T>
inline std::string encodeOne() {
  return @encode(T);
}

class RuntimeClass {
 public:
  RuntimeClass(const char* stem, Class super) : name_(uniqueName(stem)) {
    cls_ = objc_getClass(name_.c_str()) ? Nil : objc_allocateClassPair(super, name_.c_str(), 0);
  }
  RuntimeClass(const RuntimeClass&) = delete;
  RuntimeClass& operator=(const RuntimeClass&) = delete;
  ~RuntimeClass() {
    if (cls_ && registered_) objc_disposeClassPair(cls_);
  }

  // A pointer-sized instance variable (zero in a new object).
  RuntimeClass& pointer(const char* ivar) {
    if (cls_) class_addIvar(cls_, ivar, sizeof(void*), alignof(void*) == 8 ? 3 : 2, "^v");
    return *this;
  }
  template <typename R, typename... A>
  RuntimeClass& method(SEL sel, R (*fn)(id, SEL, A...)) {
    if (cls_) {
      const std::string types = encodeOne<R>() + "@:" + (std::string() + ... + encodeOne<A>());
      class_addMethod(cls_, sel, reinterpret_cast<IMP>(fn), types.c_str());
    }
    return *this;
  }
  RuntimeClass& conforms(Protocol* p) {
    if (cls_ && p) class_addProtocol(cls_, p);
    return *this;
  }
  // Registers the class (once); Nil if the runtime refused it.
  Class done() {
    if (cls_ && !registered_) {
      objc_registerClassPair(cls_);
      registered_ = true;
    }
    return cls_;
  }
  Class cls() const { return registered_ ? cls_ : Nil; }
  const std::string& name() const { return name_; }

 private:
  std::string name_;
  Class cls_ = Nil;
  bool registered_ = false;
};

// A pointer instance variable of `obj` by name (nullptr when the object has none).
inline void** pointerIvar(id obj, const char* ivar) {
  if (!obj) return nullptr;
  Ivar iv = class_getInstanceVariable(object_getClass(obj), ivar);
  return iv ? reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + ivar_getOffset(iv)) : nullptr;
}
template <typename T>
inline T* getPointer(id obj, const char* ivar) {
  void** p = pointerIvar(obj, ivar);
  return p ? static_cast<T*>(*p) : nullptr;
}
inline void setPointer(id obj, const char* ivar, void* value) {
  if (void** p = pointerIvar(obj, ivar)) *p = value;
}

// [super sel] from inside a method of a class made here (`cls` is that class).
template <typename R = void, typename... A>
inline R callSuper(id self, Class cls, SEL sel, A... args) {
  struct objc_super s{self, class_getSuperclass(cls)};
  return reinterpret_cast<R (*)(struct objc_super*, SEL, A...)>(objc_msgSendSuper)(&s, sel, args...);
}

}  // namespace gl::objc
#endif
