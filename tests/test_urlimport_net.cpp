// Gridline - Import from URL over the real network, the way a build without the tools does it the
// first time: yt-dlp from its newest GitHub release and ffmpeg from Gridline's newest release
// (both checked against their published checksums and run once), then a link read and imported
// into a 32-bit float WAV the engine decodes. Public test material only: a short Creative Commons
// clip from Wikimedia Commons (a direct link to an audio file).
// Skipped (exit code 77) without a network, when GitHub limits requests, or with
// GRIDLINE_NO_NETWORK=1. Requests carry Gridline's generic User-Agent and nothing else.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "Check.h"
#include "Hermetic.h"
#include "core/MediaTools.h"
#include "core/Platform.h"
#include "core/Update.h"
#include "core/UrlImport.h"
#include "engine/Sample.h"
#include "host/net/Http.h"

namespace fs = std::filesystem;
using namespace gl;
namespace ul = gl::urlimport;
namespace mt = gl::mediatools;

namespace {

constexpr int kSkip = 77;
// Wikimedia Commons, "Example.ogg" (CC BY-SA 3.0): a few seconds of audio, about 100 KB.
constexpr const char* kClip = "https://upload.wikimedia.org/wikipedia/commons/c/c8/Example.ogg";

std::shared_ptr<ul::Job> waitJob(std::shared_ptr<ul::Job> j, double seconds) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!j->done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return j;
}

void check(bool ok, const std::string& name) { expect(ok, name.c_str()); }

int skip(const std::string& why) {
  std::printf("SKIPPED: %s\n", why.c_str());
  return kSkip;
}

}  // namespace

int main() {
  const std::string root = test::hermeticTestEnv("urlimport-net");
  if (const char* n = std::getenv("GRIDLINE_NO_NETWORK"); n && std::strcmp(n, "1") == 0) return skip("GRIDLINE_NO_NETWORK=1");
  platform::unsetEnv("GRIDLINE_YTDLP");
  platform::unsetEnv("GRIDLINE_FFMPEG");
  const update::Target target = update::currentTarget();
  if (mt::ytdlpAsset(target).empty() || mt::mediaToolsAsset("0", target).empty())
    return skip("no ready-made yt-dlp and ffmpeg for " + target.os + " " + target.arch);
  const mt::Net net{http::fetcher(), http::downloader()};
  const update::HttpResult probe = net.fetch(mt::kYtdlpLatest);
  if (probe.offline || probe.status == 0) return skip("no network");
  if (probe.status == 403 || probe.status == 429) return skip("GitHub is limiting requests");

  // 1. The first-use set-up, into a folder with a space in its name.
  const fs::path tools = fs::u8path(root) / "data" / "gridline tools";
  const mt::InstallResult y = mt::installYtdlp(net, tools.u8string(), nullptr, nullptr);
  if (!y.ok && y.error.find("limiting") != std::string::npos) return skip("GitHub is limiting requests");
  check(y.ok, "yt-dlp installed from its newest release: " + y.error);
  check(!y.ok || (!mt::quarantined(y.path) && mt::recordedVersion(tools.u8string()) == y.version), "yt-dlp: version recorded, no flag");
  const mt::InstallResult f = mt::installFfmpeg(net, tools.u8string(), nullptr, nullptr, target);
  if (!f.ok && f.error.find("limiting") != std::string::npos) return skip("GitHub is limiting requests");
  check(f.ok, "ffmpeg installed from Gridline's newest release: " + f.error);
  check(!f.ok || f.version.find("ffmpeg version") == 0, "ffmpeg runs: " + f.version);
  check(fs::exists(tools / mt::exeName("ffprobe")), "ffprobe beside ffmpeg");
  if (!y.ok || !f.ok) return finish();
  // The update check finds the copy current.
  const mt::UpdateResult u = mt::updateYtdlp(net, tools.u8string(), y.version, nullptr);
  check(u.checked && !u.updated, "update: the fresh copy is current");

  // 2. Read the link and import it, as the popup does.
  mt::Search se{tools.u8string(), "", false};
  const mt::Tool yt = mt::locateYtdlp(se), ff = mt::locateFfmpeg(se);
  check(yt.from == mt::From::Installed && ff.from == mt::From::Installed, "the installed copies are found");
  std::vector<std::string> ytArgv = yt.argv();
  const auto js = mt::jsRuntimeArgs();
  ytArgv.insert(ytArgv.end(), js.begin(), js.end());
  const ul::Tools t{ytArgv, ff.path};
  const fs::path work = fs::u8path(root) / "work dir";
  auto fetch = waitJob(ul::startFetch(t, kClip, (work / "info").u8string()), 120);
  ul::Info info;
  {
    std::lock_guard<std::mutex> lk(fetch->mu);
    check(fetch->get() == ul::Phase::Done, "the link is read: " + fetch->error + " " + fetch->lead);
    info = fetch->info;
  }
  if (fetch->get() != ul::Phase::Done) return finish();
  check(!info.playlist && !info.title.empty(), "a single item with a title: " + info.title);
  ul::Request rq;
  rq.item.title = info.title;
  rq.item.url = info.url;
  rq.item.duration = info.duration;
  rq.site = info.site;
  auto imp = waitJob(ul::startImport(t, {rq}, (work / "items").u8string()), 300);
  std::string wav;
  {
    std::lock_guard<std::mutex> lk(imp->mu);
    check(imp->get() == ul::Phase::Done && imp->results.size() == 1, "downloaded and converted: " + imp->error + " " + imp->lead);
    if (!imp->results.empty()) wav = imp->results[0].wav;
  }
  SamplePtr smp = wav.empty() ? nullptr : loadSample(wav);
  check(smp && smp->seconds() > 1.0 && smp->sampleRate > 8000, "a WAV the engine decodes");
  if (smp) std::printf("imported %.2f s at %.0f Hz\n", smp->seconds(), static_cast<double>(smp->sampleRate));
  return finish();
}
