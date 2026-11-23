// Gridline - K-314 B: song fields across collaboration and comparison.
//  1. Every operator== of the song model (core/Song.h, core/Types.h) compares every field of its
//     struct, or the field is one of the exceptions named below with the reason (a source check:
//     a field added later without being compared fails here).
//  2. Every Song field is either part of the collaboration's "rest" (collab/Session.cpp copyRest)
//     or one of the exceptions named below (the five lists travel element by element, flOrigin on
//     its own path, session state stays on each computer).
//  3. Two peers: every rest field changed on one reaches the other (and the author keeps it), a
//     project saved before 0.7.2 keeps its slide behaviour, its FL export choice and its workspace
//     copy through a collaborator's change, and record safe switches off (B-01, B-02).
//  4. View state stays with each person (B-04, B-05): track folding in Clips' grid, the automation
//     lane shown in Smooth, take lanes; and a track shown on this computer only (B-03).
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/LocalState.h"
#include "collab/Session.h"
#include "core/Project.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

// K-256: a workspace copy as the app writes it now: a workspace keeping its own control bar, its own pinned
// commands and a Studio dock plan (the new keys of the copy must survive a collaborator's change too).
const char* const kWorkspaceCopy =
    "{\"active\":1,\"items\":[{\"name\":\"Arrange\",\"saved\":{\"open\":[\"Playlist\"]}},"
    "{\"name\":\"Mix\",\"ownBar\":\"[{\\\"id\\\":\\\"menus\\\",\\\"on\\\":false}]\","
    "\"ownPins\":\"[{\\\"command\\\":\\\"cmd:edit.undo\\\"}]\",\"keepPanes\":false,"
    "\"saved\":{\"dock\":\"{\\\"tree\\\":{\\\"windows\\\":[\\\"Mixer\\\"],\\\"central\\\":true}}\",\"open\":[\"Mixer\"]}}]}";

std::string readText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The text without comments and string literals (so braces and names inside them do not count).
std::string stripComments(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') {
        ++i;
      }
      out += '\n';
    } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      i += 2;
      while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
        ++i;
      }
      ++i;
    } else if (s[i] == '"') {
      out += '"';
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') {
          ++i;
        }
        ++i;
      }
      out += '"';
    } else {
      out += s[i];
    }
  }
  return out;
}

bool identChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// The text between the brace that opens at `open` and its closing brace.
std::string braceBody(const std::string& s, size_t open) {
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    if (s[i] == '{') {
      ++depth;
    } else if (s[i] == '}') {
      if (--depth == 0) {
        return s.substr(open + 1, i - open - 1);
      }
    }
  }
  return std::string();
}

// The data members declared directly in a struct body (not in nested structs, not functions).
std::vector<std::string> members(const std::string& body) {
  std::vector<std::string> out;
  std::string stmt;
  int depth = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '{') {
      if (depth == 0 && stmt.find('(') != std::string::npos) {
        stmt.clear();  // a function body follows
      }
      ++depth;
      continue;
    }
    if (c == '}') {
      --depth;
      if (depth == 0) {
        // the end of a function or nested struct: what came before it was not a member
        if (stmt.find("struct") != std::string::npos || stmt.find("enum") != std::string::npos || stmt.find('(') != std::string::npos ||
            stmt.find('=') == std::string::npos) {
          stmt.clear();
        } else {
          stmt += "{}";  // a brace initializer: `std::array<LayerMacro, 3> macros{};`
        }
      }
      continue;
    }
    if (depth > 0) {
      continue;
    }
    if (c == ';') {
      std::string st = stmt;
      stmt.clear();
      while (!st.empty() && (st[0] == ' ' || st[0] == '\n' || st[0] == '\t' || st[0] == '\r')) {
        st.erase(0, 1);
      }
      if (st.empty() || st.rfind("using ", 0) == 0 || st.rfind("static ", 0) == 0 || st.rfind("friend ", 0) == 0 ||
          st.rfind("struct ", 0) == 0 || st.rfind("enum ", 0) == 0) {
        continue;
      }
      // a function declaration: a '(' before any '='
      const size_t eq = st.find('='), par = st.find('(');
      if (par != std::string::npos && (eq == std::string::npos || par < eq)) {
        continue;
      }
      // split the declarators at commas outside <> () {}
      std::vector<std::string> parts;
      std::string cur;
      int ang = 0, paren = 0;
      for (char ch : st) {
        if (ch == '<') {
          ++ang;
        } else if (ch == '>') {
          --ang;
        } else if (ch == '(' || ch == '{') {
          ++paren;
        } else if (ch == ')' || ch == '}') {
          --paren;
        }
        if (ch == ',' && ang == 0 && paren == 0) {
          parts.push_back(cur);
          cur.clear();
        } else {
          cur += ch;
        }
      }
      parts.push_back(cur);
      for (std::string p : parts) {
        const size_t e = p.find_first_of("={");
        if (e != std::string::npos) {
          p = p.substr(0, e);
        }
        while (!p.empty() && !identChar(p.back())) {
          p.pop_back();
        }
        size_t b = p.size();
        while (b > 0 && identChar(p[b - 1])) {
          --b;
        }
        const std::string name = p.substr(b);
        if (!name.empty() && !(name[0] >= '0' && name[0] <= '9')) {
          out.push_back(name);
        }
      }
      continue;
    }
    stmt += c;
  }
  return out;
}

