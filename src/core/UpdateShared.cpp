// Gridline - K-308: the shared update check state (see UpdateShared.h).
#include "UpdateShared.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include "Json.h"
#include "Update.h"

namespace gl::update::shared {

namespace fs = std::filesystem;

bool State::operator==(const State& o) const {
  return lastAttempt == o.lastAttempt && lastSuccess == o.lastSuccess && latest == o.latest && page == o.page && suiteUrl == o.suiteUrl &&
         miniUrl == o.miniUrl && skipVersion == o.skipVersion && remindAfter == o.remindAfter && pluginsAuto == o.pluginsAuto && beta == o.beta;
}

namespace {
bool set(const Env& env, const char* k) {
  const char* v = env(k);
  return v && *v;
}
}  // namespace

std::string configDir(const Env& env, bool windows) {
  if (set(env, "GRIDLINE_CONFIG_HOME")) return (fs::u8path(env("GRIDLINE_CONFIG_HOME")) / "gridline").u8string();
  if (windows) return set(env, "APPDATA") ? (fs::u8path(env("APPDATA")) / "gridline").u8string() : std::string();
  if (set(env, "XDG_CONFIG_HOME")) return (fs::u8path(env("XDG_CONFIG_HOME")) / "gridline").u8string();
  if (set(env, "HOME")) return (fs::u8path(env("HOME")) / ".config" / "gridline").u8string();
  return {};
}

bool disabledByEnv(const Env& env) {
  for (const char* k : {"GRIDLINE_NO_UPDATE_CHECK", "CI", "GRIDLINE_PLUGIN_SHOT", "GRIDLINE_PLUGIN_WINDOW_SHOT", "GRIDLINE_PLUGIN_OPEN_HOSTED"})
    if (set(env, k)) return true;
  return false;
}

bool read(const std::string& dir, State& out) {
  std::ifstream f(fs::u8path(dir) / "update-check.json", std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || !j.is(Json::Type::Object)) return false;
  State s;
  s.lastAttempt = j.num("lastAttempt");
  s.lastSuccess = j.num("lastSuccess");
  s.latest = j.text("latest");
  s.page = j.text("page");
  s.suiteUrl = j.text("suiteUrl");
  s.miniUrl = j.text("miniUrl");
  s.skipVersion = j.text("skipVersion");
  s.remindAfter = j.num("remindAfter");
  if (const Json* b = j.find("pluginsAuto"); b && b->is(Json::Type::Bool)) s.pluginsAuto = b->boolean;
  if (const Json* b = j.find("beta"); b && b->is(Json::Type::Bool)) s.beta = b->boolean;
  out = s;
  return true;
}

bool write(const std::string& dir, const State& s) {
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  Json j = Json::makeObject();
  auto boolean = [](bool v) {
    Json b;
    b.type = Json::Type::Bool;
    b.boolean = v;
    return b;
  };
  j.object["lastAttempt"] = Json::makeNumber(s.lastAttempt);
  j.object["lastSuccess"] = Json::makeNumber(s.lastSuccess);
  j.object["latest"] = Json::makeString(s.latest);
  j.object["page"] = Json::makeString(s.page);
  j.object["suiteUrl"] = Json::makeString(s.suiteUrl);
  j.object["miniUrl"] = Json::makeString(s.miniUrl);
  j.object["skipVersion"] = Json::makeString(s.skipVersion);
  j.object["remindAfter"] = Json::makeNumber(s.remindAfter);
  j.object["pluginsAuto"] = boolean(s.pluginsAuto);
  j.object["beta"] = boolean(s.beta);
  const fs::path final = fs::u8path(dir) / "update-check.json";
  fs::path tmp = final;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << jsonStringify(j) << "\n";
    if (!f) return false;
  }
  fs::rename(tmp, final, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

Decision decide(const State& s, double now, const Env& env) {
  if (disabledByEnv(env) || !s.pluginsAuto) return Decision::Off;
  if (now - s.lastAttempt < kDay && now >= s.lastAttempt) return Decision::Cached;  // a clock set back fetches again
  return Decision::Fetch;
}

bool claim(const std::string& dir, double now) {
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  const fs::path lock = fs::u8path(dir) / "update-check.lock";
  // The lock is a file holding its claim time, made complete under a private name and then hard
  // linked to the shared name: the link fails when the lock exists, so only one process wins and
  // nobody ever reads a lock without its time.
  auto make = [&] {
    std::random_device rd;
    fs::path tmp = lock;
    tmp += "." + std::to_string(rd()) + std::to_string(rd());
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      if (!f) return false;
      f << static_cast<long long>(now) << "\n";
    }
    fs::create_hard_link(tmp, lock, ec);
    const bool won = !ec;
    std::error_code ec2;
    fs::remove(tmp, ec2);
    return won;
  };
  if (make()) return true;
  long long at = 0;
  std::ifstream(lock) >> at;
  if (now - static_cast<double>(at) <= kStaleLock && static_cast<double>(at) <= now) return false;
  fs::remove(lock, ec);  // stale: take it over
  return make();
}

void release(const std::string& dir) {
  std::error_code ec;
  fs::remove(fs::u8path(dir) / "update-check.lock", ec);
}

bool shouldShow(const State& s, const std::string& current, double now) {
  if (s.latest.empty() || now < s.remindAfter) return false;
  const Version l = parseVersion(s.latest), c = parseVersion(current);
  if (!l.valid || !c.valid || compare(l, c) <= 0) return false;
  return !(s.skipVersion == s.latest);
}

State runCheck(const std::string& dir, double now, const Env& env, const Fetcher& fetch, const std::string& currentVersion,
               const Target& target, bool forced) {
  State s;
  read(dir, s);
  if (dir.empty() || disabledByEnv(env)) return s;
  if (!forced && decide(s, now, env) != Decision::Fetch) return s;
  if (!claim(dir, now)) return s;  // another process is asking: its answer arrives in the file
  s.lastAttempt = now;
  write(dir, s);  // claimed: the others see today's attempt at once
  const CheckResult r = check(fetch, currentVersion, s.beta);
  if (r.status == Status::Available || r.status == Status::UpToDate) {
    s.lastSuccess = now;
    s.latest = r.status == Status::Available ? toString(r.release.version) : std::string();
    s.page = r.release.htmlUrl.empty() ? std::string(kReleasesPage) : r.release.htmlUrl;
    const Asset* suite = r.status == Status::Available ? pickSuiteAsset(r.release, target, false) : nullptr;
    const Asset* mini = r.status == Status::Available ? pickSuiteAsset(r.release, target, true) : nullptr;
    s.suiteUrl = suite ? suite->url : std::string();
    s.miniUrl = mini ? mini->url : std::string();
  }
  write(dir, s);
  release(dir);
  return s;
}

}  // namespace gl::update::shared
