// Gridline — playlist chop / stutter / region editing UI (PL-13). See ChopUi.h.
#include "ChopUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include "../core/Grouping.h"
#include "../core/I18n.h"
#include "../core/Timeline.h"
#include "../engine/Chop.h"
#include "../core/PluginInfo.h"
#include "../packs/edison/edison.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "Keys.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::chopui {

using theme::pal;
namespace ed = packs::edison;

namespace {

enum Mode { kSlice = 0, kStutter = 1 };
enum SliceMode { kGrid = 0, kEqual = 1, kTransients = 2 };

// Grid choices of the slice mode (in beats; the snap grid is the last one).
struct GridChoice {
  const char* name;
  int num, den;  // beats = num / den (0 / 0 = the playlist snap)
};
const GridChoice kGrids[] = {{i18n::N_("Bar"), -1, 1},           {i18n::N_("2 beats"), 2, 1},  {i18n::N_("Beat"), 1, 1},
                             {i18n::N_("Half beat"), 1, 2},      {i18n::N_("Quarter beat"), 1, 4},
                             {i18n::N_("Eighth beat"), 1, 8},   {i18n::N_("Snap grid"), 0, 0}};
constexpr int kGridCount = static_cast<int>(sizeof(kGrids) / sizeof(kGrids[0]));
const int kRates[] = {4, 8, 16, 32, 64};
constexpr int kRateCount = 5;

struct State {
  bool open = false;
  std::set<ObjectId> clips;  // target (ids before the preview)
  Tick a = 0, b = 0;         // stutter region
  int mode = kStutter;
  int sliceMode = kGrid;
  int grid = 2;  // kGrids index
  int parts = 4;
  double sensitivity = 0.5;
  chop::StutterParams p = chop::presetParams(chop::Preset::BeatRepeat);
  int preset = 0;  // -1 = custom
  bool preview = true;
  // Live preview: the result is one undo step, undone and redone on every change.
  bool previewActive = false;
  uint64_t previewSerial = 0;
  std::string appliedSig;
  std::vector<ObjectId> result;  // ids the last application produced
  bool focus = false;
};
State& st() {
  static State s;
  return s;
}

Tick gridTicks(const App& a, int g) {
  const Song& s = a.doc.song();
  const GridChoice& c = kGrids[std::clamp(g, 0, kGridCount - 1)];
  if (c.num < 0) return s.barTicks();
  if (c.num == 0) return std::max<Tick>(1, snapTicks(s, a.playlist.snap == Snap::None ? Snap::Line : a.playlist.snap));
  return std::max<Tick>(1, s.beatTicks() * c.num / c.den);
}

std::string signature(const State& q, const App& a) {
  char buf[512];
  const auto& p = q.p;
  std::snprintf(buf, sizeof buf, "%d|%d|%lld|%d|%.4f|%d,%d,%d,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%d,%.4f", q.mode, q.sliceMode,
                static_cast<long long>(gridTicks(a, q.grid)), q.parts, q.sensitivity, p.rate, p.triplet ? 1 : 0, p.endRate, p.decay,
                p.pitchStart, p.pitchEnd, p.reverse, p.gate, p.speedStart, p.speedEnd, p.follow ? 1 : 0, p.fadeMs);
  return buf;
}

// Tracks a region action works on: the selected clips' tracks, else all.
std::set<int> regionTracks(const App& a) {
  std::set<int> t;
  for (ObjectId id : a.playlist.selected)
    if (const Clip* c = a.doc.song().clip(id)) t.insert(c->track);
  return t;
}

bool hasTimeSel(const App& a) { return a.playlist.selEnd > a.playlist.selStart; }

bool previewLive(const App& a) {
  const State& q = st();
  return q.previewActive && a.doc.history().stateSerial() == q.previewSerial;
}

// Applies the window's operation as one undo step.
void applyOp(App& a) {
  State& q = st();
  std::vector<ObjectId> out;
  const auto clips = q.clips;
  if (q.mode == kSlice) {
    const Tick g = gridTicks(a, q.grid);
    const int parts = q.parts;
    const double sens = q.sensitivity;
    const int sm = q.sliceMode;
    a.doc.editSong("chop clips", [&](Song& s) {
      out = sm == kGrid ? chop::sliceAtGrid(s, clips, g) : sm == kEqual ? chop::sliceEqual(s, clips, parts) : chop::sliceAtTransients(s, clips, sens);
    });
  } else {
    const auto p = q.p;
    const Tick ra = q.a, rb = q.b;
    a.doc.editSong("stutter clips", [&](Song& s) { out = chop::stutter(s, clips, ra, rb, p); });
  }
  q.result = out;
  a.playlist.selected = std::set<ObjectId>(out.begin(), out.end());
}

void undoPreview(App& a) {
  State& q = st();
  if (previewLive(a)) {
    a.doc.undo();
    a.playlist.selected = q.clips;
  }
  q.previewActive = false;
}

void refreshPreview(App& a) {
  State& q = st();
  const std::string sig = signature(q, a);
  if (sig == q.appliedSig) return;  // shown already (or nothing to change)
  undoPreview(a);
  const uint64_t before = a.doc.history().stateSerial();
  applyOp(a);
  q.previewSerial = a.doc.history().stateSerial();
  q.previewActive = q.previewSerial != before;  // an edit that changed nothing records no step
  q.appliedSig = sig;
}

void close(App& a, bool keep) {
  State& q = st();
  if (!keep) undoPreview(a);
  else if (!previewLive(a)) applyOp(a);
  q.previewActive = false;
  q.open = false;
}

// Plan graph: one bar per slice (height = level, lit = sounding part,
// a back arrow = reversed), the pitch ramp as a line.
void drawPlan(App& a, float w, float h) {
  const State& q = st();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
  ImGui::InvisibleButton("##plan", ImVec2(w, h));
  dl->AddRectFilled(p0, p1, P.well, 3.0f);
  dl->AddRect(p0, p1, P.line, 3.0f);
  const Song& s = a.doc.song();
  const Tick len = q.mode == kStutter ? q.b - q.a : 0;
  // Beat grid.
  const Tick span = q.mode == kStutter ? len : 0;
  if (q.mode == kStutter && span > 0) {
    const Tick beat = std::max<Tick>(1, s.beatTicks());
    for (Tick t = beat; t < span; t += beat) {
      const float x = p0.x + w * static_cast<float>(t) / static_cast<float>(span);
      dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), t % s.barTicks() == 0 ? P.gridBar : P.gridBeat);
    }
    const ImU32 lit = theme::rgb(theme::role::kSlice), hot = P.hot;
    const auto steps = chop::plan(q.p, len, s.ppq);
    const float pad = 3.0f, bh = h - 2 * pad;
    for (const auto& stp : steps) {
      const float x0 = p0.x + w * static_cast<float>(stp.at) / static_cast<float>(len);
      const float x1 = p0.x + w * static_cast<float>(stp.at + stp.slot) / static_cast<float>(len);
      const float xp = p0.x + w * static_cast<float>(stp.at + stp.play) / static_cast<float>(len);
      const float lv = static_cast<float>(std::clamp(stp.gain, 0.0, 1.0));
      const float y0 = p1.y - pad - bh * std::max(0.04f, lv);
      dl->AddRectFilled(ImVec2(x0 + 0.5f, y0), ImVec2(std::max(x0 + 1.5f, xp - 0.5f), p1.y - pad), theme::withAlpha(lit, 0.35f + 0.5f * lv));
      dl->AddLine(ImVec2(x0 + 0.5f, p0.y + pad), ImVec2(x0 + 0.5f, p1.y - pad), theme::withAlpha(lit, 0.9f));
      if (stp.reverse && x1 - x0 > 7) {
        const float cy = y0 + 5, cx = 0.5f * (x0 + xp);
        dl->AddTriangleFilled(ImVec2(cx - 3, cy), ImVec2(cx + 2, cy - 3), ImVec2(cx + 2, cy + 3), hot);
      }
    }
    // Pitch ramp (±24 semitones over the height), speed ramp dashed.
    if (q.p.pitchStart != 0 || q.p.pitchEnd != 0) {
      auto py = [&](double st2) { return p0.y + h * 0.5f - static_cast<float>(std::clamp(st2, -24.0, 24.0) / 24.0) * (h * 0.5f - pad); };
      dl->AddLine(ImVec2(p0.x, py(q.p.pitchStart)), ImVec2(p1.x, py(q.p.pitchEnd)), hot, 1.5f);
    }
  } else {
    // Slice mode: the cuts of the current preview over the target's time span.
    Tick lo = 0, hi = 0;
    bool first = true;
    const auto& ids = q.result.empty() ? std::vector<ObjectId>(q.clips.begin(), q.clips.end()) : q.result;
    for (ObjectId id : ids)
      if (const Clip* c = s.clip(id)) {
        const Tick e = c->start + s.clipLength(*c);
        lo = first ? c->start : std::min(lo, c->start);
        hi = first ? e : std::max(hi, e);
        first = false;
      }
    if (!first && hi > lo) {
      const ImU32 lit = theme::rgb(theme::role::kSlice);
      int lanes = 0;
      std::map<int, int> laneOf;
      for (ObjectId id : ids)
        if (const Clip* c = s.clip(id); c && !laneOf.count(c->track)) laneOf[c->track] = lanes++;
      const float lh = (h - 6) / static_cast<float>(std::max(1, lanes));
      for (ObjectId id : ids)
        if (const Clip* c = s.clip(id)) {
          const float x0 = p0.x + w * static_cast<float>(c->start - lo) / static_cast<float>(hi - lo);
          const float x1 = p0.x + w * static_cast<float>(c->start + s.clipLength(*c) - lo) / static_cast<float>(hi - lo);
          const float y0 = p0.y + 3 + lh * static_cast<float>(laneOf[c->track]);
          dl->AddRectFilled(ImVec2(x0 + 1, y0 + 1), ImVec2(std::max(x0 + 2, x1 - 1), y0 + lh - 1), theme::withAlpha(lit, 0.45f), 2.0f);
          dl->AddLine(ImVec2(x0 + 0.5f, y0), ImVec2(x0 + 0.5f, y0 + lh), lit, 1.5f);
        }
    }
  }
}

