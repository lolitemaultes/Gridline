// Gridline - automatic updates at start-up (see SplashUpdate.h).
#include "SplashUpdate.h"
#include "GridlineVersion.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <thread>
#include "../core/Crash.h"
#include "../core/Platform.h"
#include "../core/Settings.h"
#include "../core/Update.h"
#include "../core/UpdateFlow.h"
#include "../host/net/Http.h"
#include "App.h"

namespace gl::splashupd {

namespace fs = std::filesystem;
namespace ap = update::apply;
using update::flow::Flow;
using update::flow::Outcome;
using update::flow::Stage;

namespace {

ap::Recovery gRecovery;
std::unique_ptr<Flow> gBackground;

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

bool testServer() {
  const char* t = std::getenv("GRIDLINE_UPDATE_TEST_SERVER");
  return t && std::strncmp(t, "http://127.0.0.1:", 17) == 0;
}

void remember(App& a, const std::string& result, bool checked) {
  a.settings.set("update.lastResult", result);
  if (checked) a.settings.set("update.lastCheck", static_cast<double>(now()));
  a.settings.save(Settings::defaultPath());
}

}  // namespace

const update::Install& install() {
  static const update::Install in = update::detectCurrentInstall();
  return in;
}

bool recoverAtStart(const std::string& configDir, const std::vector<std::string>& args) {
  const std::string dir = ap::updatesDir(configDir);
  gRecovery = ap::recover(dir, GRIDLINE_VERSION, (fs::u8path(configDir) / "settings.json").u8string(), ap::FileOps{}, now());
  if (gRecovery.what != ap::Recovery::What::RollBackSelf) return false;
  // This (new) version failed to start before: the old one is back in place; start it.
  update::Install in;
  in.kind = update::kindFromId(gRecovery.journal.kind);
  in.target = gRecovery.journal.target;
  const std::vector<std::string> cmd = update::flow::relaunchCommand(in, platform::executablePath(), args);
  platform::Process p;
  const bool ok = p.start(cmd[0], cmd, platform::Process::kTree);
  ap::log(dir, std::string("started the previous version again") + (ok ? "" : " (could not start it)"), now());
  return ok;
}

const ap::Recovery& startRecovery() { return gRecovery; }

bool devView(const std::string& name, SplashUpdateView& v) {
  v = SplashUpdateView{};
  v.version = "0.7.9";
  v.total = 37748736;
  if (name == "checking") v.state = SplashUpdateView::Checking, v.progress = 0.22f;
  else if (name == "downloading") v.state = SplashUpdateView::Downloading, v.progress = 0.34f, v.got = 12834816;
  else if (name == "verifying") v.state = SplashUpdateView::Verifying, v.progress = 1.0f;
  else if (name == "installing") v.state = SplashUpdateView::Installing, v.progress = 1.0f;
  else if (name == "permission") v.state = SplashUpdateView::Permission, v.progress = 1.0f;
  else if (name == "permission-windows") v.state = SplashUpdateView::Permission, v.progress = 1.0f, v.windows = true;
  else if (name == "restarting") v.state = SplashUpdateView::Restarting, v.progress = 1.0f;
  else if (name == "failed") v.state = SplashUpdateView::Failed, v.progress = 0.22f;
  else if (name == "background") v.state = SplashUpdateView::Background, v.progress = 0.34f;
  else return false;
  return true;
}

bool backgroundRunning() { return gBackground && !gBackground->finished(); }

void shutdown() {
  if (!gBackground) return;
  gBackground->cancel();  // a download in progress is kept as a part for the next start
  gBackground.reset();
}

Result run(App& a, const std::vector<std::string>& args, const Options& o, const FrameFn& frame) {
  Settings& st = a.settings;
  if (!st.getBool("update.auto") || !st.getBool("update.autoInstall")) return Result::Continue;
  if (o.disabled || update::disabledByEnvironment()) return Result::Continue;
  if (o.automated && !testServer()) return Result::Continue;  // scripted runs never touch the network
  const std::string configDir = a.configDir();
  const std::string dir = ap::updatesDir(configDir);
  // A crash left a recovery copy that has not been offered yet: that comes first, in this version.
  for (const crash::PendingReport& r : crash::collectPending(configDir))
    if (!r.recoveryFile.empty()) {
      ap::log(dir, "splash: not now (a recovery copy waits to be opened)", now());
      return Result::Continue;
    }
  update::flow::Env env = update::flow::systemEnv(configDir, GRIDLINE_VERSION, st.getBool("update.beta"), args);
  env.settingsPath = Settings::defaultPath();
  env.fetch = http::condFetcher();
  env.download = [](const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
                    const std::atomic<bool>* cancel, int stall, std::string* err) {
    return http::downloadContinue(url, path, progress, cancel, stall, err);
  };
  // Never at the splash for a copy that cannot update itself: no request at all (the popup's
  // daily check tells about new versions there).
  const update::Strategy plan = update::strategyFor(env.install, nullptr, env.target);
  const update::Method m = plan.method;
  if (m != update::Method::ReplaceFile && m != update::Method::ReplaceFolder && m != update::Method::RunInstallScript &&
      m != update::Method::RunWindowsSetup && m != update::Method::SystemPackage)
    return Result::Continue;
  // A test run of the watcher's restart: never really starts another Gridline from a scripted run.
  if (o.automated) env.startWatcher = std::getenv("GRIDLINE_UPDATE_TEST_WATCHER") != nullptr;

  auto flow = std::make_unique<Flow>(env);
  flow->start();
  const auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  double shownSince = -1;  // the closing state's time on screen
  SplashUpdateView v;
  for (;;) {
    const update::flow::Snapshot s = flow->snapshot();
    v.version = s.version;
    v.got = s.got;
    v.total = s.total;
    v.windows = env.target.os == "windows";
    switch (s.stage) {
      case Stage::Checking: v.state = SplashUpdateView::Checking; break;
      case Stage::Downloading:
        v.state = SplashUpdateView::Downloading;
        v.progress = s.total > 0 ? static_cast<float>(static_cast<double>(s.got) / static_cast<double>(s.total)) : 0.0f;
        break;
      case Stage::Verifying: v.state = SplashUpdateView::Verifying, v.progress = 1.0f; break;
      case Stage::Installing: v.state = SplashUpdateView::Installing, v.progress = 1.0f; break;
      case Stage::NeedsPermission: v.state = SplashUpdateView::Permission, v.progress = 1.0f; break;
      case Stage::Restarting: v.state = SplashUpdateView::Restarting, v.progress = 1.0f; break;
      case Stage::Finished:
        if (s.outcome == Outcome::Relaunch) v.state = SplashUpdateView::Restarting, v.progress = 1.0f;
        else if (s.outcome == Outcome::Failed) v.state = SplashUpdateView::Failed;
        break;
    }
    if (s.wentBackground && !flow->finished()) v.state = SplashUpdateView::Background;
    const bool finished = flow->finished();
    // How long the last state stays on screen: a failure is read, a restart is seen, the rest go at once.
    if (finished && shownSince < 0) shownSince = elapsed();
    const double hold = s.outcome == Outcome::Failed ? 1.6 : s.outcome == Outcome::Relaunch ? 0.6 : 0.0;
    if (finished && elapsed() - shownSince >= hold) break;
    bool quit = false;
    frame(v, &quit);
    if (quit) {  // the window was closed: no update, no start
      flow->cancel();
      flow.reset();
      remember(a, "cancelled (the window was closed)", false);
      return Result::Quit;
    }
    if (finished) continue;
    // A check slower than its time limit, or a stalled download: Gridline starts by itself; the
    // update goes on behind it and installs at the next start. (No buttons, no keys.)
    const bool slowCheck = s.stage == Stage::Checking && elapsed() > env.checkTimeout + 1.0;
    if (slowCheck || v.state == SplashUpdateView::Background) {
      flow->background();
      ap::log(dir, slowCheck ? "splash: the check is slow; starting" : "splash: the download stalled; starting", now());
      gBackground = std::move(flow);
      remember(a, "continuing in the background", false);
      return Result::Continue;
    }
  }
  const update::flow::Snapshot s = flow->snapshot();
  flow->wait();
  std::string text;
  switch (s.outcome) {
    case Outcome::UpToDate: text = "up to date"; break;
    case Outcome::Relaunch: text = "installed " + s.version; break;
    case Outcome::Offline: text = "couldn't check (no connection)"; break;
    case Outcome::Staged: text = s.version + " is ready for the next start"; break;
    case Outcome::Failed: text = s.version + " didn't install: " + ap::plainReason(s.message); break;
    case Outcome::PermissionLater: text = s.version + " wasn't installed (the system's prompt was closed)"; break;
    case Outcome::NotHere: text = "updated by its package manager or store"; break;
    case Outcome::Skipped: text = "skipped"; break;
    default: text = std::string(update::flow::outcomeId(s.outcome)); break;
  }
  remember(a, text, s.outcome != Outcome::Offline);
  return s.outcome == Outcome::Relaunch ? Result::Restart : Result::Continue;
}

}  // namespace gl::splashupd
