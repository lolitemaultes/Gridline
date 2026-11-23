// Gridline: installed plugins (VST3, VST2, CLAP, AU, LV2) in a collaborative project. In-process
// collaborators (tests/collab/CollabSim.h: messages delayed and reordered in memory) with the in-tree
// CLAP test synth, and a plugin that one computer does not have:
//  * parameter edits reach the other computer both ways;
//  * a plugin's own state (one that no parameter explains: the synth counts the notes it played)
//    reaches the other computer both ways, and its instance there loads it;
//  * a collaborator who joins later gets the plugins with their states; three computers pass a
//    state on through the one in the middle;
//  * a plugin missing on one computer stays in the project with its state byte for byte, through
//    that computer's own edits of the channel; each side learns who lacks it; once installed there it
//    loads the shared state, with the parameters mapped by their ids;
//  * a big state travels as a compressed file with progress, arrives intact;
//  * conflicts: a newer full state wins over an older parameter edit, a newer edit wins over it;
//  * a state still arriving is never loaded; a collaborator's state takes the song's values;
//  * what a collaborator can't do: name a state file outside Audio/ (or not by its sha256), send a
//    state over the limit, load anything by naming a file as a plugin type, or send a plugin binary.
// No window is opened.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include "Check.h"
#include "Hermetic.h"
#include "collab/CollabSim.h"
#include "core/Params.h"
#include "host/HostedPlugins.h"
#include "host/PluginScan.h"
#include "core/Update.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

const std::string kClapBundle = GRIDLINE_CLAP_TESTPLUG;
const std::string kSynth = "clap:org.gridline.test.synth";
const std::string kGhost = "vst3:0123456789ABCDEF0123456789ABCDEF";  // installed only where a test says so

struct Installed {
  std::map<std::string, std::string> have;  // type -> version
  void wire(Session& s) {
    s.pluginInstalledFn = [this](const std::string& t, std::string& v) {
      auto it = have.find(t);
      if (it == have.end()) return false;
      v = it->second;
      return true;
    };
  }
};

struct Group {
  Clock clock;
  Net net{8601};
  Peer a, b, c;
  Installed ia, ib, ic;
  std::vector<std::string> noticesB;
  explicit Group(bool third = false, bool connectC = true) {
    a.label = "Alex", b.label = "Sam", c.label = "Kim";
    for (Peer* p : {&a, &b, &c}) p->id = crypto::Identity::generate();
    a.doc.reset(makeEmptySong());
    a.folder = tmpDir("k86_a");
    b.folder = tmpDir("k86_b");
    c.folder = tmpDir("k86_c");
    ia.have[kSynth] = ib.have[kSynth] = ic.have[kSynth] = "1.0";
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock));
    ia.wire(*a.s);
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s));
    ib.wire(*b.s);
    b.s->onNotice = [this](const std::string& t, bool) { noticesB.push_back(t); };
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    if (third) joinC(connectC);
    sync();
  }
  void joinC(bool viaA) {
    c.s = Session::join(c.doc, config(c.folder, c.id, c.label, &clock), ticketOf(*a.s));
    ic.wire(*c.s);
    a.s->admitMember(memberOf(c));
    net.add(&c);
    if (viaA) net.connect(&a, &c);
    else net.connect(&b, &c);
  }
  std::vector<Peer*> all() {
    std::vector<Peer*> v{&a, &b};
    if (c.s) v.push_back(&c);
    return v;
  }
  void sync() { settle(net, all(), clock); }
};

const Channel* chanByName(const Peer& p, const std::string& name) {
  for (const auto& c : p.doc.song().channels)
    if (c.name == name) return &c;
  return nullptr;
}

PluginState stateOf(const Peer& p, const std::string& name) {
  const Channel* c = chanByName(p, name);
  return c ? c->plugin : PluginState{};
}

