// Gridline — "edison" pack: the tool panels, the Preview section and Loupe's panel dialogs
// (edison_repair_ui.h). Same frame as Loupe's other tool dialogs (edison_dialog.h): titled
// sections with a label column, presets, then Preview, then the footer.
#include "edison_repair_ui.h"
#include "../../core/ThreadName.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <vector>
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "edison_dialog.h"

namespace gl::packs::edison {

using namespace dlg;

namespace {

double now() { return ImGui::GetTime(); }

// ------------------------------------------------------------------ small views
struct Strip {
  std::vector<float> lo, hi;
};
Strip columns(const Audio& a, int w) {
  Strip s;
  if (w <= 0 || a.frames() == 0) return s;
  s.lo.assign(static_cast<size_t>(w), 0.f);
  s.hi.assign(static_cast<size_t>(w), 0.f);
  const int64_t n = a.frames();
  for (int x = 0; x < w; ++x) {
    const int64_t f0 = n * x / w, f1 = std::max(f0 + 1, n * (x + 1) / w);
    const int64_t step = std::max<int64_t>(1, (f1 - f0) / 256);  // long columns: a sample of them
    float lo = 0, hi = 0;
    for (int64_t i = f0; i < f1; i += step) {
      lo = std::min({lo, a.L[static_cast<size_t>(i)], a.R[static_cast<size_t>(i)]});
      hi = std::max({hi, a.L[static_cast<size_t>(i)], a.R[static_cast<size_t>(i)]});
    }
    s.lo[static_cast<size_t>(x)] = lo, s.hi[static_cast<size_t>(x)] = hi;
  }
  return s;
}

void drawStrip(ImDrawList* dl, const Strip& s, ImVec2 p0, ImVec2 p1, ImU32 col, float scale) {
  const float cy = std::round((p0.y + p1.y) * 0.5f), h = (p1.y - p0.y) * 0.5f - 2;
  for (size_t x = 0; x < s.lo.size(); ++x) {
    const float X = p0.x + static_cast<float>(x) + 0.5f;
    const float y0 = cy - std::min(1.0f, s.hi[x] * scale) * h, y1 = cy - std::max(-1.0f, s.lo[x] * scale) * h;
    dl->AddLine(ImVec2(X, y0), ImVec2(X, std::max(y1, y0 + 1)), col);
  }
}

// The target's overview with marks (regions red, ticks yellow).
void overview(const PanelContext& c, const std::vector<Range>& red, const std::vector<int64_t>& ticks) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x, h = 40;
  const ImVec2 q0 = ImGui::GetCursorScreenPos(), q1(q0.x + w, q0.y + h);
  ImGui::Dummy(ImVec2(w, h));
  skin::insetField(dl, q0, q1, C(col::WaveBg), theme::kRadius);
  const Range tr = c.target;
  if (tr.empty() || !c.audio) return;
  const float cy = std::round((q0.y + q1.y) * 0.5f), hh = h * 0.5f - 3;
  const int W = static_cast<int>(w) - 2;
  auto xOf = [&](double f) { return q0.x + 1 + static_cast<float>((f - static_cast<double>(tr.a)) / static_cast<double>(tr.len())) * static_cast<float>(W); };
  for (const Range& r : red) {
    float x0 = std::floor(xOf(static_cast<double>(r.a))), x1 = std::ceil(xOf(static_cast<double>(r.b)));
    if (x1 - x0 < 1) x1 = x0 + 1;
    dl->AddRectFilled(ImVec2(x0, q0.y + 2), ImVec2(x1, q1.y - 2), C(col::Rec, 0.4f));
  }
  const Audio& au = *c.audio;
  for (int x = 0; x < W; ++x) {
    const int64_t f0 = tr.a + tr.len() * x / W, f1 = std::max(f0 + 1, tr.a + tr.len() * (x + 1) / W);
    float mn = 0, mx = 0;
    if (c.peaks && c.peaks->built() && c.peaks->frames() == au.frames()) {
      float mn2 = 0, mx2 = 0;
      c.peaks->query(au, 0, f0, f1, mn, mx);
      c.peaks->query(au, 1, f0, f1, mn2, mx2);
      mn = std::min(mn, mn2), mx = std::max(mx, mx2);
    } else {
      const int64_t step = std::max<int64_t>(1, (f1 - f0) / 64);
      for (int64_t i = f0; i < f1 && i < au.frames(); i += step) mn = std::min(mn, au.L[static_cast<size_t>(i)]), mx = std::max(mx, au.L[static_cast<size_t>(i)]);
    }
    const float X = q0.x + 1 + static_cast<float>(x) + 0.5f;
    const float y0 = cy - std::min(1.0f, mx) * hh;
    dl->AddLine(ImVec2(X, y0), ImVec2(X, std::max(y0 + 1, cy - std::max(-1.0f, mn) * hh)), C(col::Wave, 0.85f));
  }
  for (const Range& r : red) {
    float x0 = std::floor(xOf(static_cast<double>(r.a))), x1 = std::ceil(xOf(static_cast<double>(r.b)));
    if (x1 - x0 < 2) x1 = x0 + 2;
    dl->AddRectFilled(ImVec2(x0, q0.y + 1), ImVec2(x1, q0.y + 4), C(col::Rec));
  }
  for (int64_t t : ticks) {
    const float x = std::round(xOf(static_cast<double>(t))) + 0.5f;
    dl->AddLine(ImVec2(x, q0.y + 2), ImVec2(x, q1.y - 2), C(col::Marker, 0.8f));
  }
}

bool checkRow(const char* label, float col, bool* v, const char* text) {
  row(label, col);
  ImGui::PushID(label);
  const bool c = ImGui::Checkbox(i18n::tr(text), v);
  ImGui::PopID();
  return c;
}

void textRow(const char* label, float col, const char* text) {
  const float x0 = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
  ImGui::TextUnformatted(i18n::tr(label));
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::SetCursorPosX(x0 + col);
  ImGui::TextUnformatted(text);
}

// A double setting through the float slider (shown times `scale`).
bool sliderD(const char* label, float col, double* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags = 0, double scale = 1) {
  float f = static_cast<float>(*v * scale);
  const bool ch = slider(label, col, &f, lo, hi, fmt, flags);
  if (ch) *v = f / scale;
  return ch;
}

// ------------------------------------------------------------------ per-key state
struct PreviewState {
  std::future<Audio> job;
  std::shared_ptr<Progress> jobProg;
  std::string jobKey;
  std::vector<std::future<Audio>> old;  // superseded renders, stopped, left to finish
  Audio orig, proc, removed;
  std::string key;
  bool have = false;
  Strip so, sp;
  int stripW = 0;
  bool on = false;
  int mode = 0;  // 0 processed, 1 original, 2 removed
  SamplePtr smp;
  double t0 = 0;
  int64_t at0 = 0, cycle = 0;
  std::string seenKey;
  double changedAt = -1;
};

template <class T>
struct Analysis {  // a background analysis of the target, redone when its key changes
  std::future<T> job;
  std::shared_ptr<Progress> prog;
  std::string key;
  T value{};
  bool have = false;
  // Starts `fn` when the key changes (an older run is stopped and waited for: they stop fast).
  template <class F>
  void want(const std::string& k, F fn) {
    if (k == key) return;
    if (job.valid()) {
      if (prog) prog->cancel = true;
      job.wait();
      job = {};
    }
    key = k;
    have = false;
    prog = std::make_shared<Progress>();
    auto pr = prog;
    job = std::async(std::launch::async, [fn, pr]() { gl::threads::setName("gl-loupe-repair"); return fn(pr.get()); });
  }
  void poll() {
    if (job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      value = job.get();
      have = true;
    }
  }
  float done() const { return prog ? prog->done.load() : 0.0f; }
};

struct PanelState {
  PreviewState pv;
  Analysis<DecayEstimate> decay;
  Analysis<ClipScan> clips;
  Analysis<std::vector<Range>> clicks;
  Analysis<LoopTempo> loop;
  bool dvMeasure = true;
  bool drSrcSet = false, drBarsSet = false, drDstInit = false;
  std::string drRange;
  int drag = -1;
  PanelMarks marks;
  ToolKind marksFor = ToolKind::Count;
};
std::map<uint64_t, PanelState>& panels() {
  static std::map<uint64_t, PanelState> m;
  return m;
}

std::string targetKey(const PanelContext& c) {
  char b[96];
  std::snprintf(b, sizeof b, "%p:%lld:%lld", static_cast<const void*>(c.audio.get()), static_cast<long long>(c.target.a), static_cast<long long>(c.target.b));
  return b;
}

std::string tt(const PanelContext& c, int64_t f) {
  if (c.timeText) return c.timeText(f);
  char b[32];
  std::snprintf(b, sizeof b, "%.3f s", c.audio ? static_cast<double>(f) / c.audio->rate : 0.0);
  return b;
}

// ------------------------------------------------------------------ the panels
bool denoisePanel(DenoiseSettings& d, const PanelContext& c) {
  const float col = column({i18n::N_("Noise"), i18n::N_("Learn"), i18n::N_("Reduction"), i18n::N_("Sensitivity"), i18n::N_("Smoothing"), i18n::N_("Character")});
  BeginSection(i18n::N_("NOISE"), kDlgW);
  int mode = d.adaptive ? 1 : 0;
  const char* modes[] = {i18n::tr("Learned profile"), i18n::tr("Adaptive (follows the noise)")};
  if (combo(i18n::N_("Noise"), col, &mode, modes, 2)) d.adaptive = mode == 1;
  if (c.takeProfile) {
    row(i18n::N_("Learn"), col);
    if (ui::TextButton(d.profile.valid() ? i18n::N_("Learn again from the selection") : i18n::N_("Learn from the selection"), false, ImGui::GetContentRegionAvail().x,
                       i18n::N_("Select a part with only noise, then learn its profile"))) {
      NoiseProfile np;
      if (c.takeProfile(np)) d.profile = np, d.adaptive = false;
    }
  }
  if (d.adaptive)
    note(i18n::tr("Adaptive: the noise floor is followed as it changes; no profile needed."));
  else if (d.profile.valid())
    note(i18n::tr("Profile learned. For noise that changes over time, choose Adaptive."));
  else
    note(i18n::tr("No profile yet: the noise floor is followed as it changes. For the best result, learn it from a part with only noise."));
  ui::EndSection();
  BeginSection(i18n::N_("REDUCTION"), kDlgW);
  sliderD(i18n::N_("Reduction"), col, &d.reductionDb, 0.f, 40.f, "%.0f dB");
  sliderD(i18n::N_("Sensitivity"), col, &d.sensitivity, 0.f, 100.f, "%.0f %%", 0, 100);
  sliderD(i18n::N_("Smoothing"), col, &d.smoothing, 0.f, 100.f, "%.0f %%", 0, 100);
  const char* ch[] = {i18n::tr("Broadband (hiss, room)"), i18n::tr("Tonal (hum, whine)")};
  combo(i18n::N_("Character"), col, &d.character, ch, 2);
  note(i18n::tr("More smoothing: steadier and more natural; less: quicker to follow the sound."));
  ui::EndSection();
  switch (presets({i18n::N_("Light"), i18n::N_("Medium"), i18n::N_("Strong")})) {
    case 0: d.reductionDb = 9, d.sensitivity = 0.45, d.smoothing = 0.6; break;
    case 1: d.reductionDb = 15, d.sensitivity = 0.5, d.smoothing = 0.5; break;
    case 2: d.reductionDb = 26, d.sensitivity = 0.6, d.smoothing = 0.45; break;
    default: break;
  }
  return true;
}

bool declickPanel(DeclickSettings& d, PanelState& ps, const PanelContext& c) {
  char k[160];
  std::snprintf(k, sizeof k, "%s %.3f %.3f", targetKey(c).c_str(), d.sensitivity, d.maxClickMs);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    auto au = c.audio;
    const Range tr = c.target;
    const DeclickSettings ds = d;
    ps.clicks.want(k, [au, tr, ds](Progress* p) { return findClicks(*au, tr, ds, p); });
  }
  ps.clicks.poll();
  const float col = column({i18n::N_("Sensitivity"), i18n::N_("Longest click")});
  BeginSection(i18n::N_("DETECTION"), kDlgW);
  sliderD(i18n::N_("Sensitivity"), col, &d.sensitivity, 0.f, 100.f, "%.0f %%", 0, 100);
  sliderD(i18n::N_("Longest click"), col, &d.maxClickMs, 0.1f, 10.f, "%.1f ms", ImGuiSliderFlags_Logarithmic);
  overview(c, ps.clicks.have ? ps.clicks.value : std::vector<Range>{}, {});
  char b[160];
  if (!ps.clicks.have)
    std::snprintf(b, sizeof b, i18n::tr("Looking for clicks... %.0f%%"), static_cast<double>(ps.clicks.done()) * 100.0);
  else if (ps.clicks.value.empty())
    std::snprintf(b, sizeof b, "%s", i18n::tr("No clicks found. Raise the sensitivity for fainter ones."));
  else
    std::snprintf(b, sizeof b, i18n::tr("Found %zu clicks, marked in red."), ps.clicks.value.size());
  statusNote(b);  // (the percent, then the result: one height)
  ui::EndSection();
  switch (presets({i18n::N_("Clicks"), i18n::N_("Crackle"), i18n::N_("Heavy")})) {
    case 0: d.sensitivity = 0.5, d.maxClickMs = 2.0; break;
    case 1: d.sensitivity = 0.75, d.maxClickMs = 0.8; break;
    case 2: d.sensitivity = 0.7, d.maxClickMs = 5.0; break;
    default: break;
  }
  ps.marks.regions = ps.clicks.have ? ps.clicks.value : std::vector<Range>{};
  ps.marks.ticks.clear();
  return ps.clicks.have && !ps.clicks.value.empty();
}

