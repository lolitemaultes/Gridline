// K-86 part 2 (0.7.9): Collaboration chat unit & two-site MemLink/sim sync tests.
// Covers: empty-project byte-identity, JSON persistence round-trip, two-site P2P sync
// (both simulated network with delays/partitions and real MemFabric/MemLink transport),
// deterministic ordering under concurrent offline writes, unread counts, all 8 attachment
// kinds, rename & delete resilience, undo-stack isolation, and reopen persistence.

#include "Check.h"
#include "collab/CollabSim.h"

#include "collab/Hub.h"
#include "collab/Transport.h"
#include "core/CollabChat.h"
#include "core/Document.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/Song.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace gl;
using namespace gl::collab;

namespace {

void testEmptyProjectByteIdentity() {
  Song empty = makeEmptySong();
  const std::string t1 = projectToText(empty);
  expect(t1.find("collabChat") == std::string::npos, "empty project JSON omits collabChat key");

  Song loaded;
  std::string err;
  expect(loadProjectText(t1, "empty.gridline", loaded, &err), "empty project reloads cleanly");
  expect(loaded.collabChat.empty(), "reloaded empty project has no chat messages");
  const std::string t2 = projectToText(loaded);
  expect(t1 == t2, "empty project round-trip is byte-identical");

  // Populated non-collab project with zero chat messages also omits collabChat byte-identically.
  Document d;
  d.reset(makeEmptySong());
  const ObjectId ch = d.addChannel("Lead", "gl.synth");
  const ObjectId pat = d.song().patterns.front().id;
  d.addNote(pat, ch, Note{0, 96, 60, 100});
  const std::string p1 = projectToText(d.song());
  expect(p1.find("collabChat") == std::string::npos, "non-collab project without chat omits collabChat");
  Song loaded2;
  expect(loadProjectText(p1, "song.gridline", loaded2, &err), "populated project reloads cleanly");
  expect(p1 == projectToText(loaded2), "populated non-collab project round-trip is byte-identical");
}

void testPersistenceRoundTrip() {
  Document d;
  d.reset(makeEmptySong());
  const ObjectId ch = d.addChannel("Warm Pad", "gl.synth");
  const ObjectId pat = d.song().patterns.front().id;
  d.addNote(pat, ch, Note{0, 96, 60, 100});
  d.addNote(pat, ch, Note{96, 96, 64, 95});
  const ObjectId clip = d.addClip(pat, 0, 0, 384);
  const ObjectId mx = d.song().mixer[1].id;
  d.setSlotPlugin(mx, 0, "gl.eq");
  d.editSong("marker", [](Song& s) {
    Marker mk;
    mk.pos = 384;
    mk.name = "Chorus";
    s.markers.push_back(mk);
  });

  const Song& s = d.song();
  std::vector<ChatMessage> msgs;
  auto pushMsg = [&](uint64_t hlc, uint64_t site, const std::string& author, uint32_t col, const std::string& text,
                     const ChatAttachment& att) {
    ChatMessage m;
    m.hlc = hlc;
    m.site = site;
    m.id = makeChatMessageId(hlc, site);
    m.atMs = 1700000000000ull + hlc;
    m.author = author;
    m.color = col;
    m.text = text;
    m.att = att;
    msgs.push_back(std::move(m));
  };

  pushMsg(10, 0x1111ull, "Alice", 0x3A86FF, "Starting the chorus arrangement!", {});
  pushMsg(11, 0x2222ull, "Bob", 0xFF6B35, "Check this clip", makeClipAttachment(s, clip));
  pushMsg(12, 0x1111ull, "Alice", 0x3A86FF, "Pattern check", makePatternAttachment(s, pat));
  pushMsg(13, 0x2222ull, "Bob", 0xFF6B35, "Nice pad channel", makeChannelAttachment(s, ch));
  pushMsg(14, 0x1111ull, "Alice", 0x3A86FF, "Instrument plugin", makePluginAttachment(s, ch, -1));
  pushMsg(15, 0x2222ull, "Bob", 0xFF6B35, "EQ slot", makePluginAttachment(s, mx, 0));
  pushMsg(16, 0x1111ull, "Alice", 0x3A86FF, "Volume knob", makeParamAttachment(s, channelParam(ch, ChannelParam::Volume)));
  pushMsg(17, 0x2222ull, "Bob", 0xFF6B35, "Insert 1", makeMixerTrackAttachment(s, mx));
  pushMsg(18, 0x1111ull, "Alice", 0x3A86FF, "Two notes", makeNotesAttachment(s, pat, ch, {0, 1}));
  pushMsg(19, 0x2222ull, "Bob", 0xFF6B35, "Chorus marker", makeMarkerAttachment(s, 0));

  d.setCollabChat(msgs);
  const std::string saved = projectToText(d.song());
  expect(saved.find("\"collabChat\"") != std::string::npos, "non-empty chat is written into project JSON");

  Song reloaded;
  std::string err;
  expect(loadProjectText(saved, "chat.gridline", reloaded, &err), "project with chat reloads cleanly");
  expect(reloaded.collabChat.size() == msgs.size(), "all 10 chat messages persisted and reloaded");
  for (size_t i = 0; i < msgs.size() && i < reloaded.collabChat.size(); ++i) {
    expect(reloaded.collabChat[i] == msgs[i], ("chat message " + std::to_string(i) + " round-trips identically").c_str());
  }
}

void testMemLinkTwoSiteSync() {
  const std::string root = sim::tmpDir("k86_chat_memlink");

  sim::Clock clock;
  sim::Peer alice, bob;
  alice.label = "Alice";
  bob.label = "Bob";
  alice.id = crypto::Identity::generate();
  bob.id = crypto::Identity::generate();
  alice.folder = (std::filesystem::path(root) / "alice").string();
  bob.folder = (std::filesystem::path(root) / "bob").string();

  alice.doc.reset(makeEmptySong());
  const ObjectId ch = alice.doc.addChannel("Bass", "gl.synth");
  const ObjectId pat = alice.doc.song().patterns.front().id;
  alice.doc.addNote(pat, ch, Note{0, 96, 48, 110});
  alice.doc.addClip(pat, 0, 0, 384);

  auto cfgA = sim::config(alice.folder, alice.id, "Alice", &clock);
  cfgA.profile.color = 0x3A86FF;
  auto cfgB = sim::config(bob.folder, bob.id, "Bob", &clock);
  cfgB.profile.color = 0xFF6B35;

  std::string err;
  alice.s = Session::create(alice.doc, cfgA, &err);
  expect(alice.s != nullptr, "MemLink: Alice creates session");
  bob.s = Session::join(bob.doc, cfgB, sim::ticketOf(*alice.s), &err);
  expect(bob.s != nullptr, "MemLink: Bob joins session");
  alice.s->admitMember(sim::memberOf(bob));

  // Wire Alice and Bob over an encrypted in-memory SecureLink pair!
  const crypto::Key psk = crypto::randomKey();
  SecureLink::Hs hsA, hsB;
  hsA.initiator = true;
  hsA.psk = psk;
  hsA.hsid = std::string(16, 'c');
  hsA.me = alice.id;
  hsA.remoteDh = bob.id.dhPublic;
  hsA.remoteDhKnown = true;
  hsA.hello.object["name"] = Json::makeString("Alice");

  hsB.initiator = false;
  hsB.psk = psk;
  hsB.hsid = std::string(16, 'c');
  hsB.me = bob.id;
  hsB.remoteDh = alice.id.dhPublic;
  hsB.remoteDhKnown = false;
  hsB.hello.object["name"] = Json::makeString("Bob");

  SecureLink linkA(hsA), linkB(hsB);
  std::vector<std::pair<int, std::string>> wireQ;
  linkA.output = [&](const std::string& d) { wireQ.emplace_back(1, d); };
  linkB.output = [&](const std::string& d) { wireQ.emplace_back(0, d); };

  const SiteId siteA = alice.id.site(), siteB = bob.id.site();
  alice.s->sendFn = [&](SiteId to, int stream, const std::string& w) {
    if (to == siteB) linkA.send(stream, w);
  };
  bob.s->sendFn = [&](SiteId to, int stream, const std::string& w) {
    if (to == siteA) linkB.send(stream, w);
  };
  linkA.onMessage = [&](int stream, const std::string& w) { alice.s->receive(siteB, stream, w); };
  linkB.onMessage = [&](int stream, const std::string& w) { bob.s->receive(siteA, stream, w); };
  linkA.onOpen = [&] { alice.s->peerUp(siteB); };
  linkB.onOpen = [&] { bob.s->peerUp(siteA); };

  uint64_t tMs = 1000;
  double now = 1.0;
  linkA.start(tMs);
  linkB.start(tMs);

  auto pump = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      tMs += 20;
      now += 0.02;
      clock.ms += 20;
      std::vector<std::pair<int, std::string>> batch;
      batch.swap(wireQ);
      for (const auto& [to, d] : batch) (to == 0 ? linkA : linkB).input(d, tMs);
      linkA.tick(tMs);
      linkB.tick(tMs);
      alice.s->frame(now);
      bob.s->frame(now);
    }
  };
  pump(80);
  expect(linkA.state() == SecureLink::State::Open && linkB.state() == SecureLink::State::Open,
         "MemLink: encrypted P2P links opened");
  expect(bob.s->ready(), "MemLink: Bob became ready");

  std::vector<ChatMessage> bobCb;
  bob.s->onChatMessage = [&](const ChatMessage& m) { bobCb.push_back(m); };

  const size_t undoBeforeA = alice.doc.history().size();
  const uint64_t revBeforeA = alice.doc.revision();
  const size_t undoBeforeB = bob.doc.history().size();
  const uint64_t revBeforeB = bob.doc.revision();

  expect(alice.s->sendChatMessage("Hey Bob over MemLink!", makeChannelAttachment(alice.doc.song(), ch), &err),
         "MemLink: Alice sends message with channel attachment");
  pump(50);

  expect(bob.s->chatMessages().size() == 1, "MemLink: Bob received Alice's chat message");
  expect(bob.s->unreadChat() == 1, "MemLink: Bob has 1 unread message");
  expect(bobCb.size() == 1 && bobCb[0].text == "Hey Bob over MemLink!", "MemLink: onChatMessage fired on Bob");
  expect(bobCb[0].att.kind == ChatRefKind::Channel && bobCb[0].att.id == ch, "MemLink: channel attachment synced");
  expect(bobCb[0].author == "Alice" && bobCb[0].color == 0x3A86FF, "MemLink: peer author name & colour synced");
  expect(alice.doc.history().size() == undoBeforeA && alice.doc.revision() == revBeforeA,
         "MemLink: sending chat leaves sender undo & revision untouched");
  expect(bob.doc.history().size() == undoBeforeB && bob.doc.revision() == revBeforeB,
         "MemLink: receiving chat leaves receiver undo & revision untouched");

  alice.s.reset();
  bob.s.reset();
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