std::string randomB64(size_t bytes, uint32_t seed) {
  std::mt19937 rng(seed);
  static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string raw(bytes, '\0');
  for (auto& ch : raw) ch = static_cast<char>(rng() & 0xFF);
  std::string out;
  for (size_t i = 0; i + 2 < raw.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(raw[i]) << 16) | (static_cast<uint8_t>(raw[i + 1]) << 8) | static_cast<uint8_t>(raw[i + 2]);
    out += k[(v >> 18) & 63];
    out += k[(v >> 12) & 63];
    out += k[(v >> 6) & 63];
    out += k[v & 63];
  }
  return out;
}

// The plugin object of a channel in a peer's shared document, as text.
std::string sharedPlugin(const Peer& p, ObjectId ch) {
  return jsonCompact(p.s->replica().materialize({"song", "channels", "#" + std::to_string(ch), "plugin"}));
}

// A CLAP synth instance on `st` (the engine's way: prepareHost with the song's state).
struct Inst {
  std::unique_ptr<Generator> g;
  host::HostedInstance* hi = nullptr;
  explicit Inst(const PluginState& st) {
    g = createGenerator(st.type);
    if (g) {
      hi = host::hostedInstance(g.get());
      g->prepareHost(st, 48000);
    }
  }
  void run(const PluginState& st, int frames = 1024) {
    std::vector<float> L(256), R(256);
    ProcessContext ctx;
    ctx.sampleRate = 48000;
    ctx.tempo = 120;
    ChannelAux aux;
    for (int pos = 0; pos < frames; pos += 256) g->process(st, aux, ctx, L.data(), R.data(), 256);
  }
  std::string state() {
    std::string d;
    if (hi) hi->captureState(d);
    return d;
  }
};

// ---------------------------------------------------------------- parameters and states

void testParamsAndState() {
  Group t;
  const ObjectId ch = t.a.doc.addChannel("Lead", kSynth);
  t.sync();
  expect(chanByName(t.b, "Lead") && chanByName(t.b, "Lead")->plugin.type == kSynth, "an installed plugin reaches the collaborator");
  // A turns Gain (parameter 0) as its interface would; B turns Mode (parameter 2)
  t.a.doc.setParam(channelPluginParam(ch, 0), 0.8, true);
  t.a.doc.endGesture();
  t.sync();
  expect(std::fabs(stateOf(t.b, "Lead").params[0] - 0.8f) < 1e-6f, "a parameter edit reaches the other computer");
  t.b.doc.setParam(channelPluginParam(ch, 2), 2.0, true);
  t.b.doc.endGesture();
  t.sync();
  expect(std::fabs(stateOf(t.a, "Lead").params[2] - 2.0f) < 1e-6f, "and the other way");
  // the shared record names the plugin and its parameters' ids
  const std::string shared = sharedPlugin(t.a, ch);
  expect(shared.find("\"pids\":[") != std::string::npos, "the shared identity carries the parameters' stable ids");

  // A plays notes on its instance (state that no parameter explains) and stores the state
  Inst ia(stateOf(t.a, "Lead"));
  expect(ia.g != nullptr, ("the CLAP test synth runs " + host::lastError()).c_str());
  if (!ia.g) return;
  for (int k = 0; k < 5; ++k) {
    ia.g->noteOn(k + 1, 60.0 + k, 1.0f, 0);
    ia.run(stateOf(t.a, "Lead"), 256);
  }
  const std::string dA = ia.state();
  t.a.doc.editChannel(ch, "plugin state", [&](Channel& c) { c.plugin.data = dA; });
  t.sync();
  expect(stateOf(t.b, "Lead").data == dA, "a plugin's own state reaches the other computer");
  Inst ib(stateOf(t.b, "Lead"));
  expect(ib.g && ib.state() == dA, "the collaborator's instance loads that state");
  // B plays a note there and its state goes back to A
  ib.g->noteOn(9, 64.0, 1.0f, 0);
  ib.run(stateOf(t.b, "Lead"), 256);
  const std::string dB = ib.state();
  expect(dB != dA, "(the state changed without a parameter)");
  t.b.doc.editChannel(ch, "plugin state", [&](Channel& c) { c.plugin.data = dB; });
  t.sync();
  expect(stateOf(t.a, "Lead").data == dB, "a state written on the other computer comes back");
  expect(songText(t.a) == songText(t.b), "both computers hold the same project");

  // undo of the state write (B's own step): back to A's state everywhere
  t.b.doc.undo();
  t.sync();
  expect(stateOf(t.a, "Lead").data == dA && stateOf(t.b, "Lead").data == dA, "undo takes a state write back on both");

  // a collaborator who joins later gets the plugin with its state
  t.joinC(true);
  t.sync();
  expect(t.c.s->ready() && stateOf(t.c, "Lead").data == dA && std::fabs(stateOf(t.c, "Lead").params[0] - 0.8f) < 1e-6f,
         "a late joiner gets the plugin, its parameters and its state");
}