// One labelled knob in a row.
bool knob(const char* id, double* v, const ParamDesc& d, const char* label, bool bipolar, bool last = false) {
  ImGui::BeginGroup();
  bool rel = false;
  ui::Knob(id, v, d, 15.0f, label, bipolar, &rel);
  ImGui::EndGroup();
  if (!last) ImGui::SameLine(0, theme::kS3);
  return rel;
}

ParamDesc lin(const char* name, const char* unit, double lo, double hi, double def) {
  ParamDesc d;
  d.name = name, d.unit = unit, d.min = lo, d.max = hi, d.def = def;
  return d;
}
ParamDesc choice(const char* name, std::vector<std::string> labels, double def) {
  ParamDesc d;
  d.name = name, d.kind = ParamKind::Choice, d.min = 0, d.max = static_cast<double>(labels.size() - 1), d.def = def;
  d.choices = std::move(labels);
  return d;
}

void stutterControls() {
  State& q = st();
  auto& p = q.p;
  // Presets.
  ui::Label(i18n::tr("PRESETS"));
  for (int i = 0; i < chop::kPresetCount; ++i) {
    const auto pr = static_cast<chop::Preset>(i);
    if (i) ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton(i18n::tr(chop::presetName(pr)), q.preset == i, 0, chop::presetHint(pr))) {
      q.preset = i;
      p = chop::presetParams(pr);
    }
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const chop::StutterParams before = p;
  ui::BeginSection(i18n::tr("Slices"));
  {
    double rate = 0, endRate = 0;
    for (int i = 0; i < kRateCount; ++i) {
      if (kRates[i] == p.rate) rate = i;
      if (kRates[i] == p.endRate) endRate = i + 1;
    }
    const ParamDesc dRate = choice(i18n::tr("Rate"), {"1/4", "1/8", "1/16", "1/32", "1/64"}, 2);
    const ParamDesc dEnd = choice(i18n::tr("End rate"), {i18n::tr("Same"), "1/4", "1/8", "1/16", "1/32", "1/64"}, 0);
    knob("rate", &rate, dRate, i18n::tr("Rate"), false);
    ui::Hint(i18n::tr("Slice length: a note value of the bar (1/16 = a sixteenth)"));
    knob("endrate", &endRate, dEnd, i18n::tr("End rate"), false);
    ui::Hint(i18n::tr("The rate the slices reach at the region end (rolls and builds); Same keeps it"));
    p.rate = kRates[std::clamp(static_cast<int>(std::lround(rate)), 0, kRateCount - 1)];
    const int er = static_cast<int>(std::lround(endRate));
    p.endRate = er <= 0 ? 0 : kRates[std::clamp(er - 1, 0, kRateCount - 1)];
    double gate = p.gate * 100.0, decay = p.decay * 100.0, fade = p.fadeMs;
    knob("gate", &gate, lin(i18n::tr("Gate"), "%", 5, 100, 100), i18n::tr("Gate"), false);
    ui::Hint(i18n::tr("Share of each slice that sounds"));
    knob("decay", &decay, lin(i18n::tr("Decay"), "%", -100, 100, 0), i18n::tr("Decay"), true);
    ui::Hint(i18n::tr("Each repeat quieter by this share; below zero the repeats build up instead"));
    knob("fade", &fade, lin(i18n::tr("Fades"), "ms", 0, 50, 2), i18n::tr("Fades"), false, true);
    ui::Hint(i18n::tr("Fade in / out of every audio slice (0 = edge declick only)"));
    p.gate = gate / 100.0, p.decay = decay / 100.0, p.fadeMs = fade;
    bool trip = p.triplet;
    if (ui::TextButton(i18n::tr("Triplets"), trip, 0, i18n::tr("Slices two thirds as long"))) p.triplet = !p.triplet;
    ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton(i18n::tr("Play on"), p.follow, 0, i18n::tr("Slices play on through the audio instead of repeating the first one")))
      p.follow = !p.follow;
    ImGui::SameLine(0, theme::kS3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr("Reverse"));
    ImGui::SameLine(0, theme::kS1);
    const char* rv[] = {i18n::tr("Off"), i18n::tr("All"), i18n::tr("Every other")};
    ui::Segmented("##rev", rv, 3, &p.reverse, 0);
  }
  ui::EndSection();
  ui::BeginSection(i18n::tr("Pitch and speed"));
  {
    double ps = p.pitchStart, pe = p.pitchEnd, ss = p.speedStart * 100.0, se = p.speedEnd * 100.0;
    knob("pst", &ps, lin(i18n::tr("Pitch from"), "st", -24, 24, 0), i18n::tr("Pitch from"), true);
    ui::Hint(i18n::tr("Pitch of the first slice, semitones"));
    knob("pen", &pe, lin(i18n::tr("Pitch to"), "st", -24, 24, 0), i18n::tr("Pitch to"), true);
    ui::Hint(i18n::tr("Pitch of the last slice, semitones (ramped in between)"));
    knob("sst", &ss, lin(i18n::tr("Speed from"), "%", 10, 200, 100), i18n::tr("Speed from"), false);
    ui::Hint(i18n::tr("Playback speed at the region start"));
    knob("sen", &se, lin(i18n::tr("Speed to"), "%", 10, 200, 100), i18n::tr("Speed to"), false, true);
    ui::Hint(i18n::tr("Playback speed at the region end (tape stop: down to 10 %)"));
    p.pitchStart = std::round(ps * 10.0) / 10.0, p.pitchEnd = std::round(pe * 10.0) / 10.0;
    p.speedStart = std::round(ss) / 100.0, p.speedEnd = std::round(se) / 100.0;
  }
  ui::EndSection();
  if (!(p == before)) q.preset = -1;
  for (int i = 0; i < chop::kPresetCount && q.preset < 0; ++i)
    if (p == chop::presetParams(static_cast<chop::Preset>(i))) q.preset = i;
}

