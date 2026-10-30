// Gridline - J-192: Import from URL popup, settings and hooks (see UrlImportUi.h).
#include "UrlImportUi.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/MediaTools.h"
#include "../core/Platform.h"
#include "../core/PluginInfo.h"
#include "../core/Settings.h"
#include "../core/UrlImport.h"
#include "../engine/ClipOps.h"
#include "../engine/Sample.h"
#include "../host/net/Http.h"
#include "App.h"
#include "DevSignal.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"

namespace fs = std::filesystem;

namespace gl::urlimportui {

using i18n::tr;
using theme::pal;
namespace mt = gl::mediatools;
namespace ul = gl::urlimport;

namespace {

constexpr float kW = 520.0f;  // content width
constexpr int kMaxListRows = 6;

enum class Screen { Closed, Entry, Setup, Fetching, Preview, Importing, Error };

// Installing the missing tools on a worker thread.
struct SetupJob {
  std::atomic<int64_t> got{0}, total{-1};
  std::atomic<int> step{0};  // 0 yt-dlp, 1 ffmpeg
  std::atomic<bool> cancel{false}, done{false};
  std::mutex mu;
  std::string error;
};

// The daily yt-dlp update on a worker thread.
struct UpdateJob {
  std::atomic<bool> done{false};
  std::mutex mu;
  mt::UpdateResult result;
};

struct State {
  Screen screen = Screen::Closed;
  bool openPopup = false;
  double shownAt = 0;
  char url[4096] = {};
  bool focusUrl = false;
  bool reloadUrl = false;  // the field shows `url` again (set from code while it is being edited)
  bool clipboardChecked = false;
  std::string urlHint;  // why the link is not accepted
  // tools
  mt::Tool yt, ff;
  std::shared_ptr<SetupJob> setup;
  bool fetchAfterSetup = false;
  // jobs
  std::shared_ptr<ul::Job> job;
  std::string workDir;
  ul::Info info;
  std::vector<char> picked;  // playlist items to import
  int playlistMode = 0;      // 0 first item, 1 the items picked
  bool trimOn = false;
  char from[16] = "0:00", to[16] = "";
  std::string error, lead;
  Screen errorBack = Screen::Entry;
  // update
  std::shared_ptr<UpdateJob> upd;
  std::string updStatus;
  bool updChecked = false;
  // dev
  std::string fakeTool;  // GRIDLINE_URLIMPORT_FAKE_NET: offline tool setup (screenshots, tests)
  int fakeStepMs = 0;
  bool automated = false;
  std::string devClipboard;
  bool devClipboardSet = false;
};

State& S() {
  static State s;
  return s;
}

int64_t wallNow() { return static_cast<int64_t>(std::time(nullptr)); }

std::string mb(int64_t bytes) {
  char b[32];
  const double v = static_cast<double>(std::max<int64_t>(0, bytes));
  if (v < 1024.0 * 1024.0) std::snprintf(b, sizeof b, "%.0f KB", v / 1024.0);
  else std::snprintf(b, sizeof b, "%.1f MB", v / (1024.0 * 1024.0));
  return b;
}

bool usingFake() { return !S().fakeTool.empty(); }

mt::Search search() {
  if (usingFake()) return mt::Search{mt::installDir(), "", false};  // offline set-up: nothing else counts
  return mt::Search{mt::installDir(), mt::bundledDir(), true};
}

mt::Net net() {
  State& s = S();
  if (usingFake()) return mt::fakeNet(s.fakeTool, s.fakeStepMs);
  return mt::Net{http::fetcher(), http::downloader()};
}

std::string clipboardText() {
  State& s = S();
  if (s.devClipboardSet) return s.devClipboard;
  if (s.automated) return "";  // scripted runs never read the computer's clipboard
  const char* c = ImGui::GetClipboardText();
  return c ? std::string(c) : std::string();
}

void setUrl(const std::string& u) {
  State& s = S();
  std::snprintf(s.url, sizeof s.url, "%s", u.c_str());
  s.urlHint.clear();
  s.reloadUrl = true;
}

std::string workRoot() {
  std::error_code ec;
  fs::path t = fs::temp_directory_path(ec);
  if (ec) t = fs::u8path(platform::homeDir());
  return (t / ("gridline-url-import-" + std::to_string(platform::processId()))).u8string();
}

void showError(const std::string& error, const std::string& lead, Screen back) {
  State& s = S();
  s.error = error;
  s.lead = lead;
  s.errorBack = back;
  s.screen = Screen::Error;
  devsig::reach("urlimport-error");
}

void stopJobs() {
  State& s = S();
  if (s.job && !s.job->done) s.job->cancel = true;
  if (s.setup && !s.setup->done) s.setup->cancel = true;
}

void close() {
  State& s = S();
  stopJobs();
  s.job.reset();
  s.screen = Screen::Closed;
  ImGui::CloseCurrentPopup();
}

// ---- tools ----------------------------------------------------------------

void locate() {
  State& s = S();
  const mt::Search se = search();
  if (usingFake()) {
    s.yt = mt::locateYtdlp(se);
    s.ff = mt::locateFfmpeg(se);
    return;
  }
  s.yt = mt::locateYtdlp(se);
  s.ff = mt::locateFfmpeg(se);
}

void startSetup() {
  State& s = S();
  auto job = std::make_shared<SetupJob>();
  s.setup = job;
  s.screen = Screen::Setup;
  devsig::reach("urlimport-setup");
  const bool needYt = !s.yt.found(), needFf = !s.ff.found();
  const mt::Net n = net();
  const std::string dir = mt::installDir();
  std::thread([job, needYt, needFf, n, dir] {
    auto progress = [job](int64_t got, int64_t total) {
      job->got = got;
      job->total = total;
    };
    std::string err;
    if (needYt) {
      job->step = 0;
      const mt::InstallResult r = mt::installYtdlp(n, dir, progress, &job->cancel);
      if (!r.ok) err = "yt-dlp: " + r.error;
    }
    if (err.empty() && needFf && !job->cancel) {
      job->step = 1;
      job->got = 0;
      job->total = -1;
      const mt::InstallResult r = mt::installFfmpeg(n, dir, progress, &job->cancel, update::currentTarget());
      if (!r.ok) err = "ffmpeg: " + r.error;
    }
    {
      std::lock_guard<std::mutex> lk(job->mu);
      job->error = err;
    }
    job->done = true;
  }).detach();
}

// The tools are there (true), or are being set up / cannot be (false; the popup shows which).
bool toolsReady(App& a) {
  State& s = S();
  locate();
  if (s.yt.found() && s.ff.found()) return true;
  const bool overridden = std::getenv("GRIDLINE_YTDLP") || std::getenv("GRIDLINE_FFMPEG");
  if (overridden) {
    showError(std::string("GRIDLINE_YTDLP / GRIDLINE_FFMPEG do not point at a program: ") + (s.yt.found() ? "ffmpeg" : "yt-dlp"),
              tr("The downloader is not available."), Screen::Entry);
    return false;
  }
  if (s.automated && !usingFake()) {  // scripted runs never touch the network
    showError("yt-dlp or ffmpeg is missing and this run does not download", tr("The downloader is not available."), Screen::Entry);
    return false;
  }
  (void)a;
  startSetup();
  return false;
}

void maybeUpdate(App& a) {
  State& s = S();
  if (s.updChecked || s.upd || s.automated) return;
  s.updChecked = true;
  if (usingFake()) return;
  if (s.yt.from != mt::From::Installed && s.yt.from != mt::From::Bundled) return;  // the system's copy is the system's business
  const int64_t last = static_cast<int64_t>(a.settings.getNumber("import.urlLastUpdateCheck"));
  if (!mt::updateDue(a.settings.getBool("import.urlAutoUpdate"), last, wallNow())) return;
  a.settings.set("import.urlLastUpdateCheck", static_cast<double>(wallNow()));
  const std::string cur = mt::recordedVersion(fs::u8path(s.yt.path).parent_path().u8string());
  auto job = std::make_shared<UpdateJob>();
  s.upd = job;
  const mt::Net n = net();
  const std::string dir = mt::installDir();
  std::thread([job, n, dir, cur] {
    mt::UpdateResult r = mt::updateYtdlp(n, dir, cur, nullptr);
    std::lock_guard<std::mutex> lk(job->mu);
    job->result = std::move(r);
    job->done = true;
  }).detach();
}

void collectUpdate() {
  State& s = S();
  if (!s.upd || !s.upd->done) return;
  mt::UpdateResult r;
  {
    std::lock_guard<std::mutex> lk(s.upd->mu);
    r = s.upd->result;
  }
  s.upd.reset();
  if (r.updated) s.updStatus = std::string(tr("Updated yt-dlp to ")) + r.version;
  else if (r.checked) s.updStatus = std::string(tr("yt-dlp is up to date")) + (r.version.empty() ? "" : " (" + r.version + ")");
  else s.updStatus = std::string(tr("Could not check for a newer yt-dlp: ")) + r.error;
  if (r.updated && s.screen != Screen::Importing && s.screen != Screen::Fetching) locate();
}

// ---- actions ----------------------------------------------------------------

void fetch(App& a) {
  State& s = S();
  std::string why;
  std::string u = s.url;
  while (!u.empty() && (u.back() == ' ' || u.back() == '\n')) u.pop_back();
  while (!u.empty() && u.front() == ' ') u.erase(u.begin());
  setUrl(u);
  if (!ul::validUrl(u, &why)) {
    s.urlHint = tr(why.c_str());
    return;
  }
  if (!toolsReady(a)) {
    s.fetchAfterSetup = s.screen == Screen::Setup;
    return;
  }
  maybeUpdate(a);
  s.workDir = workRoot();
  std::error_code ec;
  fs::remove_all(fs::u8path(s.workDir) / "info", ec);
  s.job = ul::startFetch(ul::Tools{s.yt.argv(), s.ff.path}, u, (fs::u8path(s.workDir) / "info").u8string());
  s.screen = Screen::Fetching;
}

void paste() {
  State& s = S();
  const std::string t = clipboardText();
  const std::string l = ul::linkInText(t);
  if (!l.empty()) setUrl(l);
  else s.urlHint = tr("The clipboard holds no link");
  s.focusUrl = true;
}

std::vector<ul::Request> requests() {
  State& s = S();
  std::vector<ul::Request> out;
  ul::Trim trim;
  if (s.trimOn && !s.info.playlist) {
    trim.start = std::max(0.0, ul::parseTime(s.from));
    const double e = ul::parseTime(s.to);
    trim.end = e > trim.start ? e : -1;
  }
  if (!s.info.playlist) {
    ul::Request r;
    r.item.title = s.info.title;
    r.item.uploader = s.info.uploader;
    r.item.url = s.info.url;
    r.item.duration = s.info.duration;
    r.site = s.info.site;
    r.trim = trim;
    out.push_back(r);
    return out;
  }
  for (size_t i = 0; i < s.info.entries.size(); ++i) {
    if (s.playlistMode == 0 ? i != 0 : !(i < s.picked.size() && s.picked[i])) continue;
    ul::Request r;
    r.item = s.info.entries[i];
    if (r.item.uploader.empty()) r.item.uploader = s.info.uploader;
    r.site = s.info.site;
    out.push_back(r);
  }
  return out;
}

void startImport(App& a) {
  State& s = S();
  const auto reqs = requests();
  if (reqs.empty()) return;
  if (!toolsReady(a)) return;
  s.job = ul::startImport(ul::Tools{s.yt.argv(), s.ff.path}, reqs, (fs::u8path(s.workDir) / "items").u8string());
  s.screen = Screen::Importing;
  devsig::reach("urlimport-importing");
}

int freeTrack(const Song& x, const std::vector<int>& taken) {
  for (int t = 0; t < kMaxPlaylistTracks; ++t) {
    bool used = std::find(taken.begin(), taken.end(), t) != taken.end();
    for (const auto& c : x.playlist) used = used || c.track == t;
    if (!used) return t;
  }
  return 0;
}

bool moveFile(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec) return true;
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);  // another volume
  if (ec) return false;
  fs::remove(from, ec);
  return true;
}

