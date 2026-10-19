// Gridline — CPU profiling: samples the engine's per-object CPU counters into
// App::perf and draws the "Performance" window (top consumers, load, threads,
// delay compensation).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/PluginInfo.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl {

namespace {
constexpr double kPeriod = 0.5;  // seconds between samples
}

void updatePerformance(App& a) {
  // Counting costs two clock reads per object per block: only while someone looks.
  const bool want = a.showPerformance || a.showMixer;
  if (a.engine.profiling() != want) a.engine.setProfiling(want);
  auto& pf = a.perf;
  if (!want) {
    pf.load.clear();
    pf.lastNs.clear();
    pf.lastT = -1;
    return;
  }
  if (pf.lastT >= 0 && a.now - pf.lastT < kPeriod) return;
  const double dt = pf.lastT < 0 ? 0.0 : a.now - pf.lastT;
  pf.lastT = a.now;
  pf.cpuPeak = a.engine.takeCpuPeak();
  const auto& prog = a.engine.program();
  if (!prog) return;
  std::map<App::PerfStats::Key, uint64_t> ns;
  for (const auto& c : prog->channels)
    if (c.rt) ns[{App::PerfStats::Channel, c.id, -1}] = c.rt->cpu.load();
  for (const auto& m : prog->mixer) {
    if (m.rt) ns[{App::PerfStats::Insert, m.id, -1}] = m.rt->cpu.load();
    for (size_t k = 0; k < m.slots.size(); ++k)
      if (m.slots[k].rt) ns[{App::PerfStats::Slot, m.id, static_cast<int>(k)}] = m.slots[k].rt->cpu.load();
  }
  pf.load.clear();
  if (dt > 0)
    for (const auto& [key, v] : ns) {
      auto it = pf.lastNs.find(key);
      if (it == pf.lastNs.end() || v < it->second) continue;  // new runtime: next period
      const float l = static_cast<float>(static_cast<double>(v - it->second) / (dt * 1e9));
      if (l > 0) pf.load[key] = l;
    }
  pf.lastNs = std::move(ns);
}

