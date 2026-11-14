// Gridline - Import from URL over the real network, the way a build without the tools does it the
// first time: yt-dlp from its newest GitHub release and ffmpeg from Gridline's newest release
// (both checked against their published checksums and run once), then a link read and imported
// into a 32-bit float WAV the engine decodes. Public test material only: a short Creative Commons
// clip from Wikimedia Commons (a direct link to an audio file).
// Skipped (exit code 77) without a network, when GitHub limits requests, or with
// GRIDLINE_NO_NETWORK=1. Requests carry Gridline's generic User-Agent and nothing else.
#include <algorithm>
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
  // (A second unauthenticated request to GitHub's API: a shared runner may be limited by now, and that
  // check alone is skipped. A release published between the two requests is installed: also right.)
  const mt::UpdateResult u = mt::updateYtdlp(net, tools.u8string(), y.version, nullptr);
  if (!u.checked && u.error.find("limiting") != std::string::npos) {
    std::printf("skipped: update check (%s)\n", u.error.c_str());
  } else {
    const bool current = u.checked && !u.updated && u.error.empty() && u.version == y.version;
    const bool newer = u.checked && u.updated && u.error.empty() && u.version > y.version;
    check(current || newer, "update: the fresh copy is current (" + y.version + " -> " + u.version + ") " + u.error);
  }

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

  // 3. One stable, freely licensed link per main kind of site, up to ten seconds of each from 1 s on
  // (the whole stream is downloaded, only the range is converted). A site that limits or refuses this
  // computer right now (a bot check, HTTP 403 / 429 on every try) is reported and skipped.
  struct Source {
    const char* name;
    const char* url;
  };
  const Source sources[] = {
      // Blender Foundation, "Big Buck Bunny" (CC BY 3.0): a 5.1 film mix next to stereo streams
      {"YouTube", "https://www.youtube.com/watch?v=aqz-KE-bpKQ"},
      // Kevin MacLeod, "Parisian" (CC BY), as uploaded to SoundCloud
      {"SoundCloud", "https://soundcloud.com/user-231592479/kevin-macleod-parisian"},
      // Internet Archive item "Never To Appear Again" (CC BY-ND 4.0), about 40 seconds
      {"Internet Archive", "https://archive.org/details/NeverToAppearAgain"},
      // Wikimedia Commons file page (CC BY-SA 3.0), whose title the site gives as HTML
      {"Wikimedia Commons", "https://commons.wikimedia.org/wiki/File:Example.ogg"},
  };
  for (const Source& src : sources) {
    std::this_thread::sleep_for(std::chrono::seconds(2));  // gentle with the sites
    const std::string tag = src.name;
    auto f = waitJob(ul::startFetch(t, src.url, (work / ("info " + tag)).u8string()), 200);
    ul::Info in;
    std::string err;
    {
      std::lock_guard<std::mutex> lk(f->mu);
      in = f->info;
      err = f->error;
    }
    auto limited = [](const std::string& e) {
      return e.find("not a bot") != std::string::npos || e.find("429") != std::string::npos || e.find("403") != std::string::npos;
    };
    if (f->get() != ul::Phase::Done && limited(err)) {
      std::printf("skipped: %s limits this computer right now (%s)\n", src.name, err.c_str());
      continue;
    }
    check(f->get() == ul::Phase::Done && !in.title.empty() && in.title.find('<') == std::string::npos && in.duration > 3,
          tag + ": read, a plain title and a length: " + in.title + " " + err);
    if (f->get() != ul::Phase::Done) continue;
    ul::Request r;
    r.item.title = in.title;
    r.item.url = in.url;
    r.item.duration = in.duration;
    r.site = in.site;
    r.trim = ul::Trim{1, std::min(11.0, in.duration)};
    const double want = r.trim.end - r.trim.start;
    auto im = waitJob(ul::startImport(t, {r}, (work / ("items " + tag)).u8string()), 400);
    std::string w, ierr;
    {
      std::lock_guard<std::mutex> lk(im->mu);
      if (!im->results.empty()) w = im->results[0].wav;
      ierr = im->error;
    }
    if (im->get() != ul::Phase::Done && limited(ierr)) {
      std::printf("skipped: %s refused the download on every try (%s)\n", src.name, ierr.c_str());
      continue;
    }
    SamplePtr s = w.empty() ? nullptr : loadSample(w);
    float peak = 0;
    if (s)
      for (size_t i = 0; i < s->left.size(); ++i) peak = std::max(peak, std::fabs(s->left[i]));
    check(im->get() == ul::Phase::Done && s && std::fabs(s->seconds() - want) < 0.1 && peak > 1e-3,
          tag + ": the range imported, not silent: " + ierr);
    if (s) std::printf("%s: %.2f s at %.0f Hz, peak %.3f\n", src.name, s->seconds(), static_cast<double>(s->sampleRate), static_cast<double>(peak));
  }
  return finish();
}
