// Gridline: OSC in the app (see OscUi.h).
#include "OscUi.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/OscControl.h"
#include "../core/OscServer.h"
#include "App.h"
#include "DevSignal.h"
#include "DialogLayout.h"
#include "MenuParityUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::oscui {

using theme::pal;
using i18n::tr;

namespace {

// The address space's view of the app.
struct Host final : OscHost {
  explicit Host(App& app) : a(app) {}
  App& a;
  double lastEdit = -1;

  Document& doc() override { return a.doc; }
  bool playing() const override { return a.engine.playing(); }
  void play() override {
    if (!a.engine.playing()) menuparity::playPressed(a);
  }
  void pause() override { a.engine.pause(); }
  void stop() override { menuparity::stopPressed(a); }
  bool recording() const override { return a.recordArmed; }
  void setRecording(bool on) override {
    if (a.recordArmed == on) return;
    a.recordArmed = on;
    a.notify(on ? tr("Recording armed") : tr("Recording off"));
  }
  bool songMode() const override { return a.engine.mode() == PlayMode::Song; }
  void setSongMode(bool song) override {
    const PlayMode m = song ? PlayMode::Song : PlayMode::Pattern;
    if (a.engine.mode() != m) a.engine.setMode(m);
  }
  double position() const override { return a.engine.position(); }
  void setPosition(double ticks) override { a.engine.setPosition(static_cast<Tick>(std::llround(std::max(0.0, ticks)))); }
  void setLoop(Tick start, Tick end) override {
    if (end <= start) start = end = 0;
    a.playlist.selStart = start;
    a.playlist.selEnd = end;
    a.engine.setLoopRange(start, end);
  }
  ObjectId currentPattern() const override { return a.curPattern; }
  void selectPattern(ObjectId p) override { a.selectPattern(p); }
  ObjectId selectedChannel() const override { return a.selChannel; }
  void note(ObjectId ch, int pitch, int velocity) override {
    if (velocity > 0) {
      a.engine.noteOn(ch, pitch, static_cast<float>(velocity) / 127.0f);
      a.playedNoteOn(ch, pitch, velocity);
    } else {
      a.engine.noteOff(ch, pitch);
      a.playedNoteOff(ch, pitch);
    }
  }
  void action(RemoteAction act) override { a.runAction(act); }
  bool learn(const std::string& address) override {
    // The remote control dialog listening for a control takes the address.
    if (a.showRemoteDialog && a.remoteListening) {
      a.remoteEdit.source = RemoteSource::Osc;
      a.remoteEdit.oscAddress = address;
      a.remoteListening = false;
      return true;
    }
    // Multilink: an address not linked yet links to the next control touched in Gridline.
    if (a.multilink && !a.multilinkQueue.empty()) {
      for (const auto& l : a.doc.song().links)
        if (l.source == RemoteSource::Osc && l.oscAddress == address) return false;
      const ParamId t = a.multilinkQueue.front();
      a.multilinkQueue.erase(a.multilinkQueue.begin());
      if (!describeParam(a.doc.song(), t)) return true;
      ControllerLink l;
      l.source = RemoteSource::Osc;
      l.oscAddress = address;
      l.owner = t.owner;
      l.index = t.index;
      a.doc.editSong("link controller", [&](Song& s) {
        s.links.erase(std::remove_if(s.links.begin(), s.links.end(),
                                     [&](const ControllerLink& x) { return x.action == RemoteAction::None && x.owner == t.owner && x.index == t.index; }),
                      s.links.end());
        s.links.push_back(l);
      });
      a.notify("Linked " + remoteSourceLabel(l) + " to " + paramDisplayName(a.doc.song(), t));
      return true;
    }
    return false;
  }
  void edited() override { lastEdit = a.now; }
};

struct Seen {
  std::string text, result, from;
  double time = 0;
};

struct State {
  std::unique_ptr<OscServer> server;
  std::unique_ptr<Host> host;
  std::unique_ptr<OscControl> control;
  OscServerConfig cfg;
  std::string error;
  double retryAt = 0;
  std::vector<OscIncoming> pending;  // bundles timed for later
  std::deque<Seen> seen;             // activity monitor, newest first
  uint64_t count = 0;
  uint32_t lastIp = 0;
  uint16_t lastPort = 0;
  std::string feedbackKey, feedbackError;
  uint64_t feedbackSent = 0;
  double pingAt = -1;
  bool pingSeen = false;
  bool trace = std::getenv("GRIDLINE_OSC_TRACE") != nullptr;
};
State S;

constexpr size_t kMaxPending = 1024;
constexpr size_t kSeenRows = 8;

double unixNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

OscServerConfig configOf(const Settings& st) {
  OscServerConfig c;
  c.port = std::clamp(static_cast<int>(st.getNumber("osc.port")), 0, 65535);
  c.network = st.getBool("osc.network");
  c.tcp = st.getBool("osc.tcp");
  c.allowedHosts = st.getString("osc.allowedHosts");
  return c;
}

bool sameConfig(const OscServerConfig& x, const OscServerConfig& y) {
  return x.port == y.port && x.network == y.network && x.tcp == y.tcp && (x.allowedHosts == y.allowedHosts || !x.network);
}

void stopServer() {
  if (S.control) S.control->releaseNotes();
  if (S.server && S.server->running()) {
    S.server->stop();
    logging::info("osc", "OSC server stopped");
  }
  S.pending.clear();
}

void dispatch(App& a, const OscIncoming& in) {
  S.lastIp = in.ip;
  S.lastPort = in.port;
  const OscControl::Result r = S.control->handle(in.msg);
  if (in.msg.address == "/gridline/ping") S.pingSeen = true;
  Seen s;
  s.text = osc::describe(in.msg, 96);
  s.result = OscControl::resultName(r);
  s.from = OscServer::ipText(in.ip) + ":" + std::to_string(in.port) + (in.tcp ? " TCP" : "");
  s.time = a.now;
  S.seen.push_front(std::move(s));
  if (S.seen.size() > kSeenRows) S.seen.pop_back();
  ++S.count;
  devsig::reach("osc-received:" + std::to_string(S.count));
  if (S.trace) {
    std::printf("osc: %s -> %s\n", osc::describe(in.msg).c_str(), OscControl::resultName(r));
    std::fflush(stdout);
  }
}

// Where feedback goes now: the configured host and port, or the last sender (port 0).
bool feedbackTarget(App& a, uint32_t& ip, int& port) {
  port = static_cast<int>(a.settings.getNumber("osc.feedbackPort"));
  if (port <= 0) {
    if (S.lastPort == 0) return false;
    ip = S.lastIp;
    port = S.lastPort;
    return true;
  }
  if (port > 65535) return false;
  return OscServer::parseIPv4(a.settings.getString("osc.feedbackHost"), ip);
}

void label(const char* text) { ui::FormLabelFit(tr(text), 168.0f, pal().textDim); }

}  // namespace

