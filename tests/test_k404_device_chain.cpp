// Gridline - K-404: unit tests for Studio Device chain layout, dynamic height-fit scaling,
// viewport culling (CPU guard), slot reorder remapping, Junction FX insertion, Undo/Redo, and
// save/load round-trip.
#include <cmath>
#include <string>
#include <vector>
#include "Check.h"
#include "core/DeviceChainLayout.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/Project.h"

using namespace gl;
using namespace gl::devchain;

int main() {
  // 1. Height-fit scaling law & clamping
  {
    expect(std::fabs(fitScale(360.0f, 360.0f) - 1.0f) < 0.01f, "fitScale: exact 1:1 height gives 1.0");
    expect(fitScale(180.0f, 360.0f) > 0.49f && fitScale(180.0f, 360.0f) <= 0.50f, "fitScale: half height scales to ~0.50");
    expect(std::fabs(fitScale(40.0f, 500.0f) - kMinScale) < 0.006f, "fitScale: clamps small panels to kMinScale");
    expect(std::fabs(fitScale(1200.0f, 200.0f) - kMaxScale) < 0.006f, "fitScale: clamps tall panels to kMaxScale");
    expect(fitScale(420.0f, 360.0f) > fitScale(260.0f, 360.0f), "fitScale: resizing panel taller grows scale");
  }

  // 2. Edge-to-edge snapped row geometry (instrument + effects + collapsed + hosted + add card)
  {
    std::vector<DeviceSpec> specs;
    specs.push_back(DeviceSpec{558.0f, 540.0f, false, false, true});   // Instrument (e.g. Trio Synth)
    specs.push_back(DeviceSpec{780.0f, 420.0f, false, false, false});  // EQ
    specs.push_back(DeviceSpec{600.0f, 340.0f, true, false, false});   // Collapsed Compressor
    specs.push_back(DeviceSpec{600.0f, 360.0f, false, true, false});   // Installed 3rd-party plugin card
    specs.push_back(DeviceSpec{760.0f, 500.0f, false, false, false});  // Junction FX

    const RowLayout rSmall = computeRow(specs, 260.0f, 0.0f, 1200.0f, true);
    const RowLayout rTall = computeRow(specs, 480.0f, 0.0f, 1200.0f, true);
    expect(rSmall.boxes.size() == 5, "row layout produces one box per device");
    expect(std::fabs(rSmall.cardH - 260.0f) < 0.5f, "every card snaps to panel height (260 px)");
    expect(std::fabs(rTall.cardH - 480.0f) < 0.5f, "every card snaps to resized panel height (480 px)");
    for (size_t i = 0; i < rSmall.boxes.size(); ++i) {
      expect(std::fabs(rSmall.boxes[i].h - rSmall.cardH) < 0.01f, "card height matches row height");
      expect(std::fabs(rTall.boxes[i].h - rTall.cardH) < 0.01f, "resized card height matches row height");
    }
    expect(std::fabs(rSmall.boxes[2].w - kCollapsedW) < 0.01f, "collapsed device uses slim kCollapsedW strip");
    expect(std::fabs(rSmall.boxes[3].w - kHostedCardW) < 0.01f, "installed plugin uses compact kHostedCardW card");
    expect(rSmall.boxes[0].w >= kMinExpandedW, "expanded instrument card respects minimum header width");
    expect(rTall.boxes[1].scale > rSmall.boxes[1].scale, "resizing panel taller dynamically scales up built-in EQ");
    expect(rTall.boxes[4].scale > rSmall.boxes[4].scale, "resizing panel taller dynamically scales up Junction FX");
    expect(rTall.boxes[1].w > rSmall.boxes[1].w, "resizing panel taller widens scaled built-in card proportionally");
    // Edge-to-edge adjacency check
    expect(std::fabs((rSmall.boxes[0].x + rSmall.boxes[0].w + kSignalGap) - rSmall.boxes[1].x) < 0.01f,
           "instrument followed by signal-flow gap before first effect");
    expect(std::fabs((rSmall.boxes[1].x + rSmall.boxes[1].w + kGap) - rSmall.boxes[2].x) < 0.01f,
           "effects sit edge to edge with kGap");
    expect(std::fabs((rSmall.boxes[4].x + rSmall.boxes[4].w + kGap) - rSmall.addX) < 0.01f,
           "Add effect card sits right after last effect");
    expect(std::fabs((rSmall.addX + kAddCardW) - rSmall.totalW) < 0.01f, "totalW includes Add effect card");
  }

  // 3. Horizontal viewport culling (CPU guard: off-screen devices marked visible = false)
  {
    std::vector<DeviceSpec> many;
    for (int i = 0; i < 8; ++i) many.push_back(DeviceSpec{600.0f, 360.0f, false, false, i == 0});
    const RowLayout left = computeRow(many, 320.0f, 0.0f, 800.0f, true);
    int visLeft = 0, hiddenLeft = 0;
    for (const auto& b : left.boxes) (b.visible ? visLeft : hiddenLeft)++;
    expect(visLeft >= 1 && visLeft < 8, "at scrollX=0 only first few devices are visible");
    expect(hiddenLeft >= 4, "far-right devices are culled off-screen");
    expect(left.boxes[0].visible && !left.boxes[7].visible, "device 0 visible and device 7 culled at scrollX=0");

    const float farScroll = left.boxes[6].x;
    const RowLayout right = computeRow(many, 320.0f, farScroll, 800.0f, true);
    expect(!right.boxes[0].visible && !right.boxes[1].visible, "scrolled-left devices are culled off-screen");
    expect(right.boxes[6].visible && right.boxes[7].visible, "scrolled-into-view far-right devices are visible");
  }

  // 4. Slot reorder index remapping
  {
    expect(remapSlotOnMove(1, 1, 3) == 3, "moved slot follows to destination");
    expect(remapSlotOnMove(2, 1, 3) == 1, "intermediate slot shifts down on forward move");
    expect(remapSlotOnMove(3, 1, 3) == 2, "destination slot shifts down on forward move");
    expect(remapSlotOnMove(0, 1, 3) == 0, "earlier slot unchanged on forward move");
    expect(remapSlotOnMove(3, 3, 1) == 1, "moved slot follows on backward move");
    expect(remapSlotOnMove(1, 3, 1) == 2, "destination slot shifts up on backward move");
    expect(remapSlotOnMove(2, 3, 1) == 3, "intermediate slot shifts up on backward move");
  }

  // 5. Song / Document integration: Junction FX in chain + reorder + bypass + undo + save/load round-trip
  {
    const PluginInfo* jfx = findPlugin("gl.patcherfx");
    expect(jfx != nullptr && jfx->kind == PluginKind::Effect, "Junction FX (gl.patcherfx) is registered as a loadable Effect");
    const PluginInfo* jgen = findPlugin("gl.patcher");
    expect(jgen != nullptr && jgen->kind == PluginKind::Generator, "Junction (gl.patcher) is registered as a loadable Generator");

    Document doc(makeEmptySong());
    expect(!doc.song().mixer.empty(), "empty song has mixer tracks");
    const ObjectId trackId = doc.song().mixer.size() > 1 ? doc.song().mixer[1].id : doc.song().mixer[0].id;
    expect(doc.setSlotPlugin(trackId, 0, "gl.eq"), "insert EQ in slot 0");
    expect(doc.setSlotPlugin(trackId, 1, "gl.patcherfx"), "insert Junction FX in slot 1");
    expect(doc.setSlotPlugin(trackId, 2, "gl.reverb"), "insert Reverb in slot 2");
    expect(doc.song().mixerTrack(trackId)->slots[1].plugin.type == "gl.patcherfx", "Junction FX occupies slot 1");

    // Reorder Junction FX from slot 1 to slot 0
    doc.moveSlot(trackId, 1, 0);
    expect(doc.song().mixerTrack(trackId)->slots[0].plugin.type == "gl.patcherfx", "moveSlot moves Junction FX to slot 0");
    expect(doc.song().mixerTrack(trackId)->slots[1].plugin.type == "gl.eq", "EQ shifted to slot 1");

    // Undo restores original order in one step
    expect(doc.undo(), "undo reorder succeeds");
    expect(doc.song().mixerTrack(trackId)->slots[0].plugin.type == "gl.eq", "undo restores EQ to slot 0");
    expect(doc.song().mixerTrack(trackId)->slots[1].plugin.type == "gl.patcherfx", "undo restores Junction FX to slot 1");

    // Redo reapplies reorder
    expect(doc.redo(), "redo reorder succeeds");
    expect(doc.song().mixerTrack(trackId)->slots[0].plugin.type == "gl.patcherfx", "redo puts Junction FX back at slot 0");

    // Save / load JSON round-trip preserves Junction FX and chain order
    const Json j = songToJson(doc.song());
    Song loaded;
    std::string err;
    expect(songFromJson(j, loaded, &err), ("song JSON round-trip succeeds: " + err).c_str());
    const MixerTrack* mtLoaded = loaded.mixerTrack(trackId);
    expect(mtLoaded != nullptr, "loaded song has mixer track");
    expect(mtLoaded && mtLoaded->slots[0].plugin.type == "gl.patcherfx", "Junction FX preserved in slot 0 after round-trip");
    expect(mtLoaded && mtLoaded->slots[1].plugin.type == "gl.eq", "EQ preserved in slot 1 after round-trip");
    expect(mtLoaded && mtLoaded->slots[2].plugin.type == "gl.reverb", "Reverb preserved in slot 2 after round-trip");
  }

  return finish();
}