void sliceControls() {
  State& q = st();
  const char* modes[] = {i18n::tr("At the grid"), i18n::tr("Equal parts"), i18n::tr("At transients")};
  const char* hints[] = {i18n::tr("Cuts at every grid line"), i18n::tr("Cuts each clip into pieces of equal length"),
                         i18n::tr("Audio: cuts at the attacks; patterns: at the note starts")};
  ui::Segmented("##slmode", modes, 3, &q.sliceMode, 0, 0, hints);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::BeginSection(i18n::tr("Slices"));
  if (q.sliceMode == kGrid) {
    for (int i = 0; i < kGridCount; ++i) {
      if (i) ImGui::SameLine(0, theme::kS1);
      if (ui::TextButton(i18n::tr(kGrids[i].name), q.grid == i)) q.grid = i;
    }
  } else if (q.sliceMode == kEqual) {
    double parts = q.parts;
    knob("parts", &parts, lin(i18n::tr("Parts"), "", 2, 32, 4), i18n::tr("Parts"), false, true);
    q.parts = std::clamp(static_cast<int>(std::lround(parts)), 2, 32);
    ImGui::SameLine(0, theme::kS3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(pal().textDim), i18n::tr("%d pieces per clip"), q.parts);
  } else {
    double sens = q.sensitivity * 100.0;
    knob("sens", &sens, lin(i18n::tr("Sensitivity"), "%", 0, 100, 50), i18n::tr("Sensitivity"), false, true);
    ui::Hint(i18n::tr("Higher finds softer attacks"));
    q.sensitivity = std::round(sens) / 100.0;
    ImGui::SameLine(0, theme::kS3);
    const char* lv[] = {i18n::tr("Gentle"), i18n::tr("Medium"), i18n::tr("Sharp")};
    const double vals[] = {0.25, 0.5, 0.8};
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine(0, theme::kS1);
      if (ui::TextButton(lv[i], std::fabs(q.sensitivity - vals[i]) < 1e-6)) q.sensitivity = vals[i];
    }
  }
  ui::EndSection();
}

