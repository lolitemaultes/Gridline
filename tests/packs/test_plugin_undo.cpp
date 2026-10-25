// Gridline - undo of plugin editor tweaks (user report: "Ctrl+Z must undo a knob turned in
// Plasma"). The model side of "one pointer gesture = one undo step" (Document::
// beginPointerGesture, the UI opens it while a mouse button is down or the wheel turns): a knob
// drag, a drag that moves several parameters at once, a drawn shape that rewrites the plugin's
// data every frame, a group made during the gesture; two gestures with the same merge key stay
// two steps; an undo during a gesture stops the joining; Document::amendStep (a hosted plugin's
// captured state folded into its own gesture's step) and moveSerial. The app side (real Ctrl+Z
// in the main window and in a detached plugin window) is tests/plugin_undo_app.cmake.
#include <cmath>
#include <cstdio>
#include <string>
#include "../../src/core/Document.h"
#include "../../src/core/Params.h"
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Song.h"
#include "../Check.h"

using namespace gl;

namespace {

double pv(const Document& d, ParamId id) { return getParam(d.song(), id).value_or(-1.0); }
bool near(double a, double b) { return std::fabs(a - b) < 1e-5; }  // parameters are stored as float

// The way plugin editors write their data (PluginUI.cpp setPluginState).
void setData(Document& d, ObjectId ch, const std::string& data, uint64_t key) {
  d.editChannel(ch, "edit data", [&](Channel& c) { c.plugin.data = data; }, key);
}

void testKnobDrag() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("Plasma", "gl.plasma");
  const ParamId p = channelPluginParam(ch, 0), q = channelPluginParam(ch, 1);
  const double p0 = pv(d, p), q0 = pv(d, q);
  const size_t base = d.history().position();
  d.beginPointerGesture();
  for (int i = 1; i <= 120; ++i) d.setParam(p, p0 + i * 0.001, true);  // one frame each
  d.endGesture();  // the knob's own mouse-up
  d.endPointerGesture();
  expect(d.history().position() == base + 1, "a knob drag of 120 frames is one undo step");
  d.undo();
  expect(near(pv(d, p), p0), "undo restores the value before the drag");
  d.redo();
  expect(near(pv(d, p), p0 + 0.12), "redo restores the value at the end of the drag");
  // an XY pad: two parameters alternately (two merge keys), every frame
  d.beginPointerGesture();
  for (int i = 1; i <= 60; ++i) {
    d.setParam(p, p0 + i * 0.002, true);
    d.setParam(q, q0 + i * 0.003, true);
  }
  d.endPointerGesture();
  expect(d.history().position() == base + 2, "a drag moving two parameters every frame is one undo step");
  d.undo();
  expect(near(pv(d, p), p0 + 0.12) && near(pv(d, q), q0), "its undo restores both parameters");
  d.redo();
  expect(near(pv(d, p), p0 + 0.12) && near(pv(d, q), q0 + 0.18), "its redo brings both back");
  // a typed value / menu pick (merge off) outside a gesture: one step each
  d.setParam(p, 0.3, false);
  d.setParam(p, 0.4, false);
  expect(d.history().position() == base + 4, "typed values and menu picks are one step each");
}

void testDataEdits() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("Plasma", "gl.plasma");
  const std::string d0 = d.song().channel(ch)->plugin.data;
  const size_t base = d.history().position();
  // a drawn shape that rewrites the data every frame without a merge key
  d.beginPointerGesture();
  for (int i = 0; i < 50; ++i) setData(d, ch, "shape " + std::to_string(i), 0);
  d.endPointerGesture();
  expect(d.history().position() == base + 1, "a drawn shape rewriting the plugin data every frame is one undo step");
  // two strokes with the same merge key (an editor that never ends its gesture itself)
  const uint64_t key = 0x504C0000ull + 1300;
  d.beginPointerGesture();
  for (int i = 0; i < 5; ++i) setData(d, ch, "lfo a" + std::to_string(i), key);
  d.endPointerGesture();
  d.beginPointerGesture();
  for (int i = 0; i < 5; ++i) setData(d, ch, "lfo b" + std::to_string(i), key);
  d.endPointerGesture();
  expect(d.history().position() == base + 3, "two strokes with the same merge key stay two undo steps");
  d.undo();
  expect(d.song().channel(ch)->plugin.data == "lfo a4", "undo takes back only the second stroke");
  d.undo();
  d.undo();
  expect(d.song().channel(ch)->plugin.data == d0, "undo all: the data as it was");
  d.redo();
  d.redo();
  d.redo();
  expect(d.song().channel(ch)->plugin.data == "lfo b4", "redo all: the data after the last stroke");
  // a parameter and the data in one gesture (Plasma: a knob that also rewrites a table)
  const ParamId p = channelPluginParam(ch, 0);
  const double p0 = pv(d, p);
  d.beginPointerGesture();
  for (int i = 1; i <= 10; ++i) {
    d.setParam(p, p0 + i * 0.01, true);
    setData(d, ch, "mixed " + std::to_string(i), 0);
  }
  d.endPointerGesture();
  d.undo();
  expect(near(pv(d, p), p0) && d.song().channel(ch)->plugin.data == "lfo b4", "parameter and data edits of one gesture undo together");
  d.redo();
  expect(near(pv(d, p), p0 + 0.1) && d.song().channel(ch)->plugin.data == "mixed 10", "and redo together");
}

