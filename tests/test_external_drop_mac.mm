// Gridline — J-75 (macOS): the main window as a drag destination, without a
// person dragging: pasteboards written the way Finder / Electron apps (file
// URLs), browsers / Mail / sample stores (file promises), web pages (links)
// and audio apps (raw data) write them, read back as drag items; and the
// window's NSDraggingDestination methods driven with a synthetic dragging
// info (enter → move → perform), checked through the events they post.
#import <AppKit/AppKit.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "core/ExternalDrop.h"

using namespace gl;
namespace fs = std::filesystem;

// Stands in for the system's dragging info (only what Gridline asks for).
@interface GLFakeDrag : NSObject
@property(nonatomic, retain) NSPasteboard* pasteboard;
@property(nonatomic) NSPoint location;
@property(nonatomic) NSDragOperation mask;
@end
@implementation GLFakeDrag
- (NSPasteboard*)draggingPasteboard {
  return self.pasteboard;
}
- (NSPoint)draggingLocation {
  return self.location;
}
- (NSDragOperation)draggingSourceOperationMask {
  return self.mask;
}
@end

// A source app keeping its file promise: writes a small WAV.
@interface GLPromiseSource : NSObject <NSFilePromiseProviderDelegate>
@end
@implementation GLPromiseSource
- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)p fileNameForType:(NSString*)type {
  return @"Promised Loop.wav";
}
- (void)filePromiseProvider:(NSFilePromiseProvider*)p writePromiseToURL:(NSURL*)url completionHandler:(void (^)(NSError*))done {
  NSData* d = [@"RIFF" dataUsingEncoding:NSASCIIStringEncoding];
  [d writeToURL:url atomically:YES];
  done(nil);
}
@end

namespace {
std::vector<xdrop::Event> waitEvents(size_t want, double seconds) {
  std::vector<xdrop::Event> got;
  NSDate* until = [NSDate dateWithTimeIntervalSinceNow:seconds];
  while (got.size() < want && [until timeIntervalSinceNow] > 0) {
    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    for (auto& e : xdrop::take()) got.push_back(e);
  }
  return got;
}
}  // namespace