// ---------------------------------------------------------------- the conflict rule

void testConflictRule() {
  Group t;
  const ObjectId ch = t.a.doc.addChannel("Pad", kSynth);
  t.sync();
  // apart: B turns Gain first, A writes a new state a second later (its Gain still at the default)
  t.net.disconnect(&t.a, &t.b);
  t.b.doc.setParam(channelPluginParam(ch, 0), 0.3, true);
  t.b.doc.endGesture();
  run(t.net, t.all(), t.clock, 0.2);
  t.clock.ms += 1000;
  const float gainA = stateOf(t.a, "Pad").params[0];
  t.a.doc.editChannel(ch, "plugin state", [&](Channel& c) { c.plugin.data = "clap1:AAAAAAAAAAA="; });
  run(t.net, t.all(), t.clock, 0.2);
  t.net.connect(&t.a, &t.b);
  t.sync();
  expect(std::fabs(stateOf(t.a, "Pad").params[0] - gainA) < 1e-6f && std::fabs(stateOf(t.b, "Pad").params[0] - gainA) < 1e-6f,
         "the newer full state wins over an older parameter edit");
  // an edit made after the state applies on top
  t.b.doc.setParam(channelPluginParam(ch, 0), 0.7, true);
  t.b.doc.endGesture();
  t.sync();
  expect(std::fabs(stateOf(t.a, "Pad").params[0] - 0.7f) < 1e-6f && stateOf(t.a, "Pad").data == "clap1:AAAAAAAAAAA=",
         "a parameter edit after the state applies on top of it");
  expect(songText(t.a) == songText(t.b), "both agree after the conflict");
}

// ---------------------------------------------------------------- a plugin one computer lacks

