// Gridline - K-105: First-run setup wizard & interactive guided tour unit tests.
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include "Check.h"
#include "core/Demo.h"
#include "core/LayoutPreset.h"
#include "core/Project.h"
#include "core/Settings.h"
#include "core/SetupTour.h"

using namespace gl;
using namespace gl::setuptour;
namespace fs = std::filesystem;

namespace {

bool rectsOverlap(const Rect& a, const Rect& b) {
  return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

bool rectInside(const Rect& inner, const Rect& outer) {
  return inner.x >= outer.x - 0.5f && inner.y >= outer.y - 0.5f &&
         inner.x + inner.w <= outer.x + outer.w + 0.5f && inner.y + inner.h <= outer.y + outer.h + 0.5f;
}

void testFirstRunPolicy() {
  FirstRunInputs base;
  base.plainLaunch = true;
  expect(shouldShowFirstRunWizard(base), "brand-new plain interactive launch shows setup wizard");

  {
    FirstRunInputs x = base;
    x.setupDone = true;
    expect(!shouldShowFirstRunWizard(x), "ui.setupDone=true suppresses wizard");
  }
  {
    FirstRunInputs x = base;
    x.layoutChosen = true;
    expect(!shouldShowFirstRunWizard(x), "ui.layoutChosen=true (0.7.8 upgrade / tests) suppresses wizard");
  }
  {
    FirstRunInputs x = base;
    x.hidden = true;
    expect(!shouldShowFirstRunWizard(x), "--hidden suppresses wizard");
  }
  {
    FirstRunInputs x = base;
    x.quietWindows = true;
    expect(!shouldShowFirstRunWizard(x), "GRIDLINE_QUIET_WINDOWS=1 suppresses wizard");
  }
  {
    FirstRunInputs x = base;
    x.automated = true;
    expect(!shouldShowFirstRunWizard(x), "automated/screenshot/dev-input suppresses wizard");
  }
  {
    FirstRunInputs x = base;
    x.hasPluginArg = true;
    expect(!shouldShowFirstRunWizard(x), "--plugin suppresses wizard");
  }
  {
    FirstRunInputs x = base;
    x.embedded = true;
    expect(!shouldShowFirstRunWizard(x), "embedded plugin host suppresses wizard");
  }
  {
    FirstRunInputs x = base;
    x.plainLaunch = false;
    expect(!shouldShowFirstRunWizard(x), "non-plain launch (project file / CLI flags) suppresses wizard");
  }
}

void testSettingsPersistence() {
  Settings s;
  expect(!s.getBool("ui.setupDone"), "ui.setupDone defaults to false");
  expect(!isSetupDone(s), "fresh settings are not setupDone");

  Settings upgraded;
  upgraded.set("ui.layoutChosen", true);
  expect(isSetupDone(upgraded), "existing user with ui.layoutChosen=true is treated as setupDone");

  markSetupDone(s);
  expect(s.getBool("ui.setupDone") && s.getBool("ui.layoutChosen"), "markSetupDone sets both keys");
  expect(isSetupDone(s), "isSetupDone returns true after markSetupDone");

  const fs::path dir = fs::temp_directory_path() / "gridline_k105_settings";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const std::string path = (dir / "settings.json").string();
  expect(s.save(path), "settings save succeeds");

  Settings loaded;
  expect(loaded.load(path), "settings reload succeeds");
  expect(loaded.getBool("ui.setupDone"), "ui.setupDone survives save/load");
  expect(loaded.getBool("ui.layoutChosen"), "ui.layoutChosen survives save/load");
  fs::remove_all(dir, ec);
}

void testLookPresets() {
  for (int i = 0; i < kLookPresetCount; ++i) {
    const auto p = static_cast<LookPreset>(i);
    const LookSpec& sp = lookSpec(p);
    expect(sp.id && *sp.id && sp.name && *sp.name && sp.blurb && *sp.blurb, "look preset has id, name, and blurb");
    Settings s;
    applyLookToSettings(p, s);
    expect(detectLookPreset(s) == p, (std::string("look preset round-trip: ") + sp.id).c_str());
  }
}

void testTourStepsPerLayout() {
  for (int k = 0; k < layout::kPresetCount; ++k) {
    const auto preset = static_cast<layout::Preset>(k);
    const auto& steps = tourStepsFor(preset);
    expect(steps.size() >= 5, (std::string("layout has >=5 tour steps: ") + layout::id(preset)).c_str());
    std::set<std::string> ids;
    bool allValid = true;
    for (const TourStep& st : steps) {
      allValid &= st.id && *st.id && st.title && *st.title && st.body && *st.body && st.tip && *st.tip;
      allValid &= ids.insert(st.id).second;
    }
    expect(allValid, (std::string("every tour step has unique id and copy: ") + layout::id(preset)).c_str());
  }

  auto hasTarget = [](layout::Preset p, TourTarget t) {
    for (const TourStep& s : tourStepsFor(p))
      if (s.target == t) return true;
    return false;
  };
  expect(hasTarget(layout::Preset::Studio, TourTarget::ChannelRack), "Studio tour highlights Channel rack");
  expect(hasTarget(layout::Preset::Live, TourTarget::DeviceChain), "Clips tour highlights Device chain");
  expect(hasTarget(layout::Preset::Logic, TourTarget::Inspector), "Smooth tour highlights Inspector");
  expect(hasTarget(layout::Preset::Tracker, TourTarget::TrackerHeader), "Tracker tour highlights Song header");
  expect(hasTarget(layout::Preset::Tracker, TourTarget::PatternEditor), "Tracker tour highlights Pattern editor");
}

void testEmptySongDetection() {
  const Song empty = makeEmptySong();
  expect(isSongEmpty(empty), "makeEmptySong is detected as empty");

  Song withDemo = makeDemoTrack();
  expect(!isSongEmpty(withDemo), "demo track is not empty");
}

void testCalloutPlacement() {
  const Rect vp{0.0f, 0.0f, 1440.0f, 900.0f};
  const float cardW = 360.0f, cardH = 220.0f;

  // Browser on left -> card should sit to the right without overlapping.
  {
    const Rect spot = clampSpotlightRect(vp, {0.0f, 48.0f, 240.0f, 826.0f});
    const CardPlacement pl = placeCalloutCard(vp, spot, cardW, cardH);
    expect(pl.side == CardSide::Right, "left browser places callout on the right");
    expect(rectInside(pl.card, vp), "browser callout stays inside viewport");
    expect(!rectsOverlap(pl.card, spot), "browser callout does not overlap spotlight");
  }
  // Mixer on right -> card should sit to the left without overlapping.
  {
    const Rect spot = clampSpotlightRect(vp, {1100.0f, 48.0f, 340.0f, 826.0f});
    const CardPlacement pl = placeCalloutCard(vp, spot, cardW, cardH);
    expect(pl.side == CardSide::Left, "right mixer places callout on the left");
    expect(rectInside(pl.card, vp), "mixer callout stays inside viewport");
    expect(!rectsOverlap(pl.card, spot), "mixer callout does not overlap spotlight");
  }
  // Top bar -> card should sit below without overlapping.
  {
    const Rect spot = clampSpotlightRect(vp, {0.0f, 0.0f, 1440.0f, 48.0f});
    const CardPlacement pl = placeCalloutCard(vp, spot, cardW, cardH);
    expect(pl.side == CardSide::Bottom, "top bar places callout below");
    expect(rectInside(pl.card, vp), "top bar callout stays inside viewport");
    expect(!rectsOverlap(pl.card, spot), "top bar callout does not overlap spotlight");
  }
  // Bottom device chain -> card should sit above without overlapping.
  {
    const Rect spot = clampSpotlightRect(vp, {0.0f, 650.0f, 1440.0f, 224.0f});
    const CardPlacement pl = placeCalloutCard(vp, spot, cardW, cardH);
    expect(pl.side == CardSide::Top, "bottom device chain places callout above");
    expect(rectInside(pl.card, vp), "device chain callout stays inside viewport");
    expect(!rectsOverlap(pl.card, spot), "device chain callout does not overlap spotlight");
  }
  // Nearly full-screen target -> clean inset fallback still strictly inside viewport.
  {
    const Rect spot = clampSpotlightRect(vp, {8.0f, 52.0f, 1424.0f, 836.0f});
    const CardPlacement pl = placeCalloutCard(vp, spot, cardW, cardH);
    expect(pl.side == CardSide::Inset, "full-screen target uses clean inset fallback");
    expect(rectInside(pl.card, vp), "inset callout stays strictly inside viewport");
  }
}

}  // namespace

int main() {
  testFirstRunPolicy();
  testSettingsPersistence();
  testLookPresets();
  testTourStepsPerLayout();
  testEmptySongDetection();
  testCalloutPlacement();
  return finish();
}
