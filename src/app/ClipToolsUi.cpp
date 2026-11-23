// Gridline: the audio clip menus' Audio tools (ClipToolsUi.h).
#include "ClipToolsUi.h"
#include "../core/ThreadName.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../engine/ClipTools.h"
#include "../packs/edison/edison_dialog.h"
#include "../packs/edison/edison_repair_ui.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "ClipEditor.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::cliptoolsui {

namespace ed = packs::edison;
namespace ct = cliptools;
using ed::ToolKind;

namespace {

struct Row {
  ToolKind kind;
  const char* label;  // the menu row
  const char* dev;    // dev name
  const char* hint;
};
// The rows in the submenu's order, a separator after each group (sep).
constexpr Row kRows[] = {
    {ToolKind::Denoise, i18n::N_("Clean up..."), "denoise", i18n::N_("Takes hiss, hum and room noise out of the clip")},
    {ToolKind::Declick, i18n::N_("Declick..."), "declick", i18n::N_("Finds clicks and crackle and rebuilds the audio under them")},
    {ToolKind::Declip, i18n::N_("Declip..."), "declip", i18n::N_("Rebuilds peaks that were cut off by clipping")},
    {ToolKind::Deverb, i18n::N_("Deverb..."), "deverb", i18n::N_("Takes the room's reverb tail out of a recording")},
    {ToolKind::Gate, i18n::N_("Noise gate..."), "gate", i18n::N_("Silences the quiet parts between the sounds")},
    {ToolKind::Equalize, i18n::N_("Equalize..."), "eq", i18n::N_("Shapes the tone with a curve of bands")},
    {ToolKind::Reverb, i18n::N_("Reverb..."), "reverb", i18n::N_("Adds a room; the clip grows by the reverb's tail")},
    {ToolKind::Blur, i18n::N_("Blur..."), "blur", i18n::N_("Smears the sound in time, from a soft haze to a wash")},
    {ToolKind::Normalize, i18n::N_("Normalize..."), "normalize", i18n::N_("Brings the clip's peak to a level")},
    {ToolKind::Amplify, i18n::N_("Amplify..."), "amplify", i18n::N_("Makes the clip louder or quieter")},
    {ToolKind::FadeIn, i18n::N_("Fade in..."), "fadein", i18n::N_("Fades the clip in over its length (or the editor's selection)")},
    {ToolKind::FadeOut, i18n::N_("Fade out..."), "fadeout", i18n::N_("Fades the clip out over its length (or the editor's selection)")},
    {ToolKind::Reverse, i18n::N_("Reverse"), "reverse", i18n::N_("Turns the audio round in a new file at once (the clip's Reverse switch only plays it backwards)")},
    {ToolKind::DrumStretch, i18n::N_("Drum loop stretch..."), "drum", i18n::N_("A drum loop at a new tempo, every hit kept sharp and on the grid; the clip takes the new length")},
};
bool sepAfter(ToolKind k) { return k == ToolKind::Gate || k == ToolKind::Blur || k == ToolKind::Reverse; }
// Tools with nothing to set run at once from the menu, without a window.
bool direct(ToolKind k) { return k == ToolKind::Reverse; }
// A run from the menu shows its progress once it has taken this long (seconds).
constexpr double kShowProgressAfter = 0.4;

constexpr uint64_t kKeyBase = 0xC1A70000'00000000ull;

// A run of the tool on the clips (worker thread), one progress for all of them.
struct Run {
  std::vector<std::unique_ptr<ed::Progress>> each;  // one per clip
  std::atomic<int> at{0};
  std::atomic<bool> cancel{false};
  std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
  bool direct = false;  // started from the menu (no window): progress only when it takes a while
  float done() const {
    const int n = static_cast<int>(each.size());
    if (n == 0) return 0.0f;
    const int i = std::min(at.load(), n - 1);
    return std::clamp((static_cast<float>(i) + each[static_cast<size_t>(i)]->done.load()) / static_cast<float>(n), 0.0f, 1.0f);
  }
  void stop() {
    cancel = true;
    for (auto& p : each) p->cancel = true;
  }
};

struct Dlg {
  bool open = false, focus = false;
  ObjectId clip = kNoId;
  std::vector<ObjectId> clips;  // the clip first, then the other selected audio clips
  ToolKind kind = ToolKind::Denoise;
  std::map<int, ed::ToolParams> params;  // per tool, kept while the app runs
  std::string name;                      // the clicked clip's name
  bool hasSel = false;
  double selT0 = 0, selT1 = 0;
  int scope = 0;  // 0 the whole clip, 1 the editor's selection
  ct::Target first;
  std::string targetErr;
  int targetScope = -1;
  ed::AudioPtr audio;  // the clicked clip's window as heard (the panel's audio)
  std::future<ed::AudioPtr> loading;
  std::shared_ptr<Run> run;
  std::future<std::vector<ct::Result>> job;
  uint64_t key = 0;
  // dev
  bool devAccept = false, devStop = false;
  std::string devLast;
};
Dlg gD;

// The menu rows drawn last (dev "cliptools hover / click").
struct DevRect {
  ImVec2 a, b;
  int frame = -1;
};
std::map<std::string, DevRect> gRects;
bool gReveal = false;  // dev "cliptools reveal": the menu scrolls the submenu's row into view
void noteRect(const std::string& name) { gRects[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetFrameCount()}; }

ed::ToolParams& params() {
  ed::ToolParams& p = gD.params[static_cast<int>(gD.kind)];
  p.kind = gD.kind;
  return p;
}

std::string clipName(const Song& s, ObjectId id) {
  const Clip* c = s.clip(id);
  if (!c) return "";
  if (!c->name.empty()) return c->name;
  const Channel* ch = s.channel(c->source);
  return ch ? ch->name : std::string();
}

// The target of the clicked clip for the chosen scope, its window read on a worker.
void refreshTarget(App& a) {
  if (gD.targetScope == gD.scope && (gD.audio || gD.loading.valid() || !gD.targetErr.empty())) return;
  gD.targetScope = gD.scope;
  gD.targetErr.clear();
  ed::stopPreview(a, gD.key);
  const bool sel = gD.scope == 1 && gD.hasSel;
  std::string why;
  if (!ct::target(a.doc.song(), gD.clip, sel ? gD.selT0 : 0, sel ? gD.selT1 : 0, gD.first, &why)) {
    gD.targetErr = why;
    gD.audio = nullptr;
    return;
  }
  // The window is the same for both scopes: read it once.
  if (gD.audio) return;
  const ct::Target t = gD.first;
  gD.loading = std::async(std::launch::async, [t]() { gl::threads::setName("gl-cliptools"); return ct::windowAudio(t); });
}

void closeDlg(App& a) {
  ed::stopPreview(a, gD.key);
  if (gD.run) gD.run->stop();
  gD.open = false;
}

// The clicked clip, then the other selected audio clips with it; the clip editor's selection.
void takeClips(App& a, ObjectId clip, ToolKind kind) {
  const Song& s = a.doc.song();
  gD.clip = clip;
  gD.kind = kind;
  gD.key = kKeyBase ^ static_cast<uint64_t>(clip);
  gD.name = clipName(s, clip);
  gD.clips = {clip};
  if (a.playlist.selected.count(clip))
    for (const Clip& k : s.playlist)
      if (k.id != clip && k.kind == ClipKind::Audio && a.playlist.selected.count(k.id)) gD.clips.push_back(k.id);
  gD.hasSel = clipeditor::selectionOf(clip, gD.selT0, gD.selT1);
  gD.scope = gD.hasSel && gD.clips.size() == 1 ? 1 : 0;
  gD.devLast.clear();
}

void startRun(App& a, bool fromMenu = false) {
  const Song& s = a.doc.song();
  {  // the clip as it is now (it may have been trimmed or moved while the window was open)
    const bool sel = gD.scope == 1 && gD.hasSel;
    ct::Target now;
    if (ct::target(s, gD.clip, sel ? gD.selT0 : 0, sel ? gD.selT1 : 0, now)) gD.first = now;
  }
  std::vector<ct::Target> ts{gD.first};
  if (gD.clips.size() > 1) {
    ts.clear();
    for (ObjectId id : gD.clips) {
      ct::Target t;
      if (id == gD.clip) ts.push_back(gD.first);
      else if (ct::target(s, id, t)) ts.push_back(t);
    }
  }
  if (ts.empty()) return;
  ed::stopPreview(a, gD.key);
  auto run = std::make_shared<Run>();
  run->direct = fromMenu;
  for (size_t i = 0; i < ts.size(); ++i) run->each.push_back(std::make_unique<ed::Progress>());
  gD.run = run;
  const ed::ToolParams p = params();
  const ct::Target firstT = gD.first;
  const std::string dir = editedDir(a);
  gD.job = std::async(std::launch::async, [run, ts, p, firstT, dir]() { gl::threads::setName("gl-cliptools");
    std::vector<ct::Result> out;
    for (size_t i = 0; i < ts.size() && !run->cancel; ++i) {
      run->at = static_cast<int>(i);
      out.push_back(ct::render(ts[i], ct::paramsFor(p, firstT, ts[i]), dir, run->each[i].get()));
    }
    return out;
  });
}

// The worker finished: every clip's new file in one undo step (nothing when it was stopped).
void finishRun(App& a) {
  if (!gD.job.valid() || gD.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  const std::vector<ct::Result> rs = gD.job.get();
  const bool stopped = gD.run && gD.run->cancel;
  gD.run.reset();
  if (stopped) {
    gD.devLast = "stopped";
    a.notify(i18n::tr("Stopped: the clips are as they were"));
    return;
  }
  std::vector<ct::Result> ok;
  std::string err;
  for (const ct::Result& r : rs)
    if (!r.path.empty()) ok.push_back(r);
    else err = r.error;
  int n = 0;
  if (!ok.empty())
    a.doc.editSong(ct::undoLabel(gD.kind, static_cast<int>(ok.size())), [&](Song& x) {
      for (const ct::Result& r : ok) n += ct::apply(x, r) ? 1 : 0;
    });
  char b[96];
  std::snprintf(b, sizeof b, "applied %d", n);
  gD.devLast = b;
  for (const ct::Result& r : ok) gD.devLast += " " + r.path;
  if (n == 0) {
    a.notify(std::string(i18n::tr(ed::toolName(gD.kind))) + ": " + i18n::tr(err.empty() ? "nothing changed" : err.c_str()), true);
    return;
  }
  a.setHint(std::string(i18n::tr(ed::toolName(gD.kind))) + ": " + i18n::tr("a new file; the original is kept (undo goes back to it)"));
  gD.open = false;
}

std::string timeText(double rate, int64_t f) {
  char b[32];
  const double sec = rate > 0 ? static_cast<double>(f) / rate : 0.0;
  if (sec >= 60) std::snprintf(b, sizeof b, "%d:%05.2f", static_cast<int>(sec) / 60, sec - 60.0 * std::floor(sec / 60.0));
  else std::snprintf(b, sizeof b, "%.2f s", sec);
  return b;
}

void scopeRows(App& a) {
  const auto& P = theme::pal();
  char b[256];
  const double rate = gD.first.rate > 0 ? gD.first.rate : 44100.0;
  if (gD.clips.size() > 1) {
    std::snprintf(b, sizeof b, i18n::tr("Applies to %d clips, each to a new file of its own (the settings and preview are %s's)"),
                  static_cast<int>(gD.clips.size()), gD.name.c_str());
  } else {
    const int64_t len = gD.scope == 1 && !gD.first.part.empty() ? gD.first.part.len() : gD.first.b - gD.first.a;
    std::snprintf(b, sizeof b, i18n::tr("Applies to %s of %s (%s), as a new file: the original stays"),
                  gD.scope == 1 ? i18n::tr("the selection") : i18n::tr("all"), gD.name.c_str(), timeText(rate, len).c_str());
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ed::dlg::kDlgW);
  ImGui::TextUnformatted(b);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  if (gD.hasSel && gD.clips.size() == 1) {
    const char* opts[] = {i18n::tr("Whole clip"), i18n::tr("Selection")};
    int sc = gD.scope;
    if (ui::Segmented("##ctscope", opts, 2, &sc, std::floor((ed::dlg::kDlgW - theme::kS1) / 2))) gD.scope = sc;
    ui::Hint("The clip editor's time selection, or all of the clip");
  }
  (void)a;
}

// Fade in / out: the curve (the other tools have their own panels).
bool fadePanel(ed::ToolParams& p) {
  const float col = ed::dlg::column({"Curve"});
  ed::dlg::BeginSection(i18n::N_("FADE"), ed::dlg::kDlgW);
  const char* curves[static_cast<int>(ed::Curve::Count)];
  for (int i = 0; i < static_cast<int>(ed::Curve::Count); ++i) curves[i] = i18n::tr(ed::curveName(static_cast<ed::Curve>(i)));
  int cv = static_cast<int>(p.fade);
  if (ed::dlg::combo("Curve", col, &cv, curves, static_cast<int>(ed::Curve::Count))) p.fade = static_cast<ed::Curve>(cv);
  ui::EndSection();
  return true;
}

void footer(App& a, bool ready);

// A run started from the menu: its progress and Stop, once it has taken a moment.
void drawDirectRun(App& a) {
  if (!gD.run || !gD.run->direct) return;
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - gD.run->began).count();
  if (sec < kShowProgressAfter) return;
  ui::PlaceNextDialog(ImVec2(ed::dlg::kDlgW + 2 * ImGui::GetStyle().WindowPadding.x, 0));
  const std::string title = std::string(i18n::tr(ed::toolName(gD.kind))) + "###cliptoolsrun";
  if (ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
    ui::TrackWindowInViewport();
    footer(a, false);
  }
  ImGui::End();
}

void footer(App& a, bool ready) {
  using ed::dlg::kBtnW;
  if (gD.run) {
    const float fr = gD.run->done();
    ui::DialogFooter(ed::dlg::kDlgW);
    char b[96];
    if (gD.clips.size() > 1)
      std::snprintf(b, sizeof b, i18n::tr("Applying to clip %d of %d... %.0f%%"), std::min(gD.run->at.load(), static_cast<int>(gD.clips.size()) - 1) + 1,
                    static_cast<int>(gD.clips.size()), static_cast<double>(fr) * 100.0);
    else
      std::snprintf(b, sizeof b, i18n::tr("Applying... %.0f%%"), static_cast<double>(fr) * 100.0);
    ImGui::ProgressBar(fr, ImVec2(ed::dlg::kDlgW - kBtnW - theme::kS2, 0), b);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton(i18n::N_("Stop"), false, kBtnW, i18n::N_("Stop: the clips stay as they were")) || gD.devStop) gD.run->stop();
    gD.devStop = false;
    return;
  }
  ui::DialogFooter(ui::ButtonRowWidth(2, kBtnW));
  if (ui::TextButton("Cancel", false, kBtnW)) closeDlg(a);
  ImGui::SameLine(0, theme::kS2);
  const bool ok = ui::PrimaryButton("Accept", kBtnW, nullptr, ready);
  if ((ok || gD.devAccept) && ready) startRun(a);
  gD.devAccept = false;
}

}  // namespace

