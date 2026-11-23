// Gridline — CPU profiling: samples the engine's per-object CPU counters into
// App::perf and draws the "Performance" window (top consumers, load, threads,
// delay compensation).
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../core/I18n.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/PluginInfo.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl {

namespace {
constexpr double kPeriod = 0.5;  // seconds between samples

// A consumer's name and kind ("Channel", "Insert 3", "<track> > slot 2") and its mixer index (-1:
// none). `translated`: the words in the interface language (the Tracker's panel; the other layouts
// keep theirs as they were).
int consumerName(const Song& s, const App::PerfStats::Key& key, std::string* name, std::string* kind, bool translated = false) {
  char b[160];
  const int mi = s.mixerIndex(key.id);
  if (key.kind == App::PerfStats::Channel) {
    const Channel* c = s.channel(key.id);
    *name = c ? c->name : (translated ? i18n::tr("(removed channel)") : "(removed channel)");
    *kind = (translated ? i18n::tr("Channel") : "Channel");
  } else if (mi >= 0) {
    *name = mixerTrackName(s, static_cast<size_t>(mi));
    if (translated) {
      std::snprintf(b, sizeof b, i18n::tr("Insert %d"), mi);
      *kind = mi == 0 ? i18n::tr("Master") : b;
    } else {
      *kind = mi == 0 ? "Master" : "Insert " + std::to_string(mi);
    }
    if (key.kind == App::PerfStats::Slot) {
      const auto& slot = s.mixer[static_cast<size_t>(mi)].slots[static_cast<size_t>(key.slot)];
      const PluginInfo* info = findPlugin(slot.plugin.type);
      if (translated) {
        std::snprintf(b, sizeof b, i18n::tr("slot %d"), key.slot + 1);
        *kind = *name + " \xE2\x80\xBA " + b;
      } else {
        *kind = *name + " \xE2\x80\xBA slot " + std::to_string(key.slot + 1);
      }
      *name = info ? info->name : slot.plugin.type;
    } else {
      *name += (translated ? i18n::tr(" (mixing)") : " (mixing)");
    }
  } else {
    *name = (translated ? i18n::tr("(removed track)") : "(removed track)");
  }
  return mi;
}

// A click on a consumer: its channel selected, or its mixer track (and the effect's editor) shown.
void showConsumer(App& a, const App::PerfStats::Key& key, int mi) {
  if (key.kind == App::PerfStats::Channel) {
    a.selChannel = key.id;
  } else if (mi >= 0) {
    a.selMixer = key.id;
    a.showMixer = true;
    if (key.kind == App::PerfStats::Slot) a.fxEditor = {key.id, key.slot};
  }
}

// ---- the Tracker's Performance panel (0.7.8) ----
// In the Tracker the panel is drawn with the header band's pieces (TrackerLook.h): one raised bevel box
// over the panel, three boxes in it (Engine, Load history, Top consumers) with their titles in the bold
// face, the readouts in sunken fields in the monospaced face and the values' ink, the load meter as lamp
// segments and the history as a scope in a sunken well (the band's scope ink, hard pixel edges), the
// consumers in a sunken list with the band's scroll bar. The other layouts keep their window.
bool trackerLook(const App& a) { return layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker); }
namespace sp = trackerview::space;

// The load history: one point each kHistStep seconds, the last kHistN of them.
constexpr int kHistN = 120;
constexpr double kHistStep = 0.25;
float gHist[kHistN] = {};
int gHistAt = 0, gHistCount = 0;
double gHistT = -1;
double gListScroll = 0;

// What the last Tracker draw laid out (the dev report and the tests).
struct TrkReport {
  int frame = -1;
  ImRect win, engine, history, list;
  float need = 0;      // the width the panel asks for (its rail left out)
  int texts = 0, clipped = 0, wrapped = 0, rows = 0;
  bool historyShown = false;
  bool rtNote = false;  // the Threads note (workers without real-time priority) was drawn
  std::string cut;  // the texts cut short or wrapped: " clip \"<text>\" need>room"
} gTrk;

void noteFit(const char* kind, const char* text, float need, float room) {
  ++gTrk.texts;
  if (need <= room + 0.5f) return;
  char b[200];
  std::snprintf(b, sizeof b, " %s \"%.80s\" %.0f>%.0f", kind, text, static_cast<double>(need), static_cast<double>(room));
  gTrk.cut += b;
  if (std::strcmp(kind, "wrap") == 0) ++gTrk.wrapped;
  else ++gTrk.clipped;
}

// The Threads note when some worker threads lack real-time priority (in the Tracker, on its own line).
const char* const kThreadNote = i18n::N_("%d of %d workers without real-time priority");
const char* const kEngineLabels[] = {i18n::N_("Audio load"), i18n::N_("Processing"), i18n::N_("Threads"), i18n::N_("Delay compensation")};
float engineLabelW(App& a) {
  return trackerview::labelColumn(a, {kEngineLabels[0], kEngineLabels[1], kEngineLabels[2], kEngineLabels[3]}, 2 * sp::kGroup);
}
// The widest readout the panel is made to show on one line (a long delay, the load and its peak).
float widestValue(App& a) {
  ImFont* mono = trackerview::monoFont(a);
  const float fs = ImGui::GetFontSize();
  char b[200];
  float w = 0;
  std::snprintf(b, sizeof b, i18n::tr("on   %d samples (%.1f ms)"), 9999, 999.9);
  w = std::max(w, mono->CalcTextSizeA(fs, FLT_MAX, 0, b).x);
  std::snprintf(b, sizeof b, i18n::tr("%d%%   peak %.0f%%"), 100, 100.0);
  w = std::max(w, mono->CalcTextSizeA(fs, FLT_MAX, 0, b).x);
  std::snprintf(b, sizeof b, i18n::tr("%.1f%% of one core"), 999.9);
  w = std::max(w, mono->CalcTextSizeA(fs, FLT_MAX, 0, b).x);
  return std::ceil(w);
}

// A label and its readout in a sunken field from `fx` to `x1`; the value wraps onto more lines (the field
// grows) when it does not fit. Returns the row's bottom.
float readout(App& a, ImDrawList* dl, float x, float fx, float x1, float y, const char* label, const char* value, ImU32 ink, const char* hint) {
  const auto& P = theme::pal();
  ImFont* mono = trackerview::monoFont(a);
  const float fs = ImGui::GetFontSize();
  const float rowH = std::round(fs + 8);
  const float room = x1 - fx - 2 * sp::kPad;
  const float one = mono->CalcTextSizeA(fs, FLT_MAX, 0, value).x;
  noteFit("wrap", value, one, room);
  const float th = mono->CalcTextSizeA(fs, FLT_MAX, std::max(1.0f, room), value).y;
  const float h = std::max(rowH, std::round(th + 8));
  const char* lb = i18n::tr(label);
  noteFit("clip", lb, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, lb).x, fx - x - sp::kGroup);
  dl->AddText(a.fontRegular, fs, ImVec2(x, std::round(y + (rowH - fs) * 0.5f)), P.textDim, lb);
  trackerview::bevelBox(dl, ImVec2(fx, y), ImVec2(x1, y + h), true);
  dl->AddText(mono, fs, ImVec2(fx + sp::kPad, std::round(y + (rowH - fs) * 0.5f)), ink, value, nullptr, std::max(1.0f, room));
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ImGui::PushID(label);
  ImGui::InvisibleButton("##ro", ImVec2(std::max(1.0f, x1 - x), h));
  ImGui::PopID();
  if (hint) ui::Hint(hint);
  return y + h;
}

