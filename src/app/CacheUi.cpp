// Gridline - K-45: see CacheUi.h.
#include "CacheUi.h"
#include "../../third_party/imgui/imgui.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/ProjectInterop.h"
#include "../engine/Sample.h"
#include "../engine/TimeStretch.h"
#include "App.h"
#include "DevSignal.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::cacheui {

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kStartupWait = 4.0;  // seconds: the start of the app comes first
constexpr double kCloseGrace = 2.0;   // seconds: what the closed project still reads finishes first
constexpr double kQuitWait = 1.5;     // seconds: a quit never waits longer

// What the worker and the interface share; the worker owns a reference, so a worker left behind at
// quit never touches anything that has gone.
struct State {
  std::mutex mu;
  std::condition_variable wake;
  bool stop = false;
  bool working = false;
  // Requests.
  bool haveUses = false;
  archive::CopyHold nextUse;
  std::vector<std::string> nextFolders;
  std::vector<std::string> moreFolders;  // K-47: claimed on top of what is held
  bool sweepDue = false;
  Clock::time_point sweepAt{};
  bool clearWanted = false;
  bool measureWanted = false;
  // Results.
  std::atomic<uint64_t> bytes{0};
  std::atomic<bool> measured{false};
  std::atomic<bool> clearing{false};
  std::atomic<int> sweeps{0}, clears{0};
  archive::CacheReport lastSweep;
  archive::CacheReport lastClear;
  bool haveClear = false;
  // Only the worker's.
  archive::CopyHold use;
  std::vector<archive::CopyHold> folders;

  void work() {
    std::unique_lock<std::mutex> lk(mu);
    for (;;) {
      if (stop && !haveUses && !sweepDue) return;
      const bool sweepNow = sweepDue && (stop || Clock::now() >= sweepAt);
      if (!haveUses && moreFolders.empty() && !sweepNow && !clearWanted && !measureWanted) {
        if (sweepDue) wake.wait_until(lk, sweepAt);
        else wake.wait(lk);
        continue;
      }
      working = true;
      if (haveUses) {
        haveUses = false;
        archive::CopyHold newUse = std::move(nextUse);
        nextUse = nullptr;
        std::vector<std::string> names = std::move(nextFolders);
        nextFolders.clear();
        lk.unlock();
        // The new ones first, then the old ones go: a copy both use is never free in between.
        std::vector<archive::CopyHold> held;
        for (const auto& f : names)
          if (!newUse || newUse->folder != f)
            if (archive::CopyHold h = archive::holdCopy(f)) held.push_back(std::move(h));
        use = std::move(newUse);
        folders = std::move(held);
        lk.lock();
      } else if (!moreFolders.empty()) {
        std::vector<std::string> names = std::move(moreFolders);
        moreFolders.clear();
        lk.unlock();
        for (const auto& f : names) {
          bool have = use && use->folder == f;
          for (const auto& h : folders) have = have || h->folder == f;
          if (!have)
            if (archive::CopyHold h = archive::holdCopy(f)) folders.push_back(std::move(h));
        }
        lk.lock();
      } else if (clearWanted) {
        clearWanted = false;
        lk.unlock();
        const archive::CacheReport r = archive::clearCache();
        lk.lock();
        lastClear = r;
        haveClear = true;
        bytes = r.bytesLeft;
        measured = true;
        clearing = false;
        ++clears;
        logging::info("cache", "Clear cache: " + std::to_string(r.removed) + " removed, " + std::to_string(r.kept()) + " kept");
      } else if (sweepNow) {
        sweepDue = false;
        lk.unlock();
        const archive::CacheReport r = archive::sweepCache();
        lk.lock();
        lastSweep = r;
        bytes = r.bytesLeft;
        measured = true;
        ++sweeps;
        if (r.removed > 0) logging::info("cache", "Removed " + std::to_string(r.removed) + " unpacked project copies nothing needs");
      } else if (measureWanted) {
        measureWanted = false;
        lk.unlock();
        const uint64_t n = archive::cacheBytes();
        lk.lock();
        bytes = n;
        measured = true;
      }
      working = false;
    }
  }
};