std::set<ObjectId> targetOf(const App& a, ObjectId clip, Tick& ra, Tick& rb) {
  const Song& s = a.doc.song();
  std::set<ObjectId> out;
  if (hasTimeSel(a)) {
    ra = a.playlist.selStart, rb = a.playlist.selEnd;
    for (ObjectId id : a.playlist.selected)
      if (const Clip* c = s.clip(id); c && c->start < rb && c->start + s.clipLength(*c) > ra) out.insert(id);
    if (out.empty()) out = chop::clipsInRange(s, ra, rb);
  } else {
    out = a.playlist.selected;
    if (clip != kNoId && !out.count(clip)) out = ops::expandClipGroups(s, {clip});
    bool first = true;
    for (ObjectId id : out)
      if (const Clip* c = s.clip(id)) {
        const Tick e = c->start + s.clipLength(*c);
        ra = first ? c->start : std::min(ra, c->start);
        rb = first ? e : std::max(rb, e);
        first = false;
      }
  }
  for (auto it = out.begin(); it != out.end();) {
    const Clip* c = s.clip(*it);
    it = c && c->kind != ClipKind::Automation ? std::next(it) : out.erase(it);
  }
  return out;
}

}  // namespace

bool isOpen() { return st().open; }

void open(App& a, ObjectId clip, int mode, int preset) {
  State& q = st();
  if (q.open) close(a, previewLive(a));  // a new target: keep what the window showed
  Tick ra = 0, rb = 0;
  q.clips = targetOf(a, clip, ra, rb);
  if (q.clips.empty() || rb <= ra) {
    a.notify(i18n::tr("Select clips or a time selection to chop"), true);
    return;
  }
  q.a = ra, q.b = rb;
  if (mode >= 0) q.mode = mode;
  if (preset >= 0 && preset < chop::kPresetCount) q.preset = preset, q.p = chop::presetParams(static_cast<chop::Preset>(preset));
  q.open = true;
  q.focus = true;
  q.previewActive = false;
  q.appliedSig.clear();
  q.result.clear();
}