bool mentions(const std::string& text, const std::string& prefix, const std::string& name) {
  const std::string k = prefix + name;
  for (size_t p = text.find(k); p != std::string::npos; p = text.find(k, p + 1)) {
    const size_t end = p + k.size();
    if ((end >= text.size() || !identChar(text[end])) && (p == 0 || !identChar(text[p - 1]))) {
      return true;
    }
  }
  return false;
}

// Every `bool operator==(const X& o) const { ... }` inside `struct X { ... }` of a header.
void checkEquality(const std::string& file, const std::map<std::string, std::set<std::string>>& allowed, std::set<std::string>& seen) {
  const std::string src = stripComments(readText(file));
  expect(!src.empty(), ("reads " + file).c_str());
  for (size_t p = src.find("struct "); p != std::string::npos; p = src.find("struct ", p + 1)) {
    if (p > 0 && identChar(src[p - 1])) {
      continue;
    }
    size_t n = p + 7;
    std::string name;
    while (n < src.size() && identChar(src[n])) {
      name += src[n++];
    }
    while (n < src.size() && src[n] == ' ') {
      ++n;
    }
    if (name.empty() || n >= src.size() || src[n] != '{') {
      continue;
    }
    const std::string body = braceBody(src, n);
    const std::string sig = "operator==(const " + name + "& o) const";
    const size_t op = body.find(sig);
    if (op == std::string::npos) {
      continue;
    }
    const size_t ob = body.find('{', op);
    const std::string cmp = braceBody(body, ob);
    seen.insert(name);
    const auto al = allowed.find(name);
    for (const std::string& m : members(body)) {
      const bool ok = mentions(cmp, "o.", m) || (al != allowed.end() && al->second.count(m));
      if (!ok) {
        std::printf("# %s::%s is not compared by %s::operator== (compare it, or name it as an exception in test_k314b.cpp)\n", name.c_str(),
                    m.c_str(), name.c_str());
      }
      expect(ok, (name + "::" + m + " is compared").c_str());
    }
  }
}

void testEqualityCoversFields() {
  const std::string src = GRIDLINE_SOURCE_DIR;
  // Fields an operator== leaves out on purpose (each one's reason in its declaration's comment).
  const std::map<std::string, std::set<std::string>> allowed = {
      // view and session state: no undo step, never "modified"
      {"Song", {"pluginWindows", "workspaceCopy", "idSlot", "idStride", "newTrackRoom"}},
      {"LiveSettings", {"view"}},  // read once from 0.7.7 development saves, never written
      {"SmoothView", {"library"}},
      {"EffectSlot", {"uid"}},  // the running instance's identity (Document::tagSlots), never saved
  };
  std::set<std::string> seen;
  checkEquality(src + "/src/core/Song.h", allowed, seen);
  checkEquality(src + "/src/core/Types.h", allowed, seen);
  for (const char* s : {"Song", "Channel", "MixerTrack", "PlaylistTrack", "Pattern", "Clip", "Note", "AutomationClip", "Arrangement", "StageSettings"}) {
    expect(seen.count(s) == 1, (std::string("the source check found ") + s + "::operator==").c_str());
  }
  // The check itself: a member list of a known struct.
  const std::string songH = stripComments(readText(src + "/src/core/Song.h"));
  const size_t at = songH.find("struct MixerTrack {");
  const std::vector<std::string> m = members(braceBody(songH, songH.find('{', at)));
  bool rs = false, slots = false, fn = false;
  for (const auto& x : m) {
    rs = rs || x == "recordSafe";
    slots = slots || x == "slots";
    fn = fn || x == "hasInput" || x == "records";
  }
  expect(rs && slots && !fn, "the source check reads MixerTrack's fields (recordSafe, slots; no functions)");
}

