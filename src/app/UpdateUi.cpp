// Gridline - J-89: update check popup and settings (see UpdateUi.h).
#include "UpdateUi.h"
#include "GridlineVersion.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/Platform.h"
#include "../core/Settings.h"
#include "../core/Update.h"
#include "../collab/VersionGate.h"
#include "../host/net/Http.h"
#include "App.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::updui {

using i18n::tr;
using theme::pal;
namespace fs = std::filesystem;

namespace {

constexpr float kDialogW = 500.0f;
constexpr float kNotesH = 170.0f;
constexpr int kStartupDelayFrames = 45;  // the window is up and drawn before anything is asked

int64_t wallNow() { return static_cast<int64_t>(std::time(nullptr)); }

// One check on a worker thread; shared with it, so the app may go away first.
struct Job {
  std::mutex mu;
  bool done = false;
  update::CheckResult result;
};

// One installer download on a worker thread.
struct Download {
  std::atomic<int64_t> got{0}, total{-1};
  std::atomic<bool> cancel{false}, done{false};
  std::mutex mu;
  update::InstallerResult result;
  std::string name;
  int64_t size = 0;
};

enum class Dialog { None, Checking, Result, Available };

struct State {
  Options opt;
  bool started = false;
  int frames = 0;
  bool startupDue = false;  // automatic check waiting for kStartupDelayFrames
  std::shared_ptr<Job> job;
  bool manual = false;       // the running job was asked for (reports every outcome)
  bool offerPending = false;  // a release found by the automatic check, waiting for a good moment
  Dialog dialog = Dialog::None;
  bool openPopup = false;
  double shownAt = 0;
  update::CheckResult shown;  // what the dialog shows
  std::string notes;          // plain-text excerpt of shown.release.body
  std::shared_ptr<Download> dl;
  std::string lastOutcome;    // Settings: result of the last check this session
  update::Fetcher fetch;
  update::Downloader download;
  std::string downloadDir;  // "" = the user's Downloads folder
  // J-111: the collaboration version gate (its own check, cached an hour) and the
  // "update required" mode of the dialog (no skip / remind: Update or Cancel).
  bool required = false;
  std::string requiredTitle;
  std::string requiredVersion;
  struct GateJob {
    std::mutex mu;
    bool done = false;
    collab::GateState result;
  };
  std::shared_ptr<GateJob> gateJob;
  collab::GateState gate;
  bool gateLoaded = false;
  std::string fakeLatest;  // GRIDLINE_FAKE_LATEST / --fake-update: a made-up newest release (no network)
};

State& S() {
  static State s;
  return s;
}

std::string formatTime(int64_t t) {
  if (t <= 0) return tr("Never");
  std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

std::string mb(int64_t bytes) {
  char b[32];
  if (bytes < 1024 * 1024) std::snprintf(b, sizeof b, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  else std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return b;
}

std::string outcomeText(const update::CheckResult& r) {
  char buf[256];
  switch (r.status) {
    case update::Status::UpToDate:
    case update::Status::NoRelease: std::snprintf(buf, sizeof buf, tr("You're up to date (%s)"), r.current.c_str()); return buf;
    case update::Status::Available:
      std::snprintf(buf, sizeof buf, tr("Gridline %s is available"), update::toString(r.release.version).c_str());
      return buf;
    case update::Status::Offline: return tr("Could not reach GitHub");
    case update::Status::RateLimited: return tr("GitHub is busy, try again later");
    case update::Status::Disabled: return tr("Update checks are turned off");
    case update::Status::Failed: break;
  }
  return tr("The check failed");
}

void startJob(App& a, bool manual) {
  State& s = S();
  if (s.job) {  // one at a time; a manual request takes over the running one
    s.manual = s.manual || manual;
    return;
  }
  s.manual = manual;
  auto job = std::make_shared<Job>();
  s.job = job;
  const bool beta = a.settings.getBool("update.beta");
  update::Fetcher fetch = s.fetch;
  std::thread([job, fetch, beta] {
    update::CheckResult r = update::check(fetch, GRIDLINE_VERSION, beta);
    std::lock_guard<std::mutex> lk(job->mu);
    job->result = std::move(r);
    job->done = true;
  }).detach();
}

void showDialog(Dialog d) {
  State& s = S();
  s.dialog = d;
  s.openPopup = true;
}

void offer(const update::CheckResult& r) {
  State& s = S();
  s.shown = r;
  s.notes = update::notesExcerpt(r.release.body);
  if (s.dl && !s.dl->done) s.dl->cancel = true;
  s.dl.reset();
  showDialog(Dialog::Available);
}

// The automatic offer waits: not while playing or recording, not over another prompt.
bool goodMoment(App& a) {
  if (a.engine.playing() || a.engine.recordingOn()) return false;
  if (a.confirmDiscard || ImGui::GetTopMostPopupModal() != nullptr) return false;
  return S().frames >= kStartupDelayFrames;
}

void collect(App& a) {
  State& s = S();
  if (!s.job) return;
  update::CheckResult r;
  {
    std::lock_guard<std::mutex> lk(s.job->mu);
    if (!s.job->done) return;
    r = s.job->result;
  }
  s.job.reset();
  const bool manual = s.manual;
  s.manual = false;
  s.lastOutcome = outcomeText(r);
  update::Prefs p = update::loadPrefs(a.settings);
  // Offline / failed checks are tried again at the next start; the others count as done.
  if (r.status != update::Status::Offline && r.status != update::Status::Failed) {
    p.lastCheck = wallNow();
    update::savePrefs(a.settings, p);
    a.settings.save(Settings::defaultPath());
  }
  if (manual || s.required) {
    if (r.status == update::Status::Available) {
      offer(r);
    } else {
      s.shown = r;
      showDialog(Dialog::Result);
    }
    return;
  }
  // Automatic: quiet unless there is something to offer.
  if (r.status == update::Status::Available && update::shouldOffer(p, r.release, wallNow())) {
    s.shown = r;
    s.offerPending = true;
  }
}

void openPage(const std::string& url) {
  State& s = S();
  if (s.opt.hidden || !s.opt.openUrl) {
    std::printf("open url: %s\n", url.c_str());
    std::fflush(stdout);
    return;
  }
  s.opt.openUrl(url);
}

void startDownload(const update::Asset& asset) {
  State& s = S();
  auto d = std::make_shared<Download>();
  d->name = asset.name;
  d->size = asset.size;
  s.dl = d;
  const update::Release rel = s.shown.release;
  const update::Asset as = asset;
  const update::Fetcher fetch = s.fetch;
  const update::Downloader download = s.download;
  const std::string dir = s.downloadDir;
  std::thread([d, rel, as, fetch, download, dir] {
    const std::string folder = dir.empty() ? http::downloadsFolder() : dir;
    update::InstallerResult r = update::downloadInstaller(
        fetch, download, rel, as, folder,
        [d](int64_t got, int64_t total) {
          d->got = got;
          d->total = total;
        },
        &d->cancel);
    {
      std::lock_guard<std::mutex> lk(d->mu);
      d->result = std::move(r);
    }
    d->done = true;
  }).detach();
}

void closeDialog() {
  State& s = S();
  s.dialog = Dialog::None;
  ImGui::CloseCurrentPopup();
}

void title(App& a, const std::string& t) {
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kDialogW);
  ImGui::TextUnformatted(t.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
}

void dimLine(const std::string& t, ImU32 col) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kDialogW);
  ImGui::TextColored(theme::vec(col), "%s", t.c_str());
  ImGui::PopTextWrapPos();
}

float buttonW(const char* label) { return std::max(96.0f, ImGui::CalcTextSize(tr(label)).x + 2 * theme::kS4); }

void rightAlign(float width) {
  const float x = ImGui::GetCursorPosX() + kDialogW - width;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
}

// ---- the dialog bodies

void drawAvailable(App& a) {
  State& s = S();
  const auto& P = pal();
  const update::Release& r = s.shown.release;
  const std::string v = update::toString(r.version);
  char buf[256];
  std::snprintf(buf, sizeof buf, tr("Gridline %s is available (you have %s)"), v.c_str(), GRIDLINE_VERSION);
  title(a, s.required && !s.requiredTitle.empty() ? s.requiredTitle : std::string(buf));
  if (s.required) dimLine(buf, P.textDim);
  std::string sub = r.prerelease || !r.version.pre.empty() ? tr("A beta version.") : std::string();
  if (r.published.size() >= 10) {
    std::snprintf(buf, sizeof buf, tr("Released %s."), r.published.substr(0, 10).c_str());
    sub += (sub.empty() ? "" : " ") + std::string(buf);
  }
  if (!sub.empty()) dimLine(sub, P.textDim);
  ImGui::Dummy(ImVec2(kDialogW, theme::kS1));
  // Release notes: plain text, scrollable.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, theme::kS2));
  ImGui::BeginChild("##notes", ImVec2(kDialogW, kNotesH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushTextWrapPos(0);
  if (s.notes.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("No release notes."));
  else ImGui::TextColored(theme::vec(P.text), "%s", s.notes.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  const update::Asset* asset = update::pickAsset(r, update::currentTarget());
  const bool canInstall = asset && update::checksumAsset(r);
  // Download progress / outcome.
  if (s.dl) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    const Download& d = *s.dl;
    if (!d.done) {
      const int64_t total = d.total > 0 ? d.total.load() : d.size;
      const float f = total > 0 ? std::clamp(static_cast<float>(static_cast<double>(d.got) / static_cast<double>(total)), 0.0f, 1.0f) : 0.0f;
      std::snprintf(buf, sizeof buf, tr("Downloading %s"), d.name.c_str());
      ui::TextFit(buf, kDialogW - 130, P.textDim);
      ImGui::SameLine(ImGui::GetCursorPosX() + kDialogW - 120);
      ImGui::PushFont(a.fontMono, theme::kFontLabel);
      const std::string amount = mb(d.got) + " / " + (total > 0 ? mb(total) : std::string("?"));
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, 120 - ImGui::CalcTextSize(amount.c_str()).x));
      ImGui::TextColored(theme::vec(P.textDim), "%s", amount.c_str());
      ImGui::PopFont();
      const ImVec2 q = ImGui::GetCursorScreenPos();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(q, ImVec2(q.x + kDialogW, q.y + 4), P.surface950, 2);
      dl->AddRectFilled(q, ImVec2(q.x + kDialogW * f, q.y + 4), P.accent, 2);
      ImGui::Dummy(ImVec2(kDialogW, 4));
    } else {
      update::InstallerResult res;
      {
        std::lock_guard<std::mutex> lk(s.dl->mu);
        res = s.dl->result;
      }
      if (res.ok) {
        dimLine(tr("Downloaded and checked against the release's SHA256SUMS. It is not installed: open it when you are ready."), P.textDim);
        // "<file>  in  <folder>": a long folder is cut, the file name stays readable.
        const fs::path saved = fs::u8path(res.path);
        std::string folder = saved.parent_path().u8string();
        const std::string home = platform::homeDir();
        if (home.size() > 1 && folder.rfind(home, 0) == 0) folder = "~" + folder.substr(home.size());
        const std::string shown = saved.filename().u8string() + "  " + tr("in") + "  " + folder;
        ImGui::PushFont(a.fontMono, theme::kFontLabel);
        ui::TextFit(shown.c_str(), kDialogW, P.text);  // cut: the full path on hover
        ImGui::PopFont();
      } else {
        std::snprintf(buf, sizeof buf, tr("The download did not finish: %s"), res.error.c_str());
        dimLine(buf, P.danger);
      }
    }
  } else if (!canInstall) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    dimLine(tr("Download opens the release page in your browser."), P.textFaint);
  }
  if (s.required && collab::installKind() != collab::InstallKind::Official) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    dimLine(tr("Or update from your software store when it arrives there."), P.textDim);
  }
  ImGui::Dummy(ImVec2(kDialogW, theme::kS2));

  // Footer: Skip on the left; Remind me later, the installer, Download on the right.
  const bool downloading = s.dl && !s.dl->done;
  const bool downloaded = s.dl && s.dl->done && s.dl->result.ok;
  const char* mid = downloading ? i18n::N_("Cancel download") : downloaded ? i18n::N_("Show in folder") : canInstall ? i18n::N_("Download installer") : nullptr;
  const float wSkip = buttonW("Skip this version"), wLater = buttonW("Remind me later"), wMid = mid ? buttonW(mid) : 0.0f,
              wGo = buttonW("Download");
  const float x0 = ImGui::GetCursorPosX();
  bool skip = false, later = false, cancelRequired = false;
  if (s.required) {
    // J-111: an update is required for collaboration: no skipping, just not now
    const float wCancel = buttonW("Cancel");
    const float right = wCancel + (mid ? wMid + theme::kS2 : 0.0f) + wGo + theme::kS2;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + kDialogW - right));
    cancelRequired = ui::TextButton("Cancel", false, wCancel) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  } else {
    skip = ui::TextButton("Skip this version", false, wSkip);
    ImGui::SameLine(0, 0);
    const float right = wLater + (mid ? wMid + theme::kS2 : 0.0f) + wGo + theme::kS2;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + theme::kS2, x0 + kDialogW - right));
    later = ui::TextButton("Remind me later", false, wLater) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  }
  bool midPressed = false;
  if (mid) {
    ImGui::SameLine(0, theme::kS2);
    midPressed = ui::TextButton(mid, false, wMid);
  }
  ImGui::SameLine(0, theme::kS2);
  bool go = ui::TextButton("Download", true, wGo) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

  update::Prefs p = update::loadPrefs(a.settings);
  if (cancelRequired) {
    if (s.dl && !s.dl->done) s.dl->cancel = true;
    s.required = false;
    closeDialog();
  } else if (skip) {
    update::skipVersion(p, r);
    update::savePrefs(a.settings, p);
    a.settings.save(Settings::defaultPath());
    if (s.dl) s.dl->cancel = true;
    closeDialog();
  } else if (later) {
    update::remindLater(p, wallNow());
    update::savePrefs(a.settings, p);
    a.settings.save(Settings::defaultPath());
    if (s.dl) s.dl->cancel = true;
    closeDialog();
  } else if (midPressed) {
    if (downloading) {
      s.dl->cancel = true;
    } else if (downloaded) {
      if (s.opt.hidden) std::printf("reveal: %s\n", s.dl->result.path.c_str());
      else http::reveal(s.dl->result.path);
    } else if (canInstall) {
      startDownload(*asset);
    }
  } else if (go) {
    openPage(r.htmlUrl.empty() ? std::string(update::kReleasesPage) : r.htmlUrl);
    if (s.dl) s.dl->cancel = true;
    closeDialog();
  }
  // A finished download is shown in the file manager once.
  static const Download* revealed = nullptr;
  if (s.dl && s.dl->done && s.dl->result.ok && revealed != s.dl.get()) {
    revealed = s.dl.get();
    if (s.opt.hidden) std::printf("reveal: %s\n", s.dl->result.path.c_str());
    else http::reveal(s.dl->result.path);
  }
}

