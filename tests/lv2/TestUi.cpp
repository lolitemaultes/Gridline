// Gridline — interfaces of the in-tree LV2 test plugins (built into
// test-lv2/GridlineTest.lv2 next to the plugins; never installed).
//   urn:gridline:lv2test:state#extui  external (kxstudio external-ui)
//       interface: show / hide / run counted in the probe; on show it sends
//       an Int atom (10) to the plugin's control port; Int atoms from the
//       plugin are recorded; the test can ask it to report "closed".
//   urn:gridline:lv2test:gain#ui      Cocoa interface (macOS, TestUiCocoa.mm).
#include <cstring>
#include <new>
#include "Probe.h"
#include "lv2/atom/atom.h"
#include "lv2/core/lv2.h"
#include "lv2/ui/ui.h"
#include "lv2/urid/urid.h"

GridlineLv2UiProbe gUiProbe;

// kxstudio external-ui ABI (a widget of three callbacks, a host struct),
// declared at global scope with C linkage so host and interface agree on the types.
extern "C" {
struct GridlineKxExternalWidget {
  void (*run)(GridlineKxExternalWidget*);
  void (*show)(GridlineKxExternalWidget*);
  void (*hide)(GridlineKxExternalWidget*);
};
}

#if defined(__APPLE__)
const LV2UI_Descriptor* gridlineTestCocoaUi();  // TestUiCocoa.mm
#endif

namespace {

struct ExternalHost {
  void (*ui_closed)(LV2UI_Controller);
  const char* plugin_human_id;
};

struct ExtUi {
  GridlineKxExternalWidget widget;  // first member: the widget pointer is the object
  const ExternalHost* host = nullptr;
  LV2UI_Write_Function write = nullptr;
  LV2UI_Controller controller = nullptr;
  LV2_URID atomInt = 0, eventTransfer = 0;
};

const LV2_Feature* feature(const LV2_Feature* const* f, const char* uri) {
  for (; f && *f; ++f)
    if (!std::strcmp((*f)->URI, uri)) return *f;
  return nullptr;
}

LV2UI_Handle extNew(const LV2UI_Descriptor*, const char*, const char*, LV2UI_Write_Function write, LV2UI_Controller controller,
                    LV2UI_Widget* widget, const LV2_Feature* const* features) {
  const LV2_Feature* fh = feature(features, "http://kxstudio.sf.net/ns/lv2ext/external-ui#Host");
  const LV2_Feature* fm = feature(features, LV2_URID__map);
  if (!fh || !fm) return nullptr;
  auto* ui = new (std::nothrow) ExtUi;
  if (!ui) return nullptr;
  ui->host = static_cast<const ExternalHost*>(fh->data);
  ui->write = write;
  ui->controller = controller;
  auto* map = static_cast<LV2_URID_Map*>(fm->data);
  ui->atomInt = map->map(map->handle, LV2_ATOM__Int);
  ui->eventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);
  ui->widget.run = [](GridlineKxExternalWidget* w) {
    auto* u = reinterpret_cast<ExtUi*>(w);
    ++gUiProbe.runs;
    if (gUiProbe.closeNow) {
      gUiProbe.closeNow = false;
      u->host->ui_closed(u->controller);
    }
  };
  ui->widget.show = [](GridlineKxExternalWidget* w) {
    auto* u = reinterpret_cast<ExtUi*>(w);
    ++gUiProbe.shows;
    LV2_Atom_Int msg{{sizeof(int32_t), u->atomInt}, 10};
    u->write(u->controller, 5, sizeof msg, u->eventTransfer, &msg);
  };
  ui->widget.hide = [](GridlineKxExternalWidget*) { ++gUiProbe.hides; };
  *widget = &ui->widget;
  return ui;
}
void extFree(LV2UI_Handle h) { delete static_cast<ExtUi*>(h); }
void extPortEvent(LV2UI_Handle h, uint32_t, uint32_t size, uint32_t format, const void* buffer) {
  auto* u = static_cast<ExtUi*>(h);
  if (format != u->eventTransfer || size < sizeof(LV2_Atom)) return;
  const auto* a = static_cast<const LV2_Atom*>(buffer);
  ++gUiProbe.atomEvents;
  if (a->type == u->atomInt && a->size == sizeof(int32_t)) gUiProbe.lastAtomInt = static_cast<const LV2_Atom_Int*>(buffer)->body;
}

const LV2UI_Descriptor kExtUi = {"urn:gridline:lv2test:state#extui", extNew, extFree, extPortEvent, nullptr};

}  // namespace

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index) {
  if (index == 0) return &kExtUi;
#if defined(__APPLE__)
  if (index == 1) return gridlineTestCocoaUi();
#endif
  return nullptr;
}

LV2_SYMBOL_EXPORT GridlineLv2UiProbe* gridline_lv2_ui_probe() { return &gUiProbe; }