void open(App& a, ObjectId clip, int kind) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c || c->kind != ClipKind::Audio || kind < 0 || kind >= static_cast<int>(ToolKind::Count)) return;
  if (direct(static_cast<ToolKind>(kind))) {
    runNow(a, clip, kind);
    return;
  }
  if (gD.run) return;  // (a run is still going)
  if (gD.open) closeDlg(a);
  gD.open = true;
  gD.focus = true;
  takeClips(a, clip, static_cast<ToolKind>(kind));
  gD.audio = nullptr;
  gD.loading = {};
  gD.targetScope = -1;
  gD.targetErr.clear();
  refreshTarget(a);
}

void runNow(App& a, ObjectId clip, int kind) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c || c->kind != ClipKind::Audio || kind < 0 || kind >= static_cast<int>(ToolKind::Count)) return;
  if (gD.run) return;  // (a run is still going)
  if (gD.open) closeDlg(a);
  takeClips(a, clip, static_cast<ToolKind>(kind));
  // The tool's own settings (Reverse has none); the clip editor's selection when it has one.
  const bool sel = gD.scope == 1 && gD.hasSel;
  std::string why;
  if (!ct::target(s, clip, sel ? gD.selT0 : 0, sel ? gD.selT1 : 0, gD.first, &why)) {
    gD.devLast = "error " + why;
    a.notify(std::string(i18n::tr(ed::toolName(gD.kind))) + ": " + i18n::tr(why.c_str()), true);
    return;
  }
  startRun(a, true);
}