bool declipPanel(DeclipSettings& d, PanelState& ps, const PanelContext& c) {
  char k[160];
  std::snprintf(k, sizeof k, "%s %d %.2f", targetKey(c).c_str(), d.autoLevel ? 1 : 0, d.autoLevel ? 0.0 : d.clipDb);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    auto au = c.audio;
    const Range tr = c.target;
    const bool autoL = d.autoLevel;
    const double lv = d.clipDb;
    ps.clips.want(k, [au, tr, autoL, lv](Progress*) { return findClipping(*au, tr, autoL, lv); });
  }
  ps.clips.poll();
  const float col = column({i18n::N_("Clip level"), i18n::N_("Auto"), i18n::N_("Gain"), i18n::N_("Ceiling")});
  BeginSection(i18n::N_("DETECTION"), kDlgW);
  checkRow(i18n::N_("Auto"), col, &d.autoLevel, i18n::N_("Find the level the audio was clipped at"));
  ImGui::BeginDisabled(d.autoLevel);
  sliderD(i18n::N_("Clip level"), col, &d.clipDb, -6.f, 0.f, "%.2f dB");
  ImGui::EndDisabled();
  const ClipScan& sc = ps.clips.value;
  overview(c, ps.clips.have ? sc.regions : std::vector<Range>{}, {});
  char b[200];
  if (!ps.clips.have)
    std::snprintf(b, sizeof b, "%s", i18n::tr("Looking for clipping..."));
  else if (sc.regions.empty())
    std::snprintf(b, sizeof b, "%s", d.autoLevel ? i18n::tr("No clipping found. Turn Auto off to set a lower level.") : i18n::tr("No clipping found at this level."));
  else
    std::snprintf(b, sizeof b, i18n::tr("Found %zu clipped regions (%.1f ms in all) at %.2f dBFS, marked in red."), sc.regions.size(),
                  static_cast<double>(sc.samples) / std::max(1.0, c.audio->rate) * 1000.0 / (c.audio->channels == 2 ? 2.0 : 1.0),
                  20.0 * std::log10(std::max(1e-9, sc.level)));
  note(b);
  ui::EndSection();
  BeginSection(i18n::N_("OUTPUT"), kDlgW);
  checkRow(i18n::N_("Gain"), col, &d.fitGain, i18n::N_("Lower the gain so the rebuilt peaks fit"));
  ImGui::BeginDisabled(!d.fitGain);
  sliderD(i18n::N_("Ceiling"), col, &d.ceilingDb, -6.f, 0.f, "%.1f dBFS");
  ImGui::EndDisabled();
  note(i18n::tr("Rebuilt peaks rise above the old clip level; the gain makes room for them."));
  ui::EndSection();
  ps.marks.regions = ps.clips.have ? sc.regions : std::vector<Range>{};
  ps.marks.ticks.clear();
  return ps.clips.have && !sc.regions.empty();
}

