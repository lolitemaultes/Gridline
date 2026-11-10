// Gridline - J-219: the Track Receipt window (see TrackReceiptUi.h).
#include "TrackReceiptUi.h"
#include "Keys.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/TrackReceipt.h"
#include "../engine/TrackReceiptPass.h"
#include "../host/HostedPlugins.h"
#include "App.h"
#include "CollabUi.h"
#include "ContextMenus.h"
#include "DevSignal.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"
#include "WorkerStall.h"

namespace fs = std::filesystem;

namespace gl::receiptui {

namespace {

using theme::pal;
using namespace gl::receipt;

constexpr float kW = 720.0f;  // content width

struct Job {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0.0};
  std::mutex mu;
  Measurement result;
  std::string error;  // K-133: why the measurement stopped (guarded by mu); shown to the person
  WorkerBeat beat;    // K-133: each progress step
  StallWatch watch;
  void join() {
    if (worker.joinable()) worker.join();
  }
};
Job& J() {
  static Job j;
  return j;
}

struct State {
  bool open = false, focus = false;
  int page = 0;  // 0 picker, 1 measuring, 2 results
  char search[128] = {};
  Measurement scanned, measured;
  Report pick, report;
  Saved saved;
  std::vector<FileRef> refs;
  std::string lastFile;
  double demoProgress = -1;  // screenshots: a frozen progress page
};
State& S() {
  static State s;
  return s;
}

std::string projectName(const App& a) {
  if (a.projectPath.empty()) return "Untitled";
  return fs::u8path(a.projectPath).stem().u8string();
}

void rebuild(App& a) {
  State& s = S();
  s.pick = build(s.scanned, s.saved, projectName(a));
  if (s.page == 2) s.report = build(s.measured, s.saved, projectName(a));
}

void commit(App& a) {
  State& s = S();
  const std::string json = savedToJson(s.saved);
  if (json != a.doc.song().trackReceipt) a.doc.editSong("Track Receipt", [&](Song& x) { x.trackReceipt = json; });
  rebuild(a);
}

// A text field bound to a std::string; true when an edit is finished (focus left / Enter).
int resizeCb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* str = static_cast<std::string*>(d->UserData);
    str->resize(static_cast<size_t>(d->BufTextLen));
    d->Buf = str->data();
  }
  return 0;
}
bool field(const char* id, std::string& v, float w, const char* hint = "") {
  ImGui::SetNextItemWidth(w);
  if (v.capacity() < 64) v.reserve(64);
  ImGui::InputTextWithHint(id, hint, v.data(), v.capacity() + 1, ImGuiInputTextFlags_CallbackResize, resizeCb, &v);
  return ImGui::IsItemDeactivatedAfterEdit();
}

void label(const char* t, float w) {
  const auto& P = pal();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", t);
  ImGui::SameLine(w);
}

// The used parts of one original on a bar (seconds 0..length).
void timelineBar(const Timeline& t, float w) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 10.0f;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), P.well, h * 0.5f);
  const double len = std::max(t.length, 0.001);
  for (const Range& r : t.used) {
    const float a = p.x + w * static_cast<float>(std::clamp(r.a / len, 0.0, 1.0));
    const float b = p.x + w * static_cast<float>(std::clamp(r.b / len, 0.0, 1.0));
    dl->AddRectFilled(ImVec2(a, p.y), ImVec2(std::max(a + 1.5f, b), p.y + h), P.accent);
  }
  ImGui::Dummy(ImVec2(w, h));
  ImGui::PushFont(nullptr, theme::kFontLabel);
  ImGui::TextColored(theme::vec(P.textFaint), "0:00");
  const std::string end = timeText(t.length);
  ImGui::SameLine(w - ImGui::CalcTextSize(end.c_str()).x + ImGui::GetStyle().WindowPadding.x * 0.0f);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", end.c_str());
  ImGui::PopFont();
}

std::string tierText(const SongRow& s) {
  if (s.seconds <= 0) return i18n::tr("Not played");
  if (s.over) return i18n::tr("Over 60 s: not licensable");
  char b[64];
  std::snprintf(b, sizeof b, i18n::tr("Up to %d s"), s.tier);
  return b;
}

void chip(const std::string& t, ImU32 bg, ImU32 fg) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 sz(ts.x + 16, ts.y + 4);
  dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, sz.y * 0.5f);
  dl->AddText(ImVec2(p.x + 8, p.y + 2), fg, t.c_str());
  ImGui::Dummy(sz);
}

