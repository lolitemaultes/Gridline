// Gridline — J-75: the main window as a drag destination on macOS.
//
// SDL makes its NSWindow subclass the drag destination itself, registered for
// file URLs only, and it accepts a drag only when the source offers the
// "generic" operation: Finder does, but Electron apps (Tracklib, Splice …)
// offer "copy" only, so their drags were refused; promises (browsers, Mail,
// Photos) and web links were not even registered; and SDL reports files only
// on release (SDL_DROPFILE), without the hover position.
//
// Here SDL's window class gets NSDraggingDestination methods (method
// swizzling; other windows of the class keep SDL's) that, for the main
// window, post xdrop events: Enter with the dragged items and the pointer, Move with the
// pointer and modifier keys (periodic updates keep them current while the
// pointer rests), Leave, and Drop. Mouse handling is untouched: no view is put
// over SDL's content view. Pending items (file promises, downloads, raw audio
// data) are resolved on drop into the drop folder on background queues.
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include "../../core/ExternalDrop.h"

namespace gl::xdrop::native {

namespace {

NSWindow* gWindow = nil;
NSOperationQueue* gQueue = nil;

NSOperationQueue* promiseQueue() {
  if (!gQueue) {
    gQueue = [[NSOperationQueue alloc] init];
    gQueue.qualityOfService = NSQualityOfServiceUserInitiated;
    gQueue.name = @"Gridline dropped files";
  }
  return gQueue;
}

bool logTypes() {
  static const bool on = [] {
    const char* e = std::getenv("GRIDLINE_DND_LOG");
    return e && *e && *e != '0';
  }();
  return on;
}

std::string str(NSString* s) { return s ? std::string(s.UTF8String ? s.UTF8String : "") : std::string(); }

std::string extensionOf(NSString* uti) {
  std::string e = extensionForUti(str(uti));
  if (!e.empty()) return e;
  if (@available(macOS 11.0, *)) {
    // UTType is resolved at run time (no link-time dependency on the framework).
    Class cls = NSClassFromString(@"UTType");
    if (cls && [cls respondsToSelector:@selector(typeWithIdentifier:)]) {
      id t = [cls performSelector:@selector(typeWithIdentifier:) withObject:uti];
      if (t && [t respondsToSelector:@selector(preferredFilenameExtension)]) {
        NSString* ext = [t performSelector:@selector(preferredFilenameExtension)];
        if (ext.length) return "." + str(ext);
      }
    }
  }
  return "";
}

Mods currentMods() {
  const NSEventModifierFlags f = [NSEvent modifierFlags];
  Mods m;
  m.shift = (f & NSEventModifierFlagShift) != 0;
  m.alt = (f & NSEventModifierFlagOption) != 0;
  m.ctrl = (f & NSEventModifierFlagCommand) != 0;
  return m;
}

void locate(id<NSDraggingInfo> info, float& x, float& y) {
  NSView* cv = [gWindow contentView];
  const NSPoint v = [cv convertPoint:[info draggingLocation] fromView:nil];
  x = static_cast<float>(v.x);
  y = static_cast<float>(cv.isFlipped ? v.y : cv.bounds.size.height - v.y);
}

// The operation shown by the pointer: copy over a target that takes the
// drag, generic elsewhere (the drop then says why nothing happened), always
// one the source offers.
NSDragOperation operationFor(id<NSDraggingInfo> info, bool haveItems) {
  if (cancelled() || !haveItems) return NSDragOperationNone;
  const NSDragOperation mask = [info draggingSourceOperationMask];
  const NSDragOperation want = hoverAccepted() ? NSDragOperationCopy : NSDragOperationGeneric;
  if (mask & want) return want;
  for (NSDragOperation o : {NSDragOperationCopy, NSDragOperationGeneric, NSDragOperationLink, NSDragOperationMove})
    if (mask & o) return o;
  return NSDragOperationNone;
}

bool gHaveItems = false;

// ---- resolving pending items ----

struct Download {
  NSURLSessionDownloadTask* task;
  uint64_t token;
  size_t index;
};
std::mutex gDlMu;
std::vector<Download> gDownloads;

void receivePromise(NSFilePromiseReceiver* r, uint64_t token, size_t base, size_t n) {
  const std::string folder = dropFolder();
  NSString* dir = [NSString stringWithUTF8String:folder.c_str()];
  [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  NSURL* dest = [NSURL fileURLWithPath:dir isDirectory:YES];
  auto next = std::make_shared<std::atomic<size_t>>(base);
  [r receivePromisedFilesAtDestination:dest
                               options:@{}
                        operationQueue:promiseQueue()
                                reader:^(NSURL* fileURL, NSError* error) {
                                  size_t idx = (*next)++;
                                  if (idx >= base + n) idx = base + n - 1;
                                  Event e;
                                  e.token = token;
                                  e.index = idx;
                                  if (error || !fileURL) {
                                    e.type = EventType::Failed;
                                    e.error = error ? str(error.localizedDescription) : std::string("no file");
                                  } else {
                                    e.type = EventType::Arrived;
                                    e.path = fileURL.fileSystemRepresentation;
                                  }
                                  post(e);
                                }];
}

bool listableName(const std::string& name) { return browse::listable(browse::kindOf(name)); }

}  // namespace

std::string describePasteboard(void* pasteboard) {
  NSPasteboard* pb = static_cast<NSPasteboard*>(pasteboard);
  std::string out;
  int k = 0;
  for (NSPasteboardItem* item in pb.pasteboardItems) {
    out += "item " + std::to_string(k++) + ":";
    for (NSString* t in item.types) out += " " + str(t);
    out += "\n";
  }
  return out;
}

std::vector<Item> readPasteboard(void* pasteboard, bool resolve, uint64_t token) {
  NSPasteboard* pb = static_cast<NSPasteboard*>(pasteboard);
  std::vector<Item> out;
  @autoreleasepool {
    // 1. Files that exist (Finder, file managers, Electron's startDrag).
    NSArray* files = [pb readObjectsForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL* u in files) {
      Item it;
      it.path = u.fileSystemRepresentation;
      it.name = str(u.lastPathComponent);
      BOOL dir = NO;
      [[NSFileManager defaultManager] fileExistsAtPath:u.path isDirectory:&dir];
      it.dir = dir;
      out.push_back(it);
    }
    if (!out.empty()) return out;

    // 2. File promises: the source writes the files once the drop happened.
    NSArray* recvs = [pb readObjectsForClasses:@[ [NSFilePromiseReceiver class] ] options:nil];
    for (NSFilePromiseReceiver* r in recvs) {
      NSArray<NSString*>* types = r.fileTypes;
      const size_t n = std::max<size_t>(1, types.count);
      const size_t base = out.size();
      for (size_t k = 0; k < n; ++k) {
        Item it;
        it.pending = true;
        const std::string ext = k < types.count ? extensionOf(types[k]) : std::string();
        it.guessed = ext.empty();
        it.name = std::string("Dropped file") + (recvs.count > 1 || n > 1 ? " " + std::to_string(base + k + 1) : std::string()) + ext;
        out.push_back(it);
      }
      if (resolve) receivePromise(r, token, base, n);
    }
    if (!out.empty()) return out;

    // 3. Links to audio / MIDI on the web: downloaded only when dropped.
    NSMutableArray<NSString*>* links = [NSMutableArray array];
    for (NSURL* u in [pb readObjectsForClasses:@[ [NSURL class] ] options:nil])
      if (u.absoluteString) [links addObject:u.absoluteString];
    if (NSString* s = [pb stringForType:NSPasteboardTypeString])
      if (![links containsObject:s]) [links addObject:[s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]]];
    for (NSString* l in links) {
      const std::string url = str(l);
      if (!isWebUrl(url) || !listableName(fileNameForUrl(url))) continue;
      Item it;
      it.url = url;
      it.name = fileNameForUrl(url);
      it.pending = true;
      if (resolve) download(token, out.size(), url);
      out.push_back(it);
    }
    if (!out.empty()) return out;

    // 4. Raw audio data.
    for (const std::string& uti : audioUtis()) {
      NSString* t = [NSString stringWithUTF8String:uti.c_str()];
      if (![pb availableTypeFromArray:@[ t ]]) continue;
      Item it;
      it.name = "Dropped audio" + extensionOf(t);
      if (resolve) {
        NSData* d = [pb dataForType:t];
        const std::string path = uniquePath(dropFolder(), it.name);
        if (d && [d writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES]) it.path = path;
      } else {
        it.pending = true;
      }
      if (!resolve || !it.path.empty()) out.push_back(it);
      break;
    }
  }
  return out;
}

void download(uint64_t token, size_t index, const std::string& url) {
  NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
  if (!u) {
    Event e;
    e.type = EventType::Failed;
    e.token = token;
    e.index = index;
    e.error = "invalid link";
    post(e);
    return;
  }
  const std::string folder = dropFolder(), fallback = fileNameForUrl(url);
  __block NSURLSessionDownloadTask* task = nil;
  task = [[NSURLSession sharedSession]
      downloadTaskWithURL:u
        completionHandler:^(NSURL* location, NSURLResponse* response, NSError* error) {
          Event e;
          e.token = token;
          e.index = index;
          const NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ? static_cast<NSHTTPURLResponse*>(response).statusCode : 200;
          if (error || !location || status >= 400) {
            e.type = EventType::Failed;
            e.error = error ? str(error.localizedDescription) : "HTTP " + std::to_string(static_cast<long>(status));
          } else {
            std::string name = str(response.suggestedFilename);
            if (name.empty() || browse::kindOf(name) == browse::Kind::Other) name = fallback;
            const std::string dest = uniquePath(folder, name);
            NSError* moveErr = nil;
            if ([[NSFileManager defaultManager] moveItemAtURL:location
                                                        toURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:dest.c_str()]]
                                                        error:&moveErr]) {
              e.type = EventType::Arrived;
              e.path = dest;
            } else {
              e.type = EventType::Failed;
              e.error = str(moveErr.localizedDescription);
            }
          }
          post(e);
          std::lock_guard<std::mutex> l(gDlMu);
          for (size_t k = 0; k < gDownloads.size(); ++k)
            if (gDownloads[k].task == task) {
              [gDownloads[k].task release];
              gDownloads.erase(gDownloads.begin() + static_cast<long>(k));
              break;
            }
        }];
  {
    std::lock_guard<std::mutex> l(gDlMu);
    gDownloads.push_back({[task retain], token, index});
  }
  [task resume];
}

