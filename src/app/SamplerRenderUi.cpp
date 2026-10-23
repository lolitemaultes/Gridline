// Gridline — render to sampled instrument window (RN-08, see J59Ui.h and
// engine/SampledInstrument.h).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Presets.h"
#include "../engine/SampledInstrument.h"
#include "App.h"
#include "DialogLayout.h"
#include "HostedUi.h"
#include "J59Ui.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::j59 {

using theme::pal;
namespace fs = std::filesystem;

namespace {

constexpr int kRates[] = {44100, 48000, 96000};

struct Job {
  std::thread worker;
  std::atomic<bool> running{false}, finished{false}, cancel{false};
  std::atomic<double> progress{0};
  std::mutex mu;
  sampled::Result result;  // guarded by mu
  ~Job() {
    cancel = true;
    if (worker.joinable()) worker.join();
  }
};

struct State {
  bool open = false;
  ObjectId channel = kNoId;
  sampled::Options o;
  char name[128] = "";
  int notesMode = 0;  // 0 range, 1 the piano roll's selected notes
  std::vector<int> selection;
  bool replace = false, library = true;
  int rate = 0, bits = 1;
  // The job's target (taken when it starts).
  ObjectId jobChannel = kNoId;
  bool jobReplace = false, jobLibrary = true;
  std::string jobName;
} R;
Job job;

void formLabel(const char* label, float x = 140.0f) { ui::FormLabelFit(i18n::tr(label), x, pal().textDim); }

std::string outputParent(const App& a) {
  const std::string& f = a.projectFile();
  if (!f.empty()) return (fs::path(f).parent_path() / "Rendered instruments").string();
  return (fs::path(a.configDir()) / "Rendered instruments").string();
}

sampled::Options currentOptions() {
  sampled::Options o = R.o;
  o.notes = R.notesMode == 1 ? R.selection : std::vector<int>{};
  o.sampleRate = kRates[std::clamp(R.rate, 0, 2)];
  o.bits = R.bits == 0 ? 16 : 24;
  o.name = R.name[0] ? R.name : "Sampled";
  return o;
}

void start(App& a) {
  const Channel* c = a.doc.song().channel(R.channel);
  if (!c || job.running) return;
  captureHostedStates(a);
  const sampled::Options o = currentOptions();
  const std::string dir = sampled::freshFolder(outputParent(a), o.name);
  R.jobChannel = R.channel, R.jobReplace = R.replace, R.jobLibrary = R.library, R.jobName = o.name;
  job.cancel = false, job.finished = false, job.progress = 0;
  job.running = true;
  Song song = a.doc.song();
  const ObjectId ch = R.channel;
  auto work = [song = std::move(song), ch, o, dir]() {
    sampled::Result r = sampled::renderToSampler(song, ch, o, dir, [](double p) { job.progress = p; }, &job.cancel);
    {
      std::lock_guard<std::mutex> lk(job.mu);
      job.result = std::move(r);
    }
    job.finished = true;
    job.running = false;
  };
  // Hosted plugins make their offline instance where the render runs; some
  // formats allow that on the main thread only (as the render dialog does).
  bool hosted = isHostedType(c->plugin.type);
  for (ObjectId id : c->layer)
    if (const Channel* l = a.doc.song().channel(id)) hosted = hosted || isHostedType(l->plugin.type);
  if (hosted) work();
  else job.worker = std::thread(std::move(work));
}

void finish(App& a) {
  if (!job.finished.exchange(false)) return;
  if (job.worker.joinable()) job.worker.join();
  sampled::Result r;
  {
    std::lock_guard<std::mutex> lk(job.mu);
    r = std::move(job.result);
  }
  if (r.cancelled) {
    std::error_code ec;
    fs::remove_all(r.dir, ec);
    a.notify(i18n::tr("Render cancelled"));
    return;
  }
  if (!r.error.empty()) {
    a.notify(r.error, true);
    return;
  }
  const ObjectId id = sampled::applyToProject(a.doc, R.jobChannel, r.state, R.jobReplace, "");
  if (id) a.selectChannel(id);
  if (R.jobLibrary) savePreset(presetRoot(a.configDir()), r.state, "Rendered/" + safePresetName(R.jobName), "Rendered sampled instrument");
  char b[512];
  std::snprintf(b, sizeof b, i18n::tr("%d samples rendered to %s"), static_cast<int>(r.takes.size()), r.dir.c_str());
  a.notify(b);
  R.open = false;
}

void window(App& a) {
  finish(a);
  if (!R.open) return;
  const Song& song = a.doc.song();
  const Channel* ch = song.channel(R.channel);
  if (!ch) {
    R.open = false;
    return;
  }
  const auto& P = pal();
  ui::PlaceNextDialog(ImVec2(560, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const std::string title = std::string(i18n::tr("Render to sampled instrument")) + "###j59render";
  bool vis = ImGui::Begin(title.c_str(), &R.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  const bool busy = job.running;
  ui::BeginDialogBody("##j59renderbody");
  ImGui::BeginDisabled(busy);
  const float w = 520;
  {
    const PluginInfo* info = findPlugin(ch->plugin.type);
    formLabel("Source");
    ImGui::PushFont(a.fontBold, theme::kFontBody);
    ImGui::TextUnformatted(ch->name.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(theme::vec(P.textDim), "%s", info ? info->name.c_str() : ch->plugin.type.c_str());
    if (isHostedType(ch->plugin.type)) {
      formLabel("");
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Hosted plugin: it renders as itself"));
    }
  }
  ui::BeginSection("NOTES", w);
  {
    formLabel("Sample");
    static const char* const modes[] = {i18n::N_("A key range"), i18n::N_("Selected notes")};
    if (R.selection.empty()) R.notesMode = 0;
    ui::Segmented("notesmode", modes, R.selection.empty() ? 1 : 2, &R.notesMode, 120);
    if (R.selection.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("or select notes in the piano roll"));
    }
    if (R.notesMode == 0) {
      auto noteField = [&](const char* id, int* v) {
        ImGui::SetNextItemWidth(90);
        ImGui::InputInt(id, v, 1, 12);
        *v = std::clamp(*v, 0, 127);
        ui::FocusRing();
        ImGui::SameLine();
        ImGui::PushFont(a.fontMono, theme::kFontBody);
        ImGui::TextColored(theme::vec(P.textDim), "%s", sampled::noteLabel(*v).c_str());
        ImGui::PopFont();
      };
      formLabel("Lowest note");
      noteField("##lo", &R.o.noteLo);
      formLabel("Highest note");
      noteField("##hi", &R.o.noteHi);
      formLabel("Sample every");
      ImGui::SetNextItemWidth(200);
      ImGui::SliderInt("##step", &R.o.step, 1, 12, i18n::tr("%d semitones"));
      ui::Hint("Notes in between play the nearest sample, transposed");
    } else {
      formLabel("Notes");
      std::string list;
      for (size_t k = 0; k < R.selection.size(); ++k) list += (k ? " " : "") + sampled::noteLabel(R.selection[k]);
      ui::TextFit(list.c_str(), w - 160, P.text);
    }
  }
  ui::EndSection();
  ui::BeginSection("TAKES", w);
  {
    formLabel("Velocity layers");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("##layers", &R.o.velocityLayers, 1, 8);
    ui::Hint("Each layer is rendered at the top velocity of its range");
    formLabel("Round robins");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("##rr", &R.o.roundRobins, 1, 4);
    ui::Hint("Takes per note, played in turn (instruments with free-running movement differ from take to take)");
    formLabel("Note length");
    float len = static_cast<float>(R.o.noteSeconds), tail = static_cast<float>(R.o.tailSeconds);
    ImGui::SetNextItemWidth(200);
    if (ImGui::DragFloat("##len", &len, 0.01f, 0.1f, 10.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) R.o.noteSeconds = len;
    formLabel("Release tail");
    ImGui::SetNextItemWidth(200);
    if (ImGui::DragFloat("##tail", &tail, 0.01f, 0.0f, 10.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) R.o.tailSeconds = tail;
    const sampled::Options o = currentOptions();
    const int notes = static_cast<int>(sampled::sampledNotes(o).size());
    const int takes = notes * std::clamp(o.velocityLayers, 1, 8) * std::clamp(o.roundRobins, 1, 4);
    formLabel("Total");
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%d notes, %d samples, %.0f s of audio"), notes, takes, takes * (o.noteSeconds + o.tailSeconds));
  }
  ui::EndSection();
  ui::BeginSection("PROCESSING", w);
  {
    formLabel("Trim silence");
    ImGui::Checkbox("##trim", &R.o.autoTrim);
    ImGui::SameLine();
    ImGui::BeginDisabled(!R.o.autoTrim);
    float db = static_cast<float>(R.o.trimDb);
    ImGui::SetNextItemWidth(200);
    if (ImGui::DragFloat("##trimdb", &db, 0.25f, -96.0f, -30.0f, i18n::tr("below %.0f dB"), ImGuiSliderFlags_AlwaysClamp)) R.o.trimDb = db;
    ImGui::EndDisabled();
    formLabel("Sustain loops");
    ImGui::Checkbox(i18n::tr("Find loop points in the held part"), &R.o.findLoops);
    ui::Hint("Held notes loop while the key is down, then play their recorded release");
    formLabel("Format");
    static const char* const rates[] = {"44.1 kHz", "48 kHz", "96 kHz"};
    ui::Segmented("rate", rates, 3, &R.rate, 76);
    ImGui::SameLine();
    static const char* const bits[] = {"16-bit", "24-bit"};
    ui::Segmented("bits", bits, 2, &R.bits, 64);
  }
  ui::EndSection();
  ui::BeginSection("OUTPUT", w);
  {
    formLabel("Name");
    ImGui::SetNextItemWidth(260);
    ImGui::InputText("##name", R.name, sizeof R.name);
    ui::FocusRing();
    formLabel("Folder");
    const std::string parent = outputParent(a);
    ui::TextFit(sampled::freshFolder(parent, R.name[0] ? R.name : "Sampled").c_str(), w - 160, P.textDim);
    formLabel("Instrument");
    int rep = R.replace ? 1 : 0;
    static const char* const repl[] = {i18n::N_("New channel"), i18n::N_("Replace this channel's")};
    static const char* const replHints[] = {i18n::N_("Adds a Folio channel after this one, on the same mixer track"),
                                            i18n::N_("Swaps the channel's instrument for Folio (undo brings it back)")};
    if (ui::Segmented("replace", repl, 2, &rep, 150, 0, replHints)) R.replace = rep == 1;
    formLabel("Preset");
    ImGui::Checkbox(i18n::tr("Also save to the preset library (Rendered)"), &R.library);
  }
  ui::EndSection();
  ImGui::EndDisabled();
  ui::EndDialogBody();
  // Footer.
  const float bw = 96;
  if (busy) {
    ui::DialogFooter(w);
    char pr[64];
    std::snprintf(pr, sizeof pr, i18n::tr("Rendering %.0f%%"), job.progress.load() * 100.0);
    ImGui::ProgressBar(static_cast<float>(job.progress.load()), ImVec2(w - bw - theme::kS2, 0), pr);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Stop", false, bw, "Cancel the render")) job.cancel = true;
  } else {
    ui::DialogFooter(ui::ButtonRowWidth(2, bw));
    if (ui::TextButton("Cancel", false, bw) || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape)))
      R.open = false;
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Render", true, bw, "Render the notes, write the samples and build the instrument")) start(a);
  }
  ImGui::End();
}

}  // namespace

void openRenderSampler(App& a, ObjectId channel) {
  const Channel* c = a.doc.song().channel(channel);
  if (!c) return;
  if (R.channel != channel || !R.name[0]) std::snprintf(R.name, sizeof R.name, "%s", c->name.c_str());
  R.channel = channel;
  // The piano roll's selected notes on this channel.
  R.selection.clear();
  if (a.piano.selChannel == channel && !a.piano.selected.empty())
    if (const Pattern* p = a.doc.song().pattern(a.piano.selPattern))
      if (auto it = p->notes.find(channel); it != p->notes.end())
        for (size_t i : a.piano.selected)
          if (i < it->second.size()) R.selection.push_back(it->second[i].pitch);
  std::sort(R.selection.begin(), R.selection.end());
  R.selection.erase(std::unique(R.selection.begin(), R.selection.end()), R.selection.end());
  R.notesMode = R.selection.empty() ? 0 : 1;
  R.open = true;
}

void drawRenderSampler(App& a) { window(a); }

}  // namespace gl::j59