void drawResult(App& a) {
  State& s = S();
  const auto& P = pal();
  const update::CheckResult& r = s.shown;
  const bool ok = r.status == update::Status::UpToDate || r.status == update::Status::NoRelease;
  if (s.required) {
    // J-111: the newest release could not be fetched just now: point at the releases page
    title(a, s.requiredTitle);
    dimLine(tr("Couldn't check for updates right now. The releases page has every download."), P.textDim);
    if (collab::installKind() != collab::InstallKind::Official)
      dimLine(tr("Or update from your software store when it arrives there."), P.textDim);
    ImGui::Dummy(ImVec2(kDialogW, theme::kS1 + theme::kS2));
    const float wc = buttonW("Cancel"), wp2 = buttonW("Open the releases page");
    rightAlign(wc + wp2 + theme::kS2);
    const bool cancel = ui::TextButton("Cancel", false, wc) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Open the releases page", true, wp2)) {
      openPage(update::kReleasesPage);
      s.required = false;
      closeDialog();
    } else if (cancel) {
      s.required = false;
      closeDialog();
    }
    return;
  }
  title(a, ok ? outcomeText(r) : std::string(tr("Could not check for updates")));
  switch (r.status) {
    case update::Status::NoRelease: dimLine(tr("No release has been published on GitHub yet."), P.textDim); break;
    case update::Status::UpToDate: dimLine(tr("This is the newest version."), P.textDim); break;
    case update::Status::Offline: dimLine(tr("GitHub could not be reached. Check the internet connection and try again."), P.textDim); break;
    case update::Status::RateLimited: dimLine(tr("GitHub is limiting requests right now. Try again in a while."), P.textDim); break;
    case update::Status::Disabled: dimLine(tr("Update checks are turned off for this run."), P.textDim); break;
    default: dimLine(r.detail.empty() ? std::string(tr("Something went wrong.")) : r.detail, P.textDim); break;
  }
  ImGui::Dummy(ImVec2(kDialogW, theme::kS1 + theme::kS2));
  const float w = buttonW("OK");
  rightAlign(w);
  if (ui::TextButton("OK", true, w) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
    closeDialog();
}