void testMissingPlugin() {
  // the plugin is registered on this process (A's build) with ids 10, 20, 30; B says it lacks it
  PluginInfo info;
  info.type = kGhost;
  info.name = "Ghost Synth";
  info.category = "Nobody Audio";
  info.kind = PluginKind::Generator;
  for (const char* n : {"Cutoff", "Drive", "Width"}) {
    ParamDesc d;
    d.name = n;
    d.max = 1;
    info.params.push_back(d);
  }
  info.hostParamIds = {10, 20, 30};
  registerDynamicPlugin(info);

  Group t;
  t.ia.have[kGhost] = "2.1";
  const ObjectId ch = t.a.doc.addChannel("Ghost", "gl.synth");
  PluginState st;
  st.type = kGhost;
  st.params = {0.1f, 0.2f, 0.3f};
  st.data = "vst3s1:" + randomB64(600, 1) + ":";  // small: inline
  st.ident = "{\"type\":\"" + kGhost + "\",\"name\":\"Ghost Synth\",\"vendor\":\"Nobody Audio\",\"version\":\"2.1\"}";
  t.a.doc.editChannel(ch, "plugin", [&](Channel& c) { c.plugin = st; });
  run(t.net, t.all(), t.clock, 3.0);  // the plugin check runs every 2 s
  t.sync();
  const std::string before = sharedPlugin(t.a, ch);
  expect(stateOf(t.b, "Ghost").type == kGhost && stateOf(t.b, "Ghost").data == st.data && stateOf(t.b, "Ghost").params == st.params,
         "a plugin missing on a computer stays there with its state");
  bool told = false;
  for (const auto& n : t.noticesB) told = told || (n.find("Ghost Synth") != std::string::npos && n.find("Nobody Audio") != std::string::npos &&
                                                   n.find("2.1") != std::string::npos && n.find("not installed") != std::string::npos);
  expect(told, "the computer without it is told which plugin, vendor and version, and to install it");
  bool lacking = false, notLackingHere = true;
  for (const PluginUse& u : t.a.s->pluginUses())
    if (u.type == kGhost) lacking = u.here && u.lacking.size() == 1 && u.lacking[0] == t.b.s->site() && u.name == "Ghost Synth";
  for (const PluginUse& u : t.b.s->pluginUses())
    if (u.type == kGhost) notLackingHere = !u.here && u.versions.count(t.a.s->site()) && u.versions.at(t.a.s->site()) == "2.1";
  expect(lacking, "the computer that has it sees who lacks it");
  expect(notLackingHere, "the one without it sees who has it, and the version");

  // B edits the channel (name, volume): the plugin's shared record stays byte for byte
  t.b.doc.editChannel(ch, "rename", [](Channel& c) { c.name = "Ghost 2"; });
  t.b.doc.setParam(channelParam(ch, ChannelParam::Volume), 0.6, false);
  t.sync();
  expect(sharedPlugin(t.a, ch) == before && sharedPlugin(t.b, ch) == before, "the missing plugin's state is never rewritten by its channel's edits");
  expect(chanByName(t.a, "Ghost 2") != nullptr, "(the channel edit itself arrived)");

  // B installs a build that lists the parameters in another order (ids 30, 10, 20) and rescans
  info.hostParamIds = {30, 10, 20};
  ParamDesc d0 = info.params[2], d1 = info.params[0], d2 = info.params[1];
  info.params = {d0, d1, d2};
  registerDynamicPlugin(info);
  t.ia.have[kGhost] = "2.2";  // (this process's registry is shared: A's build changes as well)
  t.ib.have[kGhost] = "2.2";
  run(t.net, t.all(), t.clock, 3.0);
  t.sync();
  const PluginState sb = stateOf(t.b, "Ghost 2");
  expect(sb.params.size() == 3 && std::fabs(sb.params[0] - 0.3f) < 1e-6f && std::fabs(sb.params[1] - 0.1f) < 1e-6f &&
             std::fabs(sb.params[2] - 0.2f) < 1e-6f && sb.data == st.data,
         "once installed it loads the shared state, its parameters mapped by id");
  // B turns its first parameter (id 30): the third of the shared order
  t.b.doc.setParam(channelPluginParam(ch, 0), 0.9, true);
  t.b.doc.endGesture();
  t.sync();
  const Json sp = t.a.s->replica().materialize({"song", "channels", "#" + std::to_string(ch), "plugin", "params"});
  expect(sp.is(Json::Type::Array) && sp.array.size() == 3 && std::fabs(sp.array[2].number - 0.9) < 1e-6 && std::fabs(sp.array[0].number - 0.1) < 1e-6,
         "a parameter edit lands on the same parameter id in the shared order");
  expect(songText(t.a) == songText(t.b), "both agree");
}

// ---------------------------------------------------------------- a big state, passed on

