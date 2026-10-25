// Gridline - J-111: headless collaboration for scripts and the integration
// test (two processes on one computer, separate GRIDLINE_CONFIG_HOME each).
//
//   gridline collab-run --folder DIR --name NAME (--new | --create FILE.gridline.json | --join CODEFILE | --open)
//                       [--invite-out FILE] [--seconds N] [--edits host|guest] [--audio WAV]
//                       [--report FILE] [--stun HOST:PORT] [--app-net] [--ice-delay S] [--test-connection]
//
// --open reopens the collaborative project in DIR (a later run of the same
// person): it reconnects to the others by itself. --ice-delay holds back the
// inviter's candidates for S seconds after joining (ICE that connects late).
//
// Networking is libjuice ICE; without --stun no server is contacted (host
// candidates only, loopback included), which is what the test uses. --app-net
// uses the app's defaults instead: the public STUN server, router port mapping
// and no loopback candidates.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include "collab/Controller.h"
#include "collab/LocalState.h"
#include "collab/NetTest.h"
#include "core/Platform.h"
#include "core/Project.h"
#include "core/ProjectInterop.h"
#include "core/Update.h"

namespace fs = std::filesystem;
using namespace gl;
using namespace gl::collab;

namespace {

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

uint64_t wallMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string songHash(const Song& in) { return collab::sharedSongDigest(in); }

}  // namespace

