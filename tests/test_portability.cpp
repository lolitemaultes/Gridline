// Gridline tests — J-86: the operating-system layer (core/Platform, core/Net, host/Dylib) on
// every system the CI builds: folders, environment, child processes, locale-independent numbers,
// loopback sockets, shared objects, and the project file format under a comma-decimal locale.
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include "Check.h"
#include "Hermetic.h"
#include "core/Json.h"
#include "core/Net.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "host/Dylib.h"
#include "host/KeyForward.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void testFolders() {
  expect(platform::homeEnv() != nullptr && fs::is_directory(fs::u8path(platform::homeDir())), "home folder known and present");
  platform::setEnv("GRIDLINE_CONFIG_HOME", "");
  platform::unsetEnv("GRIDLINE_CONFIG_HOME");
  platform::setEnv("XDG_CONFIG_HOME", "/tmp/xdg-test");
  expect(platform::configRoot() == "/tmp/xdg-test", "config root follows XDG_CONFIG_HOME");
  platform::setEnv("GRIDLINE_CONFIG_HOME", "/tmp/gl-test");
  expect(platform::configRoot() == "/tmp/gl-test", "GRIDLINE_CONFIG_HOME wins");
  expect(Settings::defaultPath() == (fs::path("/tmp/gl-test") / "gridline" / "settings.json").string(), "settings path from the config root");
  platform::unsetEnv("GRIDLINE_CONFIG_HOME");
  platform::unsetEnv("XDG_CONFIG_HOME");
  const std::string defaultRoot = platform::configRoot();
  platform::setEnv("XDG_CONFIG_HOME", (test::hermeticTestEnv("portability") + "/config").c_str());  // back to the private folders
#if defined(_WIN32)
  expect(!defaultRoot.empty() && defaultRoot != "." && defaultRoot.find(".config") == std::string::npos, "Windows: config root is %APPDATA%");
#else
  expect(defaultRoot == (fs::path(platform::homeDir()) / ".config").string(), "config root defaults to ~/.config");
#endif
  expect(!platform::cacheRoot().empty() && !platform::dataRoot().empty(), "cache and data roots");
}

void testEnvironment() {
  expect(platform::setEnv("GL_PORT_TEST", "a=b c") && std::string(std::getenv("GL_PORT_TEST")) == "a=b c", "setEnv / getenv");
  expect(platform::setEnv("GL_PORT_TEST", "kept", false) && std::string(std::getenv("GL_PORT_TEST")) == "a=b c", "setEnv keeps a value");
  expect(platform::unsetEnv("GL_PORT_TEST") && (std::getenv("GL_PORT_TEST") == nullptr || !*std::getenv("GL_PORT_TEST")), "unsetEnv");
  expect(platform::processId() > 0, "process id");
  const std::string exe = platform::executablePath();
  expect(!exe.empty() && fs::exists(fs::u8path(exe)), "executable path found");
}

void testNumbers() {
  char* end = nullptr;
  expect(platform::parseDouble("1.5e3x", &end) == 1500.0 && end && *end == 'x', "parseDouble reads like strtod");
  expect(platform::formatDouble("%.3f", 0.25) == "0.250", "formatDouble");
  // A comma-decimal locale, as a hosted plugin or desktop library may set for the process.
  const char* locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "de_DE", "German_Germany.1252", "de-DE"};
  const std::string before = std::setlocale(LC_NUMERIC, nullptr);
  bool comma = false;
  for (const char* l : locales)
    if (std::setlocale(LC_NUMERIC, l) && *std::localeconv()->decimal_point == ',') {
      comma = true;
      break;
    }
  if (comma) {
    expect(platform::parseDouble("2.75") == 2.75, "comma locale: '.' still parses");
    expect(platform::formatDouble("%.2f", 2.75) == "2.75", "comma locale: '.' still written");
    Json j = Json::makeObject();
    j.object["x"] = Json::makeNumber(0.1);
    j.object["y"] = Json::makeNumber(-1234.5678);
    const std::string text = jsonStringify(j, 0);
    Json back;
    expect(text.find("0.1") != std::string::npos && text.find("1234.5678") != std::string::npos && jsonParse(text, back) && back.num("x", 0) == 0.1 && back.num("y", 0) == -1234.5678,
           "comma locale: project JSON numbers round-trip");
  } else {
    std::printf("# no comma-decimal locale installed: locale checks skipped\n");
  }
  std::setlocale(LC_NUMERIC, before.c_str());
}

void testShell() {
  const fs::path dir = fs::u8path(test::hermeticTestEnv("portability")) / "shell dir";
  fs::create_directories(dir);
  const fs::path out = dir / "out file.txt";
  const std::string cmd = platform::shellQuote(GRIDLINE_CLI_EXE) + " --version > " + platform::shellQuote(out.string());
  expect(platform::runShell(cmd) == 0, "runShell with quoted paths (spaces)");
  std::ifstream is(out);
  std::string line;
  std::getline(is, line);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  expect(line.rfind("Gridline ", 0) == 0, "the CLI wrote its version through the shell");
}

