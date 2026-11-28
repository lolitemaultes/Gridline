// Gridline — "Block internet access" for installed plugins (K-437). A test CLAP effect
// (tests/clap/NetProbe.cpp) tries a TCP connection, a UDP datagram, a connection from a forked
// child and one from a program it starts, each time it is created. They go to a listener this
// test opens on 127.0.0.1 only: no internet is used. The test checks what arrived.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Platform.h"
#include "core/PluginInfo.h"
#include "engine/Effects.h"
#include "host/HostedPlugins.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"
#include "host/sandbox/NetBlock.h"
#include "packs/Packs.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

const std::string kProbe = GRIDLINE_NETPROBE_PLUG;
const std::string kType = "clap:org.gridline.test.netprobe";

// A loopback listener: TCP and UDP on one port, recording every message.
class Listener {
 public:
  Listener() {
    tcp_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int one = 1;
    ::setsockopt(tcp_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(tcp_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(tcp_, 16) != 0) return;
    socklen_t n = sizeof a;
    ::getsockname(tcp_, reinterpret_cast<sockaddr*>(&a), &n);
    port_ = ntohs(a.sin_port);
    udp_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (::bind(udp_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) port_ = 0;
    thread_ = std::thread([this] { run(); });
  }
  ~Listener() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (tcp_ >= 0) ::close(tcp_);
    if (udp_ >= 0) ::close(udp_);
  }
  int port() const { return port_; }
  // The messages that arrived with this tag ("T", "U", "F", "S" each).
  std::set<std::string> arrived(const std::string& tag) {
    settle();
    std::lock_guard<std::mutex> g(lock_);
    std::set<std::string> out;
    for (const std::string& m : msgs_)
      if (m.size() > 2 && m.substr(2) == tag) out.insert(m.substr(0, 1));
    return out;
  }

 private:
  void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
  void run() {
    while (!stop_) {
      pollfd p[2] = {{tcp_, POLLIN, 0}, {udp_, POLLIN, 0}};
      if (::poll(p, 2, 50) <= 0) continue;
      char buf[256];
      if (p[0].revents & POLLIN) {
        const int c = ::accept(tcp_, nullptr, nullptr);
        if (c >= 0) {
          pollfd q{c, POLLIN, 0};
          std::string m;
          while (::poll(&q, 1, 500) > 0) {
            const ssize_t k = ::recv(c, buf, sizeof buf, 0);
            if (k <= 0) break;
            m.append(buf, static_cast<size_t>(k));
          }
          ::close(c);
          add(m);
        }
      }
      if (p[1].revents & POLLIN) {
        const ssize_t k = ::recv(udp_, buf, sizeof buf, 0);
        if (k > 0) add(std::string(buf, static_cast<size_t>(k)));
      }
    }
  }
  void add(const std::string& m) {
    std::lock_guard<std::mutex> g(lock_);
    msgs_.push_back(m);
  }
  int tcp_ = -1, udp_ = -1, port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex lock_;
  std::vector<std::string> msgs_;
};

std::string gOut;  // the probe's own report file

void tag(const std::string& t) { platform::setEnv("GRIDLINE_NETPROBE_TAG", t.c_str()); }
// The probe's report line for a tag ("" when it never ran).
std::string reportOf(const std::string& t) {
  std::ifstream f(gOut);
  std::string line, found;
  while (std::getline(f, line))
    if (line.rfind(t + " ", 0) == 0) found = line;
  return found;
}
const char* kAllRefused = "tcp=refused udp=refused fork=refused spawn=refused";
bool ends(const std::string& s, const char* tail) {
  const std::string t = tail;
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// Passes a sine through the effect: the plugin copies its input.
bool passesAudio(Effect& fx) {
  PluginState st = defaultPluginState(kType);
  fx.prepareHost(st, 48000);
  std::vector<float> l(256), r(256), in(256);
  for (int i = 0; i < 256; ++i) in[static_cast<size_t>(i)] = std::sin(static_cast<float>(i) * 0.05f) * 0.5f;
  l = in;
  r = in;
  ProcessContext c;
  c.sampleRate = 48000;
  c.tempo = 120;
  c.offline = true;
  fx.processAux(st, ChannelAux{}, c, l.data(), r.data(), 256);
  float err = 0, peak = 0;
  for (int i = 0; i < 256; ++i) err = std::max(err, std::fabs(l[static_cast<size_t>(i)] - in[static_cast<size_t>(i)])), peak = std::max(peak, std::fabs(l[static_cast<size_t>(i)]));
  return peak > 0.2f && err < 1e-5f;
}

std::string guardName(int g) {
  return g == host::kNetSeatbelt ? "system sandbox" : g == host::kNetNamespace ? "network space" : g == host::kNetSeccomp ? "socket filter" : "none";
}

void testPrefs(const std::string& dir) {
  const std::string cfg = dir + "/prefs";
  fs::create_directories(cfg);
  host::loadPluginPrefs(cfg);
  host::PluginPrefs p = host::pluginPrefs();
  p.netBlock = {"clap:a", "vst3:b"};
  p.netBlockNew = true;
  host::setPluginPrefs(p);
  host::loadPluginPrefs(cfg);
  const host::PluginPrefs q = host::pluginPrefs();
  expect(q.netBlock == p.netBlock && q.netBlockNew, "prefs: blocked plugins and the new plugins default are saved and read back");
  host::setNetAccess({"clap:a"}, host::NetAccess::AllowedUntilClose);
  expect(host::netAccess("clap:a") == host::NetAccess::AllowedUntilClose && !host::blocksNetwork("clap:a") && !host::runsInSandbox("clap:a"),
         "prefs: allowed until Gridline closes: not blocked, not forced into the sandbox");
  host::loadPluginPrefs(cfg);
  expect(host::pluginPrefs().netBlock.count("clap:a") == 1, "prefs: allowed until Gridline closes is not saved (still blocked in the file)");
  host::resetNetAllowedUntilClose();
  expect(host::netAccess("clap:a") == host::NetAccess::Blocked && host::blocksNetwork("clap:a") && host::runsInSandbox("clap:a"),
         "prefs: after Gridline closes it is blocked again, in the sandbox");
  host::setNetAccess({"clap:a", "vst3:b"}, host::NetAccess::Allowed);
  expect(host::pluginPrefs().netBlock.empty() && host::netAccess("vst3:b") == host::NetAccess::Allowed, "prefs: several allowed at once");
  host::blockNewlyFound({"clap:new"});
  expect(host::blocksNetwork("clap:new"), "prefs: newly found plugins blocked while that is the default");
  p = host::pluginPrefs();
  p.netBlockNew = false;
  p.netBlock.clear();
  host::setPluginPrefs(p);
  host::blockNewlyFound({"clap:new2"});
  expect(!host::blocksNetwork("clap:new2"), "prefs: newly found plugins allowed by default");
}

void testUnavailable() {
  // Systems without a way to block: the choice can be saved, but a blocked plugin never loads.
  if (host::networkBlockAvailable()) return;
  host::setNetAccess({kType}, host::NetAccess::Blocked);
  auto fx = createEffect(kType);
  host::HostedInstance* hi = fx ? host::hostedInstance(fx.get()) : nullptr;
  expect(!hi || hi->crashed(), "unsupported system: a blocked plugin does not load (fails closed)");
  host::setNetAccess({kType}, host::NetAccess::Allowed);
}

void testHosting(Listener& srv) {
  host::setChildExecutable(GRIDLINE_CLI_EXE);
  // In Gridline's own process the plugin can connect: blocking needs a process of its own.
  tag("inproc");
  host::ScanResult r = host::findFormat("clap")->scanInProcess(kProbe);
  expect(r.ok && r.classes.size() == 1, ("probe scans: " + r.error).c_str());
  host::registerScanResult(r);
  expect(srv.arrived("inproc") == std::set<std::string>{"T", "U", "F", "S"}, "in Gridline's process: the probe reaches the listener (all four)");

  if (!host::networkBlockAvailable()) {
    testUnavailable();
    return;
  }

  // The sandbox alone does not block.
  host::setRunInSandbox(kType, true);
  tag("open");
  {
    auto fx = createEffect(kType);
    host::HostedInstance* hi = fx ? host::hostedInstance(fx.get()) : nullptr;
    expect(hi && hi->sandboxed() && !hi->networkBlocked(), "sandbox without blocking starts");
    expect(srv.arrived("open") == std::set<std::string>{"T", "U", "F", "S"}, "sandbox without blocking: the probe connects");
  }
  host::setRunInSandbox(kType, false);

  // Blocked: in the sandbox, restricted, nothing arrives, audio still passes.
  host::setNetAccess({kType}, host::NetAccess::Blocked);
  expect(host::runsInSandbox(kType), "blocked: runs in the sandbox");
  tag("blocked");
  {
    auto fx = createEffect(kType);
    host::HostedInstance* hi = fx ? host::hostedInstance(fx.get()) : nullptr;
    const int guard = hi ? hi->backend()->networkGuard() : 0;
    char why[256];
    std::snprintf(why, sizeof why, "blocked: starts in the sandbox, kept off the internet (%s)", guardName(guard).c_str());
    expect(hi && hi->sandboxed() && hi->networkBlocked() && guard != host::kNetOpen && !hi->crashed(), why);
    expect(srv.arrived("blocked").empty(), "blocked: nothing reaches the listener (TCP, UDP, forked child, started program)");
    const std::string rep = reportOf("blocked");
    expect(ends(rep, kAllRefused), ("blocked: the probe ran and every attempt was refused: " + rep).c_str());
    expect(fx && passesAudio(*fx), "blocked: audio still passes through the plugin");
  }
  // Gridline itself keeps its network.
  {
    tag("self");
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(srv.port()));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::send(fd, "T:self", 6, 0) == 6;
    if (fd >= 0) ::close(fd);
    expect(ok && srv.arrived("self") == std::set<std::string>{"T"}, "blocked: Gridline's own process still connects");
  }

  // Allowed until Gridline closes, then blocked again.
  host::setNetAccess({kType}, host::NetAccess::AllowedUntilClose);
  tag("session");
  {
    auto fx = createEffect(kType);
    host::HostedInstance* hi = fx ? host::hostedInstance(fx.get()) : nullptr;
    expect(hi && !hi->networkBlocked(), "allowed until Gridline closes: starts unrestricted");
    expect(srv.arrived("session") == std::set<std::string>{"T", "U", "F", "S"}, "allowed until Gridline closes: the probe connects");
  }
  host::resetNetAllowedUntilClose();
  tag("again");
  {
    auto fx = createEffect(kType);
    host::HostedInstance* hi = fx ? host::hostedInstance(fx.get()) : nullptr;
    expect(hi && hi->networkBlocked() && srv.arrived("again").empty(), "after Gridline closes: blocked again, nothing arrives");
  }

  // A scan loads the plugin: a blocked scan cannot connect either.
  tag("scan");
  host::ScanResult iso = host::scanIsolated(GRIDLINE_CLI_EXE, "clap", kProbe, 30, nullptr, nullptr, 0, true);
  expect(iso.ok && iso.classes.size() == 1, ("blocked scan describes the plugin: " + iso.error).c_str());
  expect(srv.arrived("scan").empty() && ends(reportOf("scan"), kAllRefused), "blocked scan: nothing reaches the listener");
  tag("scanopen");
  iso = host::scanIsolated(GRIDLINE_CLI_EXE, "clap", kProbe, 30);
  expect(iso.ok && srv.arrived("scanopen").size() == 4, "unblocked scan: the probe connects");

  // Fail closed: when blocking cannot be applied, the plugin is not loaded at all.
  platform::setEnv("GRIDLINE_DEV_NET_BLOCK_FAIL", "1");
  tag("fail");
  {
    auto fx = createEffect(kType);
    host::HostedInstance* hi = fx ? host::hostedInstance(fx.get()) : nullptr;
    expect(!hi || hi->crashed(), ("blocking fails: the plugin does not load (" + host::lastError() + ")").c_str());
    expect(reportOf("fail").empty() && srv.arrived("fail").empty(), "blocking fails: the plugin's code never ran");
  }
  tag("failscan");
  iso = host::scanIsolated(GRIDLINE_CLI_EXE, "clap", kProbe, 30, nullptr, nullptr, 0, true);
  expect(!iso.ok && reportOf("failscan").empty(), ("blocking fails: the scan fails without loading the plugin: " + iso.error).c_str());
  platform::setEnv("GRIDLINE_DEV_NET_BLOCK_FAIL", "");
  host::setNetAccess({kType}, host::NetAccess::Allowed);
}

// The scan service with "new plugins: blocked": a plugin found for the first time is blocked, and
// its first scan already cannot connect.
void testNewPlugins(Listener& srv, const std::string& dir) {
  if (!host::networkBlockAvailable()) return;
  const std::string cfg = dir + "/service", folder = dir + "/service/plugins";
  fs::create_directories(folder);
  const std::string name = fs::path(kProbe).filename().string();
  fs::copy(kProbe, folder + "/" + name, fs::copy_options::recursive);
  host::initHosting(cfg);
  host::PluginPrefs p = host::pluginPrefs();
  p.disabledFormats = {"vst3", "vst2", "au", "lv2"};
  p.noDefaultFolders = {"vst3", "vst2", "clap", "lv2"};  // never the computer's installed plugins
  p.formatFolders["clap"] = {folder};
  p.netBlockNew = true;
  p.netBlock.clear();
  host::setPluginPrefs(p);
  platform::setEnv("CLAP_PATH", "");
  tag("newscan");
  host::startScan(GRIDLINE_CLI_EXE, false);
  host::stopScan();
  expect(host::blocksNetwork(kType), "new plugins blocked: the plugin found is blocked");
  const std::string rep = reportOf("newscan");
  expect(srv.arrived("newscan").empty() && ends(rep, kAllRefused), ("new plugins blocked: its first scan cannot connect: " + rep).c_str());
  // A rebuilt plugin cache (deleted, or a new cache version) does not make known plugins new: one
  // the user allowed stays allowed, while a plugin never seen before is still blocked.
  host::setNetAccess({kType}, host::NetAccess::Allowed);
  fs::remove(cfg + "/plugin-cache.json");
  host::initHosting(cfg);
  const std::string name2 = fs::path(GRIDLINE_CLAP_TESTPLUG).filename().string();
  fs::copy(GRIDLINE_CLAP_TESTPLUG, folder + "/" + name2, fs::copy_options::recursive);
  tag("rebuilt");
  host::startScan(GRIDLINE_CLI_EXE, false);
  host::stopScan();
  expect(!host::blocksNetwork(kType), "cache rebuilt: a known plugin the user allowed stays allowed");
  expect(host::blocksNetwork("clap:org.gridline.test.synth") && host::blocksNetwork("clap:org.gridline.test.gain"),
         "cache rebuilt: plugins never seen before are still blocked");
  p = host::pluginPrefs();
  p.netBlockNew = false;
  p.netBlock.clear();
  host::setPluginPrefs(p);
  tag("forced");
  host::requestScan(GRIDLINE_CLI_EXE);
  host::stopScan();
  expect(!host::blocksNetwork(kType), "new plugins allowed: an allowed plugin stays allowed");
}

}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("netblock");
  Listener srv;
  expect(srv.port() > 0, "loopback listener opens");
  if (srv.port() <= 0) return finish();
  gOut = dir + "/probe.log";
  platform::setEnv("GRIDLINE_NETPROBE_PORT", std::to_string(srv.port()).c_str());
  platform::setEnv("GRIDLINE_NETPROBE_OUT", gOut.c_str());
  platform::setEnv("GRIDLINE_NETPROBE_HELPER", GRIDLINE_NETPROBE_HELPER);
  testPrefs(dir);
  testHosting(srv);
  testNewPlugins(srv, dir);
  return finish();
}