// Results into the project: the files into Imported/, one undo step for the clips.
void finishImport(App& a, const std::vector<ul::Result>& results) {
  const std::string dir = importedDir(a);
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  struct Placed {
    std::string path, name;
  };
  std::vector<Placed> placed;
  for (const auto& r : results) {
    const std::string title = r.request.item.title.empty() ? std::string("Imported audio") : r.request.item.title;
    const std::string dest = ul::uniquePath(dir, ul::safeFileName(title), ".wav");
    if (!moveFile(fs::u8path(r.wav), fs::u8path(dest))) {
      a.notify("Could not save " + fs::u8path(dest).filename().u8string(), true, dest);
      continue;
    }
    ul::Source src;
    src.title = title;
    src.uploader = r.request.item.uploader;
    src.site = r.request.site.empty() ? ul::hostOf(r.request.item.url) : r.request.site;
    src.url = r.request.item.url;
    src.duration = r.request.item.duration;
    ul::writeSource(dest, src);
    placed.push_back({dest, title});
  }
  fs::remove_all(fs::u8path(S().workDir) / "items", ec);
  if (placed.empty()) return;
  const bool clips = static_cast<int>(a.settings.getNumber("import.urlPlacement")) == 0;
  if (clips) {
    for (const auto& p : placed) sampleCache().get(p.path);  // decoded before the edit (clip lengths)
    const Tick at = static_cast<Tick>(std::max(0.0, a.engine.mode() == PlayMode::Song ? a.engine.position() : 0.0));
    a.doc.editSong(placed.size() == 1 ? "import from URL" : "import from URL (" + std::to_string(placed.size()) + " items)", [&](Song& x) {
      std::vector<int> taken;
      for (const auto& p : placed) {
        Channel c;
        c.id = x.allocateId();
        c.name = p.name;
        c.color = 0x8A9BA8;
        c.plugin = defaultPluginState("gl.sampler");
        c.plugin.data = p.path;
        c.mixerTrack = x.masterId();
        x.channels.push_back(c);
        Clip k;
        k.id = x.allocateId();
        k.kind = ClipKind::Audio;
        k.source = c.id;
        k.track = freeTrack(x, taken);
        taken.push_back(k.track);
        k.start = at;
        if (static_cast<int>(x.tracks.size()) <= k.track) x.tracks.resize(static_cast<size_t>(k.track) + 1);
        x.playlist.push_back(k);
        x.playlist.back().length = audioClipNaturalLength(x, x.playlist.back());
      }
    });
    a.showPlaylist = true;
    a.syncEngine();
  }
  a.showBrowser = true;
  const std::string first = fs::u8path(placed.front().path).filename().u8string();
  if (placed.size() == 1)
    a.notify(clips ? "Imported " + first + " as an audio clip" : "Imported " + first + " into the browser (Imported)", false, placed.front().path);
  else
    a.notify("Imported " + std::to_string(placed.size()) + " files" + (clips ? " as audio clips" : " into the browser (Imported)"), false, dir);
  a.uiSound(uisound::Event::Import);
}