std::string songDisplay(const std::string& key, const Report& r) {
  for (const SongRow& s : r.songs)
    if (s.key == key) return s.artist.empty() ? s.title : s.artist + " - " + s.title;
  return key;
}

// ---- measuring

void startMeasure(App& a) {
  Job& j = J();
  if (j.running.load()) return;
  j.join();
  State& s = S();
  s.page = 1;
  s.demoProgress = -1;
  j.cancel = false;
  j.finished = false;
  j.progress = 0;
  workerstall::start(j.watch, j.beat);
  j.running = true;
  Song song = a.doc.song();
  const int rate = a.audioSampleRate > 0 ? a.audioSampleRate : 44100;
  j.worker = std::thread([&j, song, rate]() {
    host::UiThreadRenderScope scope;  // hosted plugins: as the render dialog does
    PassOptions o;
    o.sampleRate = rate;
    o.cancel = &j.cancel;
    o.progress = [&j](double f) {
      j.beat.beat();
      j.progress = f;
    };
    Measurement m;
    std::string error;
    try {  // an exception must not leave this thread
      if (std::getenv("GRIDLINE_DEV_RECEIPT_FAIL")) throw std::runtime_error("a test failure");  // K-133 tests
      m = measure(song, o);
    } catch (const std::exception& e) {
      error = e.what();
    } catch (...) {
      error = "an internal error";
    }
    if (!error.empty()) logging::error("receipt", "the measurement stopped: " + error);
    {
      std::lock_guard<std::mutex> lk(j.mu);
      j.result = std::move(m);
      j.error = std::move(error);
    }
    j.running = false;
    j.finished = true;
  });
}

void finish(App& a) {
  Job& j = J();
  if (!j.finished.exchange(false)) return;
  j.join();
  State& s = S();
  Measurement m;
  std::string error;
  {
    std::lock_guard<std::mutex> lk(j.mu);
    m = std::move(j.result);
    error = std::move(j.error);
    j.error.clear();
  }
  if (!error.empty()) {  // K-133: told, not only logged; the files page is where it started
    s.page = 0;
    a.notify("The Track Receipt could not be calculated. Nothing was changed: try Calculate again.", true, error);
    devsig::reach("receipt-failed");
    return;
  }
  if (m.cancelled) {
    s.page = 0;
    devsig::reach("receipt-cancelled");
    return;
  }
  s.measured = std::move(m);
  s.page = 2;
  rebuild(a);
  devsig::reach("receipt-done");
}

// ---- outputs

bool writeText(const std::string& path, const std::string& text) {
  std::ofstream f(fs::u8path(path), std::ios::binary);
  f << text;
  return static_cast<bool>(f);
}

void openFile(const std::string& path) {
  std::string url = "file://";
  for (unsigned char ch : fs::absolute(fs::u8path(path)).generic_u8string()) {
    if (std::isalnum(ch) || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      url += static_cast<char>(ch);
    } else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", ch);
      url += b;
    }
  }
  collabui::openUrl(url);
}

// Writes `name` into the project folder, or asks where while the project is unsaved.
void saveOutput(App& a, const std::string& name, const std::string& ext, std::function<std::string()> make, bool openAfter) {
  auto write = [&a, make, openAfter](const std::string& path) {
    if (!writeText(path, make())) {
      a.notify("Could not write " + fs::u8path(path).filename().u8string(), true);
      return;
    }
    S().lastFile = path;
    a.notify("Saved " + fs::u8path(path).filename().u8string(), false, fs::u8path(path).parent_path().u8string());
    if (openAfter && !std::getenv("GRIDLINE_TRACK_RECEIPT_DEMO")) openFile(path);
  };
  if (!a.projectPath.empty()) {
    write((fs::u8path(a.projectPath).parent_path() / fs::u8path(name)).u8string());
    return;
  }
  menus::fileDialog(a, i18n::tr("Save Track Receipt"), true, {ext}, name, write);
}

// ---- pages

