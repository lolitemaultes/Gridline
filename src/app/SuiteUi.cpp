// Gridline - J-131: the Gridline Suite in the app. See SuiteUi.h.
#include "SuiteUi.h"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/PluginSuite.h"
#include "../core/Settings.h"
#include "../core/SuiteInstall.h"
#include "App.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::suiteui {
namespace {

using i18n::tr;

constexpr float kDialogW = 500.0f;
constexpr int kStartupDelayFrames = 60;  // after the update check's moment: never two questions at once

// One copy (install / update / remove of every suite that needs it) on a
// worker thread; shared with it.
struct Job {
  std::mutex mu;
  bool done = false, ok = false, removing = false, updating = false;
  std::string err;
};

struct State {
  bool started = false, automated = true;
  bool statusKnown = false;
  std::vector<suite::InstallStatus> status;  // one per suite (suite::suites())
  bool offerPending = false, dialogOpen = false;
  int frames = 0;
  std::shared_ptr<Job> job;
  std::string lastOutcome;
};
State& S() {
  static State s;
  return s;
}

bool needsCopy(const suite::InstallStatus& st) {
  return st.state == suite::InstallState::NotInstalled || st.state == suite::InstallState::Outdated;
}
bool userCopy(const suite::InstallStatus& st) {
  return st.state == suite::InstallState::Installed || st.state == suite::InstallState::Outdated;
}
bool anyNeedsCopy() {
  return std::any_of(S().status.begin(), S().status.end(), needsCopy);
}

void refresh() {
  State& s = S();
  s.status.clear();
  for (const suite::Suite& su : suite::suites()) s.status.push_back(suite::installStatus(su.id));
  s.statusKnown = true;
}

// Installs the suites that need it (or removes the per-user copies).
void startCopy(bool remove) {
  State& s = S();
  if (s.job) return;
  std::vector<suite::InstallStatus> todo;
  for (const auto& st : s.status)
    if (remove ? userCopy(st) : needsCopy(st)) todo.push_back(st);  // current copies stay as they are
  if (todo.empty()) return;
  auto job = std::make_shared<Job>();
  job->removing = remove;
  s.job = job;
  std::thread([job, todo, remove] {
    std::string err;
    bool ok = true;
    for (const auto& st : todo) ok = (remove ? suite::remove(st, &err) : suite::install(st, &err)) && ok;
    std::lock_guard<std::mutex> lk(job->mu);
    job->ok = ok;
    job->err = err;
    job->done = true;
  }).detach();
}

void collect(App& a) {
  State& s = S();
  if (!s.job) return;
  bool ok = false, removing = false, updating = false;
  std::string err;
  {
    std::lock_guard<std::mutex> lk(s.job->mu);
    if (!s.job->done) return;
    ok = s.job->ok;
    removing = s.job->removing;
    updating = s.job->updating;
    err = s.job->err;
  }
  s.job.reset();
  refresh();
  if (ok) {
    s.lastOutcome = removing ? tr("Removed from your plug-in folders") : tr("Installed in your plug-in folders");
    a.notify(removing   ? tr("Gridline Suite removed from your plug-in folders")
             : updating ? tr("Gridline Suite updated to this version: restart your other music programs to use it")
                        : tr("Gridline Suite installed: rescan plugins in your other music programs to find it"));
  } else {
    s.lastOutcome = err;
    a.notify(std::string(tr("Gridline Suite: ")) + err, true);
  }
}

bool goodMoment(App& a) {
  if (a.engine.playing() || a.engine.recordingOn()) return false;
  if (a.confirmDiscard || ImGui::GetTopMostPopupModal() != nullptr) return false;
  return S().frames >= kStartupDelayFrames;
}

float buttonW(const char* label) { return std::max(96.0f, ImGui::CalcTextSize(tr(label)).x + 2 * theme::kS4); }

void wrapped(const std::string& t, ImU32 col) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kDialogW);
  ImGui::TextColored(theme::vec(col), "%s", t.c_str());
  ImGui::PopTextWrapPos();
}

std::string stateText(const suite::InstallStatus& st) {
  switch (st.state) {
    case suite::InstallState::NotShipped: return tr("Not part of this copy of Gridline (a build folder)");
    case suite::InstallState::NotInstalled: return tr("Not installed: other music programs do not see it yet");
    case suite::InstallState::Outdated: return tr("Installed, from another Gridline version");
    case suite::InstallState::Installed: return tr("Installed in your plug-in folders");
    case suite::InstallState::SystemInstalled: return tr("Installed for all users");
  }
  return {};
}