void drawChecking(App& a) {
  State& s = S();
  const auto& P = pal();
  title(a, tr("Checking for updates..."));
  const int dots = static_cast<int>(ImGui::GetTime() * 3.0) % 4;
  dimLine(std::string(tr("Asking GitHub for the latest release")) + std::string(static_cast<size_t>(dots), '.'), P.textDim);
  ImGui::Dummy(ImVec2(kDialogW, theme::kS1 + theme::kS2));
  const float w = buttonW("Cancel");
  rightAlign(w);
  if (ui::TextButton("Cancel", false, w) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    s.manual = false;  // the result arrives quietly
    closeDialog();
  }
}

}  // namespace

void start(App& a, const Options& o) {
  State& s = S();
  s = State{};
  s.opt = o;
  s.started = true;
  s.fetch = http::fetcher();
  s.download = http::downloader();
  if (const char* fl = std::getenv("GRIDLINE_FAKE_LATEST"); fl && *fl) s.fakeLatest = fl;  // J-111 screenshots
  if (!o.fakeVersion.empty()) s.fakeLatest = o.fakeVersion;
  if (!s.fakeLatest.empty() && o.fakeVersion.empty()) {
    const update::Release r = update::fakeRelease(s.fakeLatest);
    s.fetch = update::fakeFetcher(r);
    s.download = update::fakeDownloader();
    s.downloadDir = (fs::path(a.configDir()) / "dev-downloads").string();
  }
  if (!o.fakeVersion.empty()) {  // dev: a made-up release, no network
    const update::Release r = update::fakeRelease(o.fakeVersion);
    s.fetch = update::fakeFetcher(r);
    s.download = update::fakeDownloader();
    s.downloadDir = (fs::path(a.configDir()) / "dev-downloads").string();
    update::CheckResult c = update::check(s.fetch, GRIDLINE_VERSION, true);
    if (c.status != update::Status::Available) {  // e.g. --fake-update 0.1.0: show what the check says
      s.shown = c;
      showDialog(Dialog::Result);
    } else {
      s.shown = c;
      s.offerPending = true;
    }
    s.lastOutcome = outcomeText(c);
    return;
  }
  if (o.automated || o.disabled) return;  // automated runs never touch the network
  s.startupDue = update::dueAtStartup(update::loadPrefs(a.settings), wallNow(), update::disabledByEnvironment());
}