void pickerPage(App& a) {
  State& s = S();
  const auto& P = pal();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     i18n::tr("Every audio file this project uses. Tick the files that are licensed samples: files named like "
                              "Tracklib downloads are ticked for you. Calculate then plays the project through and measures "
                              "which parts of each original are used."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ImGui::SetNextItemWidth(kW - ui::ButtonRowWidth(2, 90) - theme::kS2);
  ImGui::InputTextWithHint("##rsearch", i18n::tr("Search files"), s.search, sizeof s.search);
  const std::string q = lowerText(s.search);
  auto visible = [&](const FileRow& f) { return q.empty() || lowerText(f.name).find(q) != std::string::npos || lowerText(f.song).find(q) != std::string::npos; };
  ImGui::SameLine(0, theme::kS2);
  int setAll = -1;
  if (ui::TextButton(i18n::tr("Tick all"), false, 90)) setAll = 1;
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::tr("Tick none"), false, 90)) setAll = 0;
  if (setAll >= 0) {
    for (const FileRow& f : s.pick.files)
      if (visible(f)) s.saved.files[f.path].ticked = setAll;
    commit(a);
  }
  // Groups by song.
  std::map<std::string, std::vector<const FileRow*>> groups;
  for (const FileRow& f : s.pick.files)
    if (visible(f)) groups[f.song].push_back(&f);
  std::vector<std::string> keys;
  for (const auto& [k, v] : groups) keys.push_back(k);
  std::stable_sort(keys.begin(), keys.end(), [&](const std::string& x, const std::string& y) {
    auto ticked = [&](const std::string& k) {
      for (const FileRow* f : groups[k])
        if (f->ticked) return true;
      return false;
    };
    return ticked(x) > ticked(y);
  });
  std::set<std::string> allSongs;
  for (const FileRow& f : s.pick.files) allSongs.insert(f.song);
  ImGui::BeginChild("##rpick", ImVec2(kW, 360), ImGuiChildFlags_Borders);
  if (s.pick.files.empty())
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("This project uses no audio files."));
  std::string linkFile;
  for (const std::string& k : keys) {
    ImGui::PushID(k.c_str());
    ImGui::PushFont(a.fontBold, 0.0f);
    ImGui::TextUnformatted(songDisplay(k, s.pick).c_str());
    ImGui::PopFont();
    for (const SongRow& sr : s.pick.songs)
      if (sr.key == k && sr.checkName) {
        ImGui::SameLine(0, theme::kS2);
        ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("check artist and title"));
      }
    for (const FileRow* f : groups[k]) {
      ImGui::PushID(f->path.c_str());
      bool on = f->ticked;
      if (ImGui::Checkbox("##tick", &on)) {
        s.saved.files[f->path].ticked = on ? 1 : 0;
        commit(a);
      }
      ImGui::SameLine();
      const float nameW = kW - 260;
      ImGui::TextColored(theme::vec(f->ticked ? P.text : P.textDim), "%s", theme::fitTextMiddle(f->name.c_str(), nameW));
      if (ImGui::IsItemHovered()) ui::Tooltip(f->path.c_str());
      ImGui::SameLine(nameW + 36);
      int nUses = 0;
      for (const FileRef& r : s.refs) nUses += r.path == f->path ? 1 : 0;
      std::string note = f->derived ? std::string(i18n::tr("derived")) : !f->exists ? std::string(i18n::tr("missing")) : "";
      char b[64];
      std::snprintf(b, sizeof b, nUses == 1 ? i18n::tr("%d place") : i18n::tr("%d places"), nUses);
      ImGui::TextColored(theme::vec(!f->exists ? P.danger : P.textFaint), "%s%s%s", b, note.empty() ? "" : ", ", note.c_str());
      ImGui::SameLine(kW - 96);
      if (ui::TextButton(i18n::tr("Song..."), false, 80, i18n::tr("Link this file to an original song"))) {
        linkFile = f->path;
        ImGui::OpenPopup("##link");
      }
      if (ImGui::BeginPopup("##link")) {
        if (ImGui::MenuItem(i18n::tr("Automatic (from the file name)"))) {
          s.saved.files[f->path].song.clear();
          commit(a);
        }
        ImGui::Separator();
        for (const std::string& song : allSongs)
          if (ImGui::MenuItem(songDisplay(song, s.pick).c_str(), nullptr, f->song == song)) {
            s.saved.files[f->path].song = song;
            commit(a);
          }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  ImGui::EndChild();
}

void measuringPage(App& a) {
  (void)a;
  const auto& P = pal();
  const double f = S().demoProgress >= 0 ? S().demoProgress : J().progress.load();
  ImGui::Dummy(ImVec2(kW, theme::kS6));
  ImGui::TextColored(theme::vec(P.text), "%s", i18n::tr("Playing the project through to measure what each sample voice reads..."));
  ImGui::Spacing();
  ImGui::ProgressBar(static_cast<float>(f), ImVec2(kW, 0), "");
  ImGui::TextColored(theme::vec(P.textDim), "%d%%", static_cast<int>(f * 100));
  ImGui::Dummy(ImVec2(kW, theme::kS6));
}

void songCard(App& a, SongRow& sr, int idx) {
  State& s = S();
  const auto& P = pal();
  ImGui::PushID(idx);
  ImGui::PushID(sr.key.c_str());
  const float x0 = ImGui::GetCursorPosX();
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(sr.title.c_str());
  ImGui::PopFont();
  {
    char secs[32];
    std::snprintf(secs, sizeof secs, "%s s", secondsText(sr.seconds).c_str());
    ImGui::PushFont(a.fontBold, theme::kFontTitle);
    const float w = ImGui::CalcTextSize(secs).x;
    ImGui::SameLine(x0 + kW - w - 16);
    ImGui::TextColored(theme::vec(sr.over ? P.danger : P.text), "%s", secs);
    ImGui::PopFont();
  }
  ImGui::TextColored(theme::vec(P.textDim), "%s", sr.artist.empty() ? i18n::tr("Artist unknown") : sr.artist.c_str());
  {
    const std::string t = tierText(sr);
    const float w = ImGui::CalcTextSize(t.c_str()).x + 16;
    ImGui::SameLine(x0 + kW - w - 16);
    chip(t, sr.over ? P.danger : sr.seconds <= 0 ? P.well : theme::withAlpha(P.accent, 0.3f), sr.over ? P.paper : P.text);
  }
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  ImGui::PushTextWrapPos(x0 + kW - 140);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", sr.sourceName.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  if (sr.checkName) ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("Check artist and title: guessed from the file name."));
  SongFields& f = s.saved.songs[sr.key];
  const float lw = 70, half = (kW - 16) / 2, fw = half - lw - theme::kS2;
  label(i18n::tr("Artist"), x0 + lw);
  if (field("##artist", f.artist, fw, sr.artist.c_str())) commit(a);
  ImGui::SameLine(x0 + half + theme::kS2);
  label(i18n::tr("Title"), x0 + half + theme::kS2 + lw);
  if (field("##title", f.title, fw, sr.title.c_str())) commit(a);
  for (const Timeline& t : sr.timelines) {
    ImGui::Spacing();
    char head[160];
    std::snprintf(head, sizeof head, "%s  %s s %s",
                  t.part > 0 ? (std::string(i18n::tr("Part")) + " " + std::to_string(t.part)).c_str()
                  : t.part < 0 ? i18n::tr("Derived files, part of the original unknown")
                               : i18n::tr("Original"),
                  secondsText(t.seconds).c_str(), i18n::tr("used"));
    ImGui::PushFont(nullptr, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textDim), "%s", head);
    ImGui::PopFont();
    timelineBar(t, kW - 16);
    for (const RangeFrom& rf : t.parts) {
      ImGui::TextColored(theme::vec(P.accent), "%s - %s", timeText(rf.r.a).c_str(), timeText(rf.r.b).c_str());
      ImGui::SameLine(x0 + 130);
      ImGui::TextColored(theme::vec(P.textDim), "%s s", secondsText(rf.r.b - rf.r.a).c_str());
      ImGui::SameLine(x0 + 190);
      std::string from;
      for (const auto& fr : rf.from) from += (from.empty() ? "" : "; ") + fr;
      if (rf.estimated) from += std::string("  ") + i18n::tr("(not measured: whole file, type the parts below)");
      ImGui::PushTextWrapPos(x0 + kW - 16);
      ImGui::TextColored(theme::vec(rf.estimated ? P.danger : P.textFaint), "%s", from.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::Spacing();
  // Fields from the license.
  const float cw = (kW - 16 - 2 * theme::kS4) / 3.0f;
  {
    static const char* cats[] = {"", "A", "B", "C"};
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Category"));
    ImGui::SameLine(x0 + 70);
    ImGui::SetNextItemWidth(cw - 70);
    if (ImGui::BeginCombo("##cat", f.category.empty() ? "-" : f.category.c_str())) {
      for (const char* c : cats)
        if (ImGui::Selectable(*c ? c : "-", f.category == c)) {
          f.category = c;
          commit(a);
        }
      ImGui::EndCombo();
    }
    ImGui::SameLine(x0 + cw + theme::kS4);
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Share %"));
    ImGui::SameLine(x0 + cw + theme::kS4 + 70);
    char autoHint[48];
    if (sr.autoShare >= 0) std::snprintf(autoHint, sizeof autoHint, i18n::tr("%g (auto)"), sr.autoShare);
    else std::snprintf(autoHint, sizeof autoHint, "%s", i18n::tr("auto"));
    if (field("##share", f.share, cw - 70, autoHint)) commit(a);
    ImGui::SameLine(x0 + 2 * (cw + theme::kS4));
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Fee"));
    ImGui::SameLine(x0 + 2 * (cw + theme::kS4) + 70);
    if (field("##fee", f.fee, cw - 70)) commit(a);
  }
  {
    char note[160] = "";
    if (sr.over || sr.seconds <= 0) {
    } else if (f.category.empty()) {
      std::snprintf(note, sizeof note, "%s", i18n::tr("Choose the category on the license and the share is calculated from it and the length."));
    } else if (f.share.empty()) {
      std::snprintf(note, sizeof note, i18n::tr("Calculated: category %s, up to %d s. Type a share if your license says otherwise."), f.category.c_str(), sr.tier);
    } else if (sr.autoShare >= 0 && sr.share != sr.autoShare) {
      std::snprintf(note, sizeof note, i18n::tr("Your typed share is used. Calculated: %g%%. Clear the field to use it."), sr.autoShare);
    }
    if (*note) {
      ImGui::PushFont(nullptr, theme::kFontLabel);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", note);
      ImGui::PopFont();
    }
  }
  label(i18n::tr("Rights holders"), x0 + 110);
  if (field("##holders", f.holders, kW - 16 - 110, i18n::tr("Master and publishing owners, from the license"))) commit(a);
  // Parts typed by the user for uses Gridline cannot measure.
  for (const std::string& path : sr.files) {
    const FileRow* row = nullptr;
    for (const FileRow& fr : s.report.files)
      if (fr.path == path) row = &fr;
    if (!row || !row->estimated) continue;
    ImGui::PushID(path.c_str());
    FileFields& ff = s.saved.files[path];
    ImGui::TextColored(theme::vec(P.textDim), "%s", theme::fitTextMiddle(row->name.c_str(), kW - 16));
    label(i18n::tr("Parts used"), x0 + 110);
    Ranges tmp;
    const bool ok = ff.ranges.empty() || parseRanges(ff.ranges, tmp);
    if (!ok) ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.danger));
    if (field("##ranges", ff.ranges, kW - 16 - 110, i18n::tr("0:01.0-0:03.0, 0:10-0:12 (empty: the whole file)"))) commit(a);
    if (!ok) ImGui::PopStyleColor();
    ImGui::PopID();
  }
  for (const auto& w : sr.warnings) {
    ImGui::PushTextWrapPos(x0 + kW - 16);
    ImGui::TextColored(theme::vec(P.danger), "%s", w.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::PopID();
  ImGui::PopID();
}

void resultsPage(App& a) {
  State& s = S();
  const auto& P = pal();
  Report& r = s.report;
  {
    double secs = 0;
    int songs = 0, over = 0;
    for (const SongRow& sr : r.songs)
      if (sr.seconds > 0) ++songs, secs += sr.seconds, over += sr.over ? 1 : 0;
    char b[200];
    std::snprintf(b, sizeof b, i18n::tr("%d original songs, %s s used in all"), songs, secondsText(secs).c_str());
    ImGui::TextUnformatted(b);
    if (over) {
      ImGui::SameLine(0, theme::kS2);
      std::snprintf(b, sizeof b, i18n::tr("%d over 60 s"), over);
      ImGui::TextColored(theme::vec(P.danger), "%s", b);
    }
    ImGui::SameLine(0, theme::kS2);
    if (r.sharesComplete) std::snprintf(b, sizeof b, i18n::tr("sample shares %s%%, yours %s%%"), secondsText(r.totalShare).c_str(),
                                         secondsText(std::max(0.0, 100 - r.totalShare)).c_str());
    else std::snprintf(b, sizeof b, "%s", i18n::tr("shares to fill in: choose each song's category"));
    ImGui::TextColored(theme::vec(P.textDim), "%s", b);
  }
  for (const auto& w : r.warnings) ImGui::TextColored(theme::vec(P.danger), "%s", w.c_str());
  ImGui::BeginChild("##rres", ImVec2(kW + 16, 430), ImGuiChildFlags_Borders);
  if (r.songs.empty())
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No ticked files: go back and tick the licensed samples."));
  int idx = 0;
  for (size_t i = 0; i < r.songs.size(); ++i) {  // (a copy: an edit in the card rebuilds the report)
    SongRow sr = r.songs[i];
    songCard(a, sr, idx++);
  }
  // Not counted.
  bool anyMuted = false;
  for (const FileRow& f : r.files) anyMuted = anyMuted || !f.muted.empty();
  if (anyMuted) {
    ImGui::PushFont(a.fontBold, 0.0f);
    ImGui::TextUnformatted(i18n::tr("Not counted (muted or silent)"));
    ImGui::PopFont();
    for (const FileRow& f : r.files)
      for (const Use& u : f.muted) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW - 16);
        ImGui::TextColored(theme::vec(P.textFaint), "%s: %s, %s", f.name.c_str(), u.where.c_str(), u.note.c_str());
        ImGui::PopTextWrapPos();
      }
    ImGui::Spacing();
    ImGui::Separator();
  }
  // Release and writers.
  ImGui::Spacing();
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(i18n::tr("Your release"));
  ImGui::PopFont();
  Release& rel = s.saved.release;
  const float lw = 110, half = (kW - 16) / 2;
  const float x0 = ImGui::GetCursorPosX();
  struct F {
    const char* label;
    std::string* v;
    const char* hint;
  };
  const F rows[] = {{i18n::tr("Title"), &rel.title, ""},          {i18n::tr("Performer"), &rel.performer, ""},
                    {i18n::tr("Licensee"), &rel.licensee, ""},    {i18n::tr("ISRC"), &rel.isrc, ""},
                    {i18n::tr("Release date"), &rel.date, "YYYY-MM-DD"}, {i18n::tr("Distributor"), &rel.distributor, ""}};
  for (size_t i = 0; i < 6; ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (i % 2) ImGui::SameLine(x0 + half + theme::kS2);
    const float bx = i % 2 ? x0 + half + theme::kS2 : x0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", rows[i].label);
    ImGui::SameLine(bx + lw);
    if (field("##rel", *rows[i].v, half - lw - theme::kS2, rows[i].hint)) commit(a);
    ImGui::PopID();
  }
  ImGui::Spacing();
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(i18n::tr("Your writers and publishers"));
  ImGui::PopFont();
  {
    const char* hdr[5] = {i18n::tr("Name"), i18n::tr("Role"), i18n::tr("PRO"), i18n::tr("IPI"), i18n::tr("Share of yours %")};
    const float cols[5] = {0, 0.30f, 0.50f, 0.66f, 0.84f};
    const float cw = kW - 16 - 30;
    for (int i = 0; i < 5; ++i) {
      if (i) ImGui::SameLine(x0 + cw * cols[i]);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", hdr[i]);
    }
    int remove = -1;
    for (size_t k = 0; k < s.saved.writers.size(); ++k) {
      Writer& w = s.saved.writers[k];
      ImGui::PushID(static_cast<int>(k));
      std::string* v[5] = {&w.name, &w.role, &w.pro, &w.ipi, &w.share};
      for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(x0 + cw * cols[i]);
        ImGui::PushID(i);
        const float next = i < 4 ? cols[i + 1] : 1.0f;
        if (field("##w", *v[i], cw * (next - cols[i]) - theme::kS1)) commit(a);
        ImGui::PopID();
      }
      ImGui::SameLine(x0 + cw + theme::kS1);
      if (ui::TextButton("x", false, 24, i18n::tr("Remove"))) remove = static_cast<int>(k);
      ImGui::PopID();
    }
    if (remove >= 0) {
      s.saved.writers.erase(s.saved.writers.begin() + remove);
      commit(a);
    }
    if (ui::TextButton(i18n::tr("Add writer"), false, 0)) {
      s.saved.writers.push_back({});
      commit(a);
    }
  }
  ImGui::Spacing();
  ImGui::EndChild();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kW);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("A helper, not legal advice. Check the figures against your license on Tracklib."));
  ImGui::PopTextWrapPos();
}

}  // namespace