bool deverbPanel(DeverbSettings& d, PanelState& ps, const PanelContext& c) {
  {
    auto au = c.audio;
    const Range tr = c.target;
    ps.decay.want(targetKey(c), [au, tr](Progress* p) { return estimateDecay(*au, tr, p); });
  }
  const bool wasHave = ps.decay.have;
  ps.decay.poll();
  const DecayEstimate& est = ps.decay.value;
  const bool measured = ps.decay.have && est.valid();
  if (ps.decay.have && !wasHave && measured) d.t60 = std::clamp(est.t60, 0.1, 8.0);
  d.estimate = est;
  d.useEstimate = ps.dvMeasure && measured;
  const float col = column({i18n::N_("Reverb time"), i18n::N_("Measure"), i18n::N_("Early"), i18n::N_("Amount")});
  BeginSection(i18n::N_("ROOM"), kDlgW);
  checkRow(i18n::N_("Measure"), col, &ps.dvMeasure, i18n::N_("Measure the reverb time from the audio"));
  ImGui::BeginDisabled(ps.dvMeasure && (measured || !ps.decay.have));
  sliderD(i18n::N_("Reverb time"), col, &d.t60, 0.1f, 8.0f, "%.2f s", ImGuiSliderFlags_Logarithmic);
  ImGui::EndDisabled();
  char b[200];
  if (!ps.decay.have)
    std::snprintf(b, sizeof b, i18n::tr("Measuring the room... %.0f%%"), static_cast<double>(ps.decay.done()) * 100.0);
  else if (measured)
    std::snprintf(b, sizeof b, i18n::tr("Measured %.2f s from %d decays (lows %.2f s, highs %.2f s)."), est.t60, est.decays, decayAt(est, 250), decayAt(est, 4000));
  else
    std::snprintf(b, sizeof b, "%s", i18n::tr("No clear decays to measure: set the reverb time by ear."));
  statusNote(b);  // (the percent, then the result: one height)
  checkRow(i18n::N_("Early"), col, &d.keepEarly, i18n::N_("Keep early reflections (only the late tail comes out)"));
  ui::EndSection();
  BeginSection(i18n::N_("REDUCTION"), kDlgW);
  sliderD(i18n::N_("Amount"), col, &d.amount, 0.f, 100.f, "%.0f %%", 0, 100);
  note(i18n::tr("Moderate amounts sound natural; high amounts dry the sound out further."));
  ui::EndSection();
  switch (presets({i18n::N_("Gentle"), i18n::N_("Medium"), i18n::N_("Strong")})) {
    case 0: d.amount = 0.35; break;
    case 1: d.amount = 0.6; break;
    case 2: d.amount = 0.9; break;
    default: break;
  }
  return ps.decay.have;
}