// The load meter: lamp segments in a sunken well (the meter's three colours), the period's peak a tick.
void loadMeter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float load, float peak) {
  const auto& P = theme::pal();
  trackerview::bevelBox(dl, p0, p1, true);
  const trackerview::Aliased hard(dl);
  const float w = p1.x - p0.x - 4;
  const int segs = std::clamp(static_cast<int>(w / 7), 12, 40);
  const float sw = w / static_cast<float>(segs);
  for (int i = 0; i < segs; ++i) {
    const float f0 = static_cast<float>(i) / static_cast<float>(segs);
    const bool lit = f0 < load;
    const ImU32 c = f0 >= 0.85f ? P.meterHigh : f0 >= 0.6f ? P.meterMid : P.meterLow;
    const ImVec2 q0(std::round(p0.x + 2 + static_cast<float>(i) * sw + 1), p0.y + 3), q1(std::round(p0.x + 2 + static_cast<float>(i + 1) * sw - 1), p1.y - 3);
    dl->AddRectFilled(q0, q1, lit ? c : theme::withAlpha(c, 0.14f));
  }
  const float pk = std::clamp(peak, 0.0f, 1.0f);
  if (pk > 0) {
    const float x = std::round(p0.x + 2 + w * pk);
    dl->AddRectFilled(ImVec2(x - 1, p0.y + 1), ImVec2(x + 1, p1.y - 1), P.text);
  }
}