void testBigState() {
  Group t(true, false);  // C reaches A only through B
  expect(t.c.s && t.c.s->ready(), "a third collaborator joined through the second");
  const ObjectId ch = t.a.doc.addChannel("Big", "gl.synth");
  PluginState st;
  st.type = kGhost + "0";  // nobody has it
  st.params = {0.5f};
  st.data = "vst3s1:" + randomB64(3u << 20, 7) + ":" + randomB64(3000, 8);
  t.a.s->fileBudget = 256 * 1024;
  t.b.s->fileBudget = 256 * 1024;
  t.a.doc.editChannel(ch, "plugin", [&](Channel& c) { c.plugin = st; });
  const std::string shared = sharedPlugin(t.a, ch);
  bool sawProgress = false, pendingThere = false;
  for (int k = 0; k < 4000 && stateOf(t.c, "Big").data != st.data; ++k) {
    run(t.net, t.all(), t.clock, 0.01);
    const PluginState sb = stateOf(t.b, "Big");
    float p = 0;
    if (!sb.type.empty() && t.b.s->stateLoading(sb.data, &p)) {
      pendingThere = true;
      sawProgress = sawProgress || (p > 0 && p < 1);
    }
  }
  t.sync();
  expect(sharedPlugin(t.a, ch).find(kPendingStatePrefix) != std::string::npos && shared.size() < 4096,
         "a big state travels as a file reference, not inside the edit");
  expect(pendingThere && sawProgress, "it arrives as a file with progress");
  expect(stateOf(t.b, "Big").data == st.data, "it arrives intact");
  expect(stateOf(t.c, "Big").data == st.data, "the computer in the middle passes it on");
  uint64_t fileSize = 0;
  for (const auto& e : std::filesystem::directory_iterator(t.a.folder + "/Audio"))
    if (e.path().extension() == ".glstate") fileSize = std::filesystem::file_size(e.path());
  expect(fileSize > 0 && fileSize < st.data.size() * 4 / 5, "it is sent compressed");
  // the project file on disk holds the state itself (it opens as a normal project)
  t.b.s->saveNow();
  std::string text;
  readFileText(t.b.s->projectFile(), text);
  expect(text.find(st.data.substr(7, 64)) != std::string::npos && text.find(kPendingStatePrefix) == std::string::npos,
         "the project file holds the state itself");
}

// ---------------------------------------------------------------- what a collaborator can't do

bool isPluginFile(const std::filesystem::path& p) {
  static const std::set<std::string> kExt = {".clap", ".vst3", ".vst", ".component", ".lv2", ".so", ".dylib", ".dll", ".bundle"};
  return kExt.count(p.extension().string()) != 0;
}

// Every file and folder under `dir`, with its sha256 ("dir" for a folder).
// A plain file (a CLAP plugin outside macOS is one shared library, not a bundle folder) is itself.
std::map<std::string, std::string> filesUnder(const std::string& dir) {
  std::map<std::string, std::string> out;
  std::error_code ec;
  if (std::filesystem::is_regular_file(dir, ec)) {
    out[dir] = update::sha256File(dir);
    return out;
  }
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    out[it->path().string()] = it->is_regular_file(ec) ? update::sha256File(it->path().string()) : std::string("dir");
  return out;
}