bool gatePanel(GateSettings& g) {
  const float col = column({i18n::N_("Threshold"), i18n::N_("Hysteresis"), i18n::N_("Attack"), i18n::N_("Hold"), i18n::N_("Release"), i18n::N_("Floor")});
  BeginSection(i18n::N_("GATE"), kDlgW);
  sliderD(i18n::N_("Threshold"), col, &g.thresholdDb, -90.f, 0.f, "%.1f dB");
  sliderD(i18n::N_("Hysteresis"), col, &g.hysteresisDb, 0.f, 20.f, "%.1f dB");
  sliderD(i18n::N_("Attack"), col, &g.attackMs, 0.f, 50.f, "%.1f ms");
  sliderD(i18n::N_("Hold"), col, &g.holdMs, 0.f, 500.f, "%.0f ms");
  sliderD(i18n::N_("Release"), col, &g.releaseMs, 1.f, 1000.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
  sliderD(i18n::N_("Floor"), col, &g.floorDb, -90.f, 0.f, g.floorDb <= -90 ? i18n::tr("silence") : "%.0f dB");
  note(i18n::tr("Opens above the threshold and closes the hysteresis below it, so it does not chatter."));
  ui::EndSection();
  return true;
}

bool drumPanel(DrumStretchSettings& d, PanelState& ps, const PanelContext& c) {
  const int beats = std::max(1, c.beatsPerBar);
  d.beatsPerBar = beats;
  char k[160];
  std::snprintf(k, sizeof k, "%s %.3f", targetKey(c).c_str(), d.sensitivity);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    auto au = c.audio;
    const Range tr = c.target;
    const double sens = d.sensitivity;
    ps.loop.want(k, [au, tr, sens, beats](Progress*) { return analyseLoop(*au, tr, sens, beats); });
  }
  const bool wasHave = ps.loop.have;
  ps.loop.poll();
  if (ps.drRange != targetKey(c)) ps.drRange = targetKey(c), ps.drSrcSet = false, ps.drBarsSet = false;
  if (!ps.drDstInit) d.targetBpm = std::clamp(c.songTempo, 40.0, 240.0), ps.drDstInit = true;
  const LoopTempo& lt = ps.loop.value;
  if (ps.loop.have && !wasHave && !ps.drSrcSet && lt.bpm > 0) d.sourceBpm = lt.bpm;
  const double rate = c.audio ? c.audio->rate : 44100.0;
  const double sec = static_cast<double>(c.target.len()) / rate;
  const double srcBars = sec * d.sourceBpm / (60.0 * beats);
  if (!ps.drBarsSet) d.bars = srcBars;
  const float col = column({i18n::N_("Detected"), i18n::N_("Loop tempo"), i18n::N_("New tempo"), i18n::N_("Length"), i18n::N_("Sensitivity"),
                            i18n::N_("Between hits"), i18n::N_("Attack")});
  BeginSection(i18n::N_("TEMPO"), kDlgW);
  char b[200];
  if (!ps.loop.have)
    std::snprintf(b, sizeof b, "%s", i18n::tr("Finding the hits..."));
  else if (lt.bpm > 0)
    std::snprintf(b, sizeof b, i18n::tr("%.2f BPM, %.2f bars, %zu hits"), lt.bpm, lt.bars, lt.hits.size());
  else
    std::snprintf(b, sizeof b, i18n::tr("No tempo found (%zu hits): set the loop tempo"), lt.hits.size());
  textRow(i18n::N_("Detected"), col, b);
  if (sliderD(i18n::N_("Loop tempo"), col, &d.sourceBpm, 40.f, 240.f, "%.2f BPM")) ps.drSrcSet = true;
  sliderD(i18n::N_("New tempo"), col, &d.targetBpm, 40.f, 240.f, "%.2f BPM");
  if (sliderD(i18n::N_("Length"), col, &d.bars, 0.25f, 32.f, "%.2f bars", ImGuiSliderFlags_Logarithmic)) ps.drBarsSet = true;
  const int64_t newLen = drumStretchLength(d, rate);
  std::snprintf(b, sizeof b, i18n::tr("New length %s (x %.3f), every hit on its new place."), tt(c, newLen).c_str(),
                static_cast<double>(newLen) / static_cast<double>(std::max<int64_t>(1, c.target.len())));
  note(b);
  ui::EndSection();
  switch (presets({i18n::N_("Song tempo"), i18n::N_("Half time"), i18n::N_("Double time")})) {
    case 0: d.targetBpm = std::clamp(c.songTempo, 40.0, 240.0), ps.drBarsSet = false; break;
    case 1: d.targetBpm = std::max(40.0, d.sourceBpm / 2), ps.drBarsSet = false; break;
    case 2: d.targetBpm = std::min(240.0, d.sourceBpm * 2), ps.drBarsSet = false; break;
    default: break;
  }
  BeginSection(i18n::N_("HITS"), kDlgW);
  sliderD(i18n::N_("Sensitivity"), col, &d.sensitivity, 0.f, 100.f, "%.0f %%", 0, 100);
  std::vector<int64_t> ticks;
  for (int64_t h : lt.hits) ticks.push_back(c.target.a + h);
  overview(c, {}, ps.loop.have ? ticks : std::vector<int64_t>{});
  const char* tails[] = {i18n::tr("Stretch the sound"), i18n::tr("Slices with fades")};
  combo(i18n::N_("Between hits"), col, &d.tails, tails, 2);
  sliderD(i18n::N_("Attack"), col, &d.attackMs, 5.f, 80.f, "%.0f ms");
  note(i18n::tr("Each hit's attack is kept as it is; the sound between hits is stretched, or each slice plays and fades."));
  ui::EndSection();
  ps.marks.regions.clear();
  ps.marks.ticks = ps.loop.have ? ticks : std::vector<int64_t>{};
  return ps.loop.have && newLen != c.target.len() && c.target.len() >= 2048;
}