// Every Song field is part of the collaboration's rest, or travels another way, or stays here.
void testRestCoversSong() {
  const std::string src = GRIDLINE_SOURCE_DIR;
  const std::string songH = stripComments(readText(src + "/src/core/Song.h"));
  const size_t at = songH.find("struct Song {");
  expect(at != std::string::npos, "rest: finds struct Song");
  const std::vector<std::string> fields = members(braceBody(songH, songH.find('{', at)));
  const std::string sess = stripComments(readText(src + "/src/collab/Session.cpp"));
  const size_t cr = sess.find("void copyRest(Song& t, const Song& c) {");
  expect(cr != std::string::npos, "rest: finds copyRest in collab/Session.cpp");
  const std::string body = cr == std::string::npos ? std::string() : braceBody(sess, sess.find('{', cr));
  const std::map<std::string, const char*> elsewhere = {
      {"channels", "a list, element by element"},
      {"patterns", "a list, element by element"},
      {"playlist", "a list, element by element"},
      {"mixer", "a list, element by element"},
      {"automations", "a list, element by element"},
      {"flOrigin", "its own path (\"flo\")"},
      {"pluginWindows", "this computer's open windows"},
      {"idSlot", "session state"},
      {"idStride", "session state"},
      {"newTrackRoom", "session state"},
  };
  expect(fields.size() > 30, "rest: reads Song's fields");
  for (const std::string& f : fields) {
    const bool ok = mentions(body, "c.", f) || elsewhere.count(f);
    if (!ok) {
      std::printf("# Song::%s is in no collaboration path: add it to copyRest (collab/Session.cpp) or name where it goes in test_k314b.cpp\n",
                  f.c_str());
    }
    expect(ok, ("rest: Song::" + f + " reaches collaborators").c_str());
  }
}

// ------------------------------------------------------------ two peers ----
struct Pair {
  Clock clock;
  Net net;
  Peer a, b;
  Pair(uint32_t seed, const std::string& name, const Song& start) : net(seed) {
    a.label = "Alex";
    b.label = "Sam";
    a.id = crypto::Identity::generate();
    b.id = crypto::Identity::generate();
    a.doc.reset(start);
    a.folder = tmpDir(name + "_a");
    b.folder = tmpDir(name + "_b");
    std::string err;
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
    if (!a.s) {
      std::printf("# create failed: %s\n", err.c_str());
    }
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    settle();
  }
  void settle() { sim::settle(net, {&a, &b}, clock); }
  // The shared projects (what stays on each computer masked) are the same; the first difference is printed.
  bool same() const {
    Json ja = songToJson(a.doc.song()), jb = songToJson(b.doc.song());
    maskSessionJson(ja);
    maskSessionJson(jb);
    ja.object.erase("nextId");
    jb.object.erase("nextId");
    const std::string ta = jsonStringify(ja, 0), tb = jsonStringify(jb, 0);
    if (ta != tb) {
      size_t i = 0;
      while (i < ta.size() && i < tb.size() && ta[i] == tb[i]) {
        ++i;
      }
      const size_t from = i > 80 ? i - 80 : 0;
      std::printf("# differ near: %s\n#    vs: %s\n", ta.substr(from, 160).c_str(), tb.substr(from, 160).c_str());
    }
    return ta == tb;
  }
};

Song baseSong() {
  Document d;
  d.reset(makeEmptySong());
  d.addChannel("Lead", "gl.synth");
  return d.song();
}