void testGroupsAndUndoInside() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("Plasma", "gl.plasma");
  const ObjectId mt = d.song().mixer.size() > 1 ? d.song().mixer[1].id : d.song().mixer[0].id;
  const std::string d0 = d.song().channel(ch)->plugin.data;
  const size_t base = d.history().position();
  // FX rack edits (add, then a preset load as a group) made while the button is held
  d.beginPointerGesture();
  d.setSlotPlugin(mt, 0, "gl.delay");
  d.beginGroup("load preset");
  setData(d, ch, "preset", 0);
  d.setParam(channelPluginParam(ch, 0), 0.25, false);
  d.endGroup();
  d.endPointerGesture();
  expect(d.history().position() == base + 1, "edits and a group made during one gesture are one undo step");
  d.undo();
  expect(d.song().mixerTrack(mt)->slots[0].plugin.type.empty() && d.song().channel(ch)->plugin.data == d0, "and undo as one");
  d.redo();
  // an undo during the gesture (the editor's own undo button): later edits start a new step
  const ParamId p = channelPluginParam(ch, 2);
  d.beginPointerGesture();
  d.setParam(p, 0.1, true);
  d.undo();
  d.setParam(p, 0.2, true);
  d.endPointerGesture();
  expect(d.history().position() == base + 2 && d.history().size() == base + 2, "an undo during a gesture: the edits after it are a new step");
  // the history mark of a collaborative rename never joins a gesture
  d.beginPointerGesture();
  d.setParam(p, 0.3, true);
  d.recordMark("rename project");
  d.endPointerGesture();
  expect(d.history().undoLabel() == "rename project", "a history mark stays a step of its own");
}

void testAmendStep() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("Hosted", "gl.plasma");
  const std::string d0 = d.song().channel(ch)->plugin.data;
  const ParamId p = channelPluginParam(ch, 0);
  const double p0 = pv(d, p);
  const size_t base = d.history().position();
  // a hosted plugin's interface: a gesture of edits, then its state captured a moment later
  for (int i = 1; i <= 8; ++i) d.setParam(p, p0 + i * 0.01, true);
  d.endGesture();
  const uint64_t own = d.history().stateSerial();
  expect(d.amendStep(own, [&] { setData(d, ch, "captured", 0x7374617465ull); }), "the captured state folds into the plugin's own step");
  expect(d.history().position() == base + 1, "a gesture in a plugin's own interface plus its state: one undo step");
  d.undo();
  expect(near(pv(d, p), p0) && d.song().channel(ch)->plugin.data == d0, "undo restores the value and the state together");
  expect(d.moveSerial() == d.history().stateSerial(), "moveSerial: the state right after the undo");
  d.redo();
  expect(near(pv(d, p), p0 + 0.08) && d.song().channel(ch)->plugin.data == "captured", "redo brings both back");
  // anything else in between: no folding (a step of its own)
  d.setParam(channelPluginParam(ch, 1), 0.5, false);
  expect(d.moveSerial() != d.history().stateSerial(), "an edit after the redo: moveSerial no longer matches");
  expect(!d.amendStep(own, [&] { setData(d, ch, "later", 0); }), "a stale serial does not fold");
  expect(d.history().position() == base + 3, "the change is then a step of its own");
  expect(!d.amendStep(0, [] {}), "serial 0 never folds");
}

}  // namespace

int main() {
  testKnobDrag();
  testDataEdits();
  testGroupsAndUndoInside();
  testAmendStep();
  return finish();
}