void poll(App& a) {
  State& s = S();
  collectUpdate();
  if (s.setup && s.setup->done) {
    std::string err;
    {
      std::lock_guard<std::mutex> lk(s.setup->mu);
      err = s.setup->error;
    }
    const bool cancelled = s.setup->cancel.load();
    s.setup.reset();
    locate();
    if (cancelled) {
      s.screen = Screen::Entry;
    } else if (!err.empty() || !s.yt.found() || !s.ff.found()) {
      showError(err.empty() ? std::string("the tools could not be set up") : err, tr("The downloader could not be set up."), Screen::Entry);
    } else {
      s.screen = Screen::Entry;
      if (s.fetchAfterSetup) {
        s.fetchAfterSetup = false;
        fetch(a);
      }
    }
  }
  if (!s.job || !s.job->done) return;
  const ul::Phase ph = s.job->get();
  std::vector<ul::Result> results;
  std::string error, lead;
  ul::Info info;
  {
    std::lock_guard<std::mutex> lk(s.job->mu);
    results = s.job->results;
    error = s.job->error;
    lead = s.job->lead;
    info = s.job->info;
  }
  const bool fetching = s.screen == Screen::Fetching;
  s.job.reset();
  if (fetching) {
    if (ph == ul::Phase::Done) {
      s.info = info;
      s.picked.assign(s.info.entries.size(), 1);
      s.playlistMode = 0;
      s.trimOn = false;
      std::snprintf(s.from, sizeof s.from, "0:00");
      std::snprintf(s.to, sizeof s.to, "%s", s.info.duration > 0 ? ul::formatTime(s.info.duration).c_str() : "");
      s.screen = Screen::Preview;
      devsig::reach("urlimport-preview");
    } else if (ph == ul::Phase::Cancelled) {
      s.screen = Screen::Entry;
    } else {
      showError(error, lead, Screen::Entry);
    }
    return;
  }
  if (!results.empty()) finishImport(a, results);
  if (ph == ul::Phase::Done) {
    devsig::reach("urlimport-done");
    close();
  } else if (ph == ul::Phase::Cancelled) {
    s.screen = Screen::Preview;
    devsig::reach("urlimport-cancelled");
  } else {
    showError(error, lead, Screen::Preview);
  }
}