int collabMain(int argc, char** argv) {
#ifdef SIGPIPE
  ::signal(SIGPIPE, SIG_IGN);  // a closed pipe or peer fails the write instead of ending the run (tests pipe two runs together)
#endif
  std::string folder, name = "Peer", create, joinFile, inviteOut, edits, audio, report, stun;
  bool fresh = false, netTest = false, appNet = false, reopen = false;
  double iceDelay = 0;
  double seconds = 20;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--folder") folder = next();
    else if (a == "--name") name = next();
    else if (a == "--new") fresh = true;
    else if (a == "--create") create = next();
    else if (a == "--join") joinFile = next();
    else if (a == "--invite-out") inviteOut = next();
    else if (a == "--seconds") seconds = std::atof(next().c_str());
    else if (a == "--edits") edits = next();
    else if (a == "--audio") audio = next();
    else if (a == "--report") report = next();
    else if (a == "--stun") stun = next();
    else if (a == "--app-net") appNet = true;
    else if (a == "--open") reopen = true;
    else if (a == "--ice-delay") iceDelay = std::atof(next().c_str());
    else if (a == "--test-connection") netTest = true;
    else {
      std::fprintf(stderr, "collab-run: unknown option %s\n", a.c_str());
      return 2;
    }
  }
  NetSettings net;
  net.stunHost.clear();
  net.portMapping = false;
  net.loopback = true;
  if (!stun.empty()) {
    const size_t c = stun.rfind(':');
    net.stunHost = stun.substr(0, c);
    net.stunPort = c == std::string::npos ? 3478 : static_cast<uint16_t>(std::atoi(stun.c_str() + c + 1));
    net.loopback = false;
  }
  if (appNet) {
    net.portMapping = true;
    net.loopback = false;
    if (net.stunHost.empty()) net.stunHost = "stun.l.google.com", net.stunPort = 19302;
  }
  if (netTest) {
    net.portMapping = true;
    if (net.stunHost.empty()) net.stunHost = "stun.l.google.com", net.stunPort = 19302;
    const NetReport r = testConnection(net);
    std::printf("public address: %s\nNAT: %d\nport mapping: %s %s\nIPv6: %s\n\n%s\n", r.publicAddress.c_str(), static_cast<int>(r.nat),
                r.portMapping ? "yes" : "no", r.portMappingProtocol.c_str(), r.ipv6 ? r.ipv6Address.c_str() : "no", r.summary.c_str());
    return 0;
  }
  if (folder.empty() || (!fresh && !reopen && create.empty() && joinFile.empty())) {
    std::fprintf(stderr, "collab-run: --folder and one of --new / --create / --join / --open are needed\n");
    return 2;
  }
  if (!networkBuiltIn()) {
    std::fprintf(stderr, "collab-run: this build has no collaboration networking (GRIDLINE_COLLAB=OFF)\n");
    return 3;
  }
  Controller ctl((fs::path(platform::configRoot()) / "gridline" / "collab").string());
  ctl.net = net;
  Profile p = ctl.profiles().profile();
  p.name = name;
  ctl.setProfile(p);
  ctl.onNotice = [&](const std::string& t, bool e) { std::fprintf(stderr, "%s: %s%s\n", name.c_str(), e ? "error: " : "", t.c_str()); };
  ctl.onVersionRefusal = [&](const Hub::Refusal& r) {
    std::fprintf(stderr, "%s: %s\n", name.c_str(), (r.weAreOlder ? fmtVersionOlder(r) : fmtVersionNewer(r)).c_str());
  };
  Document doc;
  std::string err;
  bool host = joinFile.empty();
  if (reopen) {
    if (!ctl.open(doc, folder, &err)) {
      std::fprintf(stderr, "collab-run: %s\n", err.c_str());
      return 1;
    }
  } else if (host) {
    Song s;
    if (!create.empty()) {
      if (!loadProject(create, s, &err)) {
        std::fprintf(stderr, "collab-run: %s\n", err.c_str());
        return 1;
      }
    } else {
      s = makeEmptySong();
    }
    if (s.channels.empty()) {
      doc.reset(s);
      doc.addChannel("Kick", "gl.kick");
      doc.addChannel("Lead", "gl.synth");
      s = doc.song();
    }
    doc.reset(s);
    if (!ctl.create(doc, folder, "Song", &err)) {
      std::fprintf(stderr, "collab-run: %s\n", err.c_str());
      return 1;
    }
  } else {
    std::string code;
    const auto t0 = std::chrono::steady_clock::now();
    while (secondsSince(t0) < 60) {
      std::ifstream f(joinFile);
      std::getline(f, code);
      if (code.find(kInvitePrefix) != std::string::npos) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    InviteInfo info;
    if (!parseInviteCode(code, info, &err)) {
      std::fprintf(stderr, "collab-run: %s\n", err.c_str());
      return 1;
    }
    ctl.testIceDelaySec = iceDelay;
    if (!ctl.join(doc, info, folder, &err)) {
      std::fprintf(stderr, "collab-run: %s\n", err.c_str());
      return 1;
    }
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::string inviteId;
  if (host && !inviteOut.empty()) inviteId = ctl.hub()->createInvite(60);
  bool inviteWritten = false, edited = false, sessionDone = false;
  double editedAt = -1;
  uint64_t opsBeforeSession = 0, opsAfterSession = 0;
  double connectedAt = -1, latencyMs = -1;
  uint64_t latencySent = 0;
  while (secondsSince(t0) < seconds) {
    const double now = secondsSince(t0);
    ctl.frame(now);
    Session* s = ctl.session();
    if (!inviteWritten && !inviteId.empty()) {
      const auto* v = ctl.hub()->invite(inviteId);
      if (v && v->status >= Hub::InviteStatus::Ready) {
        const std::string tmp = inviteOut + ".tmp";
        std::ofstream(tmp) << v->code << "\n";
        fs::rename(tmp, inviteOut);
        inviteWritten = true;
      }
    }
    bool connected = false;
    for (const auto& [site, pv] : s->peers()) connected = connected || pv.connected;
    if (connected && s->ready() && connectedAt < 0) connectedAt = now;
    if (!edited && connectedAt >= 0 && now - connectedAt > 1.0 && !edits.empty()) {
      edited = true;
      const Song& song = doc.song();
      const ObjectId pat = song.patterns.empty() ? kNoId : song.patterns[0].id;
      const ObjectId ch0 = song.channels.empty() ? kNoId : song.channels[0].id;
      if (edits == "host") {
        // notes, the mixer, a native plugin parameter, an audio file
        for (int k = 0; k < 8; ++k) doc.addNote(pat, ch0, Note(k * 48, 48, 60 + k, 90));
        const ObjectId ins = song.mixer.size() > 1 ? song.mixer[1].id : kNoId;
        doc.setParam(mixerParam(ins, MixerParam::Volume), 0.5, false);
        doc.setSlotPlugin(ins, 0, "gl.eq");
        doc.setParam(slotParam(ins, 0, 0), 0.25, false);
        if (!audio.empty()) {
          const ObjectId smp = doc.addChannel("Loop", "gl.sampler");
          doc.editChannel(smp, "sample", [&](Channel& c) { c.plugin.data = fs::absolute(audio).string(); });
        }
        latencySent = wallMs();
        doc.renameChannel(ch0, "lat:" + std::to_string(latencySent));
        editedAt = now;
      } else {
        for (int k = 0; k < 4; ++k) doc.addNote(pat, ch0, Note(k * 96 + 24, 24, 72 - k, 70));
        doc.setParam(channelParam(ch0, ChannelParam::Pan), -0.3, false);
        if (!song.patterns.empty()) doc.renamePattern(pat, "Edited by " + name);
      }
    }
    // J-111: session state (solo, record arm, input monitoring, a launched clip, a folded
    // track) stays on this computer: it must make no op
    if (host && !sessionDone && editedAt >= 0 && now - editedAt > 2.0) {
      sessionDone = true;
      ctl.frame(now);
      opsBeforeSession = s->opCount();
      const Song& song = doc.song();
      if (song.mixer.size() > 1)
        doc.editMixerTrack(song.mixer[1].id, "solo + arm", [](MixerTrack& m) {
          m.solo = true;
          m.armed = true;
          m.input = 0;
          m.monitor = 1;
        });
      if (!song.playlist.empty()) doc.editClip(song.playlist[0].id, "launch", [](Clip& c) { c.launched = true; });
      doc.editPlaylistTrack(0, "fold", [](PlaylistTrack& t) { t.folded = true; });
      ctl.frame(now);
      opsAfterSession = s->opCount();
    }
    if (latencyMs < 0 && !host)
      for (const auto& c : doc.song().channels)
        if (c.name.rfind("lat:", 0) == 0) latencyMs = static_cast<double>(wallMs()) - std::atof(c.name.c_str() + 4);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Session* s = ctl.session();
  s->saveNow();
  size_t loading = 0;
  for (const auto& t : s->transfers()) loading += t.failed ? 0 : 1;
  size_t audioFiles = 0;
  std::error_code ec;
  if (fs::exists(fs::path(folder) / "Audio", ec))
    for (const auto& e : fs::directory_iterator(fs::path(folder) / "Audio", ec)) audioFiles += e.is_regular_file() ? 1 : 0;
  // connected during the run (at the end the other process may already have quit and said bye)
  const bool connected = connectedAt >= 0;
  Json r = Json::makeObject();
  r.object["name"] = Json::makeString(name);
  r.object["ready"] = Json::makeNumber(s->ready() ? 1 : 0);
  r.object["connected"] = Json::makeNumber(connected ? 1 : 0);
  r.object["members"] = Json::makeNumber(static_cast<double>(s->members().size()));
  r.object["song"] = Json::makeString(songHash(doc.song()));
  {
    Song saved;
    std::string e2;
    r.object["file"] = Json::makeString(loadProject(s->projectFile(), saved, &e2) ? songHash(saved) : std::string("unreadable"));
  }
  r.object["loading"] = Json::makeNumber(static_cast<double>(loading));
  r.object["audioFiles"] = Json::makeNumber(static_cast<double>(audioFiles));
  r.object["latencyMs"] = Json::makeNumber(latencyMs);
  r.object["flushMs"] = Json::makeNumber(s->lastFlushMs());
  r.object["applyMs"] = Json::makeNumber(s->lastApplyMs());
  r.object["channels"] = Json::makeNumber(static_cast<double>(doc.song().channels.size()));
  r.object["sessionOps"] = Json::makeNumber(static_cast<double>(opsAfterSession - opsBeforeSession));
  bool soloSeen = false;
  for (const auto& m : doc.song().mixer) soloSeen = soloSeen || (!host && (m.solo || m.armed));
  r.object["soloSeen"] = Json::makeNumber(soloSeen ? 1 : 0);
  std::string text = jsonStringify(r, 2);
  std::printf("%s\n", text.c_str());
  if (!report.empty()) std::ofstream(report) << text << "\n";
  const bool ok = s->ready();
  ctl.close();
  return ok ? 0 : 1;
}