bool blurPanel(ToolParams& p) {
  const float col = column({"Amount", "Mix"});
  BeginSection("OPTIONS", kDlgW);
  sliderD("Amount", col, &p.blurMs, 2.f, 500.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
  sliderD("Mix", col, &p.blurMix, 0.f, 1.f, "%.2f");
  ui::EndSection();
  return true;
}

bool reverbPanel(ReverbSettings& rv) {
  const float col = column({"Decay", "Pre-delay", "Damping", "Width", "Wet", "Dry"});
  BeginSection("ROOM", kDlgW);
  sliderD("Decay", col, &rv.decay, 0.1f, 10.f, "%.2f s", ImGuiSliderFlags_Logarithmic);
  sliderD("Pre-delay", col, &rv.predelayMs, 0.f, 200.f, "%.0f ms");
  sliderD("Damping", col, &rv.damping, 0.f, 1.f, "%.2f");
  sliderD("Width", col, &rv.width, 0.f, 1.f, "%.2f");
  ui::EndSection();
  BeginSection("MIX", kDlgW);
  sliderD("Wet", col, &rv.wet, 0.f, 1.f, "%.2f");
  sliderD("Dry", col, &rv.dry, 0.f, 1.f, "%.2f");
  note(i18n::tr("The tail rings into the following audio (and past the end)."));
  ui::EndSection();
  return true;
}

bool eqPanel(std::vector<EqPoint>& eq, PanelState& ps) {
  std::vector<EqPt> pts;
  for (auto& q : eq) pts.push_back({q.hz, q.db});
  auto toS = [](const EqPt& q, ImVec2 p0, ImVec2 p1) {
    float x = p0.x + static_cast<float>(std::log(q.first / 20.0) / std::log(1000.0)) * (p1.x - p0.x);
    float y = (p0.y + p1.y) * 0.5f - static_cast<float>(q.second / 24.0) * (p1.y - p0.y) * 0.5f;
    return ImVec2(x, y);
  };
  auto fromS = [](ImVec2 m, ImVec2 p0, ImVec2 p1) {
    double hz = 20.0 * std::pow(1000.0, (m.x - p0.x) / (p1.x - p0.x));
    double db = -((m.y - (p0.y + p1.y) * 0.5) / ((p1.y - p0.y) * 0.5)) * 24.0;
    return EqPt{hz, std::round(db * 10) / 10};
  };
  BeginSection("CURVE", kDlgWideW);
  pointGraph("eqgraph", pts, ImVec2(ImGui::GetContentRegionAvail().x, 200), toS, fromS, false, ps.drag, [](ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
      float x = std::round(p0.x + static_cast<float>(std::log(hz / 20.0) / std::log(1000.0)) * (p1.x - p0.x)) + 0.5f;
      dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), C(col::WaveGrid));
      char b[12];
      std::snprintf(b, sizeof b, hz >= 1000 ? "%.0fk" : "%.0f", hz >= 1000 ? hz / 1000 : hz);
      gridText(dl, ImVec2(x + 3, p1.y - theme::kFontLabel - 4), b);
    }
    for (int db : {-12, 0, 12}) {
      float y = std::round((p0.y + p1.y) * 0.5f - db / 24.f * (p1.y - p0.y) * 0.5f) + 0.5f;
      dl->AddLine(ImVec2(p0.x + 1, y), ImVec2(p1.x - 1, y), db == 0 ? C(col::WaveGridHi) : C(col::WaveGrid));
      char b[12];
      std::snprintf(b, sizeof b, "%+d dB", db);
      gridText(dl, ImVec2(p0.x + 5, y - theme::kFontLabel - 3), db == 0 ? "0 dB" : b);
    }
  });
  eq.clear();
  for (auto& q : pts) eq.push_back({q.first, q.second});
  note(i18n::tr("Drag points; double-click adds, right-click removes. +-24 dB, 20 Hz - 20 kHz."));
  ui::EndSection();
  switch (presets({"Flat", "Low cut", "Telephone"})) {
    case 0: eq = {{30, 0}, {120, 0}, {500, 0}, {2000, 0}, {8000, 0}, {16000, 0}}; break;
    case 1: eq = {{20, -24}, {60, -24}, {120, 0}, {16000, 0}}; break;
    case 2: eq = {{20, -24}, {300, -24}, {500, 0}, {2500, 3}, {3400, -24}, {20000, -24}}; break;
    default: break;
  }
  return true;
}

bool envelopePanel(std::vector<std::pair<double, double>>& env, PanelState& ps) {
  auto toS = [](const std::pair<double, double>& q, ImVec2 p0, ImVec2 p1) {
    return ImVec2(p0.x + static_cast<float>(q.first) * (p1.x - p0.x), p1.y - static_cast<float>(q.second / 2.0) * (p1.y - p0.y));
  };
  auto fromS = [](ImVec2 m, ImVec2 p0, ImVec2 p1) {
    return std::make_pair(static_cast<double>((m.x - p0.x) / (p1.x - p0.x)), static_cast<double>((p1.y - m.y) / (p1.y - p0.y) * 2.0));
  };
  BeginSection("ENVELOPE", kDlgWideW);
  pointGraph("envgraph", env, ImVec2(ImGui::GetContentRegionAvail().x, 180), toS, fromS, true, ps.drag, [](ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    const float y1 = std::round(p1.y - 0.5f * (p1.y - p0.y)) + 0.5f;
    dl->AddLine(ImVec2(p0.x + 1, y1), ImVec2(p1.x - 1, y1), C(col::WaveGridHi));
    gridText(dl, ImVec2(p0.x + 5, y1 - theme::kFontLabel - 3), "0 dB");
  });
  note(i18n::tr("Gain across the range (0 to +6 dB). Double-click adds a point, right-click removes."));
  ui::EndSection();
  switch (presets({"Reset", "Swell", "Duck"})) {
    case 0: env = {{0, 1}, {1, 1}}; break;
    case 1: env = {{0, 0}, {0.7, 1}, {1, 1}}; break;
    case 2: env = {{0, 1}, {0.1, 0.3}, {0.6, 0.3}, {1, 1}}; break;
    default: break;
  }
  return true;
}

// ------------------------------------------------------------------ preview
Range previewWindow(const PanelContext& c, bool whole) {
  const Range tr = c.target;
  const double rate = c.audio ? c.audio->rate : 44100.0;
  const int64_t maxLen = static_cast<int64_t>(rate * (whole ? 60.0 : 12.0));
  if (tr.len() <= maxLen) return tr;
  int64_t a0 = c.cursor >= tr.a && c.cursor < tr.b ? c.cursor : tr.a;
  a0 = std::min(a0, tr.b - maxLen);
  return {a0, a0 + maxLen};
}

void stopListening(App& a, PreviewState& p) {
  if (p.smp && a.engine.previewingSample(p.smp.get())) a.engine.preview(nullptr);
  p.smp = nullptr;
  p.on = false;
}

int64_t listenPos(const PreviewState& p, double rate) {
  if (!p.smp || p.cycle <= 0) return 0;
  const int64_t t = static_cast<int64_t>((now() - p.t0) * rate);
  return (p.at0 + t) % p.cycle;
}

const Audio& heard(const PreviewState& p) { return p.mode == 1 ? p.orig : p.mode == 2 ? p.removed : p.proc; }

// Loops `src` through the engine's preview from frame `at` (then the whole of it, no gap).
void listenFrom(App& a, PreviewState& p, const Audio& src, int64_t at) {
  const int64_t n = src.frames();
  if (n == 0) return;
  at = std::clamp<int64_t>(at, 0, n - 1);
  Audio part = extract(src, {at, n});
  if (at > 0) {
    part.L.insert(part.L.end(), src.L.begin(), src.L.end());
    part.R.insert(part.R.end(), src.R.begin(), src.R.end());
  }
  p.smp = toSample(part, "");
  a.engine.preview(p.smp, true, at > 0 ? n - at : 0);
  p.t0 = now();
  p.at0 = at;
  p.cycle = n;
}