void draw(App& a) {
  State& q = st();
  if (!q.open) return;
  const auto& P = pal();
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
  {  // first open: lower right of the work area, clear of the playlist top rows
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.62f, vp->WorkPos.y + vp->WorkSize.y * 0.97f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 1.0f));
  }
  if (q.focus) ImGui::SetNextWindowFocus(), q.focus = false;
  bool open = true;
  const std::string title = std::string(i18n::tr("Chop and stutter")) + "###chopwin";
  if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
    ImGui::End();
    if (!open) close(a, false);
    return;
  }
  ui::KeepWindowInViewport();
  if (q.previewActive && !previewLive(a)) {  // edited elsewhere meanwhile: the preview stays as it is
    q.previewActive = false;
    q.open = false;
    ImGui::End();
    return;
  }
  const Song& s = a.doc.song();
  {  // target line
    const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks());
    auto pos = [&](Tick t) {
      char b2[32];
      std::snprintf(b2, sizeof b2, "%lld.%lld", static_cast<long long>(t / bar + 1), static_cast<long long>(t % bar / beat + 1));
      return std::string(b2);
    };
    char line[160];
    std::snprintf(line, sizeof line, i18n::tr("Clips: %d \xC2\xB7 bars %s \xE2\x80\x93 %s"), static_cast<int>(q.clips.size()), pos(q.a).c_str(),
                  pos(q.b).c_str());
    ImGui::PushFont(a.fontMono, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", line);
    ImGui::PopFont();
  }
  const char* modes[] = {i18n::tr("Slice"), i18n::tr("Stutter")};
  const char* mh[] = {i18n::tr("Cut the clips into pieces"), i18n::tr("Repeat slices of the region with rate, decay, pitch and gate")};
  ui::Segmented("##chopmode", modes, 2, &q.mode, 96, 0, mh);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (q.mode == kSlice) sliceControls();
  else stutterControls();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  drawPlan(a, ImGui::GetContentRegionAvail().x, 64.0f);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  // Preview / play / apply / cancel.
  if (ui::TextButton(i18n::tr("Preview"), q.preview, 0, i18n::tr("Show and play the result in the playlist while you edit")))
    q.preview = !q.preview;
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton(i18n::tr("Play region"), false, 0, i18n::tr("Loops the region from its start"))) {
    if (q.mode == kSlice && !previewLive(a)) refreshPreview(a);
    Tick lo = q.a, hi = q.b;
    a.playlist.selStart = lo, a.playlist.selEnd = hi;
    a.engine.setMode(PlayMode::Song);
    a.engine.setLoopRange(lo, hi);
    a.engine.setPosition(lo);
    a.engine.play();
  }
  const float bw = 84.0f;
  ImGui::SameLine(ImGui::GetContentRegionMax().x - 2 * bw - theme::kS1);
  bool applied = ui::TextButton(i18n::tr("Apply"), true, bw, i18n::tr("Keep the result (one undo step)"));
  ImGui::SameLine(0, theme::kS1);
  bool cancel = ui::TextButton(i18n::tr("Cancel"), false, bw);
  // The live preview follows the controls once no control is being dragged.
  if (q.preview && !ImGui::IsAnyItemActive() && !applied && !cancel) refreshPreview(a);
  if (!q.preview && (q.previewActive || !q.appliedSig.empty())) undoPreview(a), q.appliedSig.clear();
  ImGui::End();
  if (applied) close(a, true);
  else if (cancel || !open) close(a, false);
}