// The load history: a scope in a sunken well, the band's scope ink, a faint line each quarter of its
// scale; the newest point at the right. The scale is the next quarter above the highest point (25, 50,
// 75 or 100 percent), so a light load still shows its shape; the danger line (85 percent) when in scale.
void historyGraph(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const auto& P = theme::pal();
  trackerview::bevelBox(dl, p0, p1, true);
  const trackerview::Aliased hard(dl);
  const float x0 = p0.x + 3, x1 = p1.x - 3, y0 = p0.y + 3, y1 = p1.y - 3, h = y1 - y0;
  float top = 0;
  for (int k = 0; k < gHistCount; ++k) top = std::max(top, gHist[(gHistAt - gHistCount + k + kHistN) % kHistN]);
  const float scale = std::clamp(std::ceil(top * 4.0f + 1e-4f) / 4.0f, 0.25f, 1.0f);
  for (int q = 1; q < 4; ++q) {
    const float y = std::round(y1 - h * 0.25f * static_cast<float>(q));
    for (float x = x0; x < x1; x += 4) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1, y + 1), theme::withAlpha(P.textFaint, 0.5f));
  }
  if (scale >= 0.85f) {
    const float yd = std::round(y1 - h * 0.85f / scale);
    dl->AddRectFilled(ImVec2(x0, yd), ImVec2(x1, yd + 1), theme::withAlpha(P.meterHigh, 0.45f));
  }
  ImFont* mono = trackerview::monoFont(a);
  const float sfs = std::round(ImGui::GetFontSize() * 0.8f);
  if (h > sfs * 2 + 4) {  // the scale's ends, small, as the band's small scope numbers
    char t[16];
    std::snprintf(t, sizeof t, "%d%%", static_cast<int>(std::lround(scale * 100.0f)));
    dl->AddText(mono, sfs, ImVec2(x0 + 1, y0), P.textDim, t);
    dl->AddText(mono, sfs, ImVec2(x0 + 1, y1 - sfs), P.textDim, "0%");
  }
  if (gHistCount < 2) {
    dl->AddRectFilled(ImVec2(x0, y1 - 1), ImVec2(x1, y1), theme::withAlpha(P.led, 0.55f));
    return;
  }
  const float step = (x1 - x0) / static_cast<float>(kHistN - 1);
  ImVec2 prev;
  for (int k = 0; k < gHistCount; ++k) {
    const int idx = (gHistAt - gHistCount + k + kHistN) % kHistN;
    const float v = std::clamp(gHist[idx], 0.0f, 1.0f);
    const ImVec2 pt(std::round(x1 - static_cast<float>(gHistCount - 1 - k) * step), std::round(y1 - 1 - v / scale * (h - 1)));
    if (k > 0) dl->AddQuadFilled(ImVec2(prev.x, y1), prev, pt, ImVec2(pt.x, y1), theme::withAlpha(v >= 0.85f ? P.meterHigh : P.led, 0.18f));
    if (k > 0) dl->AddLine(ImVec2(prev.x + 0.5f, prev.y + 0.5f), ImVec2(pt.x + 0.5f, pt.y + 0.5f), v >= 0.85f ? P.meterHigh : P.led, 1.0f);
    prev = pt;
  }
}