void reap(PreviewState& p) {
  p.old.erase(std::remove_if(p.old.begin(), p.old.end(), [](std::future<Audio>& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
              p.old.end());
}

void startRender(PreviewState& p, const PanelContext& c, Range win, const std::string& key, const ToolParams& params) {
  if (p.job.valid()) {
    if (p.jobProg) p.jobProg->cancel = true;
    p.old.push_back(std::move(p.job));
  }
  p.jobProg = std::make_shared<Progress>();
  p.jobKey = key;
  ToolParams tp = params;
  for (auto& q : tp.brush) q.f0 -= win.a, q.f1 -= win.a;  // the brush in the part's frames
  const Content part = makeContent(extract(*c.audio, win));
  auto prog = p.jobProg;
  p.job = std::async(std::launch::async, [part, prog, tp]() -> Audio { gl::threads::setName("gl-loupe-repair"); return processContent(part, {0, part.frames()}, tp, prog.get()).a(); });
}

}  // namespace

std::string paramsKey(const ToolParams& p) {
  char b[400];
  switch (p.kind) {
    case ToolKind::Denoise: {
      const auto& d = p.denoise;
      std::snprintf(b, sizeof b, "dn %.2f %.3f %.3f %d %d %zu %.6g", d.reductionDb, d.sensitivity, d.smoothing, d.character, d.adaptive ? 1 : 0, d.profile.mag.size(),
                    d.profile.valid() ? static_cast<double>(d.profile.mag[d.profile.mag.size() / 3]) : 0.0);
      break;
    }
    case ToolKind::Declick: std::snprintf(b, sizeof b, "dk %.3f %.3f", p.declick.sensitivity, p.declick.maxClickMs); break;
    case ToolKind::Declip: std::snprintf(b, sizeof b, "dc %d %.3f %d %.3f", p.declip.autoLevel ? 1 : 0, p.declip.clipDb, p.declip.fitGain ? 1 : 0, p.declip.ceilingDb); break;
    case ToolKind::Deverb:
      std::snprintf(b, sizeof b, "dv %.3f %.3f %d %d %.3f", p.deverb.amount, p.deverb.t60, p.deverb.useEstimate ? 1 : 0, p.deverb.keepEarly ? 1 : 0, p.deverb.estimate.t60);
      break;
    case ToolKind::Gate: {
      const auto& g = p.gate;
      std::snprintf(b, sizeof b, "gt %.2f %.2f %.2f %.1f %.1f %.1f", g.thresholdDb, g.hysteresisDb, g.attackMs, g.holdMs, g.releaseMs, g.floorDb);
      break;
    }
    case ToolKind::Blur: std::snprintf(b, sizeof b, "bl %.2f %.3f", p.blurMs, p.blurMix); break;
    case ToolKind::Reverb: {
      const auto& r = p.reverb;
      std::snprintf(b, sizeof b, "rv %.3f %.1f %.3f %.3f %.3f %.3f", r.decay, r.predelayMs, r.damping, r.width, r.wet, r.dry);
      break;
    }
    case ToolKind::Equalize: {
      std::string k = "eq";
      for (const auto& q : p.eq) k += " " + std::to_string(q.hz) + "/" + std::to_string(q.db);
      return k;
    }
    case ToolKind::Envelope: {
      std::string k = "en";
      for (const auto& q : p.envelope) k += " " + std::to_string(q.first) + "/" + std::to_string(q.second);
      return k;
    }
    case ToolKind::Normalize: std::snprintf(b, sizeof b, "nm %.2f", p.normalizeDb); break;
    case ToolKind::Amplify: std::snprintf(b, sizeof b, "am %.2f", p.amplifyDb); break;
    case ToolKind::DrumStretch: {
      const auto& d = p.drum;
      std::snprintf(b, sizeof b, "dr %.3f %.4f %.3f %d %.1f %d", d.targetBpm, d.bars, d.sensitivity, d.tails, d.attackMs, d.beatsPerBar);
      break;
    }
    case ToolKind::SpectralBrush: {
      std::string k = "sb " + std::to_string(p.brushDb);
      for (const auto& q : p.brush) k += " " + std::to_string(q.f0) + "," + std::to_string(q.f1) + "," + std::to_string(q.hz0) + "," + std::to_string(q.hz1);
      return k;
    }
    default: std::snprintf(b, sizeof b, "k%d", static_cast<int>(p.kind)); break;
  }
  return b;
}

bool drawToolPanel(ToolParams& p, const PanelContext& c) {
  if (!c.audio) return false;
  PanelState& ps = panels()[c.key];
  if (ps.marksFor != p.kind) ps.marks = {}, ps.marksFor = p.kind;
  switch (p.kind) {
    case ToolKind::Denoise: return denoisePanel(p.denoise, c);
    case ToolKind::Declick: return declickPanel(p.declick, ps, c);
    case ToolKind::Declip: return declipPanel(p.declip, ps, c);
    case ToolKind::Deverb: return deverbPanel(p.deverb, ps, c);
    case ToolKind::Gate: return gatePanel(p.gate);
    case ToolKind::DrumStretch: return drumPanel(p.drum, ps, c);
    case ToolKind::Blur: return blurPanel(p);
    case ToolKind::Reverb: return reverbPanel(p.reverb);
    case ToolKind::Equalize: return eqPanel(p.eq, ps);
    case ToolKind::Envelope: return envelopePanel(p.envelope, ps);
    case ToolKind::Normalize: {
      const float col = column({"Peak level"});
      BeginSection("LEVEL", kDlgW);
      sliderD("Peak level", col, &p.normalizeDb, -24.f, 0.f, "%.1f dBFS");
      ui::EndSection();
      return true;
    }
    case ToolKind::Amplify: {
      const float col = column({"Gain"});
      BeginSection("LEVEL", kDlgW);
      sliderD("Gain", col, &p.amplifyDb, -48.f, 24.f, "%+.1f dB");
      ui::EndSection();
      return true;
    }
    case ToolKind::SpectralBrush: {
      char b[96];
      std::snprintf(b, sizeof b, i18n::tr("%zu painted areas"), p.brush.size());
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
      ImGui::TextUnformatted(b);
      ImGui::PopStyleColor();
      const float col = column({"Gain"});
      BeginSection("BRUSH", kDlgW);
      sliderD("Gain", col, &p.brushDb, -60.f, 12.f, "%+.0f dB");
      note(i18n::tr("Attenuate (negative) or boost the painted time / frequency areas. The edges are soft, so nothing clicks."));
      ui::EndSection();
      return !p.brush.empty();
    }
    default: return true;
  }
}

void stopPreview(App& a, uint64_t key) {
  auto it = panels().find(key);
  if (it == panels().end()) return;
  PreviewState& p = it->second.pv;
  stopListening(a, p);
  if (p.job.valid()) {
    if (p.jobProg) p.jobProg->cancel = true;
    p.old.push_back(std::move(p.job));
  }
  p.have = false;
  p.key.clear();
  p.mode = 0;
  reap(p);
}

const PanelMarks* panelMarks(uint64_t key) {
  auto it = panels().find(key);
  return it == panels().end() ? nullptr : &it->second.marks;
}

void drawPreview(App& a, const PanelContext& c, const ToolParams& params, const std::function<void()>& beforeListen) {
  if (!c.audio) return;
  PreviewState& p = panels()[c.key].pv;
  reap(p);
  const bool whole = changesLength(params.kind);
  const bool canRemove = hasRemoved(params.kind);
  if (!canRemove && p.mode == 2) p.mode = 0;
  const double rate = c.audio->rate;
  const Range win = previewWindow(c, whole);
  const std::string key = paramsKey(params) + "|" + targetKey(c) + ":" + std::to_string(win.a);
  if (key != p.seenKey) p.seenKey = key, p.changedAt = now();
  if (p.job.valid() && p.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    Audio out = p.job.get();
    if (p.jobProg && !p.jobProg->cancelled()) {
      const int64_t pos = p.on ? listenPos(p, rate) : 0;
      const int64_t oldCycle = p.cycle;
      p.orig = extract(*c.audio, win);
      p.proc = std::move(out);
      p.removed = p.orig;
      if (p.proc.frames() == p.orig.frames())
        for (int ch = 0; ch < 2; ++ch)
          for (int64_t i = 0; i < p.orig.frames(); ++i) p.removed.ch(ch)[i] = p.orig.ch(ch)[i] - p.proc.ch(ch)[i];
      p.key = p.jobKey;
      p.have = true;
      p.stripW = 0;
      if (p.on) {
        const Audio& src = heard(p);
        listenFrom(a, p, src, oldCycle > 0 ? pos * src.frames() / oldCycle : 0);
      }
    }
  }
  const bool resting = !ImGui::IsMouseDown(ImGuiMouseButton_Left) && now() - p.changedAt > 0.25;
  if (p.on && key != p.key && key != p.jobKey && resting) startRender(p, c, win, key, params);
  if (p.on && p.smp && !a.engine.previewingSample(p.smp.get())) p.on = false, p.smp = nullptr;  // stopped elsewhere

  BeginSection(i18n::N_("PREVIEW"), kDlgW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x, h = 46;
  const ImVec2 q0 = ImGui::GetCursorScreenPos(), q1(q0.x + w, q0.y + h);
  ImGui::Dummy(ImVec2(w, h));
  skin::insetField(dl, q0, q1, C(col::WaveBg), theme::kRadius);
  const bool fresh = p.have && p.key == key;
  if (p.have) {
    const int sw = static_cast<int>(w) - 2;
    if (p.stripW != sw) p.so = columns(p.orig, sw), p.sp = columns(p.mode == 2 ? p.removed : p.proc, sw), p.stripW = sw;
    float pk = 0;
    for (size_t x = 0; x < p.so.lo.size(); ++x) pk = std::max({pk, -p.so.lo[x], p.so.hi[x]});
    const float scale = pk > 1e-6f ? 1.0f / pk : 1.0f;
    const ImVec2 i0(q0.x + 1, q0.y + 1), i1(q1.x - 1, q1.y - 1);
    drawStrip(dl, p.so, i0, i1, C(col::WaveDim, 0.9f), scale);
    drawStrip(dl, p.sp, i0, i1, p.mode == 2 ? C(col::Brush, fresh ? 1.0f : 0.5f) : C(col::Wave, fresh ? 1.0f : 0.5f), scale);
    if (p.on && p.smp && p.cycle > 0) {
      const float x = std::round(i0.x + static_cast<float>(listenPos(p, rate)) / static_cast<float>(p.cycle) * (i1.x - i0.x)) + 0.5f;
      dl->AddLine(ImVec2(x, i0.y), ImVec2(x, i1.y), C(col::Playhead), 2.0f);
    }
  } else {
    ImFont* f = ImGui::GetFont();
    const char* t = i18n::tr("Press Preview to listen before you apply");
    const ImVec2 ts = f->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, t);
    dl->AddText(ImVec2(std::round((q0.x + q1.x - ts.x) * 0.5f), std::round((q0.y + q1.y - ts.y) * 0.5f)), C(col::TextDim), t);
  }
  if (p.job.valid() && p.jobProg) {
    const float fr = std::clamp(p.jobProg->done.load(), 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(q0.x + 2, q1.y - 4), ImVec2(q0.x + 2 + (w - 4) * fr, q1.y - 2), C(col::Lit), 1.0f);
  }
  const float bw = canRemove ? std::floor((w - kBtnW - 3 * theme::kS2) / 3) : std::floor((w - kBtnW - 2 * theme::kS2) / 2);
  if (ui::TextButton(p.on ? i18n::N_("Stop") : i18n::N_("Preview"), p.on, kBtnW, i18n::N_("Listen to the result (it loops) before you apply it"))) {
    if (p.on) {
      stopListening(a, p);
    } else {
      if (beforeListen) beforeListen();
      p.on = true;
      if (fresh) listenFrom(a, p, heard(p), 0);
      else if (key != p.jobKey || !p.job.valid()) startRender(p, c, win, key, params);
    }
  }
  auto pick = [&](int m) {
    if (p.mode == m) return;
    const int64_t pos = p.on && p.smp ? listenPos(p, rate) : 0;
    const int64_t from = p.cycle;
    p.mode = m;
    p.stripW = 0;
    if (p.on && p.smp && fresh) {
      const Audio& src = heard(p);
      listenFrom(a, p, src, from > 0 ? pos * src.frames() / from : 0);
    }
  };
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::N_("A: Processed"), p.mode == 0, bw, i18n::N_("Hear the result"))) pick(0);
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::N_("B: Original"), p.mode == 1, bw, i18n::N_("Hear the audio as it is now, at the same place"))) pick(1);
  if (canRemove) {
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton(i18n::N_("C: Removed"), p.mode == 2, bw, i18n::N_("Hear only what the tool takes out (it should hold none of the sound you want)"))) pick(2);
  }
  char b[160];
  if (p.job.valid() && p.jobProg)
    std::snprintf(b, sizeof b, i18n::tr("Rendering the preview... %.0f%%"), static_cast<double>(p.jobProg->done.load()) * 100.0);
  else if (p.have)
    std::snprintf(b, sizeof b, i18n::tr("Preview of %s from %s%s"), tt(c, win.len()).c_str(), tt(c, win.a).c_str(), fresh ? "" : i18n::tr(" (settings changed)"));
  else
    std::snprintf(b, sizeof b, i18n::tr("Plays %s from %s"), tt(c, win.len()).c_str(), tt(c, win.a).c_str());
  statusNote(b);  // (the percent, then the result: one height)
  ui::EndSection();
}