void drawOffer(App& a) {
  State& s = S();
  const auto& P = theme::pal();
  const bool update = std::any_of(s.status.begin(), s.status.end(),
                                  [](const suite::InstallStatus& st) { return st.state == suite::InstallState::Outdated; });
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kDialogW);
  ImGui::TextUnformatted(update ? tr("Update the Gridline Suite?") : tr("Install the Gridline Suite?"));
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  wrapped(tr("Gridline's instruments and effects also work as plugins in other music programs (VST3, CLAP and, on macOS, "
             "Audio Unit). Gridline can copy them into your plug-in folders now and keep them up to date."),
          P.text);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  for (const auto& st : s.status)
    if (needsCopy(st))
      for (const auto& b : st.bundles) wrapped(b.format + ": " + b.dest, P.textDim);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  wrapped(tr("You can change this later in Settings, Files, Gridline Suite."), P.textFaint);
  ImGui::Dummy(ImVec2(kDialogW, theme::kS1 + theme::kS2));
  const float wn = buttonW("Never"), wl = buttonW("Not now"), wi = buttonW("Install");
  const float x = ImGui::GetCursorPosX() + kDialogW - (wn + wl + wi + 2 * theme::kS2);
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
  const bool never = ui::TextButton(tr("Never"), false, wn, tr("Don't copy the suite; Settings can still install it"));
  ImGui::SameLine(0, theme::kS2);
  const bool later = ui::TextButton(tr("Not now"), false, wl, tr("Ask again the next time Gridline starts")) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SameLine(0, theme::kS2);
  const bool install = ui::TextButton(tr("Install"), true, wi);
  if (never || install) {
    a.settings.set("suite.install", never ? "no" : "yes");
    a.settings.save(Settings::defaultPath());
  }
  if (install) startCopy(false);
  if (never || later || install) {
    s.dialogOpen = false;
    ImGui::CloseCurrentPopup();
  }
}

}  // namespace

void start(App& a, bool automated) {
  State& s = S();
  if (s.started) return;
  s.started = true;
  s.automated = automated || std::getenv("GRIDLINE_NO_SUITE_INSTALL") || std::getenv("GRIDLINE_NO_PLUGIN_SCAN");
  if (s.automated) return;
  refresh();
  switch (suite::startupAction(s.status, a.settings.getString("suite.install"))) {
    case suite::StartupAction::Copy: {
      const bool updating = std::any_of(s.status.begin(), s.status.end(), userCopy);
      startCopy(false);  // the copies follow this version (and a suite new in it joins them)
      if (s.job) s.job->updating = updating;
      break;
    }
    case suite::StartupAction::Ask: s.offerPending = true; break;
    case suite::StartupAction::None: break;
  }
}

void draw(App& a) {
  State& s = S();
  if (!s.started) return;
  ++s.frames;
  collect(a);
  if (s.offerPending && !s.dialogOpen && goodMoment(a)) {
    s.offerPending = false;
    s.dialogOpen = true;
    ImGui::OpenPopup("##suite-offer");
  }
  if (!s.dialogOpen) return;
  ui::PlaceNextDialog(ImVec2(kDialogW + 2 * theme::kS6, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  const bool open = ImGui::BeginPopupModal("##suite-offer", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (!open) {
    s.dialogOpen = false;
    return;
  }
  drawOffer(a);
  ImGui::EndPopup();
}

void settingsSection(App& a) {
  State& s = S();
  if (!s.statusKnown) refresh();
  const auto& P = theme::pal();
  Settings& st = a.settings;
  ui::BeginSection(tr("GRIDLINE SUITE"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     tr("Gridline's instruments and effects as plugins for other music programs (VST3, CLAP and, on macOS, Audio Unit)."));
  ImGui::PopTextWrapPos();
  bool copyable = false, anyUser = false;
  for (const auto& is : s.status) {
    std::string line = is.name + ": " + stateText(is);
    if (!is.shippedVersion.empty()) line += "  \xC2\xB7  " + is.shippedVersion;
    ImGui::AlignTextToFramePadding();
    ui::TextFit(line.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.text);
    if (is.state == suite::InstallState::NotShipped || is.state == suite::InstallState::SystemInstalled) continue;
    copyable = true;
    anyUser = anyUser || userCopy(is);
    for (const auto& b : is.bundles) {
      const std::string where = b.format + ": " + b.dest;
      ui::TextFit(where.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.textFaint);
    }
  }
  if (copyable) {
    bool keep = st.getString("suite.install") == "yes";
    if (ImGui::Checkbox(tr("Install the Gridline Suite automatically"), &keep)) {
      st.set("suite.install", keep ? "yes" : "no");
      if (keep) startCopy(false);
    }
    ui::Hint("Installed copies always update with Gridline; this also installs the suite where it is missing");
    if (s.job) {
      ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Copying..."));
    } else {
      if (anyNeedsCopy() && ui::TextButton(tr("Install"), true, 0, tr("Copy the suite into your plug-in folders"))) startCopy(false);
      if (anyUser) {
        if (anyNeedsCopy()) ImGui::SameLine();
        if (ui::TextButton(tr("Remove"), false, 0, tr("Delete the copies in your plug-in folders"))) {
          st.set("suite.install", "no");  // removed on purpose: the next start does not copy it back
          startCopy(true);
        }
      }
    }
    if (!s.lastOutcome.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", s.lastOutcome.c_str());
  }
  ui::EndSection();
}

}  // namespace gl::suiteui
