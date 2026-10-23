// Gridline — J-92: every built-in plugin parameter recorded as working in the
// audit baseline (tests/param_audit_baseline.tsv, written by
// `gridline param-audit --baseline`) still changes the plugin's output: the
// parameter at its recorded value (on top of its recorded condition, for
// parameters that work in combination with another one) against the same
// settings without it. The value sweep, live changes and the project round
// trip run on a few plugins of each kind.
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "../src/core/PluginInfo.h"
#include "../src/core/flp/Flp.h"
#include "../src/engine/ParamAudit.h"
#include "Check.h"
#include "Hermetic.h"

using namespace gl;

namespace {

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void testBaseline() {
  const std::string text = readFile("tests/param_audit_baseline.tsv");
  expect(!text.empty(), "baseline file present");
  const auto rows = paramaudit::parseBaseline(text);
  std::map<std::string, std::vector<const paramaudit::BaselineRow*>> byType;
  size_t rowsChecked = 0, skippedNoData = 0;
  const std::string factoryData = flp::defaultFactoryData();
  std::error_code ec;
  const bool noFactoryData = factoryData.empty() || !std::filesystem::is_directory(std::filesystem::u8path(factoryData), ec);
  for (const auto& r : rows) {
    if (r.status != "working" && r.status != "combination") continue;
    // Sounds read from FL Studio's factory content (Sample Keys' piano sets) are silent
    // where it is not installed, as on the CI runners: check them only here.
    if (noFactoryData && r.type == "gl.samplekeys") {
      ++skippedNoData;
      continue;
    }
    byType[r.type].push_back(&r), ++rowsChecked;
  }
  // Every built-in plugin is in the baseline (a new plugin needs a baseline refresh).
  size_t missingTypes = 0;
  for (const PluginInfo& info : pluginCatalog()) {
    if (isHostedType(info.type)) continue;
    bool any = false;
    for (const auto& r : rows) any = any || r.type == info.type;
    if (!any) {
      ++missingTypes;
      std::printf("# not in the baseline: %s\n", info.type.c_str());
    }
  }
  expect(missingTypes == 0, "every built-in plugin is in the audit baseline");
  // Every parameter of the catalogue has its row (same name), and no row is
  // left unexplained: a parameter that changes nothing is either fixed or
  // documented as intentionally inert (engine/ParamAudit.cpp, kInert).
  size_t stale = 0, unexplained = 0, rowsTotal = 0;
  for (const auto& r : rows) {
    ++rowsTotal;
    const PluginInfo* info = findPlugin(r.type);
    if (!info || r.index < 0 || static_cast<size_t>(r.index) >= info->params.size() || info->params[static_cast<size_t>(r.index)].name != r.name) {
      if (stale++ < 10) std::printf("# baseline row does not match the catalogue: %s #%d %s\n", r.type.c_str(), r.index, r.name.c_str());
    }
    if (r.status == "no-effect" || r.status == "broken") {
      if (unexplained++ < 20) std::printf("# %s #%d %s: %s\n", r.type.c_str(), r.index, r.name.c_str(), r.status.c_str());
    }
  }
  size_t catalogParams = 0;
  for (const PluginInfo& info : pluginCatalog())
    if (!isHostedType(info.type)) catalogParams += info.params.size();
  expect(stale == 0 && rowsTotal == catalogParams, "baseline rows match the catalogue (refresh: gridline param-audit --baseline)");
  expect(unexplained == 0, "every parameter works, works in combination, routes, or is documented as inert");

  std::vector<std::pair<const PluginInfo*, std::vector<const paramaudit::BaselineRow*>>> jobs;
  for (auto& [type, list] : byType) {
    const PluginInfo* info = findPlugin(type);
    if (!info) {
      std::printf("# baseline names an unknown plugin: %s\n", type.c_str());
      expect(false, ("baseline plugin exists: " + type).c_str());
      continue;
    }
    jobs.push_back({info, list});
  }
  std::sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) { return a.second.size() > b.second.size(); });
  // Warm-up (lazily built tables) on this thread.
  for (const auto& j : jobs) (void)paramaudit::checkRows(*j.first, {j.second.front()});
  std::vector<std::vector<double>> results(jobs.size());
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t k; (k = next.fetch_add(1)) < jobs.size();) results[k] = paramaudit::checkRows(*jobs[k].first, jobs[k].second);
  };
  const unsigned nt = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
  int dead = 0;
  for (size_t j = 0; j < jobs.size(); ++j)
    for (size_t i = 0; i < jobs[j].second.size(); ++i) {
      const auto* r = jobs[j].second[i];
      // Dead = no measurable change. -100 dB, not tighter: a few quiet differences measure
      // between -80 and -82 dB on arm64 (different float rounding than x86_64).
      if (results[j][i] > -100) continue;
      ++dead;
      std::printf("# %s #%d %s (%s): no longer changes the output (%.1f dB)\n", r->type.c_str(), r->index, r->name.c_str(), r->status.c_str(),
                  results[j][i]);
    }
  std::printf("# %zu working parameters of %zu plugins checked\n", rowsChecked, jobs.size());
  if (skippedNoData) std::printf("# %zu parameters skipped: they play FL Studio factory content, not installed here\n", skippedNoData);
  expect(rowsChecked > 3000, "baseline lists the working parameters");
  expect(dead == 0, "every parameter recorded as working still changes the output");
}

// Values, live change and round trip of a few plugins (the condition search runs in the full sweep).
void testFullAudit() {
  paramaudit::Options opt;
  opt.combos = false;  // the condition search runs in the full sweep (gridline param-audit)
  for (const char* type : {"gl.delay", "gl.comp", "gl.analog", "gl.arp"}) {
    const PluginInfo* info = findPlugin(type);
    if (!info) continue;
    const auto rep = paramaudit::auditPlugin(*info, opt);
    int broken = 0, lost = 0, notLive = 0, working = 0;
    for (const auto& q : rep.params) {
      broken += q.status == paramaudit::Status::Broken;
      working += q.status == paramaudit::Status::Working;
      lost += !q.roundTrip;
      notLive += q.liveTested && !q.live;
      if (q.status == paramaudit::Status::Broken || !q.roundTrip || (q.liveTested && !q.live))
        std::printf("# %s #%d %s: %s %s\n", type, q.index, q.name.c_str(), paramaudit::statusName(q.status), q.note.c_str());
    }
    std::printf("# %s: %zu params, %d working, %d renders, %.1f s\n", type, rep.params.size(), working, rep.renders, rep.seconds);
    expect(working > 0 && broken == 0 && lost == 0 && notLive == 0,
           (std::string(type) + ": full audit (working parameters, finite output, live changes, project round trip)").c_str());
  }
}

}  // namespace

namespace {
const char* volatile phase = "start";
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep every line if a platform crashes the run
  for (int sig : {SIGSEGV, SIGILL, SIGFPE, SIGABRT
#ifdef SIGBUS
                  , SIGBUS
#endif
       })
    std::signal(sig, [](int s) {
      std::fprintf(stderr, "# crashed (signal %d) in phase: %s\n", s, phase);  // diagnostics only: the process ends next
      paramaudit::writeActive(2);
      std::signal(s, SIG_DFL);
      std::raise(s);
    });
  gl::test::hermeticTestEnv("param_audit");
  phase = "full audit";
  testFullAudit();
  phase = "baseline";
  testBaseline();
  phase = "finish";
  const int rc = finish();
  phase = "exit";
  return rc;
}