void drawPerformanceTracker(App& a) {
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  const double sr = std::max(1.0, a.engine.sampleRate());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* mono = trackerview::monoFont(a);
  const float fs = ImGui::GetFontSize();
  const ImVec2 sp0 = ImGui::GetCursorScreenPos();
  const ImRect r = ImGui::GetCurrentWindow()->InnerRect;
  gTrk = TrkReport{};
  gTrk.frame = ImGui::GetFrameCount();
  gTrk.win = r;
  gTrk.need = performanceTrackerWidth(a);
  trackerview::bevelBox(dl, r.Min, r.Max, false);
  const float load = a.engine.cpuLoad();
  if (gHistT < 0 || a.now - gHistT >= kHistStep || a.now < gHistT) {
    gHistT = a.now;
    gHist[gHistAt] = load;
    gHistAt = (gHistAt + 1) % kHistN;
    gHistCount = std::min(kHistN, gHistCount + 1);
  }
  const float bx0 = r.Min.x + sp::kPad, bx1 = r.Max.x - sp::kPad;
  const float ix0 = bx0 + sp::kGroup, ix1 = bx1 - sp::kGroup;  // inside a box
  const float titleH = std::round(fs + sp::kGroup + sp::kPad);
  char buf[200];

  // ---- Engine ----
  float y = r.Min.y + sp::kPad;
  const float e0 = y;
  const float labelW = engineLabelW(a), fx = ix0 + labelW;
  // the box goes under its rows (channel 0); its height is known once they are laid out
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  float ry = y + titleH;
  std::snprintf(buf, sizeof buf, i18n::tr("%d%%   peak %.0f%%"), CpuMeter::displayPercent(load), a.perf.cpuPeak * 100.0f);
  ry = readout(a, dl, ix0, fx, ix1, ry, kEngineLabels[0], buf, std::max(load, a.perf.cpuPeak) > 0.8f ? P.danger : trackerview::valueInk(),
               "Time spent rendering each audio buffer (on every thread, including waiting for worker threads) against the buffer's duration (same as the top bar)");
  ry += sp::kGap;
  const float mh = std::round(std::max(10.0f, fs * 0.85f));
  loadMeter(dl, ImVec2(fx, ry), ImVec2(ix1, ry + mh), load, a.perf.cpuPeak);
  ry += mh + sp::kGroup;
  {
    float sum = 0;
    for (const auto& kv : a.perf.load) sum += kv.second;
    std::snprintf(buf, sizeof buf, i18n::tr("%.1f%% of one core"), sum * 100.0f);
    ry = readout(a, dl, ix0, fx, ix1, ry, kEngineLabels[1], buf, trackerview::valueInk(),
                 "Sum of the consumers below (channels, inserts, effects). Spread over several threads it can exceed the audio load") + sp::kGap;
  }
  const int rtw = a.engine.realtimeWorkers(), wk = a.engine.workerThreads();
  const bool noRt = wk > 0 && rtw < wk;
  std::snprintf(buf, sizeof buf, "%d", a.engine.threads());
  ry = readout(a, dl, ix0, fx, ix1, ry, kEngineLabels[2], buf, noRt ? P.danger : trackerview::valueInk(), nullptr) + sp::kGap;
  gTrk.rtNote = noRt;
  if (noRt) {  // the note on a line of its own under the field, the box's full width (a system without real-time limits)
    std::snprintf(buf, sizeof buf, i18n::tr(kThreadNote), wk - rtw, wk);
    const float room = ix1 - ix0;
    noteFit("wrap", buf, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, buf).x, room);
    const float th = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, std::max(1.0f, room), buf).y;
    dl->AddText(a.fontRegular, fs, ImVec2(ix0, ry), P.danger, buf, nullptr, std::max(1.0f, room));
    ImGui::SetCursorScreenPos(ImVec2(ix0, ry));
    ImGui::InvisibleButton("##rtnote", ImVec2(std::max(1.0f, room), std::max(1.0f, th)));
    ui::Hint("The audio engine's worker threads run at normal priority, so other programs can delay them and the sound can break up. "
             "On Linux, allow real-time priority (rtkit, or the audio group's limits) and start Gridline again");
    ry += std::round(th) + sp::kGap;
  }
  const int ol = a.engine.outputLatency();
  if (a.settings.getBool("audio.pdc"))
    std::snprintf(buf, sizeof buf, i18n::tr("on   %d samples (%.1f ms)"), ol, 1000.0 * ol / sr);
  else
    std::snprintf(buf, sizeof buf, "%s", i18n::tr("off"));
  ry = readout(a, dl, ix0, fx, ix1, ry, kEngineLabels[3], buf, trackerview::valueInk(), nullptr);
  const float e1 = ry + sp::kGroup;
  dl->ChannelsSetCurrent(0);
  trackerview::bevelBox(dl, ImVec2(bx0, e0), ImVec2(bx1, e1));
  dl->ChannelsMerge();
  trackerview::boxTitle(a, ImVec2(ix0, e0 + sp::kPad), i18n::N_("Engine"));
  gTrk.engine = ImRect(ImVec2(bx0, e0), ImVec2(bx1, e1));
  y = e1 + sp::kGap;

  // ---- Top consumers: the list's room first (the history takes what is left over it) ----
  const char* caption = i18n::tr("Share of one core's real time over the last half second.");
  const float capW = ix1 - ix0;
  const float capH = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, std::max(1.0f, capW), caption).y;
  noteFit("wrap", caption, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, caption).x, capW);
  const float listRowH = std::round(fs + 6);
  const float listChrome = titleH + capH + sp::kGroup + 2 + sp::kGroup;  // title, caption, the well's edges, the box's foot
  const float bottom = r.Max.y - sp::kPad;
  const float graphMin = std::round(fs * 3), graphMax = std::round(fs * 6);
  const float histChrome = titleH + sp::kGroup + sp::kGap;
  const float listMin = listChrome + listRowH * 3;
  const float spare = bottom - y - listMin - histChrome;
  const bool history = spare >= graphMin;
  gTrk.historyShown = history;
  if (history) {
    const float gh = std::min(graphMax, spare);
    const float h0 = y, h1 = y + titleH + gh + sp::kGroup;
    trackerview::bevelBox(dl, ImVec2(bx0, h0), ImVec2(bx1, h1));
    trackerview::boxTitle(a, ImVec2(ix0, h0 + sp::kPad), i18n::N_("Load history"));
    const char* span = i18n::tr("last 30 seconds");
    const float tw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, span).x;
    const float titleW = (a.fontBold ? a.fontBold : a.fontRegular)->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Load history")).x;
    noteFit("clip", span, tw, ix1 - ix0 - titleW - sp::kGroup * 2);
    dl->AddText(a.fontRegular, fs, ImVec2(std::round(ix1 - tw), h0 + sp::kPad), P.textFaint, span);
    const ImVec2 g0(ix0, h0 + titleH), g1(ix1, h0 + titleH + gh);
    historyGraph(a, dl, g0, g1);
    ImGui::SetCursorScreenPos(g0);
    ImGui::InvisibleButton("##hist", ImVec2(std::max(1.0f, g1.x - g0.x), std::max(1.0f, g1.y - g0.y)));
    ui::Hint("The audio load over the last half minute, newest at the right. The scale grows to fit the highest point; the red line marks 85 percent");
    gTrk.history = ImRect(ImVec2(bx0, h0), ImVec2(bx1, h1));
    y = h1 + sp::kGap;
  }
  const float l0 = y, l1 = std::max(bottom, y + listMin);
  trackerview::bevelBox(dl, ImVec2(bx0, l0), ImVec2(bx1, l1));
  trackerview::boxTitle(a, ImVec2(ix0, l0 + sp::kPad), i18n::N_("Top consumers"));
  dl->AddText(a.fontRegular, fs, ImVec2(ix0, l0 + titleH), P.textFaint, caption, nullptr, std::max(1.0f, capW));
  const ImVec2 w0(ix0, std::round(l0 + titleH + capH + sp::kGroup)), w1(ix1, l1 - sp::kGroup);
  trackerview::bevelBox(dl, w0, w1, true);
  gTrk.list = ImRect(ImVec2(bx0, l0), ImVec2(bx1, l1));
  struct Row {
    App::PerfStats::Key key;
    float load;
  };
  std::vector<Row> rows;
  for (const auto& [k, l] : a.perf.load) rows.push_back({k, l});
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& q) { return x.load > q.load; });
  if (rows.size() > 10) rows.resize(10);
  gTrk.rows = static_cast<int>(rows.size());
  if (rows.empty()) {
    const char* msg = i18n::tr(a.engine.playing() ? "Measuring..." : "Start playback to measure.");
    noteFit("clip", msg, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, msg).x, w1.x - w0.x - 2 * sp::kPad);
    theme::centreText(dl, a.fontRegular, fs, w0, w1, P.textFaint, msg);
    gListScroll = 0;
  } else {
    const float view = w1.y - w0.y - 2;
    const float total = listRowH * static_cast<float>(rows.size());
    const bool bar = total > view + 0.5f;
    const float barW = std::round(std::max(12.0f, fs + 1));
    const float rx0 = w0.x + 1, rx1 = w1.x - 1 - (bar ? barW + sp::kLine : 0.0f);
    gListScroll = std::clamp(gListScroll, 0.0, static_cast<double>(std::max(0.0f, total - view)));
    if (bar) {
      if (ImGui::IsMouseHoveringRect(w0, w1) && ImGui::GetIO().MouseWheel != 0) gListScroll -= ImGui::GetIO().MouseWheel * listRowH * 2;
      trackerview::vScrollBar(a, "##perfsb", ImVec2(w1.x - 1 - barW, w0.y + 1), ImVec2(w1.x - 1, w1.y - 1), &gListScroll, view, total, listRowH);
      gListScroll = std::clamp(gListScroll, 0.0, static_cast<double>(std::max(0.0f, total - view)));
    }
    const float pctW = std::ceil(mono->CalcTextSizeA(fs, FLT_MAX, 0, "100.0%").x) + 2 * sp::kPad;
    const float barCol = std::round(std::clamp((rx1 - rx0) * 0.26f, 40.0f, 140.0f));
    const float maxLoad = std::max(rows[0].load, 0.01f);
    dl->PushClipRect(ImVec2(rx0, w0.y + 1), ImVec2(rx1, w1.y - 1), true);
    int n = 0;
    for (const Row& row : rows) {
      const float yy = std::round(w0.y + 1 + listRowH * static_cast<float>(n) - static_cast<float>(gListScroll));
      std::string name, kind;
      const int mi = consumerName(s, row.key, &name, &kind, true);
      ImGui::PushID(n++);
      if (yy + listRowH > w0.y && yy < w1.y) {
        ImGui::SetCursorScreenPos(ImVec2(rx0, yy));
        const bool click = ImGui::InvisibleButton("##cons", ImVec2(std::max(1.0f, rx1 - rx0), listRowH));
        const bool hov = ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(w0, w1);
        if (hov) dl->AddRectFilled(ImVec2(rx0, yy), ImVec2(rx1, yy + listRowH), theme::withAlpha(P.surface600, 0.6f));
        if (click) showConsumer(a, row.key, mi);
        const std::string tip = name + "  (" + kind + ")";
        if (hov) ui::Hint(tip);
        const float ty = std::round(yy + (listRowH - fs) * 0.5f);
        const float px1 = rx1 - sp::kPad;
        std::snprintf(buf, sizeof buf, "%5.1f%%", static_cast<double>(row.load * 100.0f));
        const float pw = mono->CalcTextSizeA(fs, FLT_MAX, 0, buf).x;
        dl->AddText(mono, fs, ImVec2(std::round(px1 - pw), ty), trackerview::valueInk(), buf);
        const float b1 = px1 - pctW, b0 = b1 - barCol;
        {
          const ImVec2 q0(b0, std::round(yy + listRowH * 0.5f - 4)), q1(b1, std::round(yy + listRowH * 0.5f + 4));
          trackerview::bevelBox(dl, q0, q1, true);
          const trackerview::Aliased hard(dl);
          const float f = std::min(1.0f, row.load / maxLoad);
          if (f > 0) dl->AddRectFilled(ImVec2(q0.x + 1, q0.y + 1), ImVec2(std::round(q0.x + 1 + (q1.x - q0.x - 2) * f), q1.y - 1),
                                       row.load > 0.25f ? P.meterHigh : row.load > 0.1f ? P.meterMid : P.meterLow);
        }
        const float nx = rx0 + sp::kPad, nroom = b0 - sp::kGroup - nx;
        const float nw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str()).x;
        theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), nroom, P.text, name.c_str());
        const float kx = nx + nw + sp::kGroup;
        if (b0 - sp::kGroup - kx > fs * 2) theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(kx, ty), b0 - sp::kGroup - kx, P.textFaint, kind.c_str());
      }
      ImGui::PopID();
    }
    dl->PopClipRect();
  }
  ImGui::SetCursorScreenPos(sp0);
  ImGui::Dummy(ImVec2(1, 1));  // (the panel draws in place: nothing to scroll)
}
}  // namespace