struct Ui {
  std::shared_ptr<State> st = std::make_shared<State>();
  std::thread worker;
  bool startupDone = false;
  bool holdsCopy = false;  // the open project uses a copy (interface thread's view)
  int sweepsSeen = 0, clearsSeen = 0;
  double nextMeasure = 0;
  ImVec2 buttonAt{0, 0};  // (dev-input: where the button is)
};

Ui& ui_() {
  static Ui u;
  return u;
}

void ensureWorker() {
  Ui& u = ui_();
  if (u.worker.joinable()) return;
  u.st->stop = false;
  std::shared_ptr<State> s = u.st;
  u.worker = std::thread([s] { s->work(); });
}

void requestSweep(double inSeconds) {
  Ui& u = ui_();
  ensureWorker();
  {
    std::lock_guard<std::mutex> lk(u.st->mu);
    u.st->sweepDue = true;
    u.st->sweepAt = Clock::now() + std::chrono::milliseconds(static_cast<int>(inSeconds * 1000.0));
  }
  u.st->wake.notify_all();
}

void requestMeasure() {
  Ui& u = ui_();
  ensureWorker();
  {
    std::lock_guard<std::mutex> lk(u.st->mu);
    u.st->measureWanted = true;
  }
  u.st->wake.notify_all();
}

void startClear() {
  Ui& u = ui_();
  ensureWorker();
  {
    std::lock_guard<std::mutex> lk(u.st->mu);
    u.st->clearWanted = true;
    u.st->clearing = true;
  }
  u.st->wake.notify_all();
}

std::string sizeText(uint64_t bytes) {
  char b[48];
  const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
  const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
  if (gb >= 1.0) std::snprintf(b, sizeof b, "%.1f GB", gb);
  else if (mb >= 1.0 || bytes == 0) std::snprintf(b, sizeof b, "%.0f MB", mb);
  else std::snprintf(b, sizeof b, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  return b;
}

std::string fmt1(const char* format, int n) {
  char b[256];
  std::snprintf(b, sizeof b, i18n::tr(format), n);
  return b;
}

// What Clear cache did, in words: what went, then each reason something stayed.
std::vector<std::string> describe(const archive::CacheReport& r) {
  std::vector<std::string> out;
  if (r.removed > 0) {
    char b[256];
    std::snprintf(b, sizeof b, i18n::tr(i18n::N_("Cleared, %s freed.")), sizeText(r.freed).c_str());
    out.push_back(b);
  } else {
    out.push_back(i18n::tr(i18n::N_("Nothing could be cleared.")));
  }
  if (r.keptOpen) out.push_back(fmt1(i18n::N_("Kept %d: open in Gridline now."), r.keptOpen));
  if (r.keptSaved) out.push_back(fmt1(i18n::N_("Kept %d: a saved project uses its samples."), r.keptSaved));
  if (r.keptRecovery) out.push_back(fmt1(i18n::N_("Kept %d: a recent backup or crash recovery uses its samples."), r.keptRecovery));
  if (r.keptBusy) out.push_back(fmt1(i18n::N_("Kept %d: its files are in use, tried again at the next start."), r.keptBusy));
  if (r.keptUnknown) out.push_back(fmt1(i18n::N_("Kept %d: could not be checked, so it stays."), r.keptUnknown));
  return out;
}

}  // namespace

void skipStartupSweep() { ui_().startupDone = true; }

void tick(App& a) {
  Ui& u = ui_();
  if (const int n = u.st->sweeps.load(); n != u.sweepsSeen) {  // (dev-input's "waitfor" reads these here)
    u.sweepsSeen = n;
    devsig::reach("cache-swept");
  }
  if (const int n = u.st->clears.load(); n != u.clearsSeen) {
    u.clearsSeen = n;
    devsig::reach("cache-cleared");
  }
  if (u.startupDone || a.embedded) return;
  u.startupDone = true;
  requestSweep(kStartupWait);
}