void testProcess() {
  platform::Process p;
  expect(p.start(GRIDLINE_CLI_EXE, {GRIDLINE_CLI_EXE, "--version"}) && p.wait(60) && p.status().exited && !p.status().crashed && p.status().code == 0,
         "child process runs and exits 0");
  platform::Process bad;
  expect(bad.start(GRIDLINE_CLI_EXE, {GRIDLINE_CLI_EXE, "info", "/nonexistent/project.gridline.json"}) && bad.wait(60) && bad.status().code != 0,
         "child exit code is reported");
  platform::Process slow;
  expect(slow.start(GRIDLINE_CLI_EXE, {GRIDLINE_CLI_EXE, "mcp", "--attach", "--endpoint", "/nonexistent/endpoint.json"},
                    platform::Process::kQuiet), "a child that waits for input starts");
  // Its stdin is the null device: it reads end of input and leaves; a hung one is killed.
  if (!slow.wait(20)) {
    slow.kill();
    slow.wait();
  }
  expect(slow.status().exited, "child ended (exit or kill)");
  expect(!platform::Process().running(), "an unstarted process is not running");
}

void testFiles() {
  const fs::path dir = fs::u8path(test::hermeticTestEnv("portability"));
  // Non-ASCII names (UTF-8 everywhere, the Windows manifest sets the UTF-8 code page).
  const fs::path f = dir / fs::u8path("D\xC3\xBCsen \xE2\x99\xAA.txt");
  expect(platform::writePrivateFile(f.u8string(), "one"), "writePrivateFile (non-ASCII name)");
  const fs::path g = dir / "replace.txt";
  expect(platform::writePrivateFile(g.string(), "two") && platform::replaceFile(f.u8string(), g.string()), "replaceFile over an existing file");
  std::ifstream is(g, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
  expect(text == "one" && !fs::exists(f), "replaced content");
  expect(platform::writable(g.string()), "writable file");
  std::FILE* c = std::fopen(f.u8string().c_str(), "wb");  // narrow fopen with a UTF-8 name
  expect(c != nullptr, "fopen with a UTF-8 name");
  if (c) std::fclose(c);
  {  // CPU time is counted in clock ticks (FreeBSD: statistics clock): use some first
    const auto t0 = std::chrono::steady_clock::now();
    volatile double sink = 0;
    while (platform::cpuSeconds() <= 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
      for (int k = 0; k < 100000; ++k) sink = sink + std::sqrt(static_cast<double>(k));
    expect(platform::cpuSeconds() > 0 && platform::peakRssMb() > 1, "process statistics");
  }
}

void testSockets() {
  expect(net::startup(), "socket layer starts");
  const net::Sock srv = net::tcpSocket();
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = 0;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = srv != net::kNoSock && ::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(srv, 4) == 0;
  net::SockLen len = sizeof a;
  ok = ok && getsockname(srv, reinterpret_cast<sockaddr*>(&a), &len) == 0;
  expect(ok, "loopback listener");
  std::thread client([port = a.sin_port] {
    const net::Sock c = net::tcpSocket();
    sockaddr_in b{};
    b.sin_family = AF_INET;
    b.sin_port = port;
    b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(c, reinterpret_cast<sockaddr*>(&b), sizeof b) == 0) net::sendSome(c, "ping", 4);
    net::closeSock(c);
  });
  net::PollFd pf = net::pollEntry(srv, POLLIN);
  const bool ready = net::pollSocks(&pf, 1, 5000) > 0;
  std::string got;
  if (ready) {
    const net::Sock s = static_cast<net::Sock>(::accept(srv, nullptr, nullptr));
    net::setTimeouts(s, 5);
    char buf[16];
    long n;
    while ((n = net::recvSome(s, buf, sizeof buf)) > 0) got.append(buf, static_cast<size_t>(n));
    net::closeSock(s);
  }
  client.join();
  net::closeSock(srv);
  expect(got == "ping", "bytes over a loopback connection");
}

void testDylib() {
  std::string err;
  expect(host::dl::open((fs::u8path(test::hermeticTestEnv("portability")) / "missing-library.bin").string(), &err) == nullptr && !err.empty(),
         "a missing shared object fails with a reason");
  const std::string self = host::dl::pathOf(reinterpret_cast<const void*>(&testDylib));
  expect(!self.empty() && fs::exists(fs::u8path(self)), "the file containing an address");
}

void testKeys() {
  expect(host::hidUsageOfWinVk(0x20) == 44 && host::macKeyOfHidUsage(44) == 0x31, "Windows Space -> HID -> macOS key code");
  expect(host::hidUsageOfWinVk('S') == 22 && host::macKeyOfHidUsage(host::hidUsageOfWinVk('S')) == 0x01, "letters map");
  host::KeyEvent e;
  e.code = host::macKeyOfHidUsage(host::hidUsageOfWinVk('S'));
  e.mods = host::kKeyCtrl;
  const keys::Chord c = host::chordOf(e);
#if defined(__APPLE__)
  expect(c.mods == 0, "macOS: Ctrl is not Gridline's command key");
#else
  expect((c.mods & keys::kCtrl) != 0, "Ctrl+S forwarded as Gridline's Ctrl+S");
#endif
}

}  // namespace

int main() {
  test::hermeticTestEnv("portability");
  testFolders();
  testEnvironment();
  testNumbers();
  testShell();
  testProcess();
  testFiles();
  testSockets();
  testDylib();
  testKeys();
  return finish();
}