int main() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    const fs::path tmp = fs::temp_directory_path() / "gridline-test-external-drop-mac";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "Folder");
    std::ofstream((tmp / "Kick.wav").string()) << "RIFF";
    std::ofstream((tmp / "My Pad.wav").string()) << "RIFF";
    xdrop::setDropFolder((tmp / "Dropped").string());
    NSString* (^ns)(const fs::path&) = ^NSString*(const fs::path& p) { return [NSString stringWithUTF8String:p.c_str()]; };

    // ---- file URLs (Finder; Electron's startDrag as used by Tracklib) ----
    {
      NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
      [pb clearContents];
      [pb writeObjects:@[ [NSURL fileURLWithPath:ns(tmp / "Kick.wav")], [NSURL fileURLWithPath:ns(tmp / "My Pad.wav")],
                          [NSURL fileURLWithPath:ns(tmp / "Folder")] ]];
      auto items = xdrop::native::readPasteboard(pb, false, 0);
      expect(items.size() == 3, "file URLs: three items");
      expect(items.size() == 3 && items[1].path == (tmp / "My Pad.wav").string() && items[1].name == "My Pad.wav" && !items[1].pending,
             "file URL: path and name (spaces kept)");
      expect(items.size() == 3 && items[2].dir && !items[0].dir, "file URL: folder detected");
      expect(xdrop::native::describePasteboard(pb).find("public.file-url") != std::string::npos, "pasteboard types described");
      [pb releaseGlobally];
    }

    // ---- web link to audio (downloaded only on drop; not resolved here) ----
    {
      NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
      [pb clearContents];
      [pb writeObjects:@[ [NSURL URLWithString:@"https://example.com/packs/Snare%2002.wav?x=1"] ]];
      auto items = xdrop::native::readPasteboard(pb, false, 0);
      expect(items.size() == 1 && items[0].pending && items[0].name == "Snare 02.wav" && items[0].url.find("https://") == 0, "web link to audio");
      [pb clearContents];
      [pb writeObjects:@[ [NSURL URLWithString:@"https://example.com/page.html"] ]];
      expect(xdrop::native::readPasteboard(pb, false, 0).empty(), "web link to a page is not taken");
      [pb releaseGlobally];
    }

    // ---- raw audio data ----
    {
      NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
      [pb clearContents];
      NSData* wav = [@"RIFF....WAVE" dataUsingEncoding:NSASCIIStringEncoding];
      [pb setData:wav forType:@"com.microsoft.waveform-audio"];
      auto hover = xdrop::native::readPasteboard(pb, false, 0);
      expect(hover.size() == 1 && hover[0].pending && hover[0].name == "Dropped audio.wav", "raw audio data: hover item");
      auto dropped = xdrop::native::readPasteboard(pb, true, 7);
      expect(dropped.size() == 1 && !dropped[0].pending && fs::file_size(dropped[0].path) == 12 &&
                 fs::path(dropped[0].path).parent_path() == tmp / "Dropped",
             "raw audio data: written into the drop folder");
      [pb releaseGlobally];
    }

    // ---- file promise ----
    GLPromiseSource* source = [[GLPromiseSource alloc] init];
    {
      NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
      [pb clearContents];
      NSFilePromiseProvider* prov = [[NSFilePromiseProvider alloc] initWithFileType:@"com.microsoft.waveform-audio" delegate:source];
      [pb writeObjects:@[ prov ]];
      auto items = xdrop::native::readPasteboard(pb, false, 0);
      expect(items.size() == 1 && items[0].pending && items[0].name == "Dropped file.wav" && items[0].path.empty(), "file promise: hover item typed");
      // Receiving needs the drag session's file coordination with the source;
      // outside a real drag the system may not deliver. Checked when it does.
      xdrop::take();
      auto resolved = xdrop::native::readPasteboard(pb, true, 42);
      expect(resolved.size() == 1 && resolved[0].pending, "file promise: still pending at the drop");
      auto ev = waitEvents(1, 3.0);
      if (!ev.empty())
        expect(ev[0].token == 42 && ev[0].index == 0 && (ev[0].type == xdrop::EventType::Arrived || ev[0].type == xdrop::EventType::Failed),
               "file promise: completion event for the drop");
      else
        std::printf("# file promise not delivered without a drag session (expected headless)\n");
      [prov release];
      [pb releaseGlobally];
    }

    // ---- the window as drag destination, driven by a synthetic drag ----
    {
      NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 300) styleMask:NSWindowStyleMaskTitled
                                                  backing:NSBackingStoreBuffered defer:YES];
      expect(xdrop::native::install(w), "install on a window");
      expect(!xdrop::native::install(w), "installed once");
      NSPasteboard* pb = [NSPasteboard pasteboardWithUniqueName];
      [pb clearContents];
      [pb writeObjects:@[ [NSURL fileURLWithPath:ns(tmp / "Kick.wav")] ]];
      GLFakeDrag* drag = [[GLFakeDrag alloc] init];
      drag.pasteboard = pb;
      drag.location = NSMakePoint(100, 50);
      drag.mask = NSDragOperationCopy;  // like Electron apps: copy only (SDL refused these)
      xdrop::take();
      id<NSDraggingInfo> info = static_cast<id<NSDraggingInfo>>(drag);
      const NSDragOperation op = [(id<NSDraggingDestination>)w draggingEntered:info];
      expect(op == NSDragOperationCopy, "copy-only source accepted");
      auto ev = xdrop::take();
      expect(ev.size() == 1 && ev[0].type == xdrop::EventType::Enter && ev[0].items.size() == 1 && ev[0].x == 100 && ev[0].y == 250 && ev[0].hasMods,
             "enter: items and pointer (top-left based)");
      drag.location = NSMakePoint(120, 200);
      [(id<NSDraggingDestination>)w draggingUpdated:info];
      ev = xdrop::take();
      expect(ev.size() == 1 && ev[0].type == xdrop::EventType::Move && ev[0].x == 120 && ev[0].y == 100, "move: pointer");
      expect([(id<NSDraggingDestination>)w wantsPeriodicDraggingUpdates], "periodic updates (modifier keys while resting)");
      expect([(id<NSDraggingDestination>)w prepareForDragOperation:info] && [(id<NSDraggingDestination>)w performDragOperation:info], "drop accepted");
      ev = xdrop::take();
      expect(ev.size() == 1 && ev[0].type == xdrop::EventType::Drop && ev[0].token > 0 && ev[0].items.size() == 1 &&
                 ev[0].items[0].path == (tmp / "Kick.wav").string(),
             "drop: items, token");
      xdrop::setCancelled(true);
      expect(![(id<NSDraggingDestination>)w performDragOperation:info], "cancelled drag (Escape) refuses the drop");
      xdrop::setCancelled(false);
      [(id<NSDraggingDestination>)w draggingExited:info];
      ev = xdrop::take();
      expect(ev.size() == 1 && ev[0].type == xdrop::EventType::Leave, "leave");
      [drag release];
      [pb releaseGlobally];
      [w release];
    }
    [source release];
    fs::remove_all(tmp);
  }
  return finish();
}