void projectChanged(App& a, archive::CopyHold use) {
  if (a.embedded) return;
  Ui& u = ui_();
  std::vector<std::string> folders = archive::copiesUsedBy(a.doc.song());  // (no disk)
  const bool closing = u.holdsCopy;
  u.holdsCopy = use != nullptr || !folders.empty();
  if (!closing && !u.holdsCopy) return;  // no copy before or after: nothing to do
  ensureWorker();
  {
    std::lock_guard<std::mutex> lk(u.st->mu);
    u.st->haveUses = true;
    u.st->nextUse = std::move(use);
    u.st->nextFolders = std::move(folders);
    u.st->moreFolders.clear();
    u.st->sweepDue = true;
    u.st->sweepAt = Clock::now() + std::chrono::milliseconds(static_cast<int>(kCloseGrace * 1000.0));
  }
  u.st->wake.notify_all();
}

void projectUsesMore(App& a) {
  if (a.embedded) return;
  std::vector<std::string> folders = archive::copiesUsedBy(a.doc.song());  // (no disk)
  if (folders.empty()) return;
  Ui& u = ui_();
  u.holdsCopy = true;
  ensureWorker();
  {
    std::lock_guard<std::mutex> lk(u.st->mu);
    u.st->moreFolders = std::move(folders);
  }
  u.st->wake.notify_all();
}

void shutdown(App& a) {
  Ui& u = ui_();
  if (a.embedded || !u.worker.joinable()) return;
  std::shared_ptr<State> s = u.st;
  {
    std::lock_guard<std::mutex> lk(s->mu);
    s->stop = true;
    s->haveUses = true;
    s->nextUse = nullptr;
    s->nextFolders.clear();
    s->sweepDue = true;
    s->sweepAt = Clock::now();
    s->clearWanted = false;
    s->measureWanted = false;
  }
  s->wake.notify_all();
  // A big delete must not hold up the quit: a worker still busy is left behind (it owns its state and
  // the process is ending), and the next start finishes what it did not.
  const Clock::time_point until = Clock::now() + std::chrono::milliseconds(static_cast<int>(kQuitWait * 1000.0));
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(s->mu);
      if (!s->working && !s->haveUses && !s->sweepDue) break;
    }
    if (Clock::now() >= until) {
      u.worker.detach();
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  u.worker.join();
}