// Every rest field changed on one peer: the other peer's project is the same, key by key, and a later
// change by the other peer (a new channel: the id counter moves, so the rest goes out again) keeps them.
void testEveryRestFieldTravels() {
  Pair p(11, "k314b_rest", baseSong());
  const ObjectId lead = p.a.doc.song().channels[0].id;
  p.a.doc.editSong("every field", [&](Song& x) {
    x.info.title = "All of it";
    x.info.comments = "notes";
    x.tempo = 97;
    x.timeSigNum = 7;
    x.timeSigDen = 8;
    x.swing = 0.25f;
    x.swingNotes = false;
    x.groove.accent = 0.5f;
    x.groove.preset = "Shuffle";
    x.slidesAnyColour = true;
    x.masterPitch = 2;
    ControllerLink l;
    l.cc = 21;
    l.owner = lead;
    x.links.push_back(l);
    Marker m;
    m.pos = 384;
    m.name = "Here";
    x.markers.push_back(m);
    x.channelGroups.push_back("Drums");
    PatternGroup g;
    g.name = "Verses";
    g.patterns.push_back(x.patterns[0].id);
    x.patternGroups.push_back(g);
    x.live.areaEnd = 4 * 384;
    {
      SceneInfo sc;
      sc.name = "Verse", sc.color = 0xFF0000;
      sc.tempo = 128, sc.sigNum = 3, sc.sigDen = 4;  // grid2-1118: a scene's own settings travel too
      sc.follow.on = true;
      x.live.scenes.push_back(sc);
    }
    x.autoCrossfades = false;
    x.panLaw = PanLaw::ConstantPower;
    x.stage.depthStrength = 0.8f;
    x.cycleStart = 0, x.cycleEnd = 768, x.cycleOn = true;
    x.projectEnd = 9600;
    x.smooth.controls.push_back(SmartControlMap{x.masterId(), 1, lead, 0});
    x.trackReceipt = "{\"v\":1}";
    x.flExportGridline = "suite";
    x.workspaceCopy["studio"] = kWorkspaceCopy;
    x.tracks[0].name = "Lead track";
    x.tracks[0].locked = true;
  });
  p.settle();
  const Song& b = p.b.doc.song();
  expect(b.slidesAnyColour && b.flExportGridline == "suite" && b.workspaceCopy.count("studio") == 1,
         "rest: slide colours, the FL export choice and the workspace copy reach the collaborator (B-01)");
  expect(!b.autoCrossfades && b.panLaw == PanLaw::ConstantPower && b.trackReceipt == "{\"v\":1}" && b.cycleOn && b.projectEnd == 9600,
         "rest: the other song settings reach the collaborator");
  expect(p.same(), "rest: both peers' projects are the same, key by key");
  {
    Json ja = songToJson(p.a.doc.song()), jb = songToJson(p.b.doc.song());
    bool keys = true;
    for (const auto& kv : ja.object) {
      if (!jb.find(kv.first)) {
        std::printf("# key %s did not reach the collaborator\n", kv.first.c_str());
        keys = false;
      }
    }
    expect(keys && ja.find("workspaces") && ja.find("flExportGridline"), "rest: every key of the author's project is in the collaborator's");
  }
  p.b.doc.addChannel("Sam's", "gl.kick");
  p.settle();
  const Song& a = p.a.doc.song();
  expect(a.slidesAnyColour && a.flExportGridline == "suite" && a.workspaceCopy.count("studio") == 1 && !a.autoCrossfades,
         "rest: the author keeps them after the collaborator's change (B-01: defaults were written over them)");
  expect(a.workspaceCopy.at("studio") == kWorkspaceCopy && p.b.doc.song().workspaceCopy.at("studio") == kWorkspaceCopy,
         "rest: a workspace's own bar, own pins and dock plan in the copy stay whole on both peers (K-256)");
  expect(p.same(), "rest: still the same after the collaborator's change");
  // and a change of only one of these fields travels by itself
  p.b.doc.editSong("export choice", [](Song& x) { x.flExportGridline = "render"; });
  p.settle();
  expect(p.a.doc.song().flExportGridline == "render", "rest: a change of the FL export choice alone travels");
  p.a.doc.editSong("slides", [](Song& x) { x.slidesAnyColour = false; });
  p.settle();
  expect(!p.b.doc.song().slidesAnyColour && p.same(), "rest: a change of the slide behaviour alone travels");
}

// A project saved before 0.7.2 (no "slidesByColour": slides over other colours glide every colour),
// hosted: its values stay, on the host and the collaborator, through the collaborator's first channel.
void testLegacyProjectKeepsValues() {
  Song s = baseSong();
  s.flExportGridline = "suite+fallback";
  s.workspaceCopy["logic"] = "{\"sets\":[1]}";
  {
    // a slide note meeting a note of another colour, saved without the key, as an older version did
    Pattern& pt = s.patterns[0];
    const ObjectId lead = s.channels[0].id;
    Note n1(0, 96, 60, 100), n2(48, 96, 64, 100);
    n1.color = 0;
    n2.color = 3;
    n2.slide = true;
    pt.notes[lead] = {n1, n2};
    Json j = songToJson(s);
    j.object.erase("slidesByColour");
    Song back;
    std::string err;
    expect(songFromJson(j, back, &err) && back.slidesAnyColour, "legacy: the older project loads with slides over every colour");
    s = back;
  }
  Pair p(12, "k314b_legacy", s);
  expect(p.b.doc.song().slidesAnyColour && p.b.doc.song().flExportGridline == "suite+fallback" && p.b.doc.song().workspaceCopy.count("logic") == 1,
         "legacy: the collaborator gets the older project's values");
  p.b.doc.addChannel("S1", "gl.synth");
  p.settle();
  const Song& a = p.a.doc.song();
  expect(a.slidesAnyColour && a.flExportGridline == "suite+fallback" && a.workspaceCopy.count("logic") == 1,
         "legacy: the host keeps them through the collaborator's first channel");
  expect(p.same(), "legacy: the peers agree");
}

