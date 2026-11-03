// Gridline: the crash pop-up and the project's recovery copies (see core/Crash.h).
#include "CrashUi.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Project.h"
#include "App.h"
#include "CollabUi.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"

namespace fs = std::filesystem;

namespace gl::crashui {

using i18n::tr;
using theme::pal;

namespace {

constexpr float kWidth = 640.0f;
constexpr float kCodeHeight = 206.0f;
constexpr float kButtonGap = 8.0f;
constexpr const char* kNote = i18n::N_("Gridline sends nothing anywhere. The code holds no file paths and no project names: share it where you choose, for example in a GitHub issue.");

// ---- what the pop-up says ----
struct Content {
  std::string title, lead, work, code, id, folder;
  const char* buttons[3] = {nullptr, nullptr, nullptr};  // first is the suggested one
  int dangerButton = -1;
};

std::string fieldOf(const std::string& text, const char* key) {
  const std::string k = std::string("\n") + key + ": ";
  const size_t p = text.find(k);
  if (p == std::string::npos) return {};
  const size_t b = p + k.size();
  return text.substr(b, text.find('\n', b) - b);
}

std::string formatTr(const char* fmt, const std::string& a) {
  const char* f = tr(fmt);
  char buf[512];
  std::snprintf(buf, sizeof buf, f, a.c_str());
  return buf;
}

std::string ageText(int64_t seconds) {
  char buf[64];
  if (seconds < 90) std::snprintf(buf, sizeof buf, tr("%d seconds"), static_cast<int>(std::max<int64_t>(seconds, 1)));
  else std::snprintf(buf, sizeof buf, tr("%d minutes"), static_cast<int>(seconds / 60));
  return buf;
}

// The honest state of the user's work after this report.
std::string workLine(const crash::PendingReport& r) {
  if (r.kind == "graphics" && r.recoveryRescued)
    return tr("A copy of your project was saved when this happened, with every change. Nothing was damaged: open it to carry on.");
  if (r.recoveryRescued)
    return tr("A copy of your project was saved at the moment this happened. Memory may already have been damaged by then, so check the copy before you save over the original.");
  if (!r.recoveryFile.empty() && r.recoverySaved > 0)
    return formatTr("The last recovery copy of your project is from %s before this happened. Changes made after that are lost.",
                    ageText(std::max<int64_t>(r.crashTime - r.recoverySaved, 1)));
  if (fieldOf(r.text, "unsaved work") == "yes")
    return tr("No recovery copy could be saved, so changes since your last save are lost. Automatic backups may have an older state: see Settings, Backup.");
  return tr("There were no unsaved changes, so nothing was lost.");
}

std::string leadFor(const crash::PendingReport& r) {
  if (r.kind == "freeze")
    return tr("The interface has not answered for a while. It may still be working on something long: if it comes back, this window closes by itself.");
  if (r.kind == "ended")
    return tr("Gridline ended without saying why. It may have been closed from outside, or the computer may have run out of memory.");
  if (r.kind == "graphics")
    return tr("The graphics driver stopped working and did not come back for Gridline, so it had to close. A driver update or a driver timeout can do this. Restarting Gridline usually puts it right; if not, restart the computer.");
  return tr("Gridline hit a serious error and had to close. After a fault like this its memory can no longer be trusted, so it cannot safely keep running.");
}

std::string titleFor(const std::string& kind) {
  if (kind == "freeze") return tr("Gridline is not responding");
  if (kind == "ended") return tr("Gridline ended unexpectedly");
  if (kind == "graphics") return tr("Gridline lost the graphics driver");
  return tr("Gridline stopped after an error");
}

// ---- drawing ----
// One pop-up body. Returns the clicked button (0, 1, 2) or -1.
int drawBody(const Content& c, ImFont* bold, ImFont* mono, double& copiedAt, float codeHeight, float width) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Title with the mark of something wrong.
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 18, p.y));
    if (bold) ImGui::PushFont(bold, theme::kFontTitle);
    const float titleH = ImGui::GetTextLineHeight();
    dl->AddCircleFilled(ImVec2(p.x + 5, p.y + titleH * 0.5f), 4.5f, P.danger);
    ImGui::PushStyleColor(ImGuiCol_Text, P.text);
    ImGui::TextUnformatted(c.title.c_str());
    ImGui::PopStyleColor();
    if (bold) ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::PushStyleColor(ImGuiCol_Text, P.textDim);
  ImGui::TextUnformatted(c.lead.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  // What became of the user's work: a card of its own.
  if (!c.work.empty()) {
    const float pad = theme::kS3;
    const float textW = width - 2 * pad;
    const float textH = ImGui::CalcTextSize(c.work.c_str(), nullptr, false, textW).y;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 b(a.x + width, a.y + textH + 2 * pad);
    dl->AddRectFilled(a, b, P.surface800, theme::kRadius);
    dl->AddRect(a, b, P.line, theme::kRadius);
    dl->AddRectFilled(a, ImVec2(a.x + 3, b.y), P.accent, theme::kRadius, ImDrawFlags_RoundCornersLeft);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textW);
    ImGui::PushStyleColor(ImGuiCol_Text, P.text);
    ImGui::TextUnformatted(c.work.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
    ImGui::Dummy(ImVec2(width, theme::kS1));
  }
  // The code.
  {
    const float copyW = ui::TextButtonWidth("Copy code");
    const float rowY = ImGui::GetCursorPosY();
    const float rowX = ImGui::GetCursorPosX();
    const float textOff = (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorPosY(rowY + textOff);
    if (bold) ImGui::PushFont(bold, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, P.textDim);
    ImGui::TextUnformatted(tr("Crash code"));
    ImGui::PopStyleColor();
    if (bold) ImGui::PopFont();
    if (!c.id.empty()) {
      ImGui::SameLine(0, theme::kS2);
      if (mono) ImGui::PushFont(mono, 0.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, P.textFaint);
      ImGui::TextUnformatted(c.id.c_str());
      ImGui::PopStyleColor();
      if (mono) ImGui::PopFont();
    }
    const bool justCopied = ImGui::GetTime() - copiedAt < 1.6;
    ImGui::SetCursorPos(ImVec2(rowX + width - copyW, rowY));
    if (ui::TextButton(justCopied ? "Copied" : "Copy code", justCopied, copyW)) {
      ImGui::SetClipboardText(c.code.c_str());
      copiedAt = ImGui::GetTime();
    }
    ImGui::SetCursorPosX(rowX);
    if (codeHeight <= 0) {  // fills the window: what comes after it is the note and the buttons
      const float note = ImGui::CalcTextSize(tr(kNote), nullptr, false, width).y;
      const float spacing = ImGui::GetStyle().ItemSpacing.y;
      codeHeight = std::max(90.0f, ImGui::GetContentRegionAvail().y - note - ImGui::GetFrameHeight() - theme::kS1 - 1 - 4 * spacing);
    }
    if (mono) ImGui::PushFont(mono, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, P.surface950);
    ImGui::PushStyleColor(ImGuiCol_Text, P.textDim);
    // Read only, and selectable: part of it can be copied by hand too.
    ImGui::InputTextMultiline("##crashcode", const_cast<char*>(c.code.c_str()), c.code.size() + 1, ImVec2(width, codeHeight), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor(2);
    if (mono) ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(0, 1));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::PushStyleColor(ImGuiCol_Text, P.textFaint);
  ImGui::TextUnformatted(tr(kNote));
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  // Footer: the suggested action on the right.
  float total = 0;
  int count = 0;
  for (const char* b : c.buttons)
    if (b) {
      total += ui::TextButtonWidth(b);
      ++count;
    }
  total += kButtonGap * static_cast<float>(std::max(0, count - 1));
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const float x0 = ImGui::GetCursorPosX() + width - total;
  ImGui::SetCursorPosX(x0);
  int clicked = -1;
  bool first = true;
  for (int i = count - 1; i >= 0; --i) {
    if (!first) ImGui::SameLine(0, kButtonGap);
    first = false;
    ImGui::PushID(i);
    const float w = ui::TextButtonWidth(c.buttons[i]);
    const bool hit = i == c.dangerButton ? ui::DangerButton(c.buttons[i], false, w) : ui::TextButton(c.buttons[i], i == 0, w);
    if (hit) clicked = i;
    ImGui::PopID();
  }
  return clicked;
}

// ---- the recovery copy ----
struct CopyState {
  std::thread writer;
  std::atomic<bool> writing{false};
  std::atomic<bool> lastOk{false};
  uint64_t copiedRev = 0;
  bool haveCopy = false;
  bool wasDirty = false;
  double nextAt = 0;
  std::string dir, sid;
  std::string recoveredSid;
  std::string recoveredOriginal;
  bool recovering = false;
  App* app = nullptr;
  double testSeconds = 0;  // tests: seconds between copies
};
CopyState gCopy;

double monoNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string copyPath() { return (fs::u8path(gCopy.dir) / (gCopy.sid + ".gridline.json")).u8string(); }

void writeMeta(const std::string& original, int64_t saved) {
  std::ofstream f(fs::u8path(gCopy.dir) / (gCopy.sid + ".meta"), std::ios::trunc);
  f << "original=" << original << "\nsaved=" << saved << "\n";
}

// Forked child after a crash, a copy of this process's memory: the song as it was.
bool rescueSave(const char* path) {
  App* a = gCopy.app;
  if (!a) return false;
  if (!gl::saveProject(path, a->doc.song(), nullptr)) return false;
  Song check;  // what cannot be read back is no copy at all
  return gl::loadProject(path, check, nullptr);
}

void joinWriter() {
  if (gCopy.writer.joinable()) gCopy.writer.join();
}

// ---- pop-ups in the app ----
struct Shown {
  bool active = false;
  bool recoverable = false;  // the app keeps running
  Content content;
  crash::PendingReport report;
  double copiedAt = -100;
};
Shown gShown;
std::vector<crash::PendingReport> gPending;
constexpr const char* kPopup = "##crashreport";

void showPending() {
  if (gShown.active || gPending.empty()) return;
  crash::PendingReport r = std::move(gPending.back());
  gPending.pop_back();
  std::fprintf(stderr, "crash report shown: %s %s\n", r.kind.c_str(), r.id.c_str());
  Shown s;
  s.active = true;
  s.report = r;
  s.content.title = titleFor(r.kind == "freeze" ? "crash" : r.kind);
  if (r.kind == "freeze") s.content.lead = tr("The interface stopped answering last time, and Gridline did not come back from it.");
  else if (r.kind == "ended") s.content.lead = leadFor(r);
  else s.content.lead = leadFor(r);
  s.content.work = workLine(r);
  s.content.code = r.text;
  s.content.id = r.id;
  if (!r.recoveryFile.empty()) {
    s.content.buttons[0] = "Recover project";
    s.content.buttons[1] = "Not now";
  } else {
    s.content.buttons[0] = "Close";
  }
  gShown = std::move(s);
}

void showRecoverable(const crash::Recoverable& r) {
  Shown s;
  s.active = true;
  s.recoverable = true;
  s.content.title = tr("Something went wrong");
  s.content.lead = formatTr("%s failed with an internal error. Gridline kept running and your project is still open. If anything looks wrong, save a copy under a new name.",
                            r.where.empty() ? std::string(tr("An operation")) : std::string(tr(r.where.c_str())));
  s.content.code = r.code;
  s.content.id = r.id;
  s.content.buttons[0] = "Continue";
  gShown = std::move(s);
}

}  // namespace

// ---- the app's side ----
void startup(App& a) {
  gCopy.app = &a;
  if (!crash::installed() || a.embedded) return;
  gCopy.dir = crash::recoveryDir(a.configDir());
  gCopy.sid = crash::sessionId();
  if (!std::getenv("GRIDLINE_CRASH_TEST_NO_RESCUE")) crash::setRescue(rescueSave);
  if (const char* t = std::getenv("GRIDLINE_CRASH_TEST_COPY_SECONDS")) gCopy.testSeconds = std::atof(t);
  // Oldest first in the list; shown newest first (one after the other).
  gPending = crash::collectPending(a.configDir());
  if (gPending.size() > 3) {  // a pile of old ones: the newest three are enough to look at
    for (size_t i = 0; i + 3 < gPending.size(); ++i) crash::markSeen(gPending[i].path);
    gPending.erase(gPending.begin(), gPending.end() - 3);
  }
  crash::sweep(a.configDir());
}

void frame(App& a) {
  if (gCopy.dir.empty()) return;
  const bool dirty = a.doc.modified();
  crash::setUnsavedWork(dirty);
  const double now = monoNow();
  if (!dirty) {
    if (gCopy.haveCopy && !gCopy.writing.load()) {
      crash::discardRecovery(a.configDir(), gCopy.sid);
      gCopy.haveCopy = false;
    }
    if (!gCopy.recoveredSid.empty()) {  // the recovered project was saved: its old copy has served
      crash::discardRecovery(a.configDir(), gCopy.recoveredSid);
      gCopy.recoveredSid.clear();
    }
    gCopy.wasDirty = false;
    return;
  }
  if (!gCopy.wasDirty) {  // the first change: where this project belongs, for a rescue before the first copy
    gCopy.wasDirty = true;
    gCopy.nextAt = now + (gCopy.testSeconds > 0 ? gCopy.testSeconds : 15.0);
    gCopy.copiedRev = a.doc.revision() - 1;
    writeMeta(!a.projectPath.empty() ? a.projectPath : a.openedFrom, 0);
  }
  if (now < gCopy.nextAt || a.doc.revision() == gCopy.copiedRev || gCopy.writing.load()) return;
  // Not in the middle of a gesture, unless that goes on for a long time.
  if (ImGui::GetCurrentContext() && ImGui::IsAnyItemActive() && now < gCopy.nextAt + 30.0) return;
  joinWriter();
  const double t0 = monoNow();
  std::string text = projectToText(a.doc.song());
  const double cost = monoNow() - t0;
  // Thirty seconds apart, further when this project is big: at most half a percent of the interface's time.
  gCopy.nextAt = now + (gCopy.testSeconds > 0 ? gCopy.testSeconds : std::max(30.0, cost * 200.0));
  gCopy.copiedRev = a.doc.revision();
  gCopy.haveCopy = true;
  gCopy.writing.store(true);
  gCopy.writer = std::thread([text = std::move(text), file = copyPath(), original = !a.projectPath.empty() ? a.projectPath : a.openedFrom] {
    const std::string tmp = file + ".writing";
    {
      std::ofstream f(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
      f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::error_code ec;
    fs::rename(fs::u8path(tmp), fs::u8path(file), ec);
    gCopy.lastOk.store(!ec);
    if (!ec) writeMeta(original, unixNow());
    else logging::warning("app", "Could not write the recovery copy");
    gCopy.writing.store(false);
  });
}

void shutdown(App& a) {
  crash::setRescue(nullptr);
  joinWriter();
  // Unsaved work on a clean end: the user answered the "save changes?" question; the copy goes too.
  if (!gCopy.dir.empty()) crash::discardRecovery(a.configDir(), gCopy.sid);
}

void recoverFrom(App& a, const std::string& reportPath) {
  const crash::PendingReport r = crash::readReport(a.configDir(), reportPath);
  crash::markSeen(reportPath);
  if (r.recoveryFile.empty()) {
    a.notify("There is no recovery copy for this report", true);
    return;
  }
  gCopy.recoveredSid = r.sessionId;
  gCopy.recoveredOriginal = r.originalProject;
  gCopy.recovering = true;
  crash::crumb("recover project");
  a.openProject(r.recoveryFile);
}

bool isRecovering() { return gCopy.recovering; }

void recoveredLoaded(App& a) {
  gCopy.recovering = false;
  // The copy lives in Gridline's own folder: Save goes to the project's real home, and asks when
  // that was not a Gridline file.
  const std::string& o = gCopy.recoveredOriginal;
  const bool ownFormat = o.size() > 5 && (o.rfind(".json") == o.size() - 5 || o.rfind(".gridline") == o.size() - 9);
  a.projectPath = ownFormat ? o : std::string();
  a.openedFrom = ownFormat ? std::string() : o;
  a.doc.markUnsaved();
  a.notify("Recovered project: check it, then save to keep it");
}

void draw(App& a) {
  crash::Recoverable rec;
  if (!gShown.active && crash::takeRecoverable(rec)) showRecoverable(rec);
  showPending();
  if (!gShown.active) return;
  if (!ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
  ui::PlaceNextDialog(ImVec2(kWidth + 2 * theme::kS6, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  const bool open = ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleVar();
  if (!open) return;
  const int hit = drawBody(gShown.content, a.fontBold, a.fontMono, gShown.copiedAt, kCodeHeight, kWidth);
  if (hit >= 0) {
    const crash::PendingReport r = gShown.report;
    const bool recoverable = gShown.recoverable, hadCopy = !gShown.report.recoveryFile.empty();
    gShown.active = false;
    ImGui::CloseCurrentPopup();
    if (!recoverable) {
      crash::markSeen(r.path);
      if (hadCopy && hit == 0) recoverFrom(a, r.path);
    }
  }
  ImGui::EndPopup();
}

KeepsFailingResult drawKeepsFailing(App& a) {
  KeepsFailingResult out;
  static double copiedAt = -100;
  static Content c;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    crash::Recoverable r;
    while (crash::takeRecoverable(r)) c.code = r.code, c.id = r.id;
    c.title = tr("The interface keeps failing");
    c.lead = tr("Drawing the interface failed several times in a row, so Gridline stopped trying. Your project is still in memory. You can save a copy and restart, or try again.");
    c.buttons[0] = "Save a copy and restart";
    c.buttons[1] = "Try again";
    gShown.active = false;
  }
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(display);
  // Centred, about as tall as the same text is in a dialog.
  const float padX = std::max<float>(theme::kS6, (display.x - kWidth) * 0.5f), padY = std::max<float>(theme::kS6, (display.y - 440.0f) * 0.5f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
  ImGui::Begin("##crashfail", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleVar();
  const float w = std::min(kWidth, ImGui::GetContentRegionAvail().x);
  const int hit = drawBody(c, a.fontBold, a.fontMono, copiedAt, kCodeHeight, w);
  ImGui::End();
  if (hit == 1) {
    out.done = true;
    out.retry = true;
    loaded = false;
  } else if (hit == 0) {
    out.done = true;
    if (!gCopy.dir.empty()) {
      joinWriter();
      if (gl::saveProject(copyPath(), a.doc.song(), nullptr)) {
        writeMeta(!a.projectPath.empty() ? a.projectPath : a.openedFrom, unixNow());
        // The report the restart recovers from says which session the copy belongs to.
        std::string report = (fs::u8path(crash::reportsDir(a.configDir())) / ("failed-" + gCopy.sid + ".txt")).u8string();
        std::ofstream(fs::u8path(report)) << c.code;
        crash::startSelf({"--crash-recover", report});
      } else {
        crash::startSelf({});
      }
    } else {
      crash::startSelf({});
    }
  }
  return out;
}

// ---- the dialog process ----
struct DialogState {
  std::string configDir, reportPath, sessionFile;
  crash::SessionView* session = nullptr;
  crash::PendingReport report;
  Content content;
  double copiedAt = -100;
  bool frozenMode = false;
  bool finished = false;
};

namespace {

void markShowing(const std::string& report, bool on) {
  std::error_code ec;
  const fs::path mark = fs::u8path(report + ".showing");
  if (!on) {
    fs::remove(mark, ec);
    return;
  }
  std::ofstream(mark) << crash::ownPid() << "\n";
}

void setCrashContent(DialogState* s) {
  const crash::PendingReport& r = s->report;
  Content c;
  c.title = titleFor(r.kind);
  c.lead = leadFor(r);
  c.work = workLine(r);
  c.code = r.text;
  c.id = r.id;
  if (!r.recoveryFile.empty()) {
    c.buttons[0] = "Restart and recover project";
    c.buttons[1] = "Restart";
    c.buttons[2] = "Close";
  } else {
    c.buttons[0] = "Restart";
    c.buttons[1] = "Close";
  }
  s->content = std::move(c);
  s->frozenMode = false;
}

void setFrozenContent(DialogState* s) {
  const crash::PendingReport& r = s->report;
  Content c;
  c.title = titleFor("freeze");
  c.lead = leadFor(r);
#if defined(_WIN32)  // no copy can be taken from outside here: the last automatic one is what there is
  c.work = r.recoveryFile.empty() ? tr("Force restart ends the frozen window. Changes since your last save are lost.")
                                  : tr("Force restart ends the frozen window. You can open the last automatic recovery copy of your project after the restart.");
#else
  c.work = r.recoveryFile.empty() ? tr("Force restart ends the frozen window. Gridline first tries to save a recovery copy of your project.")
                                  : tr("Force restart ends the frozen window. Gridline first tries to save a fresh recovery copy of your project, and you can open it after the restart.");
#endif
  c.code = r.text;
  c.id = r.id;
  c.buttons[0] = "Keep waiting";
  c.buttons[1] = "Force restart";
  c.dangerButton = 1;
  s->content = std::move(c);
  s->frozenMode = true;
}

}  // namespace

DialogState* dialogOpen(const std::string& configDir, const std::string& reportPath, const std::string& sessionFile) {
  auto* s = new DialogState;
  s->configDir = configDir;
  s->reportPath = reportPath;
  s->sessionFile = sessionFile;
  s->report = crash::readReport(configDir, reportPath);
  if (!sessionFile.empty()) s->session = crash::openSession(sessionFile);
  markShowing(reportPath, true);
  const bool live = s->session && crash::processAlive(crash::sessionPid(s->session));
  if (s->report.kind == "freeze" && live) setFrozenContent(s);
  else setCrashContent(s);
  return s;
}

void dialogClose(DialogState* s) {
  if (!s) return;
  markShowing(s->reportPath, false);
  crash::closeSession(s->session);
  std::error_code ec;
  if (!s->sessionFile.empty() && !(s->session && crash::processAlive(crash::sessionPid(s->session)))) fs::remove(fs::u8path(s->sessionFile), ec);
  delete s;
}

void dialogSize(int& w, int& h) {
  w = static_cast<int>(kWidth + 2 * theme::kS6);
  h = 530;
}

bool dialogFrame(DialogState* s, const DialogFonts& fonts) {
  if (s->frozenMode) {
    // The frozen process is watched from here: it came back, or it went.
    const bool alive = crash::processAlive(crash::sessionPid(s->session));
    if (alive && !crash::sessionFrozen(s->session)) return false;
    if (!alive) {
      // Its report may have changed (a crash on top of the freeze).
      std::vector<crash::PendingReport> reports = crash::collectPending(s->configDir);
      for (const auto& r : reports)
        if (r.sessionId == s->report.sessionId) {
          markShowing(s->reportPath, false);
          s->reportPath = r.path;
          s->report = r;
          markShowing(s->reportPath, true);
        }
      setCrashContent(s);
    }
  }
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("##crashdialog", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar(2);
  const float width = ImGui::GetContentRegionAvail().x;
  const int hit = drawBody(s->content, fonts.bold, fonts.mono, s->copiedAt, 0.0f, width);
  ImGui::End();
  if (hit < 0) return true;
  if (s->frozenMode) {
    if (hit == 0) {
      crash::sessionSnooze(s->session);
      return false;
    }
    crash::sessionForceEnd(s->session, 8000);
    s->report = crash::readReport(s->configDir, s->reportPath);
    crash::markSeen(s->reportPath);
    crash::startSelf(s->report.recoveryFile.empty() ? std::vector<std::string>{} : std::vector<std::string>{"--crash-recover", s->reportPath});
    return false;
  }
  const bool hasCopy = !s->report.recoveryFile.empty();
  if (hasCopy ? hit == 0 : false) {
    crash::markSeen(s->reportPath);
    crash::startSelf({"--crash-recover", s->reportPath});
    return false;
  }
  const int restart = hasCopy ? 1 : 0;
  if (hit == restart) {
    // The copy stays until the user decides: the next start's pop-up offers it again.
    if (!hasCopy) crash::markSeen(s->reportPath);
    crash::startSelf({});
    return false;
  }
  if (!hasCopy) crash::markSeen(s->reportPath);
  return false;
}

}  // namespace gl::crashui
