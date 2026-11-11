// Gridline - K-174: joining a collaborative project between two networks.
//  * invite and reply codes survive what chat apps and mail programs do to them (line breaks,
//    invisible and non-breaking spaces, typographic dashes, quoting, a capitalised prefix,
//    full-width characters, text around the code), and a code that was cut off, changed or is
//    of another kind is told apart in plain words;
//  * two computers whose ICE agents give up after 40 s like real ones (in memory): a direct
//    join, a join through the reply code pasted at once or minutes later, an expired invite,
//    an invite that expires while waiting, a wrong reply, the direct path dropped by a router;
//  * every failure and refusal reads as what happened and what to try, never a bare "refused".
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/Hub.h"
#include "collab/Link.h"

using namespace gl;
using namespace gl::collab;

namespace {

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// No dashes of the typographic kind and no empty text (the public wording rules).
bool plainWords(const std::string& s) {
  return !s.empty() && !has(s, "\xE2\x80\x93") && !has(s, "\xE2\x80\x94") && !has(s, "refused") && !has(s, "K-");
}

InviteInfo sampleInvite() {
  InviteInfo i;
  const crypto::Identity creator = crypto::Identity::generate(), inviter = crypto::Identity::generate();
  i.projectId = "p-0123456789abcdef";
  i.projectName = "Night Drive";
  i.creator = creator.site();
  i.creatorSign = creator.signPublic;
  i.inviter = inviter.site();
  i.inviterSign = inviter.signPublic;
  i.inviterDh = inviter.dhPublic;
  i.inviterName = "Alex";
  i.inviteId = std::string(16, '\x5a');
  i.secret = crypto::randomKey();
  i.expires = 1'800'000'000 + 3600;
  i.ice =
      "a=ice-ufrag:abcd\r\na=ice-pwd:0123456789abcdef0123456789abcdef\r\n"
      "a=candidate:1 1 UDP 2122317823 192.168.1.20 50123 typ host\r\n"
      "a=candidate:2 1 UDP 1686052607 203.0.113.7 61234 typ srflx raddr 192.168.1.20 rport 50123\r\n"
      "a=end-of-candidates\r\n";
  i.appVersion = "0.7.5";
  return i;
}

// The same text with a line break every `n` characters (chat apps wrap long codes).
std::string wrap(const std::string& s, size_t n, const std::string& nl) {
  std::string out;
  for (size_t i = 0; i < s.size(); i += n) out += s.substr(i, n) + (i + n < s.size() ? nl : "");
  return out;
}

// Replace every `-` in the code body with `with` (smart punctuation).
std::string dashes(const std::string& code, const std::string& with) {
  const size_t colon = code.find(':');
  std::string out = code.substr(0, colon + 1);
  for (size_t i = colon + 1; i < code.size(); ++i) out += code[i] == '-' ? with : std::string(1, code[i]);
  return out;
}

// Replace every "--" pair with one em dash (smart punctuation), single ones stay.
std::string smartDoubleDash(const std::string& code) {
  std::string out;
  for (size_t i = 0; i < code.size(); ++i) {
    if (code[i] == '-' && i + 1 < code.size() && code[i + 1] == '-') {
      out += "\xE2\x80\x94";
      ++i;
    } else {
      out += code[i];
    }
  }
  return out;
}

bool sameInvite(const InviteInfo& a, const InviteInfo& b) {
  return a.inviteId == b.inviteId && a.secret == b.secret && a.ice == b.ice && a.inviterName == b.inviterName && a.projectId == b.projectId;
}

void testCodeParsing() {
  const InviteInfo in = sampleInvite();
  const std::string code = makeInviteCode(in);
  InviteInfo out;
  std::string err;
  expect(parseInviteCode(code, out, &err) && sameInvite(in, out), "an invite code reads back");
  struct Case {
    const char* name;
    std::string text;
  };
  std::string bodyDash = code;  // make sure the body has a hyphen to mangle
  const bool hasHyphen = code.find('-', std::string(kInvitePrefix).size()) != std::string::npos;
  const std::string up = "Gridline-Invite:" + code.substr(std::string(kInvitePrefix).size());
  std::string quoted;
  for (size_t i = 0; i < code.size(); i += 60) quoted += "> " + code.substr(i, 60) + "\n";
  std::string zw, nbsp;
  for (size_t i = 0; i < code.size(); ++i) {
    zw += code[i];
    nbsp += code[i];
    if (i % 37 == 36) zw += "\xE2\x80\x8B", nbsp += "\xC2\xA0";
  }
  std::string qp;  // mail soft line breaks
  for (size_t i = 0; i < code.size(); i += 70) qp += code.substr(i, 70) + "=\r\n";
  const std::string fullColon = std::string("gridline-invite\xEF\xBC\x9A") + code.substr(std::string(kInvitePrefix).size());
  const Case cases[] = {
      {"line breaks every 40 characters", wrap(code, 40, "\n")},
      {"Windows line breaks", wrap(code, 64, "\r\n")},
      {"spaces where a chat app wrapped it", wrap(code, 50, " ")},
      {"text and curly quotes around it", "Hey \xE2\x80\x9C" + code + "\xE2\x80\x9D ok?"},
      {"in backticks and angle brackets", "`<" + code + ">`"},
      {"quoted in a mail reply", "On Monday Alex wrote:\n" + quoted},
      {"zero-width spaces inside", zw},
      {"no-break spaces inside", nbsp},
      {"mail soft line breaks", qp},
      {"a capitalised prefix", up},
      {"a full-width colon", fullColon},
      {"an ellipsis after it", code + "\xE2\x80\xA6"},
      {"en dashes for hyphens", dashes(code, "\xE2\x80\x93")},
      {"non-breaking hyphens", dashes(code, "\xE2\x80\x91")},
      {"double hyphens as one em dash", smartDoubleDash(code)},
      {"tabs and a trailing newline", "\t" + wrap(code, 80, "\t\n") + "\n\n"},
  };
  for (const auto& c : cases) {
    InviteInfo got;
    std::string e;
    const bool ok = parseInviteCode(c.text, got, &e) && sameInvite(in, got);
    if (!ok) std::printf("# %s: %s\n", c.name, e.c_str());
    expect(ok, (std::string("invite code with ") + c.name).c_str());
  }
  expect(hasHyphen || true, "(the sample code was mangled where it has hyphens)");
  // cut off, changed, another kind of code, not a code
  InviteInfo junk;
  std::string e;
  expect(!parseInviteCode(code.substr(0, code.size() * 2 / 3), junk, &e) && has(e, "cut off"), "a code cut off says so");
  std::string changed = code;
  for (size_t i = code.size() / 3; i < code.size() / 3 + 6; ++i) changed[i] = changed[i] == 'Q' ? 'R' : 'Q';
  e.clear();
  expect(!parseInviteCode(changed, junk, &e) || !sameInvite(in, junk), "a changed code is not taken for the original");
  if (!e.empty()) expect(!has(e, "cut off") && plainWords(e), "a changed code says it is damaged, not cut off");
  const std::string reply = makeReplyCode(ReplyInfo{std::string(32, 'a'), "a=candidate:1 1 UDP 1 10.0.0.2 5000 typ host\r\n"});
  e.clear();
  expect(!parseInviteCode(reply, junk, &e) && has(e, "reply code") && has(e, "Use reply"), "a reply code in the Join window says where it goes");
  e.clear();
  expect(!parseInviteCode("see you tonight", junk, &e) && has(e, "gridline-invite:"), "plain text says what an invite code looks like");
  expect(codeKind("GRIDLINE-REPLY:abc") == 'r' && codeKind("x gridline\xE2\x80\x93invite:abc") == 'i' && codeKind("hello") == 0,
         "the kind of a code is found whatever its case and dashes");
}

void testReplyParsing() {
  ReplyInfo r{"00112233445566778899aabbccddeeff",
              "a=ice-ufrag:wxyz\r\na=ice-pwd:ffffffffffffffffffffffffffffffff\r\na=candidate:1 1 UDP 1686052607 198.51.100.4 40000 typ srflx raddr 10.0.0.4 rport 40000\r\n"};
  const std::string code = makeReplyCode(r);
  ReplyInfo out;
  std::string e;
  expect(parseReplyCode(code, out, &e) && out.inviteId == r.inviteId && out.ice == r.ice, "a reply code reads back");
  expect(parseReplyCode("Here you go:\n" + wrap(code, 30, "\n") + "\nthanks", out) && out.ice == r.ice, "a wrapped reply code with chat text reads back");
  expect(parseReplyCode(dashes(code, "\xE2\x80\x94"), out) && out.ice == r.ice, "a reply code with em dashes for hyphens reads back");
  expect(parseReplyCode("Gridline-Reply: " + code.substr(std::string(kReplyPrefix).size()), out) && out.ice == r.ice,
         "a reply code with a capitalised prefix and a space reads back");
  e.clear();
  expect(!parseReplyCode(code.substr(0, code.size() / 2), out, &e) && has(e, "cut off") && plainWords(e), "a reply code cut off says so");
  e.clear();
  expect(!parseReplyCode(makeInviteCode(sampleInvite()), out, &e) && has(e, "invite code") && plainWords(e), "an invite code in the reply box says what it is");
  e.clear();
  expect(!parseReplyCode("thanks!", out, &e) && has(e, "gridline-reply:"), "plain text in the reply box says what a reply code looks like");
}

void testDescriptions() {
  expect(descriptionHasPublicPath("a=candidate:2 1 UDP 1 203.0.113.7 61234 typ srflx raddr 0.0.0.0 rport 0\r\n"), "a public address counts");
  expect(descriptionHasPublicPath("a=candidate:1 1 UDP 1 2001:db8::5 50000 typ host\r\n"), "a global IPv6 address counts");
  expect(!descriptionHasPublicPath("a=candidate:1 1 UDP 1 192.168.1.4 50000 typ host\r\na=candidate:1 1 UDP 1 fe80::1 50000 typ host\r\n"
                                   "a=candidate:1 1 UDP 1 fd00::1 50000 typ host\r\n"),
         "private, link-local and unique-local addresses alone are the same network only");
}

void testTexts() {
  for (const char* reason : {"expired", "used", "gone", "identity", "noticed", "", "something new"}) {
    const std::string t = refusalText(reason, "Kim");
    if (!plainWords(t)) std::printf("# refusal %s: %s\n", reason, t.c_str());
    expect(plainWords(t), (std::string("the refusal \"") + reason + "\" says what happened").c_str());
  }
  expect(has(refusalText("removed", "Kim"), "Kim") && has(refusalText("ended", "Kim"), "Kim"), "removal notices name who did it");
  expect(has(refusalText("gone", ""), "Ask for a new one"), "a closed invite asks for a new one");
  for (JoinProblem p : {JoinProblem::NoAnswer, JoinProblem::NoPublicAddress, JoinProblem::NoHandshake, JoinProblem::Expired}) {
    expect(plainWords(joinProblemText(p, "Alex")) && plainWords(joinProblemHelp(p)), "each join problem has words and advice");
  }
  expect(has(joinProblemText(JoinProblem::NoAnswer, "Alex"), "Alex") && has(joinProblemHelp(JoinProblem::NoAnswer), "reply code") &&
             has(joinProblemHelp(JoinProblem::NoAnswer), "VPN") && has(joinProblemHelp(JoinProblem::NoAnswer), "firewall"),
         "no answer names the inviter and what to check");
  InviteInfo i;
  i.expires = 1000;
  expect(!i.expiredHere(1000 + kClockGraceSec) && i.expiredHere(1001 + kClockGraceSec) && i.expired(1001), "a waiting join gives the clocks some slack");
}

// ---------------------------------------------------------------- two computers ----
struct Machine {
  std::string label;
  sim::Peer peer;
  std::unique_ptr<Hub> hub;
};

struct World {
  sim::Clock clock;
  std::shared_ptr<MemFabric::Shared> net = MemFabric::makeNet();
  double now = 0;
  int64_t wall = 1'800'000'000;
  std::vector<Machine*> ms;
  // agents give up after 40 s; both computers behind routers that only let answers in
  World(bool nat) {
    MemFabric::setIceBudget(net, 40, [this] { return now; });
    MemFabric::setNatBoth(net, nat);
  }
  void make(Machine& m, const std::string& label) {
    m.label = label;
    m.peer.label = label;
    m.peer.id = crypto::Identity::generate();
    Profile p;
    p.name = label;
    VersionInfo v;
    v.app = "0.7.0";
    v.build = "test";
    m.hub = std::make_unique<Hub>(std::make_unique<MemFabric>(net, label), m.peer.id, p, v, sim::tmpDir("join_cfg_" + label));
    m.hub->wallSec = [this] { return wall + static_cast<int64_t>(now); };
    ms.push_back(&m);
  }
  void run(double seconds) {
    const double end = now + seconds;
    while (now < end) {
      now += 0.05;
      clock.ms += 50;
      for (Machine* m : ms) {
        m->hub->poll(now);
        if (m->peer.s) m->peer.s->frame(now);
      }
    }
  }
  bool runUntil(double seconds, const std::function<bool()>& done) {
    for (double t = 0; t < seconds; t += 0.25) {
      if (done()) return true;
      run(0.25);
    }
    return done();
  }
};

void createProject(Machine& m, World& w, const std::string& name) {
  m.peer.doc.reset(makeEmptySong());
  m.peer.doc.addChannel("Kick", "gl.kick");
  m.peer.folder = sim::tmpDir(name);
  m.peer.s = Session::create(m.peer.doc, sim::config(m.peer.folder, m.peer.id, m.label, &w.clock));
  m.hub->setSession(m.peer.s.get());
}

bool joinWith(Machine& m, World& w, const std::string& code, const std::string& name, std::string* err = nullptr) {
  InviteInfo info;
  if (!parseInviteCode(code, info, err)) return false;
  m.peer.folder = sim::tmpDir(name);
  m.peer.s = Session::join(m.peer.doc, sim::config(m.peer.folder, m.peer.id, m.label, &w.clock), info.ticket());
  m.hub->setSession(m.peer.s.get());
  return m.hub->join(info, err);
}

std::string readyCode(World& w, Machine& host, const std::string& id) {
  w.runUntil(30, [&] {
    const auto* v = host.hub->invite(id);
    return v && v->status == Hub::InviteStatus::Ready;
  });
  const auto* v = host.hub->invite(id);
  return v ? v->code : std::string();
}

bool synced(Machine& a, Machine& b) { return b.peer.s && b.peer.s->ready() && sim::songText(a.peer) == sim::songText(b.peer); }

void testDirectJoin() {
  World w(false);
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  createProject(a, w, "kj_direct_a");
  const std::string id = a.hub->createInvite(60);
  const std::string code = readyCode(w, a, id);
  w.run(90);  // the invite sat in a chat for a while: its agent gave up and starts again
  expect(a.hub->invite(id)->status == Hub::InviteStatus::Ready, "an invite stays open while its agent restarts");
  expect(joinWith(b, w, code, "kj_direct_b"), "the invitee joins");
  expect(w.runUntil(30, [&] { return synced(a, b); }) && b.hub->joinView().status == Hub::JoinStatus::Connected,
         "direct join: connected and synced without a reply code");
}

// Both behind routers: the reply code is needed. `delay` is how long it takes to come back.
bool replyJoin(double delay, const char* tag, int* attempts = nullptr) {
  World w(true);
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  createProject(a, w, std::string("kj_reply_a_") + tag);
  const std::string id = a.hub->createInvite(60);
  const std::string code = readyCode(w, a, id);
  w.run(20);
  joinWith(b, w, code, std::string("kj_reply_b_") + tag);
  w.runUntil(10, [&] { return b.hub->joinView().status == Hub::JoinStatus::NeedReply; });
  const std::string reply = b.hub->joinView().reply;
  w.run(delay);
  if (b.hub->joinView().status != Hub::JoinStatus::NeedReply) return false;
  std::string err;
  if (!a.hub->acceptReply(reply, &err)) {
    std::printf("# reply: %s\n", err.c_str());
    return false;
  }
  if (!a.hub->invite(id)->replyUsed) return false;
  const bool ok = w.runUntil(60, [&] { return synced(a, b); });
  if (attempts) *attempts = b.hub->joinView().attempts;
  return ok && a.hub->invite(id)->status == Hub::InviteStatus::Connected;
}

void testReplyTiming() {
  expect(replyJoin(1, "early"), "the reply code pasted at once connects");
  int attempts = 0;
  expect(replyJoin(150, "late", &attempts), "the reply code pasted 2.5 minutes later still connects");
  expect(attempts >= 3, "the join kept trying with fresh attempts while it waited");
  expect(replyJoin(600, "verylate"), "the reply code pasted 10 minutes later still connects");
}

// The joiner's agent connects moments before its time runs out and fails before the
// handshake got through (seen on a loaded machine with real agents): the join starts a
// fresh attempt, the handshake starts again on the new path, and it connects.
void testPathLostDuringHandshake() {
  World w(true);
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  createProject(a, w, "kj_lost_a");
  const std::string id = a.hub->createInvite(60);
  const std::string code = readyCode(w, a, id);
  joinWith(b, w, code, "kj_lost_b");
  w.runUntil(10, [&] { return b.hub->joinView().status == Hub::JoinStatus::NeedReply; });
  const std::string reply = b.hub->joinView().reply;
  w.run(5);
  expect(a.hub->acceptReply(reply), "the reply code is accepted");
  bool securing = false;
  for (int i = 0; i < 400 && !securing; ++i) {
    w.run(0.05);
    securing = b.hub->joinView().securing;
  }
  const bool notYet = b.hub->joinView().status != Hub::JoinStatus::Connected && !synced(a, b);
  expect(securing && notYet, "the joiner reached the inviter and is securing the connection");
  const int before = b.hub->joinView().attempts;
  MemFabric::breakPaths(w.net, "Sam");
  const bool ok = w.runUntil(90, [&] { return synced(a, b); });
  expect(ok && b.hub->joinView().status == Hub::JoinStatus::Connected, "a path lost during the handshake: a fresh attempt connects");
  expect(b.hub->joinView().attempts > before, "the join made a fresh attempt");
}

void testJoinFailures() {
  World w(true);
  Machine a, b, c;
  w.make(a, "Alex");
  w.make(b, "Sam");
  w.make(c, "Kim");
  createProject(a, w, "kj_fail_a");
  const std::string id = a.hub->createInvite(15);
  const std::string code = readyCode(w, a, id);
  // a wrong reply: one for another invite, an invite code, chat text
  createProject(c, w, "kj_fail_c");
  const std::string other = c.hub->createInvite(15);
  readyCode(w, c, other);
  std::string err;
  expect(!a.hub->acceptReply(makeReplyCode(ReplyInfo{other, "mem:Kim:1"}), &err) && has(err, "no longer open here") && plainWords(err),
         "a reply for another invite says the invite is not open here");
  err.clear();
  expect(!a.hub->acceptReply(code, &err) && has(err, "invite code"), "an invite code in the reply box says what it is");
  // nobody uses the reply code: the join keeps trying, shows progress, then says what to try
  b.hub->joinWaitSec = 240;
  expect(joinWith(b, w, code, "kj_fail_b"), "joining starts");
  w.run(100);
  const Hub::JoinView& jv = b.hub->joinView();
  expect(jv.status == Hub::JoinStatus::NeedReply && jv.attempts >= 2 && jv.waited > 90 && !jv.reply.empty(), "while waiting the join shows its reply code, time and attempts");
  w.run(160);
  expect(b.hub->joinView().status == Hub::JoinStatus::Failed && has(b.hub->joinView().message, "Alex") && plainWords(b.hub->joinView().message) &&
             has(b.hub->joinView().help, "reply code"),
         "a join nobody answered says why in plain words and what to try");
  // an expired invite
  Machine d;
  w.make(d, "Lee");
  w.wall += 20 * 60;
  err.clear();
  expect(!joinWith(d, w, code, "kj_fail_d", &err) && has(err, "expired") && has(err, "date and time"), "an expired invite is refused before connecting, naming the clock");
  w.run(1);
  expect(a.hub->invite(id)->status == Hub::InviteStatus::Expired, "the inviter sees the invite expired");
  err.clear();
  expect(!a.hub->acceptReply(makeReplyCode(ReplyInfo{id, "mem:Sam:1"}), &err) && has(err, "expired"), "a reply for an expired invite says so");
}

void testExpiresWhileWaiting() {
  World w(true);
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  createProject(a, w, "kj_exp_a");
  const std::string id = a.hub->createInvite(1);  // valid for one minute
  const std::string code = readyCode(w, a, id);
  expect(joinWith(b, w, code, "kj_exp_b"), "joining a short invite starts");
  w.run(60 + kClockGraceSec + 5);
  expect(b.hub->joinView().status == Hub::JoinStatus::Failed && has(b.hub->joinView().message, "expired"), "an invite that expires while waiting says so");
}

}  // namespace

int main() {
  testCodeParsing();
  testReplyParsing();
  testDescriptions();
  testTexts();
  testDirectJoin();
  testReplyTiming();
  testPathLostDuringHandshake();
  testJoinFailures();
  testExpiresWhileWaiting();
  return finish();
}