void settingsSection(App& a) {
  Ui& u = ui_();
  State& s = *u.st;
  const auto& P = theme::pal();
  if (a.now >= u.nextMeasure && !s.clearing.load()) {  // (while the page shows; on the worker)
    u.nextMeasure = a.now + 5.0;
    requestMeasure();
  }
  ui::BeginSection(i18n::tr("UNPACKED PROJECTS"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     i18n::tr("Zipped projects open from a copy on this computer, so their folders stay clean. A local project's copy is deleted when it closes; "
                              "a copy from a web or network drive stays for quick and offline opening."));
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS1));

  const uint64_t limit = archive::kCacheLimit;
  const uint64_t bytes = s.bytes.load();
  const bool known = s.measured.load();
  char line[128];
  if (known) {
    const std::string lim = sizeText(limit);
    std::snprintf(line, sizeof line, i18n::tr("%s of %s"), sizeText(bytes).c_str(), lim.c_str());
  } else {
    std::snprintf(line, sizeof line, "%s", i18n::tr("Measuring..."));
  }
  const float avail = ImGui::GetContentRegionAvail().x;
  const float buttonW = 112.0f;
  const float barW = std::max(60.0f, std::min(240.0f, avail - buttonW - ImGui::CalcTextSize(line).x - 3 * theme::kS3));
  // The size: a slim bar and the numbers, on one line with the button.
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.text), "%s", line);
  ImGui::SameLine(0, theme::kS3);
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = 6.0f, rowH = ImGui::GetFrameHeight();
    const ImVec2 p0(at.x, at.y + (rowH - h) * 0.5f), p1(at.x + barW, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, P.surface950, 3.0f);
    const float f = known ? std::clamp(static_cast<float>(static_cast<double>(bytes) / static_cast<double>(limit)), 0.0f, 1.0f) : 0.0f;
    if (f > 0.0f) dl->AddRectFilled(p0, ImVec2(p0.x + std::max(6.0f, barW * f), p1.y), P.accent, 3.0f);
    ImGui::Dummy(ImVec2(barW, rowH));
  }
  ImGui::SameLine(0, theme::kS3);
  const bool clearing = s.clearing.load();
  ImGui::BeginDisabled(clearing);
  if (ui::TextButton(clearing ? i18n::N_("Clearing...") : i18n::N_("Clear cache"), false, buttonW, i18n::N_("Delete every copy nothing needs")) && !clearing) {
    a.confirm.open = true;
    a.confirm.title = i18n::tr("Clear the unpacked project cache?");
    a.confirm.subtitle = i18n::tr("Copies nothing needs are deleted. Open projects, copies a saved project uses and copies a recent backup uses stay. "
                                  "Zipped projects unpack again when they open; those on a drive read it again.");
    a.confirm.okLabel = i18n::N_("Clear cache");
    a.confirm.action = [] { startClear(); };
  }
  u.buttonAt = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
  ImGui::EndDisabled();
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.haveClear) lines = describe(s.lastClear);
  }
  if (!lines.empty() && !clearing) {
    ImGui::PushTextWrapPos(0);
    for (const auto& l : lines) ImGui::TextColored(theme::vec(P.textFaint), "%s", l.c_str());
    ImGui::PopTextWrapPos();
  }
  ui::EndSection();
}

void devCommand(const std::vector<std::string>& args) {
  Ui& u = ui_();
  const std::string what = args.empty() ? "state" : args[0];
  if (what == "measure") {
    requestMeasure();
  } else if (what == "memory") {  // what holds the memory now
    size_t count = 0;
    const size_t samples = sampleCache().bytes(&count);
    const double gb = 1024.0 * 1024.0 * 1024.0;
    std::printf("memory peak %.2f GB samples %.2f GB in %zu renders %.2f GB pending %d\n", platform::peakRssMb() / 1024.0, static_cast<double>(samples) / gb, count,
                static_cast<double>(stretch::clipCacheBytes()) / gb, sampleCache().pending());
    std::fflush(stdout);
  } else if (what == "list") {  // what is in the cache now, by name
    std::error_code ec;
    for (std::filesystem::directory_iterator it(std::filesystem::u8path(archive::defaultCacheRoot()), ec), end; !ec && it != end; it.increment(ec))
      std::printf("cache entry %s\n", it->path().filename().u8string().c_str());
    std::printf("cache listed\n");
    std::fflush(stdout);
  } else {
    std::lock_guard<std::mutex> lk(u.st->mu);
    const archive::CacheReport& w = u.st->lastClear;
    std::printf("cache button %.0f %.0f\n", static_cast<double>(u.buttonAt.x), static_cast<double>(u.buttonAt.y));
    std::printf("cache bytes %llu measured %d sweeps %d clearing %d lastsweep removed %d drive %d open %d\n", static_cast<unsigned long long>(u.st->bytes.load()),
                u.st->measured.load() ? 1 : 0, u.st->sweeps.load(), u.st->clearing.load() ? 1 : 0, u.st->lastSweep.removed, u.st->lastSweep.keptDrive, u.st->lastSweep.keptOpen);
    if (u.st->haveClear)
      std::printf("cache clear removed %d open %d saved %d recovery %d busy %d unknown %d\n", w.removed, w.keptOpen, w.keptSaved, w.keptRecovery, w.keptBusy, w.keptUnknown);
    std::fflush(stdout);
  }
}

}  // namespace gl::cacheui