// B-02: record safe on a track that is not armed, and off again; the collaborator follows.
void testRecordSafe() {
  Pair p(13, "k314b_safe", baseSong());
  const ObjectId mx = p.a.doc.song().mixer[1].id;
  MixerTrack t = *p.a.doc.song().mixerTrack(mx);
  MixerTrack u = t;
  u.recordSafe = !t.recordSafe;
  expect(!(t == u), "record safe: two tracks that differ only in it differ");
  expect(p.a.doc.editMixerTrack(mx, "record safe on", [](MixerTrack& m) { m.recordSafe = true; }) && p.a.doc.song().mixerTrack(mx)->recordSafe,
         "record safe: turns on on a track that is not armed");
  p.settle();
  expect(p.b.doc.song().mixerTrack(mx)->recordSafe, "record safe: reaches the collaborator");
  expect(p.a.doc.editMixerTrack(mx, "record safe off", [](MixerTrack& m) { m.recordSafe = false; }) && !p.a.doc.song().mixerTrack(mx)->recordSafe,
         "record safe: turns off again (B-02: the edit was dropped as no change)");
  p.settle();
  expect(!p.b.doc.song().mixerTrack(mx)->recordSafe && p.same(), "record safe: off reaches the collaborator");
}

// B-04, B-05: track view state stays with each person, through the other person's edits.
void testTrackViewsStayHere() {
  Pair p(14, "k314b_views", baseSong());
  p.a.doc.editSong("group", [](Song& x) { x.tracks[0].groupSize = 2; });
  p.settle();
  p.a.doc.editPlaylistTrack(0, "fold group", [](PlaylistTrack& t) { t.gridFolded = true; });
  p.a.doc.editPlaylistTrack(1, "show take lanes", [](PlaylistTrack& t) { t.takeLanes = true; });
  p.settle();
  p.a.doc.patchSong([](Song& x) { x.tracks[2].laneShown = true; });  // Smooth's Show automation (no parameter chosen)
  p.settle();
  const Song& b = p.b.doc.song();
  expect(!b.tracks[0].gridFolded && !b.tracks[1].takeLanes && !b.tracks[2].laneShown, "views: folding, take lanes and the lane shown stay with the person");
  p.b.doc.editPlaylistTrack(3, "rename", [](PlaylistTrack& t) { t.name = "Sam's"; });
  p.b.doc.addChannel("S1", "gl.synth");
  p.settle();
  const Song& a = p.a.doc.song();
  expect(a.tracks[3].name == "Sam's", "views: the collaborator's edit arrives");
  expect(a.tracks[0].gridFolded && a.tracks[1].takeLanes && a.tracks[2].laneShown,
         "views: they survive the collaborator's edits (B-04: the lane shown was lost on a full rebuild)");
  expect(p.same(), "views: the shared projects are the same");
  p.a.doc.undo();  // the take lanes: undone here
  p.settle();
  expect(!p.a.doc.song().tracks[1].takeLanes && p.a.doc.song().tracks[0].gridFolded, "views: undo of showing take lanes works here");
  // the lane shown with no parameter chosen is saved with the project
  Song s = p.a.doc.song();
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err) && back.tracks[2].laneShown && !back.tracks[2].laneSet, "views: the lane shown is saved without a parameter");
  // an older project's saved lane ("lane": {"shown": true}) still opens shown
  s.tracks[4].laneSet = true, s.tracks[4].laneShown = true, s.tracks[4].laneOwner = s.channels[0].id;
  expect(songFromJson(songToJson(s), back, &err) && back.tracks[4].laneShown && back.tracks[4].laneSet, "views: a chosen lane's shown state loads as before");
}