// ---- drawing ----------------------------------------------------------------

void title(App& a, const char* t) {
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(t);
  ImGui::PopFont();
}

void wrapped(const std::string& t, ImU32 col) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
  ImGui::TextColored(theme::vec(col), "%s", t.c_str());
  ImGui::PopTextWrapPos();
}

float buttonW(const char* label) { return std::max(88.0f, ImGui::CalcTextSize(tr(label)).x + 2 * theme::kS4); }

void rightAlign(float width) { ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + kW - width)); }

void bar(float f, bool indeterminate) {
  const auto& P = pal();
  const ImVec2 q = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(q, ImVec2(q.x + kW, q.y + 4), P.surface950, 2);
  if (indeterminate) {  // a moving segment
    const float t = static_cast<float>(std::fmod(ImGui::GetTime() * 0.6, 1.0));
    const float x0 = q.x + (kW + 120) * t - 120;
    dl->AddRectFilled(ImVec2(std::max(q.x, x0), q.y), ImVec2(std::min(q.x + kW, x0 + 120), q.y + 4), P.accent, 2);
  } else {
    dl->AddRectFilled(q, ImVec2(q.x + kW * std::clamp(f, 0.0f, 1.0f), q.y + 4), P.accent, 2);
  }
  ImGui::Dummy(ImVec2(kW, 4));
}

void rightsNote() {
  ImGui::Dummy(ImVec2(0, theme::kS1));
  wrapped(tr("Import only media you have the rights to use. Gridline does not sign in to sites or get around copy protection."), pal().textFaint);
}

void urlRow(App& a, bool enabled) {
  State& s = S();
  const float pasteW = buttonW("Paste"), fetchW = buttonW("Fetch");
  ImGui::BeginDisabled(!enabled);
  ImGui::SetNextItemWidth(kW - pasteW - fetchW - 2 * theme::kS2);
  if (s.focusUrl) {
    ImGui::SetKeyboardFocusHere();
    s.focusUrl = false;
  }
  if (s.reloadUrl) {  // an active field keeps its own copy of the text: hand it the new one
    s.reloadUrl = false;
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetID("##url"))) st->ReloadUserBufAndMoveToEnd();
  }
  const bool enter = ImGui::InputTextWithHint("##url", "https://...", s.url, sizeof s.url, ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemEdited()) s.urlHint.clear();
  ui::FocusRing();
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Paste", false, pasteW, "Paste a link from the clipboard")) paste();
  ImGui::SameLine(0, theme::kS2);
  const bool go = ui::TextButton("Fetch", true, fetchW, "Read the link: title, length, playlist items");
  ImGui::EndDisabled();
  if (enabled && (enter || go)) fetch(a);
}