void clipMenu(App& a, ObjectId clip) {
  const Clip* c = a.doc.song().clip(clip);
  if (!c || c->kind != ClipKind::Audio) return;
  const bool openMenu = ImGui::BeginMenu(i18n::tr("Audio tools"));
  noteRect("menu");
  if (gReveal) {
    ImGui::SetScrollHereY(0.5f);
    gReveal = false;
  }
  if (!openMenu) return;
  for (const Row& r : kRows) {
    if (ImGui::MenuItem(i18n::tr(r.label))) {
      const int k = static_cast<int>(r.kind);
      a.later([&a, clip, k] { open(a, clip, k); });
    }
    noteRect(r.dev);
    ui::Hint(r.hint);
    if (sepAfter(r.kind)) ImGui::Separator();
  }
  ImGui::EndMenu();
}

void draw(App& a) {
  finishRun(a);
  drawDirectRun(a);
  if (!gD.open) return;
  if (!a.doc.song().clip(gD.clip)) {  // the clip went (undo, a collaborator)
    closeDlg(a);
    return;
  }
  if (gD.loading.valid() && gD.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    gD.audio = gD.loading.get();
    if (!gD.audio) gD.targetErr = "The clip's audio could not be read";
  }
  refreshTarget(a);
  const float w = ed::dlg::kDlgW;
  ui::PlaceNextDialog(ImVec2(w + 2 * ImGui::GetStyle().WindowPadding.x, 0));
  if (gD.focus) ImGui::SetNextWindowFocus(), gD.focus = false;
  bool openFlag = true;
  const std::string title = std::string(i18n::tr(ed::toolName(gD.kind))) + "###cliptoolswin";
  const bool vis = ImGui::Begin(title.c_str(), &openFlag, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking);
  ui::TrackWindowInViewport();
  if (vis) {
    // The settings scroll on a small screen; the footer (Accept, the progress) stays in view.
    ui::BeginDialogBody("##ctbody");
    scopeRows(a);
    bool ready = false;
    ed::ToolParams& p = params();
    if (!gD.targetErr.empty()) {
      ImGui::TextColored(theme::vec(theme::pal().textDim), "%s", i18n::tr(gD.targetErr.c_str()));
    } else if (!gD.audio) {
      ImGui::TextColored(theme::vec(theme::pal().textDim), "%s", i18n::tr("Reading the clip's audio..."));
    } else {
      ed::PanelContext c;
      c.key = gD.key;
      c.audio = gD.audio;
      c.target = gD.first.part.empty() ? ed::Range{0, gD.audio->frames()} : gD.first.part;
      c.songTempo = a.doc.song().tempo;
      c.beatsPerBar = std::max(1, a.doc.song().timeSigNum);
      const double rate = gD.audio->rate;
      c.timeText = [rate](int64_t f) { return timeText(rate, f); };
      if (gD.scope == 1 && !gD.first.part.empty()) {  // Clean up: learn the noise from the selection
        const ed::AudioPtr au = gD.audio;
        const ed::Range part = gD.first.part;
        c.takeProfile = [au, part](ed::NoiseProfile& out) {
          out = ed::acquireNoise(*au, part);
          return out.valid();
        };
      }
      ImGui::BeginDisabled(gD.run != nullptr);
      if (gD.kind == ToolKind::FadeIn || gD.kind == ToolKind::FadeOut) ready = fadePanel(p);
      else ready = ed::drawToolPanel(p, c);
      ImGui::EndDisabled();
      ed::drawPreview(a, c, p, [&a]() {
        if (a.engine.playing()) a.engine.stop();
      });
    }
    ui::EndDialogBody();
    footer(a, ready && gD.audio && gD.targetErr.empty());
  }
  ImGui::End();
  if (!openFlag) closeDlg(a);
}

