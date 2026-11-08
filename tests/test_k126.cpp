// Gridline: K-126, the channel rack's solo (right-click on a channel's LED).
//  * a solo leaves the channel playing and mutes every other one, in one undo step;
//  * the same solo again restores the mute states from before it (a channel that was muted stays
//    muted, the others play again), in one undo step;
//  * soloing another channel while one is soloed keeps the states from before the first solo;
//  * undo and redo of a solo and an unsolo; a single channel; an unknown channel does nothing.
#include <map>
#include <string>
#include "Check.h"
#include "core/Document.h"
#include "core/MenuOps.h"

using namespace gl;

namespace {

std::string mutes(const Document& d) {
  std::string s;
  for (const auto& c : d.song().channels) s += c.muted ? 'm' : '-';
  return s;
}

}  // namespace

int main() {
  Document d;
  while (!d.song().channels.empty()) d.removeChannel(d.song().channels.back().id);
  const ObjectId a = d.addChannel("A", "gl.synth"), b = d.addChannel("B", "gl.synth"), c = d.addChannel("C", "gl.synth"),
                 e = d.addChannel("D", "gl.synth");
  d.setParam(channelParam(c, ChannelParam::Mute), 1, false);  // C was muted by hand
  expect(mutes(d) == "--m-", "start: C muted by hand");
  std::map<ObjectId, bool> before;

  size_t h = d.history().position();
  expect(ops::toggleChannelSolo(d, b, before), "the first solo reports a solo");
  expect(mutes(d) == "m-mm", "solo B: only B plays");
  expect(d.history().position() == h + 1, "the solo is one undo step");
  expect(d.history().undoLabel() == "solo channel", "undo label: solo channel");

  h = d.history().position();
  expect(!ops::toggleChannelSolo(d, b, before), "the same solo again reports an unsolo");
  expect(mutes(d) == "--m-", "unsolo B: the states from before come back (C stays muted)");
  expect(d.history().position() == h + 1, "the unsolo is one undo step");
  expect(d.history().undoLabel() == "unsolo channel", "undo label: unsolo channel");

  d.undo();
  expect(mutes(d) == "m-mm", "undo of the unsolo: B soloed again");
  d.undo();
  expect(mutes(d) == "--m-", "undo of the solo: the old states");
  d.redo();
  expect(mutes(d) == "m-mm", "redo of the solo");
  d.redo();
  expect(mutes(d) == "--m-", "redo of the unsolo");

  // Solo A, then D (moving the solo), then D again: the states from before the first solo.
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "-mmm", "solo A");
  ops::toggleChannelSolo(d, e, before);
  expect(mutes(d) == "mmm-", "solo D while A is soloed");
  ops::toggleChannelSolo(d, e, before);
  expect(mutes(d) == "--m-", "unsolo D: the states from before A's solo");

  // No memory (another session, a reload): an unsolo lets every channel play.
  before.clear();
  ops::toggleChannelSolo(d, c, before);
  expect(mutes(d) == "mm-m", "solo the muted channel C: it plays, the others are muted");
  before.clear();
  ops::toggleChannelSolo(d, c, before);
  expect(mutes(d) == "----", "unsolo without memory: every channel plays");

  // A channel the solo does not know (added while soloed) plays after the unsolo.
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  const ObjectId f = d.addChannel("E", "gl.synth");
  d.setParam(channelParam(f, ChannelParam::Mute), 1, false);
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "-----", "unsolo: a channel added during the solo plays");

  // One channel: a solo changes nothing; an unknown channel does nothing.
  Document one;
  while (!one.song().channels.empty()) one.removeChannel(one.song().channels.back().id);
  const ObjectId only = one.addChannel("Only", "gl.synth");
  std::map<ObjectId, bool> b1;
  const size_t h1 = one.history().position();
  ops::toggleChannelSolo(one, only, b1);
  expect(mutes(one) == "-", "a single channel keeps playing");
  expect(!ops::toggleChannelSolo(one, 987654, b1) && one.history().position() <= h1 + 1, "an unknown channel does nothing");
  return finish();
}