void testSafety() {
  Group t;
  const std::string outside = std::filesystem::path(t.b.folder).parent_path().string();
  // (1) a state file named outside Audio/, by an absolute path, or not by its sha256: kept as the
  // collaborator wrote it, never fetched, read or written
  std::filesystem::create_directories(t.b.folder + "/Audio");
  { std::ofstream(outside + "/evil.glstate") << "GLS1 planted"; }
  { std::ofstream(t.b.folder + "/Audio/notasha.glstate") << "GLS1 planted"; }
  const std::vector<std::string> badNames = {"../evil.glstate", "../../evil.glstate", outside + "/evil.glstate", "/private/var/x.glstate",
                                             "Audio/../../evil.glstate", "Audio/notasha.glstate", "Audio/" + std::string(64, 'g') + ".glstate",
                                             "Audio/" + std::string(64, 'a') + "/../../x.glstate", "Audio\\..\\evil.glstate"};
  for (size_t i = 0; i < badNames.size(); ++i) {
    const ObjectId ch = t.a.doc.addChannel("Bad" + std::to_string(i), "gl.synth");
    PluginState st;
    st.type = kGhost + "1";
    st.params = {0.5f};
    st.data = std::string(kPendingStatePrefix) + badNames[i];
    t.a.doc.editChannel(ch, "plugin", [&](Channel& c) { c.plugin = st; });
  }
  run(t.net, t.all(), t.clock, 3.0);
  bool kept = true, fetched = false;
  for (size_t i = 0; i < badNames.size(); ++i)
    kept = kept && stateOf(t.b, "Bad" + std::to_string(i)).data == std::string(kPendingStatePrefix) + badNames[i];
  for (const FileProgress& f : t.b.s->transfers())
    for (const std::string& n : badNames) fetched = fetched || f.name == n;
  expect(kept, "a state named outside Audio/ (or not by its sha256) stays the collaborator's text, never resolved");
  expect(!fetched, "and it is never fetched");
  // (other tests share the folder around ours: only the names these states point at are looked for;
  // paths compare as paths, since Windows lists them with its own separator)
  bool onlyOwn = true;
  std::error_code wec;
  const std::filesystem::path planted0 = std::filesystem::path(outside) / "evil.glstate";
  for (auto it = std::filesystem::recursive_directory_iterator(outside, wec); !wec && it != std::filesystem::recursive_directory_iterator(); it.increment(wec)) {
    const std::string f = it->path().filename().string();
    if ((f == "evil.glstate" && it->path().lexically_normal() != planted0.lexically_normal()) || f == "x.glstate" ||
        f.find("evil.glstate.part") != std::string::npos) {
      std::printf("# written outside the project folders: %s\n", it->path().string().c_str());
      onlyOwn = false;
    }
  }
  std::string planted;
  readFileText(outside + "/evil.glstate", planted);
  expect(onlyOwn && planted == "GLS1 planted" && !std::filesystem::exists("/private/var/x.glstate") &&
             !std::filesystem::exists(std::filesystem::path(outside).parent_path() / "evil.glstate"),
         "nothing is written outside the project folders");
  std::filesystem::remove(outside + "/evil.glstate");

  // (2) states over the limit: one made here is not shared, one a collaborator sends is refused
  std::string notice;
  t.a.s->onNotice = [&](const std::string& n, bool) { notice = n; };
  const ObjectId huge = t.a.doc.addChannel("Huge", "gl.synth");
  t.a.doc.editChannel(huge, "plugin", [&](Channel& c) {
    c.plugin.type = kGhost + "1";
    c.plugin.params = {0.5f};
    c.plugin.data = "vst3s1:" + std::string(kMaxStateBytes + 16, 'A');
  });
  run(t.net, t.all(), t.clock, 1.0);
  expect(chanByName(t.b, "Huge") && stateOf(t.b, "Huge").data.size() < 1024 && notice.find("64 MB") != std::string::npos,
         "a state over 64 MB made here is not shared (a notice says so)");
  const std::string bigName = "Audio/" + std::string(63, 'b') + "c.glstate";
  {
    std::filesystem::create_directories(t.a.folder + "/Audio");
    std::ofstream f(t.a.folder + "/" + bigName, std::ios::binary);
    const std::string mb(1u << 20, '\0');
    for (size_t k = 0; k < (kMaxStateFileBytes >> 20) + 2; ++k) f << mb;
  }
  const ObjectId sent = t.a.doc.addChannel("Sent", "gl.synth");
  t.a.doc.editChannel(sent, "plugin", [&](Channel& c) {
    c.plugin.type = kGhost + "1";
    c.plugin.params = {0.5f};
    c.plugin.data = std::string(kPendingStatePrefix) + bigName;
  });
  t.a.s->fileBudget = 1u << 20;
  run(t.net, t.all(), t.clock, 3.0);
  bool refused = false;
  for (const FileProgress& f : t.b.s->transfers()) refused = refused || (f.name == bigName && f.failed);
  std::error_code ec;
  expect(refused && !std::filesystem::exists(t.b.folder + "/" + bigName, ec), "a state file over the limit from a collaborator is refused, never written");
  std::filesystem::remove(t.a.folder + "/" + bigName, ec);

  // (3) a plugin type that names a file loads nothing; only plugins installed here make instances
  const std::vector<std::string> pathTypes = {"clap:" + kClapBundle, "vst3:../../../tmp/evil.vst3", "clap:/tmp/evil.clap", kClapBundle,
                                              "lv2:file:///tmp/evil.lv2", "au:../../x", "vst2:/tmp/evil.vst"};
  for (size_t i = 0; i < pathTypes.size(); ++i) {
    const ObjectId ch = t.a.doc.addChannel("Path" + std::to_string(i), "gl.synth");
    t.a.doc.editChannel(ch, "plugin", [&](Channel& c) {
      c.plugin.type = pathTypes[i];
      c.plugin.params = {0.5f};
    });
  }
  run(t.net, t.all(), t.clock, 3.0);
  bool verbatim = true, none = true;
  for (size_t i = 0; i < pathTypes.size(); ++i) {
    const std::string ty = stateOf(t.b, "Path" + std::to_string(i)).type;
    verbatim = verbatim && ty == pathTypes[i];
    none = none && !createGenerator(ty) && !findPlugin(ty) && !host::hostedClass(ty);
  }
  expect(verbatim, "a plugin type naming a file arrives as text (never switched to another plugin)");
  expect(none, "and nothing is loaded for it: only plugins installed (scanned) here make instances");

  // (4) no plugin binary travels: nothing in either project folder is a plugin or has a plugin's bytes
  std::set<std::string> binaries;
  for (const auto& [path, h] : filesUnder(kClapBundle))
    if (h != "dir") binaries.insert(h);
  bool clean = true;
  for (const std::string& folder : {t.b.folder, t.a.folder})
    for (const auto& [path, h] : filesUnder(folder)) clean = clean && !isPluginFile(path) && !binaries.count(h);
  expect(clean && !binaries.empty(), "no plugin binary is ever sent or received (only states and parameters)");
}

