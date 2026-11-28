// Gridline - the macOS menu bar (K-112): a layout's menus in the system menu bar.
//
// The tree comes from the app's own menus (core/MenuTree.h through app/SystemMenu.cpp). Items carry their id; choosing
// one queues the id and the app runs it on its next frame through the same menu code the
// in-window menus use. Shortcuts are shown at the right of each item but are not key
// equivalents: the keys stay the app's own (its shortcut sets know the focused editor), so a
// key never runs twice. Hide and Hide others get Command+H / Command+Alt+H only while the
// active key set has no command on that chord (setSystemKeys). Manual reference counting, as the other Objective-C++ files here.
#import <AppKit/AppKit.h>
#include <algorithm>
#include <string>
#include "NativeMenu.h"
#include "ObjcClass.h"

namespace {
std::string gChosen;
NSMenu* gOriginal = nil;  // the menu bar the program started with (SDL's)
id gTarget = nil;  // the items' target, of a class made at run time (ObjcClass.h)
bool gInstalled = false;
bool gHideKey = true, gHideOthersKey = true;  // the key set leaves Command+H / Command+Alt+H free

NSString* str(const std::string& s) {
  NSString* r = [NSString stringWithUTF8String:s.c_str()];
  return r ? r : @"";
}

// "Label<tab>Shortcut" with the shortcut right-aligned at `tabAt`.
NSAttributedString* titleWith(const std::string& label, const std::string& shortcut, CGFloat tabAt) {
  NSFont* font = [NSFont menuFontOfSize:0];
  NSMutableParagraphStyle* para = [[[NSMutableParagraphStyle alloc] init] autorelease];
  NSTextTab* tab = [[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentRight location:tabAt options:@{}] autorelease];
  para.tabStops = @[ tab ];
  NSMutableAttributedString* t = [[[NSMutableAttributedString alloc] initWithString:str(label)
                                                                         attributes:@{NSFontAttributeName : font, NSParagraphStyleAttributeName : para}] autorelease];
  NSDictionary* dim = @{NSFontAttributeName : font, NSParagraphStyleAttributeName : para, NSForegroundColorAttributeName : [NSColor secondaryLabelColor]};
  [t appendAttributedString:[[[NSAttributedString alloc] initWithString:[@"\t" stringByAppendingString:str(shortcut)] attributes:dim] autorelease]];
  return t;
}

Class targetClass() {
  static gl::objc::RuntimeClass rc("GridlineMenuTarget", [NSObject class]);
  static Class c = rc.method(@selector(chosen:), +[](id, SEL, id sender) {
                       id obj = [sender representedObject];
                       if ([obj isKindOfClass:[NSString class]]) {
                         const char* t = [(NSString*)obj UTF8String];
                         gChosen = t ? t : "";
                       }
                     }).done();
  return c;
}

NSMenu* build(const std::string& title, const std::vector<gl::host::nativemenu::Item>& items) {
  NSMenu* menu = [[[NSMenu alloc] initWithTitle:str(title)] autorelease];
  menu.autoenablesItems = NO;
  // The shortcut column starts after the widest label.
  NSFont* font = [NSFont menuFontOfSize:0];
  CGFloat labelW = 0, keyW = 0;
  for (const auto& it : items) {
    labelW = std::max(labelW, [str(it.label) sizeWithAttributes:@{NSFontAttributeName : font}].width);
    if (!it.shortcut.empty()) keyW = std::max(keyW, [str(it.shortcut) sizeWithAttributes:@{NSFontAttributeName : font}].width);
  }
  const CGFloat tabAt = labelW + 36 + keyW;
  for (const auto& it : items) {
    if (it.label.empty()) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    NSMenuItem* mi = [[[NSMenuItem alloc] initWithTitle:str(it.label) action:nil keyEquivalent:@""] autorelease];
    if (!it.children.empty()) {
      mi.submenu = build(it.label, it.children);
    } else {
      mi.target = gTarget;
      mi.action = @selector(chosen:);
      mi.representedObject = str(it.id);
      if (!it.shortcut.empty()) mi.attributedTitle = titleWith(it.label, it.shortcut, tabAt);
    }
    mi.enabled = it.enabled ? YES : NO;
    mi.state = it.checked ? NSControlStateValueOn : NSControlStateValueOff;  // (unchecked items show no mark)
    [menu addItem:mi];
  }
  return menu;
}
}  // namespace

namespace gl::host::nativemenu {

bool available() { return NSApp != nil; }

std::string status() { return NSApp != nil ? "cocoa" : "no application object"; }

void setWindow(unsigned long) {}  // one menu bar for the program

void setSystemKeys(const std::vector<std::string>& items) {
  gHideKey = std::find(items.begin(), items.end(), "hide") != items.end();
  gHideOthersKey = std::find(items.begin(), items.end(), "hideOthers") != items.end();
}

void poll() {}  // AppKit calls the target from the event loop SDL runs

void install(const std::vector<Item>& appMenu, const std::vector<Item>& menus) {
  if (!NSApp) return;
  @autoreleasepool {
    if (!gTarget) gTarget = [[targetClass() alloc] init];
    if (!gInstalled) gOriginal = [[NSApp mainMenu] retain];
    NSMenu* bar = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSString* appName = [[NSProcessInfo processInfo] processName];
    if (!appName) appName = @"Gridline";
    NSMenuItem* appItem = [[[NSMenuItem alloc] initWithTitle:appName action:nil keyEquivalent:@""] autorelease];
    NSMenu* app = build("Gridline", appMenu);
    // Quit keeps the system's Command-Q (it ends the program the usual way, through its quit event).
    [app addItem:[NSMenuItem separatorItem]];
    NSMenuItem* hide = [[[NSMenuItem alloc] initWithTitle:[@"Hide " stringByAppendingString:appName] action:@selector(hide:)
                                             keyEquivalent:gHideKey ? @"h" : @""] autorelease];
    [app addItem:hide];
    NSMenuItem* others = [[[NSMenuItem alloc] initWithTitle:@"Hide Others" action:@selector(hideOtherApplications:)
                                               keyEquivalent:gHideOthersKey ? @"h" : @""] autorelease];
    others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app addItem:others];
    appItem.submenu = app;
    [bar addItem:appItem];
    for (const Item& m : menus) {
      NSMenuItem* top = [[[NSMenuItem alloc] initWithTitle:str(m.label) action:nil keyEquivalent:@""] autorelease];
      top.submenu = build(m.label, m.children);
      [bar addItem:top];
    }
    [NSApp setMainMenu:bar];
    gInstalled = true;
  }
}

void uninstall() {
  if (!gInstalled || !NSApp) return;
  @autoreleasepool {
    if (gOriginal) {
      [NSApp setMainMenu:gOriginal];
      [gOriginal release];
      gOriginal = nil;
    }
    gInstalled = false;
  }
}

bool installed() { return gInstalled; }

bool shown() { return gInstalled; }  // the Cocoa menu bar is always on screen

bool busStarted() { return false; }  // no bus: the Cocoa menu bar

std::string takeChosen() {
  std::string c;
  c.swap(gChosen);
  return c;
}

}  // namespace gl::host::nativemenu