// ------------------------------------------------------------------ Loupe
namespace {
std::map<uint64_t, ToolParams>& loupeParams() {
  static std::map<uint64_t, ToolParams> m;
  return m;
}
std::map<uint64_t, std::shared_ptr<Progress>>& applying() {
  static std::map<uint64_t, std::shared_ptr<Progress>> m;
  return m;
}

PanelContext contextFor(App& a, Session& s, Range tr) {
  PanelContext c;
  c.key = s.key;
  c.audio = s.cur().audio;
  c.target = tr;
  c.cursor = s.cursor;
  c.songTempo = a.doc.song().tempo;
  c.beatsPerBar = std::max(1, a.doc.song().timeSigNum);
  App* ap = &a;
  Session* sp = &s;
  c.timeText = [ap, sp](int64_t f) { return timeText(*ap, *sp, f); };
  c.takeProfile = [sp](NoiseProfile& out) {
    NoiseProfile np = acquireNoise(sp->cur().a(), sp->sel);
    if (!np.valid()) {
      say(*sp, "Select at least 50 ms of noise first");
      return false;
    }
    sp->noise = np;
    out = np;
    return true;
  };
  c.peaks = s.peaks.built() ? &s.peaks : nullptr;
  return c;
}

// Cancel / Accept; then the edit's progress with Stop until it is done (the dialog closes).
void footerFor(App& a, Session& s, const ToolParams& p, Range tr, EditMap map, bool enabled) {
  auto& ap = applying()[s.key];
  if (ap) {
    if (!s.job.active() || s.job.progress != ap) {
      ap.reset();
      ImGui::CloseCurrentPopup();
      return;
    }
    const float fr = std::clamp(ap->done.load(), 0.0f, 1.0f);
    ui::DialogFooter(kDlgW);
    char b[96];
    std::snprintf(b, sizeof b, i18n::tr("Applying... %.0f%%"), static_cast<double>(fr) * 100.0);
    ImGui::ProgressBar(fr, ImVec2(kDlgW - kBtnW - theme::kS2, 0), b);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton(i18n::N_("Stop"), false, kBtnW, i18n::N_("Stop the edit: the audio stays as it was"))) ap->cancel = true;
    return;
  }
  ui::DialogFooter(ui::ButtonRowWidth(2, kBtnW));
  if (ui::TextButton("Cancel", false, kBtnW)) ImGui::CloseCurrentPopup();
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(!enabled || s.job.active());
  const bool ok = ui::PrimaryButton("Accept", kBtnW);
  ImGui::EndDisabled();
  if (!ok) return;
  stopPreview(a, s.key);
  const ToolParams tp = p;
  runEditP(a, s, toolName(p.kind), [tp, tr](const Content& c, Progress& pr) { return processContent(c, tr, tp, &pr); }, map);
  if (s.job.active() && s.job.progress) ap = s.job.progress;
  else ImGui::CloseCurrentPopup();
  if (p.kind == ToolKind::SpectralBrush && s.job.active()) s.brush.clear();
}
}  // namespace