// ---------------------------------------------------------------- the host side

void testHostSide() {
  PluginState st = defaultPluginState(kSynth);
  Inst x(st);
  if (!x.g) return;
  st.params[0] = 0.25f;
  x.run(st);
  const std::string d = x.state();
  // a state still arriving is never loaded: the instance keeps what it has
  PluginState pend = st;
  pend.data = std::string(kPendingStatePrefix) + "Audio/" + std::string(64, 'a') + ".glstate";
  x.g->prepareHost(pend, 48000);
  expect(x.state() == d, "a state still arriving is not loaded");
  // a collaborator's state: the song's values (edits made after it) go to the plugin
  PluginState later = st;
  later.data = d;
  later.params[0] = 0.8f;
  noteRemotePluginData(d);
  Inst y(later);
  y.run(later);
  const auto& cls = y.hi->hostedClass();
  expect(std::fabs(y.hi->backend()->paramNormalized(cls.params[0].id) - cls.toNormalized(0, 0.8)) < 1e-6,
         "a collaborator's state takes the song's parameter values");
  Inst z(later);  // not a collaborator's: the plugin's own values stay (as before)
  z.run(later);
  expect(std::fabs(z.hi->backend()->paramNormalized(cls.params[0].id) - cls.toNormalized(0, 0.25)) < 1e-6,
         "(a state loaded otherwise keeps the plugin's own values)");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("collab_plugins");
  host::ScanResult r = host::findFormat("clap")->scanInProcess(kClapBundle);
  expect(r.ok, ("CLAP test plugin scanned " + r.error).c_str());
  if (!r.ok) return finish();
  host::registerScanResult(r);
  testHostSide();
  testParamsAndState();
  testConflictRule();
  testMissingPlugin();
  testBigState();
  testSafety();
  return finish();
}
