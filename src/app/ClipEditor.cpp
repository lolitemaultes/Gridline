// Gridline: the audio clip editor. See ClipEditor.h.
#include "ClipEditor.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Automation.h"
#include "../core/ClipGain.h"
#include "../core/ClipWindow.h"
#include "../core/Params.h"
#include "../core/Platform.h"
#include "../core/SampleWarp.h"
#include "../core/ClipRate.h"
#include "../core/Fades.h"
#include "../core/I18n.h"
#include "../core/LayoutPreset.h"
#include "../core/MenuParity.h"
#include "../core/PluginParams.h"
#include "../engine/AudioRecord.h"
#include "../engine/ClipAudioEdit.h"
#include "../engine/ClipFields.h"
#include "../engine/ClipGestures.h"
#include "../engine/ClipOps.h"
#include "../engine/ClipRender.h"
#include "../engine/ClipSource.h"
#include "../engine/PlaylistTools.h"
#include "../engine/Sample.h"
#include "../engine/TakeLanes.h"
#include "../engine/TimeStretch.h"
#include "../engine/WarpOps.h"
#include "../packs/PacksUi.h"
#include "../packs/edison/edison.h"
#include "../packs/edison/edison_specimage.h"
#include "../packs/modulation/modulation_ui.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "BrowserUi.h"
#include "ChannelRoute.h"
#include "ChopUi.h"
#include "ClipToolsUi.h"
#include "ControlStyle.h"
#include "ClipRenderUi.h"
#include "ExportUi.h"
#include "GrooveUi.h"
#include "HostedBodies.h"
#include "KeyGlide.h"
#include "KeyLight.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutParts.h"
#include "LayoutShellUi.h"
#include "LiveLook.h"
#include "LiveTakes.h"
#include "LogicPlugins.h"
#include "MacroUi.h"
#include "MixerHideUi.h"
#include "PlaylistWarp.h"
#include "PluginUI.h"
#include "StemsUi.h"
#include "DevSignal.h"
#include "Theme.h"
#include "TrackerEditors.h"
#include "TrackerLook.h"
#include "TrackerPages.h"
#include "TrackerView.h"
#include "WarpWave.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl {

// Playlist.cpp: the playlist's own waveform drawing (warped, looped, at the clip's gain).
void drawAudioClipWave(ImDrawList* dl, const Song& s, const Channel& ch, const Clip& c, const TempoMap& tm, ImVec2 a, ImVec2 b, float clipX0,
                       float pxPerTick, float visX0, float visX1, ImU32 col);
// Playlist.cpp (dev checks): the pixel columns the waveform drew while recording is on.
void waveRunsRecord(bool on);
const std::vector<std::pair<float, float>>& waveRuns();
// Playlist.cpp: the scale the waveforms drawn next are drawn at (1: the playlist's own).
void waveScale(float k);

namespace clipeditor {

namespace ed = packs::edison;
namespace fs = std::filesystem;
using theme::pal;

namespace {

enum Tool : int { TPointer = 0, TWarp, TSlip, TFade, TSlice, TPencil, TScrub, TZoom, TCount };
using clipwin::TabClip;
using clipwin::TabTime;
using clipwin::TabSample;
using clipwin::TabPlayback;
using clipwin::TabEnvelope;
using clipwin::TabSampleFx;
using clipwin::TabFilter;
using clipwin::TabChannel;
using clipwin::TabNoteFx;
using clipwin::TabMidi;
using clipwin::TabEdit;
using clipwin::TabAnalysis;
using clipwin::TabCount;

struct State {
  ObjectId clip = kNoId, fitClip = kNoId;
  ObjectId channel = kNoId;  // the channel shown: the clip's, or a sampler channel no clip plays
  float panelH = 0;          // the modules' panel as the divider was dragged (0: the host's own)
  bool editDev = false;      // (dev) open the Edit audio menu next frame
  bool compactDev = false;   // (dev "clipeditor strip") the modules drew compact
  // (dev "clipeditor strip") the tab bar's tabs, the modules' panel, its scroll, as last drawn
  std::vector<std::pair<ImVec2, ImVec2>> tabRects;
  ImVec2 panelA, panelB;
  bool panelScrolls = false;
  int moreTabs = 0;
  // (dev "clipeditor toolrow") the tools row's items as last drawn, the row's box, and whether it
  // put the view's options in its menu (no room for them on the row)
  std::vector<std::pair<ImVec2, ImVec2>> toolRects;
  ImVec2 toolA, toolB;
  bool toolMenu = false;
  int hdrag = -1;            // the overview's handle being dragged (0 start, 1 end; -1 none)
  bool menuDev = false;      // (dev) open the header menu next frame
  bool chipsMode = false;    // (0.7.8 earlier: the Clips band's chip row; the band shows its own view now, always 0)
  bool mainDrawn = false;    // the window drew its Main view (dev "clipeditor mainpage": its knob and cell)
  float mainCell = 0, mainCardH = 0;
  double t0 = 0, t1 = 0;  // the view, song ticks
  bool fitted = false;
  int tab = TabClip;
  int scrollTab = -1;   // (a band) the tab whose first card is brought into view next frame (-1: none)
  std::vector<std::pair<std::string, std::pair<ImVec2, ImVec2>>> cardRects;  // (a band) its cards, as last drawn (dev "clipeditor geom")
  bool viewDev = false;  // (dev) open the band tools' view menu next frame
  int tool = TPointer;
  bool spectrum = false;
  bool seconds = false;  // ruler in min:sec
  int snap = 2;          // 0 off, 1 bar, 2 beat, 3 half beat, 4 sixteenth
  bool follow = false;
  bool loopPreview = false;
  double selT0 = 0, selT1 = 0;  // the time selection (ticks; empty when equal)
  // gestures
  int drag = 0;  // 0 none, 1 trim start, 2 trim end, 3 slip, 4 fade in, 5 fade out, 6 curve in, 7 curve out, 8 gain, 9 select, 10 warp, 11 pencil, 12 scrub, 13 overview
  Clip base;
  float y0 = 0, x0 = 0;
  double tick0 = 0;
  float uiStart = 0;
  clipgain::Drag gainDrag;
  double scrubAt = -1;
  double lastScrub = 0;
  // the last frame's geometry (dev "clipeditor geom")
  ImVec2 waveA, waveB, ovA, ovB, tabsA, tabsB;
  float xs = 0, xe = 0;
  float wavePeak = 0;  // (dev "clipeditor fill") the share of the waveform's height its loudest column takes
  std::vector<std::pair<float, float>> audiblePx, drawnPx;  // where the clip sounds / where its waveform was drawn (px runs)
};
State gHost[4];  // by Host: the window, the Clips clip band, the pane, the Clips device band
// What the window and the pane show: a clip, or a sampler channel no clip plays (clip kNoId).
struct Target {
  ObjectId clip = kNoId, channel = kNoId;
};
Target gWindow;
Target gPane;
bool gPaneOn = false;
// (dev) the checklist: the controls drawn since "clipeditor reach on" (core/ClipWindow.h's keys).
std::set<std::string> gReached;
bool gReachOn = false;
void reach(const char* key) {
  if (gReachOn) gReached.insert(key);
}
int gDrawFrame[4] = {-100, -100, -100, -100};
// 0.7.8: the Tracker's Sample editor is drawing the window's parts (trackerpage::begin .. end): the
// parts drawn with their own fills take the Tracker's bevelled boxes instead (the shared controls
// take them from the Tracker control style the page sets).
bool gTrkSkin = false;
// K-391: a time selection handed to the Tracker's page, applied once its view has fitted the clip.
struct PendSel {
  bool on = false;
  ObjectId clip = kNoId;
  double t0 = 0, t1 = 0;
};
PendSel gPendSel;
ImVec2 gDevWinSize(0, 0);  // (dev "clipeditor winsize <w> <h>") the window's size next frame
int gDevHost = -1;  // (dev) "clipeditor host <name>": the host the dev commands act on (-1: the one drawn)
std::string gDevLastOp;

State& st(Host h) { return gHost[static_cast<int>(h)]; }

bool clipsLayout() { return layoutui::active() == layout::Preset::Live; }
// The Clips layout's two bands (the clip band, the device band) draw the band form.
bool bandHost(Host h) { return h == Host::Clips || h == Host::Device; }
bool smooth() { return layoutui::active() == layout::Preset::Logic; }
bool tracker() { return layoutui::active() == layout::Preset::Tracker; }

const Channel* channelOf(const Song& s, const Clip& c) { return c.kind == ClipKind::Audio ? s.channel(c.source) : nullptr; }

int clipsSharing(const Song& s, ObjectId channel, std::vector<std::string>* names = nullptr) {
  int n = 0;
  for (const Clip& k : s.playlist)
    if (k.kind == ClipKind::Audio && k.source == channel) {
      ++n;
      if (names && names->size() < 4) names->push_back(k.name.empty() ? (s.channel(channel) ? s.channel(channel)->name : std::string()) : k.name);
    }
  return n;
}

std::vector<ObjectId> selectionFor(const App& a, ObjectId clip) {
  std::vector<ObjectId> v;
  const Song& s = a.doc.song();
  if (a.playlist.selected.count(clip))
    for (ObjectId id : a.playlist.selected)
      if (const Clip* c = s.clip(id); c && c->kind == ClipKind::Audio) v.push_back(id);
  if (std::find(v.begin(), v.end(), clip) == v.end()) v.push_back(clip);
  return v;
}

uint64_t keyOf(ObjectId clip, int field) { return 0x434C4544ull << 32 ^ (static_cast<uint64_t>(clip) << 8) ^ static_cast<uint64_t>(field); }

bool setField(App& a, ObjectId clip, clipfields::Field f, double v, bool merge = true) {
  return clipfields::set(a.doc, selectionFor(a, clip), clip, f, v, merge ? keyOf(clip, static_cast<int>(f)) : 0);
}

Tick snapStep(const Song& s, int snap) {
  const Tick beat = std::max<Tick>(1, s.beatTicks());
  switch (snap) {
    case 1: return std::max<Tick>(1, s.barTicks());
    case 2: return beat;
    case 3: return std::max<Tick>(1, beat / 2);
    case 4: return std::max<Tick>(1, beat / 4);
    default: return 1;
  }
}

double snapTick(double t, Tick step, bool free) {
  if (free || step <= 1) return std::round(t);
  return std::round(t / static_cast<double>(step)) * static_cast<double>(step);
}

// A label in the current layout's word.
std::string word(const char* clipText) {
  std::string t = i18n::tr(clipText);
  return t;
}

// A knob over a clip field (one undo step per drag).
bool fieldKnob(App& a, const char* id, ObjectId clip, clipfields::Field f, double v, double lo, double hi, double def, const char* label,
               const char* unit, bool bipolar, double step = 0, float radius = 15.0f) {
  ParamDesc d;
  d.name = label;
  d.unit = unit;
  d.min = lo, d.max = hi, d.def = def;
  double x = v;
  bool rel = false;
  if (step > 0) ui::SetNextKnobStep(step);
  ImGui::BeginGroup();
  const bool ch = ui::Knob(id, &x, d, radius, nullptr, bipolar, &rel);
  ImGui::EndGroup();
  if (ch && x != v) setField(a, clip, f, x);
  if (rel) a.doc.endGesture();
  return ch;
}


// ------------------------------------------------------------------ analysis cache

struct Measures {
  std::string key;
  ed::Levels lv;
  ed::PitchInfo pitch;
  double bpm = 0;
  bool ok = false;
};
Measures gMeasures;

const Measures& measure(const std::string& path, int64_t a, int64_t b) {
  char k[64];
  std::snprintf(k, sizeof k, "|%lld|%lld", static_cast<long long>(a), static_cast<long long>(b));
  const std::string key = path + k;
  if (gMeasures.key == key) return gMeasures;
  gMeasures = Measures{};
  gMeasures.key = key;
  SamplePtr smp = sampleCache().request(path);
  if (!smp) {
    gMeasures.key.clear();  // (not decoded yet: again next frame)
    return gMeasures;
  }
  const ed::Audio au = ed::fromSample(*smp);
  ed::Range r{std::clamp<int64_t>(a, 0, au.frames()), std::clamp<int64_t>(b, 0, au.frames())};
  if (r.empty()) r = ed::Range{0, au.frames()};
  gMeasures.lv = ed::measure(au, r);
  gMeasures.pitch = ed::detectPitch(au, r);
  gMeasures.bpm = ed::estimateTempo(ed::detectOnsets(au, r, 0.5), au.rate);
  gMeasures.ok = true;
  return gMeasures;
}

// ------------------------------------------------------------------ edits

// `chanOnly`: a sampler channel no clip plays (the window without a clip): the whole file.
void runAudioOp(App& a, ObjectId clip, clipedit::Op op, const clipedit::Args& args, double selT0, double selT1, ObjectId chanOnly = kNoId) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  const Channel* ch = c ? channelOf(s, *c) : s.channel(chanOnly);
  if (!ch || ch->plugin.data.empty()) return;
  int64_t fa = 0, fb = 0;
  if (!c) {
    if (clipedit::opUsesRange(op)) {
      const ClipSource src = clipSource(*ch);
      if (!src.sample) return;
      fb = static_cast<int64_t>(src.sample->frames());
    }
  } else if (clipedit::opUsesRange(op)) {
    const double t0 = selT1 > selT0 ? selT0 : static_cast<double>(c->start);
    const double t1 = selT1 > selT0 ? selT1 : static_cast<double>(c->start + s.clipLength(*c));
    if (!clipedit::clipRangeFrames(s, clip, t0, t1, fa, fb)) {  // (a selection over the silence after the audio: nothing to edit)
      a.notify(std::string(i18n::tr(clipedit::opName(op))) + ": " + i18n::tr("the selection holds none of the clip's audio"), true);
      return;
    }
  }
  const std::string src = ch->plugin.data.substr(0, ch->plugin.data.find('\n'));
  const ObjectId chId = ch->id;
  const std::string dir = editedDir(a);
  const clipedit::Result r = clipedit::apply(src, fa, fb, op, args, dir);
  gDevLastOp = std::string(clipedit::opName(op)) + " " + (r.path.empty() ? "failed: " + r.error : r.path);
  if (r.path.empty()) {
    a.notify(std::string(i18n::tr(clipedit::opName(op))) + ": " + i18n::tr(r.error.c_str()), true);
    return;
  }
  const std::string label = std::string("edit audio: ") + clipedit::opName(op);
  a.doc.editChannel(chId, label, [&](Channel& x) {
    const size_t nl = x.plugin.data.find('\n');  // (a slicer's region names stay after the path)
    x.plugin.data = nl == std::string::npos ? r.path : r.path + x.plugin.data.substr(nl);
  });
  a.setHint(std::string(i18n::tr(clipedit::opName(op))) + ": " + i18n::tr("a new file; the old one is kept (undo goes back to it)"));
}

// The dialog an operation with settings opens first.
struct OpDialog {
  bool open = false;
  ObjectId clip = kNoId, chanOnly = kNoId;
  clipedit::Op op = clipedit::Op::Amplify;
  clipedit::Args args;
  double selT0 = 0, selT1 = 0;
};
OpDialog gDialog;

void askOp(App& a, ObjectId clip, clipedit::Op op, double selT0, double selT1, ObjectId chanOnly = kNoId) {
  if (!clipedit::opHasDialog(op)) {
    runAudioOp(a, clip, op, clipedit::Args{}, selT0, selT1, chanOnly);
    return;
  }
  gDialog = OpDialog{};
  gDialog.open = true;
  gDialog.clip = clip;
  gDialog.chanOnly = chanOnly;
  gDialog.op = op;
  gDialog.selT0 = selT0, gDialog.selT1 = selT1;
  if (op == clipedit::Op::Amplify) gDialog.args.db = 3;
  if (op == clipedit::Op::Gate) gDialog.args.db = -40;
  if (op == clipedit::Op::Resample) gDialog.args.rate = 48000;
  if (op == clipedit::Op::Reverb) gDialog.args.mix = 0.25;
  if (op == clipedit::Op::Blur) gDialog.args.mix = 0.5;
  ImGui::OpenPopup("##clipedop");
}

void drawOpDialog(App& a) {
  if (!gDialog.open) return;
  if (!ImGui::IsPopupOpen("##clipedop")) ImGui::OpenPopup("##clipedop");
  if (!ImGui::BeginPopup("##clipedop")) {
    gDialog.open = false;
    return;
  }
  clipedit::Args& g = gDialog.args;
  ImGui::TextUnformatted(i18n::tr(clipedit::opName(gDialog.op)));
  ImGui::Separator();
  auto dragD = [](const char* label, double* v, float speed, double lo, double hi, const char* fmt) {
    float f = static_cast<float>(*v);
    ImGui::SetNextItemWidth(160);
    if (ImGui::DragFloat(label, &f, speed, static_cast<float>(lo), static_cast<float>(hi), fmt)) *v = f;
  };
  switch (gDialog.op) {
    case clipedit::Op::Amplify: dragD(i18n::tr("Gain"), &g.db, 0.1f, -60, 36, "%+.1f dB"); break;
    case clipedit::Op::Gate: dragD(i18n::tr("Threshold"), &g.db, 0.5f, -96, 0, "%.0f dB"); break;
    case clipedit::Op::InsertSilence: dragD(i18n::tr("Length"), &g.seconds, 0.01f, 0.001, 600, "%.3f s"); break;
    case clipedit::Op::StretchPitch:
      dragD(i18n::tr("Length"), &g.factor, 0.005f, 0.125, 8, "x %.3f");
      dragD(i18n::tr("Pitch"), &g.semitones, 0.05f, -24, 24, "%+.2f st");
      break;
    case clipedit::Op::Resample: {
      const char* rates[] = {"22050", "32000", "44100", "48000", "88200", "96000"};
      const double rv[] = {22050, 32000, 44100, 48000, 88200, 96000};
      int idx = 3;
      for (int i = 0; i < 6; ++i)
        if (std::fabs(g.rate - rv[i]) < 1) idx = i;
      if (ui::Segmented("##rate", rates, 6, &idx, 54)) g.rate = rv[idx];
      break;
    }
    case clipedit::Op::Equalize:
      dragD(i18n::tr("Low"), &g.eqLowDb, 0.1f, -18, 18, "%+.1f dB");
      dragD(i18n::tr("Mid"), &g.eqMidDb, 0.1f, -18, 18, "%+.1f dB");
      dragD(i18n::tr("High"), &g.eqHighDb, 0.1f, -18, 18, "%+.1f dB");
      break;
    case clipedit::Op::Reverb: dragD(i18n::tr("Mix"), &g.mix, 0.005f, 0, 1, "%.2f"); break;
    case clipedit::Op::Blur:
      dragD(i18n::tr("Length"), &g.ms, 0.5f, 1, 1000, "%.0f ms");
      dragD(i18n::tr("Mix"), &g.mix, 0.005f, 0, 1, "%.2f");
      break;
    default: break;
  }
  ImGui::Dummy(ImVec2(0, 4));
  if (ui::PrimaryButton("Apply", 90)) {
    runAudioOp(a, gDialog.clip, gDialog.op, g, gDialog.selT0, gDialog.selT1, gDialog.chanOnly);
    gDialog.open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::TextButton("Cancel", false, 90)) {
    gDialog.open = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// The clip's region of its file as a new file (To sampler, To slicer).
std::string clipRegionFile(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  const Channel* ch = c ? channelOf(s, *c) : nullptr;
  if (!c || !ch) return "";
  int64_t fa = 0, fb = 0;
  if (!clipedit::clipRangeFrames(s, clip, static_cast<double>(c->start), static_cast<double>(c->start + s.clipLength(*c)), fa, fb)) return "";
  const clipedit::Result r = clipedit::apply(ch->plugin.data.substr(0, ch->plugin.data.find('\n')), fa, fb, clipedit::Op::TrimToSelection, {}, editedDir(a));
  return r.path;
}

void previewClip(App& a, ObjectId clip, bool loop) {
  if (a.engine.previewing()) {
    a.engine.preview(nullptr);
    return;
  }
  const int sr = a.audioSampleRate > 0 ? a.audioSampleRate : 44100;
  const RenderResult r = renderClipAlone(a.doc.song(), clip, sr);
  if (!r.error.empty() || r.audio.frames == 0 || r.audio.channels == 0) {
    a.notify(r.error.empty() ? std::string("Nothing to play") : r.error, true);
    return;
  }
  auto smp = std::make_shared<Sample>();
  smp->sampleRate = sr;
  const float* l = r.audio.plane(0);
  const float* rr = r.audio.plane(r.audio.channels > 1 ? 1 : 0);
  smp->left.assign(l, l + r.audio.frames);
  smp->right.assign(rr, rr + r.audio.frames);
  a.engine.preview(smp, true, loop ? 0 : -1);
}

void scrubAt(App& a, const Song& s, const Clip& c, const TempoMap& tm, double tick) {
  const Channel* ch = channelOf(s, c);
  if (!ch) return;
  const ClipSource src = clipSourceIfReady(*ch);
  if (!src.ok()) return;
  double sec = clipHeardSourceAt(clipAudibleSpans(s, c, tm), s, tm, tick);  // as it plays (a loop's passes; none after the audio)
  if (!std::isfinite(sec)) return;
  const double rate = src.sample->sampleRate;
  if (clipPlaysReversed(*ch, c)) sec = static_cast<double>(src.sample->frames() - 1) / rate - sec;
  const int64_t n = static_cast<int64_t>(src.sample->frames());
  const int64_t f0 = std::clamp<int64_t>(static_cast<int64_t>(sec * rate), 0, n);
  const int64_t len = std::min<int64_t>(n - f0, static_cast<int64_t>(rate * 0.07));
  if (len < 32) return;
  auto g = std::make_shared<Sample>();
  g->sampleRate = rate;
  g->left.assign(src.sample->left.begin() + f0, src.sample->left.begin() + f0 + len);
  std::vector<float> r(src.sample->right.begin() + f0, src.sample->right.begin() + f0 + len);
  for (int64_t i = 0; i < len; ++i) {  // a short fade at both ends (no clicks)
    const float w = std::min(1.0f, std::min(static_cast<float>(i), static_cast<float>(len - 1 - i)) / 64.0f) * c.gain;
    g->left[static_cast<size_t>(i)] *= w;
    r[static_cast<size_t>(i)] *= w;
  }
  g->right = std::move(r);
  a.engine.preview(g, true);
}

// ------------------------------------------------------------------ header and tool strip

// The sample's orange: its tabs' cards, the "shared by" pill, the scope line of its tabs.
ImU32 sampleOrange() { return theme::rgb(0xF5A623); }
ImU32 handleBlue() { return theme::rgb(0x4C8DFF); }

// The channel's sampler parameter `idx` (its default when the state is short).
float smpPar(const Channel& ch, int idx, float def) {
  return ch.plugin.params.size() > static_cast<size_t>(idx) ? ch.plugin.params[static_cast<size_t>(idx)] : def;
}
ParamId smpPid(ObjectId ch, int idx) { return PluginTarget{ch, -1}.pid(idx); }

// 0.7.8: the window's two views (Studio's window, and the window the other layouts float): Main, the
// sampler's own page (every setting of the sampler channel's old window) on one page, and Advanced,
// the tabs. An app setting ("ui.clipWindowView": main, advanced), never saved in a project.
bool mainView(const App& a) { return a.settings.getString("ui.clipWindowView") != "advanced"; }
void setMainView(App& a, bool on) { a.settings.set("ui.clipWindowView", on ? std::string("main") : std::string("advanced")); }
// The header's Main | Advanced pair: its width, and drawn at `at` `h` tall.
const char* const kViewNames[2] = {i18n::N_("Main"), i18n::N_("Advanced")};
float viewToggleW() {
  float seg = 0;
  for (const char* n : kViewNames) seg = std::max(seg, std::ceil(ImGui::CalcTextSize(i18n::tr(n)).x + 20));
  return 2 * seg + 4;
}
void viewToggle(App& a, ImVec2 at, float h) {
  const char* names[2] = {i18n::tr(kViewNames[0]), i18n::tr(kViewNames[1])};
  const char* hints[2] = {i18n::tr("Main: the sample, how it plays, its envelope, filter, LFO and sample effects on one page"),
                          i18n::tr("Advanced: every setting of the clip, the sample and the channel in tabs, with the waveform editor")};
  int idx = mainView(a) ? 0 : 1;
  ImGui::SetCursorScreenPos(at);
  if (ui::Segmented("##ceview", names, 2, &idx, std::floor((viewToggleW() - 4) * 0.5f), h, hints)) setMainView(a, idx == 0);
  devNoteRect("ceview");  // (dev: "sdlat" targets, the pair and each half)
  const float half = std::floor((viewToggleW() - 4) * 0.5f);
  devNoteRectAt("ceviewmain", at.x, at.y, at.x + half, at.y + h);
  devNoteRectAt("ceviewadvanced", at.x + viewToggleW() - half, at.y, at.x + viewToggleW(), at.y + h);
}

// The file the channel plays (a slicer's region names after the path are left out).
std::string samplePath(const Channel& ch) { return ch.plugin.data.substr(0, ch.plugin.data.find('\n')); }

// The header's one text line: every text in it sits on this baseline (`cy` the header's middle).
float headerBaseline(float cy) { return std::round(cy + 5.0f); }
// The top to draw a text at so it sits on `baseline` (the font's ascent at that size).
float topForBaseline(ImFont* f, float size, float baseline) { return std::round(baseline - f->GetFontBaked(size)->Ascent); }

// The header menu: the instrument's menu (presets, state, rename, clone, colour, replace, delete,
// piano roll), then the clip's and the window's own items.
void headerMenu(App& a, const Clip* c, const Channel& ch) {
  for (const char* k : {"menu.presets", "menu.load", "menu.save", "menu.copy", "menu.paste", "menu.random", "menu.reset", "menu.replace",
                        "menu.rename", "menu.clone", "menu.colour", "menu.delete", "menu.pianoroll"})
    reach(k);
  pluginWindowMenu(a, ch.id, -1, false, true);
  ImGui::Separator();
  const ObjectId chId = ch.id;
  reach("menu.rack");
  if (ImGui::MenuItem(i18n::tr("Show in the channel rack"))) {
    a.selectChannel(chId);
    a.showRack = true;
    a.focusWindow = "Channel rack";
  }
  if (c) {
    reach("menu.unique");
    const ObjectId id = c->id;
    if (ImGui::MenuItem(i18n::tr("Make this clip's sample unique"), nullptr, false, clipsSharing(a.doc.song(), chId) > 1))
      a.later([&a, id] {
        if (ops::makeAudioClipUnique(a.doc, id) != kNoId) a.setHint(i18n::tr("The clip has its own sample now"));
      });
    ui::Hint("This clip gets its own copy of the sample's channel, so the sample's settings change it alone");
    reach("menu.audiotools");
    cliptoolsui::clipMenu(a, id);  // the Audio tools submenu (Loupe's tools on a new copy, one undo step)
  }
}

void header(App& a, const Clip* c, const Channel& ch, ImVec2 p0, float w, float h, Host host) {
  const bool views = host == Host::Window;  // (the window: its Main | Advanced pair)
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.surface800);
  dl->AddLine(ImVec2(p0.x, p0.y + h - 0.5f), ImVec2(p0.x + w, p0.y + h - 0.5f), P.line);
  float x = p0.x + 12;
  const float cy = std::round(p0.y + h * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(x, cy - 8));
  uint32_t rgb = ch.color;
  reach("colour");
  if (ui::ColorSwatch("##cecol", &rgb, ImVec2(16, 16)) && rgb != ch.color) ops::setChannelsColor(a.doc, {ch.id}, rgb);
  ui::Hint("Colour of the clip's channel (every clip of the sample)");
  x += 26;
  reach("name");
  const std::string name = c && !c->name.empty() ? c->name : ch.name;
  const float fs = theme::kFontTitle;
  // (a narrow header, the Clips device band's card or a small window: the clip's name over its file)
  const bool narrow = w < 900;
  const float bh = std::min(24.0f, h - 8);
  // The right side: the sampler's presets (and A/B) when they fit, else previous / next; the menu.
  const PluginInfo* pinfo = findPlugin(ch.plugin.type);
  const bool fullPresets = pinfo && !pluginHeaderTwoRows(a) && pluginHeaderWidth(a, *pinfo) < w * 0.45f;
  // The channel's mixer track box and the free track's button, as the plugin windows' header has them.
  const float routeW = std::floor(std::clamp(w * 0.15f, 120.0f, 190.0f)), routeBtn = bh;
  const float rightW = 12 + bh + 12 + (fullPresets ? pluginHeaderWidth(a, *pinfo) : 2 * 22 + 2) + 16 + routeW + 4 + routeBtn + 16 +
                       (views ? viewToggleW() + 16 : 0.0f);
  const float nameRoom = std::max(24.0f, p0.x + w - rightW - x - 4);
  const float base = headerBaseline(cy);
  ImFont* uiFont = ImGui::GetFont();
  // The file, and who shares it.
  const std::string path = samplePath(ch);
  const std::string info = path.empty() ? std::string(i18n::tr("No sample")) : fs::path(path).filename().string();
  std::string meta;
  if (const ClipSource src = clipSourceIfReady(ch); src.sample) {
    char b[160];
    std::snprintf(b, sizeof b, "%.1f kHz \xC2\xB7 %s \xC2\xB7 %.2f s", src.sample->sampleRate / 1000.0,
                  src.sample->mono() ? i18n::tr("mono") : i18n::tr("stereo"), src.sample->seconds());
    meta = b;
  }
  float infoX = x, infoBase = base;  // where the file's line starts (after the name, or under it)
  if (narrow) {  // the name over the file, both cut to the room
    const float nfs = theme::kFontBody;
    theme::textEllipsis(dl, a.fontBold, nfs, ImVec2(x, std::round(cy - nfs - 1)), nameRoom, P.text, name.c_str());
    infoBase = std::round(cy + 1 + uiFont->GetFontBaked(theme::kFontCaption)->Ascent);
  } else {
    theme::textEllipsis(dl, a.fontBold, fs, ImVec2(x, topForBaseline(a.fontBold, fs, base)), nameRoom, P.text, name.c_str());
    x += std::min(nameRoom, a.fontBold->CalcTextSizeA(fs, 1e9f, 0, name.c_str()).x) + 12;
    infoX = x;
  }
  reach("info");
  if (c && !c->warp.empty()) {
    const float bw = warpwave::badge(dl, ImVec2(narrow ? x + std::min(nameRoom, a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, name.c_str()).x) + 8 : x,
                                                narrow ? cy - 17 : cy - 8),
                                     static_cast<int>(c->warp.size()));
    if (!narrow) x += bw + 8, infoX = x;
  }
  const int shared = clipsSharing(s, ch.id);

  // Right side, from the right edge: the menu, Open in Loupe, the mixer track (and a free one), the
  // channel's volume, pan and pitch.
  float rx = p0.x + w - 12 - bh;
  ImGui::SetCursorScreenPos(ImVec2(rx, cy - bh * 0.5f));
  reach("menu");
  const bool menuClick = ui::IconButton("##cemenu", ui::Icon::Menu, false, bh, "Channel menu: presets, state, rename, clone, replace or delete the channel");
  State& S = st(host);
  if (menuClick || S.menuDev) {
    S.menuDev = false;
    ImGui::OpenPopup("##cemenupop");
  }
  ImGui::SetNextWindowPos(ImVec2(rx + bh, cy + bh * 0.5f + 2), ImGuiCond_Appearing, ImVec2(1, 0));  // under the button, its right edge
  if (ImGui::BeginPopup("##cemenupop")) {
    headerMenu(a, c, ch);
    ImGui::EndPopup();
  }
  rx -= 12;
  reach("presets");
  {
    const PluginTarget t{ch.id, -1};
    if (fullPresets) {
      rx -= pluginHeaderWidth(a, *pinfo);
      ImGui::SetCursorScreenPos(ImVec2(rx, cy - std::floor(ImGui::GetFrameHeight() * 0.5f)));
      ImGui::BeginGroup();
      drawPluginHeader(a, t, false);
      ImGui::EndGroup();
      rx = ImGui::GetItemRectMin().x;
    } else {
      const float pb = 22;
      rx -= pb;
      ImGui::SetCursorScreenPos(ImVec2(rx, cy - pb * 0.5f));
      if (ui::IconButton("##cepnext", ui::Icon::Right, false, pb, "Next preset")) stepPluginPreset(a, t, 1);
      rx -= 2 + pb;
      ImGui::SetCursorScreenPos(ImVec2(rx, cy - pb * 0.5f));
      if (ui::IconButton("##cepprev", ui::Icon::Left, false, pb, "Previous preset")) stepPluginPreset(a, t, -1);
    }
  }
  rx -= 16;
  // The mixer track the channel plays through: the box (click: the mixer's tracks, a free one
  // first), then the free track's button; where a plugin window's header has them.
  reach("route");
  reach("routefree");
  {
    const ObjectId chId = ch.id;
    rx -= routeBtn;
    ImGui::SetCursorScreenPos(ImVec2(rx, std::round(cy - routeBtn * 0.5f)));
    if (chanroute::button(a, chId, routeBtn)) a.later([&a, chId] { chanroute::routeToFree(a, chId); });
    devNoteRect("ceroutefree");  // (dev: an "sdlat" target)
    rx -= 4 + routeW;
    ImGui::SetCursorScreenPos(ImVec2(rx, std::round(cy - ImGui::GetFrameHeight() * 0.5f)));
    if (chanroute::box(a, chId, "##ceroute", routeW, "ceroute")) a.later([&a, chId] { chanroute::routeToFree(a, chId); });
    rx -= 16;
  }
  if (views) {  // Main | Advanced
    rx -= viewToggleW();
    viewToggle(a, ImVec2(rx, std::round(cy - bh * 0.5f)), bh);
    rx -= 16;
  }
  // "and N more selected" (edits of the clip's own settings change them all).
  if (c) {
    reach("selcount");
    const std::vector<ObjectId> sel = selectionFor(a, c->id);
    if (sel.size() > 1) {
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("and %d more selected"), static_cast<int>(sel.size()) - 1);
      const float tw = uiFont->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, b).x;
      if (rx - tw - 12 > x + 60) {
        rx -= tw;
        dl->AddText(uiFont, theme::kFontCaption, ImVec2(rx, topForBaseline(uiFont, theme::kFontCaption, base)), P.textDim, b);
        rx -= 16;
      }
    }
  }
  // Left side, after the name (or under it): the file, what it is, and an orange pill when several
  // clips share the sample; on the name's baseline.
  const float room = rx - infoX - 8;
  const float cfs = theme::kFontCaption, infoTop = topForBaseline(uiFont, cfs, infoBase);
  char pill[64] = "";
  if (shared > 1) std::snprintf(pill, sizeof pill, i18n::tr("shared by %d clips"), shared);
  const float pw = pill[0] ? uiFont->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, pill).x + 16 : 0.0f;
  const bool pillFits = pill[0] && pw + 8 + 60 < room;
  float ix = infoX, infoRoom = std::max(0.0f, pillFits ? room - pw - 8 : room);
  const float fw = std::min(infoRoom, uiFont->CalcTextSizeA(cfs, 1e9f, 0, info.c_str()).x);
  theme::textEllipsis(dl, uiFont, cfs, ImVec2(ix, infoTop), infoRoom, theme::mix(P.text, P.textDim, 0.35f), info.c_str());
  ix += fw;
  if (!meta.empty() && infoRoom - fw > 40) {  // the file's rate, channels and length, quieter
    const char* dot = "  \xC2\xB7  ";
    const std::string rest = dot + meta;
    theme::textEllipsis(dl, uiFont, cfs, ImVec2(ix, infoTop), infoRoom - fw, P.textDim, rest.c_str());
    ix += std::min(infoRoom - fw, uiFont->CalcTextSizeA(cfs, 1e9f, 0, rest.c_str()).x);
  }
  if (pillFits) {
    const float px = std::round(ix + 8), pcy = std::round(infoBase - uiFont->GetFontBaked(cfs)->Ascent * 0.36f);
    const ImU32 orange = sampleOrange();
    dl->AddRectFilled(ImVec2(px, pcy - 9), ImVec2(px + pw, pcy + 9), theme::withAlpha(orange, 0.12f), 9.0f);
    dl->AddRect(ImVec2(px, pcy - 9), ImVec2(px + pw, pcy + 9), theme::withAlpha(orange, 0.7f), 9.0f);
    dl->AddText(uiFont, theme::kFontLabel, ImVec2(px + 8, topForBaseline(uiFont, theme::kFontLabel, infoBase)), orange, pill);
  }
}

struct ToolDef {
  ui::Icon icon;
  const char* name;
  const char* desc;
  const char* key;
  uint32_t role;
};
const ToolDef kTools[TCount] = {
    {ui::Icon::ToolSelect, i18n::N_("Pointer"), i18n::N_("drag the edges to trim, the body with Ctrl+Shift to slip, the white squares to fade, the dashed line for the volume; drag elsewhere to select time"), nullptr, theme::role::kSelect},
    {ui::Icon::Stretch, i18n::N_("Warp"), i18n::N_("click to add a marker, drag one to stretch the audio around it (Alt: off the grid), Shift+drag slides the audio under it, double-click removes it"), "wave.warpMode", theme::role::kWarp},
    {ui::Icon::ToolSlip, i18n::N_("Slip"), i18n::N_("drag to move the audio inside the clip"), nullptr, theme::role::kSlip},
    {ui::Icon::FadeIn, i18n::N_("Fade"), i18n::N_("drag from inside over the start or the end: a fade that long"), nullptr, theme::role::kWarp},
    {ui::Icon::ToolSlice, i18n::N_("Slice"), i18n::N_("click to cut the clip in two there"), nullptr, theme::role::kSlice},
    {ui::Icon::ToolDraw, i18n::N_("Pencil"), i18n::N_("click or drag to set the volume line"), nullptr, theme::role::kDraw},
    {ui::Icon::Scrub, i18n::N_("Scrub"), i18n::N_("drag to hear the audio under the pointer"), nullptr, theme::role::kPlay},
    {ui::Icon::ToolZoom, i18n::N_("Zoom"), i18n::N_("click zooms in, right-click zooms out (Ctrl+wheel anywhere)"), nullptr, theme::role::kZoom},
};

void editAudioButton(App& a, State& S, const Clip* c, const Channel& ch, ImVec2 at, float h);
void analysisReadout(App& a, State& S, const Clip* c, const Channel& ch, float x0, float x1, float cy);
void bandTools(App& a, State& S, const Clip* cp, const Channel& ch, ImVec2 p0, float w, float h);
// (dev "clipeditor toolrow") the item just drawn, one of the tools row's
void toolItem(State& S) { S.toolRects.emplace_back(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }

// The tools row: with a clip, the editor's tools, Wave / Spectrum, Bars / Time, snap, preview and
// Follow; then the live analysis of the clip (the selection when there is one) and Edit audio...
// Too narrow for all of them (a narrow window or pane): the band's row instead, the view's options
// (Wave / Spectrum, Bars / Time, the snap grid, Follow) in its view menu.
void toolStrip(App& a, State& S, const Clip* cp, const Channel& ch, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (gTrkSkin) trackerview::bevelBox(dl, p0, ImVec2(p0.x + w, p0.y + h), false);  // (the Tracker: a raised bevel strip)
  else dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), P.surface900);
  S.toolRects.clear();
  S.toolA = p0, S.toolB = ImVec2(p0.x + w, p0.y + h);
  S.toolMenu = false;
  const float bs = std::min(24.0f, h - 6);
  const float editW = ui::TextButtonWidth("Edit audio...");
  const float editX = std::round(p0.x + w - 12 - editW);
  if (cp) {  // the whole row's width (as drawn below), against the room left of Edit audio...
    const float pad = ImGui::GetStyle().FramePadding.x;
    auto segW = [&](const char* l0, const char* l1) {
      return 2 * (std::max(ImGui::CalcTextSize(i18n::tr(l0)).x, ImGui::CalcTextSize(i18n::tr(l1)).x) + 2 * pad) + 4;
    };
    const float full = 8 + TCount * bs + (TCount - 1) * 2 + 10 + segW("Wave", "Spectrum") + 8 + segW("Bars", "Time") + 8 + bs + 4 + 90 + 10 + bs + 2 + bs + 6 +
                       ui::TextButtonWidth("Follow");
    if (p0.x + full + 16 > editX) {
      S.toolMenu = true;
      const float bh = std::min(h, 24.0f);
      bandTools(a, S, cp, ch, ImVec2(p0.x + 8, p0.y + std::round((h - bh) * 0.5f)), w - 20, bh);
      S.toolA = p0, S.toolB = ImVec2(p0.x + w, p0.y + h);
      return;
    }
  }
  editAudioButton(a, S, cp, ch, ImVec2(editX, p0.y + std::round((h - bs) * 0.5f)), bs);
  if (!cp) {
    analysisReadout(a, S, nullptr, ch, p0.x + 12, editX - 16, p0.y + h * 0.5f);
    return;
  }
  const Clip& c = *cp;
  for (const char* k : {"tools", "view", "units", "snap", "preview", "follow"}) reach(k);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 8, p0.y + (h - bs) * 0.5f));
  for (int t = 0; t < TCount; ++t) {
    ImGui::PushID(t);
    if (ui::ToolButton("##cet", kTools[t].icon, S.tool == t, bs, t == TWarp ? warpwave::modeWord() : kTools[t].name, kTools[t].desc, kTools[t].key, theme::rgb(kTools[t].role))) S.tool = t;
    toolItem(S);
    ImGui::PopID();
    ImGui::SameLine(0, 2);
  }
  ImGui::SameLine(0, 10);
  const float sh = bs;
  {
    const char* views[] = {i18n::tr("Wave"), i18n::tr("Spectrum")};
    int v = S.spectrum ? 1 : 0;
    if (ui::Segmented("##cewave", views, 2, &v, 0, sh)) S.spectrum = v == 1;
    toolItem(S);
  }
  ImGui::SameLine(0, 8);
  {
    const char* units[] = {i18n::tr("Bars"), i18n::tr("Time")};
    int v = S.seconds ? 1 : 0;
    if (ui::Segmented("##ceunits", units, 2, &v, 0, sh)) S.seconds = v == 1;
    toolItem(S);
  }
  ImGui::SameLine(0, 8);
  {
    const char* snaps[] = {i18n::tr("Off"), i18n::tr("Bar"), i18n::tr("Beat"), i18n::tr("1/2 beat"), i18n::tr("1/4 beat")};
    if (ui::IconButton("##cesnapon", ui::Icon::Magnet, S.snap != 0, sh, "Snap: edges, fades and warp markers land on the grid (Alt while dragging: free)"))
      S.snap = S.snap ? 0 : 2;
    toolItem(S);
    ImGui::SameLine(0, 4);
    const bool snapOpen = ui::BeginCombo("##cesnap", snaps[std::clamp(S.snap, 0, 4)], 90);
    toolItem(S);
    if (snapOpen) {
      for (int i = 0; i < 5; ++i)
        if (ImGui::Selectable(snaps[i], S.snap == i)) S.snap = i;
      ImGui::EndCombo();
    }
  }
  ImGui::SameLine(0, 10);
  if (ui::IconButton("##ceplay", a.engine.previewing() ? ui::Icon::Stop : ui::Icon::Play, a.engine.previewing(), sh,
                     trackerpages::active(a) ? keys::withKeys(i18n::N_("Preview: the clip alone (its channel and effects), from its start (%s)"), {keys::fixedKey("Alt+P")})
                                             : i18n::N_("Preview: the clip alone (its channel and effects), from its start")))
    previewClip(a, c.id, S.loopPreview);
  toolItem(S);
  ImGui::SameLine(0, 2);
  if (ui::IconButton("##celoop", ui::Icon::Loop, S.loopPreview, sh, "Loop the preview")) S.loopPreview = !S.loopPreview;
  toolItem(S);
  ImGui::SameLine(0, 6);
  if (ui::TextButton("Follow", S.follow, 0, "The view follows the song position while the song plays")) S.follow = !S.follow;
  toolItem(S);
  // The live analysis between the tools and Edit audio... (as many readings as fit).
  analysisReadout(a, S, cp, ch, ImGui::GetItemRectMax().x + 24, editX - 16, p0.y + h * 0.5f);
  (void)dl;
}

// ------------------------------------------------------------------ the waveform view

struct Geo {
  float x0 = 0, x1 = 0;
  double t0 = 0, t1 = 0;
  float px = 1;  // per tick
  float xOf(double t) const { return x0 + static_cast<float>((t - t0) * px); }
  double tOf(float x) const { return t0 + static_cast<double>(x - x0) / px; }
};

// The whole sample around the clip (drawn dimmed, and in the overview): the clip's first pass as
// if all its audio played, from the channel's start handle to its end handle (ticks [t0, t1)).
// It lines up with the clip: the audio the clip plays sits under the clip.
struct Ghost {
  bool ok = false;
  Clip clip;
  double t0 = 0, t1 = 0;
};
Ghost ghostOf(const Song& s, const TempoMap& tm, const std::vector<AudibleSpan>& spans) {
  Ghost g;
  if (spans.empty()) return g;
  const Clip& first = spans.front().pass;  // (a looping clip: the pass at its start, from its offset in the loop)
  Clip all = first;
  all.audioStart = 0, all.audioEnd = 0;
  double lo = 0, hi = 0;
  if (!clipWindow(s, all, nullptr, lo, hi)) return g;  // the handles' window, the clip's direction
  const double t0 = clipTickOfSource(s, first, tm, lo), t1 = clipTickOfSource(s, first, tm, hi);
  if (!std::isfinite(t0) || !std::isfinite(t1) || !(t1 > t0 + 1)) return g;
  g.clip = all;
  g.clip.audioStart = lo;
  g.clip.loop = false;
  g.clip.fadeIn = g.clip.fadeOut = 0;
  g.clip.fadeInManual = g.clip.fadeOutManual = true;
  g.clip.start = static_cast<Tick>(std::floor(t0));
  g.clip.length = std::max<Tick>(1, static_cast<Tick>(std::ceil(t1 - t0)) + 1);
  g.t0 = t0, g.t1 = t1, g.ok = true;
  return g;
}

// The view that shows the whole clip and some of the audio around it; a clip much longer than its
// audio (its audio ends before 60 % of it): the audio and a little of the silence after it (the
// rest of the clip a zoom out away), so the sound fills the view.
void fitView(const Song& s, const Clip& c, State& S, const TempoMap& tm) {
  double len = static_cast<double>(std::max<Tick>(1, s.clipLength(c)));
  const std::vector<AudibleSpan> spans = clipAudibleSpans(s, c, tm);
  bool toAudio = false;
  if (!spans.empty()) {
    double last = static_cast<double>(c.start);
    for (const AudibleSpan& sp : spans) last = std::max(last, sp.t1);
    const double heard = last - static_cast<double>(c.start);
    if (heard > 1 && heard < len * 0.6) len = heard * 1.08, toAudio = true;
  }
  double pre = 0, post = 0;
  if (const Ghost gh = ghostOf(s, tm, spans); gh.ok) {
    pre = std::clamp(static_cast<double>(c.start) - gh.t0, 0.0, len * 0.25);
    if (!toAudio) post = std::clamp(gh.t1 - (static_cast<double>(c.start) + len), 0.0, len * 0.25);
  }
  const double pad = std::max(len * 0.02, 4.0);
  S.t0 = static_cast<double>(c.start) - pre - pad;
  S.t1 = static_cast<double>(c.start) + len + post + pad;
  S.fitted = true;
}

void ruler(App& a, const Song& s, const State& S, const Geo& g, const TempoMap& tm, ImVec2 p0, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(g.x0, p0.y), ImVec2(g.x1, p0.y + h), P.dark ? P.field : P.surface950);
  dl->AddLine(ImVec2(g.x0, p0.y + h - 0.5f), ImVec2(g.x1, p0.y + h - 0.5f), P.line);
  ImFont* f = livelook::numFont(a);
  const float fs = theme::kFontCaption, ty = std::round(p0.y + (h - 9 - fs) * 0.5f) + 1;  // (the numbers above the ticks)
  if (S.seconds) {
    const double s0 = tm.secondsAt(std::max(0.0, g.t0)), s1 = tm.secondsAt(std::max(0.0, g.t1));
    double step = 0.1;
    for (double st : {0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0}) {
      step = st;
      if ((s1 - s0) / st < (g.x1 - g.x0) / 70.0) break;
    }
    for (double t = std::ceil(s0 / step) * step; t <= s1; t += step) {
      const float x = std::round(g.xOf(tm.tickAtSeconds(t))) + 0.5f;
      dl->AddLine(ImVec2(x, p0.y + h - 8), ImVec2(x, p0.y + h), P.lineStrong);
      char b[32];
      std::snprintf(b, sizeof b, "%d:%05.2f", static_cast<int>(t / 60), std::fmod(t, 60.0));
      dl->AddText(f, fs, ImVec2(x + 4, ty), P.text, b);
    }
    return;
  }
  const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks());
  const int every = std::max(1, static_cast<int>(std::ceil(40.0 / std::max(1e-6, static_cast<double>(g.px) * static_cast<double>(bar)))));
  const bool beats = g.px * static_cast<float>(beat) >= 12.0f;
  for (Tick t = std::max<Tick>(0, static_cast<Tick>(std::floor(g.t0 / static_cast<double>(beat))) * beat); static_cast<double>(t) <= g.t1; t += beat) {
    const float x = std::round(g.xOf(static_cast<double>(t))) + 0.5f;
    const bool isBar = t % bar == 0;
    if (!isBar && !beats) continue;
    dl->AddLine(ImVec2(x, p0.y + h - (isBar ? 9.0f : 4.0f)), ImVec2(x, p0.y + h), isBar ? P.lineStrong : P.line);
    if (isBar && (t / bar) % every == 0) {
      const std::string lbl = std::to_string(t / bar + 1);
      dl->AddText(f, fs, ImVec2(x + 4, ty), P.text, lbl.c_str());
    }
  }
}

// The whole sample once, at the file's own scale: its waveform, the sample's blue start and end
// handles (they trim the sample for its notes and every clip of it, the channel settings' two
// handles), the notes' loop bar and, with a clip, the part of the file the view shows (the orange
// window; drag it to move the view). Also the drop target for another file. `big`: the window of a
// sampler channel no clip plays, where it takes the waveform's place.
// A whole file's waveform from x0 to x1, one column per pixel (the column's lowest and highest value).
// `fit`: scaled so its loudest column fills the height (a small overview of a quiet file stays readable).
void fileWave(ImDrawList* dl, const Sample& smp, float x0, float y0, float x1, float y1, ImU32 col, bool fit = false) {
  const size_t n = smp.frames();
  if (n == 0) return;
  const int w = std::max(1, static_cast<int>(x1 - x0));
  const float mid = (y0 + y1) * 0.5f, half = (y1 - y0) * 0.48f;
  static std::vector<std::pair<float, float>> cols;
  cols.assign(static_cast<size_t>(w), {0.0f, 0.0f});
  float peak = 0;
  for (int x = 0; x < w; ++x) {
    const size_t a0 = n * static_cast<size_t>(x) / static_cast<size_t>(w);
    const size_t a1 = std::max(a0 + 1, n * static_cast<size_t>(x + 1) / static_cast<size_t>(w));
    float lo = 0, hi = 0;
    for (size_t i = a0; i < a1 && i < n; i += std::max<size_t>(1, (a1 - a0) / 48)) {
      const float v = (smp.left[i] + smp.right[i]) * 0.5f;
      lo = std::min(lo, v), hi = std::max(hi, v);
    }
    cols[static_cast<size_t>(x)] = {lo, hi};
    peak = std::max(peak, std::max(hi, -lo));
  }
  const float k = fit && peak > 1e-4f ? std::min(8.0f, 1.0f / peak) : 1.0f;
  for (int x = 0; x < w; ++x) {
    const auto [lo, hi] = cols[static_cast<size_t>(x)];
    dl->AddLine(ImVec2(x0 + static_cast<float>(x) + 0.5f, mid - hi * k * half), ImVec2(x0 + static_cast<float>(x) + 0.5f, mid - lo * k * half + 1), col);
  }
}

struct ViewOnFile {
  bool on = false;
  double f0 = 0, f1 = 0;                     // the view's window, fractions of the file
  std::function<void(double frac)> centreAt;  // moves the view to show that part of the file
};
void sampleStrip(App& a, State& S, const Channel& ch, ImVec2 o0, ImVec2 o1, bool big, const ViewOnFile& view) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(o0, o1, big ? (P.dark ? P.surface950 : P.paper) : P.surface900);
  dl->AddLine(ImVec2(o0.x, o0.y + 0.5f), ImVec2(o1.x, o0.y + 0.5f), P.line);
  const float capW = big ? 12.0f : 66.0f;
  if (!big) {  // its caption, as the cards' titles; the strip itself a well a step down
    ImFont* bf = app().fontBold ? app().fontBold : ImGui::GetFont();
    dl->AddText(bf, theme::kFontLabel, ImVec2(o0.x + 12, std::round((o0.y + o1.y - theme::kFontLabel) * 0.5f)), P.textDim, i18n::tr("SAMPLE"));
    if (gTrkSkin) trackerview::bevelBox(dl, ImVec2(o0.x + capW - 4, o0.y + 3), ImVec2(o1.x - 4, o1.y - 2), true);  // (the Tracker: a sunken bevel well)
    else dl->AddRectFilled(ImVec2(o0.x + capW - 4, o0.y + 4), ImVec2(o1.x - 4, o1.y - 3), P.dark ? P.surface950 : P.well, 3.0f);
  }
  const float x0 = o0.x + capW, x1 = o1.x - (big ? 12.0f : 8.0f);
  const float y0 = o0.y + (big ? 22.0f : 5.0f), y1 = o1.y - (big ? 12.0f : 4.0f);
  ImGui::SetCursorScreenPos(o0);
  ImGui::InvisibleButton("##cesmp", ImVec2(std::max(1.0f, o1.x - o0.x), std::max(1.0f, o1.y - o0.y)));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  reach("drop");
  if (ImGui::BeginDragDropTarget()) {  // another file for the sample (every clip of it and its notes)
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      const std::string p(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(p)) browserui::loadChannelSample(a, ch.id, p, "load sample");
    }
    ImGui::EndDragDropTarget();
  }
  const SamplePtr smp = ch.plugin.data.empty() ? nullptr : sampleCache().request(samplePath(ch));
  reach("handles");
  reach("loopbar");
  if (!smp || smp->frames() == 0) {
    const char* msg = ch.plugin.data.empty() ? i18n::tr("Drop an audio file here, or Sample > Replace sample...") : i18n::tr("Reading the sample...");
    const ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(std::round((x0 + x1 - ts.x) * 0.5f), std::round((y0 + y1 - ts.y) * 0.5f)), P.textFaint, msg);
    return;
  }
  const ImU32 clipCol = theme::content(ch.color);
  const ImU32 waveCol = big ? (P.dark ? theme::mix(clipCol, P.text, 0.30f) : theme::mix(clipCol, P.ink, 0.30f))
                            : (P.dark ? theme::mix(clipCol, P.text, 0.25f) : theme::mix(clipCol, P.ink, 0.25f));
  if (!big) dl->AddLine(ImVec2(x0, std::round((y0 + y1) * 0.5f) + 0.5f), ImVec2(x1, std::round((y0 + y1) * 0.5f) + 0.5f), theme::withAlpha(waveCol, 0.35f));
  fileWave(dl, *smp, x0, y0, x1, y1, waveCol, true);  // (its loudest part fills the height, big or small)
  const double secs = smp->seconds();
  double sf = 0, ef = 1;
  channelTrim(ch, sf, ef);
  auto xOf = [&](double f) { return x0 + static_cast<float>(f) * (x1 - x0); };
  const float hx0 = std::round(xOf(sf)), hx1 = std::round(xOf(ef));
  dl->AddRectFilled(ImVec2(x0, o0.y + 1), ImVec2(hx0, o1.y), theme::withAlpha(P.surface950, 0.6f));
  dl->AddRectFilled(ImVec2(hx1, o0.y + 1), ImVec2(x1, o1.y), theme::withAlpha(P.surface950, 0.6f));
  // The notes' loop (Playback), when on: a bar along the bottom.
  if (smpPar(ch, pp::SmpLoop, 0) >= 0.5f) {
    const float lx0 = xOf(smpPar(ch, pp::SmpLoopStart, 0)), lx1 = xOf(smpPar(ch, pp::SmpLoopEnd, 1));
    dl->AddRectFilled(ImVec2(lx0, o1.y - (big ? 7.0f : 4.0f)), ImVec2(lx1, o1.y - 1), theme::withAlpha(P.hot, 0.85f), 1.5f);
    if (big) dl->AddText(ImGui::GetFont(), theme::kFontLabel - 1, ImVec2(lx0 + 3, o1.y - 20), P.hot, i18n::tr("LOOP"));
  }
  // The view's window (with a clip).
  if (view.on && view.f1 > view.f0) {
    const float vx0 = std::max(x0, xOf(view.f0)), vx1 = std::min(x1, xOf(view.f1));
    if (vx1 - vx0 < x1 - x0 - 2) dl->AddRect(ImVec2(vx0, o0.y + 2), ImVec2(vx1, o1.y - 1), sampleOrange(), 2.0f, 0, 1.5f);
  }
  // The handles: the nearer one (8 px) moves on a drag, kept apart from the other.
  const ImGuiIO& io = ImGui::GetIO();
  int near = -1;
  if (hov || act) {
    const float ds = std::fabs(io.MousePos.x - hx0), de = std::fabs(io.MousePos.x - hx1);
    if (std::min(ds, de) <= 8.0f) near = ds <= de ? 0 : 1;
  }
  if (ImGui::IsItemActivated()) S.hdrag = near;
  if (act && S.hdrag >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.5f)) {
    const double v = clampTrimHandle(S.hdrag == 1, (io.MousePos.x - x0) / std::max(1.0f, x1 - x0), S.hdrag == 1 ? sf : ef, secs);
    a.doc.setParam(smpPid(ch.id, S.hdrag == 1 ? pp::SmpEnd : pp::SmpStart), v, true);
  } else if (act && S.hdrag < 0 && view.on && view.centreAt) {
    view.centreAt(std::clamp((io.MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f));
  }
  if (ImGui::IsItemDeactivated()) {
    if (S.hdrag >= 0) a.doc.endGesture();
    S.hdrag = -1;
  }
  if (near >= 0 || (act && S.hdrag >= 0)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  const ImU32 blue = handleBlue();
  for (int k = 0; k < 2; ++k) {
    const float x = k ? hx1 : hx0;
    const bool lit = near == k || (act && S.hdrag == k);
    dl->AddLine(ImVec2(x, o0.y + 1), ImVec2(x, o1.y), blue, lit ? 2.5f : 1.5f);
    const float tw = big ? 9.0f : 7.0f, th = big ? 11.0f : 8.0f;
    if (k == 0) dl->AddTriangleFilled(ImVec2(x, o0.y + 1), ImVec2(x + tw, o0.y + 1), ImVec2(x, o0.y + 1 + th), blue);
    else dl->AddTriangleFilled(ImVec2(x, o0.y + 1), ImVec2(x - tw, o0.y + 1), ImVec2(x, o0.y + 1 + th), blue);
  }
  if (big) {
    char b[160];
    std::snprintf(b, sizeof b, "%s  %.3f s  \xC2\xB7  %s  %.3f s  \xC2\xB7  %.2f s", i18n::tr("START"), sf * secs, i18n::tr("END"), ef * secs, secs);
    dl->AddText(a.fontMono ? a.fontMono : ImGui::GetFont(), theme::kFontLabel, ImVec2(x0 + 14, o0.y + 5), P.textDim, b);
  }
  if (hov)
    a.setHint(view.on ? i18n::tr("The whole sample: drag the blue handles to set where the sample starts and ends (its notes and every clip of it); drag elsewhere to move the view")
                      : i18n::tr("The whole sample: drag the blue handles to set where it starts and ends for its notes; drop a file to replace it"));
}

// The loudest the sample gets between its start and end handles (0..1; 0 while it decodes).
float samplePeak(const Channel& ch) {
  const ClipSource src = clipSourceIfReady(ch);
  if (!src.sample || src.sample->frames() < 2) return 0.0f;
  const PeakSummary& pk = livetake::samplePeaks(src.sample);
  if (pk.bins() == 0) return 0.0f;
  double sf = 0, ef = 1;
  channelTrim(ch, sf, ef);
  const size_t n = pk.bins();
  const size_t b0 = std::min(n - 1, static_cast<size_t>(std::floor(sf * static_cast<double>(n))));
  const size_t b1 = std::clamp(static_cast<size_t>(std::ceil(ef * static_cast<double>(n))), b0 + 1, n);
  float lo = 0, hi = 0;
  pk.range(b0, b1, lo, hi);
  return std::max(hi, -lo);
}

// ------------------------------------------------------------------ the spectrum
// Drawn from images of the sample made for the pixels they are shown on (Loupe's analysis,
// packs/edison/edison_specimage.h): one column per screen pixel or finer at the view's zoom, one
// row per pixel on a log-frequency axis, made on a worker. An image covers a stretch of the sample
// and is drawn, smoothly filtered, through where the clip plays each part of it (its passes, its
// stretch and warp markers, backwards when reversed), so every pass and the dimmed audio around
// the clip share one image. The whole sample's image stands in until the view's own is made (once
// the view has rested a moment); the view's image reaches a margin past each side, so scrolling a
// little makes nothing new. Idle frames make nothing: an image is made only when the sample, the
// zoom, the view's size or Loupe's level settings change.
struct ClipSpec {
  ed::SpecLayer base, view;
  ed::SpecViewRequest want;
  double wantSince = 0;
  std::weak_ptr<const Sample> smp;
  uint64_t serial = 0;
  struct Seg {
    float x0 = 0, x1 = 0;
    double f0 = 0, f1 = 0;  // sample frames at x0 and x1
    bool ghost = false;     // the audio around the clip (dimmed)
  };
  std::vector<Seg> segs;
  std::vector<double> segKey;  // what the runs were worked out for
  int segBuilds = 0;
  // (dev "clipeditor spec") the last frame's
  float pxW = 0, pxH = 0, ptW = 0, ptH = 0, stretchX = 0, stretchY = 0;
  bool current = false;
};
std::unordered_map<const State*, ClipSpec> gClipSpec;

// The ramps: Loupe's colours over the editor's own background (the floor clear), and for the
// light theme the same order of lightness turned around (pale to dark).
const ed::SpecRamp& clipSpecRamp(bool dark) {
  static const ed::SpecRamp d = [] {
    const std::pair<double, uint32_t> stops[] = {{0.00, 0x1C2124}, {0.16, 0x1D3237}, {0.36, 0x1E5A4B}, {0.55, 0x3E9466},
                                                 {0.71, 0x79C48C}, {0.85, 0xE8C46A}, {0.95, 0xFBE6B4}, {1.00, 0xFFFFFF}};
    return ed::makeSpecRamp(stops, std::size(stops), 0.2);
  }();
  static const ed::SpecRamp l = [] {
    const std::pair<double, uint32_t> stops[] = {{0.00, 0xE6EDEA}, {0.16, 0xD3E3DD}, {0.36, 0x9CCAB6}, {0.55, 0x4E9E7C},
                                                 {0.71, 0x2A6E57}, {0.85, 0x8A4A14}, {0.95, 0x4A2008}, {1.00, 0x1A0A04}};
    return ed::makeSpecRamp(stops, std::size(stops), 0.2);
  }();
  return dark ? d : l;
}

// One image: frames [r.from, r.to) of the sample (a copy of just that stretch and the analysis
// windows' reach, made on the worker).
ed::SpecView clipSpecImage(const SamplePtr& smp, const ed::SpecViewRequest& r, const std::atomic<bool>* cancel) {
  const int64_t n = static_cast<int64_t>(smp->frames());
  const int64_t pad = static_cast<int64_t>(smp->sampleRate * 1.7) + 4096;  // (the longest window at its sharpest in frequency)
  const int64_t lo = std::clamp<int64_t>(r.from - pad, 0, n), hi = std::clamp<int64_t>(r.to + pad, lo, n);
  ed::Audio au;
  au.rate = smp->sampleRate;
  const bool stereo = !smp->right.shared() && smp->right.vec().size() == smp->left.size();
  au.channels = stereo ? 2 : 1;
  au.L.assign(smp->left.begin() + lo, smp->left.begin() + hi);
  if (stereo) au.R.assign(smp->right.begin() + lo, smp->right.begin() + hi);
  ed::SpecViewRequest q = r;
  q.from -= lo, q.to -= lo;
  ed::SpecView v = ed::analyseSpecView(au, q, cancel);
  v.req = r;
  return v;
}

void drawClipSpectrum(App& a, State& S, ImDrawList* dl, const Song& s, const TempoMap& tm, const Channel& ch, const Clip& c, const Geo& g,
                      const std::vector<AudibleSpan>& spans, const Ghost& gh, ImVec2 w0, ImVec2 w1, float wy0, float wy1) {
  ClipSpec& cs = gClipSpec[&S];
  const ClipSource src = clipSourceIfReady(ch);
  if (!src.sample || src.sample->frames() < 2 || src.sample->sampleRate <= 0) return;
  const SamplePtr smp = src.sample;
  if (cs.smp.lock() != smp) cs.smp = smp, ++cs.serial;
  const double rate = smp->sampleRate, n = static_cast<double>(smp->frames());
  const double last = (n - 1) / rate;
  const bool rev = clipPlaysReversed(ch, c);
  ImGuiViewport* vp = ImGui::GetWindowViewport();
  const float fb = std::max({1.0f, ImGui::GetIO().DisplayFramebufferScale.y, vp ? vp->FramebufferScale.y : 1.0f});

  // Where the clip sounds, as runs of pixels each playing a straight stretch of the sample: worked
  // out again only when the song, the view or the clip's mapping changed.
  {
    std::vector<double> key = {static_cast<double>(a.doc.revision()), static_cast<double>(cs.serial), g.t0, g.t1, w0.x, w1.x, fb, rev ? 1.0 : 0.0,
                               gh.ok ? gh.t0 : -1, gh.ok ? gh.t1 : -1, static_cast<double>(spans.size())};
    for (const AudibleSpan& sp : spans) key.insert(key.end(), {sp.t0, sp.t1, sp.s0, sp.s1, clipSourceAt(s, sp.pass, tm, (sp.t0 + sp.t1) * 0.5)});
    if (key != cs.segKey) {
      cs.segKey = std::move(key);
      ++cs.segBuilds;
      cs.segs.clear();
      auto walk = [&](double ta, double tb, bool ghost, const std::function<double(double)>& secAt) {
        const float xa = std::max(w0.x, g.xOf(ta)), xb = std::min(w1.x, g.xOf(tb));
        if (!(xb - xa > 0.01f)) return;
        auto frameAt = [&](float x) {
          double sec = secAt(std::clamp(g.tOf(x), ta, tb));
          if (rev) sec = last - sec;
          return std::clamp(sec, 0.0, last) * rate;
        };
        float x = xa;
        double f = frameAt(xa);
        bool open = false;
        ClipSpec::Seg cur;
        while (x < xb) {
          float nx = std::floor(x) + 1.0f;  // (a point at a time: a mapping that bends is followed closely)
          if (nx > xb || xb - nx < 0.01f) nx = xb;
          const double nf = frameAt(nx);
          const double sc = open ? (cur.f1 - cur.f0) / static_cast<double>(cur.x1 - cur.x0) : 0, sn = (nf - f) / static_cast<double>(nx - x);
          if (open && std::abs(sn - sc) <= 1e-4 * std::max(std::abs(sc), std::abs(sn)) + 1e-9) {
            cur.x1 = nx, cur.f1 = nf;
          } else {
            if (open) cs.segs.push_back(cur);
            cur = ClipSpec::Seg{x, nx, f, nf, ghost};
            open = true;
          }
          x = nx, f = nf;
        }
        if (open) cs.segs.push_back(cur);
      };
      if (gh.ok) {  // around the clip: the ghost's audio, where no pass plays
        double t = gh.t0;
        for (const AudibleSpan& sp : spans) {
          if (sp.t0 > t) walk(t, std::min(sp.t0, gh.t1), true, [&](double tt) { return clipSourceAt(s, gh.clip, tm, tt); });
          t = std::max(t, sp.t1);
        }
        if (t < gh.t1) walk(t, gh.t1, true, [&](double tt) { return clipSourceAt(s, gh.clip, tm, tt); });
      }
      for (const AudibleSpan& sp : spans)
        walk(sp.t0, sp.t1, false, [&](double tt) { return std::clamp(clipSourceAt(s, sp.pass, tm, tt), sp.s0, sp.s1); });
    }
  }
  if (cs.segs.empty()) return;

  // What the view asks for: the sample's frames it shows (and a margin), at least one column per
  // screen pixel where the clip plays fastest (quarter-octave steps, rounded up), a row per pixel.
  double lo = 1e300, hi = -1e300, fpp = 1e300;
  for (const ClipSpec::Seg& sg : cs.segs) {
    lo = std::min({lo, sg.f0, sg.f1}), hi = std::max({hi, sg.f0, sg.f1});
    const double px = static_cast<double>(sg.x1 - sg.x0) * fb, df = std::abs(sg.f1 - sg.f0);
    if (px >= 0.5 && df > 1e-6) fpp = std::min(fpp, df / px);
  }
  const int rows = std::clamp(static_cast<int>(std::lround((wy1 - wy0) * fb)), 0, 4096);
  if (rows < 2 || hi - lo < 2) return;
  if (!(fpp < 1e299)) fpp = (hi - lo) / std::max(1.0, static_cast<double>(w1.x - w0.x) * fb);
  const double fq = std::max(1.0, std::exp2(std::floor(std::log2(fpp) * 4.0) / 4.0));
  const int maxCols = std::clamp(8 * 1024 * 1024 / rows, 64, 8192);  // (an image at most 8 M cells: 32 MB)
  const double span = hi - lo, q = std::exp2(std::ceil(std::log2(std::max(256.0, span / 4))));
  double from = std::max(0.0, std::floor((lo - span * 0.25) / q) * q), to = std::min(n, std::ceil((hi + span * 0.25) / q) * q);
  if ((to - from) / fq > maxCols) from = std::max(0.0, std::floor(lo)), to = std::min(n, std::ceil(hi));
  ed::SpecViewRequest want;
  want.from = static_cast<int64_t>(from), want.to = std::max(want.from + 2, static_cast<int64_t>(to));
  want.cols = static_cast<int>(std::clamp(std::ceil(static_cast<double>(want.to - want.from) / fq), 2.0, static_cast<double>(maxCols)));
  want.rows = rows, want.minHz = ed::kSpecMinHz, want.resolution = ed::specResolution(a);
  ed::SpecViewRequest whole;
  whole.from = 0, whole.to = static_cast<int64_t>(n), whole.cols = static_cast<int>(std::min<int64_t>(2048, std::max<int64_t>(2, whole.to / 8)));
  whole.rows = std::min(rows, 768), whole.minHz = ed::kSpecMinHz, whole.resolution = want.resolution;
  const double now = ImGui::GetTime();
  if (want != cs.want) cs.want = want, cs.wantSince = now;
  const bool rested = now - cs.wantSince >= 0.12 || cs.view.img.empty() || cs.view.imgSerial != cs.serial;
  const bool viewIsWhole = want.from <= 0 && want.to >= whole.to;
  const ed::SpecMaker make = [smp](const ed::SpecViewRequest& r, const std::atomic<bool>* cancel) { return clipSpecImage(smp, r, cancel); };
  ed::specStep(cs.base, whole, cs.serial, !viewIsWhole, make, "gl-clip-spec");
  ed::specStep(cs.view, want, cs.serial, rested && (cs.base.imgSerial == cs.serial || !cs.base.job.valid()), make, "gl-clip-spec");
  const auto& P = pal();
  const int range = ed::specRange(a), look = P.dark ? 1 : 2;
  const bool viewWasDirty = cs.view.dirty;
  ed::specUpload(cs.base, range, clipSpecRamp(P.dark), look);
  ed::specUpload(cs.view, range, clipSpecRamp(P.dark), look);
  cs.current = cs.view.tex && cs.view.imgSerial == cs.serial && cs.view.img.req == want;
  if (viewWasDirty && cs.current) devsig::reach("clip-spectrogram");  // dev checks

  // Draws the part of each run inside the layer's frames [a0, a1).
  auto drawLayer = [&](const ed::SpecLayer& L, double a0, double a1) {
    if (!L.tex || L.img.empty() || L.imgSerial != cs.serial || !(a1 > a0)) return;
    const double lf0 = static_cast<double>(L.img.req.from), lf1 = static_cast<double>(L.img.req.to);
    const ImTextureRef tex(static_cast<ImTextureID>(L.tex));
    for (const ClipSpec::Seg& sg : cs.segs) {
      double u0 = 0, u1 = 1;
      if (std::abs(sg.f1 - sg.f0) > 1e-9) {
        const double ua = (a0 - sg.f0) / (sg.f1 - sg.f0), ub = (a1 - sg.f0) / (sg.f1 - sg.f0);
        u0 = std::max(0.0, std::min(ua, ub)), u1 = std::min(1.0, std::max(ua, ub));
      } else if (sg.f0 < a0 || sg.f0 >= a1) {
        continue;
      }
      if (u1 <= u0) continue;
      const float xa = sg.x0 + static_cast<float>(u0) * (sg.x1 - sg.x0), xb = sg.x0 + static_cast<float>(u1) * (sg.x1 - sg.x0);
      if (xb - xa < 0.01f) continue;
      const double fa = sg.f0 + (sg.f1 - sg.f0) * u0, fbr = sg.f0 + (sg.f1 - sg.f0) * u1;
      dl->AddImage(tex, ImVec2(xa, wy0), ImVec2(xb, wy1), ImVec2(static_cast<float>((fa - lf0) / (lf1 - lf0)), 0),
                   ImVec2(static_cast<float>((fbr - lf0) / (lf1 - lf0)), 1), sg.ghost ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE);
    }
  };
  // The whole sample's image where the view's own does not reach, then the view's.
  const bool haveView = cs.view.tex && cs.view.imgSerial == cs.serial && !cs.view.img.empty();
  const double vf0 = haveView ? static_cast<double>(cs.view.img.req.from) : n + 1, vf1 = haveView ? static_cast<double>(cs.view.img.req.to) : n + 1;
  drawLayer(cs.base, 0, std::min(vf0, n + 1));
  if (haveView) drawLayer(cs.base, vf1, n + 1), drawLayer(cs.view, vf0, vf1);

  // (dev) how many screen pixels each texel of the view's image is drawn across (1 or less: sharp)
  cs.ptW = w1.x - w0.x, cs.ptH = wy1 - wy0;
  cs.pxW = cs.ptW * fb, cs.pxH = cs.ptH * fb;
  cs.stretchX = cs.stretchY = 0;
  if (haveView) {
    const double tpf = static_cast<double>(cs.view.img.req.cols) / static_cast<double>(cs.view.img.req.to - cs.view.img.req.from);
    for (const ClipSpec::Seg& sg : cs.segs) {
      const double px = static_cast<double>(sg.x1 - sg.x0) * fb, texels = std::abs(sg.f1 - sg.f0) * tpf;
      if (!sg.ghost && px >= 2 && texels > 0) cs.stretchX = std::max(cs.stretchX, static_cast<float>(px / texels));
    }
    cs.stretchY = cs.pxH / static_cast<float>(cs.view.img.req.rows);
  }
}

void waveArea(App& a, State& S, Host host, const Clip& c0, const Channel& ch, ImVec2 p0, ImVec2 p1, float ovH) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  static TempoMap tm;
  tm.build(s);
  if (!S.fitted || S.fitClip != c0.id) {
    S.fitClip = c0.id;
    S.selT0 = S.selT1 = 0;
    fitView(s, c0, S, tm);
  }
  // K-391: a selection the Tracker's classic Sample editor handed over (trackerpage::setSelection)
  if (host == Host::Pane && gPendSel.on && gPendSel.clip == c0.id) {
    S.selT0 = gPendSel.t0, S.selT1 = gPendSel.t1;
    gPendSel.on = false;
  }
  const float rulerH = 22.0f;
  const ImVec2 w0(p0.x, p0.y + rulerH), w1(p1.x, p1.y - ovH);
  Geo g;
  g.x0 = p0.x, g.x1 = p1.x, g.t0 = S.t0, g.t1 = S.t1;
  g.px = static_cast<float>((g.x1 - g.x0) / std::max(1e-6, S.t1 - S.t0));
  ruler(a, s, S, g, tm, p0, rulerH);
  S.waveA = w0, S.waveB = w1;
  const Clip* cp = s.clip(c0.id);
  if (!cp) return;
  const Clip& c = plwarp::shown(s, *cp, tm, true, 0.5);  // a dragged marker glides as in the playlist
  const double cs = static_cast<double>(c.start), ce = cs + static_cast<double>(s.clipLength(c));
  const float xs = std::round(g.xOf(cs)), xe = std::round(g.xOf(ce));
  S.xs = xs, S.xe = xe;
  dl->AddRectFilled(w0, w1, P.dark ? P.surface950 : P.paper);
  dl->PushClipRect(w0, w1, true);
  // The grid: bars and beats.
  {
    const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks());
    const bool beats = g.px * static_cast<float>(beat) >= 10.0f;
    for (Tick t = std::max<Tick>(0, static_cast<Tick>(std::floor(g.t0 / static_cast<double>(beat))) * beat); static_cast<double>(t) <= g.t1; t += beat) {
      const bool isBar = t % bar == 0;
      if (!isBar && !beats) continue;
      const float x = std::round(g.xOf(static_cast<double>(t))) + 0.5f;
      dl->AddLine(ImVec2(x, w0.y), ImVec2(x, w1.y), isBar ? P.gridBar : P.gridBeat);
    }
  }
  const ImU32 clipCol = theme::content(ch.color);
  const ImU32 waveCol = P.dark ? theme::mix(clipCol, P.text, 0.30f) : theme::mix(clipCol, P.ink, 0.30f);
  const float wy0 = w0.y + 4, wy1 = w1.y - 4;
  // The waveform drawn so the sample's loudest part fills the height (a quiet sample reads as well
  // as a loud one); the level scale at the left says what each height is, at the clip's gain.
  const float peak = samplePeak(ch);
  const float scale = peak > 1e-4f ? std::min(16.0f, 0.92f / peak) : 1.0f;
  std::vector<std::pair<float, std::string>> levelMarks;  // (drawn over the waveform, at the end)
  if (!S.spectrum) {
    const float mid = (wy0 + wy1) * 0.5f, half = (wy1 - wy0) * 0.5f;
    const ImU32 lvl = theme::withAlpha(P.text, P.dark ? 0.05f : 0.07f);
    dl->AddLine(ImVec2(w0.x, std::round(mid) + 0.5f), ImVec2(w1.x, std::round(mid) + 0.5f), theme::withAlpha(P.text, P.dark ? 0.09f : 0.12f));
    const float lfs = theme::kFontCaption - 1;
    float lastY = -1e9f;
    for (int db : {0, -3, -6, -12, -18, -24, -36, -48}) {
      const float amp = std::pow(10.0f, static_cast<float>(db) / 20.0f) * c.gain * scale;
      if (amp > 1.0f) continue;
      const float dy = amp * half;
      if (dy < 22 || mid - dy - lastY < lfs + 4) continue;  // (near the middle, or crowding the one above: left out)
      lastY = mid - dy;
      char b[8];
      std::snprintf(b, sizeof b, "%d", db);
      for (float y : {std::round(mid - dy) + 0.5f, std::round(mid + dy) + 0.5f}) {
        for (float x = w0.x + 26; x < w1.x; x += 6) dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 2, w1.x), y), lvl);
        if (half >= 56) levelMarks.emplace_back(y, b);  // (its numbers where the waveform is tall enough for them)
      }
    }
  }
  // Where the clip sounds (pass by pass): what the waveform, the spectrum, scrubbing and the
  // selection's edits follow, the engine's own mapping.
  const std::vector<AudibleSpan> spans = clipAudibleSpans(s, c, tm);
  S.audiblePx.clear();
  for (const AudibleSpan& sp : spans) {  // (dev checks: in view, passes that follow each other joined)
    const float a0 = std::max(w0.x, g.xOf(sp.t0)), a1 = std::min(w1.x, g.xOf(sp.t1));
    if (a1 <= a0) continue;
    if (!S.audiblePx.empty() && a0 - S.audiblePx.back().second < 1.0f) S.audiblePx.back().second = a1;
    else S.audiblePx.emplace_back(a0, a1);
  }
  // The rest of the sample around the clip (dimmed): its first pass as if the whole audio
  // (inside the channel's start and end handles) played.
  const Ghost gh = ghostOf(s, tm, spans);
  waveScale(scale);
  if (gh.ok && !S.spectrum)
    drawAudioClipWave(dl, s, ch, gh.clip, tm, ImVec2(g.xOf(gh.t0), wy0), ImVec2(g.xOf(gh.t1), wy1), g.xOf(gh.t0), g.px, w0.x, w1.x,
                      theme::withAlpha(waveCol, 0.35f));
  // The clip: its spectrum or its waveform as it plays.
  if (S.spectrum) {
    drawClipSpectrum(a, S, dl, s, tm, ch, c, g, spans, gh, w0, w1, wy0, wy1);
  } else {
    S.wavePeak = std::min(1.0f, peak * c.gain * scale) * (wy1 - wy0) / std::max(1.0f, w1.y - w0.y);
    waveRunsRecord(true);
    drawAudioClipWave(dl, s, ch, c, tm, ImVec2(xs, wy0), ImVec2(xe, wy1), g.xOf(cs), g.px, w0.x, w1.x, waveCol);
    S.drawnPx = waveRuns();
    waveRunsRecord(false);
  }
  waveScale(1.0f);
  // Outside the clip: dimmed.
  const ImU32 dim = theme::withAlpha(P.dark ? P.surface950 : P.paper, 0.55f);
  if (xs > w0.x) dl->AddRectFilled(w0, ImVec2(xs, w1.y), dim);
  if (xe < w1.x) dl->AddRectFilled(ImVec2(xe, w0.y), w1, dim);
  // Inside the clip where it is silent (its audio ran out before its end): lightly dimmed, with a
  // dashed line where the audio ends, so the window shows what plays.
  {
    double from = cs;
    auto silent = [&](double t0, double t1) {
      const float a0 = std::max(w0.x, std::round(g.xOf(t0))), a1 = std::min(w1.x, std::round(g.xOf(t1)));
      if (a1 - a0 < 2) return;
      // Shaded and hatched (fine diagonal lines), so it reads as empty at a glance.
      dl->AddRectFilled(ImVec2(a0, w0.y), ImVec2(a1, w1.y), theme::withAlpha(P.dark ? P.surface950 : P.ink, P.dark ? 0.45f : 0.04f));
      dl->PushClipRect(ImVec2(a0, w0.y), ImVec2(a1, w1.y), true);
      const float hh = w1.y - w0.y;
      const ImU32 hatch = theme::withAlpha(P.text, P.dark ? 0.06f : 0.08f);
      for (float hx = a0 - hh; hx < a1; hx += 12.0f)
        dl->AddLine(ImVec2(hx, w1.y), ImVec2(hx + hh, w0.y), hatch, 1.0f);
      dl->PopClipRect();
      if (t0 > cs + 0.5)
        for (float y = w0.y + 2; y < w1.y; y += 8) dl->AddLine(ImVec2(a0 + 0.5f, y), ImVec2(a0 + 0.5f, std::min(y + 4, w1.y)), theme::withAlpha(P.text, 0.45f));
      // Its name in the middle of it, on a quiet plate (when it has the room).
      const char* note = i18n::tr("No audio");
      ImFont* nf = ImGui::GetFont();
      const float nfs = theme::kFontCaption;
      const ImVec2 ts = nf->CalcTextSizeA(nfs, 1e9f, 0, note);
      if (a1 - a0 > ts.x + 40 && hh > 40) {
        const ImVec2 m(std::round((a0 + a1) * 0.5f), std::round(w0.y + (w1.y - w0.y) * 0.62f));
        const ImVec2 b0(m.x - ts.x * 0.5f - 10, m.y - 11), b1(m.x + ts.x * 0.5f + 10, m.y + 11);
        dl->AddRectFilled(b0, b1, theme::withAlpha(P.dark ? P.surface900 : P.surface800, 0.9f), 11.0f);
        dl->AddRect(b0, b1, theme::withAlpha(P.text, 0.10f), 11.0f);
        dl->AddText(nf, nfs, ImVec2(std::round(m.x - ts.x * 0.5f), std::round(m.y - nfs * 0.5f) - 1), P.textDim, note);
      }
    };
    for (const AudibleSpan& sp : spans) {
      if (sp.t0 > from + 0.5) silent(from, std::min(sp.t0, ce));
      from = std::max(from, sp.t1);
    }
    if (!spans.empty() && ce > from + 0.5) silent(from, ce);
  }
  // The time selection.
  if (S.selT1 > S.selT0) {
    const float a0 = g.xOf(S.selT0), a1 = g.xOf(S.selT1);
    dl->AddRectFilled(ImVec2(a0, w0.y), ImVec2(a1, w1.y), theme::withAlpha(P.text, 0.10f));
    dl->AddRect(ImVec2(a0, w0.y), ImVec2(a1, w1.y), theme::withAlpha(P.text, 0.35f));
  }
  // The clip window's edges (trim handles).
  const ImU32 edge = theme::rgb(0xF5A623);
  dl->AddLine(ImVec2(xs + 0.5f, w0.y), ImVec2(xs + 0.5f, w1.y), edge, 1.5f);
  dl->AddLine(ImVec2(xe - 0.5f, w0.y), ImVec2(xe - 0.5f, w1.y), edge, 1.5f);
  dl->AddTriangleFilled(ImVec2(xs, w0.y), ImVec2(xs + 7, w0.y), ImVec2(xs, w0.y + 7), edge);
  dl->AddTriangleFilled(ImVec2(xe, w0.y), ImVec2(xe - 7, w0.y), ImVec2(xe, w0.y + 7), edge);
  // The level scale's numbers (dB at the clip's gain), on small plates at the left edge.
  {
    ImFont* nf = livelook::numFont(a);
    const float lfs = theme::kFontCaption - 1;
    for (const auto& [y, t] : levelMarks) {
      dl->AddLine(ImVec2(w0.x, y), ImVec2(w0.x + 5, y), theme::withAlpha(P.text, 0.4f));
      const ImVec2 ts = nf->CalcTextSizeA(lfs, 1e9f, 0, t.c_str());
      const ImVec2 t0(w0.x + 7, std::round(y - ts.y * 0.5f));
      dl->AddRectFilled(ImVec2(t0.x - 2, t0.y - 1), ImVec2(t0.x + ts.x + 2, t0.y + ts.y + 1), theme::withAlpha(P.dark ? P.surface950 : P.paper, 0.85f), 2.0f);
      dl->AddText(nf, lfs, t0, P.textDim, t.c_str());
    }
  }
  // The clip running on past the view (its audio fills the view, the clip is longer): where it ends.
  if (xe > w1.x + 1) {
    const Tick bar = std::max<Tick>(1, s.barTicks());
    char b[64];
    std::snprintf(b, sizeof b, i18n::tr("Clip ends at bar %d \xE2\x86\x92"), static_cast<int>(static_cast<Tick>(ce) / bar + 1));
    ImFont* nf = ImGui::GetFont();
    const float nfs = theme::kFontCaption;
    const ImVec2 ts = nf->CalcTextSizeA(nfs, 1e9f, 0, b);
    const ImVec2 b1(w1.x - 8, w1.y - 8), b0(b1.x - ts.x - 16, b1.y - 20);
    if (b0.x > std::max(w0.x, xs) + 40) {
      dl->AddRectFilled(b0, b1, theme::withAlpha(P.dark ? P.surface900 : P.surface800, 0.92f), 10.0f);
      dl->AddRect(b0, b1, theme::withAlpha(edge, 0.55f), 10.0f);
      dl->AddText(nf, nfs, ImVec2(b0.x + 8, std::round(b0.y + (20 - nfs) * 0.5f) - 1), P.textDim, b);
    }
  }
  // The volume line.
  const double db = clipgain::toDb(c.gain);
  auto gainY = [&](double d) { return wy0 + static_cast<float>(std::clamp((12.0 - d) / 72.0, 0.0, 1.0)) * (wy1 - wy0); };
  auto dbAtY = [&](float y) { return 12.0 - static_cast<double>((y - wy0) / std::max(1.0f, wy1 - wy0)) * 72.0; };
  const float gy = std::round(gainY(std::max(db, -60.0))) + 0.5f;
  for (float x = xs + 30; x < xe - 30; x += 10) dl->AddLine(ImVec2(x, gy), ImVec2(std::min(x + 6, xe - 30), gy), theme::withAlpha(P.text, 0.75f), 1.0f);
  {
    char b[64];
    std::snprintf(b, sizeof b, "%s %s", i18n::tr("Gain"), clipgain::label(c.gain).c_str());  // (the clip's gain, not a level)
    // The line's level on a small plate sitting on the line, at its right end (in view).
    ImFont* lf = ImGui::GetFont();
    const float lfs = theme::kFontLabel + 0.5f;
    const ImVec2 ts = lf->CalcTextSizeA(lfs, 1e9f, 0, b);
    const float rEdge = std::min(xe - 30, w1.x - 10);
    if (rEdge - std::max(xs, w0.x) > ts.x + 60) {
      const ImVec2 b0(std::round(rEdge - ts.x - 12), std::round(gy - 9)), b1(std::round(rEdge), std::round(gy + 9));
      dl->AddRectFilled(b0, b1, theme::withAlpha(P.dark ? P.surface800 : P.paper, 0.95f), 3.0f);
      dl->AddRect(b0, b1, theme::withAlpha(P.text, 0.22f), 3.0f);
      dl->AddText(lf, lfs, ImVec2(b0.x + 6, std::round(gy - lfs * 0.5f) - 1), P.text, b);
    }
  }
  // Fades: the curves and their handles.
  const ClipFades fd = resolveFades(s, tm, c);
  const double sec0 = tm.secondsAt(cs), dur = tm.secondsAt(ce) - sec0;
  auto xAtSec = [&](double sec) { return g.xOf(tm.tickAtSeconds(sec0 + sec)); };
  const float fiX = std::round(xAtSec(fd.in.len)), foX = std::round(xAtSec(dur - fd.out.len));
  const ImU32 fadeCol = P.dark ? P.paper : P.ink;
  auto curve = [&](bool in) {
    const EdgeFade& e = in ? fd.in : fd.out;
    if (e.len <= 0) return;
    std::vector<ImVec2> pts;
    const int n = 40;
    for (int i = 0; i <= n; ++i) {
      const double p = static_cast<double>(i) / n;
      const double gn = fadeGain(p, e.shape, e.tension);
      const double sec = in ? p * e.len : dur - p * e.len;
      pts.emplace_back(xAtSec(sec), wy1 - static_cast<float>(gn) * (wy1 - wy0));
    }
    // The part the fade takes away: shaded.
    for (size_t i = 0; i + 1 < pts.size(); ++i)
      dl->AddQuadFilled(ImVec2(pts[i].x, wy0), ImVec2(pts[i + 1].x, wy0), pts[i + 1], pts[i], theme::withAlpha(P.surface950, P.dark ? 0.45f : 0.2f));
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), fadeCol, ImDrawFlags_None, 1.5f);
  };
  curve(true);
  curve(false);
  auto square = [&](float x, bool hot) {
    const float r = hot ? 4.5f : 3.5f;
    dl->AddRectFilled(ImVec2(x - r, w0.y + 1), ImVec2(x + r, w0.y + 1 + 2 * r), fadeCol);
  };
  // Curve handles: the middle of each fade.
  auto curveHandle = [&](bool in) {
    const EdgeFade& e = in ? fd.in : fd.out;
    if (e.len <= 0) return ImVec2(-1e6f, -1e6f);
    const double gn = fadeGain(0.5, e.shape, e.tension);
    return ImVec2(xAtSec(in ? e.len * 0.5 : dur - e.len * 0.5), wy1 - static_cast<float>(gn) * (wy1 - wy0));
  };
  const ImVec2 chIn = curveHandle(true), chOut = curveHandle(false);
  // Warp markers (the shared widget, through the playlist's tick mapping).
  const bool warpTool = S.tool == TWarp;
  plwarp::drawMarkers(dl, s, c, tm, w0, ImVec2(w1.x, w1.y), xs, g.px, w0.x, w1.x, warpTool, waveCol);
  // The song position.
  if (a.engine.mode() == PlayMode::Song) {
    const double pos = a.engine.position();
    if (pos >= g.t0 && pos <= g.t1) {
      const float x = std::round(g.xOf(pos)) + 0.5f;
      dl->AddLine(ImVec2(x, w0.y), ImVec2(x, w1.y), P.playhead, 1.5f);
    }
    if (S.follow && a.engine.playing() && (pos < g.t0 || pos > g.t1)) {
      const double span = S.t1 - S.t0;
      S.t0 = pos - span * 0.1, S.t1 = S.t0 + span;
    }
  }
  // ---- the gestures ----
  ImGui::SetCursorScreenPos(w0);
  reach("wave");
  ImGui::InvisibleButton("##cewave", ImVec2(std::max(1.0f, w1.x - w0.x), std::max(1.0f, w1.y - w0.y)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  if (ImGui::BeginDragDropTarget()) {  // another file for the sample (every clip of it and its notes)
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      const std::string p(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(p)) browserui::loadChannelSample(a, ch.id, p, "load sample");
    }
    ImGui::EndDragDropTarget();
  }
  const bool hov = ImGui::IsItemHovered();
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const double mt = g.tOf(m.x);
  const Tick step = snapStep(s, S.snap);
  const bool free = io.KeyAlt || S.snap == 0;
  // Wheel: scroll; Ctrl+wheel: zoom at the pointer.
  if (hov && io.MouseWheel != 0) {
    const double span = S.t1 - S.t0;
    if (io.KeyCtrl || io.KeySuper || S.tool == TZoom) {
      const double k = io.MouseWheel > 0 ? 0.8 : 1.25;
      const double ns = std::clamp(span * k, 24.0, 1e8);
      S.t0 = mt - (mt - S.t0) * ns / span, S.t1 = S.t0 + ns;
    } else {
      const double d = -io.MouseWheel * span * 0.1;
      S.t0 += d, S.t1 += d;
    }
  }
  const uint64_t gkey = keyOf(c.id, 100);
  if (S.drag == 0 && hov) {
    // What the pointer is over.
    int over = 0;
    if (S.tool == TPointer) {
      if (std::fabs(m.x - xs) < 5) over = 1;
      else if (std::fabs(m.x - xe) < 5) over = 2;
      else if (m.y < w0.y + 12 && std::fabs(m.x - fiX) < 6) over = 4;
      else if (m.y < w0.y + 12 && std::fabs(m.x - foX) < 6) over = 5;
      else if (std::hypot(m.x - chIn.x, m.y - chIn.y) < 7) over = 6;
      else if (std::hypot(m.x - chOut.x, m.y - chOut.y) < 7) over = 7;
      else if (m.x > xs && m.x < xe && std::fabs(m.y - gy) < 4) over = 8;
      else if (m.x > xs && m.x < xe && io.KeyCtrl && io.KeyShift) over = 3;
      else over = 9;
    }
    if (over == 1 || over == 2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (over == 4 || over == 5) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (over == 6 || over == 7 || over == 8) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    square(fiX, over == 4);
    square(foX, over == 5);
    if (fd.in.len > 0) dl->AddCircleFilled(chIn, over == 6 ? 4.5f : 3.5f, fadeCol);
    if (fd.out.len > 0) dl->AddCircleFilled(chOut, over == 7 ? 4.5f : 3.5f, fadeCol);
    const bool press = ImGui::IsMouseClicked(ImGuiMouseButton_Left), dbl = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (press) {
      S.base = *cp;
      S.x0 = m.x, S.y0 = m.y, S.tick0 = mt;
      switch (S.tool) {
        case TPointer:
          if (dbl && (over == 6 || over == 7)) {
            setField(a, c.id, over == 6 ? clipfields::Field::FadeInCurve : clipfields::Field::FadeOutCurve, 0.0, false);
          } else if (dbl && over == 8) {
            setField(a, c.id, clipfields::Field::GainDb, 0.0, false);
          } else if (over == 9) {
            S.drag = 9;
            S.selT0 = S.selT1 = snapTick(mt, step, free);
          } else {
            S.drag = over;
            if (over == 6 || over == 7) S.uiStart = fadeTensionToUi(over == 6 ? cp->fadeInTension : cp->fadeOutTension);
            if (over == 8) S.gainDrag.begin(std::max(clipgain::kMinDb - 0.1, clipgain::toDb(cp->gain)), io.KeyShift, io.KeyAlt);
          }
          break;
        case TWarp:
          if (dbl) {
            plwarp::removeHovered(a, *cp);
          } else if (m.x >= xs && m.x <= xe && plwarp::begin(a, *cp, tm, mt, io.KeyShift, gkey, 6.0 / std::max(1e-6f, g.px))) {
            S.drag = 10;
          }
          break;
        case TSlip:
          if (m.x > xs && m.x < xe) S.drag = 3;
          break;
        case TFade:
          if (m.x > xs && m.x < xe) S.drag = m.x < (xs + xe) * 0.5f ? 4 : 5;
          break;
        case TSlice:
          if (m.x > xs + 2 && m.x < xe - 2) {
            const Tick at = static_cast<Tick>(snapTick(mt, step, free));
            const ObjectId id = c.id;
            a.doc.editSong("slice clip", [&](Song& x) { pltools::sliceClips(x, {id}, at, false); });
          }
          break;
        case TPencil:
          S.drag = 11;
          break;
        case TScrub:
          S.drag = 12;
          break;
        case TZoom: {
          const double span = S.t1 - S.t0, ns = std::max(24.0, span * 0.5);
          S.t0 = mt - ns * 0.5, S.t1 = mt + ns * 0.5;
          break;
        }
        default: break;
      }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (S.tool == TWarp && plwarp::hovered(c.id)) {
        plwarp::removeHovered(a, *cp);
      } else if (S.tool == TZoom) {
        const double span = S.t1 - S.t0, ns = std::min(1e8, span * 2.0);
        S.t0 = mt - ns * 0.5, S.t1 = mt + ns * 0.5;
      } else {
        ImGui::OpenPopup("##cesel");
      }
    }
  }
  if (S.drag != 0) {
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const Clip* now = s.clip(c.id);
    if (!down || !now) {
      if (S.drag == 10) plwarp::end();
      if (S.drag == 12) a.engine.preview(nullptr);
      if (S.drag == 9 && std::fabs(S.selT1 - S.selT0) < 1) S.selT0 = S.selT1 = 0;
      S.drag = 0;
      a.doc.endGesture();
    } else {
      const double dt = mt - S.tick0;
      const Tick d = static_cast<Tick>(std::llround(free ? dt : std::round(dt / static_cast<double>(step)) * static_cast<double>(step)));
      auto commit = [&](const Clip& after, const char* label) {
        if (!(after == *now)) a.doc.editClip(c.id, label, [&after](Clip& k) { k = after; }, gkey);
      };
      switch (S.drag) {
        case 1: commit(clipgest::trimStart(s, tm, S.base, d, false, false), "trim clip"); break;
        case 2: commit(clipgest::trimEnd(s, tm, S.base, d, false, false), "resize clip"); break;
        case 3: commit(clipgest::slip(s, tm, S.base, static_cast<Tick>(std::llround(dt)), false), "slip clip"); break;
        case 4:
        case 5: {
          const double at = std::clamp(tm.secondsAt(std::max(0.0, mt)) - sec0, 0.0, dur);
          const int kind = S.drag == 4 ? 1 : 2;
          double v = clipgest::fadeLength(kind, at, dur, kind == 1 ? fd.out.len : fd.in.len);
          if (S.tool == TFade) {  // as long as the span dragged over the edge
            const double from = std::clamp(tm.secondsAt(std::max(0.0, S.tick0)) - sec0, 0.0, dur);
            v = std::clamp(std::fabs(at - (kind == 1 ? 0.0 : dur)) - std::fabs(from - (kind == 1 ? 0.0 : dur)) + std::fabs(from - (kind == 1 ? 0.0 : dur)), 0.0,
                           std::max(0.0, dur - (kind == 1 ? fd.out.len : fd.in.len)));
          }
          setField(a, c.id, kind == 1 ? clipfields::Field::FadeIn : clipfields::Field::FadeOut, v);
          char b[64];
          std::snprintf(b, sizeof b, "%s: %.0f ms", i18n::tr(kind == 1 ? "Fade in" : "Fade out"), v * 1000.0);
          ImGui::SetTooltip("%s", b);
          break;
        }
        case 6:
        case 7: {
          const float u = clipgest::fadeCurveUi(S.uiStart, m.y - S.y0);
          setField(a, c.id, S.drag == 6 ? clipfields::Field::FadeInCurve : clipfields::Field::FadeOutCurve, u);
          ImGui::SetTooltip("%s: %+.0f %%", i18n::tr("Fade curve"), static_cast<double>(u) * 100.0);
          break;
        }
        case 8: {
          const double v = S.gainDrag.move(static_cast<double>(S.y0 - m.y));
          setField(a, c.id, clipfields::Field::GainDb, v);
          ImGui::SetTooltip("%s: %s", i18n::tr("Clip volume"), clipgain::label(clipgain::fromDb(v)).c_str());
          break;
        }
        case 9: S.selT1 = snapTick(mt, step, free); break;
        case 10: plwarp::drag(a, tm, static_cast<Tick>(std::llround(mt)), free ? 1 : step); break;
        case 11: {
          const double v = std::clamp(dbAtY(m.y), -60.0, 12.0);
          setField(a, c.id, clipfields::Field::GainDb, v <= -59.5 ? clipgain::kMinDb - 0.1 : v);
          ImGui::SetTooltip("%s: %s", i18n::tr("Clip volume"), clipgain::label(now->gain).c_str());
          break;
        }
        case 12:
          if (ImGui::GetTime() - S.lastScrub > 0.06 && std::fabs(mt - S.scrubAt) > 0.5) {
            scrubAt(a, s, *now, tm, mt);
            S.lastScrub = ImGui::GetTime();
            S.scrubAt = mt;
          }
          break;
        default: break;
      }
    }
  }
  if (S.selT0 > S.selT1 && S.drag != 9) std::swap(S.selT0, S.selT1);
  // The selection's menu.
  if (ImGui::BeginPopup("##cesel")) {
    const double a0 = std::min(S.selT0, S.selT1), a1 = std::max(S.selT0, S.selT1);
    ImGui::TextDisabled("%s", i18n::tr(a1 > a0 ? "Selection" : "Whole clip"));
    using clipedit::Op;
    struct Row {
      Op op;
      const char* key;
    };
    const Row rows[] = {{Op::Cut, "Ctrl+X"}, {Op::Silence, nullptr}, {Op::TrimToSelection, "Ctrl+T"}};
    for (const Row& r : rows)
      if (ImGui::MenuItem(i18n::tr(clipedit::opName(r.op)), keys::namedText(r.key), false, a1 > a0 || r.op == Op::Silence)) askOp(a, c.id, r.op, a0, a1);
    ImGui::Separator();
    for (Op op : {Op::FadeIn, Op::FadeOut, Op::Normalize, Op::Reverse})
      if (ImGui::MenuItem(i18n::tr(clipedit::opName(op)))) askOp(a, c.id, op, a0, a1);
    ImGui::Separator();
    const Tick at = static_cast<Tick>(std::llround(S.selT0 > 0 ? S.selT0 : mt));
    if (ImGui::MenuItem(word("Split the clip here").c_str(), nullptr, false, at > c.start && at < static_cast<Tick>(ce))) {
      const ObjectId id = c.id;
      a.doc.editSong("slice clip", [&](Song& x) { pltools::sliceClips(x, {id}, at, false); });
    }
    if (ImGui::MenuItem(i18n::tr("Clear the selection"), nullptr, false, a1 > a0)) S.selT0 = S.selT1 = 0;
    ImGui::EndPopup();
  }
  dl->PopClipRect();
  // ---- the overview: the whole sample and the view's window ----
  const ImVec2 o0(p0.x, w1.y), o1(p1.x, p1.y);
  S.ovA = o0, S.ovB = o1;
  reach("overview");
  {
    // The part of the file the view shows: the ghost (the whole audio inside the handles, lined up
    // with the clip) maps the view's ticks to the file and back.
    ViewOnFile vf;
    const ClipSource src = clipSourceIfReady(ch);
    if (gh.ok && src.sample && src.sample->frames() > 1) {
      const double secs = src.sample->seconds();
      const double last = static_cast<double>(src.sample->frames() - 1) / src.sample->sampleRate;
      const bool rev = clipPlaysReversed(ch, c);
      auto fileFrac = [&](double t) {
        double sec = clipSourceAt(s, gh.clip, tm, std::clamp(t, gh.t0, gh.t1));
        if (rev) sec = last - sec;
        return std::clamp(sec / std::max(1e-9, secs), 0.0, 1.0);
      };
      double f0 = fileFrac(g.t0), f1 = fileFrac(g.t1);
      if (f0 > f1) std::swap(f0, f1);
      vf.on = true, vf.f0 = f0, vf.f1 = f1;
      const Clip ghc = gh.clip;
      const double span = S.t1 - S.t0;
      vf.centreAt = [&S, &s, ghc, span, rev, last, secs](double f) {
        double sec = f * secs;
        if (rev) sec = last - sec;
        const double t = clipTickOfSource(s, ghc, tm, sec);
        if (std::isfinite(t)) S.t0 = t - span * 0.5, S.t1 = t + span * 0.5;
      };
    }
    sampleStrip(a, S, ch, o0, o1, false, vf);
  }
  (void)host;
  (void)c0;
}

// ------------------------------------------------------------------ helpers the modules draw with

void replaceSampleDialog(App& a, const Channel& ch) {
  const ObjectId id = ch.id;
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = "Replace sample";
  const std::string path = samplePath(ch);
  a.fileDialog.dir = path.empty() ? (gl::platform::homeEnv() ? gl::platform::homeEnv() : ".") : fs::path(path).parent_path().string();
  a.fileDialog.exts = audioFileExtensions();
  a.fileDialog.onOk = [&a, id](const std::string& p) { browserui::loadChannelSample(a, id, p, "load sample"); };
}


void auditionKeys(App& a, const Channel& ch, float width, float kh) {
  const auto& P = pal();
  const int root = static_cast<int>(std::lround(smpPar(ch, pp::SmpRoot, 60)));
  const int base = std::clamp((root / 12) * 12 - 12, 0, 96);
  constexpr int kWhite = 14;
  const int whiteSemis[] = {0, 2, 4, 5, 7, 9, 11};
  const float kw = std::floor(std::clamp((width - 2) / kWhite, 12.0f, 60.0f));
  const float bw = std::round(kw * 0.62f), bh = std::round(kh * 0.6f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##cekeys", ImVec2(kw * kWhite, kh), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const int blackAfter[] = {1, 2, 4, 5, 6}, blackSemi[] = {1, 3, 6, 8, 10};
  int hit = -1;
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int wi = std::clamp(static_cast<int>((m.x - p.x) / kw), 0, kWhite - 1);
    hit = base + (wi / 7) * 12 + whiteSemis[wi % 7];
    if (m.y - p.y < bh)
      for (int o = 0; o < 2; ++o)
        for (int k = 0; k < 5; ++k) {
          const float bx = p.x + kw * static_cast<float>(o * 7 + blackAfter[k]) - bw * 0.5f;
          if (m.x >= bx && m.x < bx + bw) hit = base + o * 12 + blackSemi[k];
        }
  }
  // Right-click a key: the sample's root note (it plays at its own pitch there).
  if (hit >= 0 && ImGui::IsItemClicked(ImGuiMouseButton_Right) && hit != root) a.doc.setParam(smpPid(ch.id, pp::SmpRoot), hit, false);
  // Held, the key under the pointer sounds: a drag plays one key at a time (KeyGlide.h).
  static keyglide::Held glide;
  const int held = keyglide::followItem(a, ch.id, ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) ? hit : -1, glide, 0.8f);
  keyglide::devNote("clipwin", p, ImVec2(p.x + kw * kWhite, p.y + kh), base, kWhite, held);
  const keylight::ChannelLights* lit = keylight::lights(a, ch.id);
  auto fillOf = [&](ImU32 col, int note) {
    if (held == note) return P.hot;
    const float lv = lit ? lit->key(note) : 0.0f;
    if (lv > 0.004f) col = theme::mix(col, P.hot, 0.85f * lv);
    return note == root ? theme::mix(col, P.hot, 0.55f) : col;
  };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int i = 0; i < kWhite; ++i) {
    const int note = base + (i / 7) * 12 + whiteSemis[i % 7];
    const ImVec2 k0(p.x + kw * static_cast<float>(i), p.y), k1(k0.x + kw - 1, p.y + kh);
    dl->AddRectFilled(k0, k1, fillOf(hit == note ? theme::mix(P.keyWhite, P.paper, 0.6f) : P.keyWhite, note), 3, ImDrawFlags_RoundCornersBottom);
    if (i % 7 == 0) {
      char lbl[8];
      std::snprintf(lbl, sizeof lbl, "C%d", note / 12 - 1);
      const ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontLabel - 1, 1e9f, 0, lbl);
      dl->AddText(a.fontMono, theme::kFontLabel - 1, ImVec2(std::round(k0.x + (kw - 1 - ts.x) * 0.5f), k1.y - ts.y - 3), theme::mix(P.keyWhite, P.keyBlack, 0.45f), lbl);
    }
  }
  for (int o = 0; o < 2; ++o)
    for (int k = 0; k < 5; ++k) {
      const float bx = p.x + kw * static_cast<float>(o * 7 + blackAfter[k]) - bw * 0.5f;
      const int note = base + o * 12 + blackSemi[k];
      dl->AddRectFilled(ImVec2(bx, p.y), ImVec2(bx + bw, p.y + bh), fillOf(hit == note ? theme::mix(P.keyBlack, P.paper, 0.15f) : P.keyBlack, note), 2,
                        ImDrawFlags_RoundCornersBottom);
    }
  ui::Hint("Click or drag to hear the sample at each note; right-click a key to make it the root note");
}

std::string noteLabel(int n) {
  static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  n = std::clamp(n, 0, 127);
  return std::string(kNames[n % 12]) + std::to_string(n / 12 - 1);
}

// An envelope's shape from its times (seconds; a stage's width follows its time's square root so
// short stages stay visible) and its sustain level.
void envGraph(ImVec2 p0, ImVec2 p1, const double* t, int n, double sus, ImU32 col) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, P.well, 4);
  for (int k = 1; k < 4; ++k) dl->AddLine(ImVec2(p0.x, p0.y + (p1.y - p0.y) * k / 4), ImVec2(p1.x, p0.y + (p1.y - p0.y) * k / 4), P.line);
  double w[6] = {0, 0, 0, 0, 0, 0};
  double total = 0;
  for (int k = 0; k < n && k < 6; ++k) total += (w[k] = std::sqrt(std::max(0.0, t[k])) + (k == 4 ? 0.35 : 0.02));
  const float pad = 6, ww = p1.x - p0.x - 2 * pad, h = p1.y - p0.y - 2 * pad;
  auto X = [&](double s) { return p0.x + pad + static_cast<float>(s / std::max(1e-9, total)) * ww; };
  auto Y = [&](double v) { return p1.y - pad - static_cast<float>(v) * h; };
  ImVec2 pts[7];
  double at = 0;
  pts[0] = ImVec2(X(0), Y(0));
  at += w[0];
  pts[1] = ImVec2(X(at), Y(0));
  at += w[1];
  pts[2] = ImVec2(X(at), Y(1));
  at += w[2];
  pts[3] = ImVec2(X(at), Y(1));
  at += w[3];
  pts[4] = ImVec2(X(at), Y(sus));
  at += w[4];
  pts[5] = ImVec2(X(at), Y(sus));
  at += w[5];
  pts[6] = ImVec2(X(at), Y(0));
  dl->AddPolyline(pts, 7, col, 0, 1.75f);
  for (int k = 1; k < 7; ++k) dl->AddCircleFilled(pts[k], 3.0f, col);
}

// The filter's shape (its type, cutoff and resonance; a picture of the response, 20 Hz to 20 kHz,
// -30 to +18 dB): type 0 off (flat), 1 low pass, 2 low pass 24, 3 band pass, 4 high pass, 5 notch.
void filterGraph(ImVec2 p0, ImVec2 p1, int type, double cutoff, double reso, ImU32 col) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, P.well, 4);
  const float pad = 4, h = p1.y - p0.y - 2 * pad;
  const double top = 18, bottom = -30;
  auto Y = [&](double db) { return p0.y + pad + static_cast<float>((top - std::clamp(db, bottom, top)) / (top - bottom)) * h; };
  for (double f : {100.0, 1000.0, 10000.0}) {  // the decades
    const float x = std::round(p0.x + static_cast<float>(std::log10(f / 20.0) / 3.0) * (p1.x - p0.x)) + 0.5f;
    dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), P.line);
  }
  dl->AddLine(ImVec2(p0.x + 2, std::round(Y(0)) + 0.5f), ImVec2(p1.x - 2, std::round(Y(0)) + 0.5f), P.line);
  const double q = 0.707 + std::clamp(reso, 0.0, 1.0) * 9.0, fc = std::clamp(cutoff, 20.0, 20000.0);
  std::vector<ImVec2> pts;
  for (float x = p0.x + 1; x <= p1.x - 1; x += 2.0f) {
    const double f = 20.0 * std::pow(1000.0, static_cast<double>((x - p0.x) / std::max(1.0f, p1.x - p0.x)));
    const double w = f / fc, d = std::sqrt((1 - w * w) * (1 - w * w) + (w / q) * (w / q));
    double m = 1;
    switch (type) {
      case 1: m = 1 / d; break;
      case 2: m = 1 / (d * d); break;
      case 3: m = (w / q) / d; break;
      case 4: m = w * w / d; break;
      case 5: m = std::fabs(1 - w * w) / d; break;
      default: break;
    }
    pts.emplace_back(x, Y(20.0 * std::log10(std::max(1e-6, m))));
  }
  for (size_t i = 0; i + 1 < pts.size(); ++i)
    dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, p1.y - pad), ImVec2(pts[i].x, p1.y - pad), theme::withAlpha(col, 0.14f));
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), type ? col : theme::withAlpha(col, 0.45f), 0, 1.75f);
}

// The LFO's shape (two of its cycles; flat for its delay), as tall as its amount.
void lfoGraph(ImVec2 p0, ImVec2 p1, int shape, double amount, double delay, bool on, ImU32 col) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, P.well, 4);
  const float mid = std::round((p0.y + p1.y) * 0.5f) + 0.5f, half = (p1.y - p0.y) * 0.5f - 4;
  dl->AddLine(ImVec2(p0.x + 2, mid), ImVec2(p1.x - 2, mid), P.line);
  const float x0 = p0.x + 4, x1 = p1.x - 4;
  const float dx = std::min((x1 - x0) * 0.3f, static_cast<float>(std::sqrt(std::max(0.0, delay))) * (x1 - x0) * 0.25f);
  const float amp = static_cast<float>(std::clamp(std::fabs(amount), 0.15, 1.0)) * half * (amount < 0 ? -1.0f : 1.0f);
  std::vector<ImVec2> pts;
  pts.emplace_back(x0, mid);
  for (float x = x0 + dx; x <= x1; x += 1.5f) {
    const double ph = std::fmod(2.0 * static_cast<double>((x - x0 - dx) / std::max(1.0f, x1 - x0 - dx)), 1.0);
    const double v = shape == 1 ? (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph) : shape == 2 ? (ph < 0.5 ? 1.0 : -1.0) : std::sin(ph * 2 * 3.14159265358979);
    pts.emplace_back(x, mid - static_cast<float>(v) * amp);
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), on ? col : theme::withAlpha(col, 0.45f), 0, 1.75f);
}

// What the shown tab changes, said at the tab bar's right.
std::string scopeLine(App& a, int tab, const Clip* c, const Channel& ch) {
  const int n = clipsSharing(a.doc.song(), ch.id);
  char b[160];
  switch (clipwin::scopeOf(tab)) {
    case clipwin::Scope::Clip: {
      const size_t sel = c ? selectionFor(a, c->id).size() : 1;
      if (sel > 1) std::snprintf(b, sizeof b, i18n::tr("Changes this clip and the %d selected with it"), static_cast<int>(sel) - 1);
      else std::snprintf(b, sizeof b, "%s", i18n::tr("Changes this clip"));
      break;
    }
    case clipwin::Scope::Sample:
      if (n > 1) std::snprintf(b, sizeof b, i18n::tr("Changes the sample: its %d clips and its notes"), n);
      else if (n == 1) std::snprintf(b, sizeof b, "%s", i18n::tr("Changes the sample: this clip and its notes"));
      else std::snprintf(b, sizeof b, "%s", i18n::tr("Changes the sample the channel's notes play"));
      break;
    case clipwin::Scope::Channel: std::snprintf(b, sizeof b, "%s", i18n::tr("Changes how the channel plays its notes")); break;
    default:
      std::snprintf(b, sizeof b, "%s",
                    tab == TabEdit ? (c ? i18n::tr("Writes new audio for this clip") : i18n::tr("Writes a new file for the sample"))
                                   : (c ? i18n::tr("Measures this clip") : i18n::tr("Measures the sample")));
  }
  return b;
}

// ------------------------------------------------------------------ the tabs: modules on one grid
//
// Under the waveform, everything about the clip, its sample and its channel sits in modules of one
// height, a few to a tab (Clip, Warp, Sample, Playback, Envelope, Filter, Sample FX, Channel, Note
// effects, MIDI and macros), side by side and sharing the width (a module wider than its cells has
// them in its middle), wrapped to a second row when they do not fit, never scrolling sideways, with
// the Output module pinned at the right. Every module has the same numbers (a 72 px cell: 64 px of
// control and an 8 px gutter):
//   width      cols x 72 - 8 + 2 x 12
//   0..32      title row: scope tick, TITLE, scope word, at most one module switch, the module menu
//   44..100    display (56), row A 108..132, row B 140..164      (variant 0)
//   44..132    display (88), row B 140..164                       (variant 1)
//   44..164    display (120), no rows                             (variant 2)
//   44..250    display (206), no knobs                            (variant 3)
//   176..190   knob names, 192..232 knobs (40 px), 236..250 values; the module 262 tall
// Compact (a short pane or band, until the divider gives the modules the room): the title row and the
// knob row only (names at 40, knobs at 56, values at 100; 126 tall); in a pane narrower than a module,
// its knobs fold onto rows of their own under it (86 each). Buttons, toggles, segmented
// controls, fields and lists are 24 px high and span whole cells.

constexpr float kCell = 72, kGut = 8, kPad = 12, kTitleH = 32, kDispY = 44, kRowA = 108, kRowB = 140, kRowH = 24;
constexpr float kNameY = 176, kKnobOff = 16, kKnobR = 20, kValueOff = 60, kModH = 262, kModGap = 8;
constexpr float kCompactNameY = 40, kCompactModH = 126, kKnobRowH = kCompactModH - kCompactNameY;
constexpr float kTextFs = 11.0f;
float modW(int cols) { return cols * kCell - kGut + 2 * kPad; }
// The widest a module alone in its row grows (wider ones keep their own width).
constexpr float kLoneW = 640;
float spanW(int n) { return n * kCell - kGut; }

enum class MScope { Clip, Sample, Rendered, Channel };
ImU32 scopeCol(MScope s) {
  switch (s) {
    case MScope::Clip: return theme::rgb(0x5AA9E6);
    case MScope::Sample:
    case MScope::Rendered: return sampleOrange();
    default: return theme::rgb(0x3DB38A);
  }
}
// The scope colour as text: a step darker on the light theme, so it reads on pale faces.
ImU32 scopeText(MScope s) {
  const auto& P = pal();
  return P.dark ? scopeCol(s) : theme::mix(scopeCol(s), P.ink, 0.35f);
}
const char* scopeWord(MScope s) {
  switch (s) {
    case MScope::Clip: return i18n::tr("clip");
    case MScope::Sample: return i18n::tr("sample");
    case MScope::Rendered: return i18n::tr("rendered");
    default: return i18n::tr("channel");
  }
}

// (dev "clipeditor strip") what each module drew where, as last drawn: its box and its controls' boxes,
// the label each control needs and the width it got.
struct LaidCtl {
  ImVec2 a, b;
  float need = 0;  // the width its label needs (0: no label)
  int kind = 0;    // 0 control, 1 knob, 2 display
};
struct LaidMod {
  std::string id;
  ImVec2 a, b;
  bool compact = false;
  float titleEnd = 0, rightStart = 0;  // where the title ends, where the scope word, switch and menu start
  std::vector<LaidCtl> ctl;
};
std::vector<LaidMod> gLaid[5];  // by Host, then Smooth's inspector (4)
int gLaidHost = 0;
LaidMod* laidNow() { return gLaid[gLaidHost].empty() ? nullptr : &gLaid[gLaidHost].back(); }
void laid(ImVec2 a, ImVec2 b, float need = 0, int kind = 0) {
  if (LaidMod* m = laidNow()) m->ctl.push_back({a, b, need, kind});
}
// The width a TextButton's label takes (with its padding).
float labelNeed(const char* label) { return ui::TextButtonWidth(label); }

// How a module's controls are placed: on the window's grid (its tabs), in a card of the Clips
// layout's band (the same grid, its rows and knobs fitted to the band's height), or in rows down a
// column (Smooth's inspector and its audio editor's local inspector: a row per knob, the buttons in
// lines, the display a short strip on top).
enum class Form { Grid, Band, Rows };

// A band card's numbers for a card `h` tall: the title strip, the display (when it has the room),
// row A and row B, the knobs' names, knobs and values along the foot.
constexpr float kBandTitleH = 22, kBandKnobR = 14, kBandKnobOff = 13, kBandValueOff = 42, kBandKnobsH = 56, kBandMinH = 152;
// A short band (under kBandMinH): the compact card: smaller knobs, the rows closer (down to this).
constexpr float kBandCompactKnobR = 9, kBandCompactKnobOff = 10, kBandCompactValueOff = 29, kBandCompactKnobsH = 41, kBandCompactMinH = 120;
struct BandGeo {
  float rowA = 0, rowB = 0, nameY = 0, dispY = 0, dispH = 0;
  float knobR = kBandKnobR, knobOff = kBandKnobOff, valueOff = kBandValueOff;
};
BandGeo bandGeo(float h, int variant) {
  BandGeo g;
  if (h < kBandMinH) {
    h = std::max(h, kBandCompactMinH);
    g.knobR = kBandCompactKnobR, g.knobOff = kBandCompactKnobOff, g.valueOff = kBandCompactValueOff;
    g.nameY = h - 2 - kBandCompactKnobsH;
    g.rowB = g.nameY - 3 - kRowH;
    g.rowA = g.rowB - 2 - kRowH;
    g.dispY = kBandTitleH + 3;
  } else {
    g.nameY = h - 6 - kBandKnobsH;
    g.rowB = g.nameY - 8 - kRowH;
    g.rowA = g.rowB - 4 - kRowH;
    g.dispY = kBandTitleH + 6;
  }
  const float bottom = variant == 0 ? g.rowA - 6 : variant == 1 ? g.rowB - 6 : variant == 2 ? g.nameY - 6 : h - 8;
  g.dispH = std::max(0.0f, bottom - g.dispY);
  return g;
}

// Rows: the next free place down the column, the line of buttons being filled, the knob row drawn.
constexpr float kRowsKnobR = 9, kRowsLineH = 24, kRowsGap = 4;
struct Flow {
  float y = 0;          // the next free y (screen)
  float lineY = -1;     // the open line of buttons (-1: none)
  float lineX = 0;      // the width used on it
  float knobY = 0;      // the knob row being drawn
  std::string knobName;  // its name
  int cellsOnLine = 0;  // a channel part's knob cells on the open cell line
  float cellY = -1;     // that line's y (-1: none)
  int rows = 0;         // the rows drawn (a hairline between rows)
};

struct Cx {
  App& a;
  ImDrawList* dl;
  ImVec2 o;  // the module's top-left (screen)
  float w;
  int variant;
  bool compact;
  float gx = 0;  // the cells' offset in a module wider than its cells (they sit in its middle)
  int fold = 0;  // (compact, a narrow pane) knobs per row, the rest on rows under it (0: one row)
  Form form = Form::Grid;
  BandGeo band{};       // (Band) the card's rows
  Flow* flow = nullptr;  // (Rows) where the next control goes
  int cols = 1;          // (Rows) the module's cells, for the display's parts
  // (Rows) the labels of buttons left out here (their place has its own row: Smooth's Mute and Loop)
  const std::vector<std::string>* omit = nullptr;
  bool showDisp = true;  // (Rows) the module's display shows (the inspector's rows have none)
  bool head = false;     // (Band) the head card: the display holds the clip's file, not Output's notes
  bool rows() const { return form == Form::Rows; }
  float cellX(int c) const {
    if (rows()) return o.x + static_cast<float>(c) * (w + kGut) / static_cast<float>(std::max(1, cols));
    return o.x + kPad + gx + static_cast<float>(fold > 0 ? c % fold : c) * kCell;
  }
  float cellW(int n) const { return rows() ? (w + kGut) * static_cast<float>(n) / static_cast<float>(std::max(1, cols)) - kGut : spanW(n); }
  float rowY(int r) const {
    if (form == Form::Band) return o.y + (variant == 0 && r == 0 ? band.rowA : band.rowB);
    return o.y + (variant == 0 && r == 0 ? kRowA : kRowB);
  }
  float nameY() const { return form == Form::Band ? o.y + band.nameY : o.y + (compact ? kCompactNameY : kNameY); }
  // The name row of the knob in cell c (a folded module's later knobs on the rows under).
  float knobY(int c) const { return nameY() + (fold > 0 ? static_cast<float>(c / fold) * kKnobRowH : 0.0f); }
  float knobR() const { return form == Form::Band ? band.knobR : rows() ? kRowsKnobR : kKnobR; }
  float knobOff() const { return form == Form::Band ? band.knobOff : kKnobOff; }
  float valueOff() const { return form == Form::Band ? band.valueOff : kValueOff; }
  float dispH() const {
    if (form == Form::Band) return band.dispH;
    if (rows()) return variant == 0 ? 40.0f : variant == 1 ? 46.0f : variant == 2 ? 52.0f : 120.0f;
    return variant == 0 ? 56.0f : variant == 1 ? 88.0f : variant == 2 ? 120.0f : 206.0f;
  }
  float dispY() const { return form == Form::Band ? band.dispY : rows() ? 0.0f : kDispY; }
  // The module shows its display (a short band card has no room for it).
  bool disp() const { return !compact && showDisp && dispH() >= 22.0f; }
  ImVec2 d0() const { return ImVec2(rows() ? o.x : o.x + kPad, o.y + dispY()); }
  ImVec2 d1() const { return ImVec2(rows() ? o.x + w : o.x + w - kPad, o.y + dispY() + dispH()); }
  // The display's part over cells c0 .. c0 + n - 1.
  ImVec2 dp0(int c0) const { return ImVec2(cellX(c0), o.y + dispY()); }
  ImVec2 dp1(int c0, int n) const { return ImVec2(cellX(c0) + cellW(n), o.y + dispY() + dispH()); }
  void at(int c, int row) const { ImGui::SetCursorScreenPos(ImVec2(cellX(c), rowY(row))); }
  bool omitted(const char* label) const { return omit && std::find(omit->begin(), omit->end(), label) != omit->end(); }

  // ---- Rows
  // A hairline above every row but the first.
  void rowLine(float y) const {
    if (flow->rows++ > 0) dl->AddLine(ImVec2(o.x, std::round(y - kRowsGap * 0.5f) + 0.5f), ImVec2(o.x + w, std::round(y - kRowsGap * 0.5f) + 0.5f), theme::withAlpha(pal().line, 0.7f));
  }
  void closeLine() const {
    if (flow->lineY >= 0) flow->y = flow->lineY + kRowH + kRowsGap, flow->lineY = -1, flow->lineX = 0;
    if (flow->cellY >= 0) flow->y = flow->cellY + kBandValueOff + kTextFs + 6, flow->cellY = -1, flow->cellsOnLine = 0;
  }
  // The place of a control `span` cells wide (at least `need` px) on the open line of buttons, a new
  // line when it does not fit; its width.
  float place(int span, float need, ImVec2& p) const {
    if (flow->cellY >= 0) closeLine();
    const float natural = std::floor((w + 6.0f) * static_cast<float>(std::min(span, 4)) / 4.0f - 6.0f);
    float cw = std::min(w, std::max(natural, need));
    if (flow->lineY >= 0 && flow->lineX > 0 && flow->lineX + cw > w + 0.5f) closeLine();
    if (flow->lineY < 0) {
      flow->lineY = flow->y;
      rowLine(flow->y);
    }
    // the last control of a line ends at its right (a line of one: the whole width)
    if (flow->lineX + cw > w - 40 && flow->lineX + cw <= w + 0.5f) cw = w - flow->lineX;
    p = ImVec2(o.x + flow->lineX, flow->lineY);
    flow->lineX += cw + 6;
    return cw;
  }
  // A whole line `h` tall (a keyboard, a list).
  ImVec2 block(float h) const {
    closeLine();
    const ImVec2 p(o.x, flow->y);
    rowLine(flow->y);
    flow->y += h + kRowsGap;
    return p;
  }
  // A channel part's knob cell (its own name, knob and value), three or so to a line.
  ImVec2 cell(float cw) const {
    if (flow->lineY >= 0) closeLine();
    const int per = std::max(1, static_cast<int>((w + 4) / (cw + 4)));
    if (flow->cellY >= 0 && flow->cellsOnLine >= per) closeLine();
    if (flow->cellY < 0) {
      flow->cellY = flow->y;
      rowLine(flow->y);
    }
    const float step = std::floor((w - cw) / static_cast<float>(std::max(1, per - 1)));
    const ImVec2 p(std::round(o.x + (per > 1 ? step * static_cast<float>(flow->cellsOnLine) : (w - cw) * 0.5f)), flow->cellY);
    ++flow->cellsOnLine;
    return p;
  }
};

ImU32 wellFill() {
  const auto& P = pal();
  return P.dark ? P.surface950 : theme::mix(P.paper, P.surface800, 0.35f);
}
void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  if (gTrkSkin) {  // (the Tracker: a sunken bevel well)
    trackerview::bevelBox(dl, p0, p1, true);
    laid(p0, p1, 0, 2);
    return;
  }
  dl->AddRectFilled(p0, p1, wellFill(), theme::kRadius);
  dl->AddRect(p0, p1, theme::withAlpha(P.text, P.dark ? 0.08f : 0.14f), theme::kRadius);
  laid(p0, p1, 0, 2);
}
// A short readout in a display's corner (`right`, `bottom`), cut to `room` when given.
void dispNote(const Cx& x, ImVec2 p0, ImVec2 p1, const std::string& t, bool right = false, bool bottom = false, ImU32 col = 0, float room = 0) {
  const auto& P = pal();
  ImFont* f = x.a.fontMono ? x.a.fontMono : ImGui::GetFont();
  const float full = p1.x - p0.x - 12;
  room = room > 0 ? std::min(room, full) : full;
  const float tw = std::min(room, f->CalcTextSizeA(kTextFs, 1e9f, 0, t.c_str()).x);
  const float tx = right ? p1.x - 6 - tw : p0.x + 6;
  const float ty = bottom ? p1.y - 4 - kTextFs : p0.y + 4;
  theme::textEllipsis(x.dl, f, kTextFs, ImVec2(std::round(tx), std::round(ty)), room, col ? col : theme::withAlpha(P.text, 0.78f), t.c_str());
}
// Text centred on `cx`, set a little smaller to fit `maxW` (never under 10 px), then cut.
int gTextCut = 0;  // (dev "clipeditor mainpage") the names and values cut since the last check
void textC(ImDrawList* dl, ImFont* f, float fs, float cx, float y, ImU32 col, const char* t, float maxW) {
  float w = f->CalcTextSizeA(fs, 1e9f, 0, t).x;
  if (w > maxW) {
    const float k = std::max(10.0f, fs * maxW / w);
    w = f->CalcTextSizeA(k, 1e9f, 0, t).x;
    if (w > maxW) {
      ++gTextCut;
      theme::textEllipsis(dl, f, k, ImVec2(std::round(cx - maxW * 0.5f), y), maxW, col, t);
      return;
    }
    fs = k;
  }
  dl->AddText(f, fs, ImVec2(std::round(cx - w * 0.5f), y), col, t);
}

// A knob cell's name (above) and value (below), centred in cell c. In rows: the knob's row, its name
// on the left, its value right of centre, the knob at the right end.
void cellName(const Cx& x, int c, const char* name) {
  if (x.rows()) {
    x.closeLine();
    const float y = x.flow->y;
    x.rowLine(y);
    x.flow->knobY = y;
    x.flow->y += kRowsLineH + kRowsGap;
    x.flow->knobName = name;  // (drawn with the value: the name gets the room the value leaves)
    return;
  }
  textC(x.dl, ImGui::GetFont(), kTextFs, x.cellX(c) + spanW(1) * 0.5f, x.knobY(c), pal().textDim, name, spanW(1));
}
void cellValue(const Cx& x, int c, const std::string& v) {
  if (x.rows()) {
    ImFont* f = x.a.fontRegular ? x.a.fontRegular : ImGui::GetFont();
    const float fs = ImGui::GetFontSize(), right = x.o.x + x.w - 2 * kRowsKnobR - 8;
    const float vw = std::min(f->CalcTextSizeA(fs, 1e9f, 0, v.c_str()).x, x.w * 0.45f), ty = std::round(x.flow->knobY + (kRowsLineH - fs) * 0.5f);
    theme::textEllipsis(x.dl, f, fs, ImVec2(std::round(right - vw), ty), vw, pal().textDim, v.c_str());
    theme::textEllipsis(x.dl, f, fs, ImVec2(x.o.x + 2, ty), std::max(10.0f, right - vw - 8 - x.o.x - 2), pal().text, x.flow->knobName.c_str());
    return;
  }
  textC(x.dl, x.a.fontMono ? x.a.fontMono : ImGui::GetFont(), kTextFs, x.cellX(c) + spanW(1) * 0.5f, x.knobY(c) + x.valueOff(),
        gTrkSkin ? trackerview::valueInk() : pal().text, v.c_str(), spanW(1));
}
void knobAt(const Cx& x, int c) {
  if (x.rows()) {
    const float y = x.flow->knobY;
    ImGui::SetCursorScreenPos(ImVec2(x.o.x + x.w - 2 * kRowsKnobR - 1, std::round(y + kRowsLineH * 0.5f - kRowsKnobR)));
    laid(ImVec2(x.o.x, y), ImVec2(x.o.x + x.w, y + kRowsLineH), 0, 1);
    return;
  }
  const float cx = std::round(x.cellX(c) + spanW(1) * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(cx - x.knobR(), x.knobY(c) + x.knobOff()));
  laid(ImVec2(x.cellX(c), x.knobY(c)), ImVec2(x.cellX(c) + spanW(1), x.knobY(c) + x.valueOff() + kTextFs + 3), 0, 1);
}

// A knob over a document parameter (the channel's or the sampler's): its automation, links and
// controller menus are the knob's own.
void knobP(const Cx& x, int c, const char* key, ParamId pid, const char* name, bool bipolar, double step = 0) {
  cellName(x, c, i18n::tr(name));
  knobAt(x, c);
  ImGui::PushID(key);
  if (step > 0) ui::SetNextKnobStep(step);
  ui::ParamKnob("##k", pid, x.knobR(), nullptr, bipolar);
  if (x.rows()) ui::Hint(name);
  ImGui::PopID();
  std::string val;
  if (const auto d = describeParam(x.a.doc.song(), pid))
    if (const auto v = getParam(x.a.doc.song(), pid)) val = d->format(*v);
  cellValue(x, c, val);
}
// A knob over a clip field (one undo step per drag; every selected clip with it).
void knobF(const Cx& x, int c, const char* key, ObjectId clip, clipfields::Field f, double v, double lo, double hi, double def, const char* name,
           const char* unit, bool bipolar, const std::string& valueText, double step = 0) {
  cellName(x, c, i18n::tr(name));
  knobAt(x, c);
  ImGui::PushID(key);
  fieldKnob(x.a, "##k", clip, f, v, lo, hi, def, name, unit, bipolar, step, x.knobR());
  ImGui::PopID();
  cellValue(x, c, valueText);
}
// Places a control `span` cells wide in row `row` (rows: on the open line, at least `need` wide) and
// gives its width.
float placeAt(const Cx& x, int c, int span, int row, float need = 0) {
  if (x.rows()) {
    ImVec2 p;
    const float w = x.place(span, need, p);
    ImGui::SetCursorScreenPos(p);
    return w;
  }
  x.at(c, row);
  return spanW(span);
}
// A button (lit when `on`: a toggle) `span` cells wide in row `row`.
bool button(const Cx& x, int c, int span, int row, const char* label, bool on = false, const char* hint = nullptr) {
  if (x.compact || x.omitted(label)) return false;
  const float w = placeAt(x, c, span, row, labelNeed(label));
  const bool r = ui::TextButton(label, on, w, hint);
  laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), labelNeed(label));
  return r;
}
// One of n (English labels), its segments over `span` cells.
bool seg(const Cx& x, int c, int span, int row, const char* id, const char* const* labels, int n, int* idx, const char* const* hints = nullptr) {
  if (x.compact) return false;
  std::vector<const char*> tl;
  float need = 0;
  for (int k = 0; k < n; ++k) tl.push_back(i18n::tr(labels[k])), need = std::max(need, ImGui::CalcTextSize(tl.back()).x + 8);
  if (x.rows() && need * static_cast<float>(n) + 4 > x.w) {
    // (rows too narrow for every segment: the same choice as a list, as an inspector shows one)
    const float w = placeAt(x, c, 4, row, x.w);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool r = false;
    const int cur = std::clamp(*idx, 0, n - 1);
    if (ui::BeginCombo(id, tl[static_cast<size_t>(cur)], w)) {
      for (int k = 0; k < n; ++k) {
        if (ImGui::Selectable(tl[static_cast<size_t>(k)], k == cur) && k != cur) *idx = k, r = true;
        if (hints && hints[k]) ui::Hint(hints[k]);
      }
      ImGui::EndCombo();
    }
    laid(p, ImVec2(p.x + w, p.y + kRowH), 0);
    return r;
  }
  const float w = placeAt(x, c, x.rows() ? 4 : span, row, x.rows() ? need * static_cast<float>(n) + 4 : 0.0f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float segW = std::floor((w - 4) / static_cast<float>(n));
  const bool r = ui::Segmented(id, tl.data(), n, idx, segW, kRowH, hints);
  laid(p, ImVec2(p.x + w, p.y + kRowH), need > segW ? need * static_cast<float>(n) : 0);
  return r;
}
// The same over a sampler choice (one undo step).
void pseg(const Cx& x, const Channel& ch, int idx, int c, int span, int row, const char* id, const char* const* labels, int n, const char* hint = nullptr) {
  int cur = std::clamp(static_cast<int>(std::lround(smpPar(ch, idx, 0))), 0, n - 1);
  if (seg(x, c, span, row, id, labels, n, &cur)) x.a.doc.setParam(smpPid(ch.id, idx), cur, false);
  if (hint && !x.compact) ui::Hint(hint);
}
// A toggle over a sampler switch (one undo step).
void ptoggle(const Cx& x, const Channel& ch, int idx, int c, int span, int row, const char* label, const char* hint) {
  const bool on = smpPar(ch, idx, 0) >= 0.5f;
  if (button(x, c, span, row, label, on, hint)) x.a.doc.setParam(smpPid(ch.id, idx), on ? 0.0 : 1.0, false);
}
// A typed-in value over a sampler parameter (`scale`: shown as value * scale; one undo step per drag).
void pdrag(const Cx& x, const Channel& ch, int idx, int c, int span, int row, const char* id, float def, double scale, double lo, double hi, const char* fmt) {
  if (x.compact) return;
  App& a = x.a;
  float v = static_cast<float>(smpPar(ch, idx, def) * scale);
  ImGui::SetNextItemWidth(placeAt(x, c, span, row));
  if (ImGui::DragFloat(id, &v, static_cast<float>((hi - lo) / 1000.0), static_cast<float>(lo), static_cast<float>(hi), fmt) && scale > 0)
    a.doc.setParam(smpPid(ch.id, idx), std::clamp(static_cast<double>(v), lo, hi) / scale, true);
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

// A module: its title and scope, its width in cells, its variant, the tab (core/ClipWindow.h) it
// belongs to, its body and its menu of rarer options (none: no menu button).
struct Mod {
  const char* id;
  const char* title;
  MScope scope;
  int cols;
  int variant;
  int tab;
  std::function<void(Cx&)> body;
  std::function<void()> menu;
  bool free = false;  // the body is a widget of its own, laid under the title (it scrolls when taller)
  const char* switchLabel = nullptr;  // one module switch in the title row
  bool switchOn = false;
  std::function<void()> onSwitch;
  // As laid in its tab: the width it was given past its cells (its cells in its middle), its knobs
  // per row when folded (compact, a narrow pane), a free module's narrower width.
  float grow = 0;
  int fold = 0;
  float narrowW = 0;
  Mod(const char* id_, const char* title_, MScope scope_, int cols_, int variant_, int tab_)
      : id(id_), title(title_), scope(scope_), cols(cols_), variant(variant_), tab(tab_) {}
  float w() const { return narrowW > 0 ? narrowW : fold > 0 ? modW(fold) : modW(cols) + grow; }
  int knobRows() const { return fold > 0 ? (cols + fold - 1) / fold : 1; }
  float h(bool compact) const { return compact ? kCompactNameY + static_cast<float>(knobRows()) * kKnobRowH : kModH; }
};

void drawModule(App& a, Mod& m, ImVec2 o, bool compact) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = m.w(), h = m.h(compact);
  gLaid[gLaidHost].emplace_back();
  gLaid[gLaidHost].back().id = m.id, gLaid[gLaidHost].back().a = o, gLaid[gLaidHost].back().b = ImVec2(o.x + w, o.y + h);
  gLaid[gLaidHost].back().compact = compact;
  const ImVec2 p1(o.x + w, o.y + h);
  if (gTrkSkin) {  // the Tracker: a raised bevel box, its title row a raised strip inside it
    trackerview::bevelBox(dl, o, p1, false);
    trackerview::bevelBox(dl, ImVec2(o.x + 3, o.y + 3), ImVec2(p1.x - 3, o.y + kTitleH - 2), false);
  } else {
    // The face one surface step up, the title row a shade darker, a hairline round it.
    dl->AddRectFilled(o, p1, P.dark ? theme::mix(P.surface900, P.surface800, 0.6f) : P.surface800, theme::kRadiusLg);
    dl->AddRectFilled(o, ImVec2(p1.x, o.y + kTitleH), P.dark ? theme::mix(P.surface900, P.surface800, 0.25f) : theme::mix(P.surface800, P.surface700, 0.5f),
                      theme::kRadiusLg, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(o.x, o.y + kTitleH - 0.5f), ImVec2(p1.x, o.y + kTitleH - 0.5f), theme::withAlpha(P.text, P.dark ? 0.07f : 0.12f));
    dl->AddRect(o, p1, theme::withAlpha(P.text, P.dark ? 0.10f : 0.16f), theme::kRadiusLg);
  }
  // The title row: the scope tick, the title, the scope word, the switch, the menu.
  dl->AddRectFilled(ImVec2(o.x + kPad, o.y + 10), ImVec2(o.x + kPad + 3, o.y + 22), scopeCol(m.scope), 1.0f);
  std::string t = i18n::tr(m.title);
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  ImFont* bold = a.fontBold ? a.fontBold : ImGui::GetFont();
  const float ty = std::round(o.y + (kTitleH - kTextFs) * 0.5f);
  dl->AddText(bold, kTextFs, ImVec2(o.x + kPad + 9, ty), P.text, t.c_str());
  const float titleEnd = o.x + kPad + 9 + bold->CalcTextSizeA(kTextFs, 1e9f, 0, t.c_str()).x;
  ImGui::PushID(m.id);
  float rx = o.x + w - kPad + 4;  // (the menu button sits 8 from the edge)
  if (m.menu) {
    rx -= kRowH;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + (kTitleH - kRowH) * 0.5f));
    if (ui::IconButton("##modmenu", ui::Icon::Menu, false, kRowH, "More options")) ImGui::OpenPopup("##modmenupop");
    ImGui::SetNextWindowPos(ImVec2(rx + kRowH, o.y + kTitleH - 2), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##modmenupop")) {
      m.menu();
      ImGui::EndPopup();
    }
    rx -= 4;
  }
  if (m.switchLabel && !compact) {
    const float bw = std::max(spanW(1), ui::TextButtonWidth(m.switchLabel));
    rx -= bw;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + (kTitleH - kRowH) * 0.5f));
    if (ui::TextButton(m.switchLabel, m.switchOn, bw) && m.onSwitch) m.onSwitch();
    rx -= 8;
  }
  {
    const char* word = scopeWord(m.scope);
    const float sw = ImGui::GetFont()->CalcTextSizeA(kTextFs, 1e9f, 0, word).x;
    if (rx - sw - 8 > titleEnd) dl->AddText(ImGui::GetFont(), kTextFs, ImVec2(std::round(rx - sw - 4), ty), scopeText(m.scope), word);
  }
  if (LaidMod* lm = laidNow()) lm->titleEnd = titleEnd, lm->rightStart = rx;
  Cx c{a, dl, o, w, m.variant, compact};
  if (m.fold > 0) c.fold = m.fold;
  else if (m.grow > 0) c.gx = std::floor(m.grow * 0.5f);
  if (m.free) {  // its own widget under the title, in a child of its own (it scrolls when taller)
    const float top = compact ? kTitleH + 6 : kDispY;
    ImGui::SetCursorScreenPos(ImVec2(o.x + kPad, o.y + top));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("##free", ImVec2(w - 2 * kPad, h - top - 8), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground) || gReachOn) m.body(c);
    ImGui::EndChild();
    ImGui::PopStyleVar();
  } else {
    if (!compact && m.variant != 3) well(dl, c.d0(), c.d1());
    m.body(c);
  }
  ImGui::PopID();
}


// ---- the displays

// The sample's whole file in [p0, p1] (its notes' and clips' file), quiet.
void sampleWaveIn(ImDrawList* dl, const Channel& ch, ImVec2 p0, ImVec2 p1, float alpha = 0.55f) {
  const std::string path = samplePath(ch);
  const SamplePtr smp = path.empty() ? nullptr : sampleCache().request(path);
  if (!smp) return;
  const auto& P = pal();
  fileWave(dl, *smp, p0.x + 4, p0.y + 4, p1.x - 4, p1.y - 4, theme::withAlpha(theme::mix(theme::content(ch.color), P.text, 0.3f), alpha), true);
}
// A vertical marker at `frac` of [p0, p1].
void markAt(ImDrawList* dl, ImVec2 p0, ImVec2 p1, double frac, ImU32 col) {
  const float x = std::round(p0.x + 4 + static_cast<float>(std::clamp(frac, 0.0, 1.0)) * (p1.x - p0.x - 8)) + 0.5f;
  dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), col, 1.5f);
}

// ---- Clip

void takesMenu(App& a, const Clip& c) {
  const Song& s2 = a.doc.song();
  if (c.takeGroup == kNoId) {
    ImGui::MenuItem(i18n::tr("No takes yet"), nullptr, false, false);
    ui::Hint("Record over this clip with take lanes on: its takes and their ratings show here");
  } else {
    const std::vector<int> ks = takesInGroup(s2, c.takeGroup);
    char cur[48];
    std::snprintf(cur, sizeof cur, i18n::tr("Take %d of %d"), c.take + 1, static_cast<int>(ks.size()));
    if (ImGui::BeginMenu(cur)) {
      for (int k : ks) {
        char nm[32];
        std::snprintf(nm, sizeof nm, i18n::tr("Take %d"), k + 1);
        if (ImGui::MenuItem(nm, nullptr, k == c.take)) {
          const ObjectId gid = c.takeGroup;
          const Tick a0 = c.start, b0 = c.start + s2.clipLength(c);
          a.doc.editSong("comp take", [&](Song& xs) { compTakes(xs, gid, a0, b0, k); });
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(i18n::tr("Rating"))) {
      static const char* const kStars[] = {"No rating", "1 star", "2 stars", "3 stars"};
      for (int r = 0; r <= 3; ++r)
        if (ImGui::MenuItem(i18n::tr(kStars[r]), nullptr, r == c.takeRating) && r != c.takeRating) {
          const ObjectId gid = c.takeGroup;
          const int take = c.take;
          a.doc.editSong("take rating", [&](Song& xs) { takes::setRating(xs, gid, take, r); });
        }
      ImGui::EndMenu();
    }
  }
  const int tr = c.track;
  const bool shown = tr >= 0 && static_cast<size_t>(tr) < s2.tracks.size() && s2.tracks[static_cast<size_t>(tr)].takeLanes;
  if (ImGui::MenuItem(i18n::tr("Show take lanes"), nullptr, shown))
    a.doc.editPlaylistTrack(tr, shown ? "hide take lanes" : "show take lanes", [shown](PlaylistTrack& t) { t.takeLanes = !shown; });
}

Mod clipModule(App& a, const Clip& c, const Channel& ch) {
  using clipfields::Field;
  Mod m("clip", "Clip", MScope::Clip, 3, 0, TabClip);
  m.body = [&a, &c, &ch](Cx& x) {
    for (const char* k : {"gain", "normgain", "reverse.clip", "mute", "semis", "cents", "loopclip", "takes"}) reach(k);
    const Song& s2 = a.doc.song();
    const double db = std::max(-60.0, clipgain::toDb(c.gain));
    char gainText[32];
    std::snprintf(gainText, sizeof gainText, "%+.1f dB", db);
    if (x.disp()) {
      sampleWaveIn(x.dl, ch, x.d0(), x.d1(), c.muted ? 0.25f : 0.55f);
      dispNote(x, x.d0(), x.d1(), gainText, true, true);
    }
    if (button(x, 0, 2, 0, "Normalize gain", false, "The clip's gain so its audio peaks at 0 dB (the file is not changed)")) {
      const ObjectId id = c.id;
      a.doc.editSong("normalize clip", [&](Song& xs) { normalizeClip(xs, id, 0.0); });
    }
    if (button(x, 2, 1, 0, "Reverse", c.reverse, "Plays this clip backwards (the sample, its notes and the other clips are not changed)"))
      setField(a, c.id, Field::Reverse, c.reverse ? 0 : 1, false);
    if (button(x, 0, 1, 1, "Mute", c.muted, "Mutes this clip")) setField(a, c.id, Field::Mute, c.muted ? 0 : 1, false);
    if (button(x, 1, 1, 1, "Loop", c.loop, "Loop the clip: it repeats its audio to its end")) setField(a, c.id, Field::Loop, c.loop ? 0 : 1, false);
    if (!x.compact) {  // where the first pass starts (bar.beat.step)
      const Tick bar = std::max<Tick>(1, s2.barTicks()), beat = std::max<Tick>(1, s2.beatTicks()), six = std::max<Tick>(1, beat / 4);
      char b[48];
      std::snprintf(b, sizeof b, "%lld.%lld.%lld", static_cast<long long>(c.offset / bar + 1), static_cast<long long>(c.offset % bar / beat + 1),
                    static_cast<long long>(c.offset % beat / six + 1));
      int sixteenths = static_cast<int>(c.offset / six);
      const std::string shown = x.rows() ? std::string(i18n::tr("Loop start")) + "  " + b : std::string(b);
      ImGui::SetNextItemWidth(placeAt(x, 2, 1, 1, x.rows() ? ImGui::CalcTextSize(shown.c_str()).x + 16 : 0.0f));
      ImGui::BeginDisabled(!c.loop);
      if (ImGui::DragInt("##loopstart", &sixteenths, 0.2f, 0, 4096, shown.c_str()) && c.loop) setField(a, c.id, Field::LoopStart, static_cast<double>(sixteenths * six));
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      ImGui::EndDisabled();
      ui::Hint("Loop start: where the first pass starts in the clip's audio (drag; Reset loop start is in the menu)");
      laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    knobF(x, 0, "gain", c.id, Field::GainDb, db, -60, 24, 0, "Gain", "dB", true, gainText);
    const double semis = clipfields::get(s2, c, Field::Semitones), cents = clipfields::get(s2, c, Field::Cents);
    char b1[24], b2[24];
    std::snprintf(b1, sizeof b1, "%+d st", static_cast<int>(semis));
    std::snprintf(b2, sizeof b2, "%+d ct", static_cast<int>(std::lround(cents)));
    knobF(x, 1, "semi", c.id, Field::Semitones, semis, -24, 24, 0, "Semitones", "st", true, b1, 1.0);
    knobF(x, 2, "cents", c.id, Field::Cents, cents, -50, 50, 0, "Cents", "ct", true, b2, 1.0);
  };
  m.menu = [&a, &c] {
    if (ImGui::MenuItem(i18n::tr("Reset loop start"))) setField(a, c.id, clipfields::Field::LoopStart, 0, false);
    ui::Hint("The first pass starts at the start of the clip's audio");
    ImGui::Separator();
    takesMenu(a, c);
  };
  return m;
}

Mod fadesModule(App& a, const Clip& c) {
  using clipfields::Field;
  Mod m("fades", "Fades", MScope::Clip, 4, 0, TabClip);
  m.body = [&a, &c](Cx& x) {
    reach("fades");
    reach("autoxfade");
    const auto& P = pal();
    if (x.disp()) {  // the clip's level over its length: its two fades
      const ImVec2 d0 = x.d0(), d1 = x.d1();
      static TempoMap tm;
      tm.build(a.doc.song());
      const double len = std::max(1e-3, tm.secondsAt(static_cast<double>(c.start + a.doc.song().clipLength(c))) - tm.secondsAt(static_cast<double>(c.start)));
      const bool low = d1.y - d0.y < 64;  // (a short display: the labels sit under the line, inside the level)
      const float x0 = d0.x + 6, x1 = d1.x - 6, top = d0.y + (low ? 6 : 16), bot = d1.y - 4;
      const float fi = static_cast<float>(std::clamp(c.fadeIn / len, 0.0, 0.5)), fo = static_cast<float>(std::clamp(c.fadeOut / len, 0.0, 0.5));
      std::vector<ImVec2> pts;
      auto shape = [](double t, int sh, double tension) {  // 0..1 along a fade: its level
        if (sh == 1) return t;
        const double k = std::clamp(static_cast<double>(fadeTensionToUi(tension)), -1.0, 1.0);
        return k >= 0 ? std::pow(t, 1.0 / (1.0 + 2.0 * k)) : std::pow(t, 1.0 - 2.0 * k);
      };
      const float fiX = x0 + fi * (x1 - x0), foX = x1 - fo * (x1 - x0);
      for (int k = 0; k <= 16; ++k) {
        const double t = k / 16.0;
        pts.emplace_back(x0 + static_cast<float>(t) * (fiX - x0), bot - static_cast<float>(shape(t, c.fadeInShape, c.fadeInTension)) * (bot - top));
      }
      for (int k = 0; k <= 16; ++k) {
        const double t = k / 16.0;
        pts.emplace_back(foX + static_cast<float>(t) * (x1 - foX), bot - static_cast<float>(shape(1.0 - t, c.fadeOutShape, c.fadeOutTension)) * (bot - top));
      }
      const ImU32 col = scopeCol(MScope::Clip);
      for (size_t k = 1; k < pts.size(); ++k)
        x.dl->AddQuadFilled(ImVec2(pts[k - 1].x, bot), pts[k - 1], pts[k], ImVec2(pts[k].x, bot), theme::withAlpha(col, 0.14f));
      x.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, 0, 1.5f);
      // the fade handles at the line's top corners, as on the clip
      for (const float hx : {fiX, foX}) x.dl->AddRectFilled(ImVec2(hx - 3, top - 3), ImVec2(hx + 3, top + 3), col, 1.0f);
      char b[32];
      if (low) {
        ImFont* f = x.a.fontMono ? x.a.fontMono : ImGui::GetFont();
        const float ty = std::round(bot - kTextFs - 3);
        std::snprintf(b, sizeof b, "%s %.0f ms", i18n::tr("In"), c.fadeIn * 1000.0);
        x.dl->AddText(f, kTextFs, ImVec2(x0 + 6, ty), P.textDim, b);
        std::snprintf(b, sizeof b, "%s %.0f ms", i18n::tr("Out"), c.fadeOut * 1000.0);
        x.dl->AddText(f, kTextFs, ImVec2(std::round(x1 - 6 - f->CalcTextSizeA(kTextFs, 1e9f, 0, b).x), ty), P.textDim, b);
      } else {
        std::snprintf(b, sizeof b, "%.0f ms", c.fadeIn * 1000.0);
        dispNote(x, d0, d1, b);
        std::snprintf(b, sizeof b, "%.0f ms", c.fadeOut * 1000.0);
        dispNote(x, d0, d1, b, true);
      }
    }
    for (int in = 1; in >= 0; --in) {
      ImGui::PushID(in);
      const int c0 = in ? 0 : 2;
      static const char* const shapes[] = {"Smooth", "Straight"};
      int sh = in ? c.fadeInShape : c.fadeOutShape;
      if (seg(x, c0, 2, 0, "##shape", shapes, 2, &sh)) setField(a, c.id, in ? Field::FadeInShape : Field::FadeOutShape, sh, false);
      if (!x.compact) {
        float ms = static_cast<float>((in ? c.fadeIn : c.fadeOut) * 1000.0);
        ImGui::SetNextItemWidth(placeAt(x, c0, x.rows() ? 4 : 2, 1));  // (rows: the whole line)
        if (ImGui::DragFloat("##ms", &ms, 1.0f, 0.0f, 60000.0f, in ? i18n::tr("In %.0f ms") : i18n::tr("Out %.0f ms")))
          setField(a, c.id, in ? Field::FadeIn : Field::FadeOut, std::max(0.0f, ms) / 1000.0);
        if (ImGui::IsItemDeactivated()) a.doc.endGesture();
        ui::Hint(in ? "Fade in length (drag)" : "Fade out length (drag)");
        laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      }
      const double cv = fadeTensionToUi(in ? c.fadeInTension : c.fadeOutTension);
      char b[24];
      std::snprintf(b, sizeof b, "%+.0f %%", cv * 100);
      knobF(x, c0, "curve", c.id, in ? Field::FadeInCurve : Field::FadeOutCurve, cv, -1, 1, 0, in ? "In curve" : "Out curve", "", true, b);
      ImGui::PopID();
    }
  };
  m.switchLabel = "Auto crossfade";
  m.switchOn = !c.fadeInManual && !c.fadeOutManual;
  m.onSwitch = [&a, &c] {
    const bool on = !(!c.fadeInManual && !c.fadeOutManual);
    a.doc.editClip(c.id, on ? "automatic fades" : "fades set by hand", [on](Clip& k) {
      if (on) k.fadeIn = k.fadeOut = 0, k.fadeInManual = k.fadeOutManual = false;
      else k.fadeInManual = k.fadeOutManual = true;
    });
  };
  return m;
}

// ---- Warp (the clip's tempo and markers) with the sample's stretch engine beside it

// The stretch engine's part: cells c0 .. c0 + 3 (orange: it changes every clip of the sample and its notes).
void stretchPart(const Cx& x, int c0, const Clip* c, const Channel& ch) {
  App& a = x.a;
  reach("engine");
  reach("pitchshift");
  reach("timestretch");
  reach("fittempo");
  const stretch::Mode cur = stretch::choiceOf(stretch::modeOf(smpPar(ch, pp::SmpStretchMode, 0)));
  if (x.disp()) {
    const ImVec2 q0 = x.dp0(c0), q1 = x.dp1(c0, 4);
    const int n = c ? clipsSharing(a.doc.song(), ch.id) : 0;
    char note[96];
    if (n > 1) std::snprintf(note, sizeof note, i18n::tr("Stretch engine: the sample's, %d clips"), n);
    else std::snprintf(note, sizeof note, "%s", i18n::tr("Stretch engine: the sample's"));
    if (c) dispNote(x, q0, q1, note, false, false, scopeText(MScope::Sample));  // (in Warp: the part that is the sample's)
    const double from = smpPar(ch, pp::SmpFitTempo, 0);
    char b[64];
    if (from > 0) std::snprintf(b, sizeof b, i18n::tr("follows the tempo from %.1f BPM"), from);
    else std::snprintf(b, sizeof b, "%s", i18n::tr("fixed: keeps its own tempo"));
    dispNote(x, q0, q1, b, false, true);
  }
  const char* names[3] = {stretch::modeName(stretch::kChoices[0]), stretch::modeName(stretch::kChoices[1]), stretch::modeName(stretch::kChoices[2])};
  const char* hints[3] = {stretch::modeHint(stretch::kChoices[0]), stretch::modeHint(stretch::kChoices[1]), stretch::modeHint(stretch::kChoices[2])};
  int idx = cur == stretch::kChoices[1] ? 1 : cur == stretch::kChoices[2] ? 2 : 0;
  if (seg(x, c0, 4, 0, "##ceeng", names, 3, &idx, hints)) {
    const int pick = static_cast<int>(stretch::kChoices[idx]);
    if (c) {
      const ObjectId id = c->id;
      const double sf = c->stretch;
      a.doc.editSong("stretch mode", [&](Song& xs) { setClipStretchMode(xs, id, sf, pick); });
    } else {
      a.doc.setParam(smpPid(ch.id, pp::SmpStretchMode), pick, false);
    }
  }
  pdrag(x, ch, pp::SmpFitTempo, c0, 2, 1, "##fit", 0, 1.0, 0, 1000,
        smpPar(ch, pp::SmpFitTempo, 0) > 0 ? i18n::tr("from %.1f BPM") : i18n::tr("fixed"));
  if (!x.compact) ui::Hint("Set, the sample follows the tempo: a stretch keeps its beats at any tempo (0: fixed). Fit to tempo sets it");
  knobP(x, c0, "pshift", smpPid(ch.id, pp::SmpPitchShift), "Pitch shift", true, 1.0);
  knobP(x, c0 + 1, "tstretch", smpPid(ch.id, pp::SmpTimeStretch), "Time stretch", false);
}

Mod warpModule(App& a, const Clip& c, const Channel& ch) {
  using clipfields::Field;
  Mod m("warp", "Warp", MScope::Clip, 10, 0, TabTime);
  m.body = [&a, &c, &ch](Cx& x) {
    for (const char* k : {"tempo", "stretch", "speed", "warpops"}) reach(k);
    const Song& s = a.doc.song();
    static TempoMap tm;
    tm.build(s);
    const Tick grid = std::max<Tick>(1, s.beatTicks() / 4);
    const double strength = std::clamp(a.settings.getNumber("playlist.quantizeStrength"), 0.0, 1.0);
    if (x.disp()) {  // the clip's part of the display: its sample, its markers, its tempo; a rule before the engine's
      const ImVec2 q0 = x.dp0(0), q1 = x.dp1(0, 6);
      sampleWaveIn(x.dl, ch, q0, q1, 0.45f);
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("%d markers"), static_cast<int>(c.warp.size()));
      dispNote(x, q0, q1, b, true, false, c.warp.empty() ? 0 : theme::rgb(theme::role::kWarp));
      if (c.syncBpm > 0) std::snprintf(b, sizeof b, i18n::tr("source %.1f BPM"), c.syncBpm);
      else std::snprintf(b, sizeof b, "%s", i18n::tr("own tempo"));
      dispNote(x, q0, q1, b);
      const float rx = std::round(x.cellX(6) - kGut * 0.5f) + 0.5f;
      if (!x.rows()) x.dl->AddLine(ImVec2(rx, x.d0().y + 4), ImVec2(rx, x.rowY(1) + kRowH), theme::withAlpha(pal().text, 0.10f));
    }
    const bool follow = c.syncBpm > 0;
    if (button(x, 0, 2, 0, "Follow tempo", follow, "On: the clip keeps its bars when the tempo changes. Off: it keeps its audio and its length follows the tempo"))
      setField(a, c.id, Field::FollowTempo, follow ? 0 : 1, false);
    if (button(x, 2, 2, 0, "Fit to tempo", false, "Guess the number of beats and make the clip follow the tempo")) {
      const ObjectId id = c.id;
      int beats = 0;
      a.doc.editSong("fit clip to tempo", [&](Song& xs) { beats = fitClipToTempo(xs, id); });
      if (beats > 0) a.notify("Fitted to " + std::to_string(beats) + (beats == 1 ? " beat" : " beats") + "; the clip now follows the tempo");
    }
    if (button(x, 4, 1, 0, "Half", false, "Half as long")) setField(a, c.id, Field::Stretch, c.stretch * 0.5, false);
    if (button(x, 5, 1, 0, "Twice", false, "Twice as long")) setField(a, c.id, Field::Stretch, c.stretch * 2.0, false);
    if (button(x, 0, 1, 1, "Original", false, "The clip's own length again")) {
      const ObjectId id = c.id;
      a.doc.editSong("stretch clip", [&](Song& xs) { resetClipStretch(xs, id); });
    }
    if (button(x, 1, 2, 1, "Add at transients", false, "Warp markers at the clip's transients")) {
      Clip after = c;
      const int n = warpMarkersAtTransients(s, after, tm);
      if (n > 0) a.doc.editClip(c.id, "warp markers at transients", [&after](Clip& k) { k.warp = after.warp; });
      a.setHint(n > 0 ? std::to_string(n) + " " + i18n::tr("warp markers added") : std::string(i18n::tr("No transients found")));
    }
    if (button(x, 3, 2, 1, "Quantize to grid", false, "Moves the warp markers onto the grid by the strength")) {
      Clip after = c;
      const int n = quantizeAudioClip(s, after, tm, grid, strength);
      if (!(after.warp == c.warp)) a.doc.editClip(c.id, "quantize audio", [&after](Clip& k) { k.warp = after.warp; });
      a.setHint(std::to_string(n) + " " + i18n::tr("warp markers quantized"));
    }
    if (button(x, 5, 1, 1, "Clear", false, "Removes every warp marker: the clip plays at its own speed again") && !c.warp.empty())
      a.doc.editClip(c.id, "clear warp markers", [](Clip& k) { k.warp.clear(); });
    char b[32];
    std::snprintf(b, sizeof b, "x %.3f", c.stretch);
    knobF(x, 0, "stretch", c.id, Field::Stretch, c.stretch, 0.0625, 16.0, 1.0, "Stretch", "x", false, b);
    const double sp = clipfields::get(s, c, Field::Speed);
    std::snprintf(b, sizeof b, "%.1f %%", sp);
    knobF(x, 1, "speed", c.id, Field::Speed, sp, 6.25, 1600.0, 100.0, "Speed", "%", false, b);
    {  // the quantize strength (100 lands the markers on the grid, 50 halfway), in steps of 25
      cellName(x, 2, i18n::tr("Strength"));
      knobAt(x, 2);
      ParamDesc d;
      d.name = "Strength";
      d.unit = "%";
      d.min = 25, d.max = 100, d.def = 100;
      double v = std::round(strength * 4.0) * 25.0;
      v = std::clamp(v, 25.0, 100.0);
      ui::SetNextKnobStep(25.0);
      if (ui::Knob("##qstr", &v, d, x.knobR(), nullptr, false)) a.settings.set("playlist.quantizeStrength", std::clamp(v, 25.0, 100.0) / 100.0);
      ui::Hint("Quantize strength: 100 lands the markers on the grid, 50 moves them halfway");
      std::snprintf(b, sizeof b, "%.0f %%", v);
      cellValue(x, 2, b);
    }
    stretchPart(x, 6, &c, ch);
  };
  m.menu = [&a, &c] {
    static TempoMap tm;
    tm.build(a.doc.song());
    const double strength = std::clamp(a.settings.getNumber("playlist.quantizeStrength"), 0.0, 1.0);
    if (ImGui::MenuItem(i18n::tr("Swing to the song's groove"))) grooveui::swingClip(a, c.id, tm, strength);
    ui::Hint("Warp markers on the transients, moved onto the song's swung grid");
    if (ImGui::MenuItem(i18n::tr("Take its groove as the swing"))) {
      if (grooveui::fromClip(a, c.id, tm)) a.showGroove = true;
    }
    ui::Hint("The timing and accents of the clip's transients become the song's swing");
    bool snap = a.settings.getBool("playlist.warpSnapTransients");
    if (ImGui::MenuItem(i18n::tr("New markers snap to transients"), nullptr, snap)) a.settings.set("playlist.warpSnapTransients", !snap);
  };
  return m;
}

// The stretch engine alone (a sampler channel no clip plays).
Mod stretchModule(App& a, const Channel& ch) {
  Mod m("stretch", "Stretch engine", MScope::Sample, 4, 0, TabSample);
  m.body = [&ch](Cx& x) { stretchPart(x, 0, nullptr, ch); };
  (void)a;
  return m;
}

// ---- the sample


Mod sampleModule(App& a, const Clip* c, const Channel& ch) {
  Mod m("sample", "Sample", MScope::Sample, 4, 0, TabSample);
  m.body = [c, &ch](Cx& x) {
    for (const char* k : {"file", "startend", "removedc", "polarity", "swapstereo", "normalize.sample", "smpfades"}) reach(k);
    if (c) reach("unique");
    const ClipSource src = clipSourceIfReady(ch);
    const double secs = src.sample ? src.sample->seconds() : 0.0;
    double sf = 0, ef = 1;
    channelTrim(ch, sf, ef);
    if (x.disp()) {  // the file, its start and end (the blue handles), and what it is
      const ImVec2 d0 = x.d0(), d1 = x.d1();
      const std::string path = samplePath(ch);
      sampleWaveIn(x.dl, ch, ImVec2(d0.x, d0.y + 12), d1, 0.5f);
      markAt(x.dl, ImVec2(d0.x, d0.y + 12), d1, sf, handleBlue());
      markAt(x.dl, ImVec2(d0.x, d0.y + 12), d1, ef, handleBlue());
      std::string line = path.empty() ? std::string(i18n::tr("No sample")) : fs::path(path).filename().string();
      ImU32 col = 0;
      if (src.sample) {
        char b[96];
        std::snprintf(b, sizeof b, " \xC2\xB7 %.1f kHz \xC2\xB7 %s", src.sample->sampleRate / 1000.0, src.sample->mono() ? i18n::tr("mono") : i18n::tr("stereo"));
        line += b;
      } else if (!path.empty()) {
        std::error_code ec;
        const bool there = fs::exists(fs::u8path(path), ec);
        line += std::string(" \xC2\xB7 ") + (there ? i18n::tr("Reading the sample...") : i18n::tr("missing"));
        if (!there) col = pal().danger;
      }
      dispNote(x, d0, d1, line, false, false, col);
      // 0.7.8: the sample's own Normalize switch is gone. A project that has it on keeps sounding the
      // same and says so here; the module menu makes it a normalized file instead (one undo step).
      if (smpPar(ch, pp::SmpNormalize, 0) >= 0.5f) dispNote(x, d0, d1, i18n::tr("Plays normalized (an older setting): see the menu"), false, true, sampleOrange());
    }
    ptoggle(x, ch, pp::SmpRemoveDc, 0, 2, 0, "Remove DC", "Removes a constant offset from the sample");
    ptoggle(x, ch, pp::SmpPolarity, 2, 2, 0, "Invert polarity", "Turns the sample's waveform upside down");
    ptoggle(x, ch, pp::SmpSwapStereo, 0, 2, 1, "Swap stereo", "Swaps the sample's left and right");
    if (!x.compact) {  // start and end, in seconds (the blue handles under the waveform)
      App& aa = x.a;
      float s0 = static_cast<float>(sf * secs), s1 = static_cast<float>(ef * secs);
      ImGui::SetNextItemWidth(placeAt(x, 2, 1, 1, x.rows() ? 100.0f : 0.0f));
      if (ImGui::DragFloat("##ss", &s0, 0.001f, 0.0f, static_cast<float>(secs), x.rows() ? i18n::tr("Start %.3f s") : "%.3f s") && secs > 0)
        aa.doc.setParam(smpPid(ch.id, pp::SmpStart), clampTrimHandle(false, s0 / secs, ef, secs), true);
      if (ImGui::IsItemDeactivated()) aa.doc.endGesture();
      ui::Hint("Sample start: the blue handle under the waveform. Trims the sample's notes and every clip of it");
      laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      ImGui::SetNextItemWidth(placeAt(x, 3, 1, 1, x.rows() ? 100.0f : 0.0f));
      if (ImGui::DragFloat("##se", &s1, 0.001f, 0.0f, static_cast<float>(secs), x.rows() ? i18n::tr("End %.3f s") : "%.3f s") && secs > 0)
        aa.doc.setParam(smpPid(ch.id, pp::SmpEnd), clampTrimHandle(true, s1 / secs, sf, secs), true);
      if (ImGui::IsItemDeactivated()) aa.doc.endGesture();
      ui::Hint("Sample end: the blue handle under the waveform. Trims the sample's notes and every clip of it");
      laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    knobP(x, 0, "sfin", smpPid(ch.id, pp::SmpFadeIn), "Fade in", false);
    knobP(x, 1, "sfout", smpPid(ch.id, pp::SmpFadeOut), "Fade out", false);
  };
  m.menu = [&a, c, &ch] {
    if (ImGui::MenuItem(i18n::tr("Replace sample..."))) replaceSampleDialog(a, ch);
    ui::Hint("Choose another audio file for the sample (every clip of it and its notes); or drop a file on the waveform");
    if (c) {
      const ObjectId id = c->id;
      if (ImGui::MenuItem(i18n::tr("Make this clip's sample unique"), nullptr, false, clipsSharing(a.doc.song(), ch.id) > 1))
        a.later([&a, id] {
          if (ops::makeAudioClipUnique(a.doc, id) != kNoId) a.setHint(i18n::tr("The clip has its own sample now"));
        });
      ui::Hint("This clip gets its own copy of the sample, so the sample's settings change it alone");
    }
    if (smpPar(ch, pp::SmpNormalize, 0) >= 0.5f) {
      ImGui::Separator();
      const ObjectId chId = ch.id;  // (the whole file, as the older switch normalized it)
      if (ImGui::MenuItem(i18n::tr("Make it a normalized file")))
        a.later([&a, chId] {
          a.doc.beginGroup("normalize sample file");
          runAudioOp(a, kNoId, clipedit::Op::Normalize, clipedit::Args{}, 0, 0, chId);
          a.doc.setParam(smpPid(chId, pp::SmpNormalize), 0.0, false);
          a.doc.endGroup();
        });
      ui::Hint("Writes the sample normalized to a new file and turns the older setting off (one undo step)");
    }
  };
  return m;
}

// The sample's own warp (the sampler's well): markers that stretch the sample its notes play.
Mod sampleWarpModule(App& a, const Channel& ch) {
  Mod m("smpwarp", "Sample warp", MScope::Sample, 4, 3, TabTime);
  m.body = [&a, &ch](Cx& x) {
    reach("samplewarp");
    if (!x.disp()) return;
    const std::string path = samplePath(ch);
    const SamplePtr smp = path.empty() ? nullptr : sampleCache().request(path);
    const auto& P = pal();
    const ImVec2 q = x.d0(), q1 = x.d1();
    x.dl->AddRectFilled(q, q1, P.well, 3);
    laid(q, q1, 0, 2);
    if (!smp) {
      dispNote(x, q, q1, i18n::tr("Reading the sample..."));
      return;
    }
    const SampleWarp sw = sampleWarpOf(ch.plugin);
    if (const std::shared_ptr<const Sample> shown = warpwave::shownSample(sw, smp))
      fileWave(x.dl, *shown, q.x + 4, q.y + 4, q1.x - 4, q1.y - 4, theme::mix(theme::content(ch.color), P.text, 0.3f), true);
    ImGui::SetCursorScreenPos(q);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##smpwarpbg", ImVec2(q1.x - q.x, q1.y - q.y));
    if (ImGui::IsItemHovered() && !sw.on)
      a.setHint(i18n::tr("The sample's warp: turn Warp on to place markers that stretch the sample its notes play; right-click for the Warp menu"));
    warpwave::sampleWell(a, ch.id, "##cesmpwarpw", q, q1, smp, theme::rgb(theme::role::kWarp));
    if (!sw.on) dispNote(x, q, q1, i18n::tr("the notes' warp: right-click for the Warp menu"), false, true);
  };
  return m;
}

// ---- Playback and Voice

Mod playbackModule(App& a, const Channel& ch) {
  Mod m("playback", "Playback", MScope::Sample, 4, 0, TabPlayback);
  m.body = [&ch](Cx& x) {
    for (const char* k : {"oneshot", "reverse.notes", "loop.notes", "declick"}) reach(k);
    if (x.disp()) {  // the file, its notes' loop
      const ImVec2 d0 = x.d0(), d1 = x.d1();
      sampleWaveIn(x.dl, ch, d0, d1, 0.45f);
      const bool loop = smpPar(ch, pp::SmpLoop, 0) >= 0.5f;
      if (loop) {
        const ImU32 col = sampleOrange();
        const double ls = smpPar(ch, pp::SmpLoopStart, 0), le = smpPar(ch, pp::SmpLoopEnd, 1);
        const float lx = d0.x + 4 + static_cast<float>(std::clamp(ls, 0.0, 1.0)) * (d1.x - d0.x - 8);
        const float rx = d0.x + 4 + static_cast<float>(std::clamp(le, 0.0, 1.0)) * (d1.x - d0.x - 8);
        x.dl->AddRectFilled(ImVec2(lx, d0.y + 2), ImVec2(rx, d1.y - 2), theme::withAlpha(col, 0.12f));
        markAt(x.dl, d0, d1, ls, col);
        markAt(x.dl, d0, d1, le, col);
      }
      const bool one = smpPar(ch, pp::SmpOneShot, 0) >= 0.5f;
      dispNote(x, d0, d1, loop ? i18n::tr("notes loop while held") : one ? i18n::tr("one shot") : i18n::tr("notes play while held"));
    }
    ptoggle(x, ch, pp::SmpLoop, 0, 2, 0, "Loop", "Held notes repeat the loop between the loop points");
    static const char* const modes[] = {"Forward", "Ping-pong"};
    pseg(x, ch, pp::SmpLoopMode, 2, 2, 0, "##lmode", modes, 2);
    ptoggle(x, ch, pp::SmpOneShot, 0, 2, 1, "One shot", "Each note plays the sample to its end, however short the note");
    ptoggle(x, ch, pp::SmpReverse, 2, 2, 1, "Reverse sample",
            "The sampler plays the sample backwards: its notes and every clip of it (a clip's own Reverse turns that clip round again)");
    knobP(x, 0, "lst", smpPid(ch.id, pp::SmpLoopStart), "Loop start", false);
    knobP(x, 1, "len", smpPid(ch.id, pp::SmpLoopEnd), "Loop end", false);
    knobP(x, 2, "xf", smpPid(ch.id, pp::SmpLoopXfade), "Crossfade", false);
    knobP(x, 3, "declick", smpPid(ch.id, pp::SmpDeclick), "Declick", false, 1.0);
  };
  (void)a;
  return m;
}


Mod voiceModule(App& a, const Channel& ch) {
  Mod m("voice", "Voice", MScope::Sample, 6, 0, TabPlayback);
  m.body = [&a, &ch](Cx& x) {
    reach("root");
    reach("voice");
    reach("audition");
    if (x.disp()) {  // the root note (a short display: on one line)
      const ImVec2 d0 = x.d0(), d1 = x.d1();
      const std::string r = noteLabel(static_cast<int>(std::lround(smpPar(ch, pp::SmpRoot, 60))));
      const float lh = std::min(28.0f, d1.y - d0.y - 8), ly = std::round(d0.y + (d1.y - d0.y - lh) * 0.5f);
      ui::Lcd(x.dl, ImVec2(d0.x + 8, ly), ImVec2(72, lh), r.c_str(), false, false);
      const bool two = d1.y - d0.y >= 46;
      x.dl->AddText(ImGui::GetFont(), kTextFs, ImVec2(d0.x + 88, two ? ly : std::round(ly + (lh - kTextFs) * 0.5f)), pal().textDim, i18n::tr("Root note"));
      if (two) x.dl->AddText(ImGui::GetFont(), kTextFs, ImVec2(d0.x + 88, ly + 15), pal().textFaint, i18n::tr("right-click a key below to set it"));
    }
    if (!x.compact) {
      // The audition keyboard across both button rows.
      const ImVec2 k0 = x.rows() ? x.block(44) : ImVec2(x.cellX(0), x.rowY(0));
      const float kh = x.rows() ? 44.0f : x.rowY(1) + kRowH - x.rowY(0), kw = x.rows() ? x.w : spanW(6);
      ImGui::SetCursorScreenPos(k0);
      auditionKeys(a, ch, kw, kh);
      laid(k0, ImVec2(k0.x + kw, k0.y + kh));
    }
    int k = 0;
    for (auto [key, idx, name, bip, step] : {std::tuple<const char*, int, const char*, bool, double>{"root", pp::SmpRoot, "Root", false, 1.0},
                                             {"fine", pp::SmpFine, "Fine", true, 1.0},
                                             {"npan", pp::SmpPan, "Pan", true, 0.0},
                                             {"voices", pp::SmpVoices, "Voices", false, 1.0},
                                             {"glide", pp::SmpGlide, "Glide", false, 0.0},
                                             {"nvol", pp::SmpVolume, "Volume", false, 0.0}})
      knobP(x, k++, key, smpPid(ch.id, idx), name, bip, step);
  };
  return m;
}

// ---- Envelope, Filter, LFO


Mod envelopeModule(App& a, const Channel& ch) {
  Mod m("env", "Envelope", MScope::Sample, 7, 2, TabEnvelope);
  m.body = [&ch](Cx& x) {
    reach("volenv");
    if (x.disp()) {
      const double ts[6] = {smpPar(ch, pp::SmpEnvDelay, 0), smpPar(ch, pp::SmpAttack, 0.0005f), smpPar(ch, pp::SmpEnvHold, 0),
                            smpPar(ch, pp::SmpDecay, 10), 0.5, smpPar(ch, pp::SmpRelease, 0.02f)};
      envGraph(x.d0(), x.d1(), ts, 6, smpPar(ch, pp::SmpSustain, 1), sampleOrange());
    }
    int k = 0;
    for (auto [key, idx, name] : {std::tuple<const char*, int, const char*>{"edel", pp::SmpEnvDelay, "Delay"},
                                  {"eatt", pp::SmpAttack, "Attack"},
                                  {"ehold", pp::SmpEnvHold, "Hold"},
                                  {"edec", pp::SmpDecay, "Decay"},
                                  {"esus", pp::SmpSustain, "Sustain"},
                                  {"erel", pp::SmpRelease, "Release"},
                                  {"evel", pp::SmpVelVolume, "Velocity"}})
      knobP(x, k++, key, smpPid(ch.id, idx), name, false);
  };
  (void)a;
  return m;
}

// The filter and its envelope in one module: the type, its curve and its envelope's shape.
Mod filterModule(App& a, const Channel& ch) {
  Mod m("filter", "Filter", MScope::Sample, 9, 1, TabFilter);
  m.body = [&ch](Cx& x) {
    reach("filter");
    if (x.disp()) {
      const ImVec2 f0 = x.dp0(0), f1 = x.dp1(0, 5), e0 = x.dp0(5), e1 = x.dp1(5, 4);
      filterGraph(f0, f1, static_cast<int>(std::lround(smpPar(ch, pp::SmpFilterType, 0))), smpPar(ch, pp::SmpCutoff, 20000), smpPar(ch, pp::SmpReso, 0),
                  sampleOrange());
      const double ts[6] = {0, smpPar(ch, pp::SmpFiltAttack, 0), 0, smpPar(ch, pp::SmpFiltDecay, 0.3f), 0.5, smpPar(ch, pp::SmpFiltRelease, 0.3f)};
      envGraph(e0, e1, ts, 6, smpPar(ch, pp::SmpFiltSustain, 1), sampleOrange());
    }
    static const char* const ft[] = {"Off", "LP", "LP24", "BP", "HP", "Notch"};
    pseg(x, ch, pp::SmpFilterType, 0, 6, 1, "##ft", ft, 6, "The filter on each note: off, low pass (12 or 24 dB), band pass, high pass or notch");
    int k = 0;
    for (auto [key, idx, name, bip] : {std::tuple<const char*, int, const char*, bool>{"fcut", pp::SmpCutoff, "Cutoff", false},
                                       {"fres", pp::SmpReso, "Resonance", false},
                                       {"fenv", pp::SmpFiltEnv, "Env amount", true},
                                       {"fkey", pp::SmpKeyCutoff, "Key", true},
                                       {"fvel", pp::SmpVelCutoff, "Velocity", true},
                                       {"fatt", pp::SmpFiltAttack, "Attack", false},
                                       {"fdec", pp::SmpFiltDecay, "Decay", false},
                                       {"fsus", pp::SmpFiltSustain, "Sustain", false},
                                       {"frel", pp::SmpFiltRelease, "Release", false}})
      knobP(x, k++, key, smpPid(ch.id, idx), name, bip);
  };
  (void)a;
  return m;
}

Mod lfoModule(App& a, const Channel& ch) {
  Mod m("lfo", "LFO", MScope::Sample, 3, 0, TabEnvelope);
  m.body = [&ch](Cx& x) {
    reach("lfo");
    if (x.disp())
      lfoGraph(x.d0(), x.d1(), static_cast<int>(std::lround(smpPar(ch, pp::SmpLfoShape, 0))), smpPar(ch, pp::SmpLfoAmount, 0), smpPar(ch, pp::SmpLfoDelay, 0),
               smpPar(ch, pp::SmpLfoTarget, 0) >= 0.5f, sampleOrange());
    static const char* const tg[] = {"Off", "Vol", "Pan", "Cut", "Pitch"};
    pseg(x, ch, pp::SmpLfoTarget, 0, 3, 0, "##lt", tg, 5, "What the LFO moves: the volume, pan, the filter's cutoff or the pitch");
    static const char* const sh[] = {"Sine", "Triangle", "Pulse"};
    pseg(x, ch, pp::SmpLfoShape, 0, 3, 1, "##lsh", sh, 3);
    knobP(x, 0, "lrate", smpPid(ch.id, pp::SmpLfoRate), "Speed", false);
    knobP(x, 1, "lamt", smpPid(ch.id, pp::SmpLfoAmount), "Amount", true);
    knobP(x, 2, "ldel", smpPid(ch.id, pp::SmpLfoDelay), "Delay", false);
  };
  (void)a;
  return m;
}

// ---- Sample FX (rendered once into the sample)

void renderedNote(const Cx& x) {
  if (!x.disp()) return;
  dispNote(x, x.d0(), x.d1(), i18n::tr("rendered once into the sample"));
}

Mod fxShapeModule(App& a, const Channel& ch) {
  Mod m("fxshape", "FX shape", MScope::Rendered, 5, 1, TabSampleFx);
  m.body = [&ch](Cx& x) {
    reach("fx.shape");
    if (x.disp()) {  // the ramps over the sample
      const ImVec2 d0 = x.d0(), d1 = x.d1();
      sampleWaveIn(x.dl, ch, ImVec2(d0.x, d0.y + 14), d1, 0.35f);
      const float x0 = d0.x + 6, x1 = d1.x - 6, top = d0.y + 22, bot = d1.y - 6;
      const float ri = static_cast<float>(std::clamp<double>(smpPar(ch, pp::SmpPreRampIn, 0), 0.0, 1.0)) * 0.5f;
      const float ro = static_cast<float>(std::clamp<double>(smpPar(ch, pp::SmpPreRampOut, 0), 0.0, 1.0)) * 0.5f;
      const ImVec2 pts[4] = {ImVec2(x0, bot), ImVec2(x0 + ri * (x1 - x0), top), ImVec2(x1 - ro * (x1 - x0), top), ImVec2(x1, bot)};
      x.dl->AddPolyline(pts, 4, sampleOrange(), 0, 1.5f);
      renderedNote(x);
    }
    ptoggle(x, ch, pp::SmpPreClip, 0, 2, 1, "Boost clips", "The boost clips the peaks instead of letting them through");
    ptoggle(x, ch, pp::SmpPreFadeStereo, 2, 2, 1, "Stereo fade", "The sound moves from one side to the other over the sample");
    int k = 0;
    for (auto [key, idx, name, bip] : {std::tuple<const char*, int, const char*, bool>{"rin", pp::SmpPreRampIn, "Ramp in", false},
                                       {"rout", pp::SmpPreRampOut, "Ramp out", false},
                                       {"twarp", pp::SmpPreWarp, "Time warp", true},
                                       {"boost", pp::SmpPreBoost, "Boost", false},
                                       {"sdel", pp::SmpPreStereoDelay, "Stereo delay", true}})
      knobP(x, k++, key, smpPid(ch.id, idx), name, bip);
  };
  (void)a;
  return m;
}

Mod fxToneModule(App& a, const Channel& ch) {
  Mod m("fxtone", "FX tone", MScope::Rendered, 5, 2, TabSampleFx);
  m.body = [&ch](Cx& x) {
    reach("fx.tone");
    if (x.disp()) {
      sampleWaveIn(x.dl, ch, ImVec2(x.d0().x, x.d0().y + 14), x.d1(), 0.35f);
      renderedNote(x);
    }
    int k = 0;
    for (auto [key, idx, name, bip] : {std::tuple<const char*, int, const char*, bool>{"pcut", pp::SmpPreCutoff, "Cutoff", false},
                                       {"pres", pp::SmpPreReso, "Resonance", false},
                                       {"tilt", pp::SmpPreTilt, "Tilt", true},
                                       {"rmix", pp::SmpPreRingMix, "Ring mix", false},
                                       {"rfreq", pp::SmpPreRingFreq, "Ring freq", false}})
      knobP(x, k++, key, smpPid(ch.id, idx), name, bip);
  };
  (void)a;
  return m;
}

Mod fxSpaceModule(App& a, const Channel& ch) {
  Mod m("fxspace", "FX space", MScope::Rendered, 2, 1, TabSampleFx);
  m.body = [&ch](Cx& x) {
    reach("fx.space");
    if (x.disp()) sampleWaveIn(x.dl, ch, x.d0(), x.d1(), 0.35f);
    static const char* const rt[] = {"Room", "Hall"};
    pseg(x, ch, pp::SmpPreReverbType, 0, 2, 1, "##rt", rt, 2);
    knobP(x, 0, "prev", smpPid(ch.id, pp::SmpPreReverbMix), "Reverb", false);
  };
  (void)a;
  return m;
}

// ---- the channel (the old Misc page): its parts drawn on the grid by ChannelSettings.cpp

// Places for a part's knobs, toggles and lists: knob k in cell knobs[k], toggle k at toggles[k]
// (cell, span, row), list k at lists[k]; its note text in the display.
struct Slot {
  int c, span, row;
};
NoteGrid gridOf(const Cx& x, std::vector<int> knobs, std::vector<Slot> toggles, std::vector<Slot> lists) {
  NoteGrid g;
  const Cx cx = x;
  // (a band card and rows: the smaller knob, its value nearer)
  if (x.form == Form::Band) g.knobRadius = x.band.knobR, g.knobOff = x.band.knobOff, g.valueOff = x.band.valueOff;
  else if (x.form == Form::Rows) g.knobRadius = kBandKnobR, g.knobOff = kBandKnobOff, g.valueOff = kBandValueOff;
  g.place = [cx, knobs, toggles, lists](int kind, int i, float& w) -> ImVec2 {
    if (kind == 0) {
      if (cx.rows()) {  // three or so to a line
        w = spanW(1);
        const ImVec2 p = cx.cell(w);
        laid(p, ImVec2(p.x + w, p.y + kBandValueOff + kTextFs + 3), 0, 1);
        return p;
      }
      const int c = i < static_cast<int>(knobs.size()) ? knobs[static_cast<size_t>(i)] : i;
      w = spanW(1);
      laid(ImVec2(cx.cellX(c), cx.knobY(c)), ImVec2(cx.cellX(c) + spanW(1), cx.knobY(c) + cx.valueOff() + kTextFs + 3), 0, 1);
      return ImVec2(cx.cellX(c), cx.knobY(c));
    }
    if (kind == 3) {
      w = cx.d1().x - cx.d0().x - 12;
      return !cx.disp() || cx.dispH() < 40 ? ImVec2(-10000, -10000) : ImVec2(cx.d0().x + 6, cx.d0().y + 20);
    }
    const std::vector<Slot>& v = kind == 1 ? toggles : lists;
    const Slot s = i < static_cast<int>(v.size()) ? v[static_cast<size_t>(i)] : Slot{0, 1, 1};
    if (cx.compact) {  // (compact: off the module, out of sight)
      w = spanW(s.span);
      return ImVec2(-10000, -10000);
    }
    if (cx.rows()) {
      ImVec2 p;
      w = cx.place(s.span, 0, p);
      laid(p, ImVec2(p.x + w, p.y + kRowH));
      return p;
    }
    w = spanW(s.span);
    const ImVec2 p(cx.cellX(s.c), cx.rowY(s.row));
    laid(p, ImVec2(p.x + w, p.y + kRowH));
    return p;
  };
  return g;
}

Mod channelModule(App& a, const Channel& ch) {
  Mod m("channel", "Channel", MScope::Channel, 5, 0, TabChannel);
  const ObjectId id = ch.id;
  m.body = [&a, id](Cx& x) {
    reach("tune");
    reach("expr");
    if (x.disp()) dispNote(x, x.d0(), x.d1(), i18n::tr("how the channel plays its notes"));
    const NoteGrid tune = gridOf(x, {0, 1, 2, 3}, {}, {});
    drawNoteSettingsPart(a, id, 0, &tune);
    const NoteGrid expr = gridOf(x, {4}, {{0, 2, 0}, {0, 2, 1}}, {{2, 3, 0}, {2, 3, 1}});
    drawNoteSettingsPart(a, id, 3, &expr);
  };
  return m;
}

Mod polyModule(App& a, const Channel& ch) {
  Mod m("poly", "Polyphony", MScope::Channel, 4, 1, TabChannel);
  const ObjectId id = ch.id;
  m.body = [&a, id](Cx& x) {
    reach("poly");
    reach("cut");
    if (x.disp())
      if (const Channel* c = a.doc.song().channel(id)) {
        char b[96];
        std::snprintf(b, sizeof b, i18n::tr("voices: %s"), c->polyphony == 0 ? i18n::tr("all") : std::to_string(c->polyphony).c_str());
        dispNote(x, x.d0(), x.d1(), b);
        std::snprintf(b, sizeof b, i18n::tr("cut group: %s"), c->cutGroup ? std::to_string(c->cutGroup).c_str() : i18n::tr("off"));
        dispNote(x, x.d0(), x.d1(), b, false, true);
      }
    const NoteGrid poly = gridOf(x, {0, 1}, {{0, 2, 1}, {2, 2, 1}}, {});
    drawNoteSettingsPart(a, id, 1, &poly);
    const NoteGrid cut = gridOf(x, {2, 3}, {}, {});
    drawNoteSettingsPart(a, id, 2, &cut);
  };
  return m;
}

Mod echoModule(App& a, const Channel& ch) {
  Mod m("echo", "Echo", MScope::Channel, 7, 1, TabNoteFx);
  const ObjectId id = ch.id;
  m.body = [&a, id](Cx& x) {
    reach("echo");
    if (x.disp())
      if (const Channel* c = a.doc.song().channel(id)) {  // the echoes: one bar each, fading by the feedback
        const ImVec2 d0 = x.d0(), d1 = x.d1();
        const int n = std::clamp(c->echo.count, 0, 16);
        const float step = (d1.x - d0.x - 24) / 17.0f, base = d1.y - 6, top = d0.y + 20;
        float lvl = 1.0f;
        for (int k = 0; n > 0 && k <= n; ++k) {
          const float bx = std::round(d0.x + 12 + step * static_cast<float>(k) * std::max(0.25f, c->echo.time) * 2.0f);
          if (bx > d1.x - 8) break;
          x.dl->AddRectFilled(ImVec2(bx, base - lvl * (base - top)), ImVec2(bx + 3, base), theme::withAlpha(scopeCol(MScope::Channel), k ? 0.75f : 1.0f));
          lvl *= std::clamp(c->echo.feedback, 0.0f, 1.0f);
        }
        if (n == 0) dispNote(x, d0, d1, i18n::tr("no echoes"));
      }
    const NoteGrid echo = gridOf(x, {0, 1, 2, 3, 4, 5, 6}, {{0, 2, 1}}, {});
    drawNoteSettingsPart(a, id, 4, &echo);
  };
  return m;
}

Mod layerModule(App& a, const Channel& ch) {
  Mod m("layer", "Layer and MIDI", MScope::Channel, 4, 0, TabMidi);
  const ObjectId id = ch.id;
  m.body = [&a, id](Cx& x) {
    reach("layer");
    if (x.disp())
      if (const Channel* c = a.doc.song().channel(id)) {
        std::string kids;
        for (ObjectId k : c->layer)
          if (const Channel* o = a.doc.song().channel(k)) kids += (kids.empty() ? "" : ", ") + o->name;
        dispNote(x, x.d0(), x.d1(), kids.empty() ? std::string(i18n::tr("no layer children")) : std::string(i18n::tr("plays with: ")) + kids);
      }
    const NoteGrid g = gridOf(x, {}, {}, {{0, 2, 0}, {2, 2, 0}, {0, 2, 1}, {2, 2, 1}});
    drawNoteSettingsPart(a, id, 5, &g);
  };
  return m;
}

Mod noteFxModule(App& a, const Channel& ch) {
  Mod m("notefx", "Note processors", MScope::Channel, 5, 0, TabNoteFx);
  const ObjectId id = ch.id;
  m.body = [&a, id](Cx&) {
    reach("notefx");
    packs::modulation::drawChannelNoteFx(a, id, ImGui::GetContentRegionAvail().x);
  };
  m.free = true;
  return m;
}

Mod macrosModule(App& a, const Channel& ch) {
  Mod m("macros", "Macros", MScope::Channel, 7, 0, TabMidi);
  const ObjectId id = ch.id;
  m.body = [&a, id](Cx&) {
    reach("macros");
    macroui::drawPanel(a, id, ImGui::GetContentRegionAvail().x);
  };
  m.free = true;
  return m;
}

// A clip of the clip grid: its launch settings (mode, quantisation, legato), then its follow action.
Mod launchModule(App& a, const Clip& c, bool follow) {
  Mod m(follow ? "follow" : "launch", follow ? "Follow action" : "Launch", MScope::Clip, 4, 0, TabClip);
  m.body = [&a, &c, follow](Cx&) {
    reach("launch");
    livelook::clipLaunchParts(a, c.id, follow ? livelook::kLaunchPartFollow : livelook::kLaunchPartLaunch);
  };
  m.free = true;
  return m;
}

// The modules, in order: this clip, its sample, its channel.
std::vector<Mod> modulesOf(App& a, const Clip* c, const Channel& ch) {
  std::vector<Mod> v;
  const Song& s = a.doc.song();
  if (c) {
    if (clipsLayout() && c->start < s.live.areaEnd) {
      v.push_back(launchModule(a, *c, false));
      v.push_back(launchModule(a, *c, true));
    }
    v.push_back(clipModule(a, *c, ch));
    v.push_back(fadesModule(a, *c));
    v.push_back(warpModule(a, *c, ch));
  }
  v.push_back(sampleModule(a, c, ch));
  if (!c) v.push_back(stretchModule(a, ch));
  v.push_back(sampleWarpModule(a, ch));
  v.push_back(playbackModule(a, ch));
  v.push_back(voiceModule(a, ch));
  v.push_back(envelopeModule(a, ch));
  v.push_back(filterModule(a, ch));
  v.push_back(lfoModule(a, ch));
  v.push_back(fxShapeModule(a, ch));
  v.push_back(fxToneModule(a, ch));
  v.push_back(fxSpaceModule(a, ch));
  v.push_back(channelModule(a, ch));
  v.push_back(polyModule(a, ch));
  v.push_back(echoModule(a, ch));
  v.push_back(layerModule(a, ch));
  v.push_back(noteFxModule(a, ch));
  v.push_back(macrosModule(a, ch));
  return v;
}

// The pinned Output module: the channel's volume, pan and pitch, Open in Loupe, always in view; and
// its mixer track where the host has no header holding it (the Clips layout's head card: the window,
// Smooth's audio editor and the Tracker's page have the box in their header).
bool routeInHeader(Host host) { return !bandHost(host); }
Mod outputModule(App& a, const Clip* c, const Channel& ch, Host host) {
  const bool route = !routeInHeader(host);
  Mod m("output", "Output", MScope::Channel, 3, route ? 0 : 1, TabChannel);
  m.body = [&a, c, &ch, host, route](Cx& x) {
    for (const char* k : {"vol", "pan", "pitch", "loupe"}) reach(k);
    const Song& s = a.doc.song();
    const int mi = std::max(0, s.mixerIndex(ch.mixerTrack));
    const std::string ml = mi == 0 ? std::string(i18n::tr("Master")) : std::to_string(mi) + "  " + mixerTrackName(s, static_cast<size_t>(mi));
    if (x.disp() && !x.head) {
      dispNote(x, x.d0(), x.d1(), ch.name);
      if (x.dispH() >= 40) dispNote(x, x.d0(), x.d1(), std::string(i18n::tr("plays through ")) + ml, false, true);
    }
    if (route && !x.compact) {
      reach("route");
      reach("routefree");
      // (rows: the route and the free track's button on one line)
      const float rw = x.rows() ? x.w - kRowH - 6 : spanW(2);
      if (x.rows()) {
        ImVec2 p;
        x.place(4, x.w, p);
        ImGui::SetCursorScreenPos(p);
      } else {
        x.at(0, 0);
      }
      const ImVec2 r0 = ImGui::GetCursorScreenPos();
      if (chanroute::box(a, ch.id, "##ceroute", rw, "ceroute")) {
        const ObjectId id = ch.id;
        a.later([&a, id] { chanroute::routeToFree(a, id); });
      }
      laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      ImGui::SetCursorScreenPos(x.rows() ? ImVec2(r0.x + x.w - kRowH, r0.y) : ImVec2(x.cellX(2) + std::round((spanW(1) - kRowH) * 0.5f), x.rowY(0)));
      if (chanroute::button(a, ch.id, kRowH)) {
        const ObjectId id = ch.id;
        a.later([&a, id] { chanroute::routeToFree(a, id); });
      }
      devNoteRect("ceroutefree");  // (dev: an "sdlat" target)
      laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    if (button(x, 0, 3, 1, "Open in Loupe", false, "Opens the sample in Loupe for long sessions: recording, scripts, the region list")) {
      if (host == Host::Pane) gPaneOn = false;
      if (c) cliprenderui::sendToLoupe(a, c->id);
      else if (!ch.plugin.data.empty()) packs::openAudioEditor(a, ch.plugin.data, ch.id);
    }
    knobP(x, 0, "##cevol", channelParam(ch.id, ChannelParam::Volume), "Volume", false);
    knobP(x, 1, "##cepan", channelParam(ch.id, ChannelParam::Pan), "Pan", true);
    knobP(x, 2, "##cepitch", channelParam(ch.id, ChannelParam::Pitch), "Pitch", true, 1.0);
  };
  return m;
}


// ---- Edit audio and the analysis (the tools row's menu and readout)

// Every audio operation (Loupe's: a new file, the old one kept, one undo step) in grouped submenus,
// then send and split. On the selection when there is one, else the whole clip (or sample).
void editAudioItems(App& a, State& S, const Clip* c, const Channel& ch) {
  using clipedit::Op;
  const double a0 = std::min(S.selT0, S.selT1), a1 = std::max(S.selT0, S.selT1);
  char note[160];
  if (c && a1 > a0) {
    static TempoMap tm;
    tm.build(a.doc.song());
    std::snprintf(note, sizeof note, i18n::tr("On the selection (%.2f s): a new file, one undo step"), tm.secondsAt(a1) - tm.secondsAt(a0));
  } else {
    std::snprintf(note, sizeof note, "%s", c ? i18n::tr("On the whole clip: a new file, one undo step") : i18n::tr("On the whole sample: a new file, one undo step"));
  }
  ImGui::TextColored(theme::vec(pal().textDim), "%s", note);
  ImGui::Separator();
  const ObjectId clipId = c ? c->id : kNoId, chId = ch.id;
  struct Group {
    const char* name;
    std::vector<Op> ops;
  };
  static const Group groups[] = {
      {"Cut and silence", {Op::Cut, Op::TrimToSelection, Op::Silence, Op::InsertSilence, Op::TrimSideSilence}},
      {"Level and fades", {Op::FadeIn, Op::FadeOut, Op::Normalize, Op::Amplify, Op::Gate}},
      {"Repair", {Op::RemoveDc, Op::Denoise, Op::Declick, Op::Invert}},
      {"Time and pitch", {Op::Reverse, Op::StretchPitch, Op::TuneToNote, Op::Resample}},
      {"Effects", {Op::Equalize, Op::Reverb, Op::Blur}},
      {"Stereo", {Op::Mono, Op::Stereo, Op::Swap}},
  };
  for (const Group& g : groups) {
    if (!ImGui::BeginMenu(i18n::tr(g.name))) continue;
    for (Op op : g.ops) {
      // (Normalize and Reverse here rewrite the file: said so, beside the clip's and the sample's own)
      std::string label = op == Op::Reverse ? i18n::tr("Reverse the file") : op == Op::Normalize ? i18n::tr("Normalize the file") : i18n::tr(clipedit::opName(op));
      if (clipedit::opHasDialog(op)) label += "...";
      const bool needSel = op == Op::Cut || op == Op::TrimToSelection || op == Op::InsertSilence;
      ImGui::PushID(static_cast<int>(op));
      if (ImGui::MenuItem(label.c_str(), nullptr, false, !needSel || (clipId != kNoId && a1 > a0))) askOp(a, clipId, op, a0, a1, clipId == kNoId ? chId : kNoId);
      ui::Hint(clipedit::opHint(op));
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }
  if (!c) return;
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Send and split"))) {
    if (ImGui::MenuItem(i18n::tr("Split stems..."))) stemsui::openForClip(a, clipId);
    if (ImGui::MenuItem(i18n::tr("Chop"))) chopui::open(a, clipId, 0);
    ui::Hint("Slices the clip at the grid, into parts or at its transients");
    if (ImGui::MenuItem(i18n::tr("To sampler"))) {
      const std::string p = clipRegionFile(a, clipId);
      if (!p.empty()) a.settingsChannel = a.addInstrument("gl.sampler", p);
    }
    ui::Hint("The clip's audio on a new sampler channel");
    if (ImGui::MenuItem(i18n::tr("To slicer"))) {
      const std::string p = clipRegionFile(a, clipId);
      if (!p.empty()) a.settingsChannel = a.addInstrument("gl.slicer", p);
    }
    ui::Hint("The clip's audio on a new Kerf channel, sliced at its transients");
    if (ImGui::MenuItem(i18n::tr("Export WAV"))) exportui::exportClipAudio(a, clipId);
    ui::Hint("The clip's sound as a WAV file");
    if (ImGui::MenuItem(i18n::tr("Consolidate"))) {
      a.later([&a, clipId] {
        ObjectId nid = kNoId;
        const std::string dir = renderedDir(a);
        const double sr = a.engine.sampleRate();
        a.doc.editSong("consolidate clips", [&](Song& x) { nid = consolidateClips(x, {clipId}, dir, sr, true); });
        if (nid != kNoId) a.playlist.selected = {nid};
      });
    }
    ui::Hint("The clip rendered to one new audio clip in its place");
    ImGui::EndMenu();
  }
}

// The analysis of the clip (the selection when there is one) or the sample: name and value pairs,
// most wanted first. Empty while the audio is being read.
std::vector<std::pair<std::string, std::string>> analysisOf(App& a, State& S, const Clip* c, const Channel& ch, const char** scope) {
  std::vector<std::pair<std::string, std::string>> v;
  const double a0 = std::min(S.selT0, S.selT1), a1 = std::max(S.selT0, S.selT1);
  const bool sel = c && a1 > a0;
  if (scope) *scope = !c ? i18n::tr("Sample") : sel ? i18n::tr("Selection") : i18n::tr("Clip");
  const Song& s = a.doc.song();
  int64_t fa = 0, fb = 0;
  if (c) {
    clipedit::clipRangeFrames(s, c->id, sel ? a0 : static_cast<double>(c->start), sel ? a1 : static_cast<double>(c->start + s.clipLength(*c)), fa, fb);
  } else if (const ClipSource src = clipSourceIfReady(ch); src.sample) {
    fb = static_cast<int64_t>(src.sample->frames());
  }
  const Measures& m = measure(samplePath(ch), fa, fb);
  if (!m.ok) return v;
  auto num = [](const char* fmt, double val) {
    char b[48];
    if (val <= -179) std::snprintf(b, sizeof b, "-inf");
    else std::snprintf(b, sizeof b, fmt, val);
    return std::string(b);
  };
  v.emplace_back(i18n::tr("Peak"), num("%.1f dB", m.lv.peakDb));
  v.emplace_back(i18n::tr("RMS"), num("%.1f dB", m.lv.rmsDb));
  v.emplace_back(i18n::tr("Loudness"), num("%.1f LUFS", m.lv.lufs));
  v.emplace_back(i18n::tr("True peak"), num("%.1f dBTP", m.lv.truePeakDb));
  v.emplace_back(i18n::tr("Tempo"), m.bpm > 0 ? num("%.1f BPM", m.bpm) : std::string("-"));
  v.emplace_back(i18n::tr("Pitch"), m.pitch.note >= 0 && m.pitch.confidence > 0.3 ? ed::noteName(m.pitch.note) + " " + num("%+.0f ct", m.pitch.cents) : std::string("-"));
  v.emplace_back(i18n::tr("DC"), num("%.4f", m.lv.dc));
  if (const ClipSource src = clipSourceIfReady(ch); src.sample && src.sample->sampleRate > 0)
    v.emplace_back(i18n::tr("Length"), num("%.3f s", static_cast<double>(fb - fa) / src.sample->sampleRate));
  return v;
}

// The analysis at the tools row's right, as many readings as fit in [x0, x1] (the most wanted first).
void analysisReadout(App& a, State& S, const Clip* c, const Channel& ch, float x0, float x1, float cy) {
  reach("analysis");
  const char* scope = "";
  const auto v = analysisOf(a, S, c, ch, &scope);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  ImFont* uf = ImGui::GetFont();
  ImFont* mf = a.fontMono ? a.fontMono : uf;
  auto caps = [](std::string t) {
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char k) { return static_cast<char>(std::toupper(k)); });
    return t;
  };
  const float gap = 14, ty = std::round(cy - kTextFs * 0.5f);
  if (v.empty()) {
    const char* t = i18n::tr("Reading the audio...");
    const float tw = uf->CalcTextSizeA(kTextFs, 1e9f, 0, t).x;
    if (x1 - tw > x0) {
      dl->AddText(uf, kTextFs, ImVec2(x1 - tw, ty), P.textFaint, t);
      S.toolRects.emplace_back(ImVec2(x1 - tw, ty), ImVec2(x1, ty + kTextFs));
    }
    return;
  }
  std::vector<float> widths;
  for (const auto& [n, val] : v)
    widths.push_back(uf->CalcTextSizeA(kTextFs, 1e9f, 0, caps(n).c_str()).x + 6 + mf->CalcTextSizeA(kTextFs, 1e9f, 0, val.c_str()).x);
  const std::string cap = caps(scope);
  const float capW = uf->CalcTextSizeA(kTextFs, 1e9f, 0, cap.c_str()).x;
  size_t n = 0;
  float used = capW;
  while (n < v.size() && used + gap + widths[n] <= x1 - x0) used += gap + widths[n], ++n;
  if (n == 0) return;
  float x = x1 - used;
  S.toolRects.emplace_back(ImVec2(x, ty), ImVec2(x1, ty + kTextFs));
  dl->AddText(uf, kTextFs, ImVec2(x, ty), P.textFaint, cap.c_str());
  x += capW;
  for (size_t k = 0; k < n; ++k) {
    x += gap;
    const std::string nm = caps(v[k].first);
    dl->AddText(uf, kTextFs, ImVec2(x, ty), P.textDim, nm.c_str());
    const float nw = uf->CalcTextSizeA(kTextFs, 1e9f, 0, nm.c_str()).x + 6;
    dl->AddText(mf, kTextFs, ImVec2(x + nw, ty), P.text, v[k].second.c_str());
    x += widths[k];
  }
}

// The Edit audio... button and its menu.
void editAudioButton(App& a, State& S, const Clip* c, const Channel& ch, ImVec2 at, float h) {
  reach("edit.ops");
  if (c) reach("edit.send");
  ImGui::SetCursorScreenPos(at);
  if (ui::TextButton("Edit audio...", false, 0, c ? "The audio operations (Loupe's, on this clip's sample)" : "The audio operations (Loupe's, on the sample)") ||
      std::exchange(S.editDev, false))
    ImGui::OpenPopup("##ceeditaudio");
  S.toolRects.emplace_back(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, at.y + h + 2), ImGuiCond_Appearing, ImVec2(1, 0));
  if (ImGui::BeginPopup("##ceeditaudio")) {
    editAudioItems(a, S, c, ch);
    ImGui::EndPopup();
  }
}

// ---- the tab bar and the tab's modules

const char* groupCaption(MScope s) {
  switch (s) {
    case MScope::Clip: return i18n::tr("This clip");
    case MScope::Sample:
    case MScope::Rendered: return i18n::tr("The sample");
    default: return i18n::tr("The channel");
  }
}
int groupOf(MScope s) { return s == MScope::Clip ? 0 : s == MScope::Channel ? 2 : 1; }
MScope tabScope(int tab) {
  switch (clipwin::scopeOf(tab)) {
    case clipwin::Scope::Clip: return MScope::Clip;
    case clipwin::Scope::Sample: return MScope::Sample;
    default: return MScope::Channel;
  }
}
// The tabs' names (clipwin::tabName's, marked for translation here).
const char* tabLabel(int tab) {
  static const char* const k[TabCount] = {i18n::N_("Clip"),         i18n::N_("Warp"),            i18n::N_("Sample"),     i18n::N_("Playback"),
                                          i18n::N_("Envelope"),     i18n::N_("Filter"),          i18n::N_("Sample FX"),  i18n::N_("Channel"),
                                          i18n::N_("Note effects"), i18n::N_("MIDI and macros"), i18n::N_("Edit audio"), i18n::N_("Analysis")};
  return tab >= 0 && tab < TabCount ? k[tab] : "";
}

// A module is on `tab`: its own; with no clip (no Warp tab) the sample's warp is on the Sample tab.
bool onTab(const Mod& m, int tab, bool hasClip) { return m.tab == tab || (!hasClip && tab == TabSample && m.tab == TabTime); }
// The tab of modules shown for `tab`: itself, else (Edit audio and Analysis are the tools row's)
// the Clip tab, or Playback with no clip.
int moduleTab(int tab, bool hasClip) {
  tab = clipwin::shownTab(tab, hasClip);
  return clipwin::hasModules(tab) ? tab : hasClip ? TabClip : TabPlayback;
}

// A tab's modules, laid out.
struct Lay {
  bool ok = false;         // every module fits the width (folded, when allowed)
  bool pinned = false;     // Output at the right, the others in rows beside it
  std::vector<ImVec2> at;  // each module's top-left from the rows' (Output last when not pinned)
  float rowsH = 0;         // the rows' height
  float h = 0;             // with the pinned Output
  int rows = 0;
};

// Lays `ms` in rows `room` wide, in order: as many as fit in each row, the row's spare width shared
// by its modules by their cells (their cells sit in their middle), so each row ends at the room's
// right; a module alone in its row (Output aside) grows to kLoneW at most, at the row's left. With `fold` (compact), a module wider than the room folds its knobs onto rows under them
// and a free one narrows; without, such a module makes the layout not ok.
Lay layRows(const std::vector<Mod*>& ms, float room, bool compact, bool fold) {
  Lay L;
  for (Mod* m : ms) {
    m->grow = 0, m->fold = 0, m->narrowW = 0;
    if (m->w() <= room + 0.5f) continue;
    if (!fold) return L;
    if (m->free) {
      m->narrowW = std::floor(room);
    } else {
      const int most = std::max(1, static_cast<int>((room - 2 * kPad + kGut) / kCell));
      const int rows = (m->cols + most - 1) / most;
      m->fold = (m->cols + rows - 1) / rows;
    }
  }
  L.at.assign(ms.size(), ImVec2(0, 0));
  auto grows = [](const Mod* m) { return m->fold == 0 && m->narrowW <= 0; };
  float y = 0;
  size_t k = 0;
  while (k < ms.size()) {
    size_t e = k;
    float used = 0;
    while (e < ms.size() && (e == k || used + kModGap + ms[e]->w() <= room + 0.5f)) used += (e == k ? 0 : kModGap) + ms[e]->w(), ++e;
    int cells = 0;
    size_t last = e;
    for (size_t j = k; j < e; ++j)
      if (grows(ms[j])) cells += ms[j]->cols, last = j;
    float spare = std::max(0.0f, std::floor(room - used));
    {  // (a lone module: a sensible width, not the whole panel's)
      int mods = 0;
      Mod* lone = nullptr;
      for (size_t j = k; j < e; ++j)
        if (std::strcmp(ms[j]->id, "output") != 0) ++mods, lone = ms[j];
      if (mods == 1 && grows(lone) && e - k == 1) spare = std::min(spare, std::max(0.0f, std::floor(kLoneW - lone->w())));
    }
    float x = 0, rh = 0, given = 0;
    for (size_t j = k; j < e; ++j) {
      Mod* m = ms[j];
      if (cells > 0 && grows(m)) {
        m->grow = j == last ? spare - given : std::floor(spare * static_cast<float>(m->cols) / static_cast<float>(cells));
        given += m->grow;
      }
      L.at[j] = ImVec2(x, y);
      x += m->w() + kModGap;
      rh = std::max(rh, m->h(compact));
    }
    y += rh + kModGap;
    ++L.rows;
    k = e;
  }
  L.rowsH = L.h = std::max(0.0f, y - kModGap);
  L.ok = true;
  return L;
}

// The room a tab's modules get in a panel `w` wide: 12 px at each side; pinned, Output and a 12 px
// gap at the right too.
float tabRoom(float w, const Mod& out, bool pinned) { return w - 24 - (pinned ? out.w() + 12 : 0.0f); }

// The tab's modules and Output in a panel `w` wide: Output pinned at the right when the modules fit
// beside it in no more rows than with Output among them, else Output last among them. Compact, a
// module wider than the panel folds; full, the layout is not ok then (the panel goes compact).
Lay layTab(const std::vector<Mod*>& ms, Mod& out, float w, bool compact) {
  out.grow = 0, out.fold = 0, out.narrowW = 0;
  std::vector<Mod*> all = ms;
  all.push_back(&out);
  const Lay flow = layRows(all, tabRoom(w, out, false), compact, false);
  out.grow = 0, out.fold = 0, out.narrowW = 0;
  Lay pin = layRows(ms, tabRoom(w, out, true), compact, false);
  if (pin.ok && (!flow.ok || pin.rows <= flow.rows)) {
    pin.pinned = true;
    pin.h = std::max(pin.rowsH, out.h(compact));
    return pin;
  }
  if (flow.ok) return layRows(all, tabRoom(w, out, false), compact, false);
  if (!compact) return Lay{};
  return layRows(all, tabRoom(w, out, false), compact, true);
}

// The tab bar: the tabs shown, grouped by what they change (a tick in the group's colour, its caption
// when there is room); the shown tab lit and underlined in its colour. Tabs with no room go under
// More at the right (the shown one always keeps its place).
void tabBar(App& a, State& S, const Clip* cp, const Channel& ch, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* uf = ImGui::GetFont();
  const bool hasClip = cp != nullptr;
  const float fs = kTextFs + 1, capGap = 12, groupGap = 16, tick = 11;
  auto caps = [](std::string t) {
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char k) { return static_cast<char>(std::toupper(k)); });
    return t;
  };
  std::vector<int> vis;
  for (int t = 0; t < TabEdit; ++t)
    if (clipwin::tabShown(t, hasClip)) vis.push_back(t);
  const float moreW = std::ceil(uf->CalcTextSizeA(fs, 1e9f, 0, i18n::tr("More")).x + 32);
  auto widthOf = [&](const std::vector<int>& v, bool captions, float pad, bool more) {
    float x = 0;
    int g = -1;
    for (int t : v) {
      if (groupOf(tabScope(t)) != g) {
        g = groupOf(tabScope(t));
        x += (x > 0 ? groupGap : 0) + tick;
        if (captions) x += uf->CalcTextSizeA(kTextFs, 1e9f, 0, caps(groupCaption(tabScope(t))).c_str()).x + capGap;
      }
      x += std::round(uf->CalcTextSizeA(fs, 1e9f, 0, i18n::tr(tabLabel(t))).x + 2 * pad);
    }
    return x + (more ? moreW + 8 : 0.0f);
  };
  const float room = w - 24;
  bool captions = widthOf(vis, true, 12, false) <= room;
  const float pad = captions || widthOf(vis, false, 12, false) <= room ? 12.0f : 8.0f;
  std::vector<int> over;
  if (widthOf(vis, false, pad, false) > room)
    while (vis.size() > 1 && widthOf(vis, false, pad, true) > room) {
      size_t r = vis.size() - 1;
      if (vis[r] == S.tab) --r;
      over.insert(over.begin(), vis[r]);
      vis.erase(vis.begin() + static_cast<std::ptrdiff_t>(r));
    }
  if (!over.empty()) captions = false;
  S.tabRects.clear();
  S.moreTabs = static_cast<int>(over.size());
  float x = p0.x + 12;
  const float cy = std::round(p0.y + h * 0.5f);
  int g = -1;
  for (int t : vis) {
    const MScope sc = tabScope(t);
    if (groupOf(sc) != g) {
      if (g >= 0) x += groupGap;
      g = groupOf(sc);
      dl->AddRectFilled(ImVec2(x, cy - 5), ImVec2(x + 3, cy + 5), scopeCol(sc), 1.0f);
      x += tick;
      if (captions) {
        const std::string cap = caps(groupCaption(sc));
        dl->AddText(uf, kTextFs, ImVec2(x, std::round(cy - kTextFs * 0.5f)), P.textFaint, cap.c_str());
        x += uf->CalcTextSizeA(kTextFs, 1e9f, 0, cap.c_str()).x + capGap;
      }
    }
    const char* label = i18n::tr(tabLabel(t));
    const float cw = std::round(uf->CalcTextSizeA(fs, 1e9f, 0, label).x + 2 * pad);
    const ImVec2 a0(x, p0.y + 4), a1(x + cw, p0.y + h);
    ImGui::SetCursorScreenPos(a0);
    ImGui::PushID(t);
    if (ImGui::InvisibleButton("##cetab", ImVec2(cw, a1.y - a0.y))) S.tab = t;
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    ui::Hint(scopeLine(a, t, cp, ch).c_str());
    const bool on = t == S.tab;
    if (on) dl->AddRectFilled(a0, a1, theme::withAlpha(P.text, P.dark ? 0.07f : 0.09f), theme::kRadius, ImDrawFlags_RoundCornersTop);
    else if (hov) dl->AddRectFilled(a0, a1, theme::withAlpha(P.text, 0.04f), theme::kRadius, ImDrawFlags_RoundCornersTop);
    dl->AddText(uf, fs, ImVec2(x + pad, std::round(cy - fs * 0.5f + 1)), on || hov ? P.text : P.textDim, label);
    if (on) dl->AddRectFilled(ImVec2(a0.x, a1.y - 2), a1, scopeCol(sc));
    S.tabRects.emplace_back(a0, a1);
    x += cw;
  }
  if (!over.empty()) {  // More: the tabs with no room
    const ImVec2 a0(p0.x + w - 12 - moreW, p0.y + 4), a1(p0.x + w - 12, p0.y + h);
    ImGui::SetCursorScreenPos(a0);
    const bool hit = ImGui::InvisibleButton("##cetabmore", ImVec2(moreW, a1.y - a0.y));
    const bool hov = ImGui::IsItemHovered();
    ui::Hint("The tabs with no room in the bar");
    if (hov) dl->AddRectFilled(a0, a1, theme::withAlpha(P.text, 0.04f), theme::kRadius, ImDrawFlags_RoundCornersTop);
    dl->AddText(uf, fs, ImVec2(a0.x + 10, std::round(cy - fs * 0.5f + 1)), hov ? P.text : P.textDim, i18n::tr("More"));
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(a1.x - 12, cy + 1), 10, hov ? P.text : P.textDim);
    S.tabRects.emplace_back(a0, a1);
    if (hit) ImGui::OpenPopup("##cetabmorepop");
    ImGui::SetNextWindowPos(ImVec2(a1.x, a1.y + 2), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##cetabmorepop")) {
      for (int t : over)
        if (ImGui::Selectable(i18n::tr(tabLabel(t)), t == S.tab)) S.tab = t;
      ImGui::EndPopup();
    }
  }
}

// The tab's modules laid by `L` under the tab bar at p0, `w` wide, `hAvail` tall: in a panel that
// scrolls up and down when they need more height (never sideways), Output pinned at the right
// after a hairline when `L` pins it.
void tabModules(App& a, State& S, const std::vector<Mod*>& ms, Mod& out, const Lay& L, ImVec2 p0, float w, float hAvail, bool compact) {
  const auto& P = pal();
  const float room = tabRoom(w, out, L.pinned);
  const bool scrolls = L.rowsH > hAvail + 0.5f;
  const float sbW = scrolls ? 10.0f : 0.0f;
  S.panelA = ImVec2(p0.x + 12, p0.y), S.panelB = ImVec2(p0.x + 12 + room, p0.y + hAvail);
  S.panelScrolls = scrolls;
  ImGui::SetCursorScreenPos(ImVec2(p0.x + 12, p0.y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
  ImGui::BeginChild("##cetabmods", ImVec2(room + sbW, std::max(1.0f, hAvail)), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | (scrolls ? 0 : ImGuiWindowFlags_NoScrollbar));
  ImGui::PopStyleVar(2);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  for (size_t i = 0; i < ms.size(); ++i) drawModule(a, *ms[i], ImVec2(std::round(o.x + L.at[i].x), std::round(o.y + L.at[i].y)), compact);
  if (!L.pinned && !L.at.empty()) drawModule(a, out, ImVec2(std::round(o.x + L.at.back().x), std::round(o.y + L.at.back().y)), compact);
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(room, L.rowsH));
  // The wheel over the panel (not taken by a knob under it) scrolls it.
  const ImGuiIO& io = ImGui::GetIO();
  if (scrolls && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0 && !ui::wheelTaken() && !io.KeyCtrl)
    ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() - io.MouseWheel * 48.0f, 0.0f, ImGui::GetScrollMaxY()));
  ImGui::EndChild();
  if (L.pinned) {  // the hairline, then Output
    if (!scrolls) {
      const float hx = std::round(p0.x + 12 + room + 6) + 0.5f;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(hx, p0.y + 4), ImVec2(hx, p0.y + std::min(L.h, hAvail) - 4), P.lineStrong);
    }
    drawModule(a, out, ImVec2(std::round(p0.x + w - 12 - out.w()), p0.y), compact);
  }
}

// ---- the layouts' own places: the Clips band's cards, Smooth's inspector rows

// (dev "clipeditor native") what the native view drew last: its place, the box its drawing took up
// (what shows, inside the clip rectangles), the popups open, its cards.
struct NativeCheck {
  ImVec2 a, b;                  // the place it has (the band, the pane, the inspector)
  ImVec2 drawnA, drawnB;        // what it drew that shows
  bool any = false;             // it drew something
  int frame = -100;
  int popups = 0;               // popups open over it
  int cards = 0;
  std::string form;
};
NativeCheck gNative[5];
bool gMeasure = false;  // (dev) measure what the next frames draw
int gInspFrame = -100;  // Smooth's inspector drew the region's rows

// The visible bounds of what `dl` drew from index `idx0` on (each triangle cut to its command's clip).
void drawnBounds(const ImDrawList* dl, int idx0, ImVec2& lo, ImVec2& hi, bool& any) {
  for (const ImDrawCmd& cmd : dl->CmdBuffer) {
    if (cmd.UserCallback || cmd.ElemCount == 0) continue;
    const int e0 = std::max(static_cast<int>(cmd.IdxOffset), idx0), e1 = static_cast<int>(cmd.IdxOffset + cmd.ElemCount);
    for (int e = e0; e + 2 < e1; e += 3) {
      ImVec2 t0(1e9f, 1e9f), t1(-1e9f, -1e9f);
      for (int k = 0; k < 3; ++k) {
        const ImVec2 v = dl->VtxBuffer[static_cast<int>(cmd.VtxOffset) + dl->IdxBuffer[e + k]].pos;
        t0 = ImVec2(std::min(t0.x, v.x), std::min(t0.y, v.y)), t1 = ImVec2(std::max(t1.x, v.x), std::max(t1.y, v.y));
      }
      // (a triangle with no area, an antialiasing fringe, shows nothing worth counting)
      t0 = ImVec2(std::max(t0.x, cmd.ClipRect.x), std::max(t0.y, cmd.ClipRect.y));
      t1 = ImVec2(std::min(t1.x, cmd.ClipRect.z), std::min(t1.y, cmd.ClipRect.w));
      if (t1.x - t0.x < 0.5f || t1.y - t0.y < 0.5f) continue;
      lo = ImVec2(std::min(lo.x, t0.x), std::min(lo.y, t0.y)), hi = ImVec2(std::max(hi.x, t1.x), std::max(hi.y, t1.y));
      any = true;
    }
  }
}

// Measures what is drawn between begin() and end() in the current window and the child windows
// drawn inside it, into gNative[slot].
struct Measure {
  int slot;
  bool on;
  ImGuiWindow* win = nullptr;
  int idx0 = 0;
  Measure(int slot_, ImVec2 a, ImVec2 b, const char* form) : slot(slot_), on(gMeasure) {
    NativeCheck& n = gNative[slot];
    n.a = a, n.b = b, n.form = form, n.frame = ImGui::GetFrameCount();
    if (!on) return;
    win = ImGui::GetCurrentWindow();
    idx0 = win->DrawList->IdxBuffer.Size;
  }
  void end(int cards) {
    NativeCheck& n = gNative[slot];
    n.cards = cards;
    if (!on) return;
    ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
    bool any = false;
    drawnBounds(win->DrawList, idx0, lo, hi, any);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindow* w : g.Windows) {  // the child windows drawn inside it this frame
      if (w == win || !w->WasActive || w->LastFrameActive != g.FrameCount || !(w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
      bool inside = false;
      for (ImGuiWindow* p = w->ParentWindow; p && !inside; p = p->ParentWindow) inside = p == win;
      if (inside) drawnBounds(w->DrawList, 0, lo, hi, any);
    }
    n.drawnA = lo, n.drawnB = hi, n.any = any;
    n.popups = 0;
    for (const ImGuiPopupData& p : g.OpenPopupStack) n.popups += p.Window && p.Window->WasActive ? 1 : 0;
  }
};

// A band card's width: its cells, or a folded strip.
constexpr float kBandFoldW = 22, kBandGap = 6;
std::set<std::string> gBandFolded;  // the cards folded to a strip (by id)
float bandCardW(const Mod& m) { return gBandFolded.count(m.id) ? kBandFoldW : modW(m.cols); }

// A card of the Clips layout's band: the device cards' face (the title strip, a fold arrow, the
// name, the scope word, the module's switch and menu), the module's controls fitted to the band's
// height under it. A double-click on the title folds it to a strip, as a device folds.
void bandCard(App& a, Mod& m, ImVec2 o, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = bandCardW(m);
  const ImVec2 p1(o.x + w, o.y + h);
  const bool folded = gBandFolded.count(m.id) > 0;
  ImGui::PushID(m.id);
  dl->AddRectFilled(o, p1, P.surface800, theme::kRadiusLg);
  if (folded) {  // a strip: its scope tick and its name reading upwards; a click unfolds it
    dl->AddRectFilled(o, p1, P.surface700, theme::kRadiusLg);
    dl->AddRectFilled(ImVec2(std::round(o.x + w * 0.5f - 1.5f), o.y + 8), ImVec2(std::round(o.x + w * 0.5f + 1.5f), o.y + 20), scopeCol(m.scope), 1.0f);
    ImGui::SetCursorScreenPos(o);
    if (ImGui::InvisibleButton("##unfold", ImVec2(w, h))) gBandFolded.erase(m.id);
    ui::Hint("Click to unfold it");
    std::string t = i18n::tr(m.title);
    mixhideui::upwardText(dl, a.fontBold ? a.fontBold : ImGui::GetFont(), theme::kFontCaption, ImVec2(std::round(o.x + (w - theme::kFontCaption) * 0.5f), p1.y - 8),
                          P.textDim, t, h - 40);
    ImGui::PopID();
    return;
  }
  gLaid[gLaidHost].emplace_back();
  gLaid[gLaidHost].back().id = m.id, gLaid[gLaidHost].back().a = o, gLaid[gLaidHost].back().b = p1;
  dl->AddRectFilled(o, ImVec2(p1.x, o.y + kBandTitleH), P.surface700, theme::kRadiusLg, ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(o.x, o.y + kBandTitleH - 0.5f), ImVec2(p1.x, o.y + kBandTitleH - 0.5f), theme::withAlpha(P.ink, 0.45f));
  // The title strip: the scope tick, the fold arrow, the name; at the right the menu, the switch, the scope word.
  dl->AddRectFilled(ImVec2(o.x + 6, o.y + 6), ImVec2(o.x + 9, o.y + kBandTitleH - 6), scopeCol(m.scope), 1.0f);
  {
    ImGui::SetCursorScreenPos(ImVec2(o.x + 12, o.y + 2));
    const bool hit = ImGui::InvisibleButton("##fold", ImVec2(16, kBandTitleH - 4));
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(o.x + 20, o.y + kBandTitleH * 0.5f), 8, ImGui::IsItemHovered() ? P.text : P.textDim);
    ui::Hint("Fold it to a narrow strip");
    if (hit) gBandFolded.insert(m.id);
  }
  ImFont* bold = a.fontBold ? a.fontBold : ImGui::GetFont();
  const float fs = theme::kFontCaption + 1, tx = o.x + 30;
  const std::string title = i18n::tr(m.title);
  float rx = p1.x - 6;
  if (m.menu) {
    rx -= 18;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + 2));
    if (ui::IconButton("##modmenu", ui::Icon::More, false, 18, "More options")) ImGui::OpenPopup("##modmenupop");
    // (the menu opens down over the card, inside the band)
    ImGui::SetNextWindowPos(ImVec2(rx + 18, o.y + kBandTitleH), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##modmenupop")) {
      m.menu();
      ImGui::EndPopup();
    }
    rx -= 4;
  }
  if (m.switchLabel) {
    const float bw = ui::TextButtonWidth(m.switchLabel);
    rx -= bw;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 1));
    if (ui::TextButton(m.switchLabel, m.switchOn, bw) && m.onSwitch) m.onSwitch();
    ImGui::PopStyleVar();
    rx -= 6;
  }
  const float titleW = bold->CalcTextSizeA(fs, 1e9f, 0, title.c_str()).x;
  {
    const char* word = scopeWord(m.scope);
    const float sw = ImGui::GetFont()->CalcTextSizeA(kTextFs, 1e9f, 0, word).x;
    if (rx - sw - 10 > tx + titleW) {
      dl->AddText(ImGui::GetFont(), kTextFs, ImVec2(std::round(rx - sw - 2), theme::textTop(ImGui::GetFont(), kTextFs, o.y, kBandTitleH)), scopeText(m.scope), word);
    }
  }
  // (the name: a double-click folds the card)
  ImGui::SetCursorScreenPos(ImVec2(tx, o.y + 1));
  ImGui::InvisibleButton("##title", ImVec2(std::max(4.0f, rx - tx - 2), kBandTitleH - 2));
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) gBandFolded.insert(m.id);
  theme::textEllipsis(dl, bold, fs, ImVec2(tx, theme::textTop(bold, fs, o.y, kBandTitleH)), std::max(10.0f, rx - tx - 4), P.text, title.c_str());
  if (LaidMod* lm = laidNow()) lm->titleEnd = tx + std::min(titleW, std::max(10.0f, rx - tx - 4)), lm->rightStart = rx;
  Cx c{a, dl, o, w, m.variant, false};
  c.form = Form::Band;
  c.band = bandGeo(h, m.variant);
  if (m.free) {  // its own widget under the title strip, in a child of its own (it scrolls when taller)
    ImGui::SetCursorScreenPos(ImVec2(o.x + kPad, o.y + kBandTitleH + 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::BeginChild("##free", ImVec2(w - 2 * kPad, h - kBandTitleH - 12), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground) || gReachOn) m.body(c);
    ImGui::EndChild();
    ImGui::PopStyleVar();
  } else {
    if (c.disp() && m.variant != 3) well(dl, c.d0(), c.d1());
    m.body(c);
  }
  dl->AddRect(o, p1, theme::withAlpha(P.ink, 0.35f), theme::kRadiusLg);
  ImGui::PopID();
}

// The band's tools: the editor's tools, Snap, preview and its loop, the view's options in a menu
// (Wave / Spectrum, Bars / Time, the snap grid, Follow), Edit audio... at the right; the live
// analysis between them when it has the room. Without a clip: the analysis and Edit audio... alone.
// Too narrow for the tools' buttons: the tool in use, the others in its menu.
void bandTools(App& a, State& S, const Clip* cp, const Channel& ch, ImVec2 p0, float w, float h) {
  const float bs = std::min(22.0f, h - 2);
  const float editW = ui::TextButtonWidth("Edit audio...");
  const float editX = std::round(p0.x + w - editW);
  const float cy = p0.y + h * 0.5f;
  if (!S.toolMenu) {  // (called by itself, not for a narrow tools row)
    S.toolRects.clear();
    S.toolA = p0, S.toolB = ImVec2(p0.x + w, p0.y + h);
  }
  editAudioButton(a, S, cp, ch, ImVec2(editX, p0.y + std::round((h - bs) * 0.5f)), bs);
  if (!cp) {
    analysisReadout(a, S, nullptr, ch, p0.x, editX - 12, cy);
    return;
  }
  const Clip& c = *cp;
  for (const char* k : {"tools", "view", "units", "snap", "preview", "follow"}) reach(k);
  ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + std::round((h - bs) * 0.5f)));
  const bool allTools = p0.x + TCount * (bs + 1) + 8 + 4 * (bs + 1) + 12 <= editX;
  if (allTools) {
    for (int t = 0; t < TCount; ++t) {
      ImGui::PushID(t);
      if (ui::ToolButton("##cbt", kTools[t].icon, S.tool == t, bs, t == TWarp ? warpwave::modeWord() : kTools[t].name, kTools[t].desc, kTools[t].key,
                         theme::rgb(kTools[t].role)))
        S.tool = t;
      toolItem(S);
      ImGui::PopID();
      ImGui::SameLine(0, 1);
    }
  } else {  // the tool in use, the others in its menu
    const int t = std::clamp(S.tool, 0, TCount - 1);
    ImGui::PushID(t);
    const bool pick = ui::ToolButton("##cbt", kTools[t].icon, true, bs, t == TWarp ? warpwave::modeWord() : kTools[t].name, kTools[t].desc, kTools[t].key,
                                     theme::rgb(kTools[t].role));
    toolItem(S);
    ImGui::PopID();
    ImGui::SameLine(0, 1);
    const bool more = ui::IconButton("##cetoolmenu", ui::Icon::ChevronDown, false, bs, "The editor's tools");
    toolItem(S);
    if (pick || more) ImGui::OpenPopup("##cetoolpop");
    if (ImGui::BeginPopup("##cetoolpop")) {
      for (int k = 0; k < TCount; ++k)
        if (ImGui::MenuItem(k == TWarp ? warpwave::modeWord() : i18n::tr(kTools[k].name), nullptr, S.tool == k)) S.tool = k;
      ImGui::EndPopup();
    }
  }
  ImGui::SameLine(0, 8);
  if (ui::IconButton("##cesnapon", ui::Icon::Magnet, S.snap != 0, bs, "Snap: edges, fades and warp markers land on the grid (Alt while dragging: free)"))
    S.snap = S.snap ? 0 : 2;
  toolItem(S);
  ImGui::SameLine(0, 1);
  if (ui::IconButton("##ceplay", a.engine.previewing() ? ui::Icon::Stop : ui::Icon::Play, a.engine.previewing(), bs,
                     "Preview: the clip alone (its channel and effects), from its start"))
    previewClip(a, c.id, S.loopPreview);
  toolItem(S);
  ImGui::SameLine(0, 1);
  if (ui::IconButton("##celoop", ui::Icon::Loop, S.loopPreview, bs, "Loop the preview")) S.loopPreview = !S.loopPreview;
  toolItem(S);
  ImGui::SameLine(0, 1);
  const bool viewOpen = ui::IconButton("##ceviewmenu", ui::Icon::ChevronDown, false, bs, "The view: wave or spectrum, bars or time, the snap grid, follow");
  toolItem(S);
  const ImVec2 vb = ImGui::GetItemRectMax();
  if (viewOpen || std::exchange(S.viewDev, false)) ImGui::OpenPopup("##ceviewpop");
  ImGui::SetNextWindowPos(ImVec2(vb.x, vb.y + 2), ImGuiCond_Appearing, ImVec2(1, 0));
  if (ImGui::BeginPopup("##ceviewpop")) {
    if (ImGui::MenuItem(i18n::tr("Wave"), nullptr, !S.spectrum)) S.spectrum = false;
    if (ImGui::MenuItem(i18n::tr("Spectrum"), nullptr, S.spectrum)) S.spectrum = true;
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Bars"), nullptr, !S.seconds)) S.seconds = false;
    if (ImGui::MenuItem(i18n::tr("Time"), nullptr, S.seconds)) S.seconds = true;
    ImGui::Separator();
    if (ImGui::BeginMenu(i18n::tr("Snap"))) {
      const char* snaps[] = {i18n::tr("Off"), i18n::tr("Bar"), i18n::tr("Beat"), i18n::tr("1/2 beat"), i18n::tr("1/4 beat")};
      for (int i = 0; i < 5; ++i)
        if (ImGui::MenuItem(snaps[i], nullptr, S.snap == i)) S.snap = i;
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(i18n::tr("Follow"), nullptr, S.follow)) S.follow = !S.follow;
    ImGui::EndPopup();
  }
  analysisReadout(a, S, cp, ch, vb.x + 16, editX - 12, cy);
}

// The sample display: the band's tools over the waveform (the clip as it plays inside its sample,
// with its overview), or a sampler channel's whole sample with its handles and loop.
void waveSection(App& a, State& S, Host host, const Clip* cp, const Channel& ch, ImVec2 o, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p1(o.x + w, o.y + h);
  dl->AddRectFilled(o, p1, P.surface800, theme::kRadiusLg);
  const float toolH = 28;
  S.toolMenu = false;
  bandTools(a, S, cp, ch, ImVec2(o.x + 6, o.y + 2), w - 12, toolH - 4);
  const ImVec2 w0(o.x + 1, o.y + toolH), w1(p1.x - 1, p1.y - 1);
  if (w1.y - w0.y > 30) {
    if (cp) {
      waveArea(a, S, host, *cp, ch, w0, w1, w1.y - w0.y >= 150 ? 26.0f : 18.0f);
    } else {  // a sampler channel no clip plays: its whole sample, with its handles and loop
      reach("overview");
      reach("wave");
      S.waveA = w0, S.waveB = w1;
      S.ovA = S.waveA, S.ovB = S.waveB;
      sampleStrip(a, S, ch, w0, w1, true, ViewOnFile{});
    }
  }
  dl->AddRect(o, p1, theme::withAlpha(P.ink, 0.35f), theme::kRadiusLg);
}

// The clip's head card: its colour and name, the menu and the presets in the title strip; the
// file, its warp markers, who shares it and the selection count where a card has its display; the
// channel's route, Open in Loupe, and its volume, pan and pitch (the Output module's controls).
float headCardW() { return modW(3); }
void headCard(App& a, State& S, Host host, const Clip* c, const Channel& ch, ImVec2 o, float h) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = headCardW();
  const ImVec2 p1(o.x + w, o.y + h);
  ImGui::PushID("head");
  dl->AddRectFilled(o, p1, P.surface800, theme::kRadiusLg);
  dl->AddRectFilled(o, ImVec2(p1.x, o.y + kBandTitleH), P.surface700, theme::kRadiusLg, ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(o.x, o.y + kBandTitleH - 0.5f), ImVec2(p1.x, o.y + kBandTitleH - 0.5f), theme::withAlpha(P.ink, 0.45f));
  // the colour, the name
  reach("colour");
  ImGui::SetCursorScreenPos(ImVec2(o.x + 6, o.y + 4));
  uint32_t rgb = ch.color;
  if (ui::ColorSwatch("##cecol", &rgb, ImVec2(14, 14)) && rgb != ch.color) ops::setChannelsColor(a.doc, {ch.id}, rgb);
  ui::Hint("Colour of the clip's channel (every clip of the sample)");
  // at the right: the menu, then the previous / next preset
  float rx = p1.x - 6 - 18;
  reach("menu");
  ImGui::SetCursorScreenPos(ImVec2(rx, o.y + 2));
  const bool menuClick = ui::IconButton("##cemenu", ui::Icon::Menu, false, 18, "Channel menu: presets, state, rename, clone, replace or delete the channel");
  if (menuClick || std::exchange(S.menuDev, false)) ImGui::OpenPopup("##cemenupop");
  ImGui::SetNextWindowPos(ImVec2(rx + 18, o.y + kBandTitleH), ImGuiCond_Appearing, ImVec2(1, 0));
  if (ImGui::BeginPopup("##cemenupop")) {
    headerMenu(a, c, ch);
    ImGui::EndPopup();
  }
  reach("presets");
  {
    const PluginTarget t{ch.id, -1};
    rx -= 2 + 18;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + 2));
    if (ui::IconButton("##cepnext", ui::Icon::Right, false, 18, "Next preset")) stepPluginPreset(a, t, 1);
    rx -= 18;
    ImGui::SetCursorScreenPos(ImVec2(rx, o.y + 2));
    if (ui::IconButton("##cepprev", ui::Icon::Left, false, 18, "Previous preset")) stepPluginPreset(a, t, -1);
  }
  reach("name");
  ImFont* bold = a.fontBold ? a.fontBold : ImGui::GetFont();
  const float tfs = theme::kFontCaption + 1;
  const std::string name = c && !c->name.empty() ? c->name : ch.name;
  theme::textEllipsis(dl, bold, tfs, ImVec2(o.x + 26, theme::textTop(bold, tfs, o.y, kBandTitleH)), rx - o.x - 30, P.text, name.c_str());
  // The controls under it: the Output module's, the display holding the file's lines.
  Mod out = outputModule(a, c, ch, host);
  Cx x{a, dl, o, w, out.variant, false};
  x.form = Form::Band;
  x.band = bandGeo(h, out.variant);
  x.head = true;
  reach("info");
  if (c) reach("selcount");
  {
    const float lh = 15;
    float y = o.y + kBandTitleH + 6;
    const float lim = o.y + x.band.rowA - 4;
    ImFont* uf = ImGui::GetFont();
    const float cfs = theme::kFontCaption;
    auto line = [&](const std::string& t, ImU32 col) {
      if (y + lh > lim) return;
      theme::textEllipsis(dl, uf, cfs, ImVec2(o.x + kPad, y), w - 2 * kPad, col, t.c_str());
      y += lh;
    };
    const std::string path = samplePath(ch);
    std::string file = path.empty() ? std::string(i18n::tr("No sample")) : fs::path(path).filename().string();
    if (c && !c->warp.empty()) {
      const float bw = warpwave::badge(dl, ImVec2(o.x + kPad, y), static_cast<int>(c->warp.size()));
      theme::textEllipsis(dl, uf, cfs, ImVec2(o.x + kPad + bw + 6, y + 1), w - 2 * kPad - bw - 6, P.text, file.c_str());
      y += lh + 2;
    } else {
      line(file, P.text);
    }
    if (const ClipSource src = clipSourceIfReady(ch); src.sample) {
      char b[160];
      std::snprintf(b, sizeof b, "%.1f kHz \xC2\xB7 %s \xC2\xB7 %.2f s", src.sample->sampleRate / 1000.0, src.sample->mono() ? i18n::tr("mono") : i18n::tr("stereo"),
                    src.sample->seconds());
      line(b, P.textDim);
    }
    const int shared = clipsSharing(s, ch.id);
    if (shared > 1) {
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("shared by %d clips"), shared);
      line(b, sampleOrange());
    }
    if (c) {
      const std::vector<ObjectId> sel = selectionFor(a, c->id);
      if (sel.size() > 1) {
        char b[64];
        std::snprintf(b, sizeof b, i18n::tr("and %d more selected"), static_cast<int>(sel.size()) - 1);
        line(b, P.textDim);
      }
    }
    // the room left over the route: the sample's thumbnail (its start and end marked)
    if (lim - 2 - (y + 2) >= 16) {
      const ImVec2 t0(o.x + kPad, y + 2), t1(p1.x - kPad, lim - 2);
      dl->AddRectFilled(t0, t1, P.surface900, theme::kRadiusSm);
      sampleWaveIn(dl, ch, ImVec2(t0.x + 2, t0.y + 2), ImVec2(t1.x - 2, t1.y - 2), 0.6f);
      double sf = 0, ef = 1;
      channelTrim(ch, sf, ef);
      markAt(dl, ImVec2(t0.x + 2, t0.y + 2), ImVec2(t1.x - 2, t1.y - 2), sf, handleBlue());
      markAt(dl, ImVec2(t0.x + 2, t0.y + 2), ImVec2(t1.x - 2, t1.y - 2), ef, handleBlue());
    }
  }
  out.body(x);
  dl->AddRect(o, p1, theme::withAlpha(P.ink, 0.35f), theme::kRadiusLg);
  ImGui::PopID();
}

// The Clips layout's band (the clip band, a sampler's card in the device band): the clip's own
// view in the band's space, its parts side by side as the band's cards: the head card (the clip and
// its channel), the sample display with the tools, then a card per module (the clip's, the
// sample's, the channel's), scrolling sideways inside the band when they do not all fit. Nothing
// opens over the band or the views above it.
void nativeBand(App& a, State& S, Host host, const Clip* cp, const Channel& ch, ImVec2 p0, ImVec2 size) {
  const auto& P = pal();
  S.chipsMode = false;
  ImGui::SetCursorScreenPos(p0);
  Measure meas(static_cast<int>(host), p0, ImVec2(p0.x + size.x, p0.y + size.y), "band");
  std::vector<Mod> mods = modulesOf(a, cp, ch);
  // (the cards scroll inside the band's padding on both sides: a card cut at either side stops
  // `pad` short of the band's edge, its controls clear of it)
  const float pad = 4;
  const float innerW = std::max(1.0f, size.x - 2 * pad);
  float total = headCardW() + kBandGap;
  const float waveW = std::floor(std::clamp(size.x * 0.42f, 420.0f, 760.0f));
  total += waveW + kBandGap;
  for (const Mod& m : mods) total += bandCardW(m) + kBandGap;
  total -= kBandGap;
  const bool scrolls = total > innerW + 0.5f;
  const float sbH = scrolls ? 10.0f : 0.0f;
  const float h = std::max(kBandCompactMinH, std::floor(size.y - 2 * pad - sbH));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
  ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), P.surface900);
  ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y));
  ImGui::BeginChild("##cenative", ImVec2(innerW, size.y), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  // (a tab asked for, Channel settings or the dev: its first card in view)
  if (S.scrollTab >= 0) {
    float x = headCardW() + kBandGap + waveW + kBandGap;
    for (const Mod& m : mods) {
      if (onTab(m, S.scrollTab, cp != nullptr) || (S.scrollTab == TabChannel && std::string(m.id) == "channel")) {
        ImGui::SetScrollX(std::min(x, std::max(0.0f, total - innerW)));
        break;
      }
      x += bandCardW(m) + kBandGap;
    }
    if (S.scrollTab == TabEdit || S.scrollTab == TabAnalysis) ImGui::SetScrollX(0);
    S.scrollTab = -1;
  }
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float visL = ImGui::GetWindowPos().x, visR = visL + innerW;
  gLaidHost = static_cast<int>(host);
  gLaid[gLaidHost].clear();
  float x = o.x;
  const float y = o.y + pad;
  int cards = 0;
  S.cardRects.clear();
  headCard(a, S, host, cp, ch, ImVec2(x, y), h);
  S.cardRects.push_back({"head", {ImVec2(x, y), ImVec2(x + headCardW(), y + h)}});
  x += headCardW() + kBandGap, ++cards;
  waveSection(a, S, host, cp, ch, ImVec2(x, y), waveW, h);
  S.cardRects.push_back({"wave", {ImVec2(x, y), ImVec2(x + waveW, y + h)}});
  x += waveW + kBandGap, ++cards;
  S.tabRects.clear();
  for (Mod& m : mods) {
    const float cw = bandCardW(m);
    // (a card out of view is left out: the dev's checklist draws them all)
    if (gReachOn || (x + cw >= visL && x <= visR)) bandCard(a, m, ImVec2(std::round(x), y), h);
    S.cardRects.push_back({m.id, {ImVec2(x, y), ImVec2(x + cw, y + h)}});
    x += cw + kBandGap, ++cards;
  }
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(total, h + 2 * pad));
  // The wheel over the band (not taken by a knob) moves along the cards.
  const ImGuiIO& io = ImGui::GetIO();
  if (scrolls && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && (io.MouseWheel != 0 || io.MouseWheelH != 0) && !ui::wheelTaken() && !io.KeyCtrl)
    ImGui::SetScrollX(std::clamp(ImGui::GetScrollX() - (io.MouseWheel + io.MouseWheelH) * 60.0f, 0.0f, ImGui::GetScrollMaxX()));
  S.panelScrolls = scrolls;
  S.panelA = p0, S.panelB = ImVec2(p0.x + size.x, p0.y + size.y);
  S.tabsA = S.panelA, S.tabsB = S.panelB;
  ImGui::EndChild();
  meas.end(cards);
}

// ---- Smooth: the inspector's rows and the audio editor's local inspector

// Each module's section folded (by id) in Smooth's local inspector; folded ones draw nothing.
std::set<std::string> gRowsFolded = {"smpwarp", "voice", "lfo", "fxshape", "fxtone", "fxspace", "channel", "poly", "echo", "layer", "notefx", "macros"};
bool gRowsUnfoldAll = false;  // (dev) every section open

// A module as rows down the column at the cursor, `w` wide: its controls (a row per knob, the buttons
// in lines), its display a short strip on top when `displays`. `heading`: a line naming it, with its
// switch and its menu (the Region card's parts); `omit` the buttons left out (their rows are the card's).
void rowsBody(App& a, Mod& m, float w, bool displays, const std::vector<std::string>* omit) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  gLaid[gLaidHost].emplace_back();
  gLaid[gLaidHost].back().id = m.id, gLaid[gLaidHost].back().a = o;
  Flow flow;
  flow.y = o.y;
  Cx x{a, dl, o, w, m.variant, false};
  x.form = Form::Rows;
  x.flow = &flow;
  x.cols = std::max(1, m.cols);
  x.omit = omit;
  x.showDisp = displays && !m.free;
  ImGui::PushID(m.id);
  if (m.free) {
    ImGui::SetCursorScreenPos(o);
    ImGui::BeginGroup();
    ImGui::PushItemWidth(w);
    m.body(x);
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    flow.y = std::max(flow.y, ImGui::GetItemRectMax().y + kRowsGap);
  } else {
    if (x.disp()) {
      if (m.variant != 3) well(dl, x.d0(), x.d1());
      flow.y = x.d1().y + kRowsGap + 2;
      flow.rows = 1;  // (the first row has its line under the display)
    }
    m.body(x);
    x.closeLine();
  }
  ImGui::PopID();
  gLaid[gLaidHost].back().b = ImVec2(o.x + w, flow.y);
  ImGui::SetCursorScreenPos(ImVec2(o.x, flow.y));
  ImGui::Dummy(ImVec2(w, 0));
}

// A section's heading in Smooth's look (a hairline, the name dim, a disclosure arrow); `menu`, `sw`:
// the module's menu and switch at its right. A click on it folds or unfolds the section.
bool rowsHeading(App& a, Mod& m, bool foldable, bool open) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = 24;
  float rx = p.x + w;
  ImGui::PushID(m.id);
  if (m.menu) {
    rx -= 20;
    ImGui::SetCursorScreenPos(ImVec2(rx, p.y + 2));
    if (ui::IconButton("##modmenu", ui::Icon::More, false, 20, "More options")) ImGui::OpenPopup("##modmenupop");
    ImGui::SetNextWindowPos(ImVec2(rx + 20, p.y + h), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##modmenupop")) {
      m.menu();
      ImGui::EndPopup();
    }
    rx -= 4;
  }
  if (m.switchLabel && open) {
    const float bw = ui::TextButtonWidth(m.switchLabel);
    rx -= bw;
    ImGui::SetCursorScreenPos(ImVec2(rx, p.y + 2));
    if (ui::TextButton(m.switchLabel, m.switchOn, bw) && m.onSwitch) m.onSwitch();
    rx -= 4;
  }
  ImGui::SetCursorScreenPos(p);
  bool toggled = false;
  if (foldable) {
    toggled = ImGui::InvisibleButton("##fold", ImVec2(std::max(4.0f, rx - p.x), h));
    if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(rx, p.y + h), theme::withAlpha(P.text, 0.04f));
  }
  const float fs = ImGui::GetFontSize();
  const float ty = std::round(p.y + (h - fs) * 0.5f);
  float tx = p.x + 2;
  if (foldable) {
    ui::drawIcon(dl, open ? ui::Icon::ChevronDown : ui::Icon::Chevron, ImVec2(p.x + 6, p.y + h * 0.5f), 8, P.textDim);
    tx += 14;
  }
  theme::textEllipsis(dl, a.fontBold ? a.fontBold : ImGui::GetFont(), fs, ImVec2(tx, ty), std::max(10.0f, rx - tx - 4), P.text, i18n::tr(m.title));
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  ImGui::Dummy(ImVec2(w, 2));
  return toggled;
}

// Smooth's audio editor (the lower pane's Audio tab): the local inspector on the left (the clip
// and its channel at its head, then a section per module of the sample and the channel, Output
// last), the tools and the waveform on the right. The clip's own settings are the inspector's
// Region card.
void smoothPane(App& a, State& S, const Clip* cp, const Channel& ch, ImVec2 p0, ImVec2 size) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  Measure meas(static_cast<int>(Host::Pane), p0, ImVec2(p0.x + size.x, p0.y + size.y), "pane");
  // (a tab asked for: its sections open in the local inspector)
  if (S.scrollTab >= 0) {
    for (const Mod& m : modulesOf(a, cp, ch))
      if (onTab(m, S.scrollTab, cp != nullptr)) gRowsFolded.erase(m.id);
    S.scrollTab = -1;
  }
  const float iw = std::floor(std::clamp(size.x * 0.26f, 220.0f, 268.0f));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // ---- the local inspector
  ImGui::SetCursorScreenPos(p0);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface800));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
  ImGui::BeginChild("##celocal", ImVec2(iw, size.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  gLaidHost = static_cast<int>(Host::Pane);
  gLaid[gLaidHost].clear();
  int cards = 0;
  {
    ImDrawList* idl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    // the head: the colour, the clip's (or channel's) name and its file; the menu and the presets
    reach("colour");
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + 3));
    uint32_t rgb = ch.color;
    if (ui::ColorSwatch("##cecol", &rgb, ImVec2(14, 14)) && rgb != ch.color) ops::setChannelsColor(a.doc, {ch.id}, rgb);
    ui::Hint("Colour of the region's channel (every region of the sample)");
    reach("menu");
    ImGui::SetCursorScreenPos(ImVec2(q.x + w - 20, q.y));
    const bool menuClick = ui::IconButton("##cemenu", ui::Icon::Menu, false, 20, "Channel menu: presets, state, rename, clone, replace or delete the channel");
    if (menuClick || std::exchange(S.menuDev, false)) ImGui::OpenPopup("##cemenupop");
    ImGui::SetNextWindowPos(ImVec2(q.x + w, q.y + 22), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##cemenupop")) {
      headerMenu(a, cp, ch);
      ImGui::EndPopup();
    }
    reach("presets");
    {
      const PluginTarget t{ch.id, -1};
      ImGui::SetCursorScreenPos(ImVec2(q.x + w - 20 - 4 - 40, q.y));
      if (ui::IconButton("##cepprev", ui::Icon::Left, false, 20, "Previous preset")) stepPluginPreset(a, t, -1);
      ImGui::SameLine(0, 0);
      if (ui::IconButton("##cepnext", ui::Icon::Right, false, 20, "Next preset")) stepPluginPreset(a, t, 1);
    }
    reach("name");
    reach("info");
    const std::string name = cp && !cp->name.empty() ? cp->name : ch.name;
    theme::textEllipsis(idl, a.fontBold, theme::kFontBody, ImVec2(q.x + 20, q.y + 2), w - 20 - 68, P.text, name.c_str());
    const std::string path = samplePath(ch);
    std::string line2 = path.empty() ? std::string(i18n::tr("No sample")) : fs::path(path).filename().string();
    if (cp) line2 = std::string(i18n::tr("on")) + " " + ch.name + "  \xC2\xB7  " + line2;
    theme::textEllipsis(idl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + 20, q.y + 21), w - 20, P.textDim, line2.c_str());
    float y = q.y + 38;
    std::string line3;
    if (const ClipSource src = clipSourceIfReady(ch); src.sample) {
      char b[160];
      std::snprintf(b, sizeof b, "%.1f kHz \xC2\xB7 %s \xC2\xB7 %.2f s", src.sample->sampleRate / 1000.0, src.sample->mono() ? i18n::tr("mono") : i18n::tr("stereo"),
                    src.sample->seconds());
      line3 = b;
    }
    if (cp && !cp->warp.empty()) {
      const float bw = warpwave::badge(idl, ImVec2(q.x + 20, y), static_cast<int>(cp->warp.size()));
      theme::textEllipsis(idl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + 26 + bw, y + 1), w - 26 - bw, P.textDim, line3.c_str());
    } else {
      theme::textEllipsis(idl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + 20, y), w - 20, P.textDim, line3.c_str());
    }
    y += 16;
    const int shared = clipsSharing(s, ch.id);
    if (shared > 1) {
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("shared by %d regions"), shared);
      theme::textEllipsis(idl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + 20, y), w - 20, sampleOrange(), b);
      y += 16;
    }
    if (cp) {
      reach("selcount");
      const std::vector<ObjectId> sel = selectionFor(a, cp->id);
      if (sel.size() > 1) {
        char b[64];
        std::snprintf(b, sizeof b, i18n::tr("and %d more selected"), static_cast<int>(sel.size()) - 1);
        theme::textEllipsis(idl, a.fontRegular, theme::kFontCaption, ImVec2(q.x + 20, y), w - 20, P.textDim, b);
        y += 16;
      }
    }
    // The mixer track the channel plays through, under the name: the box and the free track's
    // button, as a plugin window's header has them.
    {
      reach("route");
      reach("routefree");
      const ObjectId chId = ch.id;
      const float rb = ImGui::GetFrameHeight();
      ImGui::SetCursorScreenPos(ImVec2(q.x, y + 4));
      if (chanroute::box(a, chId, "##ceroute", w - rb - 4, "ceroute")) a.later([&a, chId] { chanroute::routeToFree(a, chId); });
      ImGui::SetCursorScreenPos(ImVec2(q.x + w - rb, y + 4));
      if (chanroute::button(a, chId, rb)) a.later([&a, chId] { chanroute::routeToFree(a, chId); });
      devNoteRect("ceroutefree");
      y += rb + 8;
    }
    ImGui::SetCursorScreenPos(ImVec2(q.x, y + 2));
    ImGui::Dummy(ImVec2(w, 0));
    // the sections: the sample's and the channel's modules (the clip's are the inspector's Region card), Output last
    std::vector<Mod> mods = modulesOf(a, cp, ch);
    mods.push_back(outputModule(a, cp, ch, Host::Pane));
    for (Mod& m : mods) {
      if (cp && (m.scope == MScope::Clip)) continue;
      const bool open = gRowsUnfoldAll || !gRowsFolded.count(m.id);
      const ImVec2 hp = ImGui::GetCursorScreenPos();
      idl->AddLine(ImVec2(hp.x - 10, hp.y + 0.5f), ImVec2(hp.x + w + 10, hp.y + 0.5f), P.line);
      ImGui::Dummy(ImVec2(0, 2));
      if (rowsHeading(a, m, true, open)) {
        if (open) gRowsFolded.insert(m.id), gRowsUnfoldAll = false;
        else gRowsFolded.erase(m.id);
      }
      ++cards;
      if (!open) continue;
      rowsBody(a, m, w, true, nullptr);
      ImGui::Dummy(ImVec2(0, 4));
    }
  }
  ImGui::EndChild();
  dl->AddLine(ImVec2(p0.x + iw + 0.5f, p0.y), ImVec2(p0.x + iw + 0.5f, p0.y + size.y), P.line);
  // ---- the editor: its tools row, then the waveform (or the channel's whole sample)
  const ImVec2 e0(p0.x + iw + 1, p0.y);
  const float ew = size.x - iw - 1;
  const float toolH = 34;
  if (cp && ew >= 820) {
    toolStrip(a, S, cp, ch, e0, ew, toolH);
  } else {
    dl->AddRectFilled(e0, ImVec2(e0.x + ew, e0.y + toolH), P.surface900);
    S.toolMenu = false;
    bandTools(a, S, cp, ch, ImVec2(e0.x + 8, e0.y + 5), ew - 20, toolH - 10);
  }
  const ImVec2 w0(e0.x, e0.y + toolH), w1(e0.x + ew, p0.y + size.y);
  if (w1.y - w0.y > 40) {
    if (cp) {
      waveArea(a, S, Host::Pane, *cp, ch, w0, w1, 32.0f);
    } else {
      reach("overview");
      reach("wave");
      S.waveA = w0, S.waveB = w1;
      S.ovA = S.waveA, S.ovB = S.waveB;
      sampleStrip(a, S, ch, w0, w1, true, ViewOnFile{});
    }
  }
  S.panelA = p0, S.panelB = ImVec2(p0.x + iw, p0.y + size.y);
  S.tabsA = p0, S.tabsB = ImVec2(p0.x + size.x, p0.y + size.y);
  S.panelScrolls = false;
  meas.end(cards);
}

}  // namespace

// ------------------------------------------------------------------ public

const char* clipWord() { return smooth() ? i18n::tr("region") : tracker() ? i18n::tr("sample clip") : i18n::tr("clip"); }

ObjectId windowClip() { return gWindow.channel != kNoId ? gWindow.clip : kNoId; }
ObjectId windowChannel() { return gWindow.channel; }
void closeWindow() { gWindow = {}; }
ObjectId paneClip() { return gPaneOn ? gPane.clip : kNoId; }
ObjectId paneChannel() { return gPaneOn ? gPane.channel : kNoId; }
void paneShowsLoupe() { gPaneOn = false; }
bool followInPane(ObjectId clip) {
  if (!gPaneOn) return false;
  gPane.clip = clip;
  gPane.channel = kNoId;  // (the clip's, when drawn)
  return true;
}
bool shown(Host h) { return gDrawFrame[static_cast<int>(h)] >= ImGui::GetFrameCount() - 1; }

bool selectionOf(ObjectId clip, double& t0, double& t1) {
  for (int h = 0; h < 4; ++h) {
    const State& S = gHost[h];
    // (an editor drawn lately: one closed a while ago keeps no selection that counts)
    if (S.clip != clip || !(S.selT1 > S.selT0) || gDrawFrame[h] < ImGui::GetFrameCount() - 30) continue;
    t0 = S.selT0, t1 = S.selT1;
    return true;
  }
  return false;
}

bool showsChannel(const App& a, ObjectId channel) {
  const Song& s = a.doc.song();
  auto of = [&](const Target& t) {
    if (t.channel == channel) return true;
    const Clip* c = t.clip != kNoId ? s.clip(t.clip) : nullptr;
    return c && c->kind == ClipKind::Audio && c->source == channel;
  };
  if (tracker()) return of(gWindow);  // K-391: the Tracker's floating window (its band page is the Sample editor's own)
  return of(gWindow) || (gPaneOn && of(gPane));
}

void closeChannel(const App& a, ObjectId channel) {
  const Song& s = a.doc.song();
  auto of = [&](const Target& t) {
    const Clip* c = t.clip != kNoId ? s.clip(t.clip) : nullptr;
    return t.channel == channel || (c && c->kind == ClipKind::Audio && c->source == channel);
  };
  if (of(gWindow)) gWindow = {};
  if (gPaneOn && of(gPane)) gPaneOn = false;
}

bool takesChannel(const App& a, ObjectId channel) {
  const Channel* ch = a.doc.song().channel(channel);
  return ch && clipwin::ownsType(ch->plugin.type);
}

namespace {

// The window's body for a clip (`c`) or for a sampler channel no clip plays (`c` null).
// ---- the Main view
//
// The sampler's own small window on one page, nothing more: the header (with the mixer track box),
// the whole sample with its start and end handles and the notes' loop, the sample's file with
// Replace sample..., then that window's settings in its four pages' groups: Sample (Playback, Time
// stretching, Processing), Envelope and filter (the volume envelope, the filter, the LFO), Voice
// (root and voice, and the volume, pan and pitch of its strip) and Sample FX (shape, tone, space).
// Each control is the sampler's own parameter: one value with the Advanced view, one undo step.
// Each group sits in a box as the old window had them, its name in capitals, a row of boxes per page.
// The page fits the window while it can (its knobs a little smaller first), then scrolls.
namespace mainv {
enum class K { Knob, Toggle, Seg, Engine, Fit, ChanKnob, Loupe, Replace };
struct C {
  K k;
  int idx = 0;
  const char* name = "";
  bool bip = false;
  double step = 0;
  const char* const* opts = nullptr;
  int n = 0;
  const char* hint = nullptr;
};
struct G {
  const char* id;
  const char* title;
  int page;      // 0 Sample, 1 Envelope and filter, 2 Voice, 3 Sample FX
  int chipCols;  // the chips two to a line, or one
  std::vector<C> chips, knobs;
  std::vector<const char*> keys;  // (dev) the checklist's keys it holds
};
const char* const kLoopModes[] = {"Forward", "Ping-pong"};
const char* const kFilterTypes[] = {"Off", "LP", "LP24", "BP", "HP", "Notch"};
const char* const kLfoTargets[] = {"Off", "Vol", "Pan", "Cut", "Pitch"};
const char* const kLfoShapes[] = {"Sine", "Triangle", "Pulse"};
const char* const kRooms[] = {"Room", "Hall"};
C knob(int idx, const char* name, bool bip = false, double step = 0) { return C{K::Knob, idx, name, bip, step}; }
C chan(ChannelParam p, const char* name, bool bip = false, double step = 0) { return C{K::ChanKnob, static_cast<int>(p), name, bip, step}; }
C toggle(int idx, const char* name, const char* hint) { return C{K::Toggle, idx, name, false, 0, nullptr, 0, hint}; }
C segc(int idx, const char* const* o, int n, const char* hint = nullptr) { return C{K::Seg, idx, "", false, 0, o, n, hint}; }
const std::vector<G>& groups() {
  using namespace pp;
  static const std::vector<G> v = {
      {"playback", "Playback", 0, 2,
       {toggle(SmpLoop, "Loop", "Held notes repeat the loop between the loop points"), segc(SmpLoopMode, kLoopModes, 2),
        toggle(SmpReverse, "Reverse", "The sampler plays the sample backwards: its notes and every clip of it"),
        toggle(SmpOneShot, "One shot", "Each note plays the sample to its end, however short the note")},
       {knob(SmpDeclick, "Declick", false, 1.0)},
       {"loop.notes", "reverse.notes", "oneshot", "declick"}},
      {"stretch", "Time stretching", 0, 1, {C{K::Engine}, C{K::Fit}}, {knob(SmpPitchShift, "Pitch", true, 1.0), knob(SmpTimeStretch, "Time")},
       {"engine", "fittempo", "pitchshift", "timestretch"}},
      {"process", "Processing", 0, 2,
       {toggle(SmpNormalize, "Normalize", "Plays the sample at its loudest without clipping"),
        toggle(SmpRemoveDc, "Remove DC", "Removes a constant offset from the sample"),
        toggle(SmpPolarity, "Invert polarity", "Turns the sample's waveform upside down"),
        toggle(SmpSwapStereo, "Swap stereo", "Swaps the sample's left and right")},
       {knob(SmpFadeIn, "Fade in"), knob(SmpFadeOut, "Fade out"), knob(SmpLoopXfade, "Crossfade")},
       {"normalize.sample", "removedc", "polarity", "swapstereo", "smpfades"}},
      {"env", "Volume envelope", 1, 1, {},
       {knob(SmpEnvDelay, "Delay"), knob(SmpAttack, "Attack"), knob(SmpEnvHold, "Hold"), knob(SmpDecay, "Decay"), knob(SmpSustain, "Sustain"),
        knob(SmpRelease, "Release"), knob(SmpVelVolume, "Velocity")},
       {"volenv"}},
      {"filter", "Filter", 1, 1, {segc(SmpFilterType, kFilterTypes, 6, "The filter on each note: off, low pass (12 or 24 dB), band pass, high pass or notch")},
       {knob(SmpCutoff, "Cutoff"), knob(SmpReso, "Resonance"), knob(SmpFiltEnv, "Env amount", true), knob(SmpFiltAttack, "Attack"),
        knob(SmpFiltDecay, "Decay"), knob(SmpFiltSustain, "Sustain"), knob(SmpFiltRelease, "Release"), knob(SmpVelCutoff, "Velocity", true),
        knob(SmpKeyCutoff, "Key", true)},
       {"filter"}},
      {"lfo", "LFO", 1, 1, {segc(SmpLfoTarget, kLfoTargets, 5, "What the LFO moves: the volume, pan, the filter's cutoff or the pitch"), segc(SmpLfoShape, kLfoShapes, 3)},
       {knob(SmpLfoRate, "Speed"), knob(SmpLfoAmount, "Amount", true), knob(SmpLfoDelay, "Delay")},
       {"lfo"}},
      {"voice", "Root and voice", 2, 1, {},
       {knob(SmpRoot, "Root", false, 1.0), knob(SmpFine, "Fine", true, 1.0), knob(SmpPan, "Pan", true), knob(SmpVoices, "Voices", false, 1.0),
        knob(SmpGlide, "Glide"), knob(SmpVolume, "Volume")},
       {"root", "voice"}},
      {"channel", "Channel", 2, 1, {C{K::Loupe}},
       {chan(ChannelParam::Volume, "Volume"), chan(ChannelParam::Pan, "Pan", true), chan(ChannelParam::Pitch, "Pitch", true, 1.0)},
       {"vol", "pan", "pitch", "loupe"}},
      {"fxshape", "FX shape", 3, 2,
       {toggle(SmpPreClip, "Boost clips", "The boost clips the peaks instead of letting them through"),
        toggle(SmpPreFadeStereo, "Stereo fade", "The sound moves from one side to the other over the sample")},
       {knob(SmpPreRampIn, "Ramp in"), knob(SmpPreRampOut, "Ramp out"), knob(SmpPreWarp, "Time warp", true), knob(SmpPreBoost, "Boost"),
        knob(SmpPreStereoDelay, "Stereo delay", true)},
       {"fx.shape"}},
      {"fxtone", "FX tone", 3, 1, {},
       {knob(SmpPreCutoff, "Cutoff"), knob(SmpPreReso, "Resonance"), knob(SmpPreTilt, "Tilt", true), knob(SmpPreRingMix, "Ring mix"),
        knob(SmpPreRingFreq, "Ring freq")},
       {"fx.tone"}},
      {"fxspace", "FX space", 3, 1, {segc(SmpPreReverbType, kRooms, 2)}, {knob(SmpPreReverbMix, "Reverb")}, {"fx.space"}},
  };
  return v;
}

// One look's measures.
struct MGeo {
  float r = 13, cw = 60;  // a knob's radius, its cell's width
  float chipH = 24, gap = 6, titleH = 22, nameH = 15, valueH = 15;
  float knobH() const { return nameH + 2 * r + 4 + valueH; }
};
struct Ctx {
  App& a;
  const Clip* c;
  const Channel& ch;
  MGeo g;
  ImDrawList* dl;
};

void label(ImDrawList* dl, ImFont* f, float fs, ImVec2 at, ImU32 col, const char* t) { dl->AddText(f, fs, ImVec2(std::round(at.x), std::round(at.y)), col, t); }
float textW(ImFont* f, float fs, const char* t) { return f->CalcTextSizeA(fs, 1e9f, 0, t).x; }
// Text centred on `cx`, never shrunk or cut: one that does not fit `maxW` is counted (dev check).
void centred(ImDrawList* dl, ImFont* f, float fs, float cx, float y, float maxW, ImU32 col, const char* t) {
  const float w = textW(f, fs, t);
  if (w > maxW + 0.5f) ++gTextCut;
  dl->AddText(f, fs, ImVec2(std::round(cx - w * 0.5f), std::round(y)), col, t);
}
ImFont* boldFont(const Ctx& x) { return x.a.fontBold ? x.a.fontBold : ImGui::GetFont(); }

// A chip's width: a toggle's text, a segmented row's longest choice times its choices, the engine's
// three names, the tempo field.
float segW(const char* const* o, int n) {
  float m = 0;
  for (int k = 0; k < n; ++k) m = std::max(m, ImGui::CalcTextSize(i18n::tr(o[k])).x);
  return std::ceil(m + 20) * static_cast<float>(n) + 4;
}
const char* const* engineNames() {
  static const char* names[3] = {stretch::modeName(stretch::kChoices[0]), stretch::modeName(stretch::kChoices[1]), stretch::modeName(stretch::kChoices[2])};
  return names;
}
float chipW(const C& c) {
  switch (c.k) {
    case K::Toggle: return std::ceil(labelNeed(i18n::tr(c.name)) + 16);
    case K::Seg: return segW(c.opts, c.n);
    case K::Engine: return segW(engineNames(), 3);
    case K::Fit: return 150;
    case K::Loupe: return std::ceil(labelNeed(i18n::tr("Open in Loupe")) + 16);
    case K::Replace: return std::ceil(labelNeed(i18n::tr("Replace sample...")) + 16);
    default: return 0;
  }
}
// The chips in lines of `chipCols`, each at its own width, the lines as wide as the widest (their
// chips widened alike to fill it).
int chipLines(const G& gr) { return (static_cast<int>(gr.chips.size()) + gr.chipCols - 1) / gr.chipCols; }
float chipLineW(const G& gr, int line, const MGeo& g) {
  float w = 0;
  for (int k = line * gr.chipCols; k < std::min(static_cast<int>(gr.chips.size()), (line + 1) * gr.chipCols); ++k)
    w += chipW(gr.chips[static_cast<size_t>(k)]) + (k > line * gr.chipCols ? g.gap : 0.0f);
  return w;
}
float chipsW(const G& gr, const MGeo& g) {
  float w = 0;
  for (int l = 0; l < chipLines(gr); ++l) w = std::max(w, chipLineW(gr, l, g));
  return w;
}
float chipsH(const G& gr, const MGeo& g) {
  const int n = chipLines(gr);
  return n ? static_cast<float>(n) * g.chipH + static_cast<float>(n - 1) * g.gap : 0.0f;
}
// A knob's cell: the look's width, or its name's when longer.
float cellW(const C& c, const MGeo& g) { return std::max(g.cw, std::ceil(ImGui::GetFont()->CalcTextSizeA(kTextFs, 1e9f, 0, i18n::tr(c.name)).x + 12)); }
float knobsW(const G& gr, const MGeo& g) {
  float w = 0;
  for (const C& c : gr.knobs) w += cellW(c, g);
  return w;
}
// The group's row on the page, and its chips beside its knobs (else above them).
int rowOf(const G& gr) {
  const std::string id = gr.id;
  return id == "playback" || id == "stretch" || id == "process" ? 0 : id == "env" || id == "filter" ? 1 : id == "lfo" || id == "voice" || id == "channel" ? 2 : 3;
}
bool side(const G& gr) { return std::string(gr.id) != "filter"; }
constexpr float kSideGap = 18;
// A group's width and its height under its title.
float groupW(const G& gr, const MGeo& g) {
  if (gr.chips.empty() || gr.knobs.empty()) return std::max(chipsW(gr, g), knobsW(gr, g));
  return side(gr) ? chipsW(gr, g) + kSideGap + knobsW(gr, g) : std::max(chipsW(gr, g), knobsW(gr, g));
}
float groupH(const G& gr, const MGeo& g) {
  const float ch = chipsH(gr, g), kh = gr.knobs.empty() ? 0.0f : g.knobH();
  if (side(gr)) return std::max(ch, kh);
  return ch + (ch > 0 && kh > 0 ? 10.0f : 0.0f) + kh;
}

void drawChip(Ctx& x, const C& c, ImVec2 at, float w, int id) {
  App& a = x.a;
  const Channel& ch = x.ch;
  ImGui::PushID(id);
  ImGui::SetCursorScreenPos(at);
  float need = 0;
  // A segmented row too narrow for its choices: the same choice as a list.
  auto choice = [&](const char* const* o, int n, int cur, const char* const* hints) {
    int idx = cur;
    if (w + 0.5f >= segW(o, n)) {
      ui::Segmented("##seg", o, n, &idx, std::floor((w - 4) / static_cast<float>(n)), x.g.chipH, hints);
    } else if (ui::BeginCombo("##segc", i18n::tr(o[std::clamp(cur, 0, n - 1)]), w)) {
      for (int k = 0; k < n; ++k)
        if (ImGui::Selectable(i18n::tr(o[k]), k == cur)) idx = k;
      ImGui::EndCombo();
    }
    return idx;
  };
  switch (c.k) {
    case K::Toggle: {
      const bool on = smpPar(ch, c.idx, 0) >= 0.5f;
      if (ui::TextButton(i18n::tr(c.name), on, w, c.hint)) a.doc.setParam(smpPid(ch.id, c.idx), on ? 0.0 : 1.0, false);
      need = labelNeed(i18n::tr(c.name));
      break;
    }
    case K::Seg: {
      const int cur = std::clamp(static_cast<int>(std::lround(smpPar(ch, c.idx, 0))), 0, c.n - 1);
      const int idx = choice(c.opts, c.n, cur, nullptr);
      if (c.hint) ui::Hint(c.hint);
      if (idx != cur) a.doc.setParam(smpPid(ch.id, c.idx), idx, false);
      break;
    }
    case K::Engine: {
      const stretch::Mode cur = stretch::choiceOf(stretch::modeOf(smpPar(ch, pp::SmpStretchMode, 0)));
      const char* hints[3] = {stretch::modeHint(stretch::kChoices[0]), stretch::modeHint(stretch::kChoices[1]), stretch::modeHint(stretch::kChoices[2])};
      const int curI = cur == stretch::kChoices[1] ? 1 : cur == stretch::kChoices[2] ? 2 : 0;
      const int idx = choice(engineNames(), 3, curI, hints);
      if (idx != curI) {
        const int pick = static_cast<int>(stretch::kChoices[idx]);
        if (x.c) {
          const ObjectId cid = x.c->id;
          const double sf = x.c->stretch;
          a.doc.editSong("stretch mode", [&](Song& xs) { setClipStretchMode(xs, cid, sf, pick); });
        } else {
          a.doc.setParam(smpPid(ch.id, pp::SmpStretchMode), pick, false);
        }
      }
      break;
    }
    case K::Fit: {
      float v = static_cast<float>(smpPar(ch, pp::SmpFitTempo, 0));
      ImGui::SetNextItemWidth(w);
      if (ImGui::DragFloat("##fit", &v, 0.5f, 0.0f, 1000.0f, v > 0 ? i18n::tr("follows from %.1f BPM") : i18n::tr("fixed tempo")))
        a.doc.setParam(smpPid(ch.id, pp::SmpFitTempo), std::clamp(static_cast<double>(v), 0.0, 1000.0), true);
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      ui::Hint("Set, the sample follows the tempo: a stretch keeps its beats at any tempo (0: fixed)");
      break;
    }
    case K::Loupe:
      if (ui::TextButton(i18n::tr("Open in Loupe"), false, w, "Opens the sample in Loupe for long sessions: recording, scripts, the region list")) {
        if (x.c) cliprenderui::sendToLoupe(a, x.c->id);
        else if (!ch.plugin.data.empty()) packs::openAudioEditor(a, ch.plugin.data, ch.id);
      }
      need = labelNeed(i18n::tr("Open in Loupe"));
      break;
    case K::Replace:
      if (ui::TextButton(i18n::tr("Replace sample..."), false, w, "Choose another audio file for the sample; or drop a file on the waveform")) replaceSampleDialog(a, ch);
      need = labelNeed(i18n::tr("Replace sample..."));
      break;
    default: break;
  }
  laid(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), need);
  ImGui::PopID();
}
void drawChips(Ctx& x, const G& gr, ImVec2 at, float w) {
  const MGeo& g = x.g;
  for (int l = 0; l < chipLines(gr); ++l) {
    const int k0 = l * gr.chipCols, k1 = std::min(static_cast<int>(gr.chips.size()), k0 + gr.chipCols);
    const float natural = chipLineW(gr, l, g), gaps = g.gap * static_cast<float>(k1 - k0 - 1);
    const float k = (w - gaps) / std::max(1.0f, natural - gaps);
    float cx = at.x;
    for (int i = k0; i < k1; ++i) {
      const float cw = i == k1 - 1 ? at.x + w - cx : std::floor(chipW(gr.chips[static_cast<size_t>(i)]) * k);
      drawChip(x, gr.chips[static_cast<size_t>(i)], ImVec2(cx, at.y + static_cast<float>(l) * (g.chipH + g.gap)), cw, i);
      cx += cw + g.gap;
    }
  }
}

ParamId pidOf(const Ctx& x, const C& c) { return c.k == K::ChanKnob ? channelParam(x.ch.id, static_cast<ChannelParam>(c.idx)) : smpPid(x.ch.id, c.idx); }
std::string valueOf(const Ctx& x, ParamId pid) {
  if (const auto d = describeParam(x.a.doc.song(), pid))
    if (const auto v = getParam(x.a.doc.song(), pid)) return d->format(*v);
  return {};
}
// (dev "clipeditor mainpage") the boxes' spacing as last drawn: a line's boxes, their headings, the
// knob rows' names and the chips' centres against their knobs'.
struct SpaceLine {
  std::vector<float> l, r, top, bot, head, name;
  float y0 = 0, y1 = 0;
};
std::vector<SpaceLine> gSpace;
std::vector<float> gCentreOff;  // a group's chips' centre less its knobs' centre
float gPageL = 0, gPageR = 0;

constexpr float kValueVis = 52;  // the widest value a knob shows here ("20.00 kHz")
// A knob cell `cw` wide at `at`: its name, the knob, its value.
void drawKnob(Ctx& x, const C& c, ImVec2 at, float cw, int id) {
  const MGeo& g = x.g;
  ImFont* f = ImGui::GetFont();
  ImFont* mono = x.a.fontMono ? x.a.fontMono : f;
  const auto& P = pal();
  const float cx = std::round(at.x + cw * 0.5f);
  const ParamId pid = pidOf(x, c);
  const std::string v = valueOf(x, pid);
  centred(x.dl, f, kTextFs, cx, at.y, cw - 4, P.textDim, i18n::tr(c.name));
  ImGui::SetCursorScreenPos(ImVec2(cx - g.r, at.y + g.nameH));
  ImGui::PushID(id);
  if (c.step > 0) ui::SetNextKnobStep(c.step);
  ui::ParamKnob("##k", pid, g.r, nullptr, c.bip);
  ImGui::PopID();
  centred(x.dl, mono, kTextFs, cx, at.y + g.nameH + 2 * g.r + 4, cw - 4, P.text, v.c_str());
  // (laid as what shows: its name, the knob and a value up to the widest the page shows, in its cell)
  const float need = std::max(textW(f, kTextFs, i18n::tr(c.name)), textW(mono, kTextFs, v.c_str())) + 4;
  const float vis = std::min(cw, std::max({need, 2 * g.r, kValueVis}));
  laid(ImVec2(cx - vis * 0.5f, at.y), ImVec2(cx + vis * 0.5f, at.y + g.knobH()), need, 1);
}
// How far a knob cell at a box's edge reaches past what it shows (its name, the knob, a value up to
// the widest the page shows): the box's padding is measured from what shows.
float edgeTrim(const C& c, const MGeo& g) {
  const float name = std::ceil(ImGui::GetFont()->CalcTextSizeA(kTextFs, 1e9f, 0, i18n::tr(c.name)).x);
  return std::floor(std::max(0.0f, (cellW(c, g) - std::max({name, 2 * g.r, kValueVis})) * 0.5f));
}
// What a group's controls take: its chips, the gap, its knobs, less its edge knobs' slack.
float contentW(const G& gr, const MGeo& g) {
  float w = groupW(gr, g);
  if (!gr.knobs.empty()) {
    w -= edgeTrim(gr.knobs.back(), g);
    if (gr.chips.empty() || !side(gr)) w -= edgeTrim(gr.knobs.front(), g);
  }
  return w;
}
// A group `w` wide at `at` (under its heading), its knob row ending at `bottom` so a line's knobs
// share one row: its chips beside its knobs (their middle level with the knobs'), or above them.
void drawGroup(Ctx& x, const G& gr, ImVec2 at, float w, float bottom) {
  const MGeo& g = x.g;
  for (const char* k : gr.keys) reach(k);
  const float ky = std::round(bottom - (gr.knobs.empty() ? 0.0f : g.knobH()));
  const float knobMid = ky + g.nameH + g.r;
  float kx = at.x;
  if (!gr.knobs.empty() && (gr.chips.empty() || !side(gr))) kx -= edgeTrim(gr.knobs.front(), g);
  // (the volume envelope: its shape in the room a line's taller groups leave above its knobs)
  if (std::string(gr.id) == "env" && ky - at.y - 10 >= 20) {
    const Channel& ch = x.ch;
    const double ts[6] = {smpPar(ch, pp::SmpEnvDelay, 0), smpPar(ch, pp::SmpAttack, 0.0005f), smpPar(ch, pp::SmpEnvHold, 0), smpPar(ch, pp::SmpDecay, 10), 0.5,
                          smpPar(ch, pp::SmpRelease, 0.02f)};
    const ImVec2 e0(at.x, at.y), e1(at.x + w, ky - 10);
    envGraph(e0, e1, ts, 6, smpPar(ch, pp::SmpSustain, 1), sampleOrange());
    laid(e0, e1, 0, 2);
  }
  if (!gr.chips.empty()) {
    const float cw = gr.knobs.empty() || !side(gr) ? chipsW(gr, g) : chipsW(gr, g);
    const float ch = chipsH(gr, g);
    const float cy = side(gr) && !gr.knobs.empty() ? std::round(knobMid - ch * 0.5f) : at.y;
    drawChips(x, gr, ImVec2(at.x, cy), cw);
    if (side(gr) && !gr.knobs.empty()) {
      kx = at.x + cw + kSideGap;
      gCentreOff.push_back(cy + ch * 0.5f - knobMid);
    }
  }
  if (!gr.knobs.empty() && !gSpace.empty()) gSpace.back().name.push_back(ky);
  for (size_t k = 0; k < gr.knobs.size(); ++k) {
    const float cw = cellW(gr.knobs[k], g);
    drawKnob(x, gr.knobs[k], ImVec2(std::round(kx), ky), cw, 100 + static_cast<int>(k));
    kx += cw;
  }
}
void beginGroup(const G& gr, ImVec2 a, ImVec2 b, std::vector<std::pair<std::string, std::pair<ImVec2, ImVec2>>>& rects) {
  gLaid[gLaidHost].emplace_back();
  gLaid[gLaidHost].back().id = gr.id, gLaid[gLaidHost].back().a = a, gLaid[gLaidHost].back().b = b;
  rects.push_back({gr.id, {a, b}});
}

// ---- the boxes: four rows in the old window's order (Sample; Envelope and filter; LFO and the voice;
// Sample FX), a group's chips beside its knobs, every knob of a row on one line; when the window is
// too narrow for that, the groups in that order, as many to a line as fit. A line fills the page's
// width: its knob cells widen alike (up to half again), then its boxes alike, their contents at their
// left. Every box: the same padding on its four sides, its heading where the others' are.
constexpr float kGap = 12, kPadX = 14, kPadTop = 12, kPadBottom = 10, kHeadH = 30, kLineGap = 14;
float boxW(const G& gr, const MGeo& g) { return contentW(gr, g) + 2 * kPadX; }
float lineUsed(const std::vector<G>& gs, const std::vector<size_t>& ln, const MGeo& g) {
  float w = 0;
  for (size_t i : ln) w += boxW(gs[i], g);
  return w + kGap * static_cast<float>(ln.size() > 0 ? ln.size() - 1 : 0);
}
// The line's knob cells widened alike to fill `w` (half again at most).
MGeo lineGeo(const std::vector<G>& gs, const std::vector<size_t>& ln, const MGeo& g, float w) {
  MGeo t = g;
  for (int k = 1; k <= static_cast<int>(g.cw * 0.5f); ++k) {
    MGeo u = g;
    u.cw = g.cw + static_cast<float>(k);
    if (lineUsed(gs, ln, u) > w + 0.5f) break;
    t = u;
  }
  return t;
}
std::vector<std::vector<size_t>> lines(const std::vector<G>& gs, const MGeo& g, float w, bool& wrapped) {
  std::vector<std::vector<size_t>> out;
  wrapped = false;
  for (int r = 0; r < 4; ++r) {
    out.emplace_back();
    float x = 0;
    for (size_t i = 0; i < gs.size(); ++i) {
      if (rowOf(gs[i]) != r) continue;
      const float gw = boxW(gs[i], g);
      if (!out.back().empty() && x + kGap + gw > w + 0.5f) out.emplace_back(), x = 0, wrapped = true;
      x += (out.back().empty() ? 0 : kGap) + gw;
      out.back().push_back(i);
    }
  }
  if (!wrapped) return out;
  out.assign(1, {});
  float x = 0;
  for (size_t i = 0; i < gs.size(); ++i) {
    const float gw = boxW(gs[i], g);
    if (!out.back().empty() && x + kGap + gw > w + 0.5f) out.emplace_back(), x = 0;
    x += (out.back().empty() ? 0 : kGap) + gw;
    out.back().push_back(i);
  }
  return out;
}
float lineH(const std::vector<G>& gs, const std::vector<size_t>& ln, const MGeo& g) {
  float h = 0;
  for (size_t i : ln) h = std::max(h, groupH(gs[i], g));
  return kHeadH + h + kPadBottom;
}
float layout(const std::vector<G>& gs, const MGeo& g, float w, bool* wrappedOut = nullptr) {
  bool wrapped = false;
  const std::vector<std::vector<size_t>> ls = lines(gs, g, w, wrapped);
  if (wrappedOut) *wrappedOut = wrapped;
  float h = 0;
  for (size_t l = 0; l < ls.size(); ++l) h += (l > 0 ? kLineGap : 0.0f) + lineH(gs, ls[l], g);
  return h;
}
// `spare`: the room the page leaves, shared out between the lines.
void drawBoxes(Ctx& x, const std::vector<G>& gs, ImVec2 o, float w, float spare, std::vector<std::pair<std::string, std::pair<ImVec2, ImVec2>>>& rects) {
  const auto& P = pal();
  const MGeo g0 = x.g;
  bool wrapped = false;
  const std::vector<std::vector<size_t>> ls = lines(gs, g0, w, wrapped);
  const float extra = ls.size() > 1 ? std::floor(std::min(20.0f, spare / static_cast<float>(ls.size() - 1))) : 0.0f;
  gSpace.clear();
  gCentreOff.clear();
  gPageL = o.x, gPageR = o.x + w;
  float y = std::round(o.y);
  for (size_t l = 0; l < ls.size(); ++l) {
    const std::vector<size_t>& ln = ls[l];
    if (l > 0) y += kLineGap + extra;
    x.g = lineGeo(gs, ln, g0, w);
    const MGeo& g = x.g;
    const float lh = std::round(lineH(gs, ln, g));
    // (what the widened cells leave: the boxes widen alike, the last one to the page's right edge)
    const float widen = std::floor(std::max(0.0f, (w - lineUsed(gs, ln, g)) / static_cast<float>(ln.size())));
    gSpace.emplace_back();
    gSpace.back().y0 = y, gSpace.back().y1 = y + lh;
    float gx = std::round(o.x);
    for (size_t k = 0; k < ln.size(); ++k) {
      const G& gr = gs[ln[k]];
      const float gw = k + 1 == ln.size() ? std::round(o.x + w) - gx : boxW(gr, g) + widen;
      const ImVec2 a(gx, y), b(gx + gw, y + lh);
      x.dl->AddRectFilled(a, b, P.dark ? theme::mix(P.surface900, P.surface950, 0.55f) : theme::mix(P.surface900, P.surface800, 0.45f), 6);
      x.dl->AddRect(a, b, P.line, 6);
      std::string t = i18n::tr(gr.title);
      std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
      label(x.dl, boldFont(x), theme::kFontLabel, ImVec2(a.x + kPadX, a.y + kPadTop), P.textDim, t.c_str());
      SpaceLine& sl = gSpace.back();
      sl.l.push_back(a.x), sl.r.push_back(b.x), sl.top.push_back(a.y), sl.bot.push_back(b.y), sl.head.push_back(a.y + kPadTop);
      beginGroup(gr, a, b, rects);
      ImGui::PushID(gr.id);  // each box its own ID scope: chips and knobs are numbered per box
      drawGroup(x, gr, ImVec2(a.x + kPadX, a.y + kHeadH), contentW(gr, g), b.y - kPadBottom);
      ImGui::PopID();
      gx += gw + kGap;
    }
    y += lh;
  }
  x.g = g0;
}
// The spacing as last drawn, each the most it strays (px): the gaps between boxes, a line's tops,
// bottoms, headings and knob names, the gaps between lines, the chips' middles from their knobs',
// the lines' outer edges from the page's.
std::string spacingReport() {
  auto spread = [](const std::vector<float>& v) {
    if (v.size() < 2) return 0.0f;
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    return *hi - *lo;
  };
  std::vector<float> gaps, lineGaps;
  float tops = 0, bots = 0, heads = 0, names = 0, edges = 0, centres = 0;
  for (size_t l = 0; l < gSpace.size(); ++l) {
    const SpaceLine& s = gSpace[l];
    for (size_t k = 1; k < s.l.size(); ++k) gaps.push_back(s.l[k] - s.r[k - 1]);
    tops = std::max(tops, spread(s.top)), bots = std::max(bots, spread(s.bot)), heads = std::max(heads, spread(s.head)), names = std::max(names, spread(s.name));
    if (!s.l.empty()) edges = std::max({edges, std::fabs(s.l.front() - gPageL), std::fabs(s.r.back() - gPageR)});
    if (l > 0) lineGaps.push_back(s.y0 - gSpace[l - 1].y1);
  }
  for (float c : gCentreOff) centres = std::max(centres, std::fabs(c));
  char b[200];
  std::snprintf(b, sizeof b, "gaps %.0f tops %.0f bottoms %.0f headings %.0f names %.0f linegaps %.0f centres %.0f edges %.0f", static_cast<double>(spread(gaps)),
                static_cast<double>(tops), static_cast<double>(bots), static_cast<double>(heads), static_cast<double>(names), static_cast<double>(spread(lineGaps)),
                static_cast<double>(centres), static_cast<double>(edges));
  return b;
}

}  // namespace mainv

void mainPage(App& a, State& S, Host host, const Clip* cp, const Channel& chan, ImVec2 p0, ImVec2 size) {
  const auto& P = pal();
  const bool compactHead = size.y < 430;
  const float headH = compactHead ? 36.0f : 44.0f;
  header(a, cp, chan, p0, size.x, headH, host);
  // (fresh copies: an edit in the header may have moved the song's clips)
  const Song& s = a.doc.song();
  const Clip* cnow = cp ? s.clip(cp->id) : nullptr;
  const Channel* chnow = s.channel(chan.id);
  if ((cp && !cnow) || !chnow) return;
  const std::optional<Clip> c = cnow ? std::optional<Clip>(*cnow) : std::nullopt;
  const Channel ch = *chnow;
  const ImVec2 b0(p0.x, p0.y + headH);
  const ImVec2 bs(size.x, size.y - headH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(b0, ImVec2(b0.x + bs.x, b0.y + bs.y), P.surface900);
  // (the clip's view fitted here as the waveform would, so a selection made meanwhile survives Advanced)
  if (c && (!S.fitted || S.fitClip != c->id)) {
    static TempoMap tm;
    tm.build(s);
    S.fitClip = c->id;
    S.selT0 = S.selT1 = 0;
    fitView(s, *c, S, tm);
  }
  using namespace mainv;
  const std::vector<G>& gs = groups();
  // (the strip, its line and the boxes: the boxes' gap between each, the page's edges the boxes' edges)
  const float padX = 20.0f, padY = 12.0f, lineRowH = std::round(ImGui::GetFrameHeight()) + kGap, gapY = kGap;
  const float pageW = bs.x - 2 * padX;
  const float stripMax = 120, stripMin = 44;
  const float room = bs.y - 2 * padY - stripMin - lineRowH - gapY;
  // The largest knobs (and cells) whose page fits without wrapping a page's groups, else the
  // largest that fit at all, else the smallest (the page scrolls).
  std::vector<MGeo> tries;
  for (auto [r, cw] : {std::pair<float, float>{14, 64}, {13, 62}, {13, 60}, {13, 58}, {12, 56}, {12, 54}, {11, 52}, {10, 50}}) {
    MGeo g;
    g.r = r, g.cw = cw;
    g.chipH = std::round(ImGui::GetFrameHeight());  // (as tall as the buttons and lists beside them)
    tries.push_back(g);
  }
  auto need = [&](const MGeo& g, bool* wrapped) { return layout(gs, g, pageW, wrapped); };
  MGeo g = tries.front();  // (none fits: the largest, and the page scrolls)
  bool scrolls = true, found = false;
  for (int pass = 0; pass < 2 && !found; ++pass)
    for (const MGeo& t : tries) {
      bool wrapped = false;
      if (need(t, &wrapped) <= room + 0.5f && (pass == 1 || !wrapped)) {
        g = t, scrolls = false, found = true;
        break;
      }
    }
  const float pageH = need(g, nullptr);
  const float stripH = scrolls ? stripMin : std::floor(std::clamp(room - pageH + stripMin, stripMin, stripMax));
  // The whole sample: its start and end handles, the notes' loop; drop a file to replace it.
  reach("overview");
  const ImVec2 s0(b0.x + padX, b0.y + padY), s1(b0.x + bs.x - padX, b0.y + padY + stripH);
  S.waveA = s0, S.waveB = s1, S.ovA = s0, S.ovB = s1;
  sampleStrip(a, S, ch, s0, s1, true, ViewOnFile{});
  dl->AddRect(s0, s1, theme::withAlpha(P.ink, 0.35f));
  // The sample's line: its file, start and end, and Replace sample... (the old window's Load sample...).
  Ctx x{a, c ? &*c : nullptr, ch, g, dl};
  gLaidHost = static_cast<int>(host);
  gLaid[gLaidHost].clear();
  {
    reach("file");
    reach("startend");
    const ClipSource src = clipSourceIfReady(ch);
    const double secs = src.sample ? src.sample->seconds() : 0.0;
    double sf = 0, ef = 1;
    channelTrim(ch, sf, ef);
    const float ry = s1.y + kGap, rh = std::round(ImGui::GetFrameHeight());
    const std::string path = samplePath(ch);
    std::string line = path.empty() ? std::string(i18n::tr("No sample")) : fs::path(path).filename().string();
    if (src.sample) {
      char b[160];
      std::snprintf(b, sizeof b, "    %s %.3f s    %s %.3f s    %.1f kHz %s", i18n::tr("start"), sf * secs, i18n::tr("end"), ef * secs,
                    src.sample->sampleRate / 1000.0, src.sample->mono() ? i18n::tr("mono") : i18n::tr("stereo"));
      line += b;
    }
    ImFont* mono = a.fontMono ? a.fontMono : ImGui::GetFont();
    label(dl, mono, kTextFs, ImVec2(s0.x, std::round(ry + (rh - mono->CalcTextSizeA(kTextFs, 1e9f, 0, "0").y) * 0.5f)), P.textDim, line.c_str());
    const float rw = chipW(C{K::Replace});
    gLaid[gLaidHost].emplace_back();
    gLaid[gLaidHost].back().id = "sampleline", gLaid[gLaidHost].back().a = ImVec2(s0.x, ry), gLaid[gLaidHost].back().b = ImVec2(s1.x, ry + rh);
    drawChip(x, C{K::Replace}, ImVec2(s1.x - rw, ry), rw, 900);
  }
  const float top = s1.y + lineRowH + gapY;
  const float cardsH = b0.y + bs.y - padY - top;
  ImGui::SetCursorScreenPos(ImVec2(b0.x + padX, top));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 8.0f);
  ImGui::BeginChild("##cemain", ImVec2(pageW + (scrolls ? 12.0f : 0.0f), std::max(1.0f, cardsH)), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | (scrolls ? 0 : ImGuiWindowFlags_NoScrollbar));
  ImGui::PopStyleVar(2);
  x.dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  S.cardRects.clear();
  drawBoxes(x, gs, o, pageW, 0.0f, S.cardRects);
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(pageW, pageH));
  const ImGuiIO& io = ImGui::GetIO();
  if (scrolls && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0 && !ui::wheelTaken() && !io.KeyCtrl)
    ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() - io.MouseWheel * 48.0f, 0.0f, ImGui::GetScrollMaxY()));
  ImGui::EndChild();
  S.panelA = ImVec2(b0.x + padX - 0.5f, s1.y), S.panelB = ImVec2(b0.x + bs.x - padX + 0.5f, top + cardsH);
  S.tabsA = b0, S.tabsB = ImVec2(b0.x + bs.x, b0.y + bs.y);
  S.panelScrolls = scrolls;
  S.mainCell = g.r, S.mainCardH = g.cw;
}

void drawTarget(App& a, State& S, Host host, const Clip* cp, const Channel& chan, ImVec2 p0, ImVec2 size) {
  const Song& s = a.doc.song();
  const bool hasClip = cp != nullptr;
  S.tab = clipwin::shownTab(S.tab, hasClip);
  // The Clips layout's bands and Smooth's audio editor: the layout's own view (nothing over the views above).
  if (bandHost(host)) {
    nativeBand(a, S, host, cp, chan, p0, size);
    return;
  }
  if (host == Host::Pane && smooth()) {
    smoothPane(a, S, cp, chan, p0, size);
    return;
  }
  S.chipsMode = false;
  S.mainDrawn = host == Host::Window && mainView(a);
  if (S.mainDrawn) {  // 0.7.8: the Main view (the tabs are the Advanced view's)
    mainPage(a, S, host, cp, chan, p0, size);
    return;
  }
  const bool compactHead = size.y < 430;
  const float headH = compactHead ? 36.0f : 44.0f, toolH = 34.0f, ovH = compactHead ? 22.0f : 32.0f;
  header(a, cp, chan, p0, size.x, headH, host);
  // The tabs under the waveform, the shown tab's modules under them: full modules, or compact ones
  // (their titles and knobs) in a short pane or band until the divider gives them the room. The
  // divider's place is remembered. The panel is as tall as the tallest tab needs, so the tab bar
  // stays put from tab to tab.
  S.tab = moduleTab(S.tab, hasClip);
  const float tabH = 30.0f, panelPad = 8.0f;
  float fullPanel = 0, compactPanel = 0;
  bool fullOk = true;
  {  // (every tab's rows, full and compact, for the panel's height)
    std::vector<Mod> ms = modulesOf(a, cp, chan);
    Mod out = outputModule(a, cp, chan, host);
    for (int t = 0; t < TabEdit; ++t) {
      if (!clipwin::tabShown(t, hasClip)) continue;
      std::vector<Mod*> on;
      for (Mod& m : ms)
        if (onTab(m, t, hasClip)) on.push_back(&m);
      if (on.empty()) continue;
      const Lay lf = layTab(on, out, size.x, false), lc = layTab(on, out, size.x, true);
      fullOk = fullOk && lf.ok;
      fullPanel = std::max(fullPanel, tabH + panelPad + lf.h + panelPad);
      compactPanel = std::max(compactPanel, tabH + panelPad + lc.h + panelPad);
    }
  }
  const float top = headH + toolH;
  const float least = tabH + panelPad + kCompactModH + panelPad;
  const float most = std::max(least, size.y - top - 40.0f);
  const float kept = static_cast<float>(a.settings.getNumber("ui.clipWindowPanel"));
  if (S.panelH <= 0 && kept > 0.05f && kept < 0.95f) S.panelH = std::round(kept * size.y);
  bool compact;
  if (S.panelH > 0) compact = S.panelH < (fullPanel + compactPanel) * 0.5f;
  else compact = size.y - top - fullPanel < 180;  // (a short pane or band: compact, so the waveform keeps its room)
  if (fullPanel > most || !fullOk) compact = true;
  S.compactDev = compact;
  // (taller than the room: the waveform keeps 120 px, the modules scroll up and down, until the
  // divider is dragged)
  const float panelH = std::min(compact ? compactPanel : fullPanel, S.panelH > 0 ? most : std::max(least, size.y - top - 120.0f));
  toolStrip(a, S, cp, chan, ImVec2(p0.x, p0.y + headH), size.x, toolH);
  const float waveTop = p0.y + top;
  const float waveBottom = std::max(waveTop, p0.y + size.y - panelH);
  if (waveBottom - waveTop > 40) {
    if (hasClip) {
      waveArea(a, S, host, *cp, chan, ImVec2(p0.x, waveTop), ImVec2(p0.x + size.x, waveBottom), ovH);
    } else {  // no clip: the whole sample, big, with its handles and loop
      reach("overview");
      reach("wave");
      S.waveA = ImVec2(p0.x, waveTop), S.waveB = ImVec2(p0.x + size.x, waveBottom);
      S.ovA = S.waveA, S.ovB = S.waveB;
      sampleStrip(a, S, chan, ImVec2(p0.x, waveTop), ImVec2(p0.x + size.x, waveBottom), true, ViewOnFile{});
    }
  }
  // The divider: drag it to give the waveform or the strip the room (the strip full or compact;
  // double-click: as the pane has it).
  {
    const ImVec2 d0(p0.x, waveBottom - 3);
    ImGui::SetCursorScreenPos(d0);
    ImGui::InvisibleButton("##cedivider", ImVec2(size.x, 6));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
      S.panelH = std::clamp(p0.y + size.y - ImGui::GetIO().MousePos.y, std::min(compactPanel, most), most);
    if (ImGui::IsItemDeactivated() && S.panelH > 0) a.settings.set("ui.clipWindowPanel", static_cast<double>(S.panelH / std::max(1.0f, size.y)));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      S.panelH = 0;
      a.settings.set("ui.clipWindowPanel", 0.0);
    }
    ui::Hint("Drag to give the waveform or the settings more room (double-click: as they were)");
    {  // its grip, in the middle
      const float gx = std::round(p0.x + size.x * 0.5f), gy = std::round(waveBottom);
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(gx - 16, gy - 1), ImVec2(gx + 16, gy + 1),
                                                ImGui::IsItemHovered() || ImGui::IsItemActive() ? pal().hot : pal().lineStrong, 1.0f);
    }
  }
  // The tab bar and the tab's modules (with fresh pointers: an edit above may have moved the song's clips).
  const Song& s2 = a.doc.song();
  const Clip* cnow = hasClip ? s2.clip(cp->id) : nullptr;
  const Channel* chnow = s2.channel(chan.id);
  if ((hasClip && !cnow) || !chnow) return;
  const ImVec2 t0(p0.x, waveBottom);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(t0, ImVec2(p0.x + size.x, p0.y + size.y), pal().surface800);
  dl->AddLine(ImVec2(t0.x, t0.y + 0.5f), ImVec2(t0.x + size.x, t0.y + 0.5f), pal().line);
  S.tabsA = t0, S.tabsB = ImVec2(p0.x + size.x, p0.y + size.y);
  tabBar(a, S, cnow, *chnow, t0, size.x, tabH);
  S.tab = moduleTab(S.tab, hasClip);  // (a click on a tab)
  dl->AddLine(ImVec2(t0.x, t0.y + tabH - 0.5f), ImVec2(t0.x + size.x, t0.y + tabH - 0.5f), theme::withAlpha(pal().text, 0.08f));
  std::vector<Mod> mods = modulesOf(a, cnow, *chnow);
  Mod out = outputModule(a, cnow, *chnow, host);
  std::vector<Mod*> on;
  for (Mod& m : mods)
    if (onTab(m, S.tab, hasClip)) on.push_back(&m);
  const Lay L = layTab(on, out, size.x, compact);
  gLaidHost = static_cast<int>(host);
  gLaid[gLaidHost].clear();
  const float modTop = t0.y + tabH + panelPad;
  if (L.ok) tabModules(a, S, on, out, L, ImVec2(p0.x, modTop), size.x, std::max(1.0f, p0.y + size.y - panelPad - modTop), compact);
  (void)s;
}

}  // namespace

void drawBody(App& a, ObjectId clipId, ImVec2 size, Host host) { drawTargetBody(a, clipId, kNoId, size, host); }

void drawTargetBody(App& a, ObjectId clipId, ObjectId channelId, ImVec2 size, Host host) {
  State& S = st(host);
  gDrawFrame[static_cast<int>(host)] = ImGui::GetFrameCount();
  const Song& s = a.doc.song();
  const Clip* cp = clipId != kNoId ? s.clip(clipId) : nullptr;
  if (cp && cp->kind != ClipKind::Audio) cp = nullptr;
  const Channel* ch = cp ? channelOf(s, *cp) : channelId != kNoId ? s.channel(channelId) : nullptr;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  if (!ch) {
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + size.x, p0.y + std::max(size.y, 60.0f)), i18n::tr("No audio clip"),
                   i18n::tr("Double-click an audio clip to edit it here"));
    ImGui::Dummy(ImVec2(size.x, std::max(size.y, 60.0f)));
    return;
  }
  const std::optional<Clip> c = cp ? std::optional<Clip>(*cp) : std::nullopt;
  const Channel chan = *ch;
  ImGui::PushID("clipeditor");
  ImGui::PushID(static_cast<int>(host));
  S.clip = c ? c->id : kNoId;
  S.channel = chan.id;
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsWindowAppearing()) {
    a.focusPluginOwner = chan.id;  // controller template knobs follow the shown sampler
    a.focusPluginSlot = -1;
  }
  // (dev "layout bodies" and the K-112 inventory: a sampler channel's settings are drawn here)
  hostbody::drawn(hostbody::Body::ClipEditor, chan.id, -1);
  drawTarget(a, S, host, c ? &*c : nullptr, chan, p0, size);
  drawOpDialog(a);
  // W: warp mode while the editor is under the pointer (Alt+W in Clips, Smooth and Tracker).
  if (c && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
    if (keys::pressed("wave.warpMode")) S.tool = S.tool == TWarp ? TPointer : TWarp;
    if (keys::pressed("wave.spectrum")) S.spectrum = !S.spectrum;
  }
  // The Tracker's Audio page: Alt+P plays the clip (again: stops), as the Sample editor's Alt+P
  // plays its sample (core/TrackerKeys.h, "audio.play").
  if (c && host == Host::Pane && trackerpages::active(a)) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_P, false) && io.KeyAlt && !io.KeyCtrl && !io.KeyShift && !io.KeySuper && !io.WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
      previewClip(a, c->id, S.loopPreview);
      gDevLastOp = a.engine.previewing() ? "alt+p play" : "alt+p stop";
    }
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(size);
  ImGui::PopID();
  ImGui::PopID();
}

namespace {

// Shows `t` in the current layout's place for it, on `tab` (-1: the tab it was on).
void show(App& a, Target t, int tab, bool band) {
  if (band) {  // Clips: the clip band shows the selected clip
    layoutui::setRegionVisible(a, "live.clip", true);
    if (bool* f = layoutui::windowOpenFlag(a, "Piano roll")) *f = true;
    if (tab >= 0) st(Host::Clips).tab = tab, st(Host::Clips).scrollTab = tab;
    return;
  }
  if (smooth()) {  // the lower pane's audio editor; the clip's own settings in the inspector's Region card
    layoutui::setRegionVisible(a, "logic.lower", true);
    a.showPiano = a.showRack = a.showMixer = false;
    a.settings.set("ui.loupePinned", true);
    gPane = t, gPaneOn = true;
    if (tab >= 0) st(Host::Pane).tab = tab;
    if (t.clip != kNoId) layoutparts::openRegionCard();
    // (the tab asked for: its sections open in the local inspector)
    if (tab >= 0 && clipwin::scopeOf(tab) != clipwin::Scope::Clip) {
      const Song& s = a.doc.song();
      if (const Channel* ch = s.channel(t.channel)) {
        const Clip* c = t.clip != kNoId ? s.clip(t.clip) : nullptr;
        for (const Mod& m : modulesOf(a, c, *ch))
          if (onTab(m, tab, c != nullptr)) gRowsFolded.erase(m.id);
      }
    }
    return;
  }
  if (tracker()) {  // 0.7.8: the Sample editor page (the lower band), the window's parts in the Tracker's look
    gPane = t, gPaneOn = true;
    if (tab >= 0) st(Host::Pane).tab = tab;
    trackerpage::show(a, t.channel);
    return;
  }
  gWindow = t;
  if (tab >= 0) st(Host::Window).tab = tab;
  a.focusWindow = "Audio clip";
}

}  // namespace

void open(App& a, ObjectId clip, int tab) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c || c->kind != ClipKind::Audio) return;
  if (!a.playlist.selected.count(clip)) a.playlist.selected = {clip};
  show(a, Target{clip, c->source}, tab, clipsLayout());
}

namespace {

// The clip to show with a sampler channel: `first` when it is one of the channel's, else a selected
// one, else `then` when it is one, else its earliest; kNoId: the channel alone (the clip's tabs hidden).
ObjectId clipOfChannel(const App& a, ObjectId channel, ObjectId first, ObjectId then) {
  const Song& s = a.doc.song();
  auto ofIt = [&](ObjectId id) {
    const Clip* k = id != kNoId ? s.clip(id) : nullptr;
    return k && k->kind == ClipKind::Audio && k->source == channel;
  };
  if (ofIt(first)) return first;
  for (ObjectId id : a.playlist.selected)
    if (ofIt(id)) return id;
  if (ofIt(then)) return then;
  ObjectId clip = kNoId;
  Tick best = 0;
  for (const Clip& k : s.playlist)
    if (k.kind == ClipKind::Audio && k.source == channel && (clip == kNoId || k.start < best)) clip = k.id, best = k.start;
  return clip;
}

}  // namespace

bool openChannel(App& a, ObjectId channel, int tab) {
  if (!takesChannel(a, channel)) return false;
  // The clip to show with it: the one shown already, else a selected one, else its first; none:
  // the channel alone (the clip's tabs hidden).
  const Target& cur = smooth() ? gPane : gWindow;
  const ObjectId clip = clipOfChannel(a, channel, cur.clip, kNoId);
  // Clips: its band homes a channel's settings, so a sampler's settings asked for as a window (a
  // device's) open as a window. K-391: the Tracker too: a Sampler's settings float in this window,
  // as in Studio (its Sample button and a clip's double-click open the Sample editor page instead).
  if (clipsLayout() || tracker()) {
    gWindow = Target{clip, channel};
    if (tab >= 0) st(Host::Window).tab = tab;
    a.focusWindow = "Audio clip";
    return true;
  }
  show(a, Target{clip, channel}, tab, false);
  return true;
}

void drawDeviceBand(App& a, ObjectId channel, ImVec2 size) {
  // (the clip follows the playlist's selection; else the one it showed, else the channel's first)
  const State& S = st(Host::Device);
  const ObjectId clip = clipOfChannel(a, channel, kNoId, S.channel == channel ? S.clip : kNoId);
  drawTargetBody(a, clip, channel, size, Host::Device);
}

void deviceBandTab(int tab) {
  if (tab >= 0 && tab < TabCount) st(Host::Device).tab = tab, st(Host::Device).scrollTab = tab;
}

void openFromDoubleClick(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c || c->kind != ClipKind::Audio) return;
  const std::string how = a.settings.getString("ui.clipDoubleClick");
  if (how == "loupe") {
    cliprenderui::sendToLoupe(a, clip);
    return;
  }
  // ("channel": the same window on the sample's Playback tab, the old channel settings' page)
  open(a, clip, clipwin::entryTab(how == "channel" ? clipwin::Entry::DoubleClickSetting : clipwin::Entry::DoubleClick));
}

void drawWindow(App& a) {
  if (gWindow.clip == kNoId && gWindow.channel == kNoId) return;
  // 0.7.8: a window left open in another layout waits while the Tracker is in front. K-391: one the
  // Tracker floated for a Sampler's settings shows while they are open (not held by the band).
  if (tracker() && !(gWindow.channel != kNoId && gWindow.channel == a.settingsChannel && trackereditors::pluginWindowOpen(a))) return;
  const Song& s = a.doc.song();
  const Clip* c = gWindow.clip != kNoId ? s.clip(gWindow.clip) : nullptr;
  if (gWindow.clip != kNoId && (!c || c->kind != ClipKind::Audio)) {  // the clip went: its channel stays when asked for
    gWindow.clip = kNoId;
    c = nullptr;
  }
  if (!c && !takesChannel(a, gWindow.channel)) {
    gWindow = {};
    return;
  }
  if (c && gPaneOn && gPane.clip == c->id && hostbody::claimed(hostbody::Body::ClipEditor)) return;  // (the pane shows it)
  bool open = true;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(ImVec2(std::min(1340.0f, vp->WorkSize.x - 60), std::min(760.0f, vp->WorkSize.y - 60)), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (gDevWinSize.x > 0) ImGui::SetNextWindowSize(std::exchange(gDevWinSize, ImVec2(0, 0)), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(720.0f, vp->WorkSize.x - 20), std::min(420.0f, vp->WorkSize.y - 20)), ImVec2(1e6f, 1e6f));
  const std::string title = std::string(i18n::tr("Audio clip")) + "###Audio clip";
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const bool vis = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (vis) drawTargetBody(a, c ? c->id : kNoId, gWindow.channel, ImGui::GetContentRegionAvail(), Host::Window);
  ImGui::End();
  if (!open) gWindow = {};
}

bool drawPane(App& a, ImVec2 avail) {
  if (!gPaneOn) return false;
  if (tracker()) return false;  // 0.7.8: the Tracker's Audio page is Loupe's; the window is its Sample editor page
  const Song& s = a.doc.song();
  const Clip* c = gPane.clip != kNoId ? s.clip(gPane.clip) : nullptr;
  if (gPane.clip != kNoId && (!c || c->kind != ClipKind::Audio)) gPane.clip = kNoId, c = nullptr;
  if (!c && !takesChannel(a, gPane.channel)) {
    gPaneOn = false;
    return false;
  }
  hostbody::claim(hostbody::Body::ClipEditor, "Audio pane");
  drawTargetBody(a, c ? c->id : kNoId, gPane.channel, avail, Host::Pane);
  return true;
}

void drawInspectorRegion(App& a, ObjectId clipId) {
  const Song& s = a.doc.song();
  const Clip* cp = s.clip(clipId);
  const Channel* chp = cp ? channelOf(s, *cp) : nullptr;
  if (!cp || !chp) return;
  // (copies: an edit below may move the song's clips and channels)
  const Clip c = *cp;
  const Channel ch = *chp;
  const ImVec2 wa = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  Measure meas(4, wa, ImVec2(wa.x + ws.x, wa.y + ws.y), "inspector");
  gLaidHost = 4;
  gLaid[4].clear();
  ImGui::PushID("ceinsp");
  // (the card's own rows have Mute and Loop: a region loops on to the next one here)
  static const std::vector<std::string> omit = {"Mute", "Loop"};
  const float w = ImGui::GetContentRegionAvail().x - 8;
  std::vector<Mod> mods;
  mods.push_back(clipModule(a, c, ch));
  mods.push_back(fadesModule(a, c));
  mods.push_back(warpModule(a, c, ch));
  for (Mod& m : mods) {
    ImGui::Dummy(ImVec2(0, 2));
    rowsHeading(a, m, false, true);
    rowsBody(a, m, w, false, &omit);
  }
  ImGui::PopID();
  meas.end(static_cast<int>(mods.size()));
  gInspFrame = ImGui::GetFrameCount();
}

bool inspectorShown() { return gInspFrame >= ImGui::GetFrameCount() - 1; }

// ---- 0.7.8: the Tracker's Sample editor page (TrackerEditors.cpp draws the frame) ----

namespace trackerpage {
namespace {
struct Page {
  std::optional<Clip> clip;      // the frame's copies (the modules' bodies hold references to them)
  std::optional<Channel> chan;
  std::vector<Mod> mods;         // the modules, Output last
  bool on = false;
  int prevStyle = -1;
};
Page gPage;
}  // namespace

void show(App& a, ObjectId channel) {
  if (channel == kNoId) return;
  trackereditors::open(a, trackereditors::Editor::Sample, channel, -1, true);  // (with the keys: Esc closes it, note keys play)
}

bool showing(App& a) { return trackereditors::showsClipForm(a); }

bool reopen(App& a) {
  const Song& s = a.doc.song();
  if (takesChannel(a, a.selChannel)) {  // the selected instrument is a Sampler: its page
    show(a, a.selChannel);
    return true;
  }
  if (!gPaneOn) return false;
  const Clip* c = gPane.clip != kNoId ? s.clip(gPane.clip) : nullptr;
  const ObjectId ch = gPane.channel != kNoId ? gPane.channel : c && c->kind == ClipKind::Audio ? c->source : kNoId;
  if (!takesChannel(a, ch)) return false;
  show(a, ch);
  return true;
}

bool take(App& a, ObjectId channel) {
  if (!takesChannel(a, channel)) return false;
  const Song& s = a.doc.song();
  auto ofIt = [&](const Target& t) {
    if (t.channel == channel) return true;
    const Clip* c = t.clip != kNoId ? s.clip(t.clip) : nullptr;
    return c && c->kind == ClipKind::Audio && c->source == channel && t.channel == kNoId;
  };
  if (!(gPaneOn && ofIt(gPane))) {
    gPane = Target{clipOfChannel(a, channel, kNoId, kNoId), channel};
    gPaneOn = true;
  }
  if (gPane.clip != kNoId) {  // the clip went: the channel alone
    const Clip* c = s.clip(gPane.clip);
    if (!c || c->kind != ClipKind::Audio || c->source != channel) gPane.clip = kNoId;
  }
  gPane.channel = channel;
  return true;
}

Info info(App& a) {
  Info r;
  const Song& s = a.doc.song();
  const Clip* c = gPane.clip != kNoId ? s.clip(gPane.clip) : nullptr;
  const Channel* ch = s.channel(gPane.channel);
  if (!gPaneOn || !ch) return r;
  r.ok = true;
  r.clip = c ? c->id : kNoId;
  r.channel = ch->id;
  r.name = c && !c->name.empty() ? c->name : ch->name;
  r.channelName = ch->name;
  r.colour = ch->color;
  const std::string path = samplePath(*ch);
  r.file = path.empty() ? std::string() : fs::u8path(path).filename().u8string();
  if (const ClipSource src = clipSourceIfReady(*ch); src.sample) {
    char b[96];
    std::snprintf(b, sizeof b, "%.1f kHz   %s   %.2f s", src.sample->sampleRate / 1000.0, src.sample->mono() ? i18n::tr("mono") : i18n::tr("stereo"),
                  src.sample->seconds());
    r.meta = b;
  }
  r.shared = clipsSharing(s, ch->id);
  r.warpMarkers = c ? static_cast<int>(c->warp.size()) : 0;
  r.moreSelected = c ? static_cast<int>(selectionFor(a, c->id).size()) - 1 : 0;
  for (const char* k : {"name", "info", "presets", "menu"}) reach(k);  // (the title row: the name, the file, the preset arrows, Menu...)
  if (c) reach("selcount");
  return r;
}

bool begin(App& a) {
  gPage = Page{};
  const Song& s = a.doc.song();
  const Clip* cp = gPaneOn && gPane.clip != kNoId ? s.clip(gPane.clip) : nullptr;
  if (cp && cp->kind != ClipKind::Audio) cp = nullptr;
  const Channel* ch = gPaneOn ? s.channel(gPane.channel) : nullptr;
  if (!ch) return false;
  gPage.on = true;
  if (cp) gPage.clip = *cp;
  gPage.chan = *ch;
  State& S = st(Host::Pane);
  gDrawFrame[static_cast<int>(Host::Pane)] = ImGui::GetFrameCount();
  S.clip = cp ? cp->id : kNoId;
  S.channel = ch->id;
  S.tab = clipwin::shownTab(S.tab, cp != nullptr);
  S.chipsMode = false;
  S.compactDev = false;
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsWindowAppearing()) {
    a.focusPluginOwner = ch->id;  // controller template knobs follow the shown sampler
    a.focusPluginSlot = -1;
  }
  hostbody::drawn(hostbody::Body::ClipEditor, ch->id, -1);
  ImGui::PushID("clipeditor");
  ImGui::PushID(static_cast<int>(Host::Pane));
  gTrkSkin = true;
  gPage.prevStyle = ui::detail::gScopedStyle;
  ui::detail::gScopedStyle = static_cast<int>(ui::ControlStyle::Tracker);
  gLaidHost = static_cast<int>(Host::Pane);
  gLaid[gLaidHost].clear();
  return true;
}

void end(App& a) {
  if (!gPage.on) return;
  State& S = st(Host::Pane);
  ui::detail::gScopedStyle = gPage.prevStyle;
  gTrkSkin = false;
  drawOpDialog(a);
  const bool hasClip = gPage.clip.has_value();
  // W: warp mode while the page is under the pointer (Alt+W in the Tracker); Shift+W the spectrum.
  if (hasClip && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
    if (keys::pressed("wave.warpMode")) S.tool = S.tool == TWarp ? TPointer : TWarp;
    if (keys::pressed("wave.spectrum")) S.spectrum = !S.spectrum;
  }
  // Alt+P plays the clip (again: stops), as the Tracker Sampler's Alt+P plays its sample
  // (core/TrackerKeys.h, "audio.play").
  if (hasClip) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_P, false) && io.KeyAlt && !io.KeyCtrl && !io.KeyShift && !io.KeySuper && !io.WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
      previewClip(a, gPage.clip->id, S.loopPreview);
      gDevLastOp = a.engine.previewing() ? "alt+p play" : "alt+p stop";
    }
  }
  ImGui::PopID();
  ImGui::PopID();
  gPage = Page{};
}

void colour(App& a, ImVec2 at, float size) {
  if (!gPage.chan) return;
  const Channel& ch = *gPage.chan;
  ImGui::SetCursorScreenPos(at);
  uint32_t rgb = ch.color;
  reach("colour");
  if (ui::ColorSwatch("##cecol", &rgb, ImVec2(size, size)) && rgb != ch.color) ops::setChannelsColor(a.doc, {ch.id}, rgb);
  ui::Hint("Colour of the clip's channel (every clip of the sample)");
}

void menuItems(App& a) {
  if (!gPage.chan) return;
  headerMenu(a, gPage.clip ? &*gPage.clip : nullptr, *gPage.chan);
}

bool takeMenuAsk() { return std::exchange(st(Host::Pane).menuDev, false); }

std::string presetStep(App& a, int d) {
  reach("presets");
  return gPage.chan ? stepPluginPreset(a, PluginTarget{gPage.chan->id, -1}, d) : std::string();
}

float route(App& a, ImVec2 at, float w, float h) {
  if (!gPage.chan) return 0;
  reach("route");
  reach("routefree");
  const ObjectId chId = gPage.chan->id;
  ImGui::SetCursorScreenPos(ImVec2(at.x, std::round(at.y + (h - ImGui::GetFrameHeight()) * 0.5f)));
  if (chanroute::box(a, chId, "##ceroute", w, "ceroute")) a.later([&a, chId] { chanroute::routeToFree(a, chId); });
  ImGui::SetCursorScreenPos(ImVec2(at.x + w + 2, at.y));
  if (chanroute::button(a, chId, h)) a.later([&a, chId] { chanroute::routeToFree(a, chId); });
  devNoteRect("ceroutefree");
  return w + 2 + h;
}

void tools(App& a, ImVec2 p0, float w, float h) {
  if (!gPage.chan) return;
  toolStrip(a, st(Host::Pane), gPage.clip ? &*gPage.clip : nullptr, *gPage.chan, p0, w, h);
}

void wave(App& a, ImVec2 p0, ImVec2 p1, float ovH) {
  if (!gPage.chan) return;
  State& S = st(Host::Pane);
  if (gPage.clip) {
    waveArea(a, S, Host::Pane, *gPage.clip, *gPage.chan, p0, p1, ovH);
    return;
  }
  reach("overview");
  reach("wave");
  S.waveA = p0, S.waveB = p1, S.ovA = p0, S.ovB = p1;
  sampleStrip(a, S, *gPage.chan, p0, p1, true, ViewOnFile{});
}

void overview(App& a, ImVec2 p0, ImVec2 p1) {
  if (!gPage.chan) return;
  State& S = st(Host::Pane);
  S.ovA = p0, S.ovB = p1;
  sampleStrip(a, S, *gPage.chan, p0, p1, false, ViewOnFile{});
}

std::vector<Module> modules(App& a) {
  std::vector<Module> v;
  if (!gPage.on) return v;
  // (fresh copies: an edit above may have moved the song's clips and channels)
  const Song& s = a.doc.song();
  if (gPage.clip) {
    const Clip* c = s.clip(gPage.clip->id);
    if (!c) return v;
    gPage.clip = *c;
  }
  const Channel* ch = s.channel(gPage.chan->id);
  if (!ch) return v;
  gPage.chan = *ch;
  gPage.mods = modulesOf(a, gPage.clip ? &*gPage.clip : nullptr, *gPage.chan);
  gPage.mods.push_back(outputModule(a, gPage.clip ? &*gPage.clip : nullptr, *gPage.chan, Host::Pane));
  const bool hasClip = gPage.clip.has_value();
  for (const Mod& m : gPage.mods) v.push_back(Module{m.id, m.title, !hasClip && m.tab == TabTime ? static_cast<int>(TabSample) : m.tab, m.w()});  // (as onTab)
  return v;
}

float moduleHeight(bool compact) { return compact ? kCompactModH : kModH; }

float tabHeight(App& a, float w, bool compact) {
  if (!gPage.on || gPage.mods.empty()) return 0;
  State& S = st(Host::Pane);
  const bool hasClip = gPage.clip.has_value();
  std::vector<Mod*> on;
  for (size_t i = 0; i + 1 < gPage.mods.size(); ++i)
    if (onTab(gPage.mods[i], moduleTab(S.tab, hasClip), hasClip)) on.push_back(&gPage.mods[i]);
  const Lay L = layTab(on, gPage.mods.back(), w, compact);
  (void)a;
  return L.ok ? L.h : -1;
}

std::vector<Laid> tabPanel(App& a, ImVec2 p0, float w, float hAvail, bool compact) {
  std::vector<Laid> r;
  if (!gPage.on || gPage.mods.empty()) return r;
  State& S = st(Host::Pane);
  const bool hasClip = gPage.clip.has_value();
  S.tab = moduleTab(S.tab, hasClip);
  std::vector<Mod*> on;
  for (size_t i = 0; i + 1 < gPage.mods.size(); ++i)
    if (onTab(gPage.mods[i], S.tab, hasClip)) on.push_back(&gPage.mods[i]);
  Mod& out = gPage.mods.back();
  Lay L = layTab(on, out, w, compact);
  if (!L.ok && !compact) L = layTab(on, out, w, compact = true);
  S.compactDev = compact;
  S.tabsA = p0, S.tabsB = ImVec2(p0.x + w, p0.y + hAvail);  // (the strip check: Output inside, 12 px in)
  S.tabRects.clear();  // (the page list is the Tracker's: no tab bar)
  S.moreTabs = 0;
  gLaidHost = static_cast<int>(Host::Pane);
  gLaid[gLaidHost].clear();
  if (L.ok) tabModules(a, S, on, out, L, p0, w, hAvail, compact);
  for (const LaidMod& m : gLaid[gLaidHost]) r.push_back(Laid{m.id, m.a, m.b});
  return r;
}

void preview(App& a) {
  if (gPage.clip) previewClip(a, gPage.clip->id, st(Host::Pane).loopPreview);
}

void editAudioMenu(App& a, bool open, ImVec2 at) {
  if (!gPage.chan) return;
  State& S = st(Host::Pane);
  reach("edit.ops");
  if (gPage.clip) reach("edit.send");
  if (open || std::exchange(S.editDev, false)) ImGui::OpenPopup("##ceeditaudio");
  ImGui::SetNextWindowPos(at, ImGuiCond_Appearing, ImVec2(0, 0));
  if (ImGui::BeginPopup("##ceeditaudio")) {
    editAudioItems(a, S, gPage.clip ? &*gPage.clip : nullptr, *gPage.chan);
    ImGui::EndPopup();
  }
}

int tab() { return st(Host::Pane).tab; }
void setTab(int tab) {
  State& S = st(Host::Pane);
  S.tab = std::clamp(tab, 0, TabCount - 1);
}
bool reachAll() { return gReachOn; }
ObjectId selection(double* t0, double* t1) {
  const State& S = st(Host::Pane);
  if (t0) *t0 = std::min(S.selT0, S.selT1);
  if (t1) *t1 = std::max(S.selT0, S.selT1);
  return gPaneOn ? gPane.clip : kNoId;
}
void setSelection(double t0, double t1) {
  State& S = st(Host::Pane);
  S.selT0 = std::min(t0, t1), S.selT1 = std::max(t0, t1);
  gPendSel = PendSel{gPaneOn, gPane.clip, S.selT0, S.selT1};  // (kept over the view's first fit to the clip)
}

}  // namespace trackerpage

namespace {

// (dev) "clipeditor poke <key>": one edit as the control under that checklist key makes it (one
// undo step), on the shown clip or channel; prints what it changed.
bool poke(App& a, State& S, const std::string& key) {
  const Song& s = a.doc.song();
  const Clip* c = S.clip != kNoId ? s.clip(S.clip) : nullptr;
  const Channel* ch = s.channel(S.channel);
  if (!ch) return false;
  using clipfields::Field;
  auto par = [&](int idx, double v) { a.doc.setParam(smpPid(ch->id, idx), v, false); };
  auto flip = [&](int idx) { par(idx, smpPar(*ch, idx, 0) >= 0.5f ? 0.0 : 1.0); };
  if (key == "vol") a.doc.setParam(channelParam(ch->id, ChannelParam::Volume), 0.5, false);
  else if (key == "pan") a.doc.setParam(channelParam(ch->id, ChannelParam::Pan), 0.25, false);
  else if (key == "pitch") a.doc.setParam(channelParam(ch->id, ChannelParam::Pitch), 2, false);
  else if (key == "gain" && c) setField(a, c->id, Field::GainDb, -6, false);
  else if (key == "reverse.clip" && c) setField(a, c->id, Field::Reverse, c->reverse ? 0 : 1, false);
  else if (key == "fades" && c) setField(a, c->id, Field::FadeIn, 0.05, false);
  else if (key == "semis" && c) setField(a, c->id, Field::Semitones, 3, false);
  else if (key == "loopclip" && c) setField(a, c->id, Field::Loop, c->loop ? 0 : 1, false);
  else if (key == "stretch" && c) setField(a, c->id, Field::Stretch, c->stretch * 2.0, false);
  else if (key == "tempo" && c) setField(a, c->id, Field::FollowTempo, c->syncBpm > 0 ? 0 : 1, false);
  else if (key == "engine") par(pp::SmpStretchMode, static_cast<int>(stretch::kChoices[1]));
  else if (key == "startend") par(pp::SmpStart, 0.1);
  else if (key == "removedc") flip(pp::SmpRemoveDc);
  else if (key == "smpfades") par(pp::SmpFadeIn, 0.2);
  else if (key == "oneshot") flip(pp::SmpOneShot);
  else if (key == "reverse.notes") flip(pp::SmpReverse);
  else if (key == "loop.notes") flip(pp::SmpLoop);
  else if (key == "root") par(pp::SmpRoot, 62);
  else if (key == "volenv") par(pp::SmpAttack, 0.25);
  else if (key == "filter") par(pp::SmpFilterType, 1);
  else if (key == "lfo") par(pp::SmpLfoTarget, 2);
  else if (key == "fx.shape") par(pp::SmpPreRampIn, 0.3);
  else if (key == "fx.tone") par(pp::SmpPreTilt, 0.3);
  else if (key == "fx.space") par(pp::SmpPreReverbMix, 0.3);
  else if (key == "tune") a.doc.editChannel(ch->id, "channel pitch range", [](Channel& x) { x.bendRange = x.bendRange == 2 ? 12 : 2; });
  else if (key == "colour") ops::setChannelsColor(a.doc, {ch->id}, ch->color ^ 0x00FF00u);
  else return false;
  return true;
}

}  // namespace

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd != "clipeditor") return false;
  const std::string what = args.empty() ? "state" : args[0];
  auto host = [&]() -> Host {
    if (gDevHost >= 0) return static_cast<Host>(gDevHost);
    const int f = ImGui::GetFrameCount();
    if (gDrawFrame[1] >= f - 2) return Host::Clips;
    if (gDrawFrame[2] >= f - 2) return Host::Pane;
    if (gDrawFrame[0] >= f - 2) return Host::Window;
    if (gDrawFrame[3] >= f - 2) return Host::Device;
    return Host::Window;
  };
  const Host h = host();
  State& S = st(h);
  if (what == "state") {
    const char* hn = h == Host::Clips ? "clips" : h == Host::Pane ? "pane" : h == Host::Device ? "device" : "window";
    const bool shown = gDrawFrame[static_cast<int>(h)] >= ImGui::GetFrameCount() - 2;
    std::printf("clipeditor state frame %d host %s shown %d clip %llu tab %d tool %d spectrum %d sel %.1f %.1f window %llu pane %llu last \"%s\" "
                "channel %llu mode %s tabname \"%s\" wchannel %llu view %s chips %d\n",
                ImGui::GetFrameCount(), hn, shown ? 1 : 0, static_cast<unsigned long long>(S.clip), S.tab, S.tool, S.spectrum ? 1 : 0, S.selT0, S.selT1,
                static_cast<unsigned long long>(windowClip()), static_cast<unsigned long long>(paneClip()), gDevLastOp.c_str(),
                static_cast<unsigned long long>(S.channel), S.clip != kNoId ? "clip" : "channel", clipwin::tabName(S.tab),
                static_cast<unsigned long long>(gWindow.channel), mainView(a) ? "main" : "advanced", S.chipsMode ? 1 : 0);
  } else if (what == "host" && args.size() > 1) {  // the host the dev commands act on: clips|device|pane|window|auto
    const std::string n = args[1];
    gDevHost = n == "clips" ? 1 : n == "pane" ? 2 : n == "device" ? 3 : n == "window" ? 0 : -1;
  } else if (what == "geom") {
    std::printf("clipeditor geom frame %d wave %.0f %.0f %.0f %.0f overview %.0f %.0f %.0f %.0f tabs %.0f %.0f %.0f %.0f edges %.0f %.0f view %.1f %.1f\n",
                ImGui::GetFrameCount(), S.waveA.x, S.waveA.y, S.waveB.x, S.waveB.y, S.ovA.x, S.ovA.y, S.ovB.x, S.ovB.y, S.tabsA.x, S.tabsA.y, S.tabsB.x,
                S.tabsB.y, S.xs, S.xe, S.t0, S.t1);
    // Where the clip sounds against where its waveform was drawn (px runs, inside the view).
    auto runs = [](const std::vector<std::pair<float, float>>& v) {
      std::string o;
      char b[48];
      for (const auto& r : v) {
        std::snprintf(b, sizeof b, " %.1f-%.1f", static_cast<double>(r.first), static_cast<double>(r.second));
        o += b;
      }
      return o.empty() ? std::string(" none") : o;
    };
    for (const auto& [id, r] : S.cardRects)
      std::printf("clipeditor card \"%s\" %.0f %.0f %.0f %.0f\n", id.c_str(), static_cast<double>(r.first.x), static_cast<double>(r.first.y),
                  static_cast<double>(r.second.x), static_cast<double>(r.second.y));
    std::printf("clipeditor audible frame %d px%s\nclipeditor drawn frame %d px%s\n", ImGui::GetFrameCount(), runs(S.audiblePx).c_str(),
                ImGui::GetFrameCount(), runs(S.drawnPx).c_str());
  } else if (what == "strip") {
    // The shown tab's modules as last drawn, checked: every control inside its module, none over
    // another, every label with the width it needs, each title clear of its row's right side, the
    // modules apart, the rows, knob names, knobs and values on the same lines in every module; every
    // module inside the panel's width (`wide`: none goes past it sideways) and the tabs inside the
    // bar, none over another (`tabs`); `scroll` 1 when the panel scrolls up and down.
    const std::vector<LaidMod>& v = gLaid[static_cast<int>(h)];
    int outside = 0, overlaps = 0, clipped = 0, titles = 0, apart = 0, baselines = 0, wide = 0, tabsBad = 0;
    std::string why;
    auto note = [&](const std::string& t) {
      if (why.size() < 1400) why += " " + t;
    };
    auto inside = [](const LaidCtl& c, const LaidMod& m) {
      return c.a.x >= m.a.x - 0.5f && c.a.y >= m.a.y - 0.5f && c.b.x <= m.b.x + 0.5f && c.b.y <= m.b.y + 0.5f;
    };
    auto over = [](const LaidCtl& p, const LaidCtl& q) {
      return std::min(p.b.x, q.b.x) - std::max(p.a.x, q.a.x) > 0.5f && std::min(p.b.y, q.b.y) - std::max(p.a.y, q.a.y) > 0.5f;
    };
    for (size_t i = 0; i < v.size(); ++i) {
      const LaidMod& m = v[i];
      std::printf("clipeditor module %s %.0f %.0f %.0f %.0f compact %d controls %d\n", m.id.c_str(), static_cast<double>(m.a.x), static_cast<double>(m.a.y),
                  static_cast<double>(m.b.x - m.a.x), static_cast<double>(m.b.y - m.a.y), m.compact ? 1 : 0, static_cast<int>(m.ctl.size()));
      if (m.titleEnd + 4 > m.rightStart) ++titles, note(m.id + ":title");
      for (size_t j = i + 1; j < v.size(); ++j)
        if (m.id != "output" && v[j].id != "output" && v[j].a.y == m.a.y && std::min(m.b.x, v[j].b.x) - std::max(m.a.x, v[j].a.x) > -kModGap + 0.5f) ++apart, note(m.id + "/" + v[j].id);
      for (size_t k = 0; k < m.ctl.size(); ++k) {
        const LaidCtl& c = m.ctl[k];
        if (!inside(c, m)) ++outside, note(m.id + ":outside" + std::to_string(k));
        if (c.need > c.b.x - c.a.x + 0.5f) ++clipped, note(m.id + ":label" + std::to_string(k));
        const float ry = c.a.y - m.a.y;
        if (c.kind == 1) {  // a knob cell: on the knob row (a folded module's: on one of its knob rows)
          const float kr = ry - (m.compact ? kCompactNameY : kNameY);
          if (std::fabs(kr - std::round(kr / kKnobRowH) * kKnobRowH) > 0.5f || (!m.compact && std::fabs(kr) > 0.5f)) ++baselines, note(m.id + ":knob" + std::to_string(k));
        } else if (c.kind == 0 && !m.compact) {  // a control: on row A or row B (or the keyboard across both)
          if (std::fabs(ry - kRowA) > 0.5f && std::fabs(ry - kRowB) > 0.5f) ++baselines, note(m.id + ":row" + std::to_string(k));
        }
        for (size_t q = k + 1; q < m.ctl.size(); ++q)
          if (c.kind != 2 && m.ctl[q].kind != 2 && over(c, m.ctl[q])) ++overlaps, note(m.id + ":" + std::to_string(k) + "x" + std::to_string(q));
        if (c.kind == 2)
          for (size_t q = 0; q < m.ctl.size(); ++q)
            if (q != k && m.ctl[q].kind != 2 && over(c, m.ctl[q])) ++overlaps, note(m.id + ":display" + std::to_string(q));
      }
    }
    {
      for (const LaidMod& m : v) {
        if (m.id == "output") {  // pinned at the right, or among the others
          if (m.b.x > S.tabsB.x - 11.5f || m.a.x < S.tabsA.x + 11.5f) ++wide, note("output:wide");
        } else if (m.a.x < S.panelA.x - 0.5f || m.b.x > S.panelB.x + 0.5f) {
          ++wide, note(m.id + ":wide");
        }
      }
      for (size_t i = 0; i < S.tabRects.size(); ++i) {
        const auto& [ta, tb] = S.tabRects[i];
        if (ta.x < S.tabsA.x + 0.5f || tb.x > S.tabsB.x - 0.5f) ++tabsBad, note("tab" + std::to_string(i) + ":outside");
        for (size_t j = i + 1; j < S.tabRects.size(); ++j)
          if (std::min(tb.x, S.tabRects[j].second.x) - std::max(ta.x, S.tabRects[j].first.x) > 0.5f) ++tabsBad, note("tab" + std::to_string(i) + "x" + std::to_string(j));
      }
    }
    std::printf("clipeditor strip frame %d tab %d modules %d compact %d outside %d overlaps %d clipped %d titles %d apart %d baselines %d wide %d tabs %d more %d scroll %d%s\n",
                ImGui::GetFrameCount(), S.tab, static_cast<int>(v.size()), S.compactDev ? 1 : 0, outside, overlaps, clipped, titles, apart, baselines, wide, tabsBad,
                S.moreTabs, S.panelScrolls ? 1 : 0, why.c_str());
  } else if (what == "scroll" && args.size() > 1) {  // the tab that module (by id) is on
    const Song& s = a.doc.song();
    const Clip* c = S.clip != kNoId ? s.clip(S.clip) : nullptr;
    const Channel* ch = s.channel(S.channel);
    if (ch) {
      const std::vector<Mod> mods = modulesOf(a, c, *ch);
      for (const Mod& m : mods)
        if (args[1] == m.id) S.tab = !c && m.tab == TabTime ? TabSample : m.tab;
    }
  } else if (what == "winsize" && args.size() > 2) {  // the window's size next frame (its minimum at least)
    gDevWinSize = ImVec2(static_cast<float>(std::atof(args[1].c_str())), static_cast<float>(std::atof(args[2].c_str())));
  } else if (what == "toolrow") {
    // The tools row as last drawn: its items inside the row, none over another; `menu` 1 when the
    // view's options went into its menu (a narrow row).
    const auto& v = S.toolRects;
    int outside = 0, overlaps = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].first.x < S.toolA.x - 0.5f || v[i].second.x > S.toolB.x + 0.5f || v[i].first.y < S.toolA.y - 0.5f || v[i].second.y > S.toolB.y + 0.5f) ++outside;
      for (size_t j = i + 1; j < v.size(); ++j)
        if (std::min(v[i].second.x, v[j].second.x) - std::max(v[i].first.x, v[j].first.x) > 0.5f &&
            std::min(v[i].second.y, v[j].second.y) - std::max(v[i].first.y, v[j].first.y) > 0.5f)
          ++overlaps;
    }
    std::printf("clipeditor toolrow frame %d width %.0f items %d menu %d outside %d overlaps %d\n", ImGui::GetFrameCount(), static_cast<double>(S.toolB.x - S.toolA.x),
                static_cast<int>(v.size()), S.toolMenu ? 1 : 0, outside, overlaps);
  } else if (what == "editmenu") {
    S.editDev = true;
  } else if (what == "tab" && args.size() > 1) {  // (the window: on its Advanced view, where the tabs are)
    S.tab = std::clamp(std::atoi(args[1].c_str()), 0, TabCount - 1);
    S.scrollTab = S.tab;
    if (h == Host::Window) setMainView(a, false);
  } else if (what == "view" && args.size() > 1) {  // the window's view: main|advanced
    setMainView(a, args[1] != "advanced");
  } else if (what == "mainpage") {
    // The Main view as last drawn: its cell, card height, rows and cards; every control inside its
    // card, none over another, every label with the width it needs, no knob name or value cut
    // (`cut`), the cards inside the page and apart; `scroll` 1 when it scrolls up and down.
    const std::vector<LaidMod>& v = gLaid[static_cast<int>(h)];
    int outside = 0, overlaps = 0, clipped = 0, wide = 0, apart = 0;
    std::string why;
    for (size_t i = 0; i < v.size(); ++i) {
      const LaidMod& m = v[i];
      if (m.a.x < S.panelA.x - 0.5f || m.b.x > S.panelB.x + 0.5f) ++wide, why += " " + m.id + ":wide";
      for (size_t j = i + 1; j < v.size(); ++j)
        if (std::min(m.b.x, v[j].b.x) - std::max(m.a.x, v[j].a.x) > 0.5f && std::min(m.b.y, v[j].b.y) - std::max(m.a.y, v[j].a.y) > 0.5f)
          ++apart, why += " " + m.id + "/" + v[j].id;
      for (size_t k = 0; k < m.ctl.size(); ++k) {
        const LaidCtl& c = m.ctl[k];
        if (c.a.x < m.a.x - 0.5f || c.a.y < m.a.y - 0.5f || c.b.x > m.b.x + 0.5f || c.b.y > m.b.y + 0.5f) ++outside, why += " " + m.id + ":out" + std::to_string(k);
        if (c.need > c.b.x - c.a.x + 0.5f) ++clipped, why += " " + m.id + ":label" + std::to_string(k) + "(" + std::to_string(static_cast<int>(c.need)) + ">" + std::to_string(static_cast<int>(c.b.x - c.a.x)) + ")";
        for (size_t q = k + 1; q < m.ctl.size(); ++q) {
          const LaidCtl& d = m.ctl[q];
          if (c.kind == 2 || d.kind == 2) continue;
          if (std::min(c.b.x, d.b.x) - std::max(c.a.x, d.a.x) > 0.5f && std::min(c.b.y, d.b.y) - std::max(c.a.y, d.a.y) > 0.5f)
            ++overlaps, why += " " + m.id + ":" + std::to_string(k) + "x" + std::to_string(q);
        }
      }
    }
    for (const LaidMod& m : v)
      std::printf("clipeditor maingroup %s %.0f %.0f %.0f %.0f controls %d\n", m.id.c_str(), static_cast<double>(m.a.x), static_cast<double>(m.a.y),
                  static_cast<double>(m.b.x - m.a.x), static_cast<double>(m.b.y - m.a.y), static_cast<int>(m.ctl.size()));
    std::printf("clipeditor mainpage frame %d shown %d knob %.0f cell %.0f groups %d outside %d overlaps %d clipped %d cut %d wide %d apart %d scroll %d %s%s\n",
                ImGui::GetFrameCount(), S.mainDrawn ? 1 : 0, static_cast<double>(S.mainCell), static_cast<double>(S.mainCardH),
                static_cast<int>(v.size()), outside, overlaps, clipped, gTextCut, wide, apart, S.panelScrolls ? 1 : 0, mainv::spacingReport().c_str(), why.substr(0, 600).c_str());
    gTextCut = 0;
  } else if (what == "tool" && args.size() > 1) {
    S.tool = std::clamp(std::atoi(args[1].c_str()), 0, TCount - 1);
  } else if (what == "spectrum" && args.size() > 1) {
    S.spectrum = args[1] == "1";
  } else if (what == "spec") {  // the spectrum's images: the view's, its pixels, the stretch, what was made
    const ClipSpec& cs = gClipSpec[&S];
    std::printf("clipeditor spec frame %d current %d view %lld %lld cols %d rows %d pixels %.0f %.0f points %.0f %.0f stretch %.3f %.3f made %d %d uploads %d %d runs %d builds %d\n",
                ImGui::GetFrameCount(), cs.current ? 1 : 0, static_cast<long long>(cs.view.img.req.from), static_cast<long long>(cs.view.img.req.to),
                cs.view.img.req.cols, cs.view.img.req.rows, static_cast<double>(cs.pxW), static_cast<double>(cs.pxH), static_cast<double>(cs.ptW),
                static_cast<double>(cs.ptH), static_cast<double>(cs.stretchX),
                static_cast<double>(cs.stretchY), cs.base.made, cs.view.made, cs.base.uploads, cs.view.uploads, static_cast<int>(cs.segs.size()), cs.segBuilds);
  } else if (what == "zoom" && args.size() > 1) {  // zoom the view about its middle (2: twice as close)
    const double k = std::max(1e-3, std::atof(args[1].c_str())), mid = (S.t0 + S.t1) * 0.5, half = (S.t1 - S.t0) * 0.5 / k;
    S.t0 = mid - half, S.t1 = mid + half;
  } else if (what == "select" && args.size() > 2) {
    S.selT0 = std::atof(args[1].c_str()), S.selT1 = std::atof(args[2].c_str());
  } else if (what == "dbl" && args.size() > 1) {
    openFromDoubleClick(a, static_cast<ObjectId>(std::strtoull(args[1].c_str(), nullptr, 10)));
  } else if (what == "open" && args.size() > 1) {
    open(a, static_cast<ObjectId>(std::strtoull(args[1].c_str(), nullptr, 10)), args.size() > 2 ? std::atoi(args[2].c_str()) : -1);
  } else if (what == "channel" && args.size() > 1) {
    const bool ok = openChannel(a, static_cast<ObjectId>(std::strtoull(args[1].c_str(), nullptr, 10)), args.size() > 2 ? std::atoi(args[2].c_str()) : -1);
    std::printf("clipeditor channel %s\n", ok ? "opened" : "refused");
  } else if (what == "reach" && args.size() > 1) {
    gReachOn = args[1] == "on";
    if (args[1] == "on" || args[1] == "clear") gReached.clear();
  } else if (what == "checklist") {
    // The old controls not drawn since "reach on" (core/ClipWindow.h): none means each has a home.
    const Song& s = a.doc.song();
    const Clip* c = S.clip != kNoId ? s.clip(S.clip) : nullptr;
    const bool launch = c && clipsLayout() && c->start < s.live.areaEnd;
    // ("checklist main": the old sampler window's controls the Main view must show)
    const std::vector<std::string> miss = args.size() > 1 && args[1] == "main" ? clipwin::missingMain(gReached, c != nullptr)
                                                                               : clipwin::missing(gReached, c != nullptr, launch);
    std::string list;
    for (const std::string& k : miss) list += " " + k;
    std::printf(args.size() > 1 && args[1] == "main" ? "clipeditor checklist main reached %d missing %d%s\n" : "clipeditor checklist reached %d missing %d%s\n", static_cast<int>(gReached.size()), static_cast<int>(miss.size()), list.c_str());
  } else if (what == "menu") {
    S.menuDev = true;
  } else if (what == "bounds" && args.size() > 1) {  // measure what the native views draw (on|off)
    gMeasure = args[1] == "on";
  } else if (what == "unfold") {  // every card and section open
    gBandFolded.clear();
    gRowsUnfoldAll = true;
  } else if (what == "fold" && args.size() > 1) {  // fold that card or section (by module id)
    gBandFolded.insert(args[1]);
    gRowsFolded.insert(args[1]);
    gRowsUnfoldAll = false;
  } else if (what == "viewmenu") {
    S.viewDev = true;
  } else if (what == "native") {
    // What the layout's own view (the Clips band, Smooth's pane and inspector) drew: its place, the
    // box its drawing took up (`outside`: anything showing past its place), the popups open, its
    // cards and modules; each module's controls inside it and none over another.
    for (int k = 0; k < 5; ++k) {
      const NativeCheck& n = gNative[k];
      if (n.frame < ImGui::GetFrameCount() - 3) continue;
      const bool out = n.any && (n.drawnA.x < n.a.x - 0.5f || n.drawnA.y < n.a.y - 0.5f || n.drawnB.x > n.b.x + 0.5f || n.drawnB.y > n.b.y + 0.5f);
      int outside = 0, overlaps = 0, clipped = 0;
      std::string why;
      for (const LaidMod& m : gLaid[k]) {
        for (size_t i = 0; i < m.ctl.size(); ++i) {
          const LaidCtl& c = m.ctl[i];
          if (c.a.x < m.a.x - 0.5f || c.a.y < m.a.y - 0.5f || c.b.x > m.b.x + 0.5f || c.b.y > m.b.y + 0.5f) ++outside, why += " " + m.id + ":out" + std::to_string(i);
          if (c.need > c.b.x - c.a.x + 0.5f)
            ++clipped, why += " " + m.id + ":label" + std::to_string(i) + "(" + std::to_string(static_cast<int>(c.need)) + ">" + std::to_string(static_cast<int>(c.b.x - c.a.x)) + ")";
          for (size_t q = i + 1; q < m.ctl.size(); ++q) {
            const LaidCtl& d = m.ctl[q];
            if (c.kind == 2 || d.kind == 2) continue;
            if (std::min(c.b.x, d.b.x) - std::max(c.a.x, d.a.x) > 0.5f && std::min(c.b.y, d.b.y) - std::max(c.a.y, d.a.y) > 0.5f)
              ++overlaps, why += " " + m.id + ":" + std::to_string(i) + "x" + std::to_string(q);
          }
        }
      }
      std::printf("clipeditor native frame %d slot %d form %s rect %.0f %.0f %.0f %.0f drawn %.0f %.0f %.0f %.0f outside %d popups %d cards %d modules %d "
                  "ctloutside %d overlaps %d clipped %d%s\n",
                  ImGui::GetFrameCount(), k, n.form.c_str(), n.a.x, n.a.y, n.b.x, n.b.y, n.drawnA.x, n.drawnA.y, n.drawnB.x, n.drawnB.y, out ? 1 : 0, n.popups,
                  n.cards, static_cast<int>(gLaid[k].size()), outside, overlaps, clipped, why.substr(0, 600).c_str());
    }
  } else if (what == "chip" && args.size() > 1) {  // (0.7.8 earlier: the chip row's popover) the band's card of that tab, in view
    S.tab = std::clamp(std::atoi(args[1].c_str()), 0, TabCount - 1);
    S.scrollTab = S.tab;
  } else if (what == "poke" && args.size() > 1) {
    const size_t before = a.doc.history().position();
    const bool ok = poke(a, S, args[1]);
    std::printf("clipeditor poke %s %s undo %d\n", args[1].c_str(), ok ? "done" : "unknown", static_cast<int>(a.doc.history().position() - before));
  } else if (what == "panel" && args.size() > 1) {
    S.panelH = static_cast<float>(std::atof(args[1].c_str()));
  } else if (what == "op" && args.size() > 1) {
    for (int i = 0; i < static_cast<int>(clipedit::Op::Count); ++i)
      if (args[1] == clipedit::opName(static_cast<clipedit::Op>(i))) {
        clipedit::Args g;
        if (args.size() > 2) g.db = std::atof(args[2].c_str()), g.factor = std::atof(args[2].c_str());
        runAudioOp(a, S.clip, static_cast<clipedit::Op>(i), g, S.selT0, S.selT1, S.clip == kNoId ? S.channel : kNoId);
      }
  } else if (what == "measure") {
    const Song& s = a.doc.song();
    const Clip* c = s.clip(S.clip);
    const Channel* ch = c ? channelOf(s, *c) : nullptr;
    if (ch) {
      int64_t fa = 0, fb = 0;
      clipedit::clipRangeFrames(s, c->id, static_cast<double>(c->start), static_cast<double>(c->start + s.clipLength(*c)), fa, fb);
      gMeasures.key.clear();
      const Measures& m = measure(ch->plugin.data, fa, fb);
      std::printf("clipeditor measure peak %.2f rms %.2f lufs %.2f frames %lld %lld\n", m.lv.peakDb, m.lv.rmsDb, m.lv.lufs, static_cast<long long>(fa),
                  static_cast<long long>(fb));
    }
  } else if (what == "close") {
    gWindow = {};
    gPaneOn = false;
    if (tracker() && trackerpage::showing(a)) trackereditors::close(a);  // (the Tracker's Sample editor page closes)
  }
  return true;
}

}  // namespace clipeditor
}  // namespace gl
