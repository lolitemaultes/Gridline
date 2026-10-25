// Gridline — "edison" pack: audio editor scripts in the editor (SC-05 / ED-09).
// Tools → Scripts lists the shipped (<assets>/scripts/audio) and the user's
// (<config>/scripts/audio) scripts; the script dialog shows the form, runs a
// live preview (small budget while a control is held, full runs otherwise),
// draws the processed frames, plays them, and Accept turns the result into
// one undo step of the editor.
#include "../../core/I18n.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include "../../app/ScriptUi.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/Log.h"
#include "edison_script.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace fs = std::filesystem;

namespace {

struct ScriptRun {
  AudioScript script;
  std::vector<script::FormParam> values;
  script::PreviewScheduler sched;
  AudioScriptResult last;
  bool have = false;
  uint64_t serial = 0;  // history serial the result belongs to
  Range sel;            // selection the result belongs to
  int64_t cursor = 0;
  uint32_t seed = 1;
  double runMs = 0;
  bool lastLimitHit = false;
  SamplePtr pv;  // preview sample being played
};
std::map<uint64_t, ScriptRun> runs;  // per editor session
std::vector<script::ScriptInfo> list;
bool scanned = false;

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

script::Limits limitsFor(App& a) {
  script::Limits l;
  l.seconds = std::clamp(a.settings.getNumber("scripts.maxSeconds"), 0.05, 30.0);
  l.instructions = static_cast<uint64_t>(std::clamp(a.settings.getNumber("scripts.maxMillionInstructions"), 1.0, 2000.0) * 1e6);
  return l;
}

void runNow(App& a, Session& s, ScriptRun& r, bool full) {
  AudioScriptContext ctx;
  ctx.content = s.cur();
  ctx.sel = s.sel;
  ctx.cursor = s.cursor;
  ctx.seed = r.seed;
  const script::Limits lim = limitsFor(a);
  const double t0 = wallNow();
  AudioScriptResult res = r.script.run(ctx, r.values, full ? lim : script::previewLimits(lim));
  r.runMs = (wallNow() - t0) * 1000.0;
  r.lastLimitHit = res.limitHit;
  if (!full && res.limitHit) return;  // too slow for a live preview: keep the last result
  for (const auto& l : res.log) logging::info("script", r.script.name() + ": " + l);
  r.last = std::move(res);
  r.have = true;
  r.serial = s.hist.serial();
  r.sel = s.sel;
  r.cursor = s.cursor;
}

// Min / max columns of the processed frames.
void drawPreview(const Audio& au, float w, float h) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
  ImGui::Dummy(ImVec2(w, h));
  dl->AddRectFilled(p0, p1, C(col::WaveBg), 4);
  const int chans = au.channels == 1 ? 1 : 2;
  const float laneH = h / static_cast<float>(chans);
  const int64_t n = au.frames();
  for (int c = 0; c < chans; ++c) {
    const float mid = p0.y + laneH * (static_cast<float>(c) + 0.5f);
    dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), C(col::WaveGrid));
    if (n == 0) continue;
    const float* d = au.ch(c);
    const int cols = static_cast<int>(w);
    for (int x = 0; x < cols; ++x) {
      int64_t a = n * x / cols, b = std::max(a + 1, n * (x + 1) / cols);
      float mn = 0, mx = 0;
      // Long previews: sample at most 64 points per column (drawing only).
      const int64_t step = std::max<int64_t>(1, (b - a) / 64);
      for (int64_t i = a; i < b && i < n; i += step) mn = std::min(mn, d[i]), mx = std::max(mx, d[i]);
      const float s = laneH * 0.48f;
      dl->AddLine(ImVec2(p0.x + static_cast<float>(x), mid - std::clamp(mx, -1.f, 1.f) * s),
                  ImVec2(p0.x + static_cast<float>(x), mid - std::clamp(mn, -1.f, 1.f) * s + 1), C(col::Wave));
    }
  }
  dl->AddRect(p0, p1, C(col::BtnLo), 4);
}

std::string peakText(const Audio& au) {
  float pk = 0;
  for (float v : au.L) pk = std::max(pk, std::fabs(v));
  for (float v : au.R) pk = std::max(pk, std::fabs(v));
  char b[48];
  if (pk <= 0)
    std::snprintf(b, sizeof b, "silent");
  else
    std::snprintf(b, sizeof b, "peak %.1f dB", 20.0 * std::log10(pk));
  return b;
}

}  // namespace

const std::vector<script::ScriptInfo>& audioScripts(App& a, bool rescan) {
  if (!scanned || rescan) {
    std::vector<std::pair<std::string, bool>> dirs;
    std::string ship = shippedScriptsDir(a, "audio");
    if (!ship.empty()) dirs.push_back({ship, false});
    dirs.push_back({userScriptsDir(a, "audio"), true});
    list = script::scanScripts(dirs);
    scanned = true;
  }
  return list;
}

void audioScriptMenuItems(App& a, Session& s) {
  const bool ok = a.settings.getBool("scripts.enabled") && !s.job.active();
  std::string cat = "\x01";
  for (const auto& info : audioScripts(a)) {
    if (info.category != cat) {
      cat = info.category;
      ImGui::SeparatorText(cat.c_str());
    }
    std::string l = info.name + (info.user ? "  (yours)" : "") + "...##" + info.path;
    if (ImGui::MenuItem(l.c_str(), nullptr, false, ok)) openAudioScript(a, s, info.path);
    if (!info.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.description.c_str());
  }
  if (audioScripts(a).empty()) ImGui::TextDisabled("No scripts found");
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Rescan scripts"))) audioScripts(a, true);
  if (ImGui::MenuItem(i18n::tr("Open your scripts folder"))) {
    std::error_code ec;
    fs::create_directories(userScriptsDir(a, "audio"), ec);
    say(s, "Your audio scripts (*.lua): " + userScriptsDir(a, "audio"));
  }
}

