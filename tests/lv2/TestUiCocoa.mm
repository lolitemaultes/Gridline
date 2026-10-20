// Gridline — Cocoa interface of the in-tree LV2 test gain (see TestUi.cpp).
// A 320x180 view inside ui:parent; asks the host for that size through
// ui:resize; every gain value it receives is written back, halved, to the
// "echo" port (a round trip host -> interface -> host).
#import <AppKit/AppKit.h>
#include <cstring>
#include <new>
#include "Probe.h"
#include "lv2/core/lv2.h"
#include "lv2/ui/ui.h"

extern GridlineLv2UiProbe gUiProbe;

namespace {

struct CocoaUi {
  NSView* view = nil;
  LV2UI_Write_Function write = nullptr;
  LV2UI_Controller controller = nullptr;
};

const LV2_Feature* feature(const LV2_Feature* const* f, const char* uri) {
  for (; f && *f; ++f)
    if (!std::strcmp((*f)->URI, uri)) return *f;
  return nullptr;
}

LV2UI_Handle cocoaNew(const LV2UI_Descriptor*, const char*, const char*, LV2UI_Write_Function write, LV2UI_Controller controller,
                      LV2UI_Widget* widget, const LV2_Feature* const* features) {
  const LV2_Feature* fp = feature(features, LV2_UI__parent);
  if (!fp || !fp->data) return nullptr;
  auto* ui = new (std::nothrow) CocoaUi;
  if (!ui) return nullptr;
  ui->write = write;
  ui->controller = controller;
  ui->view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 180)];
  [ui->view setWantsLayer:YES];
  [[ui->view layer] setBackgroundColor:[[NSColor colorWithCalibratedRed:0.16 green:0.42 blue:0.55 alpha:1] CGColor]];
  NSTextField* label = [NSTextField labelWithString:@"LV2 test interface (CocoaUI)"];
  [label setTextColor:[NSColor whiteColor]];
  [label setFrameOrigin:NSMakePoint(16, 80)];
  [ui->view addSubview:label];
  [(NSView*)fp->data addSubview:ui->view];
  gUiProbe.width = 320;
  gUiProbe.height = 180;
  if (const LV2_Feature* fr = feature(features, LV2_UI__resize)) {
    auto* r = static_cast<const LV2UI_Resize*>(fr->data);
    gUiProbe.resizeAsked = r->ui_resize(r->handle, 320, 180) == 0;
  }
  ++gUiProbe.instances;
  *widget = ui->view;
  return ui;
}
void cocoaFree(LV2UI_Handle h) {
  auto* ui = static_cast<CocoaUi*>(h);
  [ui->view removeFromSuperview];
  [ui->view release];
  --gUiProbe.instances;
  delete ui;
}
void cocoaPortEvent(LV2UI_Handle h, uint32_t port, uint32_t size, uint32_t format, const void* buffer) {
  auto* ui = static_cast<CocoaUi*>(h);
  ++gUiProbe.portEvents;
  if (port == 4 && format == 0 && size == sizeof(float)) {
    float v = 0;
    std::memcpy(&v, buffer, sizeof v);
    if (v != gUiProbe.lastGain) {
      gUiProbe.lastGain = v;
      const float echo = v * 0.5f;
      ui->write(ui->controller, 8, sizeof echo, 0, &echo);
    }
  }
}
int cocoaIdle(LV2UI_Handle) {
  ++gUiProbe.idles;
  return 0;
}
int cocoaResize(LV2UI_Feature_Handle h, int w, int hh) {
  auto* ui = static_cast<CocoaUi*>(h);
  [ui->view setFrameSize:NSMakeSize(w, hh)];
  gUiProbe.width = w;
  gUiProbe.height = hh;
  return 0;
}
const void* cocoaExtension(const char* uri) {
  static const LV2UI_Idle_Interface idle = {cocoaIdle};
  static const LV2UI_Resize resize = {nullptr, cocoaResize};  // the host passes the UI handle
  if (!std::strcmp(uri, LV2_UI__idleInterface)) return &idle;
  if (!std::strcmp(uri, LV2_UI__resize)) return &resize;
  return nullptr;
}

const LV2UI_Descriptor kCocoaUi = {"urn:gridline:lv2test:gain#ui", cocoaNew, cocoaFree, cocoaPortEvent, cocoaExtension};

}  // namespace

const LV2UI_Descriptor* gridlineTestCocoaUi() { return &kCocoaUi; }