void drawEntry(App& a) {
  State& s = S();
  const auto& P = pal();
  title(a, tr("Import from URL"));
  wrapped(tr("A link to a video or audio page (most video and music sites). Its best audio is imported as a lossless WAV."), P.textDim);
  ImGui::Dummy(ImVec2(0, theme::kS2));
  urlRow(a, true);
  if (!s.urlHint.empty()) wrapped(s.urlHint, P.danger);
  rightsNote();
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  const float w = buttonW("Close");
  rightAlign(w);
  if (ui::TextButton("Close", false, w) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
}

void drawSetup(App& a) {
  State& s = S();
  const auto& P = pal();
  title(a, tr("Import from URL"));
  ImGui::Dummy(ImVec2(0, theme::kS2));
  urlRow(a, false);
  ImGui::Dummy(ImVec2(0, theme::kS2));
  const SetupJob& j = *s.setup;
  const int64_t total = j.total.load(), got = j.got.load();
  std::string line = tr("Setting up the downloader (once)...");
  line += "  ";
  line += j.step == 0 ? "yt-dlp" : "ffmpeg";
  ui::TextFit(line.c_str(), kW - 130, P.textDim);
  ImGui::SameLine(ImGui::GetCursorPosX() + kW - 120);
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  const std::string amount = total > 0 ? mb(got) + " / " + mb(total) : mb(got);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, 120 - ImGui::CalcTextSize(amount.c_str()).x));
  ImGui::TextColored(theme::vec(P.textFaint), "%s", amount.c_str());
  ImGui::PopFont();
  bar(total > 0 ? static_cast<float>(static_cast<double>(got) / static_cast<double>(total)) : 0.0f, total <= 0);
  rightsNote();
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  const float w = buttonW("Cancel");
  rightAlign(w);
  if (ui::TextButton("Cancel", false, w) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    s.setup->cancel = true;
    s.fetchAfterSetup = false;
  }
}

void drawFetching(App& a) {
  State& s = S();
  const auto& P = pal();
  title(a, tr("Import from URL"));
  ImGui::Dummy(ImVec2(0, theme::kS2));
  urlRow(a, false);
  ImGui::Dummy(ImVec2(0, theme::kS2));
  const int dots = static_cast<int>(ImGui::GetTime() * 3.0) % 4;
  ImGui::TextColored(theme::vec(P.textDim), "%s%s", tr("Reading the link"), std::string(static_cast<size_t>(dots), '.').c_str());
  bar(0, true);
  rightsNote();
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  const float w = buttonW("Cancel");
  rightAlign(w);
  if ((ui::TextButton("Cancel", false, w) || ImGui::IsKeyPressed(ImGuiKey_Escape)) && s.job) s.job->cancel = true;
}

// Title, uploader / site / length of what the link holds.
void infoCard(App& a) {
  State& s = S();
  const auto& P = pal();
  const ul::Info& in = s.info;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, theme::kS2));
  ImGui::BeginChild("##card", ImVec2(kW, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
  const float inner = kW - 2 * theme::kS3;
  ImGui::PushFont(a.fontBold, theme::kFontBody);
  ui::TextFit(in.title.c_str(), inner, P.text);
  ImGui::PopFont();
  std::string sub;
  auto add = [&](const std::string& part) {
    if (part.empty()) return;
    if (!sub.empty()) sub += "  \xC2\xB7  ";
    sub += part;
  };
  add(in.uploader);
  add(in.site.empty() ? ul::hostOf(in.url) : in.site);
  if (in.playlist) {
    char b[64];
    std::snprintf(b, sizeof b, tr("Playlist, %d items"), static_cast<int>(in.entries.size()));
    add(b);
  } else if (in.duration > 0) {
    add(ul::formatTime(in.duration));
  }
  ui::TextFit(sub.c_str(), inner, P.textDim);
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  ui::PathFit(in.url.c_str(), inner, P.textFaint);
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

void formLabel(const char* t, float col) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "%s", tr(t));
  ImGui::SameLine(col);
}