float performanceTrackerWidth(App& a) {
  // the label column, the widest readout in its field, the box's and the panel's edges; at least the
  // Threads note on one line across the box (measured always, so the width does not hang on the system)
  char b[200];
  std::snprintf(b, sizeof b, i18n::tr(kThreadNote), 99, 99);
  const float note = a.fontRegular->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, b).x;
  return std::ceil(std::max(engineLabelW(a) + widestValue(a) + 2 * sp::kPad, note) + 2 * (sp::kPad + sp::kGroup) + 2);
}

void performanceDevReport(App& a, int frame) {
  const TrkReport& t = gTrk;
  std::printf("perfpanel frame %d: open %d tracker %d drawn %d win %.0f %.0f %.0f %.0f need %.0f history %d rows %d texts %d clipped %d wrapped %d note %d%s\n", frame,
              a.showPerformance ? 1 : 0, trackerLook(a) ? 1 : 0, t.frame >= ImGui::GetFrameCount() - 2 ? 1 : 0, static_cast<double>(t.win.Min.x),
              static_cast<double>(t.win.Min.y), static_cast<double>(t.win.GetWidth()), static_cast<double>(t.win.GetHeight()), static_cast<double>(t.need),
              t.historyShown ? 1 : 0, t.rows, t.texts, t.clipped, t.wrapped, t.rtNote ? 1 : 0, t.cut.c_str());
  if (ImGuiWindow* w = ImGui::FindWindowByName(ui::windowTitle("Performance").c_str()))
    std::printf("perfpanel window: %.0f %.0f %.0f %.0f docked %d\n", static_cast<double>(w->Pos.x), static_cast<double>(w->Pos.y), static_cast<double>(w->Size.x),
                static_cast<double>(w->Size.y), w->DockIsActive ? 1 : 0);
  std::fflush(stdout);
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
  const bool trk = trackerLook(a);  // the Tracker: its own panel (it lays itself out in place, nothing scrolls)
  if (!ImGui::Begin(ui::windowTitle("Performance").c_str(), &a.showPerformance, trk ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0)) {
    ImGui::End();
    return;
  }
  if (trk) {
    drawPerformanceTracker(a);
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
    ImGui::PushTextWrapPos(0);  // hunt-clips-1119: a narrow panel (Clips' side column) wraps it, never cuts it off
    ImGui::TextColored(theme::vec(col), "%s", value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
  };
  char buf[96];
  const float load = a.engine.cpuLoad();
  std::snprintf(buf, sizeof buf, "%d%%   peak %.0f%%", CpuMeter::displayPercent(load), a.perf.cpuPeak * 100.0f);
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
  ImGui::PushTextWrapPos(0);  // (hunt-clips-1119: wrapped in a narrow panel)
  ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Share of one core's real time over the last half second."));
  ImGui::PopTextWrapPos();
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
    ImGui::TableSetupColumn(i18n::tr("name"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(i18n::tr("bar"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn(i18n::tr("pct"), ImGuiTableColumnFlags_WidthFixed, 54.0f);
    int n = 0;
    for (const auto& r : rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      std::string name, kind;
      const int mi = consumerName(s, r.key, &name, &kind);
      ImGui::PushID(n++);
      if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) showConsumer(a, r.key, mi);
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