// ----------------------------------------------------------------- menus ----

void sliceSelectedAtGrid(App& a, Tick grid) {
  std::set<ObjectId> sel = a.playlist.selected;
  if (sel.empty() || grid <= 0) return;
  std::vector<ObjectId> out;
  a.doc.editSong("chop clips", [&](Song& s) { out = chop::sliceAtGrid(s, sel, grid); });
  a.playlist.selected = std::set<ObjectId>(out.begin(), out.end());
}

void clipMenu(App& a, ObjectId clip) {
  if (!a.doc.song().clip(clip) || !ImGui::BeginMenu(i18n::tr("Chop"))) return;
  clipMenuItems(a, clip);
  ImGui::EndMenu();
}

void clipMenuItems(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c) return;
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(clip)) sel = ops::expandClipGroups(s, {clip});
  auto run = [&](const char* label, const std::function<std::vector<ObjectId>(Song&)>& fn) {
    std::vector<ObjectId> out;
    a.doc.editSong(label, [&](Song& x) { out = fn(x); });
    a.playlist.selected = std::set<ObjectId>(out.begin(), out.end());
    char msg[96];
    std::snprintf(msg, sizeof msg, i18n::tr("Chopped into %d pieces"), static_cast<int>(out.size()));
    a.notify(msg);
  };
  ImGui::SeparatorText(i18n::tr("At the grid"));
  for (int i = 0; i + 1 < kGridCount; ++i) {
    const Tick g = gridTicks(a, i);
    if (ImGui::MenuItem(i18n::tr(kGrids[i].name), nullptr, false, g < s.clipLength(*c) || sel.size() > 1))
      run("chop clips", [&](Song& x) { return chop::sliceAtGrid(x, sel, g); });
  }
  if (ImGui::MenuItem(i18n::tr("Snap grid"), keys::label("playlist.sliceGrid")))
    run("chop clips", [&](Song& x) { return chop::sliceAtGrid(x, sel, gridTicks(a, kGridCount - 1)); });
  if (ImGui::BeginMenu(i18n::tr("Into equal parts"))) {
    for (int n : {2, 3, 4, 6, 8, 12, 16}) {
      char l[32];
      std::snprintf(l, sizeof l, i18n::tr("%d parts"), n);
      if (ImGui::MenuItem(l)) run("chop clips", [&](Song& x) { return chop::sliceEqual(x, sel, n); });
    }
    ImGui::EndMenu();
  }
  if (c->kind == ClipKind::Audio) {
    ImGui::SeparatorText(i18n::tr("At transients"));
    const char* lv[] = {i18n::N_("Gentle"), i18n::N_("Medium"), i18n::N_("Sharp")};
    const double vals[] = {0.25, 0.5, 0.8};
    for (int i = 0; i < 3; ++i)
      if (ImGui::MenuItem(i18n::tr(lv[i]))) run("slice clip at transients", [&](Song& x) { return chop::sliceAtTransients(x, sel, vals[i]); });
  } else if (c->kind == ClipKind::Pattern) {
    if (ImGui::MenuItem(i18n::tr("At the note starts"))) run("chop clips", [&](Song& x) { return chop::sliceAtTransients(x, sel, 0.5); });
  }
  if (c->kind != ClipKind::Automation) {
    ImGui::SeparatorText(i18n::tr("Stutter"));
    for (int i = 0; i < chop::kPresetCount; ++i) {
      const auto pr = static_cast<chop::Preset>(i);
      std::string l = std::string(i18n::tr(chop::presetName(pr))) + "...";
      if (ImGui::MenuItem(l.c_str())) open(a, clip, kStutter, i);
      ui::Hint(i18n::tr(chop::presetHint(pr)));
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Chop and stutter..."), keys::label("playlist.chopWindow"))) open(a, clip);
}

void duplicateTime(App& a) {
  if (!hasTimeSel(a)) return;
  const Tick lo = a.playlist.selStart, hi = a.playlist.selEnd;
  const auto tracks = regionTracks(a);
  std::vector<ObjectId> out;
  a.doc.editSong("duplicate time selection", [&](Song& s) { out = chop::duplicateRange(s, lo, hi, tracks); });
  if (out.empty()) return;
  a.playlist.selected = std::set<ObjectId>(out.begin(), out.end());
  a.playlist.selStart = hi, a.playlist.selEnd = hi + (hi - lo);  // the next duplicate follows on
}

void insertSpace(App& a) {
  if (!hasTimeSel(a)) return;
  const Tick lo = a.playlist.selStart, len = a.playlist.selEnd - a.playlist.selStart;
  const auto tracks = regionTracks(a);
  a.doc.editSong("insert space", [&](Song& s) { chop::insertSpace(s, lo, len, tracks); });
}

void deleteSpace(App& a) {
  if (!hasTimeSel(a)) return;
  const Tick lo = a.playlist.selStart, hi = a.playlist.selEnd;
  const auto tracks = regionTracks(a);
  a.doc.editSong("delete space", [&](Song& s) { chop::deleteSpace(s, lo, hi, tracks); });
  a.playlist.selected.clear();
}

void consolidateTime(App& a) {
  if (!hasTimeSel(a)) return;
  const Tick lo = a.playlist.selStart, hi = a.playlist.selEnd;
  const auto tracks = regionTracks(a);
  const std::string dir = renderedDir(a);
  const double sr = a.engine.sampleRate();
  ObjectId id = kNoId;
  a.doc.editSong("consolidate playlist selection", [&](Song& s) { id = chop::consolidateRange(s, lo, hi, tracks, dir, sr); });
  if (id == kNoId) {
    a.notify(i18n::tr("Nothing to render in the time selection"), true);
    return;
  }
  a.playlist.selected = {id};
  a.notify(i18n::tr("Time selection rendered to an audio clip"));
}

void chopTimeToSlicer(App& a, Tick grid) {
  if (!hasTimeSel(a)) return;
  const Tick lo = a.playlist.selStart, hi = a.playlist.selEnd;
  const auto tracks = regionTracks(a);
  const std::string dir = renderedDir(a);
  const double sr = a.engine.sampleRate();
  chop::ChopResult r;
  a.doc.editSong("chop selection to a slicer", [&](Song& s) { r = chop::chopRangeToSlicer(s, lo, hi, tracks, grid, 0.5, dir, sr); });
  if (r.clip == kNoId) {
    a.notify(i18n::tr("Nothing to render in the time selection"), true);
    return;
  }
  a.playlist.selected = {r.clip};
  a.selectPattern(r.pattern);
  char msg[96];
  std::snprintf(msg, sizeof msg, i18n::tr("Chopped into %d slices on a new slicer channel"), r.slices);
  a.notify(msg);
}

void timeSelectionItems(App& a) {
  const bool sel = hasTimeSel(a);
  if (ImGui::MenuItem(i18n::tr("Duplicate"), keys::label("playlist.duplicateTime"), false, sel)) duplicateTime(a);
  ui::Hint(i18n::tr("Copies the selected time right after it (on the selected clips' tracks, else on all)"));
  if (ImGui::MenuItem(i18n::tr("Insert space"), keys::label("playlist.insertSpace"), false, sel)) insertSpace(a);
  ui::Hint(i18n::tr("Moves everything from the selection start right by the selection length"));
  if (ImGui::MenuItem(i18n::tr("Delete space"), keys::label("playlist.deleteSpace"), false, sel)) deleteSpace(a);
  ui::Hint(i18n::tr("Removes the selected time and closes the gap"));
  if (ImGui::MenuItem(i18n::tr("Slice the clips at the selection edges"), nullptr, false, sel)) {
    const Tick lo = a.playlist.selStart, hi = a.playlist.selEnd;
    const auto tracks = regionTracks(a);
    std::set<ObjectId> inside;
    a.doc.editSong("slice clips", [&](Song& s) { inside = chop::isolateRange(s, lo, hi, tracks); });
    a.playlist.selected = inside;
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Consolidate to an audio clip"), keys::label("playlist.consolidateTime"), false, sel)) consolidateTime(a);
  ui::Hint(i18n::tr("Renders the selected time into one audio clip on a free track"));
  if (ImGui::BeginMenu(i18n::tr("Chop it into a slicer channel"), sel)) {
    const Song& s = a.doc.song();
    if (ImGui::MenuItem(i18n::tr("At every beat"))) chopTimeToSlicer(a, s.beatTicks());
    if (ImGui::MenuItem(i18n::tr("At every half beat"))) chopTimeToSlicer(a, std::max<Tick>(1, s.beatTicks() / 2));
    if (ImGui::MenuItem(i18n::tr("At every quarter beat"))) chopTimeToSlicer(a, std::max<Tick>(1, s.beatTicks() / 4));
    if (ImGui::MenuItem(i18n::tr("At transients"))) chopTimeToSlicer(a, 0);
    ImGui::EndMenu();
  }
  ui::Hint(i18n::tr("Renders the selected time, loads it into a new slicer channel and writes a pattern that plays the slices"));
  if (ImGui::MenuItem(i18n::tr("Stutter..."), keys::label("playlist.chopWindow"), false, sel)) open(a, kNoId, kStutter);
}

void shortcuts(App& a) {
  if (keys::pressed("playlist.chopWindow")) open(a);
  if (keys::pressed("playlist.sliceGrid")) sliceSelectedAtGrid(a, gridTicks(a, kGridCount - 1));
  if (keys::pressed("playlist.duplicateTime")) duplicateTime(a);
  if (keys::pressed("playlist.insertSpace")) insertSpace(a);
  if (keys::pressed("playlist.deleteSpace")) deleteSpace(a);
  if (keys::pressed("playlist.consolidateTime")) consolidateTime(a);
}

void devDemo(App& a) {
  // Screenshots: a synthetic drum loop (written to the temp folder) as an
  // audio clip on a free track, then: "stutter" / "build" opens the window on
  // a bar of it with a preset; "slice" on the whole clip at transients;
  // "chop" sets a time selection over it and opens the playlist menu.
  static int frames = 0;
  const char* env = std::getenv("GRIDLINE_PL13_DEMO");
  if (!env || !*env || ++frames != 4) return;
  const std::string mode = env;
  const double sr = 44100;
  ed::Audio au = ed::makeAudio(sr, 2, static_cast<int64_t>(sr * 4.0 * 60.0 / std::max(40.0, a.doc.song().tempo) * 2));
  const double beat = 60.0 / std::max(40.0, a.doc.song().tempo);
  uint32_t rng = 12345;
  for (int64_t i = 0; i < au.frames(); ++i) {
    const double t = static_cast<double>(i) / sr, b = std::fmod(t, beat), h = std::fmod(t + beat * 0.5, beat);
    rng = rng * 1664525u + 1013904223u;
    const double noise = (static_cast<double>(rng >> 8) / 8388608.0 - 1.0);
    double v = 0.8 * std::exp(-b * 18) * std::sin(2 * M_PI * (50 + 90 * std::exp(-b * 40)) * b);
    v += 0.25 * std::exp(-h * 60) * noise;
    if (std::fmod(t, beat * 2) >= beat) v += 0.35 * std::exp(-b * 25) * noise;
    au.L[static_cast<size_t>(i)] = au.R[static_cast<size_t>(i)] = static_cast<float>(v);
  }
  std::error_code ec;
  const std::string path = (std::filesystem::temp_directory_path(ec) / "gridline-pl13-demo-loop.wav").string();
  ed::writeWavFile(path, ed::makeContent(au), 32);
  ObjectId clip = kNoId;
  a.doc.editSong("PL-13 demo", [&](Song& s) {
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Drum loop";
    ch.color = 0xC8724E;
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = path;
    ch.mixerTrack = s.masterId();
    ch.audioClip = true;
    s.channels.push_back(ch);
    int track = 0;
    for (int t = 0; t < static_cast<int>(s.tracks.size()); ++t) {
      bool used = false;
      for (const auto& c : s.playlist) used = used || c.track == t;
      if (!used) {
        track = t;
        break;
      }
    }
    if (static_cast<int>(s.tracks.size()) <= track) s.tracks.resize(static_cast<size_t>(track) + 1);
    s.tracks[static_cast<size_t>(track)].name = "Drum loop";
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch.id;
    c.track = track;
    c.start = 0;
    c.length = s.barTicks() * 4;
    s.playlist.push_back(c);
    clip = c.id;
  });
  const Tick bar = a.doc.song().barTicks();
  a.playlist.selected = {clip};
  if (mode == "slice") {
    open(a, clip, kSlice);
    st().sliceMode = kTransients;
  } else if (mode == "chop") {
    a.playlist.selStart = bar, a.playlist.selEnd = bar * 3;
    a.playlist.selected.clear();
  } else {
    a.playlist.selStart = bar, a.playlist.selEnd = bar * 2;
    open(a, clip, kStutter, static_cast<int>(mode == "build" ? chop::Preset::ReverseBuild : chop::Preset::TripletStutter));
  }
}

}  // namespace gl::chopui
