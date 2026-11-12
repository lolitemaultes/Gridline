// Gridline: K-126 / K-186, the channel rack's solo (right-click on a channel's headphones button or light).
//  * a solo leaves the channel playing and mutes every other one, in one undo step;
//  * the same solo again restores the mute states from before it (a channel that was muted stays
//    muted, the others play again), in one undo step;
//  * soloing another channel while one is soloed keeps the states from before the first solo;
//  * undo and redo of a solo and an unsolo; a single channel; an unknown channel does nothing;
//  * K-186: a solo is in effect only while the soloed channel plays and every channel it muted is
//    still muted (a hand change ends it, a right-click then solos anew); a channel added or deleted
//    during a solo; the soloed channel muted or deleted; an undone solo is no solo; channelSoloed.
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

// Sets the channels' mute states from "m-m..." (by hand, one undo step each).
void setMutes(Document& d, const std::string& m) {
  for (size_t i = 0; i < m.size() && i < d.song().channels.size(); ++i)
    d.setParam(channelParam(d.song().channels[i].id, ChannelParam::Mute), m[i] == 'm' ? 1 : 0, false);
}

}  // namespace

int main() {
  Document d;
  while (!d.song().channels.empty()) d.removeChannel(d.song().channels.back().id);
  const ObjectId a = d.addChannel("A", "gl.synth"), b = d.addChannel("B", "gl.synth"), c = d.addChannel("C", "gl.synth"),
                 e = d.addChannel("D", "gl.synth");
  d.setParam(channelParam(c, ChannelParam::Mute), 1, false);  // C was muted by hand
  expect(mutes(d) == "--m-", "start: C muted by hand");
  ops::ChannelSolo before;

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

  // A channel the solo does not know (added while soloed) is left as it is by the unsolo.
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  const ObjectId f = d.addChannel("E", "gl.synth");
  expect(mutes(d) == "-mmm-", "a channel added during the solo plays");
  expect(ops::channelSoloed(d.song(), a, before), "A still counts as soloed with the new channel playing");
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "-----", "unsolo: a channel added during the solo plays");
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  const ObjectId h2 = d.addChannel("Late", "gl.synth");
  d.setParam(channelParam(h2, ChannelParam::Mute), 1, false);  // added during the solo, then muted by hand
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "-----m", "unsolo: a channel added during the solo keeps the state it was given");
  d.removeChannel(h2);

  // K-186: a solo is in effect only while the soloed channel plays and every other channel it
  // muted is still muted. Unmuting one by hand ends it: the ring goes, the memory is dropped and a
  // right-click on the formerly soloed channel starts a new solo (the hand changes stand).
  setMutes(d, "--m--");
  before.clear();
  ops::toggleChannelSolo(d, b, before);
  expect(mutes(d) == "m-mmm", "K-186: solo B");
  expect(ops::channelSoloed(d.song(), b, before) && !ops::channelSoloed(d.song(), a, before), "channelSoloed: B, not A");
  d.setParam(channelParam(e, ChannelParam::Mute), 0, false);  // D unmuted by hand during the solo
  expect(!ops::channelSoloed(d.song(), b, before) && !ops::channelSoloed(d.song(), e, before),
         "D unmuted by hand: nothing shows as soloed any more");
  expect(ops::toggleChannelSolo(d, b, before) && mutes(d) == "m-mmm", "a right-click on B then solos it again");
  expect(!ops::toggleChannelSolo(d, b, before) && mutes(d) == "m-m-m", "and again: the states from that new solo (D on, the others muted)");
  setMutes(d, "--m--");
  before.clear();
  ops::toggleChannelSolo(d, b, before);
  d.setParam(channelParam(c, ChannelParam::Mute), 0, false);  // C (muted before the solo) unmuted by hand
  expect(!ops::channelSoloed(d.song(), b, before), "C unmuted by hand: B is no longer soloed");
  setMutes(d, "--m--");

  // A right-click moves the solo; the later "again" restores the states from before the first.
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  ops::toggleChannelSolo(d, b, before);
  expect(mutes(d) == "m-mmm" && ops::channelSoloed(d.song(), b, before) && !ops::channelSoloed(d.song(), a, before),
         "the solo moved from A to B");
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "-mmmm", "and back to A");
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "--m--", "again on A: the states from before the first solo");

  // The soloed channel muted by hand ends the solo: a right-click on it solos it anew.
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  d.setParam(channelParam(a, ChannelParam::Mute), 1, false);
  expect(!ops::channelSoloed(d.song(), a, before), "a muted channel is not shown as soloed");
  expect(ops::toggleChannelSolo(d, a, before) && mutes(d) == "-mmmm", "right-click on the muted channel: a new solo");
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "mmmmm", "then again: the states from that new solo (all muted)");
  setMutes(d, "--m--");

  // A channel deleted during a solo: the solo stays in effect and the rest comes back.
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  d.removeChannel(f);
  expect(ops::channelSoloed(d.song(), a, before), "a channel deleted during the solo: A still soloed");
  ops::toggleChannelSolo(d, a, before);
  expect(mutes(d) == "--m-", "a channel deleted during the solo: the others come back");
  // The soloed channel deleted: no solo any more.
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  ops::toggleChannelSolo(d, b, before);  // moved to B
  const ObjectId g = d.addChannel("F", "gl.synth");
  d.setParam(channelParam(g, ChannelParam::Mute), 1, false);
  d.removeChannel(b);  // the soloed channel is gone: A, C, D, F muted
  expect(mutes(d) == "mmmm", "the soloed channel deleted: nothing plays");
  expect(!ops::channelSoloed(d.song(), e, before) && !ops::channelSoloed(d.song(), a, before), "no channel shows as soloed");
  ops::toggleChannelSolo(d, e, before);
  expect(mutes(d) == "mm-m", "a right-click on D: a new solo");
  ops::toggleChannelSolo(d, e, before);
  expect(mutes(d) == "mmmm", "and again: the states from that new solo");
  d.removeChannel(g);
  setMutes(d, "-m-");

  // An undone solo is no solo: a right-click then solos again (it does not 'restore' a stale memory).
  before.clear();
  ops::toggleChannelSolo(d, a, before);
  d.undo();
  expect(mutes(d) == "-m-", "undo of the solo restores the states");
  expect(!ops::channelSoloed(d.song(), a, before), "after the undo A is not soloed");
  expect(ops::toggleChannelSolo(d, a, before) && mutes(d) == "-mm", "a right-click after the undo solos A again");
  expect(!ops::toggleChannelSolo(d, a, before) && mutes(d) == "-m-", "and again: back to normal");

  // One channel: a solo changes nothing; an unknown channel does nothing.
  Document one;
  while (!one.song().channels.empty()) one.removeChannel(one.song().channels.back().id);
  const ObjectId only = one.addChannel("Only", "gl.synth");
  ops::ChannelSolo b1;
  const size_t h1 = one.history().position();
  ops::toggleChannelSolo(one, only, b1);
  expect(mutes(one) == "-", "a single channel keeps playing");
  expect(!ops::toggleChannelSolo(one, 987654, b1) && one.history().position() <= h1 + 1, "an unknown channel does nothing");
  return finish();
}