bool listening() { return S.server && S.server->running(); }

void tick(App& a) {
  if (!a.settings.getBool("osc.enabled")) {
    if (S.server && S.server->running()) stopServer();
    S.error.clear();
    return;
  }
  if (!S.server) {
    S.server = std::make_unique<OscServer>();
    S.host = std::make_unique<Host>(a);
    S.control = std::make_unique<OscControl>(*S.host);
  }
  const OscServerConfig want = configOf(a.settings);
  if (S.server->running() && !sameConfig(want, S.cfg)) {
    stopServer();
    S.retryAt = 0;
  }
  if (!S.server->running()) {
    if (a.now < S.retryAt) return;
    std::string err;
    if (!S.server->start(want, &err)) {
      if (err != S.error) logging::error("osc", err);
      S.error = err;
      S.retryAt = a.now + 5.0;
      return;
    }
    S.cfg = want;
    S.error.clear();
    logging::info("osc", std::string("OSC server on ") + (want.network ? "all interfaces" : "127.0.0.1") + ", port " + std::to_string(S.server->port()));
    devsig::reach("osc-listening");
    if (S.trace) {
      std::printf("osc: listening %d\n", S.server->port());
      std::fflush(stdout);
    }
  }
  // An editor holding an undo group open (a drag, a live preview) keeps messages waiting, so
  // nothing lands inside the user's own step.
  if (a.doc.groupDepth() == 0) {
    std::vector<OscIncoming> in;
    S.server->poll(in);
    const double now = unixNow();
    for (auto& m : in) {
      if (m.time > osc::kImmediately && osc::unixFromTime(m.time) > now + 0.002 && S.pending.size() < kMaxPending)
        S.pending.push_back(std::move(m));
      else
        dispatch(a, m);
    }
    for (size_t k = 0; k < S.pending.size();) {
      if (osc::unixFromTime(S.pending[k].time) <= now + 0.002) {
        const OscIncoming m = std::move(S.pending[k]);
        S.pending.erase(S.pending.begin() + static_cast<std::ptrdiff_t>(k));
        dispatch(a, m);
      } else {
        ++k;
      }
    }
  }
  // A controller gesture ends after a short pause: one undo step per move.
  if (S.host->lastEdit >= 0 && a.now - S.host->lastEdit > 0.4) {
    a.doc.endGesture();
    S.host->lastEdit = -1;
  }
  // Feedback.
  if (!a.settings.getBool("osc.feedback")) return;
  uint32_t ip = 0;
  int port = 0;
  if (!feedbackTarget(a, ip, port)) return;
  const std::string key = OscServer::ipText(ip) + ":" + std::to_string(port);
  if (key != S.feedbackKey) {
    S.feedbackKey = key;
    S.control->resetFeedback();
  }
  std::vector<osc::Message> out;
  S.control->feedback(a.now, out);
  for (const auto& m : out) {
    std::string err;
    if (S.server->sendTo(ip, port, osc::encode(m), &err)) {
      ++S.feedbackSent;
      S.feedbackError.clear();
    } else {
      S.feedbackError = err;
    }
  }
}

