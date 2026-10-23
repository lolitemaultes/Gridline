// Gridline — J-97: drop-to-replace on the channel rack (pure parts): where a
// drag over a row lands (replace / insert before / after), which instruments
// take a dropped sample and how it goes into their data, and the channel name
// policy (automatic names follow the new content, typed names stay). The
// interface drops themselves run in tests/j97_rack_drop.cmake.
#include <string>
#include "Check.h"
#include "core/ExternalDrop.h"

using namespace gl;

namespace {

void testRowZone() {
  using xdrop::RowZone;
  // A 26 px row with a 3 px gap: the target spans 29 px from y = 100.
  expect(xdrop::rowZone(101, 100, 29) == RowZone::Before, "top edge: insert before");
  expect(xdrop::rowZone(114.5f, 100, 29) == RowZone::Replace, "middle: replace");
  expect(xdrop::rowZone(128, 100, 29) == RowZone::After, "bottom edge: insert after");
  expect(xdrop::rowZone(100 + 7.25f, 100, 29) == RowZone::Replace && xdrop::rowZone(100 + 7.2f, 100, 29) == RowZone::Before,
         "edge bands are a quarter of the row");
  expect(xdrop::rowZone(102, 100, 8) == RowZone::Before && xdrop::rowZone(105, 100, 8) == RowZone::After, "thin rows: 4 px bands at least");
}

void testSampleSlots() {
  using xdrop::SampleSlot;
  expect(xdrop::sampleSlot("gl.sampler") == SampleSlot::Whole && xdrop::sampleSlot("gl.slicer") == SampleSlot::Whole, "sample players: the data is the path");
  expect(xdrop::sampleSlot("gl.scratch") == SampleSlot::FirstLine && xdrop::sampleSlot("gl.convolver") == SampleSlot::FirstLine,
         "scratch / convolver: the path is the first line");
  expect(xdrop::sampleSlot("gl.pads") == SampleSlot::None && xdrop::sampleSlot("gl.fm") == SampleSlot::None, "drum kits / synths: no single sample");
  expect(xdrop::withSample("gl.sampler", "/old/a.wav", "/new/b.wav") == "/new/b.wav", "whole: replaced");
  expect(xdrop::withSample("gl.scratch", "/old/a.wav\nm 0 ff 1", "/new/b.wav") == "/new/b.wav\nm 0 ff 1", "first line: the moves stay");
  expect(xdrop::withSample("gl.scratch", "", "/new/b.wav") == "/new/b.wav", "first line of empty data");
  expect(xdrop::rowTakesSample("gl.sampler") && xdrop::rowTakesSample("gl.granular") && !xdrop::rowTakesSample("gl.synth") &&
             !xdrop::rowTakesSample("gl.pads"),
         "rows: sample players load the file, other instruments become a sampler");
}

void testNamePolicy() {
  expect(xdrop::autoChannelName("Sampler", "Sampler", ""), "the instrument's name is automatic");
  expect(xdrop::autoChannelName("Kick 01", "Sampler", "/packs/Drums/Kick 01.wav"), "the old sample's name is automatic");
  expect(xdrop::autoChannelName("", "Tercet", ""), "an empty name is automatic");
  expect(!xdrop::autoChannelName("My bass", "Tercet", ""), "a typed name stays");
  expect(!xdrop::autoChannelName("Lead", "Sampler", "/packs/Kick.wav"), "a typed name stays on a sampler too");
  expect(!xdrop::autoChannelName("x", "Sampler", "x\nmodel"), "multi-line data is not a sample path");
}

}  // namespace

int main() {
  testRowZone();
  testSampleSlots();
  testNamePolicy();
  return finish();
}