void testTwoSiteSyncOrderingAndUndo() {
  const std::string root = sim::tmpDir("k86_chat_sync");
  sim::Clock clock;
  sim::Net net(42);

  sim::Peer alice, bob;
  alice.label = "Alice";
  bob.label = "Bob";
  alice.id = crypto::Identity::generate();
  bob.id = crypto::Identity::generate();
  alice.folder = (std::filesystem::path(root) / "alice").string();
  bob.folder = (std::filesystem::path(root) / "bob").string();

  alice.doc.reset(makeEmptySong());
  const ObjectId ch = alice.doc.addChannel("Bass", "gl.synth");
  const ObjectId pat = alice.doc.song().patterns.front().id;
  alice.doc.addNote(pat, ch, Note{0, 96, 48, 110});
  alice.doc.addClip(pat, 0, 0, 384);

  std::string err;
  alice.s = Session::create(alice.doc, sim::config(alice.folder, alice.id, "Alice", &clock), &err);
  bob.s = Session::join(bob.doc, sim::config(bob.folder, bob.id, "Bob", &clock), sim::ticketOf(*alice.s), &err);
  alice.s->admitMember(sim::memberOf(bob));
  net.add(&alice);
  net.add(&bob);
  net.connect(&alice, &bob);
  sim::settle(net, {&alice, &bob}, clock);
  expect(bob.s->ready(), "Alice and Bob complete initial sync");

  // Record a real edit so undo history has a known state.
  alice.doc.renameChannel(ch, "Sub Bass");
  sim::settle(net, {&alice, &bob}, clock);
  const size_t aliceUndoBefore = alice.doc.history().size();
  const size_t alicePosBefore = alice.doc.history().position();
  const uint64_t aliceRevBefore = alice.doc.revision();
  const size_t bobUndoBefore = bob.doc.history().size();
  const uint64_t bobRevBefore = bob.doc.revision();

  expect(alice.s->sendChatMessage("Hey Bob, check out the bass!", {}, &err), "Alice sends text message");
  expect(alice.s->unreadChat() == 0, "sender's own message is not unread");
  sim::settle(net, {&alice, &bob}, clock);

  expect(bob.s->chatMessages().size() == 1, "Bob received Alice's chat message");
  expect(bob.s->unreadChat() == 1, "Bob unread count incremented to 1");
  bob.s->markChatRead();
  expect(bob.s->unreadChat() == 0, "markChatRead resets unread count");

  expect(alice.doc.history().size() == aliceUndoBefore && alice.doc.history().position() == alicePosBefore,
         "sending chat does not dirty sender undo stack");
  expect(alice.doc.revision() == aliceRevBefore, "sending chat does not bump Document revision");
  expect(bob.doc.history().size() == bobUndoBefore, "receiving chat does not dirty receiver undo stack");
  expect(bob.doc.revision() == bobRevBefore, "receiving chat does not bump receiver Document revision");

  // Concurrent offline messages from both peers must converge to an identical deterministic order.
  net.disconnect(&alice, &bob);
  expect(alice.s->sendChatMessage("Alice offline 1"), "Alice offline 1");
  expect(bob.s->sendChatMessage("Bob offline 1"), "Bob offline 1");
  expect(alice.s->sendChatMessage("Alice offline 2"), "Alice offline 2");
  expect(bob.s->sendChatMessage("Bob offline 2"), "Bob offline 2");
  net.connect(&alice, &bob);
  sim::settle(net, {&alice, &bob}, clock);

  const auto& aMsgs = alice.s->chatMessages();
  const auto& bMsgs = bob.s->chatMessages();
  expect(aMsgs.size() == 5 && bMsgs.size() == 5, "both peers hold all 5 messages");
  bool exactOrder = (aMsgs.size() == bMsgs.size());
  for (size_t i = 0; i < aMsgs.size() && exactOrder; ++i) {
    if (aMsgs[i].id != bMsgs[i].id || aMsgs[i].text != bMsgs[i].text) exactOrder = false;
  }
  expect(exactOrder, "concurrent chat messages converge to identical deterministic order across peers");
  expect(alice.doc.song().collabChat.size() == 5 && bob.doc.song().collabChat.size() == 5,
         "Song::collabChat mirrors session chat on both peers");
  expect(alice.s->unreadChat() == 2, "Alice has 2 unread messages from Bob");
  expect(bob.s->unreadChat() == 2, "Bob has 2 unread messages from Alice");

  // Undo on Alice undoes the channel rename and leaves all 5 chat messages intact.
  expect(alice.doc.undo(), "Alice undoes channel rename");
  sim::settle(net, {&alice, &bob}, clock);
  expect(alice.doc.song().channel(ch)->name == "Bass", "channel rename was undone");
  expect(alice.s->chatMessages().size() == 5 && bob.s->chatMessages().size() == 5,
         "undoing song edit keeps all chat messages intact");

  // Empty messages without attachment are rejected cleanly.
  expect(!alice.s->sendChatMessage("   \n\t  ", {}, &err), "blank message without attachment is rejected");

  // Skewed peer clocks: even if a peer's clock runs ahead, causally later replies sort after
  // earlier messages and preserve monotonically non-decreasing timestamps (oldest -> newest).
  sim::Clock skewedBob = clock;
  skewedBob.ms += 120'000;  // Bob's wall clock is 2 minutes ahead of Alice's
  net.disconnect(&alice, &bob);
  bob.s->saveNow();
  bob.s.reset();
  bob.s = Session::open(bob.doc, sim::config(bob.folder, bob.id, "Bob", &skewedBob), &err);
  expect(bob.s != nullptr, "Bob reopens with skewed clock");
  net.add(&bob);
  net.connect(&alice, &bob);
  sim::settle(net, {&alice, &bob}, clock);
  expect(bob.s->sendChatMessage("Skewed first message"), "Bob sends with clock +120s ahead");
  sim::settle(net, {&alice, &bob}, clock);
  expect(alice.s->sendChatMessage("Alice causal reply"), "Alice replies with behind wall clock");
  sim::settle(net, {&alice, &bob}, clock);
  {
    const auto& msgs = alice.s->chatMessages();
    expect(msgs.size() == 7, "all 7 messages present after skewed-clock reply");
    expect(msgs.size() == 7 && msgs[5].text == "Skewed first message" && msgs[6].text == "Alice causal reply",
           "causally later reply sorts strictly after earlier skewed-clock message (oldest -> newest)");
    expect(msgs.size() == 7 && msgs[6].atMs >= msgs[5].atMs,
           "displayed reply timestamp never goes backwards behind earlier message");
  }

  alice.s.reset();
  bob.s.reset();
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

void testAttachmentsRenameDeleteAndReopen() {
  const std::string root = sim::tmpDir("k86_chat_refs");
  sim::Clock clock;
  sim::Net net(77);

  sim::Peer alice, bob;
  alice.label = "Alice";
  bob.label = "Bob";
  alice.id = crypto::Identity::generate();
  bob.id = crypto::Identity::generate();
  alice.folder = (std::filesystem::path(root) / "alice").string();
  bob.folder = (std::filesystem::path(root) / "bob").string();

  alice.doc.reset(makeEmptySong());
  const ObjectId ch = alice.doc.addChannel("Pluck", "gl.synth");
  const ObjectId pat = alice.doc.song().patterns.front().id;
  alice.doc.addNote(pat, ch, Note{0, 96, 62, 100});
  alice.doc.addNote(pat, ch, Note{96, 96, 67, 100});
  const ObjectId clip = alice.doc.addClip(pat, 0, 0, 384);
  const ObjectId mx = alice.doc.song().mixer[1].id;
  alice.doc.setSlotPlugin(mx, 0, "gl.eq");
  alice.doc.editSong("add marker", [](Song& s) {
    Marker mk;
    mk.pos = 384;
    mk.name = "Drop";
    s.markers.push_back(mk);
  });

  std::string err;
  alice.s = Session::create(alice.doc, sim::config(alice.folder, alice.id, "Alice", &clock), &err);
  bob.s = Session::join(bob.doc, sim::config(bob.folder, bob.id, "Bob", &clock), sim::ticketOf(*alice.s), &err);
  alice.s->admitMember(sim::memberOf(bob));
  net.add(&alice);
  net.add(&bob);
  net.connect(&alice, &bob);
  sim::settle(net, {&alice, &bob}, clock);
  expect(bob.s->ready(), "initial sync succeeded");

  const Song& as = alice.doc.song();
  const std::vector<ChatAttachment> atts = {
      makeClipAttachment(as, clip),
      makePatternAttachment(as, pat),
      makeChannelAttachment(as, ch),
      makePluginAttachment(as, ch, -1),
      makePluginAttachment(as, mx, 0),
      makeParamAttachment(as, channelParam(ch, ChannelParam::Volume)),
      makeMixerTrackAttachment(as, mx),
      makeNotesAttachment(as, pat, ch, {0, 1}),
      makeMarkerAttachment(as, 0),
  };
  for (size_t i = 0; i < atts.size(); ++i) {
    expect(atts[i].valid(), ("attachment " + std::to_string(i) + " built validly").c_str());
    expect(alice.s->sendChatMessage("Ref " + std::to_string(i), atts[i]),
           ("Alice posted attachment " + std::to_string(i)).c_str());
  }
  sim::settle(net, {&alice, &bob}, clock);
  expect(bob.s->chatMessages().size() == atts.size(), "Bob received all attachment messages");

  // All attachments resolve live on Bob.
  for (size_t i = 0; i < bob.s->chatMessages().size(); ++i) {
    const ChatRefResolution r = resolveChatAttachment(bob.doc.song(), bob.s->chatMessages()[i].att);
    expect(r.exists, ("attachment " + std::to_string(i) + " resolves live on Bob").c_str());
    expect(!r.title.empty(), ("attachment " + std::to_string(i) + " has non-empty live title").c_str());
  }

  // Rename targets on Alice -> live names update on Bob!
  alice.doc.renameChannel(ch, "Crystal Pluck");
  alice.doc.renamePattern(pat, "Main Hook");
  alice.doc.editSong("rename mixer track", [mx](Song& s) {
    if (MixerTrack* mt = s.mixerTrack(mx)) mt->name = "Pluck Bus";
  });
  alice.doc.editSong("rename marker", [](Song& s) {
    if (!s.markers.empty()) s.markers[0].name = "Big Drop";
  });
  sim::settle(net, {&alice, &bob}, clock);

  {
    const Song& bs = bob.doc.song();
    const auto& bm = bob.s->chatMessages();
    const ChatRefResolution rClip = resolveChatAttachment(bs, bm[0].att);
    const ChatRefResolution rPat = resolveChatAttachment(bs, bm[1].att);
    const ChatRefResolution rCh = resolveChatAttachment(bs, bm[2].att);
    const ChatRefResolution rMx = resolveChatAttachment(bs, bm[6].att);
    const ChatRefResolution rMk = resolveChatAttachment(bs, bm[8].att);
    expect(rClip.exists && rClip.title == "Main Hook", "Clip attachment tracks renamed pattern live");
    expect(rPat.exists && rPat.title == "Main Hook", "Pattern attachment tracks rename live");
    expect(rCh.exists && rCh.title == "Crystal Pluck", "Channel attachment tracks rename live");
    expect(rMx.exists && rMx.title == "Pluck Bus", "Mixer attachment tracks rename live");
    expect(rMk.exists && rMk.title == "Big Drop", "Marker attachment tracks rename live");
  }

  // Delete targets on Alice -> Bob resolves gracefully with exists == false and preserved fallback labels!
  alice.doc.removeClip(clip);
  alice.doc.setSlotPlugin(mx, 0, "");
  alice.doc.removeChannel(ch);
  alice.doc.editSong("remove marker", [](Song& s) { s.markers.clear(); });
  sim::settle(net, {&alice, &bob}, clock);

  {
    const Song& bs = bob.doc.song();
    const auto& bm = bob.s->chatMessages();
    const ChatRefResolution rClip = resolveChatAttachment(bs, bm[0].att);
    const ChatRefResolution rCh = resolveChatAttachment(bs, bm[2].att);
    const ChatRefResolution rSlot = resolveChatAttachment(bs, bm[4].att);
    const ChatRefResolution rParam = resolveChatAttachment(bs, bm[5].att);
    const ChatRefResolution rNotes = resolveChatAttachment(bs, bm[7].att);
    const ChatRefResolution rMk = resolveChatAttachment(bs, bm[8].att);
    expect(!rClip.exists && !rClip.title.empty(), "deleted clip resolves gracefully with fallback label");
    expect(!rCh.exists && rCh.title == "Pluck", "deleted channel resolves gracefully with snapshot label");
    expect(!rSlot.exists && !rSlot.title.empty(), "removed effect slot resolves gracefully");
    expect(!rParam.exists && !rParam.title.empty(), "deleted channel param resolves gracefully");
    expect(!rNotes.exists && !rNotes.title.empty(), "deleted channel notes resolve gracefully");
    expect(!rMk.exists && rMk.title == "Drop", "deleted marker resolves gracefully with snapshot label");
  }

  // Persistence across session close & reopen from disk.
  const std::string aliceFolder = alice.folder;
  const crypto::Identity aliceId = alice.id;
  alice.s->saveNow();
  alice.s.reset();
  bob.s.reset();

  Document reopenedDoc;
  auto reopened = Session::open(reopenedDoc, sim::config(aliceFolder, aliceId, "Alice", &clock), &err);
  expect(reopened != nullptr, "reopening collab project with chat succeeds");
  if (reopened) {
    expect(reopened->chatMessages().size() == atts.size(), "all chat messages survive session reopen");
    expect(reopenedDoc.song().collabChat.size() == atts.size(), "reopened Song::collabChat holds all messages");
    expect(reopened->unreadChat() == 0, "replayed chat history on reopen is not marked unread");
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

}  // namespace

int main() {
  testEmptyProjectByteIdentity();
  testPersistenceRoundTrip();
  testMemLinkTwoSiteSync();
  testTwoSiteSyncOrderingAndUndo();
  testAttachmentsRenameDeleteAndReopen();
  return finish();
}