void poll() {
  std::lock_guard<std::mutex> l(gDlMu);
  for (const Download& d : gDownloads) {
    Event e;
    e.type = EventType::Progress;
    e.token = d.token;
    e.index = d.index;
    const int64_t want = d.task.countOfBytesExpectedToReceive, got = d.task.countOfBytesReceived;
    e.progress = want > 0 ? static_cast<double>(got) / static_cast<double>(want) : -1.0;
    post(e);
  }
}

}  // namespace gl::xdrop::native

// ---- the destination methods ----
// SDL's window class gets these in place of its own; other windows of that
// class (none today) keep SDL's behaviour through the saved originals. The
// object's own class is left alone: SDL observes the window with key value
// observing, which already swaps it for a private subclass.
namespace {
using namespace gl::xdrop;

using OpFn = NSDragOperation (*)(id, SEL, id<NSDraggingInfo>);
using VoidFn = void (*)(id, SEL, id<NSDraggingInfo>);
using BoolFn = BOOL (*)(id, SEL, id<NSDraggingInfo>);
OpFn origEntered = nullptr, origUpdated = nullptr;
VoidFn origExited = nullptr, origConclude = nullptr;
BoolFn origPrepare = nullptr, origPerform = nullptr;
BOOL (*origPeriodic)(id, SEL) = nullptr;

NSDragOperation glDraggingEntered(id self, SEL sel, id<NSDraggingInfo> info) {
  if (self != native::gWindow) return origEntered ? origEntered(self, sel, info) : NSDragOperationNone;
  setCancelled(false);
  NSPasteboard* pb = [info draggingPasteboard];
  if (native::logTypes()) std::fprintf(stderr, "[drop] drag entered, source mask 0x%lx\n%s", static_cast<unsigned long>([info draggingSourceOperationMask]),
                                       native::describePasteboard(pb).c_str());
  Event e;
  e.type = EventType::Enter;
  native::locate(info, e.x, e.y);
  e.mods = native::currentMods();
  e.hasMods = true;
  e.items = native::readPasteboard(pb, false, 0);
  native::gHaveItems = !e.items.empty();
  post(e);
  return native::operationFor(info, native::gHaveItems);
}

NSDragOperation glDraggingUpdated(id self, SEL sel, id<NSDraggingInfo> info) {
  if (self != native::gWindow) return origUpdated ? origUpdated(self, sel, info) : NSDragOperationNone;
  Event e;
  e.type = EventType::Move;
  native::locate(info, e.x, e.y);
  e.mods = native::currentMods();
  e.hasMods = true;
  post(e);
  return native::operationFor(info, native::gHaveItems);
}

void glDraggingExited(id self, SEL sel, id<NSDraggingInfo> info) {
  if (self != native::gWindow) {
    if (origExited) origExited(self, sel, info);
    return;
  }
  Event e;
  e.type = EventType::Leave;
  post(e);
}

BOOL glPrepare(id self, SEL sel, id<NSDraggingInfo> info) {
  if (self != native::gWindow) return origPrepare ? origPrepare(self, sel, info) : YES;
  return !cancelled() && native::gHaveItems;
}

BOOL glPerform(id self, SEL sel, id<NSDraggingInfo> info) {
  if (self != native::gWindow) return origPerform ? origPerform(self, sel, info) : NO;
  if (cancelled()) return NO;
  Event e;
  e.type = EventType::Drop;
  native::locate(info, e.x, e.y);
  e.mods = native::currentMods();
  e.hasMods = true;
  e.token = newToken();
  e.items = native::readPasteboard([info draggingPasteboard], true, e.token);
  if (native::logTypes()) std::fprintf(stderr, "[drop] dropped %zu items at %.0f %.0f\n", e.items.size(), e.x, e.y);
  post(e);
  return e.items.empty() ? NO : YES;
}

void glConclude(id self, SEL sel, id<NSDraggingInfo> info) {
  if (self != native::gWindow && origConclude) origConclude(self, sel, info);
}

BOOL glPeriodic(id self, SEL sel) {
  if (self != native::gWindow) return origPeriodic ? origPeriodic(self, sel) : NO;
  return YES;  // keeps the modifier keys current while the pointer rests
}

const char* typesOf(SEL sel) {
  objc_method_description d = protocol_getMethodDescription(@protocol(NSDraggingDestination), sel, NO, YES);
  return d.types;
}

// Installs `fn` for `sel` on `cls`; returns the implementation it had (own or inherited).
IMP hook(Class cls, SEL sel, IMP fn) {
  Method m = class_getInstanceMethod(cls, sel);
  IMP old = m ? method_getImplementation(m) : nullptr;
  if (!class_addMethod(cls, sel, fn, m ? method_getTypeEncoding(m) : typesOf(sel))) method_setImplementation(class_getInstanceMethod(cls, sel), fn);
  return old;
}
}  // namespace