bool started() { return S().started; }

void checkNow(App& a) {
  State& s = S();
  if (!s.started) return;
  if ((s.opt.disabled || s.opt.automated) && s.opt.fakeVersion.empty()) {
    s.shown = update::CheckResult{};
    s.shown.status = update::Status::Disabled;
    s.shown.current = GRIDLINE_VERSION;
    showDialog(Dialog::Result);
    return;
  }
  startJob(a, true);
  if (s.job) showDialog(Dialog::Checking);
}

void draw(App& a) {
  State& s = S();
  if (!s.started) return;
  ++s.frames;
  if (s.startupDue && s.frames >= kStartupDelayFrames) {
    s.startupDue = false;
    startJob(a, false);
  }
  collect(a);
  if (s.offerPending && s.dialog == Dialog::None && goodMoment(a)) {
    s.offerPending = false;
    offer(s.shown);
  }
  if (s.dialog == Dialog::None) return;
  if (s.openPopup) {
    s.openPopup = false;
    if (!ImGui::IsPopupOpen("##update")) {
      ImGui::OpenPopup("##update");
      s.shownAt = ImGui::GetTime();
    }
  }
  // Fades in like the other confirmations (J-77).
  const float appear = std::clamp(static_cast<float>((ImGui::GetTime() - s.shownAt) / 0.18), 0.0f, 1.0f);
  const float ease = 1.0f - (1.0f - appear) * (1.0f - appear);
  ui::PlaceNextDialog(ImVec2(kDialogW + 2 * theme::kS6, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.15f + 0.85f * ease);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg);
  dim.w *= ease;
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dim);
  const bool open = ImGui::BeginPopupModal("##update", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!open) {
    s.dialog = Dialog::None;
    return;
  }
  switch (s.dialog) {
    case Dialog::Checking: drawChecking(a); break;
    case Dialog::Result: drawResult(a); break;
    case Dialog::Available: drawAvailable(a); break;
    case Dialog::None: ImGui::CloseCurrentPopup(); break;
  }
  ImGui::EndPopup();
}