ToolKind panelKind(Tool t) {
  switch (t) {
    case Tool::Denoise: return ToolKind::Denoise;
    case Tool::Declick: return ToolKind::Declick;
    case Tool::Declip: return ToolKind::Declip;
    case Tool::Deverb: return ToolKind::Deverb;
    case Tool::Gate: return ToolKind::Gate;
    case Tool::Blur: return ToolKind::Blur;
    case Tool::Reverb: return ToolKind::Reverb;
    case Tool::Equalize: return ToolKind::Equalize;
    case Tool::Normalize: return ToolKind::Normalize;
    case Tool::Amplify: return ToolKind::Amplify;
    case Tool::Envelope: return ToolKind::Envelope;
    case Tool::DrumStretch: return ToolKind::DrumStretch;
    case Tool::Spectral: return ToolKind::SpectralBrush;
    default: return ToolKind::Count;
  }
}

bool drawRepairDialog(App& a, Session& s, Range tr) {
  const ToolKind k = panelKind(s.tool);
  if (k == ToolKind::Count) return false;
  ToolParams& p = loupeParams()[s.key];
  p.kind = k;
  if (k == ToolKind::Denoise && s.noise.valid() && p.denoise.profile.mag != s.noise.mag) p.denoise.profile = s.noise, p.denoise.adaptive = false;
  if (k == ToolKind::SpectralBrush) {
    // The brush works where it was painted: the target is the painted span (with a margin).
    p.brush = s.brush;
    Range span{s.frames(), 0};
    for (const auto& q : s.brush) span.a = std::min(span.a, q.f0), span.b = std::max(span.b, q.f1);
    tr = clampRange(s.cur(), {span.a - static_cast<int64_t>(s.rate() * 0.25), span.b + static_cast<int64_t>(s.rate() * 0.25)});
  } else {
    scopeLine(a, s, tr);
  }
  const PanelContext c = contextFor(a, s, tr);
  const bool ready = drawToolPanel(p, c);
  if (!tr.empty()) {
    Session* sp = &s;
    App* ap = &a;
    drawPreview(a, c, p, [ap, sp]() { stopPlay(*ap, *sp); });
  }
  footerFor(a, s, p, tr, changesLength(k) ? EditMap::replace(tr) : EditMap{}, ready && !tr.empty());
  return true;
}

void repairToolClosed(App& a, Session& s) {
  stopPreview(a, s.key);
  applying().erase(s.key);
}

void drawRepairMarks(const Session& s, ImDrawList* dl, ImVec2 p0, ImVec2 p1, double viewStart, double viewLen) {
  const ToolKind k = panelKind(s.tool);
  if (k != ToolKind::Declip && k != ToolKind::Declick && k != ToolKind::DrumStretch) return;
  const PanelMarks* m = panelMarks(s.key);
  if (!m || viewLen <= 0) return;
  auto xOf = [&](double f) { return p0.x + static_cast<float>((f - viewStart) / viewLen) * (p1.x - p0.x); };
  for (const Range& r : m->regions) {
    if (r.b < viewStart || r.a > viewStart + viewLen) continue;
    float x0 = std::floor(xOf(static_cast<double>(r.a))), x1 = std::ceil(xOf(static_cast<double>(r.b)));
    if (x1 - x0 < 2) x1 = x0 + 2;
    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p1.y), C(col::Rec, 0.28f));
    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p0.y + 4), C(col::Rec, 0.95f));
  }
  for (int64_t h : m->ticks) {
    const double f = static_cast<double>(h);
    if (f < viewStart || f > viewStart + viewLen) continue;
    const float x = std::round(xOf(f)) + 0.5f;
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), C(col::Marker, 0.55f));
    dl->AddTriangleFilled(ImVec2(x - 4, p0.y), ImVec2(x + 4, p0.y), ImVec2(x, p0.y + 6), C(col::Marker));
  }
}

}  // namespace gl::packs::edison