void open(App& a) {
  State& s = S();
  s.open = true;
  s.focus = true;
  if (!J().running.load()) s.page = 0;
  s.saved = savedFromJson(a.doc.song().trackReceipt);
  s.refs = fileRefs(a.doc.song());
  s.scanned = scan(a.doc.song(), false);
  rebuild(a);
}

void draw(App& a) {
  finish(a);
  if (J().running.load()) workerstall::watch(a, J().watch, J().beat, "Track Receipt", "");
  State& s = S();
  // Screenshots: GRIDLINE_TRACK_RECEIPT_DEMO=picker|progress|results opens a page on the project given
  // (K-133: calculate also starts the measurement, as the Calculate button does).
  static int frames = 0;
  if (const char* env = std::getenv("GRIDLINE_TRACK_RECEIPT_DEMO"); env && *env && ++frames == 4) {
    open(a);
    const std::string mode = env;
    if (mode == "calculate") {
      startMeasure(a);
    } else if (mode == "progress") {
      s.page = 1;
      s.demoProgress = 0.42;
    } else if (mode == "results") {
      PassOptions o;
      o.sampleRate = 44100;
      s.measured = measure(a.doc.song(), o);
      s.page = 2;
      rebuild(a);
    }
  }
  if (!s.open) return;
  Job& j = J();
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ui::PlaceNextDialog(ImVec2(kW + 16 + 2 * ImGui::GetStyle().WindowPadding.x, 0));
  if (s.focus) ImGui::SetNextWindowFocus(), s.focus = false;
  bool open = true;
  const std::string title = std::string(i18n::tr("Track Receipt")) + "###trackreceipt";
  const bool vis = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
  ui::TrackWindowInViewport();
  if (vis) {
    {  // steps
      const auto& P = pal();
      const char* steps[3] = {i18n::tr("1  Files"), i18n::tr("2  Calculate"), i18n::tr("3  Results")};
      for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, theme::kS4);
        ImGui::TextColored(theme::vec(i == s.page ? P.accent : P.textFaint), "%s", steps[i]);
      }
      ImGui::Separator();
    }
    if (s.page == 0) pickerPage(a);
    else if (s.page == 1) measuringPage(a);
    else resultsPage(a);
    const float bw = 150;
    if (s.page == 0) {
      ui::DialogFooter(ui::ButtonRowWidth(2, bw));
      if (ui::TextButton(i18n::tr("Close"), false, bw)) open = false;
      ImGui::SameLine(0, theme::kS2);
      if (ui::PrimaryButton(i18n::tr("Calculate"), bw, i18n::tr("Plays the project offline and measures the parts used"))) startMeasure(a);
    } else if (s.page == 1) {
      ui::DialogFooter(ui::ButtonRowWidth(1, bw));
      if (ui::TextButton(i18n::tr("Cancel"), false, bw) || ::gl::keys::escapePressed()) {
        j.cancel = true;
        if (s.demoProgress >= 0) s.page = 0, s.demoProgress = -1;
      }
    } else {
      ui::DialogFooter(ui::ButtonRowWidth(3, bw));
      if (ui::TextButton(i18n::tr("Back"), false, bw)) s.page = 0;
      ImGui::SameLine(0, theme::kS2);
      if (ui::TextButton(i18n::tr("Save Track Receipt"), false, bw, i18n::tr("A spreadsheet for your license: Licenses, Usage, Release, Splits"))) {
        const Report r = s.report;
        saveOutput(a, receiptFileName(projectName(a)), ".xml", [r]() { return spreadsheetXml(r); }, false);
      }
      ImGui::SameLine(0, theme::kS2);
      if (ui::PrimaryButton(i18n::tr("View results"), bw, i18n::tr("Saves the results as a PDF and opens it"))) {
        const Report r = s.report;
        saveOutput(a, resultsFileName(projectName(a)), ".pdf", [r]() { return resultsPdf(r); }, true);
      }
    }
  }
  ImGui::End();
  if (!open) {
    if (j.running.load()) j.cancel = true;
    s.open = false;
  }
}

void shutdown() {
  J().cancel = true;
  host::serveUiThreadTasksWhile(J().running);
  J().join();
}

}  // namespace gl::receiptui