namespace gl::xdrop::native {

bool install(void* nativeWindow) {
  NSWindow* w = static_cast<NSWindow*>(nativeWindow);
  if (!w || gWindow) return false;
  Class cls = [w class];  // SDL's window class (not the observing subclass)
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    origEntered = reinterpret_cast<OpFn>(hook(cls, @selector(draggingEntered:), reinterpret_cast<IMP>(glDraggingEntered)));
    origUpdated = reinterpret_cast<OpFn>(hook(cls, @selector(draggingUpdated:), reinterpret_cast<IMP>(glDraggingUpdated)));
    origExited = reinterpret_cast<VoidFn>(hook(cls, @selector(draggingExited:), reinterpret_cast<IMP>(glDraggingExited)));
    origPrepare = reinterpret_cast<BoolFn>(hook(cls, @selector(prepareForDragOperation:), reinterpret_cast<IMP>(glPrepare)));
    origPerform = reinterpret_cast<BoolFn>(hook(cls, @selector(performDragOperation:), reinterpret_cast<IMP>(glPerform)));
    origConclude = reinterpret_cast<VoidFn>(hook(cls, @selector(concludeDragOperation:), reinterpret_cast<IMP>(glConclude)));
    origPeriodic = reinterpret_cast<BOOL (*)(id, SEL)>(hook(cls, @selector(wantsPeriodicDraggingUpdates), reinterpret_cast<IMP>(glPeriodic)));
  }
  gWindow = w;
  NSMutableArray<NSString*>* types = [NSMutableArray arrayWithArray:@[ NSPasteboardTypeFileURL, NSPasteboardTypeURL, NSPasteboardTypeString,
                                                                       @"NSFilenamesPboardType" ]];
  [types addObjectsFromArray:[NSFilePromiseReceiver readableDraggedTypes]];
  for (const std::string& u : audioUtis()) [types addObject:[NSString stringWithUTF8String:u.c_str()]];
  [w registerForDraggedTypes:types];
  return true;
}

}  // namespace gl::xdrop::native