void showHere(Peer& p, int t, ObjectId ch) {
  p.doc.patchSong([&](Song& x) {
    PlaylistTrack& tr = x.tracks[static_cast<size_t>(t)];
    tr.mode = TrackMode::Instrument;
    tr.channel = ch;
    tr.name = std::to_string(t + 1) + " A1";
    tr.shownHere = true;
  });
}

// B-03: a track shown on this computer only for a collaborator's instrument: nothing of it is shared,
// the collaborator's Undo still removes the channel (and the track here goes with it), and it becomes the
// project's once this person puts a clip on it.
void testTrackShownHere() {
  Pair p(15, "k314b_here", baseSong());
  const ObjectId ch = p.a.doc.addChannel("A1", "gl.synth");
  p.settle();
  expect(p.b.doc.song().channel(ch) != nullptr, "here: the channel arrives");
  const size_t histBefore = p.b.doc.history().position();
  const int t = 5;
  showHere(p.b, t, ch);
  p.settle();
  const PlaylistTrack& at = p.a.doc.song().tracks[static_cast<size_t>(t)];
  expect(at.mode == TrackMode::Unassigned && at.name.empty(), "here: the collaborator gets no track");
  expect(p.b.doc.history().position() == histBefore && p.b.doc.song().tracks[static_cast<size_t>(t)].shownHere, "here: no undo step, the track stays here");
  expect(p.same(), "here: the shared projects are the same");
  p.a.doc.renameChannel(p.a.doc.song().channels[0].id, "Lead 2");  // a change by the collaborator: the track stays here
  p.settle();
  expect(p.b.doc.song().tracks[static_cast<size_t>(t)].shownHere && p.b.doc.song().tracks[static_cast<size_t>(t)].channel == ch,
         "here: it survives the collaborator's changes");
  p.a.doc.undo();  // the rename
  p.a.doc.undo();  // the channel
  p.settle();
  expect(!p.a.doc.song().channel(ch) && !p.b.doc.song().channel(ch), "here: the collaborator's Undo removes the channel (B-03: kept as used)");
  expect(!p.b.doc.song().tracks[static_cast<size_t>(t)].shownHere && p.b.doc.song().tracks[static_cast<size_t>(t)].mode == TrackMode::Unassigned,
         "here: the track goes with its channel");
  p.a.doc.redo();
  p.settle();
  expect(p.b.doc.song().channel(ch) != nullptr, "here: redo brings the channel back");
  showHere(p.b, t, ch);
  p.settle();
  // a track of this person's after it: the shared tracks close up and it follows them, still here only
  p.b.doc.editPlaylistTrack(t + 1, "rename", [](PlaylistTrack& x) { x.name = "Sam's"; });
  p.settle();
  const int t2 = t + 1;
  expect(p.b.doc.song().tracks[static_cast<size_t>(t)].name == "Sam's" && p.b.doc.song().tracks[static_cast<size_t>(t2)].shownHere &&
             p.b.doc.song().tracks[static_cast<size_t>(t2)].channel == ch,
         "here: a track of this person's after it takes its place; it moves after the shared tracks");
  expect(p.a.doc.song().tracks[static_cast<size_t>(t)].name == "Sam's" && p.a.doc.song().tracks[static_cast<size_t>(t2)].mode == TrackMode::Unassigned &&
             p.same(),
         "here: the collaborator gets the track of this person's (no empty track before it), not the one shown here");
  // the person uses it: a clip on it makes it the project's
  const ObjectId pat = p.b.doc.song().patterns[0].id;
  p.b.doc.addClip(pat, t2, 0);
  p.settle();
  const PlaylistTrack& at2 = p.a.doc.song().tracks[static_cast<size_t>(t2)];
  expect(!p.b.doc.song().tracks[static_cast<size_t>(t2)].shownHere && at2.channel == ch && at2.mode == TrackMode::Instrument,
         "here: a clip on it makes it the project's (shared)");
  expect(p.same(), "here: the peers agree");
  // a saved project never holds a track shown here only
  Song s = p.b.doc.song();
  s.tracks.resize(s.tracks.size() + 1);
  s.tracks.back().shownHere = true;
  s.tracks.back().name = "x";
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err) && back.tracks.size() + 1 == s.tracks.size(), "here: not saved");
}

}  // namespace

int main() {
  testEqualityCoversFields();
  testRestCoversSong();
  testEveryRestFieldTravels();
  testLegacyProjectKeepsValues();
  testRecordSafe();
  testTrackViewsStayHere();
  testTrackShownHere();
  return finish();
}