void drawPreview(App& a) {
  State& s = S();
  const auto& P = pal();
  title(a, tr("Import from URL"));
  ImGui::Dummy(ImVec2(0, theme::kS2));
  urlRow(a, true);
  if (s.screen != Screen::Preview) return;  // Fetch again was pressed
  if (!s.urlHint.empty()) wrapped(s.urlHint, P.danger);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::BeginDialogBody("##pvbody", 1);  // a long playlist scrolls; the buttons stay on screen
  infoCard(a);
  ImGui::Dummy(ImVec2(0, theme::kS2));
  const char* labels[] = {"Place", "Items", "Range"};
  const float col = ImGui::GetCursorPosX() + ui::FormColumn(labels, 3, 72, 140);
  // Where it goes.
  formLabel("Place", col);
  int place = std::clamp(static_cast<int>(a.settings.getNumber("import.urlPlacement")), 0, 1);
  const char* places[] = {i18n::N_("Clip on a new track"), i18n::N_("Browser only")};
  const char* placeHints[] = {i18n::N_("An audio clip on a new playlist track at the playhead, and the file in the browser"),
                              i18n::N_("Only the file, in the browser's Imported folder")};
  if (ui::Segmented("##place", places, 2, &place, 150, 0, placeHints)) a.settings.set("import.urlPlacement", place);
  if (s.info.playlist) {
    formLabel("Items", col);
    const char* modes[] = {i18n::N_("First item"), i18n::N_("Choose items")};
    ui::Segmented("##plmode", modes, 2, &s.playlistMode, 150);
    if (s.playlistMode == 1) {
      const float rowH = ImGui::GetFrameHeightWithSpacing();
      const int rows = std::min<int>(kMaxListRows, static_cast<int>(s.info.entries.size()));
      ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
      ImGui::BeginChild("##items", ImVec2(kW, rowH * static_cast<float>(rows) + 2 * ImGui::GetStyle().WindowPadding.y),
                        ImGuiChildFlags_Borders);
      for (size_t i = 0; i < s.info.entries.size(); ++i) {
        const ul::Item& it = s.info.entries[i];
        ImGui::PushID(static_cast<int>(i));
        bool on = i < s.picked.size() && s.picked[i];
        if (ImGui::Checkbox("##pick", &on) && i < s.picked.size()) s.picked[i] = on ? 1 : 0;
        ImGui::SameLine(0, theme::kS2);
        const std::string len = it.duration > 0 ? ul::formatTime(it.duration) : std::string();
        const float lenW = len.empty() ? 0.0f : ImGui::CalcTextSize(len.c_str()).x + theme::kS3;
        ImGui::AlignTextToFramePadding();
        char num[24];
        std::snprintf(num, sizeof num, "%zu.", i + 1);
        ImGui::TextColored(theme::vec(P.textFaint), "%s", num);
        ImGui::SameLine(0, theme::kS1);
        const float w = std::max(40.0f, ImGui::GetContentRegionAvail().x - lenW);
        ui::TextFit(it.title.c_str(), w, P.text, true);
        if (!len.empty()) {
          ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - lenW + theme::kS3);
          ImGui::AlignTextToFramePadding();
          ImGui::TextColored(theme::vec(P.textDim), "%s", len.c_str());
        }
        ImGui::PopID();
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
      int n = 0;
      for (char c : s.picked) n += c ? 1 : 0;
      char b[64];
      std::snprintf(b, sizeof b, tr("%d of %d chosen"), n, static_cast<int>(s.picked.size()));
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", b);
      ImGui::SameLine();
      if (ui::TextButton("All", false)) std::fill(s.picked.begin(), s.picked.end(), 1);
      ImGui::SameLine(0, theme::kS1);
      if (ui::TextButton("None", false)) std::fill(s.picked.begin(), s.picked.end(), 0);
    }
  } else {
    formLabel("Range", col);
    ImGui::Checkbox(tr("Only part"), &s.trimOn);
    ui::Hint("Import only a time range (minutes:seconds)");
    if (s.trimOn) {
      ImGui::SameLine(0, theme::kS3);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "%s", tr("from"));
      ImGui::SameLine(0, theme::kS1);
      ImGui::SetNextItemWidth(72);
      ImGui::InputText("##from", s.from, sizeof s.from);
      ImGui::SameLine(0, theme::kS2);
      ImGui::TextColored(theme::vec(P.textDim), "%s", tr("to"));
      ImGui::SameLine(0, theme::kS1);
      ImGui::SetNextItemWidth(72);
      ImGui::InputTextWithHint("##to", tr("end"), s.to, sizeof s.to);
    }
  }
  // What import will do.
  bool ok = true;
  std::string note;
  if (s.trimOn && !s.info.playlist) {
    const double f = ul::parseTime(s.from), t = ul::parseTime(s.to);
    if (f < 0 || (s.to[0] && t < 0)) ok = false, note = tr("Times are minutes:seconds, like 1:30");
    else if (s.to[0] && t <= f) ok = false, note = tr("The end must come after the start");
    else if (s.info.duration > 0 && f >= s.info.duration) ok = false, note = tr("The start is past the end of the media");
  }
  const size_t count = requests().size();
  if (ok && count == 0) ok = false, note = tr("Choose at least one item");
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (!note.empty()) wrapped(note, P.danger);
  else wrapped(tr("Saved as 32-bit float WAV at the source's sample rate, in the project's Imported folder."), P.textFaint);
  rightsNote();
  ui::EndDialogBody();
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  const float wc = buttonW("Cancel"), wi = buttonW("Import");
  rightAlign(wc + wi + theme::kS2);
  const bool cancel = ui::TextButton("Cancel", false, wc) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(!ok);
  const bool go = ui::TextButton("Import", true, wi);
  ImGui::EndDisabled();
  if (cancel) close();
  else if (go && ok) startImport(a);
}

void drawImporting(App& a) {
  State& s = S();
  const auto& P = pal();
  title(a, tr("Import from URL"));
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (!s.job) return;
  const ul::Phase ph = s.job->get();
  const int item = s.job->item.load(), items = s.job->items.load();
  std::string detail, name;
  {
    std::lock_guard<std::mutex> lk(s.job->mu);
    detail = s.job->detail;
  }
  const auto reqs = requests();
  if (item >= 0 && item < static_cast<int>(reqs.size())) name = reqs[static_cast<size_t>(item)].item.title;
  std::string line = ph == ul::Phase::Converting ? tr("Converting to WAV") : tr("Downloading the best audio");
  if (items > 1) {
    char b[48];
    std::snprintf(b, sizeof b, "  (%d / %d)", item + 1, items);
    line += b;
  }
  ImGui::PushFont(a.fontBold, theme::kFontBody);
  ui::TextFit(name.c_str(), kW, P.text);
  ImGui::PopFont();
  ui::TextFit(line.c_str(), kW - 140, P.textDim);
  if (!detail.empty() && ph == ul::Phase::Downloading) {
    ImGui::SameLine(ImGui::GetCursorPosX() + kW - 130);
    ImGui::PushFont(a.fontMono, theme::kFontLabel);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, 130 - ImGui::CalcTextSize(detail.c_str()).x));
    ImGui::TextColored(theme::vec(P.textFaint), "%s", detail.c_str());
    ImGui::PopFont();
  }
  const double f = s.job->fraction.load();
  bar(static_cast<float>(f), f < 0);
  rightsNote();
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  const float w = buttonW("Cancel");
  rightAlign(w);
  if (ui::TextButton("Cancel", false, w) || ImGui::IsKeyPressed(ImGuiKey_Escape)) s.job->cancel = true;
}