void openAudioScript(App& a, Session& s, const std::string& path) {
  ScriptRun r;
  std::string err;
  if (!r.script.loadFile(path, &err)) {
    say(s, "Script error: " + err);
    logging::error("script", err);
    return;
  }
  r.values = r.script.form().params;
  auto& slot = runs[s.key];
  if (slot.script.path() == path) r.values = slot.values, r.seed = slot.seed;  // same script again: keep its settings
  slot = std::move(r);
  (void)a;
  openTool(s, Tool::Script);
}

void drawScriptDialog(App& a, Session& s) {
  auto it = runs.find(s.key);
  if (it == runs.end() || !it->second.script.loaded()) {
    ImGui::TextDisabled("No script");
    if (ImGui::Button(i18n::tr("Close"), ImVec2(110, 0))) ImGui::CloseCurrentPopup();
    return;
  }
  ScriptRun& r = it->second;
  // Re-run when the audio or the selection changed under the dialog.
  if (r.have && (r.serial != s.hist.serial() || r.sel.a != s.sel.a || r.sel.b != s.sel.b || r.cursor != s.cursor))
    r.sched.changed(wallNow(), false);
  const auto kind = r.sched.poll(wallNow());
  if (kind != script::PreviewScheduler::Run::None) {
    runNow(a, s, r, kind == script::PreviewScheduler::Run::Full);
    r.sched.ran(kind, wallNow(), r.lastLimitHit);
  }
  AudioScriptContext probe;
  probe.content = s.cur();
  probe.sel = s.sel;
  probe.cursor = s.cursor;
  const Range tr = r.script.targetRange(probe);
  ImGui::TextDisabled("Applies to: %s (%s)", !s.sel.empty() ? "selection" : tr.empty() ? "the cursor" : "whole file",
                      timeText(a, s, tr.len()).c_str());
  const std::string& desc = r.script.form().description;
  if (!desc.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
    ImGui::TextColored(ImVec4(0.7f, 0.75f, 0.78f, 1), "%s", desc.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::BeginGroup();
  const bool changed = drawScriptForm(r.values);
  ImGui::EndGroup();
  const bool held = ImGui::IsItemActive();
  if (changed) r.sched.changed(wallNow(), held);
  if (r.sched.dragging() && !held) r.sched.released();
  if (r.script.form().params.empty()) ImGui::TextDisabled("This script has no settings.");
  ImGui::Spacing();
  if (r.have && r.last.ok) {
    drawPreview(r.last.preview, 440, 70);
    char line[160];
    std::snprintf(line, sizeof line, "Result: %s, %s%s%s", timeText(a, s, r.last.preview.frames()).c_str(), peakText(r.last.preview).c_str(),
                  r.last.content.markers.size() != s.cur().markers.size() ? ", markers added" : "",
                  r.last.changed ? "" : " (no change to the audio)");
    ImGui::TextDisabled("%s", line);
    if (r.sched.tooSlow()) ImGui::TextDisabled("Preview too slow: showing the last result");
    if (!r.last.log.empty()) {
      size_t from = r.last.log.size() > 3 ? r.last.log.size() - 3 : 0;
      for (size_t i = from; i < r.last.log.size(); ++i) ImGui::TextDisabled("%s", r.last.log[i].c_str());
    }
  } else if (r.have) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440);
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1), "%s", r.last.error.c_str());
    ImGui::PopTextWrapPos();
  }
  // Listen to the processed frames before accepting.
  const bool canPlay = r.have && r.last.ok && r.last.preview.frames() > 0;
  ImGui::BeginDisabled(!canPlay);
  if (ImGui::Button(i18n::tr("Play preview"), ImVec2(110, 0))) {
    stopPlay(a, s);
    r.pv = toSample(r.last.preview, "");
    a.engine.preview(r.pv, true);  // J-112: Loupe's own playback
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("Stop"), ImVec2(70, 0)) && r.pv) {
    a.engine.preview(nullptr);
    r.pv = nullptr;
  }
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("New variation"), ImVec2(110, 0))) {
    r.seed = r.seed * 1664525u + 1013904223u + 7u;
    r.sched.changed(wallNow(), false);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%.1f ms", r.runMs);
  ImGui::Spacing();
  ImGui::Separator();
  bool accept = ImGui::Button(i18n::tr("Accept"), ImVec2(110, 0));
  ImGui::SameLine();
  bool cancel = ImGui::Button(i18n::tr("Cancel"), ImVec2(110, 0));
  if (accept && (r.sched.needsFull() || !r.have)) {
    runNow(a, s, r, true);
    r.sched.ran(script::PreviewScheduler::Run::Full, wallNow(), r.lastLimitHit);
  }
  if (r.pv && (accept || cancel)) {
    a.engine.preview(nullptr);
    r.pv = nullptr;
  }
  if (accept) {
    if (!r.last.ok) {
      say(s, "The script has an error; nothing was applied");
      return;
    }
    const std::string label = "Script: " + r.script.name();
    if (r.last.changed) {
      s.sel = r.last.sel;  // kept valid by the edit's completion (clamped to the new version)
      Content next = r.last.content;
      runEdit(a, s, label, [next](const Content&) { return next; });
    } else {
      s.sel = r.last.sel;
      say(s, label + ": no change to the audio");
    }
    ImGui::CloseCurrentPopup();
  } else if (cancel) {
    ImGui::CloseCurrentPopup();
  }
}

}  // namespace gl::packs::edison
