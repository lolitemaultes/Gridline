// Gridline — J-61 follow-ups: the audio load meter (time-weighted, never
// above 100 % on screen) and the secret store that keeps the MCP token out of
// settings.json (private file backend, migration from the plain setting).
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
#include <filesystem>
#include <fstream>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/Secrets.h"
#include "core/Settings.h"
#include "engine/AudioEngine.h"
#include "engine/Render.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void testCpuMeter() {
  {  // Idle: a small steady load reads as that load.
    CpuMeter m;
    for (int i = 0; i < 100; ++i) m.add(0.0005, 0.01);
    expect(std::fabs(m.load() - 0.05f) < 0.005f, "cpu: steady 5 % reads 5 %");
  }
  {  // One stalled block (cold page faults, the thread descheduled) among
     // idle ones: the meter moves by the block's share of the window only.
    CpuMeter m;
    for (int i = 0; i < 50; ++i) m.add(0.0003, 0.0116);
    const float spike = m.add(0.030, 0.0116);  // 2.6x the block's budget
    for (int i = 0; i < 9; ++i) m.add(0.0003, 0.0116);
    expect(spike > 2.0f, "cpu: the stalled block itself is reported (peak)");
    expect(m.load() < 0.5f, "cpu: one stalled block does not push the average over 50 %");
  }
  {  // Block size does not matter: tiny blocks with the same total load read the same.
    CpuMeter big, small;
    for (int i = 0; i < 40; ++i) big.add(0.002, 0.01);
    for (int i = 0; i < 4000; ++i) small.add(0.00002, 0.0001);
    expect(std::fabs(big.load() - small.load()) < 0.01f, "cpu: time-weighted, independent of block size");
  }
  {  // Bad input is ignored.
    CpuMeter m;
    m.add(0.001, 0.0);
    m.add(-1, 0.01);
    expect(m.load() == 0.0f, "cpu: zero-length / negative blocks ignored");
  }
  expect(CpuMeter::displayPercent(1.07f) == 100 && CpuMeter::displayPercent(9.0f) == 100, "cpu: overload shows 100 %, not more");
  expect(CpuMeter::displayPercent(0.234f) == 23 && CpuMeter::displayPercent(-0.1f) == 0 && CpuMeter::displayPercent(NAN) == 0,
         "cpu: display percent clamps and rounds");
  // A real engine, idle (nothing playing), processing blocks as fast as it
  // can: the load is a small fraction, never over 100 %.
  AudioEngine eng(44100);
  std::vector<float> L(512), R(512);
  for (int i = 0; i < 400; ++i) eng.process(L.data(), R.data(), 512);
  expect(eng.cpuLoad() >= 0.0f && eng.cpuLoad() < 1.0f, "cpu: idle engine reads under 100 %");
}

void testSecretFile() {
  const fs::path dir = fs::path(test::hermeticTestEnv("j61")) / "secrets-a";
  secrets::Store st;
  st.backend = secrets::Backend::File;
  st.dir = dir.string();
  std::string v;
  expect(!st.get("mcp-token", v), "secrets: nothing stored yet");
  expect(st.set("mcp-token", "abc123abc123abc123"), "secrets: set");
  expect(st.get("mcp-token", v) && v == "abc123abc123abc123", "secrets: get");
#if !defined(_WIN32)  // POSIX permissions (Windows: the folder lives in the user's private profile)
  struct stat sb{};
  expect(::stat((dir / "mcp-token").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600, "secrets: file is 0600");
  expect(::stat(dir.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0700, "secrets: folder is 0700");
  ::chmod((dir / "mcp-token").c_str(), 0644);
  expect(st.get("mcp-token", v) && ::stat((dir / "mcp-token").c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600, "secrets: a loosened file is tightened");
#endif
  expect(st.set("mcp-token", "second-value-0000"), "secrets: replace");
  expect(st.get("mcp-token", v) && v == "second-value-0000", "secrets: replaced value");
  expect(!st.set("../escape", "x") && !st.set("", "x") && !st.set(".hidden", "x"), "secrets: names cannot leave the folder");
  expect(st.remove("mcp-token") && !st.get("mcp-token", v), "secrets: remove");
  expect(secrets::defaultBackend() == secrets::Backend::File, "secrets: tests never use the Keychain");
}

void testMigration() {
  const fs::path base = fs::path(test::hermeticTestEnv("j61"));
  const std::string settingsPath = (base / "mig-settings.json").string();
  secrets::Store st;
  st.backend = secrets::Backend::File;
  st.dir = (base / "secrets-b").string();
  {  // A token from settings.json moves to the store; the setting is cleared on disk.
    Settings s;
    s.set("mcp.token", std::string("legacy-token-0123456789"));
    s.save(settingsPath);
    bool stored = false;
    const std::string t = secrets::migrate(s, "mcp.token", settingsPath, st, "mcp-token", [] { return std::string("generated-xxxxxxxx"); }, &stored);
    expect(t == "legacy-token-0123456789" && stored, "migrate: the existing token is kept and stored");
    std::string v;
    expect(st.get("mcp-token", v) && v == t, "migrate: token in the store");
    Settings back;
    back.load(settingsPath);
    std::ifstream f(settingsPath);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    expect(back.getString("mcp.token").empty() && text.find("legacy-token") == std::string::npos, "migrate: settings.json no longer holds it");
  }
  {  // Second start: read from the store, nothing generated.
    Settings s;
    s.load(settingsPath);
    const std::string t = secrets::migrate(s, "mcp.token", settingsPath, st, "mcp-token", [] { return std::string("generated-xxxxxxxx"); });
    expect(t == "legacy-token-0123456789", "migrate: later starts read the store");
  }
  {  // Fresh install: generated and stored, never written to settings.
    secrets::Store fresh = st;
    fresh.dir = (base / "secrets-c").string();
    Settings s;
    bool stored = false;
    const std::string t = secrets::migrate(s, "mcp.token", settingsPath, fresh, "mcp-token", [] { return std::string("generated-xxxxxxxx"); }, &stored);
    expect(t == "generated-xxxxxxxx" && stored && s.getString("mcp.token").empty(), "migrate: a new token goes straight to the store");
  }
  {  // The store cannot be written: the token stays in the setting (nothing lost).
    const fs::path blocker = base / "not-a-folder";
    std::ofstream(blocker) << "x";
    secrets::Store broken = st;
    broken.dir = (blocker / "sub").string();
    Settings s;
    s.set("mcp.token", std::string("legacy-token-0123456789"));
    bool stored = true;
    const std::string t = secrets::migrate(s, "mcp.token", settingsPath, broken, "mcp-token", nullptr, &stored);
    expect(t == "legacy-token-0123456789" && !stored && s.getString("mcp.token") == t, "migrate: unwritable store keeps the setting");
  }
}

}  // namespace

int main() {
  test::hermeticTestEnv("j61");
  testCpuMeter();
  testSecretFile();
  testMigration();
  return finish();
}