void drawPerformance(App& a) {
  if (!a.showPerformance) return;
  ImGui::SetNextWindowSize(ImVec2(440, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Performance", &a.showPerformance)) {
    ImGui::End();
    return;
  }
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  const double sr = std::max(1.0, a.engine.sampleRate());

  // ---- summary ----
  ui::BeginSection("ENGINE");
  auto stat = [&](const char* label, const std::string& value, ImU32 col) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", label);
    ImGui::SameLine(150);
    ImGui::PushFont(a.fontMono, theme::kFontNumber);
    ImGui::TextColored(theme::vec(col), "%s", value.c_str());
    ImGui::PopFont();
  };
  char buf[96];
  const float load = a.engine.cpuLoad();
  std::snprintf(buf, sizeof buf, "%.0f%%   peak %.0f%%", load * 100.0f, a.perf.cpuPeak * 100.0f);
  stat("Audio load", buf, std::max(load, a.perf.cpuPeak) > 0.8f ? P.danger : P.text);
  {  // load meter: segmented lamp bar (green, amber above 60 %, red above 85 %) with a peak tick
    ImGui::SetCursorPosX(150);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(80.0f, ImGui::GetContentRegionAvail().x), h = 6;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), P.field, 2);
    const int segs = 24;
    const float sw = w / segs;
    for (int i = 0; i < segs; ++i) {
      const float f0 = static_cast<float>(i) / segs;
      if (f0 >= load) break;
      const ImU32 c = f0 >= 0.85f ? P.meterHigh : f0 >= 0.6f ? P.meterMid : P.meterLow;
      dl->AddRectFilled(ImVec2(std::round(p.x + i * sw + 1), p.y + 1), ImVec2(std::round(p.x + (i + 1) * sw - 1), p.y + h - 1), c, 1);
    }
    const float pk = std::clamp(a.perf.cpuPeak, 0.0f, 1.0f);
    if (pk > 0) dl->AddLine(ImVec2(std::round(p.x + w * pk), p.y), ImVec2(std::round(p.x + w * pk), p.y + h), P.text, 1.5f);
    ImGui::Dummy(ImVec2(w, h));
  }
  ui::Hint("Time spent rendering each audio buffer (on every thread, including waiting for worker threads) against the buffer's duration (same as the top bar)");
  {
    float sum = 0;
    for (const auto& kv : a.perf.load) sum += kv.second;
    std::snprintf(buf, sizeof buf, "%.1f%% of one core", sum * 100.0f);
    stat("Processing", buf, P.text);
    ui::Hint("Sum of the consumers below (channels, inserts, effects). Spread over several threads it can exceed the audio load");
  }
  const int rtw = a.engine.realtimeWorkers(), wk = a.engine.workerThreads();
  if (wk > 0 && rtw < wk)
    std::snprintf(buf, sizeof buf, "%d  (%d of %d workers without real-time priority)", a.engine.threads(), wk - rtw, wk);
  else
    std::snprintf(buf, sizeof buf, "%d", a.engine.threads());
  stat("Threads", buf, wk > 0 && rtw < wk ? P.danger : P.text);
  const int ol = a.engine.outputLatency();
  const bool pdc = a.settings.getBool("audio.pdc");
  if (pdc)
    std::snprintf(buf, sizeof buf, "on   %d samples (%.1f ms)", ol, 1000.0 * ol / sr);
  else
    std::snprintf(buf, sizeof buf, "off");
  stat("Delay compensation", buf, P.text);
  ui::EndSection();

  // ---- top consumers ----
  ui::BeginSection("TOP CONSUMERS");
  ImGui::TextColored(theme::vec(P.textFaint), "Share of one core's real time over the last half second.");
  struct Row {
    App::PerfStats::Key key;
    float load;
  };
  std::vector<Row> rows;
  for (const auto& [k, l] : a.perf.load) rows.push_back({k, l});
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.load > y.load; });
  if (rows.size() > 10) rows.resize(10);
  if (rows.empty()) {
    ImGui::Dummy(ImVec2(0, theme::kS2));
    ImGui::TextColored(theme::vec(P.textFaint), a.engine.playing() ? "Measuring..." : "Start playback to measure.");
  }
  const float maxLoad = rows.empty() ? 1.0f : std::max(rows[0].load, 0.01f);
  if (!rows.empty() && ImGui::BeginTable("##top", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("pct", ImGuiTableColumnFlags_WidthFixed, 54.0f);
    int n = 0;
    for (const auto& r : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      std::string name, kind;
      const int mi = s.mixerIndex(r.key.id);
      if (r.key.kind == App::PerfStats::Channel) {
        const Channel* c = s.channel(r.key.id);
        name = c ? c->name : "(removed channel)";
        kind = "Channel";
      } else if (mi >= 0) {
        name = mixerTrackName(s, static_cast<size_t>(mi));
        kind = mi == 0 ? "Master" : "Insert " + std::to_string(mi);
        if (r.key.kind == App::PerfStats::Slot) {
          const auto& slot = s.mixer[static_cast<size_t>(mi)].slots[static_cast<size_t>(r.key.slot)];
          const PluginInfo* info = findPlugin(slot.plugin.type);
          kind = name + " \xE2\x80\xBA slot " + std::to_string(r.key.slot + 1);
          name = info ? info->name : slot.plugin.type;
        } else {
          name += " (mixing)";
        }
      } else {
        name = "(removed track)";
      }
      ImGui::PushID(n++);
      if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
        if (r.key.kind == App::PerfStats::Channel) {
          a.selChannel = r.key.id;
        } else if (mi >= 0) {
          a.selMixer = r.key.id;
          a.showMixer = true;
          if (r.key.kind == App::PerfStats::Slot) a.fxEditor = {r.key.id, r.key.slot};
        }
      }
      ImGui::PopID();
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", kind.c_str());
      ImGui::TableNextColumn();
      {
        ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 4, y = p.y + (ImGui::GetTextLineHeight() - h) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + h), P.field, 2);
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * std::min(1.0f, r.load / maxLoad), y + h),
                          r.load > 0.25f ? P.meterHigh : r.load > 0.1f ? P.meterMid : P.meterLow, 2);
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight()));
      }
      ImGui::TableNextColumn();
      ImGui::PushFont(a.fontMono, theme::kFontNumber);
      ImGui::Text("%5.1f%%", r.load * 100.0f);
      ImGui::PopFont();
    }
    ImGui::EndTable();
  }
  ui::EndSection();
  ImGui::End();
}

}  // namespace gl
