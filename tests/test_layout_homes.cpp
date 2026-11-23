// Gridline - K-112: every inventory item has a home in every layout (core/LayoutHomes.h).
//   - ids and window names are unique, every row has a home in Studio, Live, Logic and Tracker;
//   - docked, hosted and part homes name a region of that layout (its "<layout>." prefix);
//   - Studio keeps its own arrangement: its docked homes are the browser, main and mixer regions,
//     and the new parts float there, closed by default;
//   - every built-in plugin of the catalogue has a row (its editor has a home everywhere);
//   - the windows Studio opens by default are the ones it opens today.
#include <set>
#include <string>
#include "Check.h"
#include "core/LayoutHomes.h"
#include "core/PluginInfo.h"

using namespace gl;
using layout::HomeKind;
using layout::Preset;

int main() {
  const auto& items = layout::items();
  expect(items.size() >= 325, "the table lists the whole inventory (325 rows or more)");
  std::set<std::string> ids, windows;
  bool idsUnique = true, windowsUnique = true, regionsOk = true, studioOk = true, partsOk = true, namesOk = true;
  std::string bad;
  for (const layout::Item& it : items) {
    idsUnique &= ids.insert(it.id).second;
    if (*it.window) windowsUnique &= windows.insert(it.window).second;
    namesOk &= *it.name != 0;
    for (int k = 0; k < layout::kLayoutCount; ++k) {
      const Preset p = static_cast<Preset>(k);
      const layout::Home& h = it.home[k];
      const std::string prefix = layout::regionPrefix(p);
      const bool needsRegion = h.kind == HomeKind::Dock || (h.kind == HomeKind::Host && p != Preset::Studio) ||
                               (h.kind == HomeKind::Part && p != Preset::Studio);
      if (needsRegion && std::string(h.region).rfind(prefix, 0) != 0) {
        regionsOk = false;
        bad += std::string(" ") + it.id + "@" + layout::id(p);
      }
      if (!needsRegion && *h.region && std::string(h.region).rfind(prefix, 0) != 0) regionsOk = false;
    }
    const layout::Home& s = it.home[0];
    if (s.kind == HomeKind::Dock) {
      const std::string r = s.region;
      studioOk &= r == "studio.browser" || r == "studio.main" || r == "studio.mixer";
    }
    if (std::string(it.id).rfind("N", 0) == 0) partsOk &= s.kind == HomeKind::Part && !*s.region && !s.openByDefault;
  }
  expect(idsUnique, "every inventory id is unique");
  expect(windowsUnique, "every window belongs to one item");
  expect(namesOk, "every item has a name");
  expect(regionsOk, ("docked, hosted and part homes name a region of their own layout" + bad).c_str());
  expect(studioOk, "Studio docks only into its browser, main and mixer regions (its arrangement is unchanged)");
  expect(partsOk, "the new parts float in Studio, closed by default (View > Panels opens them)");

  // Every built-in plugin's editor has a home in every layout.
  std::string missing;
  for (const PluginInfo& info : pluginCatalog()) {
    if (info.hidden) continue;
    const layout::Item* it = layout::findPlugin(info.type);
    if (!it) missing += " " + info.type;
  }
  expect(missing.empty(), ("every built-in plugin editor has a home row" + missing).c_str());
  {
    // K-379 part 2 (user 2026-11-22, k078-clipsnopanel): the panel above the Clips device band is
    // gone and every effect and instrument editor opened in Clips floats, as in Studio; only a
    // Sampler's settings stay in its card in the device band.
    const layout::Item* eq = layout::findPlugin("gl.eq");
    expect(eq && eq->home[static_cast<int>(Preset::Live)].kind == HomeKind::Float &&
               !*eq->home[static_cast<int>(Preset::Live)].region,
           "an effect editor opened in Clips floats (no panel above the device band)");
    const layout::Item* smp = layout::findPlugin("gl.sampler");
    expect(smp && smp->home[static_cast<int>(Preset::Live)].kind == HomeKind::Host &&
               std::string(smp->home[static_cast<int>(Preset::Live)].region) == "live.device",
           "a Sampler's settings stay in its card in the Clips device band");
    // K-391 (user 2026-11-22 on rc9, k078-trackerfloat): in the Tracker every plugin editor floats
    // in-app like the other layouts and never opens in the lower band; the band keeps only the
    // Tracker's own screens (Sample/Instrument editors, Seltmann's Chop and Instrument screens).
    expect(eq && eq->home[static_cast<int>(Preset::Tracker)].kind == HomeKind::Float &&
               !*eq->home[static_cast<int>(Preset::Tracker)].region,
           "an effect editor opened in the Tracker floats (never in the lower frame)");
    const layout::Item* syn = layout::findPlugin("gl.synth");
    expect(syn && syn->home[static_cast<int>(Preset::Tracker)].kind == HomeKind::Float &&
               !*syn->home[static_cast<int>(Preset::Tracker)].region,
           "an instrument's editor opened in the Tracker floats (never in the lower band)");
    bool trackerFloat = true;
    std::string inBand;
    for (const PluginInfo& info : pluginCatalog()) {
      if (info.hidden) continue;
      const layout::Item* it = layout::findPlugin(info.type);
      if (it) {
        const layout::Home& h = it->home[static_cast<int>(Preset::Tracker)];
        const bool ok = h.kind == HomeKind::Float && !*h.region;
        trackerFloat &= ok;
        if (!ok) inBand += " " + info.type;
      }
    }
    expect(trackerFloat, ("every plugin editor floats in the Tracker, none in the lower band" + inBand).c_str());
    const layout::Item* tsEd = layout::findItem("C26");
    expect(tsEd && tsEd->home[static_cast<int>(Preset::Tracker)].kind == HomeKind::Inside &&
               std::string(tsEd->home[static_cast<int>(Preset::Tracker)].region) == "tracker.lower",
           "the Tracker Sampler editor (a Tracker-native screen) stays inside the lower band");
  }

  // Studio opens the windows it opens today, and only those.
  std::set<std::string> studioOpen;
  for (const layout::Item& it : items)
    if (it.home[0].openByDefault) studioOpen.insert(it.window);
  expect(studioOpen == std::set<std::string>{"Browser", "Playlist", "Piano roll", "Channel rack", "Mixer"},
         "Studio opens the browser, playlist, piano roll, channel rack and mixer by default");
  // The main windows are docked or hosted in every layout, never left without a place.
  for (const char* w : {"Browser", "Playlist", "Piano roll", "Mixer", "Channel rack"}) {
    bool everywhere = true;
    for (int k = 1; k < layout::kLayoutCount; ++k) {
      const layout::Home h = layout::windowHome(w, static_cast<Preset>(k));
      everywhere &= h.kind == HomeKind::Dock && *h.region;
    }
    expect(everywhere, (std::string(w) + " is docked in a region of Live, Logic and Tracker").c_str());
  }
  expect(layout::windowHome("Not a window", Preset::Logic).kind == HomeKind::Float, "windows the table does not list float");
  expect(std::string(layout::windowHome("Mixer", Preset::Studio).region) == "studio.mixer", "Studio's mixer stays on the right");
  return finish();
}