void drawError(App& a) {
  State& s = S();
  const auto& P = pal();
  title(a, tr("Could not import"));
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (!s.lead.empty()) wrapped(tr(s.lead.c_str()), P.text);
  if (!s.error.empty()) {
    ImGui::PushFont(a.fontMono, theme::kFontLabel);
    wrapped(s.error, P.danger);
    ImGui::PopFont();
  }
  if (S().errorBack == Screen::Entry && (!s.yt.found() || !s.ff.found())) wrapped(mt::systemPackagesHint(), P.textDim);
  ImGui::Dummy(ImVec2(kW, theme::kS2));
  const float wb = buttonW("Back"), wc = buttonW("Close");
  rightAlign(wb + wc + theme::kS2);
  const bool back = ui::TextButton("Back", false, wb);
  ImGui::SameLine(0, theme::kS2);
  const bool cl = ui::TextButton("Close", true, wc) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (back) {
    s.screen = s.errorBack;
    s.focusUrl = s.errorBack == Screen::Entry;
  } else if (cl) {
    close();
  }
}

void readEnv() {
  State& s = S();
  if (const char* f = std::getenv("GRIDLINE_URLIMPORT_FAKE_NET"); f && *f) s.fakeTool = f;
  if (const char* d = std::getenv("GRIDLINE_URLIMPORT_FAKE_STEP_MS"); d && *d) s.fakeStepMs = std::atoi(d);
}

}  // namespace

std::string importedDir(const App& a) {
  if (!a.projectPath.empty()) return (fs::u8path(a.projectPath).parent_path() / "Imported").u8string();
  return (fs::u8path(a.configDir()) / "Imported").u8string();
}

void open(App& a) {
  State& s = S();
  (void)a;
  readEnv();
  if (s.screen != Screen::Closed) {  // already open: just focus the field
    s.focusUrl = true;
    return;
  }
  s.screen = Screen::Entry;
  s.openPopup = true;
  s.focusUrl = true;
  s.urlHint.clear();
  s.url[0] = 0;
  // A link already on the clipboard fills the field.
  const std::string l = ul::linkInText(clipboardText());
  if (!l.empty()) setUrl(l);
  locate();
  devsig::forget("urlimport-preview");
  devsig::forget("urlimport-done");
  devsig::forget("urlimport-error");
  devsig::forget("urlimport-importing");
  devsig::forget("urlimport-setup");
  devsig::forget("urlimport-cancelled");
}

void draw(App& a) {
  State& s = S();
  poll(a);
  if (s.screen == Screen::Closed) return;
  if (s.openPopup) {
    s.openPopup = false;
    if (!ImGui::IsPopupOpen("##urlimport")) {
      ImGui::OpenPopup("##urlimport");
      s.shownAt = ImGui::GetTime();
    }
  }
  const float appear = std::clamp(static_cast<float>((ImGui::GetTime() - s.shownAt) / 0.18), 0.0f, 1.0f);
  const float ease = 1.0f - (1.0f - appear) * (1.0f - appear);
  // Centred every frame: the popup changes height between its screens.
  ui::PlaceNextDialog(ImVec2(kW + 2 * theme::kS6, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.15f + 0.85f * ease);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg);
  dim.w *= ease;
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dim);
  const bool open = ImGui::BeginPopupModal("##urlimport", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!open) {
    stopJobs();
    s.screen = Screen::Closed;
    return;
  }
  switch (s.screen) {
    case Screen::Entry: drawEntry(a); break;
    case Screen::Setup:
      if (s.setup) drawSetup(a);
      break;
    case Screen::Fetching: drawFetching(a); break;
    case Screen::Preview: drawPreview(a); break;
    case Screen::Importing: drawImporting(a); break;
    case Screen::Error: drawError(a); break;
    case Screen::Closed: ImGui::CloseCurrentPopup(); break;
  }
  ImGui::EndPopup();
}