// ---- J-111: collaboration version gate ----------------------------------------
const collab::GateState& gate(App& a) {
  State& s = S();
  const int64_t now = wallNow();
  collab::VersionGate g(a.settings, GRIDLINE_VERSION);
  if (!s.gateLoaded) {
    s.gateLoaded = true;
    s.gate = g.state(now);
  }
  if (s.gateJob) {
    std::lock_guard<std::mutex> lk(s.gateJob->mu);
    if (s.gateJob->done) {
      s.gate = s.gateJob->result;
      g.store(s.gate);
      a.settings.save(Settings::defaultPath());
      s.gateJob.reset();
    }
  }
  const bool network = s.started && (!s.opt.automated || !s.fakeLatest.empty()) && (!s.opt.disabled || !s.fakeLatest.empty());
  // its own check: the popup's "skip" / "remind later" / automatic-off do not apply here
  if (network && !s.gateJob && (g.due(now) || !s.fakeLatest.empty()) && s.gate.checkedAt != now) {
    auto job = std::make_shared<State::GateJob>();
    s.gateJob = job;
    const update::Fetcher fetch = s.fetch;
    const std::string cur = GRIDLINE_VERSION;
    Settings* st = &a.settings;
    collab::GateState cached = s.gate;
    std::thread([job, fetch, cur, now, cached, st] {
      // the check runs on a copy of the cached decision (no settings access off the UI thread)
      (void)st;
      const update::HttpResult r = fetch(update::kListUrl);
      collab::GateState out = cached;
      std::vector<update::Release> rel;
      std::string err;
      if (r.offline || r.status == 0) {
        out.couldNotCheck = true;
        out.detail = "offline";
      } else if (r.status == 403 || r.status == 429) {
        out.couldNotCheck = true;
        out.detail = "rate limited";
      } else if (r.status != 200 || !update::parseReleases(r.body, rel, &err)) {
        out.couldNotCheck = true;
        out.detail = "failed";
      } else {
        out = collab::evaluate(rel, cur, update::currentTarget());
        out.checkedAt = now;
      }
      std::lock_guard<std::mutex> lk(job->mu);
      job->result = out;
      job->done = true;
    }).detach();
    s.gate.checkedAt = now;  // not again this second
  }
  if (!network) s.gate.couldNotCheck = s.gate.latest.empty();
  return s.gate;
}