void shutdown() {
  stopServer();
  S.control.reset();
  S.host.reset();
  S.server.reset();
}

void settingsSection(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection("OSC (OPEN SOUND CONTROL)");
  label("OSC");
  bool on = st.getBool("osc.enabled");
  if (ImGui::Checkbox(tr("Receive OSC messages"), &on)) st.set("osc.enabled", on);
  ui::Hint("Touch surfaces, other music software and lighting tools can drive the transport, the mixer, channels and any plugin "
           "parameter. See docs/osc.md for the addresses");
  ImGui::BeginDisabled(!on);
  label("Port");
  {
    int port = static_cast<int>(st.getNumber("osc.port"));
    ImGui::SetNextItemWidth(std::min(fieldW, 140.0f));
    if (ImGui::InputInt("##oscport", &port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) st.set("osc.port", std::clamp(port, 0, 65535));
    ui::Hint("The port Gridline listens on (UDP, and TCP when switched on below). 0 picks any free port");
  }
  label("Network");
  bool net = st.getBool("osc.network");
  if (ImGui::Checkbox(tr("Accept messages from other devices on the network"), &net)) st.set("osc.network", net);
  ui::Hint("Off: only programs on this computer can send (127.0.0.1). On: phones, tablets and other computers can too");
  label("Allowed hosts");
  {
    static char buf[512];
    static bool init = false;
    if (!init) std::snprintf(buf, sizeof buf, "%s", st.getString("osc.allowedHosts").c_str()), init = true;
    ImGui::BeginDisabled(!net);
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputTextWithHint("##oschosts", tr("Any host (for example 192.168.1.20, 10.0.0.0/8)"), buf, sizeof buf))
      st.set("osc.allowedHosts", std::string(buf));
    ui::FocusRing();
    ui::Hint("Addresses or ranges allowed to send, separated by commas. Empty: any host on the network. This computer is always allowed");
    ImGui::EndDisabled();
    std::string bad;
    if (net && !OscServer::validHostList(buf, &bad)) {
      label("");
      ImGui::TextColored(theme::vec(P.danger), "%s %s", tr("Not an address or range:"), bad.c_str());
    }
  }
  label("TCP");
  bool tcp = st.getBool("osc.tcp");
  if (ImGui::Checkbox(tr("Also accept OSC over TCP on the same port"), &tcp)) st.set("osc.tcp", tcp);
  ui::Hint("For senders that use TCP: packets framed with SLIP or with a length in front, detected per connection");
  label("Status");
  {
    std::string s;
    ImU32 col = P.textDim;
    const bool running = listening();
    if (!on) {
      s = tr("Off");
    } else if (running) {
      char b[200];
      std::snprintf(b, sizeof b, tr("Listening on %s port %d (%s)"), S.cfg.network ? tr("all networks,") : "127.0.0.1", S.server->port(),
                    S.cfg.tcp ? "UDP, TCP" : "UDP");
      s = b;
      col = P.text;
    } else if (!S.error.empty()) {
      s = S.error;
      col = P.danger;
    } else {
      s = tr("Starting...");
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ui::Led(ImGui::GetWindowDrawList(), ImVec2(p.x + 7, p.y + ImGui::GetFrameHeight() * 0.5f), 4.5f, running);
    ImGui::Dummy(ImVec2(16, ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ui::TextFit(s.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x - 90), col);
    ImGui::SameLine();
    if (ui::TextButton(tr("Test"), false, 70, "Sends a test message to Gridline's own port; it shows below when it arrives")) {
      std::string err;
      osc::Message ping;
      ping.address = "/gridline/ping";
      S.pingSeen = false;
      S.pingAt = a.now;
      if (!running || !S.server->sendTo(0x7F000001u, S.server->port(), osc::encode(ping), &err)) {
        S.pingAt = -1;
        a.notify(running ? std::string(tr("OSC test failed: ")) + err : tr("The OSC server is not running"), true);
      }
    }
  }
  if (S.pingAt >= 0) {
    label("");
    if (S.pingSeen)
      ImGui::TextColored(theme::vec(P.text), "%s", tr("Test message received: OSC works."));
    else if (a.now - S.pingAt > 2.0)
      ImGui::TextColored(theme::vec(P.danger), "%s", tr("The test message did not arrive. A firewall may block the port."));
    else
      ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Waiting for the test message..."));
  }
  if (listening()) {
    const OscServer::Status ss = S.server->status();
    label("Received");
    ImGui::TextColored(theme::vec(P.textDim), tr("%llu messages, %llu malformed, %llu refused, %llu dropped"),
                       static_cast<unsigned long long>(ss.received), static_cast<unsigned long long>(ss.malformed),
                       static_cast<unsigned long long>(ss.refused), static_cast<unsigned long long>(ss.dropped));
    if (!ss.lastProblem.empty()) {
      label("");
      ui::TextFit(ss.lastProblem.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.danger);
    }
  }
  label("Activity");
  if (S.seen.empty()) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Nothing received yet."));
  } else if (ImGui::BeginTable("##oscseen", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit,
                               ImVec2(std::max(200.0f, ImGui::GetContentRegionAvail().x), 0))) {
    ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Result"), ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn(tr("From"), ImGuiTableColumnFlags_WidthFixed, 150);
    for (const auto& s : S.seen) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushFont(a.fontMono, theme::kFontNumber);
      ui::TextFit(s.text.c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x), P.text);
      ImGui::PopFont();
      ImGui::TableNextColumn();
      const bool good = s.result != std::string("unknown address") && s.result != std::string("bad arguments");
      ImGui::TextColored(theme::vec(good ? P.textDim : P.danger), "%s", tr(s.result.c_str()));
      ImGui::TableNextColumn();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", s.from.c_str());
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();
  ui::EndSection();

  ui::BeginSection("OSC FEEDBACK");
  ImGui::BeginDisabled(!on);
  label("Feedback");
  bool fb = st.getBool("osc.feedback");
  if (ImGui::Checkbox(tr("Send the transport and linked values back"), &fb)) st.set("osc.feedback", fb);
  ui::Hint("Keeps a touch surface in sync: play and record state, tempo, the position while it moves, and the values of OSC links "
           "and of addresses the surface has sent, when they change");
  ImGui::BeginDisabled(!fb);
  label("Send to host");
  {
    static char host[64];
    static bool init = false;
    if (!init) std::snprintf(host, sizeof host, "%s", st.getString("osc.feedbackHost").c_str()), init = true;
    ImGui::SetNextItemWidth(std::min(fieldW, 200.0f));
    if (ImGui::InputTextWithHint("##oscfbhost", "127.0.0.1", host, sizeof host)) st.set("osc.feedbackHost", std::string(host));
    ui::FocusRing();
    ui::Hint("The IPv4 address of the device that shows the feedback");
    uint32_t ip = 0;
    if (!OscServer::parseIPv4(host, ip) && st.getNumber("osc.feedbackPort") > 0) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.danger), "%s", tr("Not an IPv4 address"));
    }
  }
  label("Port");
  {
    int port = static_cast<int>(st.getNumber("osc.feedbackPort"));
    ImGui::SetNextItemWidth(std::min(fieldW, 140.0f));
    if (ImGui::InputInt("##oscfbport", &port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) st.set("osc.feedbackPort", std::clamp(port, 0, 65535));
    ui::Hint("The port the device listens on. 0: answer the address and port the last message came from");
  }
  if (fb && listening()) {
    label("");
    if (!S.feedbackError.empty())
      ImGui::TextColored(theme::vec(P.danger), "%s %s", tr("Feedback failed:"), S.feedbackError.c_str());
    else if (S.feedbackKey.empty())
      ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Waiting for a first message to answer."));
    else
      ImGui::TextColored(theme::vec(P.textDim), tr("%llu messages sent to %s"), static_cast<unsigned long long>(S.feedbackSent), S.feedbackKey.c_str());
  }
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textFaint), "%s",
                     tr("To link a control: right-click a knob or fader, choose \"Link to controller\xE2\x80\xA6\", then move the control on the surface."));
  ImGui::PopTextWrapPos();
  ui::EndSection();
}

}  // namespace gl::oscui