void settingsSection(App& a) {
  State& s = S();
  readEnv();
  collectUpdate();
  const auto& P = pal();
  Settings& st = a.settings;
  ui::BeginSection(tr("IMPORT FROM URL"));
  static int64_t lastLocate = -1;
  if (lastLocate != wallNow()) {  // once a second at most (file system checks)
    lastLocate = wallNow();
    if (s.screen == Screen::Closed) locate();
  }
  auto toolLine = [&](const char* name, const mt::Tool& t) {
    std::string where;
    switch (t.from) {
      case mt::From::None: where = tr("not set up yet (set up on first use)"); break;
      case mt::From::Override: where = tr("set by the environment"); break;
      case mt::From::Installed: where = tr("installed by Gridline"); break;
      case mt::From::Bundled: where = tr("included with Gridline"); break;
      case mt::From::System: where = tr("this computer's"); break;
    }
    std::string line = std::string(name) + "  \xC2\xB7  " + where;
    if (t.found() && std::string(name) == "yt-dlp") {
      const std::string v = mt::recordedVersion(fs::u8path(t.path).parent_path().u8string());
      if (!v.empty()) line += "  \xC2\xB7  " + v;
    }
    ImGui::AlignTextToFramePadding();
    ui::TextFit(line.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), t.found() ? P.text : P.textDim);
    if (t.found()) ui::Hint(t.path);
  };
  toolLine("yt-dlp", s.yt);
  toolLine("ffmpeg", s.ff);
  bool on = st.getBool("import.urlAutoUpdate");
  if (ImGui::Checkbox(tr("Keep yt-dlp up to date automatically"), &on)) st.set("import.urlAutoUpdate", on);
  ui::Hint("Sites change often. At most once a day, when Import from URL is used, Gridline checks yt-dlp's releases on GitHub and "
            "installs a newer one after checking its SHA-256 checksum. Nothing else is sent.");
  if (s.upd) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Checking for a newer yt-dlp..."));
  } else {
    const bool can = !s.automated || usingFake();
    ImGui::BeginDisabled(!can);
    if (ui::TextButton(s.yt.found() ? "Update yt-dlp now" : "Set up now", false, 0, "Download and check the newest yt-dlp now")) {
      auto job = std::make_shared<UpdateJob>();
      s.upd = job;
      const mt::Net n = net();
      const std::string dir = mt::installDir();
      const std::string cur = s.yt.from == mt::From::Installed ? mt::recordedVersion(dir) : std::string();
      st.set("import.urlLastUpdateCheck", static_cast<double>(wallNow()));
      std::thread([job, n, dir, cur] {
        mt::UpdateResult r = mt::updateYtdlp(n, dir, cur, nullptr);
        std::lock_guard<std::mutex> lk(job->mu);
        job->result = std::move(r);
        job->done = true;
      }).detach();
    }
    ImGui::EndDisabled();
    if (!s.updStatus.empty()) {
      ImGui::SameLine(0, theme::kS2);
      ImGui::AlignTextToFramePadding();
      ui::TextFit(s.updStatus.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.textDim);
    }
  }
  ui::EndSection();
}

void clipSourceInfo(App& a, const std::string& audioPath) {
  if (audioPath.empty()) return;
  // Read once per file (the window draws every frame).
  static std::string lastPath;
  static bool has = false;
  static ul::Source src;
  if (audioPath != lastPath) {
    lastPath = audioPath;
    src = ul::Source{};
    has = ul::readSource(audioPath, src);
  }
  if (!has) return;
  const auto& P = pal();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::Label("SOURCE");
  const float w = std::max(200.0f, ImGui::GetContentRegionAvail().x);
  ui::TextFit(src.title.c_str(), w, P.text);
  std::string sub = src.uploader;
  const std::string site = src.site.empty() ? ul::hostOf(src.url) : src.site;
  if (!site.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + site;
  if (src.duration > 0) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + ul::formatTime(src.duration);
  if (!sub.empty()) ui::TextFit(sub.c_str(), w, P.textDim);
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  ui::PathFit(src.url.c_str(), w - 70, P.textFaint, true);
  ImGui::PopFont();
  ImGui::SameLine();
  if (ui::TextButton("Copy link", false)) ImGui::SetClipboardText(src.url.c_str());
}

void shutdown() {
  State& s = S();
  stopJobs();
  for (int i = 0; i < 40 && ((s.job && !s.job->done) || (s.setup && !s.setup->done)); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::error_code ec;
  if (!s.workDir.empty()) fs::remove_all(fs::u8path(s.workDir), ec);
}

void start(bool automated) { S().automated = automated; }

void devCommand(App& a, const std::vector<std::string>& args) {
  State& s = S();
  s.automated = true;
  const std::string cmd = args.empty() ? std::string() : args[0];
  const std::string arg = args.size() > 1 ? args[1] : std::string();
  if (cmd == "open") {
    open(a);
  } else if (cmd == "clipboard") {
    s.devClipboard = arg;
    s.devClipboardSet = true;
  } else if (cmd == "paste") {
    paste();
  } else if (cmd == "url") {
    setUrl(arg);
  } else if (cmd == "fetch") {
    fetch(a);
  } else if (cmd == "import") {
    if (s.screen == Screen::Preview) startImport(a);
  } else if (cmd == "cancel") {
    stopJobs();
  } else if (cmd == "close") {
    stopJobs();
    s.screen = Screen::Closed;
  } else if (cmd == "placement") {
    a.settings.set("import.urlPlacement", arg == "browser" ? 1 : 0);
  } else if (cmd == "playlist") {
    s.playlistMode = arg == "all" ? 1 : 0;
    std::fill(s.picked.begin(), s.picked.end(), 1);
  } else if (cmd == "trim") {
    s.trimOn = true;
    std::snprintf(s.from, sizeof s.from, "%s", arg.c_str());
    std::snprintf(s.to, sizeof s.to, "%s", args.size() > 2 ? args[2].c_str() : "");
  } else if (cmd == "state") {
    static const char* names[] = {"closed", "entry", "setup", "fetching", "preview", "importing", "error"};
    std::printf("urlimport state %s url \"%s\" title \"%s\" items %d yt-dlp %s ffmpeg %s error \"%s\" lead \"%s\"\n",
                names[static_cast<int>(s.screen)], s.url, s.info.title.c_str(), static_cast<int>(s.info.entries.size()),
                mt::fromName(s.yt.from), mt::fromName(s.ff.from), s.error.c_str(), s.lead.c_str());
    std::fflush(stdout);
  }
}

}  // namespace gl::urlimportui