bool gateChecking() { return S().gateJob != nullptr; }

void showRequired(App& a, const std::string& titleText, const std::string& version) {
  State& s = S();
  if (!s.started) return;
  s.required = true;
  s.requiredTitle = titleText;
  s.requiredVersion = version;
  if ((s.opt.disabled || s.opt.automated) && s.fakeLatest.empty()) {
    s.shown = update::CheckResult{};
    s.shown.status = update::Status::Offline;
    s.shown.current = GRIDLINE_VERSION;
    showDialog(Dialog::Result);
    return;
  }
  startJob(a, true);
  if (s.job) showDialog(Dialog::Checking);
}

void settingsSection(App& a) {
  State& s = S();
  if (!s.started) return;
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection(tr("UPDATES"));
  bool autoCheck = st.getBool("update.auto");
  if (ImGui::Checkbox(tr("Check for updates automatically"), &autoCheck)) st.set("update.auto", autoCheck);
  ui::Hint("Once a day at startup, Gridline asks GitHub for the newest release. Nothing else is sent.");
  bool beta = st.getBool("update.beta");
  if (ImGui::Checkbox(tr("Include beta versions"), &beta)) st.set("update.beta", beta);
  ui::Hint("Also offer prereleases (may be less stable)");
  const update::Prefs p = update::loadPrefs(st);
  std::string last = std::string(tr("Last checked: ")) + formatTime(p.lastCheck);
  if (!s.lastOutcome.empty()) last += "  \xC2\xB7  " + s.lastOutcome;
  ImGui::AlignTextToFramePadding();
  ui::TextFit(last.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x - 120), P.textDim);
  ImGui::SameLine();
  if (s.job) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Checking..."));
  } else if (ui::TextButton("Check now", false, 0, "Ask GitHub now and show the result")) {
    checkNow(a);
  }
  if (!p.skipVersion.empty()) {
    char buf[128];
    std::snprintf(buf, sizeof buf, tr("Skipped version: %s"), p.skipVersion.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textFaint), "%s", buf);
    ImGui::SameLine();
    if (ui::TextButton("Offer it again", false)) {
      update::Prefs q = p;
      q.skipVersion.clear();
      q.remindAfter = 0;
      update::savePrefs(st, q);
    }
  }
  if (s.opt.disabled && s.opt.fakeVersion.empty())
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Turned off: --no-update-check, GRIDLINE_NO_UPDATE_CHECK or CI is set."));
  ui::EndSection();
}

void shutdown() {
  State& s = S();
  if (!s.dl || s.dl->done) return;
  s.dl->cancel = true;
  for (int i = 0; i < 40 && !s.dl->done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

}  // namespace gl::updui