void shutdown() {
  if (gD.run) gD.run->stop();
  if (gD.job.valid()) gD.job.wait();
  if (gD.loading.valid()) gD.loading.wait();
}

bool devCommand(App& a, const std::vector<std::string>& args, int frame) {
  const std::string cmd = args.empty() ? "state" : args[0];
  auto kindOf = [](const std::string& n) {
    for (const Row& r : kRows)
      if (n == r.dev) return static_cast<int>(r.kind);
    return -1;
  };
  if (cmd == "open" && args.size() >= 3) {
    open(a, static_cast<ObjectId>(std::strtoul(args[1].c_str(), nullptr, 10)), kindOf(args[2]));
  } else if (cmd == "scope" && args.size() >= 2) {
    gD.scope = args[1] == "selection" && gD.hasSel ? 1 : 0;
  } else if (cmd == "accept") {
    gD.devAccept = true;
  } else if (cmd == "stop") {
    gD.devStop = true;
    if (gD.run) gD.run->stop();
  } else if (cmd == "wait") {  // until the window's audio is read, its analysis is done and a run has finished
    if (gD.loading.valid()) gD.loading.wait();
    if (gD.job.valid()) gD.job.wait();
  } else if (cmd == "close") {
    closeDlg(a);
  } else if (cmd == "hover" || cmd == "click") {  // the mouse onto the submenu ("hover") or a tool's row ("click <tool>")
    const std::string name = cmd == "hover" ? "menu" : args.size() >= 2 ? args[1] : "";
    auto it = gRects.find(name);
    if (it == gRects.end() || it->second.frame < ImGui::GetFrameCount() - 3) {
      std::printf("cliptools frame %d: no row %s\n", frame, name.c_str());
    } else {
      ImGuiIO& io = ImGui::GetIO();
      const ImVec2 m((it->second.a.x + it->second.b.x) * 0.5f, (it->second.a.y + it->second.b.y) * 0.5f);
      io.AddMousePosEvent(m.x, m.y);
      if (cmd == "click") {
        io.AddMouseButtonEvent(0, true);
        io.AddMouseButtonEvent(0, false);
      }
    }
  } else if (cmd == "select") {  // the playlist's clip selection (select <clip> ...)
    a.playlist.selected.clear();
    for (size_t i = 1; i < args.size(); ++i) a.playlist.selected.insert(static_cast<ObjectId>(std::strtoul(args[i].c_str(), nullptr, 10)));
  } else if (cmd == "reveal") {
    gReveal = true;
  } else if (cmd == "rows") {
    std::printf("cliptools rows frame %d:", frame);
    for (const auto& [n, r] : gRects)
      if (r.frame >= ImGui::GetFrameCount() - 3) std::printf(" %s@%.0f,%.0f", n.c_str(), static_cast<double>(r.a.x), static_cast<double>(r.a.y));
    std::printf("\n");
  } else {
    std::printf("cliptools state frame %d: open %d tool %s clip %u clips %d sel %d scope %d audio %d running %d last \"%s\"\n", frame, gD.open ? 1 : 0,
                ed::toolName(gD.kind), static_cast<unsigned>(gD.clip), static_cast<int>(gD.clips.size()), gD.hasSel ? 1 : 0, gD.scope,
                gD.audio ? 1 : 0, gD.run ? 1 : 0, gD.devLast.c_str());
  }
  std::fflush(stdout);
  return true;
}

}  // namespace gl::cliptoolsui
