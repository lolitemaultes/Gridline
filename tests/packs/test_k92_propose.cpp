// Gridline — K-92: the in-app AI assistant panel's side of the MCP server. At the Propose level
// a change waits as a proposal (what changes, in words) and the project stays as it was until the
// user accepts it (one undo step) or discards it; files are not written. The panel's conversation
// reaches the assistant through assistant_inbox and its answers through assistant_say.
#include <string>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Document.h"
#include "core/Mcp.h"
#include "core/PluginInfo.h"
#include "core/ProjectScript.h"
#include "core/SongDiff.h"

using namespace gl;
using namespace gl::mcp;

namespace {

struct F {
  Document doc;
  script::SimpleProjectHost host{doc};
  Environment env{doc, host};
  Session session{env};
  int id = 1;
  double t = 1000;
  F() {
    Song s = makeEmptySong();
    s.tempo = 120;
    doc.reset(s);
    env.now = [this] { return t += 0.5; };
    session.handle(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"helper"}}})");
  }
  Json call(const std::string& tool, const std::string& args) {
    const std::string req = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id++) + ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool +
                            "\",\"arguments\":" + args + "}}";
    Json out;
    parse(session.handle(req), out);
    const Json* r = out.find("result");
    return r ? *r : out;
  }
  static bool isError(const Json& r) {
    const Json* e = r.find("isError");
    return e && e->boolean;
  }
  static Json data(const Json& r) {
    const Json* s = r.find("structuredContent");
    return s ? *s : Json();
  }
};

void testDiff() {
  Song a = makeEmptySong();
  Song b = a;
  b.tempo = 128;
  Channel c;
  c.id = b.allocateId();
  c.name = "Bass";
  c.plugin = defaultPluginState("gl.synth");
  b.channels.push_back(c);
  b.patterns[0].notes[c.id] = {Note(0, 96, 40, 100), Note(96, 96, 43, 100)};
  const auto d = songDiff(a, b);
  bool tempo = false, chan = false, notes = false;
  for (const auto& l : d) {
    tempo = tempo || l.find("Tempo 140 -> 128") != std::string::npos;
    chan = chan || l.find("Channel 'Bass' added") != std::string::npos;
    notes = notes || l.find("'Bass': 2 notes added") != std::string::npos;
  }
  expect(tempo && chan && notes, "diff: tempo, a new channel and its notes in words");
  expect(songDiff(a, a).empty(), "diff: nothing for the same song");
}

void testPropose() {
  F f;
  f.env.propose = true;
  const size_t undo0 = f.doc.history().position();
  Json r = f.call("set_song", "{\"tempo\": 133}");
  expect(!F::isError(r), "propose: the call answers");
  const Json d = F::data(r);
  expect(d.num("proposed") == 1 && d.find("changes") && !d.find("changes")->array.empty(), "propose: a numbered proposal with its changes");
  expect(f.doc.song().tempo == 120 && f.doc.history().position() == undo0, "propose: the project is as it was, no undo step");
  expect(f.doc.history().size() == f.doc.history().position(), "propose: nothing to redo either");
  expect(f.env.proposals.size() == 1 && f.env.proposals[0].diff[0] == "Tempo 120 -> 133", "propose: waiting, in words");
  expect(F::data(f.call("get_proposals", "{}")).find("proposals")->array.size() == 1, "get_proposals: the assistant sees it waiting");
  // Accepted as it was measured.
  std::string err;
  expect(acceptProposal(f.env, 1, err) && f.doc.song().tempo == 133, "accept: applied");
  expect(f.doc.history().undoLabel() == "AI: change song settings" && f.env.proposals.empty(), "accept: one undo step named for the assistant");
  // A second, discarded.
  f.call("set_song", "{\"tempo\": 90}");
  expect(f.env.proposals.size() == 1 && discardProposal(f.env, 2) && f.doc.song().tempo == 133 && f.env.proposals.empty(), "discard: nothing changes");
  // A third, after the user changed the project: the call runs again on the project as it is.
  f.call("set_song", "{\"tempo\": 100}");
  f.doc.editSong("user edit", [](Song& s) { s.swing = 0.25f; });
  expect(acceptProposal(f.env, 3, err) && f.doc.song().tempo == 100 && f.doc.song().swing == 0.25f, "accept after an edit: both kept");
  expect(!acceptProposal(f.env, 99, err) && !discardProposal(f.env, 99), "an unknown proposal");
  // No files and no undo while proposing.
  expect(F::isError(f.call("undo", "{}")), "propose: undo refused");
  expect(F::isError(f.call("save_project", "{\"path\": \"x.gridline\"}")), "propose: files are not written");
  // Read only refuses; allow applies at once.
  f.env.allowEdits = false;
  expect(F::isError(f.call("set_song", "{\"tempo\": 80}")) && f.env.proposals.empty(), "read only: refused, nothing proposed");
  f.env.allowEdits = true;
  f.env.propose = false;
  f.call("set_song", "{\"tempo\": 80}");
  expect(f.doc.song().tempo == 80 && f.env.proposals.empty(), "allow: applied at once");
}

void testChat() {
  F f;
  f.env.say(true, "user", "Make the drums busier");
  Json d = F::data(f.call("assistant_inbox", "{}"));
  expect(d.find("messages") && d.find("messages")->array.size() == 1 && d.find("messages")->array[0].text("text") == "Make the drums busier",
         "inbox: the user's line");
  expect(F::data(f.call("assistant_inbox", "{}")).find("messages")->array.empty(), "inbox: read once");
  expect(F::data(f.call("assistant_inbox", "{\"all\": true}")).find("messages")->array.size() == 1, "inbox: the whole conversation on request");
  expect(F::data(f.call("assistant_inbox", "{}")).text("level") == "allow", "inbox: the permission level");
  f.call("assistant_say", "{\"text\": \"Done: 8 more hats.\"}");
  expect(f.env.chat.size() == 2 && !f.env.chat[1].fromUser && f.env.chat[1].text == "Done: 8 more hats.", "say: shown in the panel");
  f.env.allowEdits = false;
  expect(!F::isError(f.call("assistant_say", "{\"text\": \"read only still talks\"}")), "say: works read only");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k92_propose");
  testDiff();
  testPropose();
  testChat();
  return finish();
}
