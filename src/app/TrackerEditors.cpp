// Gridline - the Tracker layout's lower-band editors (K-106 cp18). See TrackerEditors.h.
//
// The Sample editor follows the classic tracker's sample screen in function (K-316 plan 3): the
// waveform across the band (range dragged out inverted, loop pins at the top and bottom edges, the
// sample's start and end as handles at mid height, the play position of every voice of the sample),
// a scroll bar, and the button panel under it (play, range, view, clipboard, level, rate and time,
// loop type, bits, readouts and steppers, exit). Drawn in Gridline's own Tracker look (bevel boxes,
// the band's buttons, the monospaced face); every label is Gridline's own wording. The ft2-clone
// source was read for behaviour only (zoom anchored at the pointer, pins at the edges).
//
// CPU: the instrument is read through the session's snapshot (parsed once per document revision),
// the waveform from the file's peaks pyramid through a column cache rebuilt only when the view
// changes, the play tap once a frame while a voice of the instrument sounds.
#include "UrlImportUi.h"
#include "TrackerEditors.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <SDL.h>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/BrowserIndex.h"
#include "../core/ClipWindow.h"
#include "../core/ExternalDrop.h"
#include "../core/I18n.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/TrackerGrid.h"
#include "../engine/ClipAudioEdit.h"
#include "../engine/Sample.h"
#include "../packs/seltmann/seltmann_tracker.h"
#include "../packs/tracker/tracker.h"
#include "../packs/tracker/tracker_peaks.h"
#include "../packs/tracker/tracker_session.h"
#include "App.h"
#include "BrowserUi.h"
#include "ClipEditor.h"
#include "CollabUi.h"
#include "ContextMenus.h"
#include "ExternalDragUi.h"
#include "HostedBodies.h"
#include "HostedUi.h"
#include "KeyLight.h"
#include "Midi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "TrackerSampleFiles.h"
#include "LayoutShellUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "TrackerView.h"
#include "Widgets.h"

namespace gl::trackereditors {

using theme::pal;
using namespace trackerview;
namespace tp = gl::packs::trackerpack;
namespace trk = gl::tracker;
namespace fs = std::filesystem;

namespace {

// ---- the state (view state, never saved) ----
struct KeyEv {
  int sc = 0, kc = 0, mods = 0;
};
struct Held {
  ObjectId channel = kNoId;
  int pitch = -1;
};
enum class Modal { None, Volume, Resample, Echo, Mix, LoopFade, Remove, Convert, ReplaceEntry, Effects };
enum class Shown { None, Sample, Plugin, Frame, Instrument, Chop, SeltInst, Clip };  // (Chop, SeltInst: Seltmann's screens, K-347; Clip: a Sampler's, 0.7.8)
namespace seltt = packs::seltmann::tracker;
struct St {
  Editor want = Editor::Instrument;
  int openedFrame = -100;   // open() asked on this frame (the screen is the one asked for)
  bool giveKeys = false;
  int keyFrames = 0;        // frames left to settle the keys after an open
  int drawnFrame = -100;
  bool hasKb = false;
  bool wasFocused = false;  // cp25: the band had the keys last frame
  int pressFrame = -100;    // cp25: the last press inside the band
  Shown shown = Shown::None;
  float need = 0;
  // K-391: the channel whose Tracker screen the band was asked for (open()). Any other way the
  // channel settings open (a double-click, View > Channel settings, Open plugin window, a menu) they
  // float in their window, as in Studio; the band shows only the Tracker's own screens.
  ObjectId bandCh = kNoId;
  bool devFrame = false;    // (dev "open frame") the Channel frame in the band
  // The well.
  int drag = 0;             // 1 a range, 2 a pin or handle, 3 the pencil (right-drag)
  std::vector<std::pair<double, float>> pencil;  // cp20: the pencil's stroke so far (file frame, value)
  int marker = -1;            // (4: the whole loop, 0.7.8)
  int32_t anchor = 0;
  int32_t loopGrab = 0;       // 0.7.8: the whole loop's drag: the pointer's frame from the loop start at the press
  tp::ColumnCache cols;
  // Keys.
  std::vector<KeyEv> keys;
  std::map<int, Held> held;  // scancode -> the note it plays
  // Modals.
  Modal modal = Modal::None;
  bool modalOpen = false;
  int toEntry = -1;         // 0.7.8: the used entry To entry asks to replace
  ObjectId convertCh = kNoId;  // K-391: the Sampler Convert to Tracker Sampler asks about
  // K-436: the Effects box (the chain is kept for the next sample while the program runs; never saved).
  struct FxRow {
    PluginState st;
    int mix = 100;     // percent of the effect heard
    int preset = -1;   // the factory preset last stepped to (-1: Default)
  };
  std::vector<FxRow> fx;
  int fxSel = 0;          // the row whose settings show
  int fxPick = -1;        // the row the effect picker replaces (fx.size(): a new row)
  int fxTail = 0;         // tenths of a second after the part, for echoes and reverb
  double fxScroll = 0;    // the settings' first row in view
  std::vector<std::pair<float, int32_t>> tapLines;  // 0.7.8 (dev): the play lines drawn last frame (x, file frame)
  float well[4] = {0, 0, 0, 0};                     // 0.7.8 (dev): the waveform's area as last drawn
  // Preview.
  uint32_t serial = 0;
  SamplePtr pvSample;       // K-391: a Sampler's preview (the part played, at the play note) on the sample preview
  ObjectId pvChannel = kNoId;
  int pvPitch = -1;
  double pvStarted = 0;
  // The Channel frame's notes count, by revision.
  uint64_t notesRev = ~uint64_t{0};
  ObjectId notesCh = kNoId;
  int notes = 0;
  std::string hover;        // the status line's hover text this frame
  int modalBegun = 0;       // dev: frames a modal box was drawn
  // cp19: the Instrument editor (view state, never saved).
  uint64_t insKey = 0;      // the instrument it last showed (another: the selections start again)
  int envSel[2] = {0, 0};   // the selected point of the volume / panning envelope
  int envDrag[2] = {-1, -1};
  float envSpan[2] = {0, 0};  // ticks the graph shows (kept while a point is dragged)
  int envFocus = 0;         // the envelope the keys edit (Tab)
  int envMenu = -1;         // the point a right-click menu is for
  bool envMenuPan = false;
  int slotMenu = -1;        // the preset a right-click menu is for
  bool slotMenuPan = false;
  int keyFrom = -1, keyTo = -1;  // a drag across the piano (keys given to the selected sample on release)
  int keyAud = -1;          // the key Alt+click plays
  ImVec2 envG0[2], envG1[2];  // the graphs' plotting areas (dev: pointer placement)
  ImVec2 pk0, pk1;          // the piano
  // cp20: the envelopes' play positions this frame (the play tap, read once per frame).
  int envTicks[2][tp::kTapVoices] = {};
  int envLines[2] = {0, 0};
  int pianoFirst = -1;      // a short band's piano: its first octave in view (-1: not chosen yet)
  int pianoOcts = 8;        // the octaves it shows (8: all of them)
  bool insExtra = false;    // the right column shows the instrument's extra settings (MIDI out, playback)
  // cp24: the Channel frame's fold.
  float frameRoom = 1e9f;   // the height the header band leaves the frame (setFrameRoom)
  float bandPad = 0;        // the band's own padding round the screen, as last measured
  // 0.7.8: a Sampler's Sample editor (the Audio clip window's parts in the band).
  bool clipWave = false;    // the Waveform page (else the settings page of the window's tab)
  int clipTabSeen = -1;     // the window's tab last shown (another, asked from anywhere: its page, from its start)
  std::string clipPreset;   // the preset the arrows stepped to last
  struct Part {
    std::string id;
    ImVec2 a, b;
  };
  std::vector<Part> clipParts;  // (dev "clip dump") the parts as last drawn
  ImVec2 clipBand0, clipBand1;  // (dev) the screen's area as last drawn
  int clipFrame = -100;         // (dev) the frame it was drawn
};
St& st() {
  static St s;
  return s;
}

bool trackerOn(App& a) { return layoutui::active() == layout::Preset::Tracker && layoutui::shellBuilt(a, layout::Preset::Tracker); }

const Channel* channelOf(App& a, ObjectId id) { return id == kNoId ? nullptr : a.doc.song().channel(id); }
bool isTrackerSampler(const Channel* c) { return c && c->plugin.type == trk::kInstrumentType; }
// 0.7.8: a Sampler: its Sample editor is the Audio clip window's (core/ClipWindow.h).
bool isSampler(const Channel* c) { return c && clipwin::ownsType(c->plugin.type); }
// K-391: a Sampler's Sample page shows the classic Sample editor (its file as a one-sample Tracker
// Sampler, tracker_session.h editState) or the advanced one (the Audio clip window's parts). The
// choice is an app setting ("tracker.sampleView": classic, advanced), never saved in a project.
bool advancedView(App& a) { return a.settings.getString("tracker.sampleView") == "advanced"; }
// While the classic Sample editor works on a Sampler, the header's sample ops take it too.
struct Bridge {
  bool prev;
  explicit Bridge(bool on) : prev(tp::samplerBridge()) {
    if (on) tp::setSamplerBridge(true);
  }
  ~Bridge() { tp::setSamplerBridge(prev); }
  Bridge(const Bridge&) = delete;
  Bridge& operator=(const Bridge&) = delete;
};
bool classicSampler(App& a, ObjectId ch) {
  const Channel* c = ch == kNoId ? nullptr : a.doc.song().channel(ch);
  return c && clipwin::ownsType(c->plugin.type) && !advancedView(a);
}

// Tracker note names (C-4, C#4); 1..96, 49 = C-4.
std::string noteName(int n) {
  static const char* names[] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  if (n < 1 || n > 96) return "---";
  return std::string(names[(n - 1) % 12]) + std::to_string((n - 1) / 12);
}
std::string hex2(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02X", v & 0xFF);
  return b;
}
std::string fmtInt(long long v) { return std::to_string(v); }

// ---- the preview (one sample, or a part of it, through the channel) ----
void stopPreview(App& a) {
  St& s = st();
  if (s.pvSample) {  // K-391: a Sampler's
    if (a.engine.previewingSample(s.pvSample.get())) a.engine.preview(nullptr);
    s.pvSample.reset();
  }
  if (s.serial == 0) return;
  tp::cancelAudition(s.serial);
  if (s.pvChannel != kNoId && s.pvPitch >= 0) a.engine.noteOff(s.pvChannel, s.pvPitch);
  s.serial = 0, s.pvChannel = kNoId, s.pvPitch = -1;
}
bool previewSounding(App& a) {
  const St& s = st();
  return (s.serial != 0 && tp::auditionSounding(s.serial)) || (s.pvSample && a.engine.previewingSample(s.pvSample.get()));
}
// K-391: a Sampler's sample in the classic editor plays its part on the sample preview, at the play
// note (its root key plays the file at its own rate) and backwards when it is set to.
void samplerPreview(App& a, const tp::InstrumentAux& inst, const tp::Edit& e, int k, int note, int32_t from, int32_t to) {
  const SamplePtr src = inst.files[static_cast<size_t>(k)];
  if (!src || src->frames() == 0) return;
  const auto frames = static_cast<int32_t>(std::min<size_t>(src->frames(), 0x3FFFFFFF));
  const int32_t c0 = std::clamp(e.cuts.start[static_cast<size_t>(k)], 0, frames - 1);
  const int32_t c1 = e.cuts.end[static_cast<size_t>(k)] > 0 ? std::clamp(e.cuts.end[static_cast<size_t>(k)], c0 + 1, frames) : frames;
  const int32_t f0 = std::clamp(std::max(from, c0), 0, frames), f1 = std::clamp(to > 0 ? to : c1, f0, frames);
  if (f1 <= f0) return;
  auto pv = std::make_shared<Sample>();
  const trk::SampleDef& sd = e.text.def.samples[static_cast<size_t>(k)];
  const double rate = src->sampleRate > 0 ? src->sampleRate : 44100.0;
  pv->sampleRate = rate * std::pow(2.0, (note - tp::rootNote(sd, rate)) / 12.0);
  pv->left.assign(src->left.begin() + f0, src->left.begin() + f1);
  if (src->mono()) pv->setMono();
  else pv->right.assign(src->right.begin() + f0, src->right.begin() + f1);
  if (sd.reverse) {
    std::reverse(pv->left.begin(), pv->left.end());
    if (!pv->mono()) std::reverse(pv->right.mut().begin(), pv->right.mut().end());
  }
  St& s = st();
  s.pvSample = pv;
  s.pvStarted = ImGui::GetTime();
  a.engine.preview(pv, true);
}
// Slot k at the play note: frames [from, to) of the file (to 0: to the sample's end).
void startPreview(App& a, ObjectId ch, const tp::InstrumentAux& inst, const tp::Edit& e, int k, int note, int32_t from, int32_t to) {
  stopPreview(a);
  if (k < 0 || k >= trk::kMaxSamples || !inst.data[static_cast<size_t>(k)]) return;
  if (isSampler(channelOf(a, ch))) {
    samplerPreview(a, inst, e, k, note, from, to);
    return;
  }
  // The played part starts at the sample's start: the request counts from there.
  const int32_t c0 = std::max(0, e.cuts.start[static_cast<size_t>(k)]);
  const int32_t len = inst.text.def.samples[static_cast<size_t>(k)].length;
  const int32_t f = std::clamp(from - c0, 0, std::max(0, len - 1));
  const int32_t t = to > 0 ? std::clamp(to - c0, 0, len) : 0;
  if (to > 0 && t <= f) return;
  const int pitch = std::clamp(note + trk::kPitchOffset, 0, 127);
  St& s = st();
  s.serial = tp::requestAudition(&inst, k, pitch, f, t);
  s.pvChannel = ch, s.pvPitch = pitch, s.pvStarted = ImGui::GetTime();
  a.engine.ownNoteOn(ch, pitch, 100.0f / 127.0f, AudioEngine::OwnPlay::EditorPreview);
}
void tendPreview(App& a) {
  St& s = st();
  if (s.pvSample && !a.engine.previewingSample(s.pvSample.get())) s.pvSample.reset();
  if (s.serial == 0) return;
  const bool sounding = tp::auditionSounding(s.serial);
  const bool stuck = sounding && ImGui::GetTime() - s.pvStarted > 0.75 && !tp::auditionPlaying(s.serial);
  if (!sounding || stuck) stopPreview(a);
}
void releaseHeld(App& a) {
  for (const auto& [sc, h] : st().held)
    if (h.channel != kNoId && h.pitch >= 0) a.engine.noteOff(h.channel, h.pitch);
  st().held.clear();
}

// ---- the Sample editor ----

// The instrument as the Sample editor works on it.
struct Ctx {
  ObjectId ch = kNoId;
  PluginTarget t;
  const tp::Snapshot* snap = nullptr;
  tp::Memory* m = nullptr;
  int k = -1;                 // the selected slot (-1: the instrument has no sample, or an empty entry is picked)
  int entry = -1;             // 0.7.8: the entry picked in the list (k when it holds a sample; else the empty one)
  SamplePtr sp;
  int32_t frames = 0;         // the file's
  int32_t s0 = 0, s1 = 0;     // the played part
  double rate = 44100;
  bool looping = false;
  int32_t ls = 0, le = 0;
  bool eight = false;
  bool rev = false;           // 0.7.8: the sample plays backwards
  bool sampler = false;       // K-391: a Sampler seen as a one-sample Tracker Sampler (one entry, full resolution)
  bool ok() const { return snap && snap->ok; }
  bool have() const { return k >= 0 && sp && frames > 0; }
};
Ctx context(App& a, ObjectId ch) {
  Ctx c;
  c.ch = ch;
  c.t = PluginTarget{ch, -1};
  const tp::Snapshot& sn = tp::snapshot(a, c.t);
  if (!sn.ok) return c;
  c.snap = &sn;
  c.sampler = sn.sampler;
  c.m = &tp::memory(c.t.key());
  const tp::Edit& e = sn.e;
  // 0.7.8: an empty entry picked in the list (or by Ctrl+Alt+Up / Down) shows as itself, empty.
  if (c.m->wantSlot >= 0 && c.m->wantSlot < trk::kMaxSamples && !tp::hasSample(e, c.m->wantSlot)) {
    c.entry = c.m->wantSlot;
    return c;
  }
  if (!tp::hasSample(e, c.m->sel))
    for (int k = 0; k < trk::kMaxSamples; ++k)
      if (tp::hasSample(e, k)) {
        c.m->sel = k;
        break;
      }
  c.entry = std::clamp(c.m->sel, 0, trk::kMaxSamples - 1);
  if (!tp::hasSample(e, c.m->sel)) return c;
  c.k = c.m->sel;
  const auto i = static_cast<size_t>(c.k);
  c.sp = sn.inst ? sn.inst->files[i] : nullptr;
  if (!c.sp || c.sp->frames() == 0) return c;
  c.frames = static_cast<int32_t>(std::min<size_t>(c.sp->frames(), 0x3FFFFFFF));
  c.rate = c.sp->sampleRate > 0 ? c.sp->sampleRate : 44100.0;
  c.s0 = std::clamp(e.cuts.start[i], 0, c.frames - 1);
  c.s1 = e.cuts.end[i] > 0 ? std::clamp(e.cuts.end[i], c.s0 + 1, c.frames) : c.frames;
  const trk::SampleDef& sd = e.text.def.samples[i];
  c.looping = sd.loop != trk::kLoopOff && sd.loopLength > 0;
  c.ls = sd.loopStart, c.le = sd.loopStart + sd.loopLength;
  c.eight = e.eightBit[i];
  c.rev = sd.reverse;
  if (c.m->viewSlot != c.k || c.m->viewLen <= 0 || c.m->viewLen > c.frames) {
    c.m->viewSlot = c.k;
    c.m->viewStart = 0;
    c.m->viewLen = c.frames;
  }
  c.m->viewStart = std::clamp(c.m->viewStart, 0.0, std::max(0.0, c.frames - c.m->viewLen));
  return c;
}

tp::Span rangeOf(const Ctx& c) { return c.m ? tp::rangeOf(*c.m, c.k) : tp::Span{}; }

// The view, zoomed about `centre` (file frames) by `f` (< 1 in), at least 64 frames (or the file).
void zoomAbout(Ctx& c, double f, double centre) {
  tp::Memory& m = *c.m;
  const double len = std::clamp(m.viewLen * f, std::min<double>(c.frames, 64.0), static_cast<double>(c.frames));
  m.viewStart = std::clamp(centre - (centre - m.viewStart) * len / m.viewLen, 0.0, c.frames - len);
  m.viewLen = len;
}
void zoomToRange(Ctx& c) {
  const tp::Span r = rangeOf(c);
  if (r.empty()) return;
  tp::Memory& m = *c.m;
  m.viewLen = std::max<double>(std::min<double>(c.frames, 64.0), r.b - r.a);
  m.viewStart = std::clamp(static_cast<double>(r.a), 0.0, c.frames - m.viewLen);
}
void wholeView(Ctx& c) {
  c.m->viewStart = 0;
  c.m->viewLen = c.frames;
}
void scrollBy(Ctx& c, double d) { c.m->viewStart = std::clamp(c.m->viewStart + d, 0.0, std::max(0.0, c.frames - c.m->viewLen)); }

// Alt held while a pin is dragged: the nearest frame where the sound crosses zero (within 1024).
int32_t zeroCrossing(const Ctx& c, int32_t f) {
  const float* L = c.sp->left.data();
  for (int32_t d = 0; d < 1024; ++d)
    for (int s = -1; s <= 1; s += 2) {
      const int32_t i = f + s * d;
      if (i <= 0 || i >= c.frames) continue;
      if ((L[i - 1] <= 0) != (L[i] <= 0)) return i;
    }
  return f;
}

// One sample edit on the selected slot (the plugin editor's editWave): one undo step, the edited
// audio written as a new file.
template <class Op>
bool waveEdit(App& a, Ctx& c, const char* label, Op op) {
  tp::Wave w;
  if (!c.have() || !tp::waveOf(c.snap->e, c.k, *c.snap->inst, w)) {
    a.notify(i18n::tr("The sample file is missing"));
    return false;
  }
  if (!op(w)) return false;
  stopPreview(a);
  if (!tp::opWaveCommit(a, c.t, c.k, w, label)) return false;
  if (c.m->viewLen > w.frames()) c.m->viewLen = 0;
  return true;
}

// cp20: a pencil stroke (file frames and values in the order the pointer drew them) into the
// selected sample: a straight line between the pointer's points, one undo step.
bool pencilCommit(App& a, Ctx& c, const std::vector<std::pair<double, float>>& pts) {
  if (pts.size() < 2 || !c.have()) {
    return false;
  }
  double lo = pts[0].first, hi = pts[0].first;
  for (const auto& p : pts) {
    lo = std::min(lo, p.first);
    hi = std::max(hi, p.first);
  }
  const auto fa = static_cast<int32_t>(std::clamp(std::floor(lo), 0.0, static_cast<double>(c.frames - 1)));
  const auto fb = static_cast<int32_t>(std::clamp(std::ceil(hi), 0.0, static_cast<double>(c.frames - 1)));
  std::vector<float> vals(static_cast<size_t>(fb - fa + 1), std::numeric_limits<float>::quiet_NaN());
  for (size_t i = 1; i < pts.size(); ++i) {
    const auto& p = pts[i - 1];
    const auto& q = pts[i];
    const double x0 = std::min(p.first, q.first), x1 = std::max(p.first, q.first);
    const auto f0 = static_cast<int32_t>(std::max<double>(fa, std::round(x0))), f1 = static_cast<int32_t>(std::min<double>(fb, std::round(x1)));
    for (int32_t f = f0; f <= f1; ++f) {
      const double t = x1 > x0 ? (f - p.first) / (q.first - p.first) : 1.0;
      vals[static_cast<size_t>(f - fa)] = static_cast<float>(p.second + (q.second - p.second) * std::clamp(t, 0.0, 1.0));
    }
  }
  return waveEdit(a, c, "draw sample", [&](tp::Wave& w) { return tp::waveDraw(w, fa, vals); });
}

// The edits a button or key makes (the header's shared edits, the same undo steps).
bool edit(App& a, Ctx& c, tp::HeaderEdit e) {
  if (!c.have()) return false;
  if (e != tp::HeaderEdit::Copy && e != tp::HeaderEdit::SelectAll && e != tp::HeaderEdit::ClearRange) stopPreview(a);
  return tp::headerEdit(a, c.ch, e);
}

bool canPart(const Ctx& c) {
  const tp::Span r = rangeOf(c);
  return c.have() && !r.empty() && r.b - r.a < c.frames;
}
// 0.7.8: a sample played backwards loses what precedes its loop instead (the part it never reaches).
bool minimisable(const Ctx& c) { return c.have() && c.looping && (c.rev ? c.ls > 0 : c.le < c.frames); }

void openModal(Modal m) {
  st().modal = m;
  st().modalOpen = true;
}

// 0.7.8 (To entry): the range of the selected sample into entry `to` (-1: the next free one), one undo
// step; the new entry is selected and the status line says where it went.
void regionTo(App& a, Ctx& c, int to) {
  if (!c.have()) return;
  stopPreview(a);
  const int k = c.k;
  const int slot = tp::headerRegionToEntry(a, c.ch, to);
  if (slot < 0) return;
  status(std::string(i18n::tr("The range of entry")) + " " + hex2(k + 1) + " " + i18n::tr("is now entry") + " " + hex2(slot + 1) + " (" + keys::fillKeys(i18n::N_("%s: back"), {keys::cmdKey("edit.undo", "Ctrl+Z")}) + ")");
}

// The To entry drop-down: the next free entry, then each entry (a used one asks before it is replaced).
// It opens from its button (b0..b1), as the classic screen's drop-downs do: above it, its left edge on
// the button's, so it never covers the button column it came from; with no room above, beside the
// column at its right, its foot on the button's.
void toEntryMenu(App& a, Ctx& c, ImVec2 b0, ImVec2 b1) {
  if (ImGui::IsPopupOpen("##ttoentrymenu")) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = (trk::kMaxSamples + 1) * (ImGui::GetFontSize() + st.ItemSpacing.y) + st.ItemSpacing.y + 2 * st.WindowPadding.y + 2;
    const float top = ImGui::GetMainViewport()->WorkPos.y;
    if (b0.y - space::kGap - h >= top)
      ImGui::SetNextWindowPos(ImVec2(b0.x, b0.y - space::kGap), ImGuiCond_Always, ImVec2(0, 1));
    else
      ImGui::SetNextWindowPos(ImVec2(b1.x + space::kGap, b1.y), ImGuiCond_Always, ImVec2(0, 1));
  }
  if (!ImGui::BeginPopup("##ttoentrymenu")) return;
  if (!c.have()) {
    ImGui::EndPopup();
    return;
  }
  const tp::Edit& e = c.snap->e;
  const int free = tp::firstFreeEntry(e);
  const std::string next = free >= 0 ? std::string(i18n::tr("Next free entry")) + " (" + hex2(free + 1) + ")" : std::string(i18n::tr("Next free entry (all 16 used)"));
  if (ImGui::MenuItem(next.c_str(), nullptr, false, free >= 0)) regionTo(a, c, -1);
  devMenuRow("Next free entry", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
  ImGui::Separator();
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const bool used = tp::hasSample(e, k);
    const std::string label = std::string(i18n::tr("Entry")) + " " + hex2(k + 1) + (used ? "  " + tp::editName(e, k) : std::string());
    ImGui::PushID(k);
    if (ImGui::MenuItem(label.c_str(), nullptr, false, true)) {
      if (used) {
        st().toEntry = k;
        openModal(Modal::ReplaceEntry);
      } else {
        regionTo(a, c, k);
      }
    }
    devMenuRow(("Entry " + hex2(k + 1)).c_str(), ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    ImGui::PopID();
  }
  ImGui::EndPopup();
}

// ---- K-436: Effects (Gridline's own effects run over the sample, one undo step) ----
constexpr int kMaxFx = 4;
constexpr int kFxParamRows = 5;  // the settings in view: two columns of five

const std::vector<const PluginInfo*>& fxList() {
  static const std::vector<const PluginInfo*> list = tp::sampleEffects();
  return list;
}

std::vector<tp::FxStep> fxChain() {
  std::vector<tp::FxStep> v;
  for (const auto& r : st().fx) v.push_back(tp::FxStep{r.st, r.mix / 100.0});
  return v;
}

// The chain over the range (or the whole sample) of a copy, played at the play note on the sample
// preview: what Apply would make, heard before it is kept.
void fxPreview(App& a, Ctx& c) {
  tp::Wave w;
  if (!c.have() || !tp::waveOf(c.snap->e, c.k, *c.snap->inst, w)) return;
  const tp::Span r0 = rangeOf(c);
  const tp::Span r = r0.empty() ? tp::Span{std::max(0, w.cutA), w.cutB > 0 ? w.cutB : w.frames()} : r0;
  const int32_t before = w.frames();
  if (!tp::waveEffects(w, r, fxChain(), st().fxTail / 10.0, a.doc.song().tempo)) return;
  const int32_t end = std::min<int32_t>(w.frames(), r.b + (w.frames() - before) + static_cast<int32_t>(st().fxTail / 10.0 * w.rate));
  if (end <= r.a) return;
  stopPreview(a);
  auto pv = std::make_shared<Sample>();
  const trk::SampleDef& sd = c.snap->e.text.def.samples[static_cast<size_t>(c.k)];
  pv->sampleRate = w.rate * std::pow(2.0, ((c.m ? c.m->playNote : 49) - tp::rootNote(sd, w.rate)) / 12.0);
  pv->left.assign(w.L.begin() + r.a, w.L.begin() + end);
  if (w.stereo) pv->right.assign(w.R.begin() + r.a, w.R.begin() + end);
  else pv->setMono();
  St& s = st();
  s.pvSample = pv;
  s.pvStarted = ImGui::GetTime();
  a.engine.preview(pv, true);
}

bool fxApply(App& a, Ctx& c) {
  if (st().fx.empty()) return false;
  const tp::Span sel = rangeOf(c);
  const std::vector<tp::FxStep> chain = fxChain();
  const double tail = st().fxTail / 10.0, tempo = a.doc.song().tempo;
  return waveEdit(a, c, "sample effects", [&](tp::Wave& x) { return tp::waveEffects(x, sel, chain, tail, tempo); });
}

// The effect picker (the Add button and each row's name): Gridline's own effects by category.
void fxPicker(App& a) {
  (void)a;
  if (!ImGui::BeginPopup("##tedfxpick")) return;
  St& s = st();
  std::string cat = "\x01";
  bool open = false;
  for (const PluginInfo* p : fxList()) {
    if (p->category != cat) {
      if (open) ImGui::EndMenu();
      cat = p->category;
      open = ImGui::BeginMenu(cat.c_str());
    }
    if (!open) continue;
    if (ImGui::MenuItem(p->name.c_str())) {
      St::FxRow row;
      row.st = defaultPluginState(p->type);
      if (s.fxPick >= 0 && s.fxPick < static_cast<int>(s.fx.size())) {
        s.fx[static_cast<size_t>(s.fxPick)] = row;
        s.fxSel = s.fxPick;
      } else if (static_cast<int>(s.fx.size()) < kMaxFx) {
        s.fx.push_back(row);
        s.fxSel = static_cast<int>(s.fx.size()) - 1;
      }
      s.fxScroll = 0;
    }
  }
  if (open) ImGui::EndMenu();
  ImGui::EndPopup();
}

// The settings grid of one effect: two columns when every name and value fits beside a bar, one
// parameter per row otherwise. Names are never cut short: the label column is the longest name's width.
struct FxGrid {
  int cols = 2, rowsAll = 0;
  bool scroll = false;
  float labW = 0, valW = 0, colW = 0, sbW = 0;
  bool fits = true;  // every name and value has its room (false only past one per row)
};
FxGrid fxGrid(App& a, const PluginInfo& info, const PluginState& st, float width) {
  FxGrid g;
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  float nameW = 0, valNeed = mono->CalcTextSizeA(fs, FLT_MAX, 0, "000.0").x, choiceNeed = 0;
  for (size_t k = 0; k < info.params.size(); ++k) {
    const ParamDesc& d = info.params[k];
    nameW = std::max(nameW, a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(d.name.c_str())).x);
    const float cur = k < st.params.size() ? st.params[k] : static_cast<float>(d.def);
    if (d.kind == ParamKind::Continuous) {
      for (double x : {d.min, d.max, d.def, static_cast<double>(cur)}) valNeed = std::max(valNeed, mono->CalcTextSizeA(fs, FLT_MAX, 0, d.format(x).c_str()).x);
    } else {
      const double hi = d.kind == ParamKind::Toggle ? 1.0 : d.choices.empty() ? d.max : static_cast<double>(d.choices.size() - 1);
      for (double x = 0; x <= hi && x < 256; x += 1) choiceNeed = std::max(choiceNeed, mono->CalcTextSizeA(fs, FLT_MAX, 0, d.format(x).c_str()).x);
    }
  }
  g.labW = std::ceil(nameW) + space::kGroup;
  g.valW = std::ceil(valNeed) + space::kPad;
  const float barMin = std::round(fs * 4), buttons = std::round(fs * 3.6f) + space::kPad;
  const float need = g.labW + std::max(barMin + space::kPad + g.valW, std::ceil(choiceNeed) + 12 + buttons);
  const int n = static_cast<int>(info.params.size());
  for (int cols : {2, 1}) {
    g.cols = cols;
    g.rowsAll = (n + cols - 1) / cols;
    g.scroll = g.rowsAll > kFxParamRows;
    g.sbW = g.scroll ? std::round(fs * 1.1f) : 0;
    const float room = width - (g.scroll ? g.sbW + space::kGroup : 0);
    g.colW = cols == 2 ? std::floor((room - space::kGroup * 2) * 0.5f) : std::floor(room);
    g.fits = need <= g.colW;
    if (g.fits) break;
  }
  return g;
}

void effectsModal(App& a, Ctx& c) {
  St& s = st();
  const auto& P = pal();
  const float fs = ImGui::GetFontSize(), rh = std::round(fs + 7), gap = 3;
  const float w = std::round(fs * 40);
  // caption, the chain's rows and its Add row, the settings title and rows, the tail row; the buttons.
  const float h = std::round(fs * 2.6f + (fs + 6) * 2 + (kMaxFx + 1) * (rh + gap) + space::kGroup * 2 + kFxParamRows * (rh + gap) + (rh + gap) + rh + 28 + fs);
  if (!beginModalBox(a, "##tedmodal", "Effects", ImVec2(w, h))) {
    s.modal = Modal::None;
    return;
  }
  ++s.modalBegun;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* mono = monoFont(a);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float x0 = o.x, x1 = o.x + w - 20;
  float y = o.y + 2;
  const tp::Span sel = rangeOf(c);
  dl->AddText(a.fontRegular, fs, ImVec2(x0, y), P.textDim,
              i18n::tr(sel.empty() ? "Gridline's effects over the whole sample, in order from the top" : "Gridline's effects over the range, in order from the top"));
  y += fs + 6;
  // ---- the chain ----
  if (s.fxSel >= static_cast<int>(s.fx.size())) s.fxSel = std::max(0, static_cast<int>(s.fx.size()) - 1);
  const float numW = std::round(fs * 2.0f), xW = rh, mixW = std::round(fs * 8.5f), presetW = std::round(fs * 14);
  const float nameX = x0 + numW + space::kGap, xX = x1 - xW, mixX = xX - space::kGroup - mixW, presetX = mixX - space::kGroup - presetW;
  const float nameW = presetX - space::kGroup - nameX;
  int remove = -1;
  for (int i = 0; i < static_cast<int>(s.fx.size()); ++i) {
    St::FxRow& row = s.fx[static_cast<size_t>(i)];
    const PluginInfo* info = findPlugin(row.st.type);
    ImGui::PushID(i);
    char num[8];
    std::snprintf(num, sizeof num, "%d", i + 1);
    if (bevelButton(a, "##fxn", num, ImVec2(x0, y), ImVec2(x0 + numW, y + rh), i == s.fxSel, "Show this effect's settings")) s.fxSel = i, s.fxScroll = 0;
    if (bevelButton(a, "##fxname", info ? info->name.c_str() : row.st.type.c_str(), ImVec2(nameX, y), ImVec2(nameX + nameW, y + rh), false,
                    "Another effect in this place")) {
      s.fxPick = i;
      s.fxSel = i;
      ImGui::OpenPopup("##tedfxpick");
    }
    devMenuRow(("Effect " + std::to_string(i + 1)).c_str(), nameX, y, nameX + nameW, y + rh);
    const int np = info ? static_cast<int>(info->presets.size()) : 0;
    const std::string pv = row.preset >= 0 && row.preset < np ? info->presets[static_cast<size_t>(row.preset)].name : std::string(i18n::tr("Default"));
    if (const int d = stepper(a, "##fxpre", "Preset", pv, ImVec2(presetX, y), presetW, rh, "The effect's factory presets", std::round(fs * 3.6f), true); d && info) {
      row.preset = ((row.preset + 1 + d) % (np + 1) + (np + 1)) % (np + 1) - 1;
      row.st = row.preset >= 0 ? presetState(*info, static_cast<size_t>(row.preset)) : defaultPluginState(info->type);
    }
    char mx[16];
    std::snprintf(mx, sizeof mx, "%d %%", row.mix);
    if (const int d = stepper(a, "##fxmix", "Mix", mx, ImVec2(mixX, y), mixW, rh, "How much of the effect is heard (the rest is the sound before it)", std::round(fs * 2.4f), true))
      row.mix = std::clamp(row.mix + d * (ImGui::GetIO().KeyShift ? 10 : 1), 0, 100);
    if (bevelButton(a, "##fxdel", "x", ImVec2(xX, y), ImVec2(x1, y + rh), false, "Take this effect out of the chain")) remove = i;
    ImGui::PopID();
    y += rh + gap;
  }
  if (remove >= 0) {
    s.fx.erase(s.fx.begin() + remove);
    s.fxSel = std::clamp(s.fxSel - (s.fxSel >= remove ? 1 : 0), 0, std::max(0, static_cast<int>(s.fx.size()) - 1));
    s.fxScroll = 0;
  }
  if (s.fx.empty()) {
    dl->AddText(a.fontRegular, fs, ImVec2(x0, std::round(y + (rh - fs) * 0.5f)), P.textFaint, i18n::tr("No effect yet: add one below"));
    y += rh + gap;
  }
  {
    const bool room = static_cast<int>(s.fx.size()) < kMaxFx;
    if (!room) ImGui::BeginDisabled();
    const float bw = std::round(fs * 9);
    if (bevelButton(a, "##fxadd", "Add effect...", ImVec2(x0, y), ImVec2(x0 + bw, y + rh), false, "One of Gridline's own effects at the end of the chain")) {
      s.fxPick = static_cast<int>(s.fx.size());
      ImGui::OpenPopup("##tedfxpick");
    }
    devMenuRow("Add effect", x0, y, x0 + bw, y + rh);
    if (!room) ImGui::EndDisabled();
    char cnt[48];
    std::snprintf(cnt, sizeof cnt, i18n::tr("%d of %d"), static_cast<int>(s.fx.size()), kMaxFx);
    dl->AddText(a.fontRegular, fs, ImVec2(x0 + bw + space::kGroup + 2, std::round(y + (rh - fs) * 0.5f)), P.textFaint, cnt);
  }
  // Rows not used by the chain keep the box's size steady.
  y = o.y + 2 + fs + 6 + (kMaxFx + 1) * (rh + gap) + space::kGroup;
  fxPicker(a);
  dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + 1), P.line);
  y += space::kGroup;
  // ---- the selected effect's settings ----
  St::FxRow* cur = s.fx.empty() ? nullptr : &s.fx[static_cast<size_t>(s.fxSel)];
  const PluginInfo* ci = cur ? findPlugin(cur->st.type) : nullptr;
  {
    const std::string t = ci ? std::string(i18n::tr("Settings of")) + " " + std::to_string(s.fxSel + 1) + "  " + ci->name : std::string(i18n::tr("Settings"));
    boxTitle(a, ImVec2(x0, y), t.c_str());
    y += fs + 6;
  }
  const float gridY = y;
  if (ci) {
    normalizePluginState(cur->st);
    const int n = static_cast<int>(ci->params.size());
    const FxGrid g = fxGrid(a, *ci, cur->st, x1 - x0);
    const int rowsAll = g.rowsAll;
    const bool scroll = g.scroll;
    const float sbW = g.sbW, colW = g.colW, labW = g.labW, valW = g.valW;
    const int first = std::clamp(static_cast<int>(s.fxScroll), 0, std::max(0, rowsAll - kFxParamRows));
    for (int rr = 0; rr < kFxParamRows; ++rr)
      for (int col = 0; col < g.cols; ++col) {
        const int k = (first + rr) * g.cols + col;
        if (k >= n) continue;
        const ParamDesc& d = ci->params[static_cast<size_t>(k)];
        float& v = cur->st.params[static_cast<size_t>(k)];
        const float cx = x0 + col * (colW + space::kGroup * 2), cy = gridY + rr * (rh + gap);
        ImGui::PushID(1000 + k);
        if (d.kind == ParamKind::Continuous) {
          dl->AddText(a.fontRegular, fs, ImVec2(cx, std::round(cy + (rh - fs) * 0.5f)), P.textDim, i18n::tr(d.name.c_str()));
          double out = 0;
          const ImVec2 b0(cx + labW, cy), b1(cx + colW - valW - space::kPad, cy + rh);
          if (valueBar("##fxv", b0, b1, d.toNormalized(v), 0.0, 1.0, false, &out, nullptr)) v = static_cast<float>(d.clamp(d.fromNormalized(out)));
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) v = static_cast<float>(d.def);
          const std::string txt = d.format(v);
          const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, txt.c_str()).x;
          dl->AddText(mono, fs, ImVec2(std::round(std::max(b1.x + space::kPad, cx + colW - tw)), std::round(cy + (rh - fs) * 0.5f)), valueInk(), txt.c_str());
        } else {
          const std::string txt = d.format(v);
          if (const int dd = stepper(a, "##fxc", d.name.c_str(), txt, ImVec2(cx, cy), colW, rh, nullptr, labW, true)) {
            const double hi = d.kind == ParamKind::Toggle ? 1.0 : d.choices.empty() ? d.max : static_cast<double>(d.choices.size() - 1);
            double nv = std::round(v) + dd;
            if (nv > hi) nv = 0;
            if (nv < 0) nv = hi;
            v = static_cast<float>(d.clamp(nv));
          }
        }
        ImGui::PopID();
      }
    if (scroll) {
      double st0 = first;
      if (vScrollBar(a, "##fxscroll", ImVec2(x1 - sbW, gridY), ImVec2(x1, gridY + kFxParamRows * (rh + gap) - gap), &st0, kFxParamRows, rowsAll, 1))
        s.fxScroll = st0;
      if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0 && ImGui::GetIO().MousePos.y >= gridY &&
          ImGui::GetIO().MousePos.y < gridY + kFxParamRows * (rh + gap))
        s.fxScroll = std::clamp(s.fxScroll - ImGui::GetIO().MouseWheel, 0.0, static_cast<double>(rowsAll - kFxParamRows));
    }
  } else {
    dl->AddText(a.fontRegular, fs, ImVec2(x0, std::round(gridY + (rh - fs) * 0.5f)), P.textFaint, i18n::tr("The settings of the effect picked in the chain show here"));
  }
  y = gridY + kFxParamRows * (rh + gap) + space::kGroup;
  // ---- the tail ----
  {
    char tb[24];
    std::snprintf(tb, sizeof tb, "%.1f s", s.fxTail / 10.0);
    const float tw = std::round(fs * 12);
    if (const int d = stepper(a, "##fxtail", "Tail", tb, ImVec2(x0, y), tw, rh, "Time after the sound for echoes and reverb to ring out", std::round(fs * 3.2f), true))
      s.fxTail = std::clamp(s.fxTail + d * (ImGui::GetIO().KeyShift ? 10 : 1), 0, static_cast<int>(tp::kMaxEffectTail * 10));
    dl->AddText(a.fontRegular, fs, ImVec2(x0 + tw + space::kGroup + 2, std::round(y + (rh - fs) * 0.5f)), P.textFaint,
                i18n::tr(sel.empty() ? "the sample grows to hold it" : "it plays over what follows the range"));
  }
  // ---- Preview, Apply, Cancel ----
  const float bw = std::round(fs * 7), by = std::round(o.y + h - rh - 28 - fs);
  bool apply = false;
  const bool any = !s.fx.empty() && c.have();
  if (!any) ImGui::BeginDisabled();
  if (bevelButton(a, "##fxpv", "Preview", ImVec2(x0, by), ImVec2(x0 + bw, by + rh), previewSounding(a), "Hear the result at the play note before it is kept")) fxPreview(a, c);
  if (bevelButton(a, "##mgo", "Apply", ImVec2(x1 - 2 * bw - 6, by), ImVec2(x1 - bw - 6, by + rh), false, "Enter (one undo step)")) apply = true;
  if (!any) ImGui::EndDisabled();
  if (bevelButton(a, "##mno", "Cancel", ImVec2(x1 - bw, by), ImVec2(x1, by + rh), false, "Esc")) {
    stopPreview(a);
    ImGui::CloseCurrentPopup();
    s.modal = Modal::None;
  }
  if (any && !ImGui::IsPopupOpen("##tedfxpick") && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) apply = true;
  keepKeysHere();
  if (apply) {
    stopPreview(a);
    if (fxApply(a, c)) status(std::string(i18n::tr("Effects applied to entry")) + " " + hex2(c.k + 1) + " (" + keys::fillKeys(i18n::N_("%s: back"), {keys::cmdKey("edit.undo", "Ctrl+Z")}) + ")");
    ImGui::CloseCurrentPopup();
    s.modal = Modal::None;
  }
  endModalBox();
}

// The valued edits' and the confirmations' boxes (the classic centred request, in the band's look).
void modals(App& a, Ctx& c) {
  St& s = st();
  if (s.modalOpen) {
    ImGui::OpenPopup("##tedmodal");
    s.modalOpen = false;
  }
  if (s.modal == Modal::None || s.modal == Modal::Convert) return;
  if (s.modal == Modal::Effects) {  // K-436 (a box of its own size)
    effectsModal(a, c);
    return;
  }
  const float fs = ImGui::GetFontSize(), rh = std::round(fs + 7), w = std::round(fs * 26);
  int rows = 2;
  const char* title = "";
  switch (s.modal) {
    case Modal::Volume: title = "Volume", rows = 4; break;
    case Modal::Resample: title = "Resample", rows = 2; break;
    case Modal::Echo: title = "Echo", rows = 5; break;
    case Modal::Mix: title = "Mix in a sample", rows = 3; break;
    case Modal::LoopFade: title = "Loop fade", rows = 2; break;
    case Modal::Remove: title = "Remove the sample", rows = 2; break;
    case Modal::ReplaceEntry: title = "Replace the entry", rows = 2; break;
    default: break;
  }
  const float h = std::round(fs * 2.6f + rows * (rh + 3) + rh + 16);
  if (!beginModalBox(a, "##tedmodal", title, ImVec2(w, h))) {
    s.modal = Modal::None;
    return;
  }
  ++s.modalBegun;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  static tp::PopupValues spare;
  tp::PopupValues& pv = c.m ? c.m->pv : spare;  // (c.m is set whenever a modal can open)
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float x0 = o.x, x1 = o.x + w - 20;
  float y = o.y + 2;
  auto caption = [&](const char* text) {
    dl->AddText(a.fontRegular, fs, ImVec2(x0, y), P.textDim, i18n::tr(text));
    y += fs + 6;
  };
  auto row = [&](const char* id, const char* label, int* v, int lo, int hi, const char* fmt, int big) {
    char b[48];
    std::snprintf(b, sizeof b, fmt, *v);
    const int d = stepper(a, id, label, b, ImVec2(x0, y), x1 - x0, rh, nullptr, std::round(fs * 9), true);
    if (d) *v = std::clamp(*v + d * (ImGui::GetIO().KeyShift ? big : 1), lo, hi);
    y += rh + 3;
  };
  bool apply = false;
  const tp::Span sel = rangeOf(c);
  switch (s.modal) {
    case Modal::Volume:
      caption(sel.empty() ? "Volume from the start to the end of the sample" : "Volume from the start to the end of the range");
      row("##vs", "Start", &pv.rampStart, 0, 400, "%d %%", 10);
      row("##ve", "End", &pv.rampEnd, 0, 400, "%d %%", 10);
      if (bevelButton(a, "##vmax", "Loudest without clipping", ImVec2(x0, y), ImVec2(x1, y + rh), false, "Both ends to the gain that brings the loudest point to full scale")) {
        tp::Wave wv;
        if (c.have() && tp::waveOf(c.snap->e, c.k, *c.snap->inst, wv))
          pv.rampStart = pv.rampEnd = std::clamp(static_cast<int>(std::floor(tp::waveMaxScale(wv, sel) * 100.0)), 0, 400);
      }
      y += rh + 3;
      break;
    case Modal::Resample:
      caption("More or fewer frames for the same pitch (the transpose follows)");
      row("##rs", "Semitones", &pv.resTones, -36, 36, "%+d", 12);
      break;
    case Modal::Echo:
      caption("Repeats of the sample, each quieter than the last");
      row("##en", "Repeats", &pv.echoCount, 1, 32, "%d", 4);
      row("##ed", "Distance", &pv.echoMs, 1, 2000, "%d ms", 50);
      row("##ef", "Each repeat", &pv.echoFeedback, 0, 100, "%d %%", 10);
      if (checkRow(a, "##ex", "Lengthen the sample to hold the repeats", ImVec2(x0, y), ImVec2(x1, y + rh), pv.echoExtend)) pv.echoExtend = !pv.echoExtend;
      y += rh + 3;
      break;
    case Modal::Mix: {
      caption("Another sample of this instrument mixed in from the start");
      if (c.ok() && (pv.mixSlot == c.k || !tp::hasSample(c.snap->e, pv.mixSlot))) {
        pv.mixSlot = -1;
        for (int j = 0; j < trk::kMaxSamples && pv.mixSlot < 0; ++j)
          if (j != c.k && tp::hasSample(c.snap->e, j)) pv.mixSlot = j;
      }
      const std::string cur = pv.mixSlot >= 0 && c.ok() ? hex2(pv.mixSlot + 1) + " " + tp::editName(c.snap->e, pv.mixSlot) : std::string("--");
      if (const int d = stepper(a, "##ms", "Sample", cur, ImVec2(x0, y), x1 - x0, rh, nullptr, std::round(fs * 9), true); d && c.ok())
        for (int i = 1; i < trk::kMaxSamples; ++i) {
          const int j = ((pv.mixSlot + d * i) % trk::kMaxSamples + trk::kMaxSamples) % trk::kMaxSamples;
          if (j != c.k && tp::hasSample(c.snap->e, j)) {
            pv.mixSlot = j;
            break;
          }
        }
      y += rh + 3;
      row("##mb", "Balance", &pv.mixBalance, 0, 100, "%d %%", 10);
      break;
    }
    case Modal::LoopFade:
      caption("The loop's end fades into the sound before its start");
      row("##lf", "Length", &pv.fadeMs, 1, 500, "%d ms", 10);
      break;
    case Modal::Remove: {
      char b[160];
      std::snprintf(b, sizeof b, i18n::tr("Sample %s leaves the instrument; its keys go to the nearest sample left."), hex2(c.k + 1).c_str());
      dl->AddText(a.fontRegular, fs, ImVec2(x0, y), P.text, b);
      y += fs + 6;
      caption("Undo brings it back.");
      break;
    }
    case Modal::ReplaceEntry: {
      const std::string old = c.ok() && s.toEntry >= 0 ? tp::editName(c.snap->e, s.toEntry) : std::string();
      char b[200];
      std::snprintf(b, sizeof b, i18n::tr("Entry %s (%s) becomes the range of entry %s."), hex2(s.toEntry + 1).c_str(), old.c_str(), hex2(c.k + 1).c_str());
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0, y), x1 - x0, P.text, b);
      y += fs + 6;
      caption("Its keys stay. Undo brings it back.");
      break;
    }
    default: break;
  }
  // Apply / Cancel (Enter / Esc).
  const float bw = std::round(fs * 7), by = std::round(o.y + h - rh - 28 - fs);
  const char* go = s.modal == Modal::Remove         ? "Remove"
                   : s.modal == Modal::ReplaceEntry ? "Replace"
                   : s.modal == Modal::Mix          ? "Mix"
                   : s.modal == Modal::Resample     ? "Resample"
                                                    : "Apply";
  if (bevelButton(a, "##mgo", go, ImVec2(x1 - 2 * bw - 6, by), ImVec2(x1 - bw - 6, by + rh), false, "Enter")) apply = true;
  if (bevelButton(a, "##mno", "Cancel", ImVec2(x1 - bw, by), ImVec2(x1, by + rh), false, "Esc")) ImGui::CloseCurrentPopup(), s.modal = Modal::None;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) apply = true;
  keepKeysHere();
  if (apply && c.have()) {
    switch (s.modal) {
      case Modal::Volume: {
        const double g0 = pv.rampStart / 100.0, g1 = pv.rampEnd / 100.0;
        waveEdit(a, c, "sample volume ramp", [&](tp::Wave& x) { return tp::waveRamp(x, sel, g0, g1); });
        break;
      }
      case Modal::Resample: {
        const int t = pv.resTones;
        if (waveEdit(a, c, "resample sample", [&](tp::Wave& x) { return tp::waveResample(x, t); })) c.m->selA = c.m->selB = -1;
        break;
      }
      case Modal::Echo: {
        const auto d = static_cast<int32_t>(std::max(1.0, std::round(pv.echoMs * c.rate / 1000.0)));
        const int n = pv.echoCount;
        const double fb = pv.echoFeedback / 100.0;
        const bool ext = pv.echoExtend;
        waveEdit(a, c, "sample echo", [&](tp::Wave& x) { return tp::waveEcho(x, n, d, fb, ext); });
        break;
      }
      case Modal::Mix:
        if (pv.mixSlot >= 0) {
          tp::Wave other;
          if (tp::waveOf(c.snap->e, pv.mixSlot, *c.snap->inst, other)) {
            const double bal = pv.mixBalance / 100.0;
            waveEdit(a, c, "mix samples", [&](tp::Wave& x) { return tp::waveMix(x, other, bal); });
          }
        }
        break;
      case Modal::LoopFade: {
        const auto f = static_cast<int32_t>(std::max(1.0, std::round(pv.fadeMs * c.rate / 1000.0)));
        if (!waveEdit(a, c, "crossfade sample loop", [&](tp::Wave& x) { return tp::waveCrossfadeLoop(x, f); }))
          status(i18n::tr("The loop needs sound before its start to fade into"));
        break;
      }
      case Modal::Remove:
        stopPreview(a);
        tp::opRemoveSample(a, c.t, c.k);
        c.m->selA = c.m->selB = -1;
        break;
      case Modal::ReplaceEntry:
        regionTo(a, c, s.toEntry);
        s.toEntry = -1;
        break;
      default: break;
    }
    ImGui::CloseCurrentPopup();
    s.modal = Modal::None;
  }  // (closed by Esc: the box's next frame finds it closed and the modal ends there)
  endModalBox();
}

// The edits as menu items (the well's right-click menu, and Process... in narrow windows).
void urlDialog(App& a, ObjectId ch);  // (below: Import from URL into the selected slot)

void editMenuItems(App& a, Ctx& c, bool all) {
  const bool part = canPart(c), hasClip = !tp::clipboard().L.empty();
  const tp::Span r = rangeOf(c);
  if (all) {
    if (ImGui::MenuItem(i18n::tr("Cut"), keys::namedText("Alt+X"), false, part)) edit(a, c, tp::HeaderEdit::Cut);
    if (ImGui::MenuItem(i18n::tr("Copy"), keys::namedText("Alt+C"), false, c.have())) edit(a, c, tp::HeaderEdit::Copy);
    if (ImGui::MenuItem(i18n::tr("Paste"), keys::namedText("Alt+V"), false, c.have() && hasClip)) edit(a, c, tp::HeaderEdit::Paste);
    if (ImGui::MenuItem(i18n::tr("Crop"), keys::namedText("Alt+R"), false, part)) edit(a, c, tp::HeaderEdit::Crop);
    if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, part)) edit(a, c, tp::HeaderEdit::Delete);
    if (ImGui::MenuItem(i18n::tr("Silence"), nullptr, false, c.have() && !r.empty())) edit(a, c, tp::HeaderEdit::Silence);
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Volume..."), nullptr, false, c.have())) openModal(Modal::Volume);
  }
  if (ImGui::MenuItem(i18n::tr("Fade in"), nullptr, false, c.have())) edit(a, c, tp::HeaderEdit::FadeIn);
  if (ImGui::MenuItem(i18n::tr("Fade out"), nullptr, false, c.have())) edit(a, c, tp::HeaderEdit::FadeOut);
  if (all && ImGui::MenuItem(i18n::tr("Normalise"), nullptr, false, c.have())) edit(a, c, tp::HeaderEdit::Normalise);
  if (ImGui::MenuItem(i18n::tr("Reverse"), nullptr, false, c.have())) edit(a, c, tp::HeaderEdit::Reverse);
  if (ImGui::MenuItem(i18n::tr("Fix DC"), nullptr, false, c.have())) edit(a, c, tp::HeaderEdit::FixDc);
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Resample..."), nullptr, false, c.have())) openModal(Modal::Resample);
  if (ImGui::MenuItem(i18n::tr("Echo..."), nullptr, false, c.have())) openModal(Modal::Echo);
  if (ImGui::MenuItem(i18n::tr("Mix in..."), nullptr, false, c.ok() && tp::sampleCountOf(c.snap->e) > 1)) openModal(Modal::Mix);
  if (ImGui::MenuItem(i18n::tr("Loop fade..."), nullptr, false, c.have() && c.looping)) openModal(Modal::LoopFade);
  if (ImGui::MenuItem(i18n::tr("Effects..."), nullptr, false, c.have())) openModal(Modal::Effects);  // K-436
  devMenuRow("Process Effects", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
  if (ImGui::MenuItem(i18n::tr("Cut after loop"), nullptr, false, minimisable(c))) edit(a, c, tp::HeaderEdit::TrimLoop);
  // K-443: a web link's audio into the slot (also the URL... button beside Load... when the row has room).
  if (ImGui::MenuItem(i18n::tr("Import from URL..."), nullptr, false, c.ok())) urlDialog(a, c.ch);
  if (all) {
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Select all"), keys::namedText("Alt+A"), false, c.have())) edit(a, c, tp::HeaderEdit::SelectAll);
    if (ImGui::MenuItem(i18n::tr("Zoom to range"), keys::namedText("Alt+S"), false, c.have() && !r.empty())) zoomToRange(c);
    if (ImGui::MenuItem(i18n::tr("Clear range"), nullptr, false, c.have() && c.m->selA >= 0)) edit(a, c, tp::HeaderEdit::ClearRange);
  }
}

void saveRange(App& a, Ctx& c) {
  if (!c.have()) return;
  tp::Wave w;
  if (!tp::waveOf(c.snap->e, c.k, *c.snap->inst, w)) return;
  const tp::Span r = rangeOf(c);
  const std::string name = tp::editName(c.snap->e, c.k) + (r.empty() ? "" : "-range") + ".wav";
  menus::fileDialog(a, i18n::tr("Save the range as"), true, {".wav"}, name, [&a, w, r](const std::string& p) {
    if (tp::writeWaveFile(p, w, r)) status(i18n::tr("Saved: ") + fs::u8path(p).filename().u8string());
    else a.notify(i18n::tr("The file could not be written"), true);
  });
}

// One editor command from a key (queued by sdlKey, run in the draw).
void runKey(App& a, Ctx& c, const KeyEv& k) {
  const bool ctrl = (k.mods & (KMOD_CTRL | KMOD_GUI)) != 0, alt = (k.mods & KMOD_ALT) != 0, shift = (k.mods & KMOD_SHIFT) != 0;
  const int kc = k.kc;
  if (k.sc == SDL_SCANCODE_ESCAPE || (ctrl && kc == 'w')) {
    close(a);
    return;
  }
  if (ctrl || alt) {
    if (kc == 'a') edit(a, c, tp::HeaderEdit::SelectAll);
    else if (kc == 'x') edit(a, c, tp::HeaderEdit::Cut);
    else if (kc == 'c') edit(a, c, tp::HeaderEdit::Copy);
    else if (kc == 'v') edit(a, c, tp::HeaderEdit::Paste);
    else if (alt && kc == 'r') edit(a, c, tp::HeaderEdit::Crop);
    else if (alt && kc == 's' && c.have()) zoomToRange(c);
    else if (alt && kc == 'z' && c.have()) zoomAbout(c, 2.0, c.m->viewStart + c.m->viewLen * 0.5);
    else if (alt && kc == 'p' && c.have()) startPreview(a, c.ch, *c.snap->inst, c.snap->e, c.k, c.m->playNote, 0, 0);  // (Alt+W is warp mode)
    else if (alt && kc == 'q') stopPreview(a), releaseHeld(a);
    else if (alt && (k.sc == SDL_SCANCODE_UP || k.sc == SDL_SCANCODE_DOWN)) tp::headerStepSample(a, c.ch, k.sc == SDL_SCANCODE_UP ? -1 : 1);
    return;
  }
  if (!c.have()) return;
  if (k.sc == SDL_SCANCODE_DELETE) edit(a, c, tp::HeaderEdit::Delete);
  else if (k.sc == SDL_SCANCODE_LEFT || k.sc == SDL_SCANCODE_RIGHT) scrollBy(c, (k.sc == SDL_SCANCODE_LEFT ? -1 : 1) * c.m->viewLen * (shift ? 1.0 : 0.1));
}

// ---- drawing ----

// A read-only value row: the label, the value at the right in the monospaced face.
void readout(App& a, const char* label, const std::string& value, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const float ty = std::round(p0.y + (h - fs) * 0.5f);
  dl->AddText(a.fontRegular, fs, ImVec2(p0.x + 2, ty), P.textDim, i18n::tr(label));
  const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, value.c_str()).x;
  dl->AddText(mono, fs, ImVec2(std::round(p0.x + w - 4 - tw), ty), valueInk(), value.c_str());
}

// Load...: an audio file picked into the selected slot (one undo step).
void loadDialog(App& a, ObjectId ch) {
  menus::fileDialog(a, i18n::tr("Load a sample"), false, {".wav", ".wave", ".flac", ".mp3", ".ogg", ".wv", ".aif", ".aiff", ".aifc"}, "", [&a, ch](const std::string& p) {
    const Bridge br(classicSampler(a, ch));
    if (packs::trackerpack::headerLoadSample(a, ch, p) < 0) {
      a.notify(i18n::tr("The file has no audio to load"));
    }
  });
}

// URL...: a web link's audio into the selected slot, as Load... puts a file there (Import from URL).
void urlDialog(App& a, ObjectId ch) {
  urlimportui::openFor(a, urlimportui::Target{i18n::tr("Sample editor"), [ch](App& app, const std::string& p, const std::string&) {
                                                const Bridge br(classicSampler(app, ch));
                                                if (packs::trackerpack::headerLoadSample(app, ch, p) < 0) app.notify(i18n::tr("The file has no audio to load"));
                                              }, false});
}

// The drop target over the well: an audio file goes into the selected slot.
void dropTarget(App& a, Ctx& c, ImVec2 p0, ImVec2 p1) {
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl) return;
  std::string file;
  if (pl->IsDataType("GL_FILES") || xdragui::ownsPayload(pl))
    for (const xdrop::Item& it : xdragui::items())
      if (file.empty() && !it.dir && !it.pending && !it.path.empty() && browse::kindOf(it.path) == browse::Kind::Audio) file = it.path;
  if (file.empty()) file = browserui::draggedAudioFile(a, pl);
  if (file.empty()) return;
  if (!ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), ImGui::GetID("##teddrop"))) return;
  if (const ImGuiPayload* x = ImGui::AcceptDragDropPayload(pl->DataType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery)) {
    if (x->Delivery) {
      xdragui::markConsumed();
      stopPreview(a);
      if (tp::headerLoadSample(a, c.ch, file) < 0) a.notify(i18n::tr("The file has no audio to load"));
    } else {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      fg->AddRect(p0, p1, pal().hot, 0, 0, 2.0f);
      const std::string tip = std::string(i18n::tr("Load into this slot: ")) + fs::u8path(file).filename().u8string();
      st().hover = tip;
    }
  }
  ImGui::EndDragDropTarget();
}

// ---- K-391: a Sampler's two Sample editor views ----
// "Sample editor" (the classic one) | "Advanced" (the Audio clip window's parts): a pair of the band's
// buttons where the title stands, the shown one lit. One click switches; the instrument, its sample
// and the selection stay (the range in the classic one is the advanced page's time selection on the
// clip, and back). The choice is remembered (the app setting "tracker.sampleView").
void switchView(App& a, ObjectId ch, bool advanced) {
  if (advancedView(a) == advanced) return;
  stopPreview(a);
  releaseHeld(a);
  namespace tpg = clipeditor::trackerpage;
  const Song& song = a.doc.song();
  tp::Memory& m = tp::memory(PluginTarget{ch, -1}.key());
  tpg::take(a, ch);
  double t0 = 0, t1 = 0;
  const ObjectId clip = tpg::selection(&t0, &t1);
  const Clip* cl = clip != kNoId ? song.clip(clip) : nullptr;
  if (cl && advanced) {
    if (m.selSlot == 0 && m.selA >= 0 && m.selB >= 0 && m.selA != m.selB) {
      // The tick where the clip reaches file frame f (the clip's warp is monotonic): a bisection.
      const double c0 = static_cast<double>(cl->start), c1 = static_cast<double>(cl->start + song.clipLength(*cl));
      auto tickOf = [&](int64_t f) {
        double lo = c0, hi = c1;
        for (int i = 0; i < 48 && hi - lo > 0.01; ++i) {
          const double mid = (lo + hi) * 0.5;
          int64_t fa = 0, fb = 0;
          if (clipedit::clipRangeFrames(song, clip, c0, mid, fa, fb) && fb >= f) hi = mid;
          else lo = mid;
        }
        return hi;
      };
      const double a0 = tickOf(std::min(m.selA, m.selB)), a1 = tickOf(std::max(m.selA, m.selB));
      if (a1 > a0) tpg::setSelection(a0, a1);
    }
  } else if (cl && t1 > t0) {
    int64_t fa = 0, fb = 0;
    if (clipedit::clipRangeFrames(song, clip, t0, t1, fa, fb) && fb > fa) {
      m.sel = 0, m.selSlot = 0;
      m.selA = static_cast<int32_t>(std::min<int64_t>(fa, 0x3FFFFFFF)), m.selB = static_cast<int32_t>(std::min<int64_t>(fb, 0x3FFFFFFF));
    }
  }
  a.settings.set("tracker.sampleView", advanced ? std::string("advanced") : std::string("classic"));
}
// The pair at the left of the title row [p0, p0 + titleH): the x where the title's text goes on.
float viewTabs(App& a, ObjectId ch, ImVec2 p0, float titleH) {
  const float fs = ImGui::GetFontSize();
  const bool adv = advancedView(a);
  const char* names[2] = {i18n::N_("Sample editor"), i18n::N_("Advanced")};
  const char* hints[2] = {i18n::N_("The classic sample editor: the waveform, its range and loop, the edits and the play buttons"),
                          i18n::N_("The advanced sample editor: the sample's and the channel's settings in pages of modules, as the Audio clip window has them")};
  float x = p0.x + 3;
  for (int i = 0; i < 2; ++i) {
    const float w = std::round(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(names[i])).x + 2 * space::kGroup);
    ImGui::PushID(i);
    if (bevelButton(a, "##tview", names[i], ImVec2(x, p0.y + 2), ImVec2(x + w, p0.y + titleH - 2), adv == (i == 1), hints[i]))
      a.later([&a, ch, i] { switchView(a, ch, i == 1); });
    devMenuRow(i ? "View Advanced" : "View Sample editor", x, p0.y + 2, x + w, p0.y + titleH - 2);
    ImGui::PopID();
    x += w + 2;
  }
  return std::round(x + space::kGroup);
}

void sampleScreen(App& a, ObjectId ch, ImVec2 p0, ImVec2 p1) {
  St& s = st();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Aliased hard(dl);  // cp21: the wave and its marks in hard pixel lines
  ImGuiIO& io = ImGui::GetIO();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  Ctx c = context(a, ch);
  s.hover.clear();
  s.tapLines.clear();
  // Keys queued since the last frame.
  if (!s.keys.empty()) {
    std::vector<KeyEv> ks;
    ks.swap(s.keys);
    for (const KeyEv& k : ks) {
      if (!shown(a)) break;
      runKey(a, c, k);
      c = context(a, ch);  // (an edit changed the instrument)
    }
    if (!shown(a)) return;
  }
  const Channel* chan = channelOf(a, ch);
  const float W = p1.x - p0.x;
  // Rows, top to bottom: the title, the well, the scroll bar, the button panel (three rows), the status line.
  const float titleH = std::round(fs + 8), rowH = std::round(fs + 7), gap = 2, sbH = std::round(fs + 1), statusH = std::round(fs + 6);
  const float panelH = 3 * rowH + 2 * gap + 8;
  const float wellY0 = p0.y + titleH + 1, wellY1 = std::max(wellY0 + 40, p1.y - statusH - panelH - sbH - 3);
  // ---- the title ----
  {
    bevelBox(dl, p0, ImVec2(p1.x, p0.y + titleH), false);
    const float ty = std::round(p0.y + (titleH - fs) * 0.5f);
    float x = 0;
    if (isSampler(chan)) {  // K-391: a Sampler's: the pair of views where the title stands
      x = viewTabs(a, ch, p0, titleH);
    } else {
      dl->AddText(a.fontBold ? a.fontBold : a.fontRegular, fs, ImVec2(p0.x + 8, ty), P.text, i18n::tr("Sample editor"));
      x = p0.x + 16 + (a.fontBold ? a.fontBold : a.fontRegular)->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Sample editor")).x;
    }
    const int ci = chan ? a.doc.song().channelIndex(ch) : -1;
    std::string info = std::string(i18n::tr("Instrument")) + " " + hex2(ci + trackBase(a)) + " " + (chan ? chan->name : std::string());
    if (c.have()) {
      // cp21: built as a string (a long sample name was cut by the fixed buffer before).
      char rate[32];
      std::snprintf(rate, sizeof rate, "%.1f kHz", c.rate / 1000.0);
      info += std::string("   ") + i18n::tr("Sample") + " " + hex2(c.k + 1) + " " + tp::editName(c.snap->e, c.k) + "   " + rate + "   " +
              (c.eight ? "8-bit" : "16-bit") + "   " + (c.sp->mono() ? i18n::tr("mono") : i18n::tr("stereo")) + "   " + std::to_string(c.frames) + " " +
              i18n::tr("frames");
    } else if (c.ok() && c.entry >= 0 && c.k < 0) {
      info += std::string("   ") + i18n::tr("Entry") + " " + hex2(c.entry + 1) + "   " + i18n::tr("empty");
      if (c.snap->sound > 0 && tp::sampleCountOf(c.snap->e) == 0) info += std::string("   ") + i18n::tr("Built-in sound: ") + tp::soundName(c.snap->sound);
    } else if (c.ok() && c.snap->sound > 0) {
      info += std::string("   ") + i18n::tr("Built-in sound: ") + tp::soundName(c.snap->sound);
    }
    // Right: the sample stepper.
    const float stW = std::round(fs * 12);
    const float sx = p1.x - stW - 6;
    theme::textEllipsis(dl, mono, fs, ImVec2(x, ty), sx - x - 8, valueInk(), info.c_str());
    if (c.ok()) {
      const int n = tp::sampleCountOf(c.snap->e);
      int idx = 0;
      for (int k = 0; k <= c.k && k < trk::kMaxSamples; ++k) idx += tp::hasSample(c.snap->e, k) ? 1 : 0;
      char b[32];
      std::snprintf(b, sizeof b, "%02X / %02X", c.have() ? c.k + 1 : c.entry >= 0 && c.k < 0 ? c.entry + 1 : 0, n);
      (void)idx;
      if (const int d = stepper(a, "##tsmp", "Sample", b, ImVec2(sx, p0.y + 2), stW, titleH - 4, "The instrument's previous / next sample (Alt+Up / Alt+Down)",
                                std::round(fs * 4), true))
        tp::headerStepSample(a, ch, -d);
    }
  }
  // ---- the well ----
  const ImVec2 w0(p0.x, wellY0), w1(p1.x, wellY1);
  bevelBox(dl, w0, w1, true);
  const float x0 = w0.x + 2, x1 = w1.x - 2, y0 = w0.y + 2, y1 = w1.y - 2;
  const float Wd = std::max(1.0f, x1 - x0);
  s.well[0] = x0, s.well[1] = y0, s.well[2] = x1, s.well[3] = y1;  // (dev: the dump prints it)
  const float mid = std::round((y0 + y1) * 0.5f), amp = (y1 - y0) * 0.5f - 3;
  if (c.ok()) dropTarget(a, c, w0, w1);
  if (!c.have()) {
    const char* t1 = !c.ok() ? i18n::tr("Not a Tracker Sampler") : c.k >= 0 ? i18n::tr("The sample file is missing") : i18n::tr("No sample yet");
    const char* t2 = i18n::tr("Click here or Load... to pick a file for this slot; or drop an audio file here (the browser, the desktop)");
    // 0.7.8: an empty entry picked in the list says which, and how a sound gets into it.
    std::string emptyTitle;
    const bool emptyEntry = c.ok() && c.k < 0 && c.entry >= 0;
    if (emptyEntry) {
      emptyTitle = std::string(i18n::tr("Entry")) + " " + hex2(c.entry + 1) + " " + i18n::tr("is empty");
      t1 = emptyTitle.c_str();
      t2 = i18n::tr("Load a sound into it: Load sample..., the Files page, a double-click in the Browser, or drop an audio file here");
    }
    // cp20 (K-316 7.2): a file a collaborator sent that is still arriving: how far it is (edits wait for it).
    std::string recv;
    if (c.ok() && c.k >= 0) {
      const trk::InstrumentText& tx = c.snap->e.text;
      const int fi = tx.fileOf[static_cast<size_t>(c.k)];
      float pr = 0;
      if (fi >= 0 && fi < static_cast<int>(tx.files.size()) && collabui::fileLoading(a, tx.files[static_cast<size_t>(fi)], &pr)) {
        recv = std::string(i18n::tr("Receiving the sample...")) + " " + std::to_string(static_cast<int>(std::lround(std::clamp(pr, 0.0f, 1.0f) * 100.0f))) + " %";
        t1 = recv.c_str();
        t2 = i18n::tr("A collaborator's file is on its way; the edits wait for it");
      }
    }
    theme::centreText(dl, a.fontBold ? a.fontBold : a.fontRegular, fs, ImVec2(x0, mid - fs - 2), ImVec2(x1, mid - 2), P.text, t1);
    theme::centreText(dl, a.fontRegular, fs, ImVec2(x0, mid + 2), ImVec2(x1, mid + fs + 2), P.textDim, t2);
    // cp20 (usability pass): a click on the empty well picks the file, as Load... does (not while one is arriving).
    if (c.ok() && recv.empty()) {
      // 0.7.8: a plain button under the text (submitted first, so it takes the hover over the well's).
      const float bw = std::round(fs * 10), bh = std::round(fs + 8), bx = std::round((x0 + x1 - bw) * 0.5f), by = std::round(mid + fs + 10);
      if (by + bh < y1 - 2) {
        if (bevelButton(a, "##twellbtn", "Load sample...", ImVec2(bx - std::round(bw * 0.5f) - 4, by), ImVec2(bx + std::round(bw * 0.5f) - 4, by + bh), false,
                        "Pick an audio file for this entry"))
          loadDialog(a, ch);
        if (bevelButton(a, "##twellurl", "From URL...", ImVec2(bx + std::round(bw * 0.5f) + 4, by), ImVec2(bx + std::round(bw * 1.5f) + 4, by + bh), false,
                        "Import from URL: a web link's audio into this entry"))
          urlDialog(a, ch);
      }
      ImGui::SetCursorScreenPos(ImVec2(x0, y0));
      if (ImGui::InvisibleButton("##twellload", ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, y1 - y0)))) {
        loadDialog(a, ch);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        s.hover = i18n::tr("Click: pick an audio file for this slot (Load...)");
      }
    }
  } else {
    tp::Memory& m = *c.m;
    auto xOf = [&](double f) { return x0 + static_cast<float>((f - m.viewStart) / m.viewLen * Wd); };
    auto fOf = [&](float x) { return m.viewStart + static_cast<double>(x - x0) / Wd * m.viewLen; };
    auto frameAt = [&](float x) { return static_cast<int32_t>(std::lround(std::clamp(fOf(x), 0.0, static_cast<double>(c.frames)))); };
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    // The loop's area, tinted; the range, inverted (a light band the wave shows dark on).
    if (c.looping) dl->AddRectFilled(ImVec2(std::max(x0, xOf(c.ls)), y0), ImVec2(std::min(x1, xOf(c.le)), y1), theme::withAlpha(P.hot, 0.13f));
    const tp::Span r = rangeOf(c);
    float ra = 0, rb = 0;
    if (!r.empty()) {
      ra = std::max(x0, std::floor(xOf(r.a))), rb = std::min(x1, std::max(std::ceil(xOf(r.b)), ra + 1));
      dl->AddRectFilled(ImVec2(ra, y0), ImVec2(rb, y1), theme::mix(P.textDim, P.text, 0.35f));
    } else if (m.selSlot == c.k && m.selA >= 0) {  // the place a paste goes
      const float cx = std::round(xOf(m.selA)) + 0.5f;
      for (float y = y0 + 2; y < y1 - 2; y += 6) dl->AddLine(ImVec2(cx, y), ImVec2(cx, std::min(y + 3, y1 - 2)), theme::withAlpha(P.text, 0.65f));
    }
    dl->AddLine(ImVec2(x0, mid), ImVec2(x1, mid), theme::withAlpha(P.textFaint, 0.45f));
    // The wave: one column per pixel from the cache.
    {
      const tp::PeakPyramid& pk = tp::peaksFor(c.sp);
      const float* L = c.sp->left.data();
      const float* R = c.sp->mono() ? nullptr : c.sp->right.data();
      const int width = static_cast<int>(Wd);
      s.cols.update(pk, L, R, tp::ColumnKey{c.sp.get(), m.viewStart, m.viewLen, width, c.eight});
      const ImU32 ink = P.hot, inkSel = P.surface950;
      const bool fine = m.viewLen < width;  // zoomed past a frame a pixel: a line through the values
      float lastY = 0;
      bool haveLast = false;
      for (int px = 0; px < width; ++px) {
        const float lo = s.cols.lo[static_cast<size_t>(px)], hi = s.cols.hi[static_cast<size_t>(px)];
        if (lo > hi) {
          haveLast = false;
          continue;
        }
        const float x = x0 + static_cast<float>(px);
        const bool inSel = !r.empty() && x >= ra && x < rb;
        const ImU32 col = inSel ? inkSel : ink;
        const float yh = std::round(mid - std::clamp(hi, -1.0f, 1.0f) * amp), yl = std::round(mid - std::clamp(lo, -1.0f, 1.0f) * amp);
        if (fine) {
          const float yv = yh;
          if (haveLast) dl->AddLine(ImVec2(x - 0.5f, lastY), ImVec2(x + 0.5f, yv), col);
          else dl->AddRectFilled(ImVec2(x, yv), ImVec2(x + 1, yv + 1), col);
          lastY = yv, haveLast = true;
        } else {
          dl->AddRectFilled(ImVec2(x, yh), ImVec2(x + 1, yl + 1), col);
        }
      }
    }
    // What is cut away: shaded.
    const ImU32 shade = theme::withAlpha(P.surface950, 0.62f);
    if (c.s0 > 0) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x0, xOf(c.s0)), y1), shade);
    if (c.s1 < c.frames) dl->AddRectFilled(ImVec2(std::min(x1, xOf(c.s1)), y0), ImVec2(x1, y1), shade);
    // The play position of every voice playing this sample (the play tap).
    int lines = 0;
    s.tapLines.clear();
    if (c.snap->inst) {
      tp::TapVoice v[tp::kTapVoices];
      const int n = tp::readPlayTap(c.snap->inst.get(), v, tp::kTapVoices);
      for (int i = 0; i < n; ++i) {
        if (v[i].slot != c.k) continue;
        // (0.7.8: the tap gives the frame the voice reads: on a ping-pong loop's way back the line walks back)
        const float x = std::round(xOf(c.s0 + v[i].pos)) + 0.5f;
        if (x < x0 || x > x1) continue;
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), P.led, 2.0f);
        s.tapLines.emplace_back(x, c.s0 + v[i].pos);
        ++lines;
      }
    }
    (void)lines;
    // The pins and handles.
    const float pin = std::round(std::max(6.0f, fs * 0.55f));
    const ImVec2 mouse = io.MousePos;
    const bool inWell = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1;
    int hot = -1;
    if (s.drag == 0 && inWell) {
      auto near = [&](double f) { return std::abs(mouse.x - xOf(f)) <= pin + 1; };
      if (c.looping && near(c.ls) && mouse.y <= y0 + pin * 2 + 2) hot = 2;
      else if (c.looping && near(c.le) && mouse.y >= y1 - pin * 2 - 2) hot = 3;
      else if (near(c.s0) && std::abs(mouse.y - mid) <= pin + 2) hot = 0;
      else if (near(c.s1) && std::abs(mouse.y - mid) <= pin + 2) hot = 1;
      // 0.7.8 (user report: the loop markers and the ping-pong area could not be dragged): a loop
      // line is taken anywhere along it, not only by its pin, and the loop's strip at the top or the
      // bottom of the well, between its lines, moves the whole loop (its length kept).
      else if (c.looping && near(c.ls)) hot = 2;
      else if (c.looping && near(c.le)) hot = 3;
      else if (c.looping && mouse.x > xOf(c.ls) && mouse.x < xOf(c.le) && (mouse.y <= y0 + pin * 2 + 2 || mouse.y >= y1 - pin * 2 - 2)) hot = 4;
    }
    auto handle = [&](int mk) {
      const bool on = hot == mk || (s.drag == 2 && s.marker == mk) || (mk >= 2 && (hot == 4 || (s.drag == 2 && s.marker == 4)));
      if (mk >= 2) {
        if (!c.looping) return;
        const float x = std::round(xOf(mk == 2 ? c.ls : c.le)) + 0.5f;
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), theme::withAlpha(P.hot, on ? 1.0f : 0.8f), on ? 2.0f : 1.0f);
        if (mk == 2) dl->AddTriangleFilled(ImVec2(x - pin, y0), ImVec2(x + pin, y0), ImVec2(x, y0 + pin * 1.4f), on ? P.text : P.hot);
        else dl->AddTriangleFilled(ImVec2(x - pin, y1), ImVec2(x + pin, y1), ImVec2(x, y1 - pin * 1.4f), on ? P.text : P.hot);
      } else {
        const double f = mk == 0 ? c.s0 : c.s1;
        if ((mk == 0 && c.s0 == 0) && !on && hot != 0) return;  // (at the file's edges the handles stay out of the way until hovered)
        if ((mk == 1 && c.s1 == c.frames) && !on && hot != 1) return;
        const float x = std::round(xOf(f)) + 0.5f;
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), theme::withAlpha(P.text, on ? 0.9f : 0.5f));
        const float d = pin * 0.9f;
        dl->AddQuadFilled(ImVec2(x, mid - d), ImVec2(x + d, mid), ImVec2(x, mid + d), ImVec2(x - d, mid), on ? P.hot : P.text);
      }
    };
    for (int mk = 0; mk < 4; ++mk) handle(mk);
    // Readouts over the well: the range at the top right, the pointer's frame at the top left.
    char b[160];
    if (!r.empty()) {
      std::snprintf(b, sizeof b, i18n::tr("Range %d to %d   %d frames   %.1f ms"), r.a, r.b, r.b - r.a, (r.b - r.a) * 1000.0 / c.rate);
      const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, b).x;
      dl->AddText(mono, fs, ImVec2(x1 - tw - 8, y0 + 4), P.text, b);
    }
    dl->PopClipRect();
    // ---- the well's mouse ----
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton("##twell", ImVec2(Wd, std::max(1.0f, y1 - y0)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hov = ImGui::IsItemHovered();
    if (hot == 4 || (s.drag == 2 && s.marker == 4)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if (hot >= 0 || s.drag == 2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const int32_t f = frameAt(mouse.x);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {  // the loop (on it) or the whole sample
        m.selSlot = c.k;
        if (c.looping && f >= c.ls && f < c.le) m.selA = c.ls, m.selB = c.le;
        else m.selA = 0, m.selB = c.frames;
        s.drag = 0;
      } else if (hot >= 0) {
        s.drag = 2, s.marker = hot;
        s.loopGrab = f - c.ls;
      } else {
        s.drag = 1;
        const tp::Span cur = rangeOf(c);
        if (io.KeyShift && m.selSlot == c.k && m.selA >= 0) {  // extends the range from its far end
          const int32_t a0 = cur.empty() ? m.selA : (std::abs(f - cur.a) > std::abs(f - cur.b) ? cur.a : cur.b);
          m.selA = a0, m.selB = f;
        } else {
          m.selSlot = c.k, m.selA = m.selB = f;
        }
      }
    }
    if (s.drag == 1) {
      if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m.selB = frameAt(mouse.x);
        // Dragged past an edge: the view follows.
        if (mouse.x > x1) scrollBy(c, m.viewLen * 0.02);
        if (mouse.x < x0) scrollBy(c, -m.viewLen * 0.02);
      } else {
        s.drag = 0;
      }
    }
    if (s.drag == 2) {
      if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int32_t f = frameAt(mouse.x) - (s.marker == 4 ? s.loopGrab : 0);
        if (io.KeyAlt) f = zeroCrossing(c, f);
        tp::opSampleMarker(a, c.t, c.k, s.marker, f, c.frames);
      } else {
        s.drag = 0, s.marker = -1;
        a.doc.endGesture();
      }
    }
    // cp20: the pencil: a right-drag draws the sample's values (a line between the pointer's points),
    // one undo step on release; a right-click without a drag opens the edits menu.
    if (ImGui::IsItemActivated() && io.MouseDown[ImGuiMouseButton_Right] && !io.MouseDown[ImGuiMouseButton_Left] && s.drag == 0) {
      s.drag = 3;
      s.pencil.clear();
    }
    if (s.drag == 3) {
      if (ImGui::IsItemActive() && io.MouseDown[ImGuiMouseButton_Right]) {
        if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] >= 16) {
          const double f = std::clamp(fOf(std::clamp(mouse.x, x0, x1)), 0.0, static_cast<double>(c.frames - 1));
          const float v = std::clamp((mid - mouse.y) / std::max(1.0f, amp), -1.0f, 1.0f);
          if (s.pencil.empty()) {
            const ImVec2 cp = io.MouseClickedPos[ImGuiMouseButton_Right];
            s.pencil.emplace_back(std::clamp(fOf(std::clamp(cp.x, x0, x1)), 0.0, static_cast<double>(c.frames - 1)),
                                  std::clamp((mid - cp.y) / std::max(1.0f, amp), -1.0f, 1.0f));
          }
          if (s.pencil.back().first != f || s.pencil.back().second != v) {
            s.pencil.emplace_back(f, v);
          }
        }
        if (s.pencil.size() >= 2) {
          dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
          for (size_t i = 1; i < s.pencil.size(); ++i) {
            const auto& p = s.pencil[i - 1];
            const auto& q = s.pencil[i];
            dl->AddLine(ImVec2(xOf(p.first), mid - p.second * amp), ImVec2(xOf(q.first), mid - q.second * amp), P.text, 2.0f);
          }
          dl->PopClipRect();
        }
      } else {
        const std::vector<std::pair<double, float>> pts = std::move(s.pencil);
        s.pencil.clear();
        s.drag = 0;
        if (pts.size() >= 2) {
          pencilCommit(a, c, pts);
        }
      }
    }
    if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16) ImGui::OpenPopup("##tedmenu");
    if (hov) {
      if (io.MouseWheel != 0 && !io.KeyShift) zoomAbout(c, io.MouseWheel > 0 ? 0.8 : 1.25, fOf(mouse.x));
      const float wx = io.MouseWheelH != 0 ? io.MouseWheelH : io.KeyShift ? io.MouseWheel : 0.0f;
      if (wx != 0) scrollBy(c, -wx * m.viewLen * 0.1);
      if (io.MouseWheel != 0 || io.MouseWheelH != 0) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY), ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
      const int32_t f = frameAt(mouse.x);
      const float v = f < c.frames ? c.sp->left[static_cast<size_t>(f)] : 0.0f;
      char hb[200];
      const char* what = hot == 2 ? i18n::tr("drag: the loop start (Alt: to a zero crossing)")
                         : hot == 3 ? i18n::tr("drag: the loop end (Alt: to a zero crossing)")
                         : hot == 0 ? i18n::tr("drag: the sample's start")
                         : hot == 1 ? i18n::tr("drag: the sample's end")
                                    : i18n::tr("drag: a range (Shift: extend) · double-click: all or the loop · right-drag: draw · right-click: edits · wheel: zoom");
      std::snprintf(hb, sizeof hb, "%s %d   %.1f ms   %s %+.3f   %s", i18n::tr("Frame"), f, f * 1000.0 / c.rate, i18n::tr("value"), static_cast<double>(v), what);
      s.hover = hb;
    }
    if (ImGui::BeginPopup("##tedmenu")) {
      editMenuItems(a, c, true);
      ImGui::EndPopup();
    }
  }
  // ---- the scroll bar ----
  {
    const ImVec2 b0(p0.x, wellY1 + 1), b1(p1.x, wellY1 + 1 + sbH);
    if (c.have()) {
      double start = c.m->viewStart;
      if (scrollBar(a, "##tsb", b0, b1, &start, c.m->viewLen, c.frames, c.m->viewLen * 0.1, "The part of the sample in view (Shift+wheel, Left / Right scroll too)"))
        c.m->viewStart = start;
    } else {
      bevelBox(dl, b0, b1, true);
    }
  }
  // ---- the button panel ----
  const float py0 = wellY1 + sbH + 3;
  bevelBox(dl, ImVec2(p0.x, py0), ImVec2(p1.x, py0 + panelH), false);
  const float ry0 = py0 + 4;
  auto rowY = [&](int r) { return std::round(ry0 + static_cast<float>(r) * (rowH + gap)); };
  // The columns. Each has a least width, from its own labels (a label never cut short), and a share
  // of the rest (in text widths). As the classic screen re-flows to its room: a window short of width
  // folds the process columns into one menu button and the readouts into one column (narrow); shorter
  // still, the range and view columns into one (Select all, Zoom..., Clear range) and the clipboard
  // columns into one (Copy, Paste, Edit...), the folded buttons in the menus (tight); shortest, those
  // two into one (Select all, Zoom..., Edit...).
  struct Col {
    int id;
    float units;
    float least = 0;
    float x = 0, w = 0;
  };
  auto textW = [&](const char* l) { return a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr(l)).x; };
  auto buttonsW = [&](std::initializer_list<const char*> ls) {
    float w = 0;
    for (const char* l : ls) w = std::max(w, textW(l));
    return std::ceil(w + 2 * space::kPad + 2);
  };
  const float radioPad = 2 * std::max(3.0f, std::round(fs * 0.3f)) + 12 + space::kPad;  // (radioRow's diamond and gaps)
  auto radiosW = [&](std::initializer_list<const char*> ls) {
    float w = 0;
    for (const char* l : ls) w = std::max(w, textW(l));
    return std::ceil(w + radioPad);
  };
  const float arrowsW = 2 * std::round(rowH * 0.8f) + 2;  // (a stepper's two arrows at their least)
  auto monoW = [&](const char* t) { return mono->CalcTextSizeA(fs, FLT_MAX, 0, t).x; };
  // The loop steppers' values: as many digits as the sample's length has (at least three).
  const std::string digits(std::max<size_t>(3, std::to_string(std::max(0, c.have() ? c.frames : 0)).size()), '0');
  auto leastOf = [&](int id, bool narrowCols) -> float {
    switch (id) {
      case 0:
        return std::max(std::round(fs * 2.6f) + monoW("C#4") + 6 + arrowsW, buttonsW({"Stop"}));
      case 1: return buttonsW({"Wave", "Range", "View"});
      case 2: return buttonsW({"Select all", "Zoom to range", "Clear range"});
      case 3: return buttonsW({"Zoom in", "Zoom out", "Whole sample"});
      case 4: return buttonsW({"Cut", "Copy", "Paste"});
      case 5: return buttonsW({"Crop", "Delete", "Silence"});
      case 6: return buttonsW({"Volume...", "Fade in", "Fade out"});
      case 7: return buttonsW({"Normalise", "Reverse", "Fix DC"});
      case 8: return buttonsW({"Resample...", "Echo...", "Mix in..."});
      case 9: return buttonsW({"Loop fade...", "Cut after loop", "Save range..."});
      case 17: return buttonsW({"Effects..."});  // K-436
      case 10: return radiosW({"No loop", "Forward", "Ping-pong"});
      case 16: return std::ceil(textW("Backwards") + std::max(8.0f, std::round(fs * 0.8f)) + 11 + 2 * space::kPad);  // 0.7.8 (checkRow's box and gaps)
      case 11: return std::max(radiosW({"16-bit", "8-bit"}), buttonsW({"Load..."}));  // (URL... joins Load... when the column has room)
      case 12: return labelColumn(a, {"Length", "Start", "End"}, space::kGroup) + monoW("00000000") + 6;
      case 13:
        return std::max(labelColumn(a, {"Loop start", "Loop length"}, space::kPad) + monoW(digits.c_str()) + 8 + arrowsW,
                        labelColumn(a, {narrowCols ? "Length" : "View"}, space::kGroup) + monoW(narrowCols ? "00000000" : "0 - 00000000") + 6);
      case 14: return buttonsW({"Exit", "Remove", "Minimise"});
      case 15: return buttonsW({"To entry", "Duplicate", "All keys"});
      case 20: return buttonsW({"Process...", "Volume...", "Normalise"});
      case 21: return buttonsW({"Copy", "Paste", "Edit..."});
      case 22: return buttonsW({"Select all", "Zoom...", "Clear range"});
      case 23: return buttonsW({"Select all", "Zoom...", "Edit..."});
      default: return 0;
    }
  };
  // 0 wide, 1 narrow, 2 tight, 3 shortest: the first that fits (the shortest when none does).
  auto build = [&](int fold) {
    std::vector<Col> v;
    auto add = [&](int id, float u) { v.push_back(Col{id, u, leastOf(id, fold >= 1)}); };
    add(0, 7.5f);  // play note, stop
    add(1, 4.6f);  // wave, range, view
    if (fold >= 3) {
      add(23, 6.0f);  // select all, zoom..., edit...
    } else if (fold >= 2) {
      add(22, 6.0f);  // select all, zoom..., clear range
      add(21, 4.4f);  // copy, paste, edit...
    } else {
      add(2, 7.2f);  // select all, zoom to range, clear range
      add(3, 6.8f);  // zoom in, zoom out, whole sample
      add(4, 4.0f);  // cut, copy, paste
      add(5, 4.6f);  // crop, delete, silence
    }
    add(15, 5.2f);  // 0.7.8: to entry, duplicate, all keys
    if (fold >= 1) {
      add(20, 6.2f);  // process..., volume..., normalise
    } else {
      add(6, 5.6f);  // volume..., fade in, fade out
      add(7, 5.4f);  // normalise, reverse, fix dc
      add(8, 6.4f);  // resample..., echo..., mix in...
      add(9, 7.6f);  // loop fade..., cut after loop, save range...
      add(17, 5.2f);  // K-436: effects... (one button the column's height)
    }
    add(10, 6.6f);  // loop type
    if (fold < 2) add(16, 5.4f);  // 0.7.8: backwards (tight: in the Process... menu)
    add(11, 5.2f);  // 16-bit, 8-bit, load...
    if (fold == 0) add(12, 7.6f);  // length, start, end
    add(13, 11.0f);  // loop start, loop length, view
    add(14, 5.2f);   // exit, remove, minimise
    return v;
  };
  const float avail0 = W - 8;
  int fold = W < fs * 96 ? 1 : 0;  // (as before: the process columns fold below 96 text widths)
  std::vector<Col> cols = build(fold);
  auto leastSum = [&](const std::vector<Col>& v) {
    float t = static_cast<float>(v.size() - 1) * 3;
    for (const Col& cl : v) t += cl.least;
    return t;
  };
  while (fold < 3 && leastSum(cols) > avail0) cols = build(++fold);
  const bool narrow = fold >= 1;
  {
    // The room shared by the text widths, no column under its least and the steppers' column at most
    // 250 px: the columns held at a bound take it, the rest share what is left (a few passes settle it).
    const float avail = avail0 - static_cast<float>(cols.size() - 1) * 3;
    std::vector<int> held(cols.size(), 0);  // 0 free, 1 at its least, 2 at the cap
    float k = 0;
    for (int pass = 0; pass < 8; ++pass) {
      float room = avail, units = 0;
      for (size_t i = 0; i < cols.size(); ++i) {
        if (held[i] == 1) room -= cols[i].least;
        else if (held[i] == 2) room -= 250;
        else units += cols[i].units;
      }
      k = units > 0 ? room / (units * fs) : 0;
      bool changed = false;
      for (size_t i = 0; i < cols.size(); ++i) {
        if (held[i]) continue;
        const float w = cols[i].units * fs * k;
        if (w < cols[i].least) held[i] = 1, changed = true;
        else if (cols[i].id == 13 && w > 250 && cols[i].least <= 250) held[i] = 2, changed = true;
      }
      if (!changed) break;
    }
    float x = p0.x + 4;
    for (size_t i = 0; i < cols.size(); ++i) {
      Col& cl = cols[i];
      cl.w = std::floor(held[i] == 1 ? cl.least : held[i] == 2 ? 250.0f : std::max(cl.least, cl.units * fs * k));
      cl.x = x;
      x += cl.w + 3;
    }
  }
  auto colOf = [&](int id) -> const Col* {
    for (const Col& cl : cols)
      if (cl.id == id) return &cl;
    return nullptr;
  };
  auto btn = [&](int col, int row, const char* id, const char* label, bool enabled, bool on, const char* hint) {
    const Col* cl = colOf(col);
    if (!cl) return false;
    if (!enabled) ImGui::BeginDisabled();
    const bool click = bevelButton(a, id, label, ImVec2(cl->x, rowY(row)), ImVec2(cl->x + cl->w, rowY(row) + rowH), on, hint);
    if (!enabled) ImGui::EndDisabled();
    return click && enabled;
  };
  const bool have = c.have();
  const tp::Span r = rangeOf(c);
  const bool part = canPart(c);
  const bool hasClip = !tp::clipboard().L.empty();
  const bool previewing = previewSounding(a);
  // Play.
  if (const Col* cl = colOf(0)) {
    const int d = stepper(a, "##tpn", "Play", noteName(c.m ? c.m->playNote : 49), ImVec2(cl->x, rowY(0)), cl->w, rowH,
                          "The note the play buttons play (the typing keyboard's notes set it too)", std::round(fs * 2.6f), true);
    if (d && c.m) c.m->playNote = std::clamp(c.m->playNote + d * (io.KeyShift ? 12 : 1), 1, 96);
    if (btn(0, 1, "##tstop", "Stop", true, false, keys::withKeys(i18n::N_("Stops the sample playing (%s)"), {keys::fixedKey("Alt+Q")}))) stopPreview(a), releaseHeld(a);
    theme::textEllipsis(dl, a.fontRegular, std::round(fs * 0.85f), ImVec2(cl->x + 2, rowY(2) + std::round((rowH - fs * 0.85f) * 0.5f)), cl->w - 4, P.textFaint,
                        i18n::tr("keys play the note"));  // (inside its column, never under the next)
  }
  if (btn(1, 0, "##twave", "Wave", have, previewing, keys::withKeys(i18n::N_("Plays the whole sample at the play note (%s)"), {keys::fixedKey("Alt+P")})))
    startPreview(a, ch, *c.snap->inst, c.snap->e, c.k, c.m->playNote, 0, 0);
  if (btn(1, 1, "##trange", "Range", have && !r.empty(), false, "Plays the range"))
    startPreview(a, ch, *c.snap->inst, c.snap->e, c.k, c.m->playNote, r.a, r.b);
  if (btn(1, 2, "##tview", "View", have, false, "Plays the part of the sample in view"))
    startPreview(a, ch, *c.snap->inst, c.snap->e, c.k, c.m->playNote, static_cast<int32_t>(c.m->viewStart),
                 static_cast<int32_t>(std::ceil(c.m->viewStart + c.m->viewLen)));
  // Range and view.
  if (fold >= 2) {  // tight: one column, the zooms in a menu
    const int rc = fold >= 3 ? 23 : 22;
    if (btn(rc, 0, "##tall", "Select all", have, false, keys::withKeys(i18n::N_("The whole sample as the range (%s)"), {keys::fixedKey("Alt+A")}))) edit(a, c, tp::HeaderEdit::SelectAll);
    if (btn(rc, 1, "##tzoom", "Zoom...", have, false, "Zoom to range, zoom in or out, the whole sample")) ImGui::OpenPopup("##tzoommenu");
    if (btn(22, 2, "##tclr", "Clear range", have && c.m && c.m->selA >= 0, false, "No range: the edits take the whole sample")) edit(a, c, tp::HeaderEdit::ClearRange);
    if (ImGui::BeginPopup("##tzoommenu")) {
      if (ImGui::MenuItem(i18n::tr("Zoom to range"), keys::namedText("Alt+S"), false, have && !r.empty())) zoomToRange(c);
      if (ImGui::MenuItem(i18n::tr("Zoom in"), nullptr, false, have)) zoomAbout(c, 0.5, c.m->viewStart + c.m->viewLen * 0.5);
      if (ImGui::MenuItem(i18n::tr("Zoom out"), keys::namedText("Alt+Z"), false, have)) zoomAbout(c, 2.0, c.m->viewStart + c.m->viewLen * 0.5);
      if (ImGui::MenuItem(i18n::tr("Whole sample"), nullptr, false, have)) wholeView(c);
      ImGui::EndPopup();
    }
    // Clipboard: copy and paste at hand, the rest in a menu.
    if (btn(21, 0, "##tcopy", "Copy", have, false, keys::withKeys(i18n::N_("The range (or the whole sample) to the clipboard (%s)"), {keys::fixedKey("Alt+C")}))) edit(a, c, tp::HeaderEdit::Copy);
    if (btn(21, 1, "##tpaste", "Paste", have && hasClip, false, keys::withKeys(i18n::N_("The clipboard over the range, or in at the cursor (%s)"), {keys::fixedKey("Alt+V")}))) edit(a, c, tp::HeaderEdit::Paste);
    if (btn(21, 2, "##tedit", "Edit...", have, false, "Cut, crop, delete, silence")) ImGui::OpenPopup("##teditmenu");
    if (btn(23, 2, "##tedit", "Edit...", have, false, "Cut, copy, paste, crop, delete, silence, clear range")) ImGui::OpenPopup("##teditmenu");
    if (colOf(21) || colOf(23)) devMenuRow("Edit button", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    if (ImGui::BeginPopup("##teditmenu")) {
      if (ImGui::MenuItem(i18n::tr("Cut"), keys::namedText("Alt+X"), false, part)) edit(a, c, tp::HeaderEdit::Cut);
      if (ImGui::MenuItem(i18n::tr("Copy"), keys::namedText("Alt+C"), false, have)) edit(a, c, tp::HeaderEdit::Copy);
      if (ImGui::MenuItem(i18n::tr("Paste"), keys::namedText("Alt+V"), false, have && hasClip)) edit(a, c, tp::HeaderEdit::Paste);
      if (ImGui::MenuItem(i18n::tr("Crop"), keys::namedText("Alt+R"), false, part)) edit(a, c, tp::HeaderEdit::Crop);
      devMenuRow("Edit Crop", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
      if (ImGui::MenuItem(i18n::tr("Delete"), "Del", false, part)) edit(a, c, tp::HeaderEdit::Delete);
      if (ImGui::MenuItem(i18n::tr("Silence"), nullptr, false, have && !r.empty())) edit(a, c, tp::HeaderEdit::Silence);
      if (fold >= 3) {
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Clear range"), nullptr, false, have && c.m && c.m->selA >= 0)) edit(a, c, tp::HeaderEdit::ClearRange);
      }
      ImGui::EndPopup();
    }
  }
  if (btn(2, 0, "##tall", "Select all", have, false, keys::withKeys(i18n::N_("The whole sample as the range (%s)"), {keys::fixedKey("Alt+A")}))) edit(a, c, tp::HeaderEdit::SelectAll);
  if (btn(2, 1, "##tzr", "Zoom to range", have && !r.empty(), false, keys::withKeys(i18n::N_("The range fills the view (%s)"), {keys::fixedKey("Alt+S")}))) zoomToRange(c);
  if (btn(2, 2, "##tclr", "Clear range", have && c.m && c.m->selA >= 0, false, "No range: the edits take the whole sample")) edit(a, c, tp::HeaderEdit::ClearRange);
  if (btn(3, 0, "##tzin", "Zoom in", have, false, "Twice as close, about the middle (the wheel zooms at the pointer)"))
    zoomAbout(c, 0.5, c.m->viewStart + c.m->viewLen * 0.5);
  if (btn(3, 1, "##tzout", "Zoom out", have, false, keys::withKeys(i18n::N_("Twice as far, about the middle (%s)"), {keys::fixedKey("Alt+Z")}))) zoomAbout(c, 2.0, c.m->viewStart + c.m->viewLen * 0.5);
  if (btn(3, 2, "##tzall", "Whole sample", have, false, "The whole sample in view")) wholeView(c);
  // Clipboard.
  if (btn(4, 0, "##tcut", "Cut", part, false, keys::withKeys(i18n::N_("The range to the clipboard, out of the sample (%s)"), {keys::fixedKey("Alt+X")}))) edit(a, c, tp::HeaderEdit::Cut);
  if (btn(4, 1, "##tcopy", "Copy", have, false, keys::withKeys(i18n::N_("The range (or the whole sample) to the clipboard (%s)"), {keys::fixedKey("Alt+C")}))) edit(a, c, tp::HeaderEdit::Copy);
  if (btn(4, 2, "##tpaste", "Paste", have && hasClip, false, keys::withKeys(i18n::N_("The clipboard over the range, or in at the cursor (%s)"), {keys::fixedKey("Alt+V")}))) edit(a, c, tp::HeaderEdit::Paste);
  if (btn(5, 0, "##tcrop", "Crop", part, false, keys::withKeys(i18n::N_("Only the range stays (%s)"), {keys::fixedKey("Alt+R")}))) edit(a, c, tp::HeaderEdit::Crop);
  if (btn(5, 1, "##tdel", "Delete", part, false, "The range goes, nothing to the clipboard (Del)")) edit(a, c, tp::HeaderEdit::Delete);
  if (btn(5, 2, "##tsil", "Silence", have && !r.empty(), false, "The range becomes silence")) edit(a, c, tp::HeaderEdit::Silence);
  // 0.7.8: entries. To entry: the range as a sample entry of its own (the same file, played from the
  // range's start to its end; nothing written), in the next free entry or one picked.
  if (colOf(15)) {
    const bool ranged = have && !r.empty() && !c.sampler;  // (K-391: a Sampler holds one sample)
    if (btn(15, 0, "##ttoentry", "To entry", ranged, false, "The range as an entry of its own: the next free one or one you pick (the same file, nothing written)"))
      ImGui::OpenPopup("##ttoentrymenu");
    const ImVec2 tb0 = ImGui::GetItemRectMin(), tb1 = ImGui::GetItemRectMax();
    devMenuRow("To entry button", tb0.x, tb0.y, tb1.x, tb1.y);
    if (c.sampler && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      s.hover = i18n::tr("A Sampler plays one sample: convert it to a Tracker Sampler for more entries");
    } else if (!ranged && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      s.hover = i18n::tr("To entry: drag a range on the waveform first");
      ImGui::SetTooltip("%s", i18n::tr("Drag a range on the waveform first"));
    }
    toEntryMenu(a, c, tb0, tb1);
    if (btn(15, 1, "##tdup", "Duplicate", have && !c.sampler, false, "The sample into the next free entry (its keys stay with this one)")) {
      stopPreview(a);
      tp::headerEntryOp(a, ch, tp::EntryOp::Duplicate, c.k);
    }
    if (btn(15, 2, "##tallkeys", "All keys", have && !c.sampler, false, "Every key plays this entry (the Instrument editor's key map shows them)"))
      tp::headerEntryOp(a, ch, tp::EntryOp::MapAllKeys, c.k);
  }
  // Level, rate and time.
  if (narrow) {
    if (btn(20, 0, "##tproc", "Process...", have, false, "Fades, reverse, DC, resample, echo, mix, loop fade")) ImGui::OpenPopup("##tedproc");
    devMenuRow("Process button", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    if (btn(20, 1, "##tvol", "Volume...", have, false, "The range's volume from a start to an end level")) openModal(Modal::Volume);
    if (btn(20, 2, "##tnorm", "Normalise", have, false, "The loudest point to full scale")) edit(a, c, tp::HeaderEdit::Normalise);
    if (ImGui::BeginPopup("##tedproc")) {
      if (!colOf(16)) {  // 0.7.8: short of width, the Backwards box lives here
        const bool rev = have && c.snap->e.text.def.samples[static_cast<size_t>(c.k)].reverse;
        if (ImGui::MenuItem(i18n::tr("Backwards"), nullptr, rev, have)) tp::opSampleReverse(a, c.t, c.k, !rev);
        devMenuRow("Process Backwards", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::Separator();
      }
      editMenuItems(a, c, false);
      if (ImGui::MenuItem(i18n::tr("Save range..."), nullptr, false, have)) saveRange(a, c);
      ImGui::EndPopup();
    }
  } else {
    if (btn(6, 0, "##tvol", "Volume...", have, false, "The range's volume from a start to an end level")) openModal(Modal::Volume);
    if (btn(6, 1, "##tfin", "Fade in", have, false, "From silence up (the range, or the whole sample)")) edit(a, c, tp::HeaderEdit::FadeIn);
    if (btn(6, 2, "##tfout", "Fade out", have, false, "Down to silence (the range, or the whole sample)")) edit(a, c, tp::HeaderEdit::FadeOut);
    if (btn(7, 0, "##tnorm", "Normalise", have, false, "The loudest point to full scale")) edit(a, c, tp::HeaderEdit::Normalise);
    if (btn(7, 1, "##trev", "Reverse", have, false, "Backwards (the range, or the whole sample)")) edit(a, c, tp::HeaderEdit::Reverse);
    if (btn(7, 2, "##tdc", "Fix DC", have, false, "Takes away a constant offset from zero")) edit(a, c, tp::HeaderEdit::FixDc);
    if (btn(8, 0, "##tres", "Resample...", have, false, "More or fewer frames for the same pitch")) openModal(Modal::Resample);
    if (btn(8, 1, "##techo", "Echo...", have, false, "Repeats of the sound, each quieter")) openModal(Modal::Echo);
    if (btn(8, 2, "##tmix", "Mix in...", c.ok() && have && tp::sampleCountOf(c.snap->e) > 1, false, "Another sample of the instrument mixed in"))
      openModal(Modal::Mix);
    if (btn(9, 0, "##tlf", "Loop fade...", have && c.looping, false, "The loop's end fades into the sound before its start")) openModal(Modal::LoopFade);
    if (const Col* cl = colOf(17)) {  // K-436: Gridline's effects over the sample (or the range)
      if (!have) ImGui::BeginDisabled();
      if (bevelButton(a, "##tfx", "Effects...", ImVec2(cl->x, rowY(0)), ImVec2(cl->x + cl->w, rowY(2) + rowH), false,
                      "Gridline's own effects over the range or the whole sample (Apply keeps the result, one undo step)") && have)
        openModal(Modal::Effects);
      if (!have) ImGui::EndDisabled();
      devMenuRow("Effects button", cl->x, rowY(0), cl->x + cl->w, rowY(2) + rowH);
    }
    const bool cal = btn(9, 1, "##tcal", "Cut after loop", minimisable(c), false,
                         c.rev ? "Played backwards: what comes before the loop's start goes (it is never heard)" : "What follows the loop's end goes");
    if (colOf(9)) devMenuRow("Cut after loop button", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    if (cal) edit(a, c, tp::HeaderEdit::TrimLoop);
    if (btn(9, 2, "##tsave", "Save range...", have, false, "The range (or the whole sample) as a WAV file")) saveRange(a, c);
  }
  // Loop type and bits.
  if (const Col* cl = colOf(10)) {
    static const char* names[] = {"No loop", "Forward", "Ping-pong"};
    static const char* hints[] = {"The sample plays once", "The loop repeats forwards while the note holds (a marked range becomes a new loop)",
                                  "The loop runs forwards and back while the note holds (a marked range becomes a new loop)"};
    const int cur = have ? c.snap->e.text.def.samples[static_cast<size_t>(c.k)].loop : 0;
    // cp25 (mouse session): a loop made with a range marked is that range (it was the whole sample).
    const tp::Span lr = rangeOf(c);
    for (int i = 0; i < 3; ++i) {
      if (!have) ImGui::BeginDisabled();
      if (radioRow(a, names[i], names[i], ImVec2(cl->x, rowY(i)), ImVec2(cl->x + cl->w, rowY(i) + rowH), have && cur == i, hints[i]) && have && cur != i) {
        tp::opSampleLoopType(a, c.t, c.k, static_cast<uint8_t>(i), lr.empty() ? c.s0 : lr.a, lr.empty() ? c.s1 : lr.b);
      }
      if (!have) ImGui::EndDisabled();
    }
  }
  // 0.7.8: the play direction beside the loop type: backwards plays the sample (or its loop) from its
  // end; the audio stays as it is (the Reverse edit is the one that turns it round).
  if (const Col* cl = colOf(16)) {
    const bool rev = have && c.snap->e.text.def.samples[static_cast<size_t>(c.k)].reverse;
    if (!have) ImGui::BeginDisabled();
    if (checkRow(a, "##tbackw", "Backwards", ImVec2(cl->x, rowY(0)), ImVec2(cl->x + cl->w, rowY(0) + rowH), rev,
                 "Plays the sample, or its loop, from end to start (the audio is not changed)") &&
        have)
      tp::opSampleReverse(a, c.t, c.k, !rev);
    devMenuRow("Backwards box", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    if (!have) ImGui::EndDisabled();
  }
  if (const Col* cl = colOf(11)) {
    for (int i = 0; i < 2; ++i) {
      const bool can = have && !(c.sampler && i == 1);  // (K-391: a Sampler plays at full resolution)
      if (!can) ImGui::BeginDisabled();
      const char* eightHint = c.sampler ? i18n::N_("A Sampler plays at full resolution: convert it to a Tracker Sampler for 8-bit")
                                        : i18n::N_("The sample plays rounded to 256 steps, as an 8-bit sample");
      if (radioRow(a, i ? "##t8" : "##t16", i ? "8-bit" : "16-bit", ImVec2(cl->x, rowY(i)), ImVec2(cl->x + cl->w, rowY(i) + rowH), have && c.eight == (i == 1),
                   i ? eightHint : "The sample plays at full resolution") &&
          can && c.eight != (i == 1))
        tp::opSampleBits(a, c.t, c.k, i == 1);
      if (!can) ImGui::EndDisabled();
    }
    // Load... and URL... share the row: a file, or a web link's audio, into this slot.
    // (a narrow row keeps Load... whole; Import from URL is then in the Process... menu and the entry menu)
    const float uw = buttonsW({"URL..."});
    const bool both = cl->w >= buttonsW({"Load..."}) + 4 + uw;
    if (!c.ok()) ImGui::BeginDisabled();
    if (bevelButton(a, "##tload", "Load...", ImVec2(cl->x, rowY(2)), ImVec2(both ? cl->x + cl->w - uw - 4 : cl->x + cl->w, rowY(2) + rowH), false,
                    "An audio file into this slot") &&
        c.ok())
      loadDialog(a, ch);
    if (both && bevelButton(a, "##turl", "URL...", ImVec2(cl->x + cl->w - uw, rowY(2)), ImVec2(cl->x + cl->w, rowY(2) + rowH), false,
                            "Import from URL: a web link's audio into this slot") &&
        c.ok())
      urlDialog(a, ch);
    if (!c.ok()) ImGui::EndDisabled();
  }
  // Readouts and steppers.
  if (const Col* cl = colOf(12)) {
    readout(a, "Length", have ? fmtInt(c.s1 - c.s0) : "-", ImVec2(cl->x, rowY(0)), cl->w, rowH);
    readout(a, "Start", have ? fmtInt(c.s0) : "-", ImVec2(cl->x, rowY(1)), cl->w, rowH);
    readout(a, "End", have ? fmtInt(c.s1) : "-", ImVec2(cl->x, rowY(2)), cl->w, rowH);
  }
  if (const Col* cl = colOf(13)) {
    if (!have || !c.looping) ImGui::BeginDisabled();
    // cp23: one label column for both rows (the fields started 9 px apart at 1024x640, "Loop length"
    // pushing its field right).
    const float lw = std::max(std::round(cl->w * 0.45f), labelColumn(a, {"Loop start", "Loop length"}, space::kPad));
    const int big = io.KeyShift ? 100 : 1;
    if (const int d = stepper(a, "##tls", "Loop start", have && c.looping ? fmtInt(c.ls) : "-", ImVec2(cl->x, rowY(0)), cl->w, rowH,
                              "The loop's start (its end stays); Shift: by 100", lw, true);
        d && have && c.looping)
      tp::opSampleMarker(a, c.t, c.k, 2, c.ls + d * big, c.frames);
    if (const int d = stepper(a, "##tll", "Loop length", have && c.looping ? fmtInt(c.le - c.ls) : "-", ImVec2(cl->x, rowY(1)), cl->w, rowH,
                              "The loop's length (its start stays); Shift: by 100", lw, true);
        d && have && c.looping)
      tp::opSampleMarker(a, c.t, c.k, 3, c.le + d * big, c.frames);
    if (!have || !c.looping) ImGui::EndDisabled();
    if (narrow) readout(a, "Length", have ? fmtInt(c.s1 - c.s0) : "-", ImVec2(cl->x, rowY(2)), cl->w, rowH);
    else {
      char b[64];
      if (have) std::snprintf(b, sizeof b, "%lld - %lld", static_cast<long long>(c.m->viewStart), static_cast<long long>(c.m->viewStart + c.m->viewLen));
      readout(a, "View", have ? b : "-", ImVec2(cl->x, rowY(2)), cl->w, rowH);
    }
  }
  // Exit, Remove, Minimise.
  if (btn(14, 0, "##texit", "Exit", true, false, "Back to the pattern (Esc)")) close(a);
  if (btn(14, 1, "##trem", "Remove", have && !c.sampler, false, c.sampler ? i18n::N_("A Sampler keeps its sample: load another file to replace it") : "Takes the sample out of the instrument (asks first)"))
    openModal(Modal::Remove);
  const bool minimise = btn(14, 2, "##tmin", "Minimise", minimisable(c), false,
                            c.rev ? "Played backwards: what comes before the loop's start goes (the sample starts with its loop)"
                                  : "What follows the loop's end goes (the sample ends with its loop)");
  if (colOf(14)) devMenuRow("Minimise button", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
  if (minimise) edit(a, c, tp::HeaderEdit::TrimLoop);
  // ---- the status line ----
  {
    const ImVec2 s0(p0.x, p1.y - statusH), s1(p1.x, p1.y);
    bevelBox(dl, s0, s1, true);
    std::string text = s.hover;
    if (text.empty() && have) {
      char b[200];
      std::string loop = c.looping ? (std::string(c.snap->e.text.def.samples[static_cast<size_t>(c.k)].loop == trk::kLoopPingPong ? i18n::tr("ping-pong") : i18n::tr("forward")) +
                                      " " + fmtInt(c.ls) + "+" + fmtInt(c.le - c.ls))
                                   : std::string(i18n::tr("no loop"));
      if (c.snap->e.text.def.samples[static_cast<size_t>(c.k)].reverse) loop += std::string(" ") + i18n::tr("backwards");
      std::snprintf(b, sizeof b, "%s %s %s  ·  %s %s  ·  %s %s  ·  %s %s  ·  %s %s  ·  %s", i18n::tr("Sample"), hex2(c.k + 1).c_str(),
                    tp::editName(c.snap->e, c.k).c_str(), i18n::tr("range"), r.empty() ? "-" : (fmtInt(r.a) + "-" + fmtInt(r.b)).c_str(), i18n::tr("loop"),
                    loop.c_str(), keys::fixedKey("Alt+A").c_str(), i18n::tr("all"), keys::fixedKey("Alt+S").c_str(), i18n::tr("zoom to range"),
                    i18n::tr("wheel: zoom"));
      text = b;
    }
    const char* keys = s.hasKb ? i18n::tr("Keys: the sample editor") : i18n::tr("Keys: the pattern");
    const float kw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, keys).x;
    const float ty = std::round(s0.y + (statusH - fs) * 0.5f);
    theme::textEllipsis(dl, mono, std::round(fs * 0.9f), ImVec2(s0.x + 6, ty + 1), s1.x - s0.x - kw - 24, P.textDim, text.c_str());
    dl->AddText(a.fontRegular, fs, ImVec2(s1.x - kw - 8, ty), s.hasKb ? P.text : P.textFaint, keys);
  }
  modals(a, c);
}

// ---- the Instrument editor (cp19) ----
// The classic tracker's instrument screen in function (K-316 plan 4): the volume and panning
// envelopes (points, sustain, loop, presets, add and delete), the selected sample's own values
// (volume, pan, fine tune, relative note), the instrument's fadeout and auto vibrato, and the key
// map across a piano under them. Gridline's own drawing and wording; every edit one of the
// session's ops (the plugin editor's labels and merge keys).

// The piano: 96 keys (C-0 .. B-7), the white keys side by side, the black ones over their upper part.
// cp20: a short band shows `octs` octaves from octave `first` (wider keys; the arrows move them).
struct KeyBox {
  ImVec2 p0, p1;
  bool black = false;
};
struct KeyView {
  int first = 0, octs = 8;
  bool has(int n) const { return n >= first * 12 && n < (first + octs) * 12; }
};
KeyBox keyBox(ImVec2 p0, ImVec2 p1, int n, KeyView kv = KeyView{}) {
  static const int whiteOf[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
  static const int leftWhite[12] = {-1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1};
  const float ww = (p1.x - p0.x) / static_cast<float>(kv.octs * 7);
  const int oct = n / 12 - kv.first, pc = n % 12;
  KeyBox k;
  if (whiteOf[pc] >= 0) {
    const float x = p0.x + ww * static_cast<float>(oct * 7 + whiteOf[pc]);
    k.p0 = ImVec2(std::round(x), p0.y);
    k.p1 = ImVec2(std::round(x + ww), p1.y);
    return k;
  }
  const float cx = p0.x + ww * static_cast<float>(oct * 7 + leftWhite[pc] + 1);
  const float bw = std::max(3.0f, ww * 0.62f);
  k.black = true;
  k.p0 = ImVec2(std::round(cx - bw * 0.5f), p0.y);
  k.p1 = ImVec2(std::round(cx + bw * 0.5f), std::round(p0.y + (p1.y - p0.y) * 0.58f));
  return k;
}
// The key under `m` (clamped into the strip: a drag that leaves it keeps the nearest key).
int keyUnder(ImVec2 p0, ImVec2 p1, ImVec2 m, KeyView kv = KeyView{}) {
  m.x = std::clamp(m.x, p0.x, p1.x - 1);
  m.y = std::clamp(m.y, p0.y, p1.y - 1);
  for (int pass = 0; pass < 2; ++pass) {  // the black keys lie over the white ones
    for (int n = kv.first * 12; n < (kv.first + kv.octs) * 12; ++n) {
      const KeyBox k = keyBox(p0, p1, n, kv);
      if (k.black == (pass == 0) && m.x >= k.p0.x && m.x < k.p1.x && m.y >= k.p0.y && m.y < k.p1.y) {
        return n;
      }
    }
  }
  return -1;
}

// A vibrato shape as a small drawn wave (0 sine, 1 square, 2 ramp up, 3 ramp down).
void shapeGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int shape, ImU32 col) {
  const trackerview::Aliased hard(dl);  // cp21: a hard-edged shape, as the classic glyphs
  const float x0 = p0.x + 7, x1 = p1.x - 7, cy = std::round((p0.y + p1.y) * 0.5f), h = std::max(3.0f, std::round((p1.y - p0.y) * 0.26f));
  ImVec2 pts[25];
  int n = 0;
  if (shape == 0) {
    for (int i = 0; i <= 24; ++i) {
      const float t = static_cast<float>(i) / 24.0f;
      pts[n++] = ImVec2(x0 + (x1 - x0) * t, cy - std::sin(t * 6.2831853f) * h);
    }
  } else if (shape == 1) {
    const float xm = std::round((x0 + x1) * 0.5f);
    pts[n++] = ImVec2(x0, cy - h);
    pts[n++] = ImVec2(xm, cy - h);
    pts[n++] = ImVec2(xm, cy + h);
    pts[n++] = ImVec2(x1, cy + h);
  } else if (shape == 2) {
    pts[n++] = ImVec2(x0, cy + h);
    pts[n++] = ImVec2(x1, cy - h);
    pts[n++] = ImVec2(x1, cy + h);
  } else {
    pts[n++] = ImVec2(x0, cy + h);
    pts[n++] = ImVec2(x0, cy - h);
    pts[n++] = ImVec2(x1, cy + h);
  }
  dl->AddPolyline(pts, n, col, 0, 1.0f);
}

// A sideways arrow button (a bevel face with a triangle), repeating while held.
bool sideArrow(App& a, const char* id, ImVec2 p0, ImVec2 p1, bool left, const char* hint) {
  ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
  const bool click = bevelButton(a, id, "", p0, p1, false, hint);
  ImGui::PopItemFlag();
  const bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cx = std::round((p0.x + p1.x) * 0.5f) + (held ? 1 : 0), cy = std::round((p0.y + p1.y) * 0.5f) + (held ? 1 : 0);
  const float r = std::max(2.0f, std::floor(std::min(p1.x - p0.x, p1.y - p0.y) * 0.22f));
  const ImU32 ink = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) ? pal().textFaint : pal().text;
  if (left) {
    dl->AddTriangleFilled(ImVec2(cx + r * 0.6f, cy - r - 1), ImVec2(cx + r * 0.6f, cy + r + 1), ImVec2(cx - r * 0.8f, cy), ink);
  } else {
    dl->AddTriangleFilled(ImVec2(cx - r * 0.6f, cy - r - 1), ImVec2(cx - r * 0.6f, cy + r + 1), ImVec2(cx + r * 0.8f, cy), ink);
  }
  return click;
}

std::string twoDigits(int v) {
  std::string t = std::to_string(v);
  return t.size() < 2 ? "0" + t : t;
}
std::string signedText(long v) { return (v > 0 ? "+" : "") + std::to_string(v); }

const trk::Envelope& envOf(const tp::Edit& e, bool pan) { return pan ? e.text.def.pan : e.text.def.vol; }

// The envelope edits the buttons and keys make (each its own undo step).
void envAdd(App& a, const Ctx& c, bool pan) {
  St& s = st();
  const int w = pan ? 1 : 0;
  int x = 0, y = 0;
  if (!tp::envPointAfter(envOf(c.snap->e, pan), pan, s.envSel[w], &x, &y)) {
    status(i18n::tr("No room for another point there (12 at most, a tick apart)"));
    return;
  }
  const int i = tp::opEnvPointAdd(a, c.t, pan, x, y);
  if (i >= 0) {
    s.envSel[w] = i;
  }
  a.doc.endGesture();
}
void envDelete(App& a, const Ctx& c, bool pan) {
  St& s = st();
  const int w = pan ? 1 : 0;
  if (tp::opEnvPointDelete(a, c.t, pan, s.envSel[w])) {
    s.envSel[w] = std::max(0, s.envSel[w] - 1);
  }
}
void envNudge(App& a, const Ctx& c, bool pan, int dx, int dy) {
  const St& s = st();
  const trk::Envelope& env = envOf(c.snap->e, pan);
  const int i = s.envSel[pan ? 1 : 0];
  if (i < 0 || i >= env.count) {
    return;
  }
  tp::opEnvPointMove(a, c.t, pan, i, env.x[static_cast<size_t>(i)] + dx, env.y[static_cast<size_t>(i)] + dy);
  a.doc.endGesture();
}

// One Instrument editor command from a key (queued by sdlKey, run in the draw).
void runInsKey(App& a, Ctx& c, const KeyEv& k) {
  St& s = st();
  const bool ctrl = (k.mods & (KMOD_CTRL | KMOD_GUI)) != 0, alt = (k.mods & KMOD_ALT) != 0, shift = (k.mods & KMOD_SHIFT) != 0;
  if (k.sc == SDL_SCANCODE_ESCAPE || (ctrl && k.kc == 'w')) {
    close(a);
    return;
  }
  if (alt) {
    if (k.sc == SDL_SCANCODE_UP || k.sc == SDL_SCANCODE_DOWN) {
      tp::headerStepSample(a, c.ch, k.sc == SDL_SCANCODE_UP ? -1 : 1);
    }
    return;
  }
  const bool pan = s.envFocus == 1;
  const int w = s.envFocus;
  const int count = envOf(c.snap->e, pan).count;
  switch (k.sc) {
    case SDL_SCANCODE_TAB: s.envFocus ^= 1; break;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT: {
      const int d = k.sc == SDL_SCANCODE_LEFT ? -1 : 1;
      if (shift) {
        envNudge(a, c, pan, d, 0);
      } else {
        s.envSel[w] = std::clamp(s.envSel[w] + d, 0, std::max(0, count - 1));
      }
      break;
    }
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN: envNudge(a, c, pan, 0, (k.sc == SDL_SCANCODE_UP ? 1 : -1) * (shift ? 8 : 1)); break;
    case SDL_SCANCODE_INSERT: envAdd(a, c, pan); break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      if (ctrl) {
        envAdd(a, c, pan);
      }
      break;
    case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_BACKSPACE: envDelete(a, c, pan); break;
    default: break;
  }
}

// Alt+click on the piano: the key sounds while held (a drag moves it from key to key; -1: silence).
void audition(App& a, ObjectId ch, int n) {
  St& s = st();
  if (s.keyAud == n) {
    return;
  }
  if (const auto it = s.held.find(-1); it != s.held.end()) {
    a.engine.noteOff(it->second.channel, it->second.pitch);
    s.held.erase(it);
  }
  s.keyAud = n;
  if (n >= 0) {
    const int pitch = tp::stripKeyPitch(n);
    a.engine.noteOn(ch, pitch, 100.0f / 127.0f);
    s.held[-1] = {ch, pitch};
  }
}

// One envelope box: the header row (on, presets, the count), the graph, the side column.
// cp22: `tabs`: the box is the only one shown (a short band); Volume and Panning tabs in its side column.
void envBox(App& a, Ctx& c, bool pan, ImVec2 p0, ImVec2 p1, float sideW, bool tabs = false) {
  St& s = st();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Aliased hard(dl);  // cp21: the envelope in hard pixel lines, as the classic editor
  ImGuiIO& io = ImGui::GetIO();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const float rh = std::round(fs + 7);
  const tp::Edit& e = c.snap->e;
  const int w = pan ? 1 : 0;
  const trk::Envelope& env = envOf(e, pan);
  const bool on = env.on();
  bevelBox(dl, p0, p1, false);
  if (s.hasKb && s.envFocus == w) {
    dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 2), ImVec2(p0.x + 3, p1.y - 2), P.hot);
  }
  const float bx0 = p0.x + 5, gEdge = p1.x - sideW - 4;
  const float hy = p0.y + 3;
  s.envSel[w] = std::clamp(s.envSel[w], 0, std::max(0, env.count - 1));
  const int sel = s.envSel[w];
  ImGui::PushID(w);
  if (checkRow(a, "##ion", pan ? "Panning envelope" : "Volume envelope", ImVec2(bx0, hy), ImVec2(bx0 + std::round(fs * 9.5f), hy + rh), on,
               pan ? "The panning envelope moves the sound while the note plays" : "The volume envelope shapes every note; off: notes stop when let go")) {
    tp::opEnvFlag(a, c.t, pan, trk::kEnvOn);
    s.envFocus = w;
  }
  // The presets: a click puts the slot in, a right-click keeps the current one there or brings back its starting shape.
  {
    const float px = bx0 + std::round(fs * 10.0f);
    dl->AddText(a.fontRegular, fs, ImVec2(px, std::round(hy + (rh - fs) * 0.5f)), P.textDim, i18n::tr("Presets"));
    const float bw = std::round(rh * 1.25f);
    float x = px + std::round(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Presets")).x + 8);
    static const char* names[] = {"1", "2", "3", "4", "5", "6"};
    static const char* ids[] = {"##ip1", "##ip2", "##ip3", "##ip4", "##ip5", "##ip6"};
    tp::EnvSlots& slots = tp::envSlots(a);
    for (int k = 0; k < tp::kEnvSlots; ++k) {
      if (x + bw > gEdge - fs * 6) {
        break;  // (a narrow box: as many as fit beside the count)
      }
      if (bevelButton(a, ids[k], names[k], ImVec2(x, hy), ImVec2(x + bw, hy + rh), false,
                      pan ? "Click: this panning envelope; right-click: keep the current one here"
                          : "Click: this volume envelope with its fadeout and vibrato; right-click: keep the current ones here")) {
        tp::opEnvApplySlot(a, c.t, pan, (pan ? slots.pan : slots.vol)[static_cast<size_t>(k)]);
        s.envFocus = w;
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        s.slotMenu = k;
        s.slotMenuPan = pan;
        ImGui::OpenPopup("##islot");
      }
      x += bw + 2;
    }
    const std::string cnt = std::to_string(env.count) + " " + i18n::tr("of") + " " + std::to_string(trk::kEnvPoints) + " " + i18n::tr("points");
    const float cfs = std::round(fs * 0.9f);
    const float tw = a.fontRegular->CalcTextSizeA(cfs, FLT_MAX, 0, cnt.c_str()).x;
    if (gEdge - tw - 4 > x) {
      dl->AddText(a.fontRegular, cfs, ImVec2(gEdge - tw - 4, std::round(hy + (rh - cfs) * 0.5f)), P.textFaint, cnt.c_str());
    }
  }
  if (ImGui::BeginPopup("##islot")) {
    tp::EnvSlots& slots = tp::envSlots(a);
    const int k = std::clamp(s.slotMenu, 0, tp::kEnvSlots - 1);
    auto& row = s.slotMenuPan ? slots.pan : slots.vol;
    char it[160];
    std::snprintf(it, sizeof it, i18n::tr("Keep this envelope in preset %d"), k + 1);
    if (ImGui::MenuItem(it)) {
      row[static_cast<size_t>(k)] = tp::takeEnvSlot(e.text.def, s.slotMenuPan);
      if (!tp::saveEnvSlots(tp::envSlotsPath(a), slots)) {
        a.notify(i18n::tr("The envelope slots could not be saved"), true);
      }
    }
    std::snprintf(it, sizeof it, i18n::tr("Give preset %d its starting shape again"), k + 1);
    if (ImGui::MenuItem(it)) {
      const tp::EnvSlots f = tp::factoryEnvSlots();
      row[static_cast<size_t>(k)] = (s.slotMenuPan ? f.pan : f.vol)[static_cast<size_t>(k)];
      if (!tp::saveEnvSlots(tp::envSlotsPath(a), slots)) {
        a.notify(i18n::tr("The envelope slots could not be saved"), true);
      }
    }
    ImGui::EndPopup();
  }
  // ---- the graph ----
  const ImVec2 g0(bx0, hy + rh + 3), g1(gEdge, p1.y - 4);
  bevelBox(dl, g0, g1, true);
  const float gx0 = g0.x + 6, gx1 = g1.x - 6, gy0 = g0.y + 5, gy1 = g1.y - 5;
  int lastX = 0;
  for (int i = 0; i < env.count; ++i) {
    lastX = std::max<int>(lastX, env.x[static_cast<size_t>(i)]);
  }
  if (s.envDrag[w] < 0 || s.envSpan[w] <= 0) {
    s.envSpan[w] = static_cast<float>(std::max(64, ((lastX + 16 + 15) / 16) * 16));
  }
  const float span = s.envSpan[w];
  s.envG0[w] = ImVec2(gx0, gy0);
  s.envG1[w] = ImVec2(gx1, gy1);
  auto pt = [&](int i) {
    return ImVec2(std::round(gx0 + (gx1 - gx0) * static_cast<float>(env.x[static_cast<size_t>(i)]) / span),
                  std::round(gy1 - (gy1 - gy0) * static_cast<float>(env.y[static_cast<size_t>(i)]) / 64.0f));
  };
  dl->PushClipRect(ImVec2(g0.x + 1, g0.y + 1), ImVec2(g1.x - 1, g1.y - 1), true);
  // Grid: a faint line every 16 ticks (64 when they crowd), the middle line for panning.
  {
    const float per = (gx1 - gx0) / span;
    const int every = per * 16 >= 10 ? 16 : 64;
    for (int t = every; t < static_cast<int>(span); t += every) {
      const float x = std::round(gx0 + per * static_cast<float>(t)) + 0.5f;
      dl->AddLine(ImVec2(x, gy0), ImVec2(x, gy1), theme::withAlpha(P.textFaint, t % 64 == 0 ? 0.30f : 0.14f));
    }
    if (pan) {
      const float y = std::round((gy0 + gy1) * 0.5f) + 0.5f;
      dl->AddLine(ImVec2(gx0, y), ImVec2(gx1, y), theme::withAlpha(P.textFaint, 0.45f));
    }
  }
  const ImU32 ink = on ? P.hot : P.textFaint;
  if ((env.flags & trk::kEnvLoop) && env.loopStart < env.count && env.loopEnd < env.count) {
    const ImVec2 la = pt(env.loopStart), lb = pt(env.loopEnd);
    dl->AddRectFilled(ImVec2(la.x, g0.y + 1), ImVec2(std::max(la.x + 2, lb.x), g1.y - 1), theme::withAlpha(P.hot, on ? 0.15f : 0.07f));
    dl->AddLine(ImVec2(la.x + 0.5f, g0.y + 1), ImVec2(la.x + 0.5f, g1.y - 1), theme::withAlpha(P.hot, 0.6f));
    dl->AddLine(ImVec2(lb.x + 0.5f, g0.y + 1), ImVec2(lb.x + 0.5f, g1.y - 1), theme::withAlpha(P.hot, 0.6f));
  }
  if ((env.flags & trk::kEnvSustain) && env.sustain < env.count) {
    const float x = pt(env.sustain).x + 0.5f;
    for (float y = g0.y + 2; y < g1.y - 2; y += 6) {
      dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 3, g1.y - 2)), P.text);
    }
  }
  for (int i = 1; i < env.count; ++i) {
    dl->AddLine(pt(i - 1), pt(i), ink, 1.0f);
  }
  if (env.count > 0) {
    const ImVec2 lp = pt(env.count - 1);
    dl->AddLine(lp, ImVec2(gx1, lp.y), theme::withAlpha(ink, 0.35f));
  } else {
    theme::centreText(dl, a.fontRegular, fs, g0, g1, P.textDim, i18n::tr("Click to add the first points"));
  }
  // The pointer.
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##ienvg", ImVec2(std::max(1.0f, g1.x - g0.x), std::max(1.0f, g1.y - g0.y)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 m = io.MousePos;
  int hot = -1;
  if (hov && s.envDrag[w] < 0) {
    float best = 8;
    for (int i = 0; i < env.count; ++i) {
      const ImVec2 q = pt(i);
      const float dd = std::hypot(q.x - m.x, q.y - m.y);
      if (dd < best) {
        best = dd;
        hot = i;
      }
    }
  }
  auto toTicks = [&](float x) { return static_cast<int>(std::lround((x - gx0) / std::max(1.0f, gx1 - gx0) * span)); };
  auto toLevel = [&](float y) { return std::clamp(static_cast<int>(std::lround((gy1 - y) / std::max(1.0f, gy1 - gy0) * 64.0f)), 0, 64); };
  // (The io arrays, not IsMouseClicked: the press may reach the item a frame after the click.)
  if (ImGui::IsItemActivated() && io.MouseDown[ImGuiMouseButton_Left]) {
    s.envFocus = w;
    if (hot >= 0 && io.MouseClickedLastCount[ImGuiMouseButton_Left] >= 2) {
      s.envDrag[w] = -1;
      if (tp::opEnvPointDelete(a, c.t, pan, hot)) {
        s.envSel[w] = std::max(0, hot - 1);
      }
    } else if (hot >= 0) {
      s.envSel[w] = hot;
      s.envDrag[w] = hot;
    } else {
      const int i = tp::opEnvPointAdd(a, c.t, pan, toTicks(m.x), toLevel(m.y));
      if (i >= 0) {
        s.envSel[w] = i;
        s.envDrag[w] = i;
      }
    }
  }
  if (s.envDrag[w] >= 0) {
    if (ImGui::IsItemActive() && io.MouseDown[ImGuiMouseButton_Left]) {
      if (io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= 1.0f) {
        tp::opEnvPointMove(a, c.t, pan, s.envDrag[w], toTicks(m.x), toLevel(m.y));  // (kept between its neighbours)
      }
    } else {
      s.envDrag[w] = -1;
      a.doc.endGesture();
    }
  }
  if (hov && hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    s.envMenu = hot;
    s.envMenuPan = pan;
    s.envSel[w] = hot;
    ImGui::OpenPopup("##ienvpt");
  }
  if (ImGui::BeginPopup("##ienvpt")) {
    const int i = s.envMenu;
    if (i >= 0 && i < env.count && s.envMenuPan == pan) {
      if (ImGui::MenuItem(i18n::tr("Sustain point"), nullptr, (env.flags & trk::kEnvSustain) && env.sustain == i)) {
        tp::opEnvSustain(a, c.t, pan, i);
      }
      if (ImGui::MenuItem(i18n::tr("Loop start"), nullptr, (env.flags & trk::kEnvLoop) && env.loopStart == i)) {
        tp::opEnvLoopStart(a, c.t, pan, i);
      }
      if (ImGui::MenuItem(i18n::tr("Loop end"), nullptr, (env.flags & trk::kEnvLoop) && env.loopEnd == i)) {
        tp::opEnvLoopEnd(a, c.t, pan, i);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Delete point"), "Del", false, i > 0 && env.count > 2)) {
        tp::opEnvPointDelete(a, c.t, pan, i);
      }
    }
    ImGui::EndPopup();
  }
  // cp20: where each voice of the instrument is on this envelope (the play tap), a line across the graph.
  for (int i = 0; i < s.envLines[w]; ++i) {
    const float x = std::round(gx0 + (gx1 - gx0) * static_cast<float>(s.envTicks[w][i]) / span) + 0.5f;
    if (x >= gx0 && x <= gx1) {
      dl->AddLine(ImVec2(x, g0.y + 1), ImVec2(x, g1.y - 1), P.led, 2.0f);
    }
  }
  // The points: small squares, the selected one larger in the hot colour.
  for (int i = 0; i < env.count; ++i) {
    const ImVec2 q = pt(i);
    const bool isSel = i == sel;
    const float r = isSel || i == hot ? 4.0f : 2.5f;
    dl->AddRectFilled(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r + 1, q.y + r + 1), isSel ? P.hot : i == hot ? P.text : theme::mix(P.text, ink, 0.4f));
    if (isSel) {
      dl->AddRect(ImVec2(q.x - r - 1, q.y - r - 1), ImVec2(q.x + r + 2, q.y + r + 2), P.surface950);
    }
  }
  // The selected point, read out at the top right.
  if (env.count > 0) {
    const int lv = env.y[static_cast<size_t>(sel)];
    std::string ro = std::string(i18n::tr("Point")) + " " + twoDigits(sel + 1) + "   " + i18n::tr("tick") + " " + std::to_string(env.x[static_cast<size_t>(sel)]) + "   ";
    ro += pan ? std::string(i18n::tr("pan")) + " " + signedText(lv - 32) : std::string(i18n::tr("level")) + " " + std::to_string(lv);
    const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, ro.c_str()).x;
    dl->AddText(mono, fs, ImVec2(g1.x - tw - 8, g0.y + 4), P.textDim, ro.c_str());
  }
  dl->PopClipRect();
  if (hov) {
    const int i = hot >= 0 ? hot : s.envDrag[w];
    if (i >= 0 && i < env.count) {
      s.hover = std::string(i18n::tr("Point")) + " " + twoDigits(i + 1) + ": " + i18n::tr("tick") + " " + std::to_string(env.x[static_cast<size_t>(i)]) + ", " +
                i18n::tr("level") + " " + std::to_string(env.y[static_cast<size_t>(i)]) + "  ·  " + i18n::tr("drag: move · double-click: delete · right-click: sustain, loop");
    } else {
      s.hover = std::string(i18n::tr("Tick")) + " " + std::to_string(std::max(0, toTicks(m.x))) + ", " + i18n::tr("level") + " " + std::to_string(toLevel(m.y)) + "  ·  " +
                i18n::tr("click: add a point here (12 at most)");
    }
  }
  // ---- the side column: Add / Delete, Sustain and its point, Loop and its start / end ----
  {
    const float sx0 = gEdge + 4, sw = p1.x - 5 - sx0;
    const float half = std::floor((sw - 6) * 0.5f);
    const float A = sx0, B = sx0 + half + 6;
    auto rowY = [&](int r) { return std::round(p0.y + 3 + static_cast<float>(r) * (rh + 3)); };
    const float bw = std::floor((half - space::kGap) * 0.5f);  // cp23: Add and Delete 2 px apart, as the preset buttons (were 3)
    // cp23: Point, Start and End share one label column: their fields and arrows line up (Start's and
    // End's arrows were 4 px apart at 1024x640, their fields 5 px).
    const float slw = labelColumn(a, {"Point", "Start", "End"}, space::kPad);
    const bool full = env.count >= trk::kEnvPoints;
    if (full) {
      ImGui::BeginDisabled();
    }
    if (bevelButton(a, "##iadd", "Add", ImVec2(A, rowY(0)), ImVec2(A + bw, rowY(0) + rh), false, "A point after the selected one, halfway to the next (Insert)") && !full) {
      s.envFocus = w;
      envAdd(a, c, pan);
    }
    if (full) {
      ImGui::EndDisabled();
    }
    const bool canDel = sel > 0 && env.count > 2;
    if (!canDel) {
      ImGui::BeginDisabled();
    }
    if (bevelButton(a, "##idel", "Delete", ImVec2(A + bw + space::kGap, rowY(0)), ImVec2(A + half, rowY(0) + rh), false,
                    "The selected point goes (never the first, never below two) (Del)") &&
        canDel) {
      s.envFocus = w;
      envDelete(a, c, pan);
    }
    if (!canDel) {
      ImGui::EndDisabled();
    }
    const bool sus = (env.flags & trk::kEnvSustain) != 0, loop = (env.flags & trk::kEnvLoop) != 0;
    if (checkRow(a, "##isus", "Sustain", ImVec2(A, rowY(1)), ImVec2(A + half, rowY(1) + rh), sus, "The envelope holds at its sustain point while the key is down")) {
      tp::opEnvFlag(a, c.t, pan, trk::kEnvSustain);
    }
    const bool susOn = sus && env.count > 0, loopOn = loop && env.count > 0;
    if (!susOn) {
      ImGui::BeginDisabled();
    }
    if (const int dd = stepper(a, "##isusp", "Point", susOn ? twoDigits(env.sustain + 1) : std::string("--"), ImVec2(A, rowY(2)), half, rh, "The sustain point", slw, true);
        dd && susOn) {
      tp::opEnvSustain(a, c.t, pan, std::clamp(env.sustain + dd, 0, env.count - 1));
    }
    if (!susOn) {
      ImGui::EndDisabled();
    }
    if (checkRow(a, "##iloop", "Loop", ImVec2(B, rowY(0)), ImVec2(B + half, rowY(0) + rh), loop, "The envelope repeats between its loop start and end")) {
      tp::opEnvFlag(a, c.t, pan, trk::kEnvLoop);
    }
    if (!loopOn) {
      ImGui::BeginDisabled();
    }
    if (const int dd = stepper(a, "##ils", "Start", loopOn ? twoDigits(env.loopStart + 1) : std::string("--"), ImVec2(B, rowY(1)), half, rh,
                               "The loop's first point (up to its end)", slw, true);
        dd && loopOn) {
      tp::opEnvLoopStart(a, c.t, pan, std::clamp(env.loopStart + dd, 0, static_cast<int>(env.loopEnd)));
    }
    if (const int dd = stepper(a, "##ile", "End", loopOn ? twoDigits(env.loopEnd + 1) : std::string("--"), ImVec2(B, rowY(2)), half, rh,
                               "The loop's last point (from its start)", slw, true);
        dd && loopOn) {
      tp::opEnvLoopEnd(a, c.t, pan, std::clamp(env.loopEnd + dd, static_cast<int>(env.loopStart), env.count - 1));
    }
    if (!loopOn) {
      ImGui::EndDisabled();
    }
    if (tabs) {  // cp22: the envelope shown; each tab's lamp says whether that envelope is on
      const float ty = std::max(rowY(3), p1.y - 3 - rh);
      const char* labels[] = {i18n::N_("Volume"), i18n::N_("Panning")};
      const char* hints[] = {i18n::N_("The volume envelope (Tab: the other one)"), i18n::N_("The panning envelope (Tab: the other one)")};
      for (int k = 0; k < 2; ++k) {
        const float x0 = k == 0 ? A : B, x1 = k == 0 ? A + half : B + half;
        if (bevelButton(a, k == 0 ? "##itabv" : "##itabp", labels[k], ImVec2(x0, ty), ImVec2(x1, ty + rh), k == w, hints[k])) {
          s.envFocus = k;
        }
        const bool kOn = envOf(e, k == 1).on();
        dl->AddRectFilled(ImVec2(x1 - 8, ty + 4), ImVec2(x1 - 4, ty + 8), kOn ? P.led : P.surface950);
      }
    }
  }
  ImGui::PopID();
}

// cp20: the right column's "Extra" page, after the classic instrument screen's extension box: the
// instrument's MIDI out (send its notes, channel, program, bend range, its own sound muted) and the
// Tracker Sampler's playback settings (interpolation, pitch table, rows per beat, ticks per row,
// level, click removal, the built-in sound while it has no sample). Every value is data the
// instrument already has: the MIDI settings through the header's MIDI edits (their undo labels),
// the rest as the plugin's own parameters (as its plugin window sets them).
void insExtraColumn(App& a, Ctx& c, ImVec2 r0, ImVec2 r1, int rows, float rrh, bool compact) {
  St& s = st();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* bold = a.fontBold ? a.fontBold : a.fontRegular;
  // (Wide windows: the rows keep a readable width, the values near their labels.)
  const float rx0 = r0.x + 6, rx1 = std::min(r1.x - 6, rx0 + std::round(fs * 34.0f));
  const float rp = rrh + 2;
  auto rowY = [&](int r) { return std::round(r0.y + 3 + static_cast<float>(r) * rp); };
  auto mid = [&](float y) { return std::round(y + (rrh - fs) * 0.5f); };
  const PluginState* st0 = c.snap->st;
  const PluginInfo* info = c.t.info(a.doc.song());
  auto par = [&](int k) { return st0 && k < static_cast<int>(st0->params.size()) ? static_cast<double>(st0->params[static_cast<size_t>(k)]) : 0.0; };
  auto choiceOf = [&](int k, int v) {
    if (info && k < static_cast<int>(info->params.size())) {
      const auto& ch = info->params[static_cast<size_t>(k)].choices;
      if (v >= 0 && v < static_cast<int>(ch.size())) {
        return std::string(i18n::tr(ch[static_cast<size_t>(v)].c_str()));
      }
    }
    return std::to_string(v);
  };
  auto setPar = [&](int k, double v) {
    a.doc.setParam(c.t.pid(k), v, false);
    a.doc.endGesture();
  };
  (void)rows;
  // Row 0: the title.
  dl->AddText(bold, fs, ImVec2(rx0, mid(rowY(0))), P.text, i18n::tr("Instrument extra"));
  const float half = std::floor((rx1 - rx0 - 4) * 0.5f);
  const float lw = std::round(fs * 6.6f);
  // ---- MIDI out ----
  tp::MidiSettings m = c.snap->e.midi;
  const char* out = a.midi && !a.midi->output.empty() ? a.midi->output.c_str() : nullptr;
  const std::string hint = std::string(i18n::tr("The notes this instrument plays also go to the MIDI output, on its channel and program")) + " (" +
                           (out ? out : i18n::tr("no output is chosen: Settings, MIDI")) + ")";
  if (checkRow(a, "##xmon", "Send notes to the MIDI output", ImVec2(rx0, rowY(1)), ImVec2(rx1, rowY(1) + rrh), m.on, hint.c_str())) {
    m.on = !m.on;
    tp::headerSetMidi(a, c.ch, m, "MIDI output", 0);
  }
  char b[32];
  const float slw = std::round(fs * 4.6f);
  if (!m.on) {
    ImGui::BeginDisabled();
  }
  std::snprintf(b, sizeof b, "%d", m.channel + 1);
  if (const int d = stepper(a, "##xch", "Channel", b, ImVec2(rx0, rowY(2)), half, rrh, "The MIDI channel its notes go out on (1 to 16)", slw, true)) {
    m.channel = std::clamp(m.channel + d, 0, 15);
    tp::headerSetMidi(a, c.ch, m, "MIDI channel", 1);
  }
  std::snprintf(b, sizeof b, "%d", m.program + 1);
  if (const int d = stepper(a, "##xprg", "Program", b, ImVec2(rx0 + half + 4, rowY(2)), rx1 - rx0 - half - 4, rrh, "The program change sent before its notes (1 to 128)", slw,
                            true)) {
    m.program = std::clamp(m.program + d, 0, 127);
    tp::headerSetMidi(a, c.ch, m, "MIDI program", 2);
  }
  if (!m.on) {
    ImGui::EndDisabled();
  }
  std::snprintf(b, sizeof b, "%d", m.bend);
  if (const int d = stepper(a, "##xbend", "Bend", b, ImVec2(rx0, rowY(3)), half, rrh, "Semitones the pitch wheel bends its notes (0 to 36)", slw, true)) {
    m.bend = std::clamp(m.bend + d, 0, 36);
    tp::headerSetMidi(a, c.ch, m, "pitch wheel range", 3);
  }
  if (!m.on) {
    ImGui::BeginDisabled();
  }
  if (checkRow(a, "##xmute", "Mute its own sound", ImVec2(rx0 + half + 4, rowY(3)), ImVec2(rx1, rowY(3) + rrh), m.mute,
               "While its notes go to MIDI, the instrument's own sound is silent (the external synth plays alone)")) {
    m.mute = !m.mute;
    tp::headerSetMidi(a, c.ch, m, "mute instrument", 0);
  }
  if (!m.on) {
    ImGui::EndDisabled();
  }
  // ---- playback ----
  int r = 4;
  if (!compact) {
    const float y = rowY(r++);
    dl->AddText(bold, fs, ImVec2(rx0, mid(y)), P.textDim, i18n::tr("Playback"));
    dl->AddLine(ImVec2(rx0 + bold->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Playback")).x + 8, std::round(y + rrh * 0.5f) + 0.5f),
                ImVec2(rx1, std::round(y + rrh * 0.5f) + 0.5f), theme::withAlpha(P.textFaint, 0.5f));
  }
  auto choiceRow = [&](int row, float x, float w, const char* id, const char* label, int k, int n, const char* hint2, float labelW) {
    const int v = static_cast<int>(std::lround(par(k)));
    if (const int d = stepper(a, id, label, choiceOf(k, v), ImVec2(x, rowY(row)), w, rrh, hint2, labelW, true)) {
      setPar(k, std::clamp(v + d, 0, n - 1));
    }
  };
  choiceRow(r++, rx0, rx1 - rx0, "##xint", "Interpolation", trk::PInterp, 4,
            "How the samples are read between their points: none (gritty), linear, cubic, sinc (the cleanest)", lw);
  choiceRow(r++, rx0, rx1 - rx0, "##xami", "Pitch table", trk::PAmiga, 2, "Linear pitches, or the older period table (slides bend as on old machines)", lw);
  {
    const int y = r++;
    choiceRow(y, rx0, half, "##xrpb", "Rows/beat", trk::PRowsPerBeat, 5, "Pattern rows per beat: how the instrument counts ticks for drawn notes", slw + fs);
    const int tk = static_cast<int>(std::lround(par(trk::PTicks)));
    if (const int d = stepper(a, "##xtk", "Ticks/row", std::to_string(tk), ImVec2(rx0 + half + 4, rowY(y)), rx1 - rx0 - half - 4, rrh,
                              "Ticks per row for notes without tracker cells (drawn or played notes; 1 to 31)", slw + fs, true)) {
      setPar(trk::PTicks, std::clamp(tk + d, 1, 31));
    }
  }
  {
    const int y = r++;
    const double lv = par(trk::PLevel);
    std::snprintf(b, sizeof b, "%d %%", static_cast<int>(std::lround(lv * 100.0)));
    if (const int d = stepper(a, "##xlvl", "Level", b, ImVec2(rx0, rowY(y)), half, rrh, "The instrument's output level (0 to 200 %; Shift: steps of 1 %)", slw, true)) {
      const double stepL = ImGui::GetIO().KeyShift ? 0.01 : 0.05;
      setPar(trk::PLevel, std::clamp(std::round((lv + d * stepL) * 100.0) / 100.0, 0.0, 2.0));
    }
    const bool ramp = par(trk::PRamp) >= 0.5;
    if (checkRow(a, "##xramp", "Click removal", ImVec2(rx0 + half + 4, rowY(y)), ImVec2(rx1, rowY(y) + rrh), ramp,
                 "Short volume ramps where notes start and stop, so they do not click")) {
      setPar(trk::PRamp, ramp ? 0.0 : 1.0);
    }
  }
  {
    const int nSounds = info && trk::PSound < static_cast<int>(info->params.size()) ? static_cast<int>(info->params[trk::PSound].choices.size()) : 1;
    const bool any = tp::sampleCountOf(c.snap->e) > 0;
    choiceRow(r++, rx0, rx1 - rx0, "##xsnd", "Built-in sound", trk::PSound, std::max(1, nSounds),
              any ? "The sound it plays while it has no sample (it has samples now: they play)" : "The sound it plays while it has no sample", lw);
  }
  // The last row: back to the sample's values, and Exit.
  {
    const float y = rowY(r);
    const float exitW = std::round(fs * 4.5f);
    if (bevelButton(a, "##xback", "Sample values", ImVec2(rx0, y), ImVec2(rx1 - exitW - 4, y + rrh), false, "Back to the selected sample's values (Extra again)")) {
      s.insExtra = false;
    }
    if (bevelButton(a, "##xexit", "Exit", ImVec2(rx1 - exitW, y), ImVec2(rx1, y + rrh), false, "Back to the pattern (Esc)")) {
      close(a);
    }
  }
}

// The right column: the selected sample's values, the instrument's fadeout and vibrato, the tuning, the keys.
void insColumn(App& a, Ctx& c, ImVec2 r0, ImVec2 r1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  ImFont* bold = a.fontBold ? a.fontBold : a.fontRegular;
  const tp::Edit& e = c.snap->e;
  const trk::InstrumentDef& d = e.text.def;
  const bool have = c.k >= 0;
  const trk::SampleDef* sd = have ? &d.samples[static_cast<size_t>(c.k)] : nullptr;
  bevelBox(dl, r0, r1, false);
  const float rx0 = r0.x + 6, rx1 = r1.x - 6;
  // Short bands (1024x640): the octave and semitone buttons share one row.
  const bool compact = r1.y - r0.y - 6 < 13.0f * std::round(fs + 3);
  const int rows = compact ? 12 : 13;
  const float rrh = std::clamp(std::floor((r1.y - r0.y - 6) / static_cast<float>(rows)) - 2, std::round(fs + 1), std::round(fs + 7));
  const float rp = rrh + 2;
  auto rowY = [&](int r) { return std::round(r0.y + 3 + static_cast<float>(r) * rp); };
  auto mid = [&](float y) { return std::round(y + (rrh - fs) * 0.5f); };
  // cp20: "Extra" at the end of the title row swaps the column for the instrument's extra settings.
  St& s = st();
  const float xw = std::round(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Extra")).x + fs * 1.4f);
  if (bevelButton(a, "##iextra", "Extra", ImVec2(rx1 - xw, rowY(0)), ImVec2(rx1, rowY(0) + rrh), s.insExtra,
                  "The instrument's extra settings: MIDI out and playback (interpolation, pitch table, ticks, level)")) {
    s.insExtra = !s.insExtra;
  }
  if (s.insExtra) {
    insExtraColumn(a, c, r0, r1, rows, rrh, compact);
    return;
  }
  // Row 0: the sample.
  {
    const std::string t = have ? std::string(i18n::tr("Sample")) + " " + hex2(c.k + 1) + " " + tp::editName(e, c.k) : std::string(i18n::tr("No sample yet"));
    theme::textEllipsis(dl, bold, fs, ImVec2(rx0, mid(rowY(0))), rx1 - rx0 - xw - 6, P.text, t.c_str());
  }
  const float lw = std::round(fs * 6.6f), vw = std::round(fs * 3.4f);
  // A value row: the label, a left arrow, the bar, a right arrow, the value. The arrows step by 1
  // (Shift: 8), the wheel over the bar too; a drag is one undo step (the ops' merge keys).
  auto valueRow = [&](int row, const char* id, const char* label, long v, long lo, long hi, bool centred, long def, const std::string& text, const char* hint, bool enabled,
                      auto&& set) {
    const float y = rowY(row);
    if (!enabled) {
      ImGui::BeginDisabled();
    }
    dl->AddText(a.fontRegular, fs, ImVec2(rx0, mid(y)), enabled ? P.textDim : P.textFaint, i18n::tr(label));
    ImGui::PushID(id);
    const long step = io.KeyShift ? 8 : 1;
    const float bx0 = rx0 + lw, bx1 = rx1 - vw;
    auto put = [&](long nv) {
      nv = std::clamp(nv, lo, hi);
      if (enabled && nv != v) {
        set(nv);
      }
    };
    if (sideArrow(a, "##l", ImVec2(bx0, y), ImVec2(bx0 + rrh, y + rrh), true, hint)) {
      put(v - step);
    }
    if (ImGui::IsItemDeactivated()) {
      a.doc.endGesture();
    }
    double nv = static_cast<double>(v);
    if (valueBar("##b", ImVec2(bx0 + rrh + 1, y), ImVec2(bx1 - rrh - 1, y + rrh), static_cast<double>(v), static_cast<double>(lo), static_cast<double>(hi), centred, &nv, hint)) {
      put(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? def : std::lround(nv));
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0) {
      put(v + (io.MouseWheel > 0 ? step : -step));
      ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
      a.doc.endGesture();
    }
    if (ImGui::IsItemDeactivated()) {
      a.doc.endGesture();
    }
    if (sideArrow(a, "##r", ImVec2(bx1 - rrh, y), ImVec2(bx1, y + rrh), false, hint)) {
      put(v + step);
    }
    if (ImGui::IsItemDeactivated()) {
      a.doc.endGesture();
    }
    ImGui::PopID();
    const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str()).x;
    dl->AddText(mono, fs, ImVec2(std::round(rx1 - tw), mid(y)), enabled ? valueInk() : P.textFaint, text.c_str());
    if (!enabled) {
      ImGui::EndDisabled();
    }
  };
  const long vol = sd ? sd->volume : 0, pn = sd ? sd->pan - 128 : 0, fine = sd ? sd->finetune : 0;
  valueRow(1, "##ivol", "Volume", vol, 0, 64, false, 64, std::to_string(vol), "The sample's own volume (0 to 64; double-click: full)", have,
           [&](long v) { tp::opSampleField(a, c.t, c.k, tp::SampleField::Volume, v); });
  valueRow(2, "##ipan", "Pan", pn, -128, 127, true, 0, signedText(pn), "The sample's own pan (double-click: the middle)", have,
           [&](long v) { tp::opSampleField(a, c.t, c.k, tp::SampleField::Pan, v); });
  valueRow(3, "##ifine", "Fine tune", fine, -128, 127, true, 0, signedText(fine), "The sample's tuning in 128ths of a semitone (double-click: 0)", have,
           [&](long v) { tp::opSampleField(a, c.t, c.k, tp::SampleField::Fine, v); });
  valueRow(4, "##ifade", "Fadeout", d.fadeout, 0, 4095, false, 0, std::to_string(d.fadeout),
           "How fast notes fade once the key is let go (with the volume envelope on); 0: they hold", true, [&](long v) { tp::opInsFadeout(a, c.t, v); });
  valueRow(5, "##irate", "Vibrato speed", d.vibRate, 0, 63, false, 0, std::to_string(d.vibRate), "How fast the automatic vibrato swings", true,
           [&](long v) { tp::opInsVibrato(a, c.t, tp::VibField::Rate, v); });
  valueRow(6, "##idepth", "Vibrato depth", d.vibDepth, 0, 15, false, 0, std::to_string(d.vibDepth), "How far the automatic vibrato swings", true,
           [&](long v) { tp::opInsVibrato(a, c.t, tp::VibField::Depth, v); });
  valueRow(7, "##isweep", "Vibrato sweep", d.vibSweep, 0, 255, false, 0, std::to_string(d.vibSweep), "How long the vibrato takes to reach its depth after a note starts", true,
           [&](long v) { tp::opInsVibrato(a, c.t, tp::VibField::Sweep, v); });
  // Row 8: the vibrato's shape, and Exit.
  {
    const float y = rowY(8);
    dl->AddText(a.fontRegular, fs, ImVec2(rx0, mid(y)), P.textDim, i18n::tr("Vibrato shape"));
    const float exitW = std::round(fs * 4.5f);
    const float bw = std::floor((rx1 - exitW - space::kGroup - (rx0 + lw) + space::kGap) / 4.0f) - space::kGap;  // cp23: the four end 6 px before Exit
    static const char* ids[] = {"##ish0", "##ish1", "##ish2", "##ish3"};
    static const char* hints[] = {"Vibrato shape: sine", "Vibrato shape: square", "Vibrato shape: ramp up", "Vibrato shape: ramp down"};
    for (int i = 0; i < 4; ++i) {
      const float x = rx0 + lw + static_cast<float>(i) * (bw + 2);
      const bool lit = (d.vibType & 3) == i;
      if (bevelButton(a, ids[i], "", ImVec2(x, y), ImVec2(x + bw, y + rrh), lit, hints[i]) && !lit) {
        tp::opInsVibShape(a, c.t, i);
      }
      shapeGlyph(dl, ImVec2(x, y), ImVec2(x + bw, y + rrh), i, lit ? theme::contrastText(P.hot) : P.text);
    }
    if (bevelButton(a, "##iexit", "Exit", ImVec2(rx1 - exitW, y), ImVec2(rx1, y + rrh), false, "Back to the pattern (Esc)")) {
      close(a);
      return;
    }
  }
  // Row 9: the root key (the key that plays the sample at its own rate), the relative note, C-4's rate.
  {
    const float y = rowY(9);
    if (!have) {
      ImGui::BeginDisabled();
    }
    const int root = have ? tp::rootNote(*sd, c.rate) : 49;
    // cp23: the field starts at the bars' column (the left arrows above it; it stood 34 px left of them
    // at 1024x640), its arrows as wide as the steppers' elsewhere.
    const float stW = lw + std::round(fs * 3.4f) + 2 * std::round(rrh * 1.35f) + space::kGap;
    if (const int dd = stepper(a, "##iroot", "Root key", have ? noteName(root) : std::string("---"), ImVec2(rx0, y), stW, rrh,
                               "The key that plays the sample at its own rate (Shift: an octave)", lw, true);
        dd && have) {
      tp::opSampleRoot(a, c.t, c.k, c.rate, std::clamp(root + dd * (io.KeyShift ? 12 : 1), 1, 96));
    }
    if (!have) {
      ImGui::EndDisabled();
    }
    if (have) {
      const double hz = 8363.0 * std::pow(2.0, (sd->relNote + sd->finetune / 128.0) / 12.0);
      const std::string hzT = "C-4 = " + std::to_string(static_cast<long>(std::lround(hz))) + " Hz";
      const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, hzT.c_str()).x;
      std::string rel = std::string(i18n::tr("relative")) + " " + signedText(sd->relNote);
      if (rx0 + stW + space::kGroup + a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, rel.c_str()).x > rx1 - tw - fs) {
        rel = signedText(sd->relNote);  // (narrow: the number alone)
      }
      dl->AddText(a.fontRegular, fs, ImVec2(rx0 + stW + space::kGroup, mid(y)), P.textDim, rel.c_str());
      dl->AddText(mono, fs, ImVec2(std::round(rx1 - tw), mid(y)), valueInk(), hzT.c_str());
    }
  }
  // Rows 10, 11 (compact: row 10 alone): the relative note an octave or a semitone up or down.
  {
    const int cols = compact ? 4 : 2;
    const float half = std::floor((rx1 - rx0 - space::kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols));  // cp23: 2 px apart (were 4)
    const int rel = sd ? sd->relNote : 0;
    auto relBtn = [&](int row, int col, const char* id, const char* label, int dd, const char* hint) {
      if (compact) {
        col += (row - 10) * 2;
        row = 10;
      }
      const float x = rx0 + static_cast<float>(col) * (half + space::kGap);
      const bool can = have && rel + dd >= -48 && rel + dd <= 71;
      if (!can) {
        ImGui::BeginDisabled();
      }
      if (bevelButton(a, id, label, ImVec2(x, rowY(row)), ImVec2(x + half, rowY(row) + rrh), false, hint) && can) {
        tp::opSampleField(a, c.t, c.k, tp::SampleField::RelNote, rel + dd);
        a.doc.endGesture();
      }
      if (!can) {
        ImGui::EndDisabled();
      }
    };
    relBtn(10, 0, "##iocu", compact ? "Oct +" : "Octave up", 12, "The sample plays an octave higher on every key");
    relBtn(10, 1, "##isemu", compact ? "Semi +" : "Semitone up", 1, "The sample plays a semitone higher on every key");
    relBtn(11, 0, "##iocd", compact ? "Oct -" : "Octave down", -12, "The sample plays an octave lower on every key");
    relBtn(11, 1, "##isemd", compact ? "Semi -" : "Semitone down", -1, "The sample plays a semitone lower on every key");
  }
  // Row 12 (compact: 11): the keys.
  {
    const float y = rowY(rows - 1);
    dl->AddText(a.fontRegular, fs, ImVec2(rx0, mid(y)), P.textDim, i18n::tr("Keys"));
    const float kx = rx0 + std::round(fs * 3.2f), half = std::floor((rx1 - kx - space::kGap) * 0.5f);
    if (!have) {
      ImGui::BeginDisabled();
    }
    if (bevelButton(a, "##ievery", "Every key: this one", ImVec2(kx, y), ImVec2(kx + half, y + rrh), false, "Every key of the piano plays the selected sample") && have) {
      tp::opMapKeys(a, c.t, 0, 95, c.k);
    }
    if (bevelButton(a, "##ihear", "Hear the sample", ImVec2(kx + half + space::kGap, y), ImVec2(rx1, y + rrh), false, "Plays the selected sample at its root key") && have &&
        c.snap->inst) {
      startPreview(a, c.ch, *c.snap->inst, e, c.k, tp::rootNote(*sd, c.rate), 0, 0);
    }
    if (!have) {
      ImGui::EndDisabled();
    }
  }
}

// The piano: the key map (the sample of every key), lit while keys sound.
void insPiano(App& a, Ctx& c, ImVec2 k0, ImVec2 k1) {
  St& s = st();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const tp::Edit& e = c.snap->e;
  const trk::InstrumentDef& d = e.text.def;
  const bool have = c.k >= 0;
  bevelBox(dl, k0, k1, true);
  ImVec2 q0(k0.x + 2, k0.y + 2), q1(k1.x - 2, k1.y - 2);
  // cp20: when all eight octaves would make the keys narrow (a short band beside the right column),
  // fewer octaves with wider keys, moved an octave at a time by the arrows at the ends or the wheel.
  KeyView kv;
  {
    const float aw = std::round(fs * 1.3f);
    const float keysW = q1.x - q0.x - 2 * (aw + 2);
    const int octs = std::clamp(static_cast<int>(std::floor(keysW / (7.0f * fs * 1.35f))), 2, 8);
    if ((q1.x - q0.x) / 56.0f < fs * 1.25f && octs < 8) {
      kv.octs = octs;
      if (s.pianoFirst < 0 || s.pianoOcts != octs) {
        s.pianoFirst = 4 - octs / 2;  // (C-4 in the middle)
      }
      const int maxFirst = 8 - octs;
      if (ImGui::IsMouseHoveringRect(k0, k1) && io.MouseWheel != 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        s.pianoFirst += io.MouseWheel > 0 ? -1 : 1;
      }
      s.pianoFirst = std::clamp(s.pianoFirst, 0, maxFirst);
      if (s.pianoFirst <= 0) {
        ImGui::BeginDisabled();
      }
      if (sideArrow(a, "##ipkl", ImVec2(q0.x, q0.y), ImVec2(q0.x + aw, q1.y), true, "Lower octaves (the wheel moves them too)")) {
        s.pianoFirst = std::max(0, s.pianoFirst - 1);
      }
      if (s.pianoFirst <= 0) {
        ImGui::EndDisabled();
      }
      if (s.pianoFirst >= maxFirst) {
        ImGui::BeginDisabled();
      }
      if (sideArrow(a, "##ipkr", ImVec2(q1.x - aw, q0.y), ImVec2(q1.x, q1.y), false, "Higher octaves (the wheel moves them too)")) {
        s.pianoFirst = std::min(maxFirst, s.pianoFirst + 1);
      }
      if (s.pianoFirst >= maxFirst) {
        ImGui::EndDisabled();
      }
      kv.first = s.pianoFirst;
      q0.x += aw + 2;
      q1.x -= aw + 2;
    }
    s.pianoOcts = kv.octs;
  }
  s.pk0 = q0;
  s.pk1 = q1;
  ImGui::SetCursorScreenPos(q0);
  ImGui::InvisibleButton("##ipiano", ImVec2(std::max(1.0f, q1.x - q0.x), std::max(1.0f, q1.y - q0.y)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  const int under = hov || act ? keyUnder(q0, q1, io.MousePos, kv) : -1;
  if (ImGui::IsItemActivated() && io.MouseDown[ImGuiMouseButton_Left] && under >= 0) {
    if (io.KeyAlt) {
      audition(a, c.ch, under);
    } else if (have) {
      s.keyFrom = under;
      s.keyTo = under;
    }
  }
  if (act && io.MouseDown[ImGuiMouseButton_Left]) {
    if (s.keyFrom >= 0 && under >= 0) {
      s.keyTo = under;
    }
    if (s.keyAud >= 0 && under >= 0) {
      audition(a, c.ch, under);
    }
  } else {
    if (s.keyFrom >= 0 && have && ImGui::IsItemDeactivated()) {
      tp::opMapKeys(a, c.t, s.keyFrom, s.keyTo, c.k);  // (once, on release, as the plugin editor)
    }
    s.keyFrom = s.keyTo = -1;
    if (s.keyAud >= 0) {
      audition(a, c.ch, -1);
    }
  }
  if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16 && have) {
    ImGui::OpenPopup("##ipianomenu");
  }
  if (ImGui::BeginPopup("##ipianomenu")) {
    char it[200];
    std::snprintf(it, sizeof it, i18n::tr("Every key plays sample %s"), hex2(std::max(0, c.k) + 1).c_str());
    if (ImGui::MenuItem(it, nullptr, false, have)) {
      tp::opMapKeys(a, c.t, 0, 95, c.k);
    }
    std::snprintf(it, sizeof it, i18n::tr("Keys of sample %s go to the nearest other sample"), hex2(std::max(0, c.k) + 1).c_str());
    if (ImGui::MenuItem(it, nullptr, false, have && tp::sampleCountOf(e) > 1)) {
      tp::opMapKeysAway(a, c.t, c.k);
    }
    ImGui::EndPopup();
  }
  const int lo = std::min(s.keyFrom, s.keyTo), hi = std::max(s.keyFrom, s.keyTo);
  const bool builtin = c.snap->sound > 0 && tp::sampleCountOf(e) == 0;
  const float dfs = std::round(std::clamp(fs * 0.78f, 8.0f, std::max(8.0f, (q1.x - q0.x) / static_cast<float>(kv.octs * 7) * 0.75f)));
  const ImU32 whiteFace = theme::mix(P.text, P.surface600, 0.12f), blackFace = P.surface950;
  for (int pass = 0; pass < 2; ++pass) {
    for (int n = kv.first * 12; n < (kv.first + kv.octs) * 12; ++n) {
      const KeyBox kb = keyBox(q0, q1, n, kv);
      if (kb.black != (pass == 1)) {
        continue;
      }
      const bool picking = s.keyFrom >= 0 && n >= lo && n <= hi;
      const int slot = picking ? c.k : d.keymap[static_cast<size_t>(n)] & 15;
      const bool hasS = builtin || tp::hasSample(e, slot);
      const bool mine = hasS && slot == c.k && !builtin;
      ImU32 face = kb.black ? blackFace : whiteFace;
      if (mine) {
        face = theme::mix(face, P.hot, kb.black ? 0.55f : 0.45f);
      }
      dl->AddRectFilled(kb.p0, ImVec2(kb.p1.x - (kb.black ? 0.0f : 1.0f), kb.p1.y), face);
      if (!kb.black) {
        dl->AddLine(ImVec2(kb.p1.x - 0.5f, kb.p0.y), ImVec2(kb.p1.x - 0.5f, kb.p1.y), P.surface900);
      }
      const float lit = keylight::key(a, c.ch, tp::stripKeyPitch(n));
      if (lit > 0.001f) {
        dl->AddRectFilled(kb.p0, kb.p1, theme::withAlpha(P.led, 0.35f * lit));
        dl->AddRectFilled(ImVec2(kb.p0.x + 1, kb.p1.y - 5), ImVec2(kb.p1.x - 1, kb.p1.y - 1), theme::withAlpha(P.led, std::min(1.0f, lit)));
      }
      if (picking) {
        dl->AddRect(kb.p0, kb.p1, P.hot, 0, 0, 2.0f);
      } else if (n == under && hov) {
        dl->AddRect(kb.p0, kb.p1, theme::withAlpha(P.hot, 0.8f));
      }
      // The sample's number on the key (the built-in sound: "b"; no sample: a dot).
      std::string digit = builtin ? std::string("b") : hasS ? hex2(slot + 1) : std::string("·");
      if (hasS && !builtin && digit.size() == 2 && digit[0] == '0') {
        digit.erase(0, 1);
      }
      const ImU32 dink = kb.black ? (hasS ? P.text : P.textFaint) : (hasS ? P.surface950 : theme::mix(P.surface950, whiteFace, 0.6f));
      const float tw = mono->CalcTextSizeA(dfs, FLT_MAX, 0, digit.c_str()).x;
      const float ty = kb.p1.y - dfs - (kb.black ? 3.0f : 6.0f);
      dl->AddText(mono, dfs, ImVec2(std::round((kb.p0.x + kb.p1.x - tw) * 0.5f), std::round(ty)), dink, digit.c_str());
      if (!kb.black && n % 12 == 0) {  // the octave's name on its C
        const std::string on = "C" + std::to_string(n / 12);
        dl->AddText(a.fontRegular, dfs, ImVec2(kb.p0.x + 2, kb.p0.y + 1), theme::mix(P.surface950, whiteFace, 0.3f), on.c_str());
      }
    }
  }
  if (hov && under >= 0) {
    const int slot = d.keymap[static_cast<size_t>(under)] & 15;
    std::string t = noteName(under + 1) + " " + i18n::tr("plays") + " ";
    if (builtin) {
      t += i18n::tr("the built-in sound");
    } else if (tp::hasSample(e, slot)) {
      t += std::string(i18n::tr("sample")) + " " + hex2(slot + 1) + " " + tp::editName(e, slot);
    } else {
      t += i18n::tr("nothing");
    }
    if (have) {
      t += std::string("  ·  ") + i18n::tr("drag across keys: give them sample") + " " + hex2(c.k + 1);
    }
    t += std::string("  ·  ") + i18n::tr("Alt+click: hear  ·  right-click: more");
    if (kv.octs < 8) {
      t += std::string("  ·  ") + i18n::tr("wheel: other octaves");
    }
    s.hover = t;
  }
}

void instrumentScreen(App& a, ObjectId ch, ImVec2 p0, ImVec2 p1) {
  St& s = st();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  ImFont* bold = a.fontBold ? a.fontBold : a.fontRegular;
  Ctx c = context(a, ch);
  s.hover.clear();
  if (!s.keys.empty()) {
    std::vector<KeyEv> ks;
    ks.swap(s.keys);
    for (const KeyEv& k : ks) {
      if (!shown(a) || !c.ok()) {
        break;
      }
      runInsKey(a, c, k);
      c = context(a, ch);  // (an edit changed the instrument)
    }
    if (!shown(a)) {
      return;
    }
  }
  if (!c.ok()) {
    return;
  }
  if (s.insKey != c.t.key()) {  // another instrument: its own selections
    s.insKey = c.t.key();
    s.envSel[0] = s.envSel[1] = 0;
    s.envDrag[0] = s.envDrag[1] = -1;
    s.envSpan[0] = s.envSpan[1] = 0;
    s.keyFrom = s.keyTo = -1;
  }
  const tp::Edit& e = c.snap->e;
  const trk::InstrumentDef& d = e.text.def;
  const Channel* chan = channelOf(a, ch);
  const bool have = c.k >= 0;
  const float W = p1.x - p0.x;
  const float titleH = std::round(fs + 8), statusH = std::round(fs + 6);
  const float pianoH = std::round(std::clamp((p1.y - p0.y) * 0.15f, fs * 2.4f, fs * 3.6f));
  const float bodyY0 = p0.y + titleH + 2, bodyY1 = std::max(bodyY0 + 80, p1.y - statusH - pianoH - 4);
  const float rightW = std::round(std::clamp(W * 0.32f, fs * 21.0f, fs * 36.0f));
  const float envX1 = p1.x - rightW - 2;
  const float sideW = std::round(std::clamp((envX1 - p0.x) * 0.3f, fs * 15.0f, fs * 22.0f));
  // ---- the title ----
  {
    bevelBox(dl, p0, ImVec2(p1.x, p0.y + titleH), false);
    const float ty = std::round(p0.y + (titleH - fs) * 0.5f);
    dl->AddText(bold, fs, ImVec2(p0.x + 8, ty), P.text, i18n::tr("Instrument editor"));
    const float x = p0.x + 16 + bold->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Instrument editor")).x;
    const int ci = chan ? a.doc.song().channelIndex(ch) : -1;
    std::string info = hex2(ci + trackBase(a)) + " " + (chan ? chan->name : std::string());
    info += "   " + std::to_string(tp::sampleCountOf(e)) + " " + i18n::tr("samples");
    info += "   " + std::string(i18n::tr("fadeout")) + " " + std::to_string(d.fadeout);
    if (d.vibDepth > 0 && d.vibRate > 0) {
      static const char* shapes[] = {"sine", "square", "ramp up", "ramp down"};
      info += "   " + std::string(i18n::tr("vibrato")) + " " + i18n::tr(shapes[d.vibType & 3]);
    } else {
      info += "   " + std::string(i18n::tr("vibrato off"));
    }
    const float stW = std::round(fs * 12);
    const float sx = p1.x - stW - 6;
    theme::textEllipsis(dl, mono, fs, ImVec2(x, ty), sx - x - 8, valueInk(), info.c_str());
    const std::string b = (have ? hex2(c.k + 1) : std::string("00")) + " / " + hex2(tp::sampleCountOf(e));
    if (const int dd = stepper(a, "##isample", "Sample", b, ImVec2(sx, p0.y + 2), stW, titleH - 4, "The instrument's previous / next sample (Alt+Up / Alt+Down)",
                               std::round(fs * 4), true)) {
      tp::headerStepSample(a, ch, -dd);
    }
  }
  // cp20: the envelopes' play positions, read once for both boxes (nothing while no voice sounds).
  s.envLines[0] = s.envLines[1] = 0;
  if (c.snap->inst) {
    tp::TapVoice v[tp::kTapVoices];
    const int n = tp::readPlayTap(c.snap->inst.get(), v, tp::kTapVoices);
    for (int i = 0; i < n; ++i) {
      if (v[i].volTick >= 0) {
        s.envTicks[0][s.envLines[0]++] = v[i].volTick;
      }
      if (v[i].panTick >= 0) {
        s.envTicks[1][s.envLines[1]++] = v[i].panTick;
      }
    }
  }
  // cp22: a short band (1024 x 640) stacked the two envelopes into graphs about three rows tall. When
  // a stacked graph would be under five rows, one envelope takes the whole height (about three times
  // the graph) and two tabs in its side column (and Tab, as before) show the other.
  const float envMid = std::round((bodyY0 + bodyY1) * 0.5f);
  const float stackedGraph = envMid - 1 - bodyY0 - std::round(fs + 7) - 10;
  if (stackedGraph < fs * 5.0f) {
    envBox(a, c, s.envFocus == 1, ImVec2(p0.x, bodyY0), ImVec2(envX1, bodyY1), sideW, true);
  } else {
    envBox(a, c, false, ImVec2(p0.x, bodyY0), ImVec2(envX1, envMid - 1), sideW);
    envBox(a, c, true, ImVec2(p0.x, envMid + 1), ImVec2(envX1, bodyY1), sideW);
  }
  // A short band: the right column runs down beside the piano so all its rows fit.
  const bool tall = bodyY1 - bodyY0 - 6 < 13.0f * std::round(fs + 3);
  const float colY1 = tall ? bodyY1 + 2 + pianoH : bodyY1;
  insColumn(a, c, ImVec2(envX1 + 2, bodyY0), ImVec2(p1.x, colY1));
  if (!shown(a)) {
    return;  // (Exit)
  }
  insPiano(a, c, ImVec2(p0.x, bodyY1 + 2), ImVec2(tall ? envX1 : p1.x, bodyY1 + 2 + pianoH));
  // ---- the status line ----
  {
    const ImVec2 s0(p0.x, p1.y - statusH), s1(p1.x, p1.y);
    bevelBox(dl, s0, s1, true);
    std::string text = s.hover;
    if (text.empty()) {
      text = std::string(i18n::tr("Instrument")) + " " + hex2((chan ? a.doc.song().channelIndex(ch) : 0) + trackBase(a)) + "  ·  " +
             i18n::tr("drag points · double-click deletes · right-click: sustain, loop · Tab: the other envelope · drag across keys: give them") + " " +
             (have ? std::string(i18n::tr("sample")) + " " + hex2(c.k + 1) : std::string(i18n::tr("the selected sample"))) + "  ·  " + i18n::tr("Alt+click: hear");
    }
    const char* keys = s.hasKb ? i18n::tr("Keys: the instrument editor") : i18n::tr("Keys: the pattern");
    const float kw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, keys).x;
    const float ty = std::round(s0.y + (statusH - fs) * 0.5f);
    theme::textEllipsis(dl, mono, std::round(fs * 0.9f), ImVec2(s0.x + 6, ty + 1), s1.x - s0.x - kw - 24, P.textDim, text.c_str());
    dl->AddText(a.fontRegular, fs, ImVec2(s1.x - kw - 8, ty), s.hasKb ? P.text : P.textFaint, keys);
  }
}

// ---- the Channel frame (a channel that is not a Tracker Sampler) ----

int notesOf(App& a, ObjectId ch) {
  St& s = st();
  if (s.notesCh == ch && s.notesRev == a.doc.revision()) return s.notes;
  int n = 0;
  for (const Pattern& p : a.doc.song().patterns)
    if (const auto it = p.notes.find(ch); it != p.notes.end()) n += static_cast<int>(it->second.size());
  s.notesCh = ch, s.notesRev = a.doc.revision(), s.notes = n;
  return n;
}

// HUNT-1119: a key's name as the pattern shows its notes (60 = C-4, flats as Settings has them):
// before, 60 read C-5 here (Highest key G-10), an octave off the pattern's names.
std::string midiNote(const App& a, int n) { return gl::trk::noteText(std::clamp(n, 0, 127), a.settings.getBool("tracker.flats")); }

void convertModal(App& a, ObjectId ch) {
  St& s = st();
  if (s.modal != Modal::Convert) return;
  if (s.modalOpen) {
    ImGui::OpenPopup("##tedconv");
    s.modalOpen = false;
  }
  const float fs = ImGui::GetFontSize(), rh = std::round(fs + 7), w = std::round(fs * 34);
  const float h = std::round(fs * 2.6f + 7 * (fs + 4) + 16 + rh + 16);
  if (!beginModalBox(a, "##tedconv", "Convert to Tracker Sampler", ImVec2(w, h))) {
    s.modal = Modal::None;
    return;
  }
  ++s.modalBegun;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const auto& P = pal();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  float y = o.y + 2;
  auto line = [&](const char* t, ImU32 col) {
    dl->AddText(a.fontRegular, fs, ImVec2(o.x, y), col, i18n::tr(t));
    y += fs + 4;
  };
  line("The Sampler becomes a Tracker Sampler with its file as sample 01 on every key.", P.text);
  line("Carried over: the file, the root key and fine tune, the start and end,", P.textDim);
  line("the loop points and loop type, reverse (as Backwards), the volume and pan,", P.textDim);
  line("and the volume envelope (from the attack, decay, sustain and release).", P.textDim);
  line("Not carried over: time stretch, the filter, the LFO and the", P.textDim);
  line("sampler's other settings; automation of its own knobs is dropped.", P.textDim);
  line("The notes, name, colour, mixer track and channel controls stay. One undo step.", P.textDim);
  const float x1 = o.x + w - 20, bw = std::round(fs * 7), by = std::round(o.y + h - rh - 28 - fs);
  bool go = bevelButton(a, "##cvgo", "Convert", ImVec2(x1 - 2 * bw - 6, by), ImVec2(x1 - bw - 6, by + rh), false, "Enter");
  if (bevelButton(a, "##cvno", "Cancel", ImVec2(x1 - bw, by), ImVec2(x1, by + rh), false, "Esc")) ImGui::CloseCurrentPopup(), s.modal = Modal::None;
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) go = true;
  keepKeysHere();
  if (go) {
    ImGui::CloseCurrentPopup();
    s.modal = Modal::None;
    const Channel* c = channelOf(a, ch);
    if (c && c->plugin.type == "gl.sampler" && !c->plugin.data.empty()) {
      SamplePtr sp = sampleCache().get(c->plugin.data);
      if (!sp || sp->frames() == 0) {
        a.notify(i18n::tr("Convert: the Sampler's file could not be read"), true);
      } else {
        const PluginState ns = tp::fromSamplerState(c->plugin, static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF)), sp->sampleRate,
                                                    fs::u8path(c->plugin.data).stem().u8string());
        const std::string name = c->name;
        a.doc.beginGroup("convert to Tracker Sampler");
        a.doc.setChannelPlugin(ch, trk::kInstrumentType);
        a.doc.editChannel(ch, "convert to Tracker Sampler", [&](Channel& x) {
          x.plugin = ns;
          x.name = name;  // (the name stays, typed or not)
        });
        a.doc.endGroup(false);
        open(a, Editor::Sample, ch);  // (K-391: its Sample editor, wherever the convert was asked)
        status(keys::fillKeys(i18n::N_("Converted to a Tracker Sampler (%s: back)"), {keys::cmdKey("edit.undo", "Ctrl+Z")}));
      }
    }
  }  // (closed by Esc: the box's next frame finds it closed and the modal ends there)
  endModalBox();
}

// cp24: the Channel frame's height: the title line (its help text at the right end) and the box of
// three rows; folded, the title line and the box's first row.
float frameContentH(float fs, bool folded) {
  const int rows = folded ? 1 : 3;
  return std::round(fs + 8) + space::kGap + 2 * space::kPad + static_cast<float>(rows) * std::round(fs + 7) + static_cast<float>(rows - 1) * 3;
}

void channelFrame(App& a, ObjectId id, ImVec2 p0, ImVec2 p1) {
  St& s = st();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  const Song& song = a.doc.song();
  const Channel* c = song.channel(id);
  if (!c) return;
  if (!s.keys.empty()) {
    std::vector<KeyEv> ks;
    ks.swap(s.keys);
    for (const KeyEv& k : ks)
      if (k.sc == SDL_SCANCODE_ESCAPE || ((k.mods & (KMOD_CTRL | KMOD_GUI)) && k.kc == 'w')) {
        close(a);
        return;
      }
  }
  s.hover.clear();
  const PluginInfo* info = findPlugin(c->plugin.type);
  const float titleH = std::round(fs + 8), rowH = std::round(fs + 7), gap = 3;
  const bool folded = frameFolded(a);
  // Title: number, name, the instrument and its kind, the sampler's file; at the right the help line
  // (cp24: it was a status line under the box, a line of the band's height) and the fold button.
  {
    bevelBox(dl, p0, ImVec2(p1.x, p0.y + titleH), false);
    const float ty = std::round(p0.y + (titleH - fs) * 0.5f);
    dl->AddText(a.fontBold ? a.fontBold : a.fontRegular, fs, ImVec2(p0.x + 8, ty), P.text, i18n::tr("Channel"));
    const float x = p0.x + 16 + (a.fontBold ? a.fontBold : a.fontRegular)->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Channel")).x;
    // The fold button: a small bevel button at the line's right end, an arrow on it (up: fold).
    const float fb = titleH - 2 * space::kGap;
    const ImVec2 f0(p1.x - space::kGap - std::round(fb * 1.5f), p0.y + space::kGap), f1(p1.x - space::kGap, p0.y + titleH - space::kGap);
    if (bevelButton(a, "##cffold", "", f0, f1, false,
                    folded ? "Shows the whole frame: pan, fine, output and the key range" : "Folds the frame to its first row; the pattern gets the height")) {
      toggleFrameFold(a);
    }
    {
      const bool held = ImGui::IsItemActive();
      const float cx = std::round((f0.x + f1.x) * 0.5f) + (held ? 1 : 0), cy = std::round((f0.y + f1.y) * 0.5f) + (held ? 1 : 0);
      const float r = std::max(2.0f, std::floor((f1.y - f0.y) * 0.25f));
      const Aliased hard(dl);
      if (!folded) {
        dl->AddTriangleFilled(ImVec2(cx - r - 1, cy + r * 0.6f), ImVec2(cx + r + 1, cy + r * 0.6f), ImVec2(cx, cy - r * 0.8f), P.text);
      } else {
        dl->AddTriangleFilled(ImVec2(cx - r - 1, cy - r * 0.6f), ImVec2(cx + r + 1, cy - r * 0.6f), ImVec2(cx, cy + r * 0.8f), P.text);
      }
    }
    std::string t = hex2(song.channelIndex(id) + trackBase(a)) + " " + c->name + "   " + (info ? info->name : c->plugin.type);
    if (info) t += std::string("   ") + (info->category.empty() ? "" : info->category);
    if (c->plugin.type == "gl.sampler" && !c->plugin.data.empty()) t += "   " + fs::u8path(c->plugin.data).filename().u8string();
    const float tw = std::max(1.0f, std::min(mono->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str()).x + 1, f0.x - space::kGroup - x));
    theme::textEllipsis(dl, mono, fs, ImVec2(x, ty), tw, valueInk(), t.c_str());
    // The help line in what is left, against the fold button (cut short when the line is narrow).
    const char* help = c->plugin.type == "gl.sampler" ? i18n::tr("A Sampler: Sample (the header) opens its audio; Convert makes it a Tracker Sampler with the Sample and Instrument editors")
                                                      : i18n::tr("The channel's controls; the instrument's own controls are in its plugin window");
    const float hs = std::round(fs * 0.9f);
    const float hx0 = x + tw + 4 * space::kGroup, hx1 = f0.x - space::kGroup;
    if (hx1 - hx0 > fs * 6) {
      const float hw = std::min(hx1 - hx0, a.fontRegular->CalcTextSizeA(hs, FLT_MAX, 0, help).x + 1);
      theme::textEllipsis(dl, a.fontRegular, hs, ImVec2(std::round(hx1 - hw), std::round(p0.y + (titleH - hs) * 0.5f)), hw, P.textDim, help);
    }
  }
  // cp23: the controls' box 2 px under the title, its rows 4 px inside it at the top and the bottom
  // alike, the columns 6 px apart. cp24: folded, the box holds its first row only; frameContentH()
  // sizes the band to this.
  const float box0 = p0.y + titleH + space::kGap;
  const float by0 = box0 + space::kPad;
  const float avail = p1.x - p0.x - 2 * space::kPad - 3 * space::kGroup;
  // Four columns: level, pitch and output, keys, the plugin.
  const float cw[4] = {std::floor(avail * 0.31f), std::floor(avail * 0.27f), std::floor(avail * 0.23f), 0};
  const float cx0 = p0.x + space::kPad, cx1 = cx0 + cw[0] + space::kGroup, cx2 = cx1 + cw[1] + space::kGroup, cx3 = cx2 + cw[2] + space::kGroup;
  const float c3w = p1.x - space::kPad - cx3;
  auto rowY = [&](int r) { return std::round(by0 + static_cast<float>(r) * (rowH + gap)); };
  const float box1 = rowY(folded ? 0 : 2) + rowH + space::kPad;
  bevelBox(dl, ImVec2(p0.x, box0), ImVec2(p1.x, box1), false);
  // Volume, pan, mute.
  {
    const float lw = std::round(fs * 4.5f), vw = std::round(fs * 4);
    auto bar = [&](int row, const char* id2, const char* label, double v, double lo, double hi, bool centred, ChannelParam prm, const std::string& text) {
      const float y = rowY(row);
      dl->AddText(a.fontRegular, fs, ImVec2(cx0 + 2, std::round(y + (rowH - fs) * 0.5f)), P.textDim, i18n::tr(label));
      double nv = v;
      if (valueBar(id2, ImVec2(cx0 + lw, y + 1), ImVec2(cx0 + cw[0] - vw, y + rowH - 1), v, lo, hi, centred, &nv, "Drag; double-click: the default"))
        a.doc.setParam(channelParam(id, prm), prm == ChannelParam::Volume && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? 0.78 : nv, true);
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      const float tw = mono->CalcTextSizeA(fs, FLT_MAX, 0, text.c_str()).x;
      dl->AddText(mono, fs, ImVec2(cx0 + cw[0] - tw - 2, std::round(y + (rowH - fs) * 0.5f)), valueInk(), text.c_str());
    };
    char b[32];
    std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(c->gain * 100)));
    bar(0, "##cfvol", "Volume", c->gain, 0.0, 1.25, false, ChannelParam::Volume, b);
    const int pn = static_cast<int>(std::lround(c->pan * 100));
    std::snprintf(b, sizeof b, "%s%d", pn < 0 ? "L" : pn > 0 ? "R" : "C", std::abs(pn));
    if (pn == 0) std::snprintf(b, sizeof b, "C");
    if (!folded) {  // (cp24: folded, the frame shows its first row only)
      bar(1, "##cfpan", "Pan", c->pan, -1.0, 1.0, true, ChannelParam::Pan, b);
      if (checkRow(a, "##cfmute", "Mute", ImVec2(cx0, rowY(2)), ImVec2(cx0 + cw[0], rowY(2) + rowH), c->muted, "The channel is silent")) {
        a.doc.setParam(channelParam(id, ChannelParam::Mute), c->muted ? 0 : 1, false);
      }
    }
  }
  // Pitch, fine, output.
  {
    char b[48];
    // The channel pitch in cents: a step is a semitone (from the nearest one), the readout shows cents.
    const long cents = std::lround(c->pitchCents);
    if (cents % 100 == 0) std::snprintf(b, sizeof b, "%+ld", cents / 100);
    else std::snprintf(b, sizeof b, "%+ld ct", cents);
    if (const int d = stepper(a, "##cfpitch", "Pitch", b, ImVec2(cx1, rowY(0)), cw[1], rowH, "Semitones (-24 to +24); the channel strip's knob sets cents", -1, true)) {
      // HUNT-1119: a pitch between semitones steps to the next whole semitone the way the arrow
      // points (+192 ct: up +2, down +1). Before, it went from the nearest one, so up from +192 ct
      // gave +3 and +2 was skipped.
      const long st = d > 0 ? (cents >= 0 ? cents / 100 : -((-cents + 99) / 100)) : (cents >= 0 ? (cents + 99) / 100 : -(-cents / 100));
      a.doc.setParam(channelParam(id, ChannelParam::Pitch), std::clamp(static_cast<double>((st + d) * 100), -kChannelPitchMaxCents, kChannelPitchMaxCents), false);
    }
  }
  if (!folded) {
    char b[48];
    std::snprintf(b, sizeof b, "%+d ct", c->fine);
    if (const int d = stepper(a, "##cffine", "Fine", b, ImVec2(cx1, rowY(1)), cw[1], rowH, "Cents (-200 to +200); Shift: by 10", -1, true)) {
      const int v = std::clamp(c->fine + d * (io.KeyShift ? 10 : 1), -200, 200);
      // (the channel settings' Fine knob: the same label and merge key)
      a.doc.editChannel(id, "change channel fine", [v](Channel& ch) { ch.fine = v; }, 0x4E53000000000000ull ^ (static_cast<uint64_t>(id) << 8) ^ std::hash<std::string>{}("fine"));
    }
    int mi = 0;
    for (size_t i = 0; i < song.mixer.size(); ++i)
      if (song.mixer[i].id == c->mixerTrack) mi = static_cast<int>(i);
    const std::string out = mi == 0 ? std::string(i18n::tr("Master")) : hex2(mi) + " " + song.mixer[static_cast<size_t>(mi)].name;
    if (const int d = stepper(a, "##cfout", "Output", out, ImVec2(cx1, rowY(2)), cw[1], rowH, "The mixer track the channel plays into", -1, true);
        d && !song.mixer.empty()) {
      const int n = std::clamp(mi + d, 0, static_cast<int>(song.mixer.size()) - 1);
      if (n != mi) a.doc.routeChannel(id, song.mixer[static_cast<size_t>(n)].id);
    }
  }
  // Key range and the notes.
  {
    auto keyRow = [&](int row, const char* id2, const char* label, bool lo) {
      const int v = lo ? c->keyLo : c->keyHi;
      if (const int d = stepper(a, id2, label, midiNote(a, v), ImVec2(cx2, rowY(row)), cw[2], rowH, "Notes outside the range do not play (Shift: an octave)", -1, true)) {
        const int nv = std::clamp(v + d * (io.KeyShift ? 12 : 1), 0, 127);
        a.doc.editChannel(id, "change channel key range", [nv, lo](Channel& ch) {
          if (lo) ch.keyLo = std::min(nv, ch.keyHi);
          else ch.keyHi = std::max(nv, ch.keyLo);
        }, 0x4B52000000000000ull ^ (static_cast<uint64_t>(id) << 8) ^ (lo ? 1u : 2u));
      }
    };
    keyRow(0, "##cfklo", "Lowest key", true);
    if (!folded) {
      keyRow(1, "##cfkhi", "Highest key", false);
      readout(a, "Notes in the song", fmtInt(notesOf(a, id)), ImVec2(cx2, rowY(2)), cw[2], rowH);
    }
  }
  // The plugin.
  {
    if (bevelButton(a, "##cfplug", "Open plugin window", ImVec2(cx3, rowY(0)), ImVec2(cx3 + c3w, rowY(0) + rowH), false,
                    "The instrument's own editor in its own window (never restyled)"))
      openPluginWindow(a, id);
  }
  if (!folded) {
    const bool sampler = c->plugin.type == "gl.sampler" && !c->plugin.data.empty();
    if (!sampler) ImGui::BeginDisabled();
    if (bevelButton(a, "##cfconv", "Convert to Tracker Sampler...", ImVec2(cx3, rowY(1)), ImVec2(cx3 + c3w, rowY(1) + rowH), false,
                    sampler ? "The Sampler's file and settings into a Tracker Sampler (asks first; one undo step)"
                            : "Only a Sampler with one audio file converts (synths and hosted plugins have no faithful mapping)") &&
        sampler) {
      st().modal = Modal::Convert, st().convertCh = id;
      st().modalOpen = true;
    }
    if (!sampler) ImGui::EndDisabled();
    if (bevelButton(a, "##cfexit", "Exit", ImVec2(cx3, rowY(2)), ImVec2(cx3 + c3w, rowY(2) + rowH), false, "Back to the pattern (Esc)")) close(a);
  }
}

}  // namespace


// ---- 0.7.8: a Sampler's Sample editor (the Audio clip window's parts in the band's frame) ----
//
// One Sample editor page for a Sampler and its audio clips: every way into the Audio clip window
// opens it here (ClipEditor.h, trackerpage). The title row (the clip or channel, its file, the
// presets and the window's menu), the page list at the left (the waveform and the window's tabs,
// Exit), the tools row (tools, views, snap, preview, the live analysis, Edit audio...), then the
// waveform, or the tab's modules in a sideways row with Output pinned at the right and the SAMPLE
// strip above them. Every control is the window's own (the same action, undo step, automation and
// link menus, collaboration ops); this frame only places them, in the band's look.

namespace {
void devMenuRowAt(const char* label, ImVec2 a, ImVec2 b) { devMenuRow(label, a.x, a.y, b.x, b.y); }
// (dev) a part as drawn: "clip dump" lists it, "menu at clip <id>" points at it.
void clipPart(const char* id, ImVec2 a, ImVec2 b) {
  st().clipParts.push_back(St::Part{id, a, b});
  devMenuRowAt((std::string("clip ") + id).c_str(), a, b);
}
}  // namespace

void clipScreen(App& a, ObjectId ch, ImVec2 p0, ImVec2 p1) {
  St& s = st();
  namespace tpg = clipeditor::trackerpage;
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  s.clipParts.clear();
  s.hover.clear();
  s.clipBand0 = p0, s.clipBand1 = p1, s.clipFrame = ImGui::GetFrameCount();
  if (!tpg::take(a, ch)) return;
  const tpg::Info in = tpg::info(a);
  if (!in.ok || !tpg::begin(a)) return;
  const float titleH = std::round(fs + 8), rowH = std::round(fs + 7), gap = space::kGap;
  // ---- the title: the clip (or channel), its file; right: the presets, the window's menu ----
  {
    const ImVec2 t1(p1.x, p0.y + titleH);
    bevelBox(dl, p0, t1, false);
    clipPart("title", p0, t1);
    const float ty = std::round(p0.y + (titleH - fs) * 0.5f);
    float x = viewTabs(a, ch, p0, titleH);  // K-391: the pair of views where the title stood
    const float sw = std::round(fs * 0.85f);
    tpg::colour(a, ImVec2(x, std::round(p0.y + (titleH - sw) * 0.5f)), sw);
    x += sw + space::kGroup;
    // Right: Menu, then the preset stepper.
    const float menuW = std::round(a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Menu...")).x + 24);
    const float mx = p1.x - 4 - menuW;
    ImGui::PushID("cpmenu");
    const bool menuClick = bevelButton(a, "##cpmenu", "Menu...", ImVec2(mx, p0.y + 2), ImVec2(p1.x - 4, t1.y - 2), ImGui::IsPopupOpen("##cemenupop"),
                                       "The channel's menu: presets, state, rename, clone, colour, replace or delete it; with a clip, its sample and Audio tools");
    if (menuClick || tpg::takeMenuAsk()) ImGui::OpenPopup("##cemenupop");
    ImGui::SetNextWindowPos(ImVec2(p1.x - 4, t1.y), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##cemenupop")) {
      tpg::menuItems(a);
      ImGui::EndPopup();
    }
    ImGui::PopID();
    clipPart("menu", ImVec2(mx, p0.y + 2), ImVec2(p1.x - 4, t1.y - 2));
    const float stW = std::round(fs * 13);
    const float sx = mx - space::kGroup - stW;
    const std::string pv = s.clipPreset.empty() ? std::string("--") : s.clipPreset;
    if (const int d = stepper(a, "##cpreset", "Preset", pv, ImVec2(sx, p0.y + 2), stW, titleH - 4, "The sampler's previous / next preset", std::round(fs * 4), true)) {
      s.clipPreset = tpg::presetStep(a, d > 0 ? 1 : -1);
    }
    clipPart("presets", ImVec2(sx, p0.y + 2), ImVec2(sx + stW, t1.y - 2));
    // Before the presets: the mixer track the channel plays through (the box and the free track's
    // button, as a plugin window's header has them), when the title row has the room.
    float infoEnd = sx - space::kGroup;
    {
      const float bh = titleH - 4, rw = std::round(fs * 12);
      const float rx = sx - space::kGroup - rw - 2 - bh;
      if (rx - x >= std::round(fs * 16) || tpg::reachAll()) {
        tpg::route(a, ImVec2(rx, p0.y + 2), rw, bh);
        clipPart("route", ImVec2(rx, p0.y + 2), ImVec2(rx + rw + 2 + bh, t1.y - 2));
        infoEnd = rx - space::kGroup;
      }
    }
    // Left, after the swatch: what it shows, in the monospaced face.
    const int ci = a.doc.song().channelIndex(ch);
    std::string info = std::string(i18n::tr("Instrument")) + " " + hex2(ci + trackBase(a)) + " " + in.channelName;
    if (in.clip != kNoId) info += std::string("   ") + i18n::tr("Clip") + " " + in.name;
    info += "   " + (in.file.empty() ? std::string(i18n::tr("No sample")) : in.file);
    if (!in.meta.empty()) info += "   " + in.meta;
    if (in.warpMarkers > 0) info += std::string("   ") + std::to_string(in.warpMarkers) + " " + i18n::tr("warp markers");
    if (in.shared > 1) {
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("shared by %d clips"), in.shared);
      info += std::string("   ") + b;
    }
    if (in.moreSelected > 0) {
      char b[64];
      std::snprintf(b, sizeof b, i18n::tr("and %d more selected"), in.moreSelected);
      info += std::string("   ") + b;
    }
    theme::textEllipsis(dl, mono, fs, ImVec2(x, ty), std::max(0.0f, infoEnd - x), valueInk(), info.c_str());
  }
  // ---- the page list: the waveform, the window's tabs (those with modules), Exit ----
  const std::vector<tpg::Module> mods = tpg::modules(a);
  std::vector<int> tabs;
  for (size_t i = 0; i + 1 < mods.size(); ++i)
    if (std::find(tabs.begin(), tabs.end(), mods[i].tab) == tabs.end()) tabs.push_back(mods[i].tab);
  std::sort(tabs.begin(), tabs.end());
  int tab = tpg::tab();
  if (tab != s.clipTabSeen) {  // a tab asked for (a way in, the dev commands): its page, from its start
    s.clipTabSeen = tab;
    s.clipWave = std::find(tabs.begin(), tabs.end(), tab) == tabs.end();  // (Edit audio, Analysis: the tools row, by the waveform)
  }
  const float top = p0.y + titleH + gap;
  float colW = labelColumn(a, {i18n::N_("Waveform"), i18n::N_("Clip"), i18n::N_("Warp"), i18n::N_("Sample"), i18n::N_("Playback"), i18n::N_("Envelope"),
                               i18n::N_("Filter"), i18n::N_("Sample FX"), i18n::N_("Channel"), i18n::N_("Note effects"), i18n::N_("MIDI and macros"),
                               i18n::N_("Edit audio...")},
                           fs + 2 * space::kGroup + 2 * space::kPad);
  colW = std::max(colW, std::round(fs * 8));
  {
    const ImVec2 c0(p0.x, top), c1(p0.x + colW, p1.y);
    bevelBox(dl, c0, c1, false);
    clipPart("pages", c0, c1);
    float y = c0.y + space::kPad;
    const float rx0 = c0.x + space::kPad, rx1 = c1.x - space::kPad;
    // Every row in the column at once: Waveform, the tabs, Play and Edit audio..., Exit; tighter rows
    // (never under the text's height) when the band is short.
    const int rows = 2 + static_cast<int>(tabs.size()) + (s.clipWave ? 0 : in.clip != kNoId ? 2 : 1);
    const float avail = c1.y - c0.y - 2 * space::kPad - space::kGroup;
    const float rh = std::max(std::round(fs + 3), std::min(rowH, std::floor(avail / static_cast<float>(rows)) - space::kLine));
    const float grp = rh < rowH ? space::kGap : space::kGroup;
    if (radioRow(a, "##cpwave", "Waveform", ImVec2(rx0, y), ImVec2(rx1, y + rh), s.clipWave, "The clip's waveform: trim, fades, the volume line, warp markers, the selection")) {
      s.clipWave = true;
    }
    clipPart("page Waveform", ImVec2(rx0, y), ImVec2(rx1, y + rh));
    y += rh + space::kLine;
    for (int t : tabs) {
      ImGui::PushID(t);
      const char* name = clipwin::tabName(t);
      char hint[128];
      std::snprintf(hint, sizeof hint, i18n::tr("The %s settings"), i18n::tr(name));
      if (radioRow(a, "##cptab", name, ImVec2(rx0, y), ImVec2(rx1, y + rh), !s.clipWave && t == tab, hint)) {
        tpg::setTab(t);
        s.clipTabSeen = tab = t;
        s.clipWave = false;
      }
      ImGui::PopID();
      clipPart((std::string("page ") + name).c_str(), ImVec2(rx0, y), ImVec2(rx1, y + rh));
      y += rh + space::kLine;
    }
    // The settings pages leave the tools row to the waveform (the band's height goes to the modules):
    // the preview and Edit audio... sit here, as the classic screen's play buttons by its pages.
    y += grp;
    if (!s.clipWave) {
      if (in.clip != kNoId) {
        if (bevelButton(a, "##cpplay", a.engine.previewing() ? "Stop" : "Play", ImVec2(rx0, y), ImVec2(rx1, y + rh), a.engine.previewing(),
                        "Preview: the clip alone (its channel and effects), from its start (Alt+P)"))
          tpg::preview(a);
        clipPart("play", ImVec2(rx0, y), ImVec2(rx1, y + rh));
        y += rh + space::kLine;
      }
      const bool editAsk = bevelButton(a, "##cpedit", "Edit audio...", ImVec2(rx0, y), ImVec2(rx1, y + rh), false,
                                       in.clip != kNoId ? "The audio operations (Loupe's, on this clip's sample)" : "The audio operations (Loupe's, on the sample)");
      tpg::editAudioMenu(a, editAsk, ImVec2(rx1, y));
      clipPart("edit audio", ImVec2(rx0, y), ImVec2(rx1, y + rh));
      y += rh + space::kLine;
    }
    // Exit, at the foot.
    const float ey = c1.y - space::kPad - rh;
    if (ey > y + space::kGap) {
      if (bevelButton(a, "##cpexit", "Exit", ImVec2(rx0, ey), ImVec2(rx1, ey + rh), false, "Closes the Sample editor (Esc)")) a.later([&a] { close(a); });
      clipPart("exit", ImVec2(rx0, ey), ImVec2(rx1, ey + rh));
    }
  }
  const float rx0 = p0.x + colW + gap, rx1 = p1.x, rw = rx1 - rx0;
  if (s.clipWave) {
    // ---- the tools row, then the waveform (with the SAMPLE strip under it) in a sunken well ----
    const float toolH = std::round(rowH + 8);
    tpg::tools(a, ImVec2(rx0, top), rw, toolH);
    clipPart("tools", ImVec2(rx0, top), ImVec2(rx1, top + toolH));
    const float body = top + toolH + gap;
    const ImVec2 w0(rx0, body), w1(rx1, p1.y);
    bevelBox(dl, w0, w1, true);
    tpg::wave(a, ImVec2(w0.x + 2, w0.y + 2), ImVec2(w1.x - 2, w1.y - 2), std::round(std::clamp((w1.y - w0.y) * 0.16f, 22.0f, 32.0f)));
    clipPart("wave", w0, w1);
  } else {
    // ---- the tab's modules (Output pinned at the right) as the window lays them, in a raised panel;
    // the SAMPLE strip above when the band has the room; compact modules in a short band ----
    const float body = top;
    const float pad = space::kPad;
    const float fullH = tpg::tabHeight(a, rw - 2 * pad, false);
    const float bodyH = p1.y - body;
    const bool compact = fullH < 0 || fullH + 2 * pad > bodyH;
    const float panelH = std::min(bodyH, (compact ? std::max(0.0f, tpg::tabHeight(a, rw - 2 * pad, true)) : fullH) + 2 * pad);
    const float modY = p1.y - std::max(panelH, std::min(bodyH, tpg::moduleHeight(compact) + 2 * pad));
    if (modY - gap - body >= 20) {
      tpg::overview(a, ImVec2(rx0, body), ImVec2(rx1, modY - gap));
      clipPart("overview", ImVec2(rx0, body), ImVec2(rx1, modY - gap));
    }
    const float panelTop = modY - gap - body >= 20 ? modY : body;
    bevelBox(dl, ImVec2(rx0, panelTop), ImVec2(rx1, p1.y), false);
    clipPart("modules", ImVec2(rx0, panelTop), ImVec2(rx1, p1.y));
    for (const tpg::Laid& m : tpg::tabPanel(a, ImVec2(rx0 + pad, panelTop + pad), rw - 2 * pad, p1.y - panelTop - 2 * pad, compact))
      clipPart((std::string("module ") + m.id).c_str(), ImVec2(std::max(m.a.x, rx0), std::max(m.a.y, panelTop)), ImVec2(std::min(m.b.x, rx1), std::min(m.b.y, p1.y)));
  }
  tpg::end(a);
  (void)P;
}

// ---- navigation ----

bool hasSampleEditor(App& a, ObjectId channel) {
  const Channel* c = channelOf(a, channel);
  return isTrackerSampler(c) || isSampler(c) || seltt::isSeltmann(a, channel);
}

bool showsClipForm(App& a) { return shown(a) && isSampler(channelOf(a, a.settingsChannel)) && st().want == Editor::Sample; }

// K-391: the Tracker's own screen for `e` exists for this channel: the Sample editor (a Tracker
// Sampler, a Sampler, Seltmann's Chop) or the Instrument screen (a Tracker Sampler's, Seltmann's).
bool bandScreen(App& a, Editor e, ObjectId channel) {
  const Channel* c = channelOf(a, channel);
  if (!c) return false;
  if (e == Editor::Sample) return hasSampleEditor(a, channel);
  return isTrackerSampler(c) || seltt::isSeltmann(a, channel) || st().devFrame;
}

void open(App& a, Editor e, ObjectId channel, int slot, bool keys) {
  const Channel* c = channelOf(a, channel);
  if (!c) return;
  St& s = st();
  // K-391: no screen of the Tracker's own for it (another instrument's Instrument): its editor
  // floats in its window, as in Studio; nothing of a plugin is drawn in the band.
  if (!bandScreen(a, e, channel)) {
    openPluginWindow(a, channel);
    return;
  }
  s.want = e;
  s.openedFrame = ImGui::GetFrameCount();
  s.giveKeys = keys;
  s.keyFrames = 3;
  if (a.settingsChannel != kNoId && a.settingsChannel != channel) seltt::leave(a, a.settingsChannel);  // (its auditions stop)
  a.selChannel = channel;
  a.settingsChannel = channel;
  s.bandCh = channel;
  if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = true;  // (the band shows)
  if (slot >= 0 && isTrackerSampler(c)) tp::headerSelectSample(a, channel, slot);
  if (trackerview::extended()) trackerview::setExtended(a, false);
}

void follow(App& a, ObjectId channel) {
  St& s = st();
  if (!bandHolds(a) || channel == kNoId || channel == a.settingsChannel) return;
  if (!bandScreen(a, s.want, channel)) {  // (another kind of instrument: no such screen; nothing pops up)
    close(a);
    return;
  }
  if (a.settingsChannel != kNoId) seltt::leave(a, a.settingsChannel);
  a.settingsChannel = channel;
  s.bandCh = channel;
}

bool bandHolds(App& a) { return a.settingsChannel != kNoId && st().bandCh == a.settingsChannel; }

void toggle(App& a, Editor e, bool keys) {
  if (showing(a, e)) {
    close(a);
    return;
  }
  if (a.selChannel != kNoId) open(a, e, a.selChannel, -1, keys);
}

void close(App& a) {
  St& s = st();
  stopPreview(a);
  releaseHeld(a);
  s.keys.clear();
  s.drag = 0;
  if (a.settingsChannel != kNoId) seltt::leave(a, a.settingsChannel);
  a.settingsChannel = kNoId;
  s.bandCh = kNoId;
  s.devFrame = false;
  if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = false;
  s.shown = Shown::None;
  s.hasKb = false;
  keysBackToPattern();
}

bool shown(App& a) { return a.settingsChannel != kNoId && !pluginWindowOpen(a); }
Editor wanted() { return st().want; }

bool showing(App& a, Editor e) {
  if (!shown(a)) return false;
  const Channel* c = channelOf(a, a.settingsChannel);
  if (!isTrackerSampler(c) && !isSampler(c) && !seltt::isSeltmann(a, a.settingsChannel)) return e == Editor::Instrument;  // (the Channel frame stands for the instrument's editor)
  return st().want == e;
}

namespace {
void noteHostedAsked(ObjectId channel);  // (the window watch, below)
}  // namespace

void openPluginWindow(App& a, ObjectId channel) {
  const Channel* c = channelOf(a, channel);
  if (!c) return;
  a.selChannel = channel;
  if (isHostedType(c->plugin.type) && !hostedui::opensInside(a, PluginTarget{channel, -1})) {  // (0.7.8: inside Gridline it is the channel window's)
    noteHostedAsked(channel);
    if (openHostedEditor(a, PluginTarget{channel, -1})) return;
    // (K-391: not loaded, or no interface of its own: its channel settings window, floating, shows
    // the stand-in or the parameters)
  }
  // The channel settings window, floating as in Studio (a Sampler's: its Audio clip window); the
  // lower band closes if it showed this channel's screen.
  St& s = st();
  stopPreview(a);
  if (bandHolds(a)) {
    releaseHeld(a);
    s.keys.clear(), s.drag = 0, s.shown = Shown::None, s.hasKb = false;
    if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = false;
    keysBackToPattern();
  }
  s.bandCh = kNoId;
  s.devFrame = false;
  a.settingsChannel = channel;
  a.focusWindow = isSampler(c) ? "Audio clip" : "###channelsettings";
}
// K-391: in the Tracker the channel settings float unless the band holds them for its own screen.
bool pluginWindowOpen(App& a) {
  St& s = st();
  if (s.bandCh != kNoId && a.settingsChannel != s.bandCh) s.bandCh = kNoId, s.devFrame = false;
  return layoutui::active() == layout::Preset::Tracker && a.settingsChannel != kNoId && s.bandCh == kNoId;
}
// The Tracker in front at all (not only once its shell is built: a switch into it must not flash the
// window for the frame before the shell exists either).
bool ownsChannelWindow(App& a) { return layoutui::active() == layout::Preset::Tracker && !pluginWindowOpen(a); }

namespace {
struct WindowWatch {
  int frames = 0;   // frames watched in the Tracker
  int shown = 0;    // ... a channel settings or effect editor window was up that nothing asked for
  int hosted = 0;   // ... an installed plugin's own window for a channel was open that nothing asked for
  int blocked = 0;  // ... the gate kept the channel settings window closed (a path that asked for it)
  int first = -1;   // the first frame one showed
  std::string what;
  std::vector<ObjectId> askedHosted;  // the channels "Open plugin window" opened an installed plugin's window for
  bool bandLast = false;              // K-391: the band held the channel settings on the last frame
  int bandRun = 0;                    // K-391: frames in a row the lower band was up with none of the Tracker's screens
  int bandForeign = 0;                // ... frames of such runs past their first (the band drew something not ours)
};
WindowWatch& watch() {
  static WindowWatch w;
  return w;
}
void noteHostedAsked(ObjectId channel) {
  std::vector<ObjectId>& v = watch().askedHosted;
  if (std::find(v.begin(), v.end(), channel) == v.end()) {
    v.push_back(channel);
  }
}
}  // namespace

void watchWindows(App& a, bool blocked) {
  (void)a;
  if (layoutui::active() != layout::Preset::Tracker) {
    return;
  }
  WindowWatch& w = watch();
  ++w.frames;
  if (blocked) {
    ++w.blocked;
  }
  // (last frame's windows: this runs before the channel settings window's Begin)
  const ImGuiWindow* cs = ImGui::FindWindowByName("###channelsettings");
  const ImGuiWindow* fx = ImGui::FindWindowByName("###fxeditor");
  // K-391: the windows float when asked for (the channel settings, an effect's editor); counted is a
  // window that was up while the band held its channel for the Tracker's own screen.
  const bool csUp = cs && cs->WasActive && !cs->Hidden && w.bandLast;
  const bool fxUp = false;
  (void)fx;
  w.bandLast = bandHolds(a);
  // K-391: the lower band up without one of our screens: a plugin drawn there (its first frame is the
  // one it closes on)
  const ImGuiWindow* dc = ImGui::FindWindowByName("###Device chain");
  if (dc && dc->WasActive && !dc->Hidden && !shown(a)) {
    if (++w.bandRun > 1) ++w.bandForeign;
  } else {
    w.bandRun = 0;
  }
  bool hostedUp = false;
  for (const PluginTarget& t : openHostedEditorTargets()) {
    if (t.slot < 0 && std::find(w.askedHosted.begin(), w.askedHosted.end(), t.owner) == w.askedHosted.end()) {
      hostedUp = true;
    }
  }
  if (csUp || fxUp) {
    ++w.shown;
  }
  if (hostedUp) {
    ++w.hosted;
  }
  if ((csUp || fxUp || hostedUp) && w.first < 0) {
    w.first = ImGui::GetFrameCount() - 1;
    w.what = csUp ? "channel settings" : fxUp ? "effect editor" : "plugin window";
  }
}

float need(App& a) {
  (void)a;
  const St& s = st();
  return ImGui::GetFrameCount() - s.drawnFrame <= 2 &&
                 (s.shown == Shown::Sample || s.shown == Shown::Frame || s.shown == Shown::Instrument || s.shown == Shown::Chop || s.shown == Shown::SeltInst || s.shown == Shown::Clip)
             ? s.need
             : 0.0f;
}

void setFrameRoom(float room) { st().frameRoom = room; }

float frameWholeNeed() { return frameContentH(ImGui::GetFontSize(), false) + st().bandPad; }

bool frameFolded(App& a) {
  const int mode = static_cast<int>(a.settings.getNumber("tracker.frameFold"));
  if (mode == 1 || mode == 2) {
    return mode == 1;
  }
  return st().frameRoom < frameWholeNeed();
}

void toggleFrameFold(App& a) {
  // The person's choice; the one the window would make anyway goes back to "by the window".
  const bool want = !frameFolded(a);
  const bool byWindow = st().frameRoom < frameWholeNeed();
  a.settings.set("tracker.frameFold", static_cast<double>(want == byWindow ? 0 : want ? 1 : 2));
}

void tend(App& a) {
  St& s = st();
  tendPreview(a);
  const bool gone = ImGui::GetFrameCount() - s.drawnFrame > 2;
  if (gone) {
    if (s.serial || s.pvSample) stopPreview(a);
    if (!s.held.empty()) releaseHeld(a);
    s.hasKb = false;
  }
  // K-391: Convert to Tracker Sampler's confirm (the instrument list's menu, the dev Channel frame)
  if (s.modal == Modal::Convert && s.convertCh != kNoId) convertModal(a, s.convertCh);
}

void askConvert(App& a, ObjectId channel) {
  const Channel* c = channelOf(a, channel);
  if (!c || c->plugin.type != "gl.sampler" || c->plugin.data.empty()) return;
  St& s = st();
  s.modal = Modal::Convert, s.modalOpen = true, s.convertCh = channel;
}

bool host(App& a, ObjectId channel) {
  if (!trackerOn(a)) return false;
  St& s = st();
  const int frame = ImGui::GetFrameCount();
  if (pluginWindowOpen(a)) {  // the channel settings float: the band closes
    if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = false;
    return true;
  }
  const Channel* c = channelOf(a, channel);
  if (!c) {
    close(a);
    return true;
  }
  // K-391: the band open with no screen asked for (a layout restored with it open, View > Device
  // chain): the current instrument's own screen when it has one, else the band closes (its plugin's
  // editor floats only when asked for).
  if (s.bandCh != channel || a.settingsChannel != channel) {
    const Editor e = isSampler(c) ? Editor::Sample : Editor::Instrument;
    if (!bandScreen(a, e, channel)) {
      if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = false;
      return true;
    }
    open(a, e, channel);
  }
  const float fs = ImGui::GetFontSize();
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const ImVec2 p1(p0.x + av.x, p0.y + std::max(av.y, 40.0f));
  // K-391: a Sampler's Sample page: the classic Sample editor (its file as a one-sample Tracker Sampler) or the advanced one.
  const bool bridged = isSampler(c) && s.want == Editor::Sample && !advancedView(a);
  const bool sampleEd = (isTrackerSampler(c) && s.want == Editor::Sample) || bridged;
  const bool insEd = isTrackerSampler(c) && s.want == Editor::Instrument;  // cp19: the Tracker's own Instrument editor (was the plugin's editor)
  const bool clipEd = isSampler(c) && s.want == Editor::Sample && advancedView(a);  // 0.7.8: a Sampler's Sample editor (the Audio clip window's parts)
  // K-347: a Seltmann's own screens (Chop where the Sample editor sits, its Instrument screen).
  const bool selt = seltt::isSeltmann(a, channel);
  const bool chopEd = selt && s.want == Editor::Sample, seltIns = selt && s.want == Editor::Instrument;
  s.drawnFrame = frame;
  {
    const PerfScope perf(PerfEditor);
    s.shown = sampleEd ? Shown::Sample : insEd ? Shown::Instrument : chopEd ? Shown::Chop : seltIns ? Shown::SeltInst : clipEd ? Shown::Clip : Shown::Frame;
    const float workH = ImGui::GetMainViewport()->WorkSize.y;
    // (a small window: a shorter well, so the pattern keeps its nine rows under the folded header)
    if (sampleEd) {
      s.need = std::max(std::round(fs * 16.5f), std::round(workH * (workH < 760 ? 0.36f : 0.43f)));
    } else if (insEd) {
      s.need = std::max(std::round(fs * 19.0f), std::round(workH * (workH < 760 ? 0.36f : 0.43f)));
    } else if (chopEd || seltIns) {
      s.need = seltt::need(chopEd ? seltt::ScreenChop : seltt::ScreenInstrument, fs, workH);
    } else if (clipEd) {
      // (the most the band gives: the window's modules want their full height; the pattern keeps its rows)
      s.need = std::max(std::round(fs * 21.5f), std::round(workH * 0.62f));
    } else {
      // cp23: the Channel frame's own height (channelFrame()). The band's window adds its own padding
      // round the content (the band is sized whole). cp24: folded, its title and first row.
      s.bandPad = std::max(0.0f, std::ceil(ImGui::GetWindowHeight() - av.y));
      s.need = frameContentH(fs, frameFolded(a)) + s.bandPad;
    }
    s.hasKb = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
    if (sampleEd) {
      const Bridge br(bridged);
      sampleScreen(a, channel, p0, p1);
    } else if (insEd) {
      instrumentScreen(a, channel, p0, p1);
    } else if (chopEd) {
      seltt::chopScreen(a, channel, p0, p1, s.hasKb);
    } else if (seltIns) {
      seltt::instrumentScreen(a, channel, p0, p1, s.hasKb);
    } else if (clipEd) {
      clipScreen(a, channel, p0, p1);
    } else {
      channelFrame(a, channel, p0, p1);
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(av.x, std::max(1.0f, p1.y - p0.y)));
    // The part with the keys has a thin frame in the hot colour.
    if (s.hasKb) {
      ImGui::GetWindowDrawList()->AddRect(p0, p1, theme::withAlpha(pal().hot, 0.8f));
    }
  }
  // A press in the band, noted before the focus check below: the press itself brings the keys here
  // (TRK-1118: noted after it, the first click on a button in the band, "Open plugin window" among
  // them, sent the keys back to the pattern, whose focus next frame took the press from the button,
  // so its release never clicked it).
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
      (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) {
    s.pressFrame = frame;
  }
  // The keys after an open: with the screen when its key opened it, else back with the pattern.
  if (s.keyFrames > 0) {
    --s.keyFrames;
    if (s.giveKeys && s.shown != Shown::Plugin) ImGui::SetWindowFocus();
    else keysBackToPattern();
  } else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    if (!s.wasFocused && frame - s.pressFrame > 2) {
      // cp25 (keyboard session): the keys came here without a press or an editor key: a window
      // closing over the Tracker (the plugin picker's Enter adding an instrument) hands ImGui's focus
      // to the band, and Esc then closed the frame instead of turning edit mode on. Back to the pattern.
      if (a.focusWindow == "###channelsettings") {
        a.focusWindow.clear();
      }
      keysBackToPattern();
    } else {
      keepKeysHere();  // (the band's buttons hand the keys to the pattern; inside the editor they stay)
    }
  }
  s.wasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  hostbody::claim(hostbody::Body::ChannelSettings, "Device chain");
  return true;
}

// ---- keys ----

bool hasKeys() {
  const St& s = st();
  return s.hasKb && ImGui::GetFrameCount() - s.drawnFrame <= 1 &&
         (s.shown == Shown::Sample || s.shown == Shown::Frame || s.shown == Shown::Instrument || s.shown == Shown::Chop || s.shown == Shown::SeltInst || s.shown == Shown::Clip);
}

bool sdlKey(App& a, int sc, int kc, int mods, bool down, bool repeat) {
  St& s = st();
  if (!down) {
    if (const auto it = s.held.find(sc); it != s.held.end()) {
      if (it->second.channel != kNoId) a.engine.noteOff(it->second.channel, it->second.pitch);
      s.held.erase(it);
    }
    return false;
  }
  if (!hasKeys()) return false;
  const bool ctrl = (mods & (KMOD_CTRL | KMOD_GUI)) != 0, alt = (mods & KMOD_ALT) != 0;
  if (ctrl && alt) {
    // 0.7.8: the previous / next sample entry, empty ones too (the pattern's keys for it as well).
    if (!(mods & KMOD_SHIFT) && (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN) && (s.shown == Shown::Sample || s.shown == Shown::Instrument)) {
      stopPreview(a);
      tp::headerStepEntry(a, a.selChannel, sc == SDL_SCANCODE_UP ? -1 : 1);
      return true;
    }
    return false;
  }
  if (sc == SDL_SCANCODE_SPACE || (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)) return false;
  if (sc >= SDL_SCANCODE_LCTRL && sc <= SDL_SCANCODE_RGUI) return false;
  // K-347: Seltmann's screens. Esc and Cmd/Ctrl+W close an open box, else the screen; the screen's own
  // keys (tracker.chop, tracker.seltmann) queue for its next draw; the note keys play the regions.
  if (s.shown == Shown::Chop || s.shown == Shown::SeltInst) {
    const int scr = s.shown == Shown::Chop ? seltt::ScreenChop : seltt::ScreenInstrument;
    if (sc == SDL_SCANCODE_ESCAPE || (ctrl && kc == 'w')) {
      if (sc == SDL_SCANCODE_ESCAPE) keys::claimEscape();
      if (repeat) return true;
      if (!seltt::closeInner(a, a.settingsChannel)) close(a);
      return true;
    }
    if (ctrl) return false;
    if (seltt::takesKey(scr, sc, kc, mods)) {
      seltt::queueKey(sc, kc, mods);
      return true;
    }
    if (alt) return false;
    const int key = TypingKeyboard::keyAtPosition(sc);
    const int off = key > 0 ? TypingKeyboard::keyOffset(key) : -1;
    if (off >= 0) {
      if (repeat || s.held.count(sc)) return true;
      const int pitch = a.typing.baseNote() + off;
      const ObjectId ch = a.settingsChannel;
      if (ch != kNoId && pitch >= 0 && pitch <= 127) {
        a.engine.noteOn(ch, pitch, std::clamp(a.typing.velocity, 1, 127) / 127.0f);
        s.held[sc] = {ch, pitch};
      }
      return true;
    }
    return true;  // (the screen's keys: no single-key command runs while it has them)
  }
  // 0.7.8: a Sampler's Sample editor (the Audio clip window's parts): Esc and Cmd/Ctrl+W close it;
  // its own keys (Alt+P the preview, Alt+W warp, Shift+W the spectrum) reach it through ImGui; the note
  // keys play the channel; the other commands (undo, the transport) stay the app's.
  if (s.shown == Shown::Clip) {
    if (sc == SDL_SCANCODE_ESCAPE || (ctrl && kc == 'w')) {
      if (sc == SDL_SCANCODE_ESCAPE) keys::claimEscape();
      if (!repeat) close(a);
      return true;
    }
    if (ctrl) return false;
    if (alt) return kc == 'p' || kc == 'w';
    const int key = TypingKeyboard::keyAtPosition(sc);
    const int off = key > 0 && !(mods & KMOD_SHIFT) ? TypingKeyboard::keyOffset(key) : -1;
    if (off >= 0) {
      if (repeat || s.held.count(sc)) return true;
      const int pitch = a.typing.baseNote() + off;
      const ObjectId ch = a.settingsChannel;
      if (ch != kNoId && pitch >= 0 && pitch <= 127) {
        a.engine.noteOn(ch, pitch, std::clamp(a.typing.velocity, 1, 127) / 127.0f);
        s.held[sc] = {ch, pitch};
      }
      return true;
    }
    return true;
  }
  // cp19: the Instrument editor's keys (K-316 4.3): Tab the other envelope, Left / Right the point,
  // Up / Down its level, Shift+Left / Right its tick, Insert or Ctrl+Enter adds, Delete deletes,
  // Alt+Up / Down the sample, Esc and Cmd/Ctrl+W close; the note keys play the instrument.
  if (s.shown == Shown::Instrument) {
    if (ctrl) {
      if (kc == 'w' || sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
        if (!repeat || kc != 'w') {
          s.keys.push_back({sc, kc, mods});
        }
        return true;
      }
      return false;
    }
    if (alt) {
      if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN) {
        s.keys.push_back({sc, kc, mods});
        return true;
      }
      return false;
    }
    if (sc == SDL_SCANCODE_ESCAPE) {
      keys::claimEscape();
      s.keys.push_back({sc, kc, mods});
      return true;
    }
    if (sc == SDL_SCANCODE_TAB || sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT || sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN ||
        sc == SDL_SCANCODE_INSERT || sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_BACKSPACE) {
      s.keys.push_back({sc, kc, mods});
      return true;
    }
  }
  if (ctrl) {
    if (kc == 'a' || kc == 'x' || kc == 'c' || kc == 'v' || kc == 'w') {
      if (!repeat) s.keys.push_back({sc, kc, mods});
      return true;
    }
    return false;
  }
  if (alt) {
    if (kc == 'a' || kc == 'x' || kc == 'c' || kc == 'v' || kc == 'r' || kc == 's' || kc == 'z' || kc == 'p' || kc == 'q' || sc == SDL_SCANCODE_UP ||
        sc == SDL_SCANCODE_DOWN) {
      s.keys.push_back({sc, kc, mods});
      return true;
    }
    return false;
  }
  if (sc == SDL_SCANCODE_ESCAPE) {
    keys::claimEscape();
    s.keys.push_back({sc, kc, mods});
    return true;
  }
  // The Channel frame: its other keys play the channel on the typing keyboard, as without it.
  if (s.shown == Shown::Frame) return false;
  if (sc == SDL_SCANCODE_DELETE || sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT) {
    s.keys.push_back({sc, kc, mods});
    return true;
  }
  // The typing keyboard's note keys: the instrument plays (never a pattern write); the play note follows.
  if (s.shown == Shown::Sample || s.shown == Shown::Instrument) {
    const int key = TypingKeyboard::keyAtPosition(sc);
    const int off = key > 0 ? TypingKeyboard::keyOffset(key) : -1;
    if (off >= 0) {
      if (repeat || s.held.count(sc)) return true;
      const int pitch = a.typing.baseNote() + off;
      const ObjectId ch = a.settingsChannel;
      if (ch != kNoId && pitch >= 0 && pitch <= 127) {
        a.engine.noteOn(ch, pitch, std::clamp(a.typing.velocity, 1, 127) / 127.0f);
        s.held[sc] = {ch, pitch};
        tp::memory(PluginTarget{ch, -1}.key()).playNote = std::clamp(pitch - trk::kPitchOffset, 1, 96);
      }
      return true;
    }
  }
  return true;  // (the screen's keys: no single-key command runs while it has them)
}

bool closeRequest(App& a) {
  if (!hasKeys()) return false;
  close(a);
  return true;
}


// ---- dev: the Instrument editor ("tracker editors ins ...") ----
namespace {
ImVec2 gDevPointer;
bool gDevPointerSet = false;
std::map<std::string, std::pair<ImVec2, ImVec2>> gDevMenuRows;  // TRK-1118: the instrument menu's rows, by label

std::string envText(const trk::Envelope& v) {
  std::string t = "f" + std::to_string(v.flags) + " n" + std::to_string(v.count) + " s" + std::to_string(v.sustain) + " l" + std::to_string(v.loopStart) + "-" +
                  std::to_string(v.loopEnd) + " ";
  for (int i = 0; i < v.count; ++i) {
    t += (i ? "," : "") + std::to_string(v.x[static_cast<size_t>(i)]) + ":" + std::to_string(v.y[static_cast<size_t>(i)]);
  }
  return t;
}

void insDev(App& a, Ctx& c, const std::vector<std::string>& args, int frame) {
  St& s = st();
  auto word = [&](size_t i) { return args.size() > i ? args[i] : std::string(); };
  auto num = [&](size_t i, long long d) { return args.size() > i ? std::atoll(args[i].c_str()) : d; };
  const std::string w = word(2);
  if (w == "dump") {
    if (!c.ok()) {
      std::printf("tracker editors frame %d: ins none\n", frame);
      std::fflush(stdout);
      return;
    }
    const tp::Edit& e = c.snap->e;
    const trk::InstrumentDef& d = e.text.def;
    std::string map;
    for (int n = 0; n < 96; ++n) {
      map += "0123456789ABCDEF"[d.keymap[static_cast<size_t>(n)] & 15];
    }
    const trk::SampleDef* sd = c.k >= 0 ? &d.samples[static_cast<size_t>(c.k)] : nullptr;
    std::printf("tracker editors frame %d: ins shown %d screen %d slot %d vol [%s] pan [%s] fade %d vib %d %d %d %d smp %d %d %d %d map %s sel %d %d focus %d keys %d "
                "need %.0f envlines %d %d pos %zu undo \"%s\"\n",
                frame, shown(a) ? 1 : 0, static_cast<int>(s.shown), c.k, envText(d.vol).c_str(), envText(d.pan).c_str(), d.fadeout, d.vibType, d.vibSweep, d.vibDepth,
                d.vibRate, sd ? sd->volume : -1, sd ? sd->pan - 128 : 0, sd ? sd->finetune : 0, sd ? sd->relNote : 0, map.c_str(), s.envSel[0], s.envSel[1], s.envFocus,
                hasKeys() ? 1 : 0, static_cast<double>(need(a)), s.envLines[0], s.envLines[1], a.doc.history().position(), a.doc.history().canUndo() ? a.doc.history().undoLabel().c_str() : "");
    std::fflush(stdout);
    return;
  }
  // The pointer placed over a part of the screen (then "down 0" / "up 0" press as a hand would).
  if (w == "at") {
    ImVec2 p(-1, -1);
    if (word(3) == "key") {
      const int n = std::clamp(static_cast<int>(num(4, 48)), 0, 95);
      KeyView kv;
      kv.octs = s.pianoOcts;
      kv.first = kv.octs < 8 ? std::clamp(s.pianoFirst, 0, 8 - kv.octs) : 0;
      if (!kv.has(n)) {  // (a short band: the octaves move to the key, as the arrows would)
        kv.first = std::clamp(n / 12 - kv.octs / 2, 0, 8 - kv.octs);
        s.pianoFirst = kv.first;
      }
      const KeyBox kb = keyBox(s.pk0, s.pk1, n, kv);
      p = ImVec2((kb.p0.x + kb.p1.x) * 0.5f, kb.black ? (kb.p0.y + kb.p1.y) * 0.5f : kb.p1.y - 4);
    } else if (word(3) == "env") {
      const int i = word(4) == "pan" ? 1 : 0;
      const float span = s.envSpan[i] > 0 ? s.envSpan[i] : 64.0f;
      p = ImVec2(s.envG0[i].x + (s.envG1[i].x - s.envG0[i].x) * static_cast<float>(num(5, 0)) / span,
                 s.envG1[i].y - (s.envG1[i].y - s.envG0[i].y) * static_cast<float>(num(6, 0)) / 64.0f);
    }
    if (p.x >= 0) {
      gDevPointer = p;
      gDevPointerSet = true;
    }
    return;
  }
  if (!c.ok()) {
    return;
  }
  if (w == "env") {  // ins env vol|pan add <x> <y> | move <i> <x> <y> | end | delete <i> | addafter <i> | sustain <i> | loop <a> <b> | flag on|sustain|loop | preset <k> | sel <i>
    const bool pan = word(3) == "pan";
    const int ix = pan ? 1 : 0;
    const std::string op = word(4);
    if (op == "add") {
      const int i = tp::opEnvPointAdd(a, c.t, pan, static_cast<int>(num(5, 16)), static_cast<int>(num(6, 32)));
      if (i >= 0) {
        s.envSel[ix] = i;
      }
      a.doc.endGesture();
    } else if (op == "move") {
      tp::opEnvPointMove(a, c.t, pan, static_cast<int>(num(5, 0)), static_cast<int>(num(6, 0)), static_cast<int>(num(7, 0)));
    } else if (op == "end") {
      a.doc.endGesture();
    } else if (op == "delete") {
      s.envSel[ix] = static_cast<int>(num(5, s.envSel[ix]));
      envDelete(a, c, pan);
    } else if (op == "addafter") {
      s.envSel[ix] = static_cast<int>(num(5, s.envSel[ix]));
      envAdd(a, c, pan);
    } else if (op == "sustain") {
      tp::opEnvSustain(a, c.t, pan, static_cast<int>(num(5, 0)));
    } else if (op == "loop") {
      tp::opEnvLoopStart(a, c.t, pan, static_cast<int>(num(5, 0)));
      tp::opEnvLoopEnd(a, c.t, pan, static_cast<int>(num(6, 0)));
    } else if (op == "flag") {
      const std::string f = word(5);
      tp::opEnvFlag(a, c.t, pan, static_cast<uint8_t>(f == "sustain" ? trk::kEnvSustain : f == "loop" ? trk::kEnvLoop : trk::kEnvOn));
    } else if (op == "preset") {
      const int k = std::clamp(static_cast<int>(num(5, 0)), 0, tp::kEnvSlots - 1);
      const tp::EnvSlots& sl = tp::envSlots(a);
      tp::opEnvApplySlot(a, c.t, pan, (pan ? sl.pan : sl.vol)[static_cast<size_t>(k)]);
    } else if (op == "sel") {
      s.envSel[ix] = static_cast<int>(num(5, 0));
      s.envFocus = ix;
    }
    return;
  }
  if (w == "extra") {  // ins extra on|off | ins extra set interp|amiga|rpb|ticks|level|ramp|sound <v>: the right column's Extra page
    if (word(3) == "set") {
      if (c.ok()) {
        static const std::pair<const char*, int> fields[] = {{"interp", trk::PInterp}, {"amiga", trk::PAmiga}, {"rpb", trk::PRowsPerBeat}, {"ticks", trk::PTicks},
                                                             {"level", trk::PLevel},   {"ramp", trk::PRamp},   {"sound", trk::PSound}};
        for (const auto& [n, k] : fields) {
          if (word(4) == n) {
            a.doc.setParam(c.t.pid(k), std::atof(word(5).c_str()), false);
            a.doc.endGesture();
          }
        }
      }
      return;
    }
    s.insExtra = word(3) != "off";
    return;
  }
  if (w == "keys") {  // as a drag across the piano: one op on release
    if (c.k >= 0) {
      tp::opMapKeys(a, c.t, static_cast<int>(num(3, 0)), static_cast<int>(num(4, 95)), c.k);
    }
  } else if (w == "everykey") {
    if (c.k >= 0) {
      tp::opMapKeys(a, c.t, 0, 95, c.k);
    }
  } else if (w == "away") {
    if (c.k >= 0) {
      tp::opMapKeysAway(a, c.t, c.k);
    }
  } else if (w == "set") {  // ins set volume|pan|fine|rel|fadeout|rate|depth|sweep|shape <v> (as a bar's drag: merged until "ins env vol end")
    const std::string f = word(3);
    const long v = static_cast<long>(num(4, 0));
    if (f == "fadeout") {
      tp::opInsFadeout(a, c.t, v);
    } else if (f == "rate") {
      tp::opInsVibrato(a, c.t, tp::VibField::Rate, v);
    } else if (f == "depth") {
      tp::opInsVibrato(a, c.t, tp::VibField::Depth, v);
    } else if (f == "sweep") {
      tp::opInsVibrato(a, c.t, tp::VibField::Sweep, v);
    } else if (f == "shape") {
      tp::opInsVibShape(a, c.t, static_cast<int>(v));
    } else if (c.k >= 0) {
      const tp::SampleField sf = f == "pan" ? tp::SampleField::Pan : f == "fine" ? tp::SampleField::Fine : f == "rel" ? tp::SampleField::RelNote : tp::SampleField::Volume;
      tp::opSampleField(a, c.t, c.k, sf, v);
    }
  }
}
}  // namespace

// ---- dev ----

void devMenuRow(const char* label, float x0, float y0, float x1, float y1) { gDevMenuRows[label] = {ImVec2(x0, y0), ImVec2(x1, y1)}; }

bool takeDevPointer(float* x, float* y) {
  if (seltt::takeDevPointer(x, y)) return true;  // K-347: "tracker editors selt at <control>"
  if (!gDevPointerSet) {
    return false;
  }
  gDevPointerSet = false;
  *x = gDevPointer.x;
  *y = gDevPointer.y;
  return true;
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  St& s = st();
  const Bridge br(bandHolds(a) && s.want == Editor::Sample && classicSampler(a, a.settingsChannel));  // (K-391: the classic editor on a Sampler)
  auto arg = [&](size_t i, long long d) { return args.size() > i ? std::atoll(args[i].c_str()) : d; };
  const std::string cmd = args.size() > 1 ? args[1] : "dump";
  const Song& song = a.doc.song();
  auto chanAt = [&](size_t i) {
    if (args.size() > i && !args[i].empty() && (args[i][0] < '0' || args[i][0] > '9')) {  // cp20: a channel by its name
      for (const Channel& ch : song.channels) {
        if (ch.name == args[i]) {
          return ch.id;
        }
      }
      return a.selChannel;
    }
    const long long idx = arg(i, -1);
    return idx >= 0 && idx < static_cast<long long>(song.channels.size()) ? song.channels[static_cast<size_t>(idx)].id : a.selChannel;
  };
  if (cmd == "open") {
    const std::string w = args.size() > 2 ? args[2] : "sample";
    s.devFrame = w == "frame";  // (K-391: the Channel frame only from here; an instrument's editor floats)
    open(a, w == "instrument" || w == "frame" ? Editor::Instrument : Editor::Sample, chanAt(3), static_cast<int>(arg(4, -1)), true);
    return;
  }
  if (cmd == "close") {
    close(a);
    return;
  }
  if (cmd == "files") {  // K-435: presets and WAV files of a Tracker Sampler (TrackerSampleFiles.h)
    trackerfiles::devCommand(a, args, frame);
    return;
  }
  // 0.7.8: a Sampler's Sample editor: "clip page wave|<tab>" (the page list's rows), "clip dump" (its parts as drawn).
  if (cmd == "clip") {
    const std::string what = args.size() > 2 ? args[2] : "dump";
    if (what == "page" && args.size() > 3) {
      if (args[3] == "wave") {
        s.clipWave = true;
      } else {
        clipeditor::trackerpage::setTab(std::atoi(args[3].c_str()));
        s.clipTabSeen = clipeditor::trackerpage::tab();
        s.clipWave = false;
      }
      return;
    }
    std::string parts;
    char b[200];
    for (const St::Part& q : s.clipParts) {
      std::snprintf(b, sizeof b, " | %s %.0f %.0f %.0f %.0f", q.id.c_str(), static_cast<double>(q.a.x), static_cast<double>(q.a.y), static_cast<double>(q.b.x),
                    static_cast<double>(q.b.y));
      parts += b;
    }
    std::printf("tracker editors clip frame %d: shown %d drawn %d wave %d tab %d channel %llu band %.0f %.0f %.0f %.0f parts %d%s\n", frame, showsClipForm(a) ? 1 : 0,
                frame - s.clipFrame <= 2 ? 1 : 0, s.clipWave ? 1 : 0, clipeditor::trackerpage::tab(), static_cast<unsigned long long>(a.settingsChannel),
                static_cast<double>(s.clipBand0.x), static_cast<double>(s.clipBand0.y), static_cast<double>(s.clipBand1.x), static_cast<double>(s.clipBand1.y),
                static_cast<int>(s.clipParts.size()), parts.c_str());
    std::fflush(stdout);
    return;
  }
  if (cmd == "selt") {  // K-347: Seltmann's screens (seltmann_tracker.h)
    seltt::devCommand(a, args, frame);
    return;
  }
  if (cmd == "windows") {  // cp22: "windows" reports, "windows reset" starts the count again
    WindowWatch& w = watch();
    if (args.size() > 2 && args[2] == "reset") {
      w = WindowWatch{};
      return;
    }
    std::printf("tracker windows frame %d: watched %d shown %d hosted %d blocked %d first %d %s plugin %s band %s\n", frame, w.frames, w.shown, w.hosted,
                w.blocked, w.first, w.what.empty() ? "-" : w.what.c_str(), pluginWindowOpen(a) ? "open" : "closed",
                shown(a) ? (s.shown == Shown::Sample ? "sample" : s.shown == Shown::Instrument ? "instrument" : s.shown == Shown::Chop ? "chop" : s.shown == Shown::SeltInst ? "seltinst" : s.shown == Shown::Clip ? "clip" : "frame") : "closed");
    std::fflush(stdout);
    return;
  }
  if (cmd == "float") {  // K-391: each editor window up (floating or docked), the lower band, frames it drew a plugin
    auto win = [](const char* name) {
      const ImGuiWindow* w = ImGui::FindWindowByName(name);
      return !w || !w->WasActive || w->Hidden ? "down" : w->DockIsActive ? "docked" : "floating";
    };
    std::printf("tracker float frame %d: settings %s fx %s clip %s master %s hosted %d band %s foreign %d\n", frame, win("###channelsettings"),
                win("###fxeditor"), win("###Audio clip"), win("###Master chain"), static_cast<int>(openHostedEditorTargets().size()),
                shown(a) ? "ours" : "closed", watch().bandForeign);
    std::fflush(stdout);
    return;
  }
  if (cmd == "focus") {
    if (ImGuiWindow* w = ImGui::FindWindowByName("###Device chain")) ImGui::FocusWindow(w);
    return;
  }
  if (cmd == "menu" && args.size() > 3 && args[2] == "at") {  // TRK-1118: menu at <label> (the words may be quoted)
    std::string label;
    for (size_t i = 3; i < args.size(); ++i) label += (i > 3 ? " " : "") + args[i];
    const auto it = gDevMenuRows.find(label);
    if (it == gDevMenuRows.end()) {
      std::printf("tracker editors frame %d: menu row \"%s\" none\n", frame, label.c_str());
    } else {
      gDevPointer = ImVec2((it->second.first.x + it->second.second.x) * 0.5f, (it->second.first.y + it->second.second.y) * 0.5f);
      gDevPointerSet = true;
      std::printf("tracker editors frame %d: menu row \"%s\" at %.0f %.0f\n", frame, label.c_str(), static_cast<double>(gDevPointer.x),
                  static_cast<double>(gDevPointer.y));
    }
    std::fflush(stdout);
    return;
  }
  if (cmd == "plugin") {
    openPluginWindow(a, chanAt(2));
    return;
  }
  if (cmd == "load" && args.size() > 2) {  // as Load... picks the file (into the selected slot)
    const ObjectId ch = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
    if (tp::headerLoadSample(a, ch, args[2]) < 0) std::printf("tracker editors frame %d: load failed\n", frame);
    return;
  }
  const ObjectId ch = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
  Ctx c = context(a, ch);
  if (cmd == "ins") {
    insDev(a, c, args, frame);
    return;
  }
  if (cmd == "dump" || cmd == "geometry") {
    const Channel* chan = channelOf(a, ch);
    const tp::Span r = rangeOf(c);
    tp::TapVoice v[tp::kTapVoices];
    int lines = 0;
    if (c.ok() && c.snap->inst) {
      const int n = tp::readPlayTap(c.snap->inst.get(), v, tp::kTapVoices);
      for (int i = 0; i < n; ++i) lines += v[i].slot == c.k ? 1 : 0;
    }
    std::printf("tracker editors frame %d: shown %d screen %d want %d channel %d tracker %d slot %d frames %d start %d end %d range %d %d view %.0f %.0f "
                "loop %d %d %d bits %d clip %d note %d previewing %d lines %d keys %d need %.0f pluginwin %d modal %d %d pos %zu undo \"%s\"\n",
                frame, shown(a) ? 1 : 0, static_cast<int>(s.shown), static_cast<int>(s.want), song.channelIndex(ch), isTrackerSampler(chan) ? 1 : 0, c.k,
                c.frames, c.s0, c.s1, r.empty() ? -1 : r.a, r.empty() ? -1 : r.b, c.m ? c.m->viewStart : 0.0, c.m ? c.m->viewLen : 0.0,
                c.have() ? c.snap->e.text.def.samples[static_cast<size_t>(c.k)].loop : -1, c.ls, c.le - c.ls, c.eight ? 8 : 16, tp::clipboard().frames(),
                c.m ? c.m->playNote : 0, previewSounding(a) ? 1 : 0, lines, hasKeys() ? 1 : 0, static_cast<double>(need(a)),
                pluginWindowOpen(a) ? 1 : 0, static_cast<int>(s.modal), s.modalBegun, a.doc.history().position(), a.doc.history().canUndo() ? a.doc.history().undoLabel().c_str() : "");
    {  // 0.7.8: the entry picked, the entries used, the cut and keys of each, and the play lines drawn
      std::string ent, lx;
      if (c.ok())
        for (int k = 0; k < trk::kMaxSamples; ++k) {
          if (!tp::hasSample(c.snap->e, k)) continue;
          int keys = 0;
          for (uint8_t m : c.snap->e.text.def.keymap) keys += m == k ? 1 : 0;
          char b[96];
          std::snprintf(b, sizeof b, " %02X:%d-%d/%d", k + 1, c.snap->e.cuts.start[static_cast<size_t>(k)], c.snap->e.cuts.end[static_cast<size_t>(k)], keys);
          ent += b;
        }
      for (const auto& [x, f] : s.tapLines) lx += " " + std::to_string(static_cast<int>(x)) + "@" + std::to_string(f);
      std::printf("tracker editors frame %d: entry %d well %.0f %.0f %.0f %.0f used%s lines%s\n", frame, c.entry, static_cast<double>(s.well[0]),
                  static_cast<double>(s.well[1]), static_cast<double>(s.well[2]), static_cast<double>(s.well[3]), ent.c_str(), lx.c_str());
    }
    if (chan && !isTrackerSampler(chan))
      std::printf("tracker editors frame %d: channel gain %.3f pan %.3f pitch %.0f fine %d mute %d out %llu keys %d %d type %s names %s %s\n", frame,
                  static_cast<double>(chan->gain), static_cast<double>(chan->pan), static_cast<double>(chan->pitchCents), chan->fine, chan->muted ? 1 : 0,
                  static_cast<unsigned long long>(chan->mixerTrack), chan->keyLo, chan->keyHi, chan->plugin.type.c_str(), midiNote(a, chan->keyLo).c_str(),
                  midiNote(a, chan->keyHi).c_str());
    if (cmd == "geometry" || frame < 0) {
      long long built = 0, served = 0;
      tp::snapshotCounts(&built, &served);
      std::printf("tracker editors frame %d: columns built %lld snapshots built %lld served %lld\n", frame, s.cols.builds, built, served);
    }
    std::fflush(stdout);
    return;
  }
  if (cmd == "fold") {  // cp24: "fold" as the frame's fold button; "fold show" reports
    if (args.size() < 3 || args[2] != "show") {
      toggleFrameFold(a);
    }
    std::printf("tracker editors frame %d: fold %d setting %d room %.0f whole %.0f need %.0f\n", frame, frameFolded(a) ? 1 : 0,
                static_cast<int>(a.settings.getNumber("tracker.frameFold")), static_cast<double>(s.frameRoom), static_cast<double>(frameWholeNeed()),
                static_cast<double>(s.need));
    std::fflush(stdout);
    return;
  }
  if (cmd == "frame") {  // the Channel frame's controls, as their steppers and bars set them
    const Channel* chan = channelOf(a, ch);
    if (!chan) return;
    const std::string w = args.size() > 2 ? args[2] : "";
    const double v = args.size() > 3 ? std::atof(args[3].c_str()) : 0.0;
    if (w == "volume") a.doc.setParam(channelParam(ch, ChannelParam::Volume), v, false);
    else if (w == "pan") a.doc.setParam(channelParam(ch, ChannelParam::Pan), v, false);
    else if (w == "pitch") a.doc.setParam(channelParam(ch, ChannelParam::Pitch), v, false);
    else if (w == "mute") a.doc.setParam(channelParam(ch, ChannelParam::Mute), v, false);
    else if (w == "fine") {
      const int f = static_cast<int>(v);
      a.doc.editChannel(ch, "change channel fine", [f](Channel& x) { x.fine = f; }, 0x4E53000000000000ull ^ (static_cast<uint64_t>(ch) << 8) ^ std::hash<std::string>{}("fine"));
    } else if (w == "output") {
      const auto i = static_cast<size_t>(v);
      if (i < song.mixer.size()) a.doc.routeChannel(ch, song.mixer[i].id);
    } else if (w == "keylo" || w == "keyhi") {
      const int n = static_cast<int>(v);
      const bool lo = w == "keylo";
      a.doc.editChannel(ch, "change channel key range", [n, lo](Channel& x) {
        if (lo) x.keyLo = std::min(n, x.keyHi);
        else x.keyHi = std::max(n, x.keyLo);
      }, 0x4B52000000000000ull ^ (static_cast<uint64_t>(ch) << 8) ^ (lo ? 1u : 2u));
    }
    return;
  }
  if (cmd == "convert") {  // the confirm's Convert
    s.modal = Modal::Convert, s.convertCh = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
    s.modalOpen = true;
    // (pressed on the next frame the modal draws: the dev "key enter")
    return;
  }
  if (!c.ok()) return;
  if (cmd == "range") {
    c.m->selSlot = c.k, c.m->selA = static_cast<int32_t>(arg(2, -1)), c.m->selB = static_cast<int32_t>(arg(3, -1));
  } else if (cmd == "pin" && c.have()) {
    tp::opSampleMarker(a, c.t, c.k, static_cast<int>(arg(2, 0)), static_cast<int32_t>(arg(3, 0)), c.frames);
    a.doc.endGesture();
  } else if (cmd == "view" && c.have()) {
    c.m->viewLen = std::clamp(static_cast<double>(arg(3, c.frames)), std::min<double>(c.frames, 64.0), static_cast<double>(c.frames));
    c.m->viewStart = std::clamp(static_cast<double>(arg(2, 0)), 0.0, c.frames - c.m->viewLen);
  } else if (cmd == "zoom" && c.have()) {
    const std::string w = args.size() > 2 ? args[2] : "";
    if (w == "in") zoomAbout(c, 0.5, c.m->viewStart + c.m->viewLen * 0.5);
    else if (w == "out") zoomAbout(c, 2.0, c.m->viewStart + c.m->viewLen * 0.5);
    else if (w == "all") wholeView(c);
    else if (w == "range") zoomToRange(c);
    else if (w == "at") zoomAbout(c, args.size() > 4 && args[4] == "out" ? 1.25 : 0.8, static_cast<double>(arg(3, 0)));
  } else if (cmd == "scroll" && c.have()) {
    scrollBy(c, static_cast<double>(arg(2, 0)));
  } else if (cmd == "op") {
    static const std::pair<const char*, tp::HeaderEdit> ops[] = {
        {"cut", tp::HeaderEdit::Cut},           {"copy", tp::HeaderEdit::Copy},       {"paste", tp::HeaderEdit::Paste},       {"crop", tp::HeaderEdit::Crop},
        {"delete", tp::HeaderEdit::Delete},     {"silence", tp::HeaderEdit::Silence}, {"reverse", tp::HeaderEdit::Reverse},   {"normalise", tp::HeaderEdit::Normalise},
        {"fadein", tp::HeaderEdit::FadeIn},     {"fadeout", tp::HeaderEdit::FadeOut}, {"fixdc", tp::HeaderEdit::FixDc},       {"minimise", tp::HeaderEdit::TrimLoop},
        {"selectall", tp::HeaderEdit::SelectAll}, {"clearrange", tp::HeaderEdit::ClearRange}};
    const std::string w = args.size() > 2 ? args[2] : "";
    for (const auto& [n, e] : ops)
      if (w == n) edit(a, c, e);
  } else if (cmd == "loop" && c.have()) {
    const std::string w = args.size() > 2 ? args[2] : "off";
    tp::opSampleLoopType(a, c.t, c.k, static_cast<uint8_t>(w == "forward" ? trk::kLoopForward : w == "pingpong" ? trk::kLoopPingPong : trk::kLoopOff), c.s0, c.s1);
  } else if (cmd == "backwards" && c.have()) {  // 0.7.8: backwards on | off
    tp::opSampleReverse(a, c.t, c.k, !(args.size() > 2 && args[2] == "off"));
  } else if (cmd == "bits" && c.have()) {
    tp::opSampleBits(a, c.t, c.k, arg(2, 16) == 8);
  } else if (cmd == "note") {
    c.m->playNote = std::clamp(static_cast<int>(arg(2, 49)), 1, 96);
  } else if (cmd == "play" && c.have()) {
    const std::string w = args.size() > 2 ? args[2] : "wave";
    const tp::Span r = rangeOf(c);
    if (w == "wave") startPreview(a, ch, *c.snap->inst, c.snap->e, c.k, c.m->playNote, 0, 0);
    else if (w == "range" && !r.empty()) startPreview(a, ch, *c.snap->inst, c.snap->e, c.k, c.m->playNote, r.a, r.b);
    else if (w == "view") startPreview(a, ch, *c.snap->inst, c.snap->e, c.k, c.m->playNote, static_cast<int32_t>(c.m->viewStart), static_cast<int32_t>(c.m->viewStart + c.m->viewLen));
    else if (w == "stop") stopPreview(a), releaseHeld(a);
  } else if (cmd == "step") {
    tp::headerStepSample(a, ch, static_cast<int>(arg(2, 1)));
  } else if (cmd == "loopstart" && c.have() && c.looping) {
    tp::opSampleMarker(a, c.t, c.k, 2, c.ls + static_cast<int32_t>(arg(2, 0)), c.frames);
  } else if (cmd == "looplen" && c.have() && c.looping) {
    tp::opSampleMarker(a, c.t, c.k, 3, c.le + static_cast<int32_t>(arg(2, 0)), c.frames);
  } else if (cmd == "modal" && c.have()) {
    // The valued edits with the values given, as their boxes' Apply.
    const std::string w = args.size() > 2 ? args[2] : "";
    tp::PopupValues& pv = c.m->pv;
    const tp::Span r = rangeOf(c);
    if (w == "volume") {
      pv.rampStart = static_cast<int>(arg(3, pv.rampStart)), pv.rampEnd = static_cast<int>(arg(4, pv.rampEnd));
      const double g0 = pv.rampStart / 100.0, g1 = pv.rampEnd / 100.0;
      waveEdit(a, c, "sample volume ramp", [&](tp::Wave& x) { return tp::waveRamp(x, r, g0, g1); });
    } else if (w == "resample") {
      pv.resTones = static_cast<int>(arg(3, pv.resTones));
      const int t = pv.resTones;
      if (waveEdit(a, c, "resample sample", [&](tp::Wave& x) { return tp::waveResample(x, t); })) c.m->selA = c.m->selB = -1;
    } else if (w == "echo") {
      pv.echoCount = static_cast<int>(arg(3, pv.echoCount)), pv.echoMs = static_cast<int>(arg(4, pv.echoMs));
      const auto d = static_cast<int32_t>(std::max(1.0, std::round(pv.echoMs * c.rate / 1000.0)));
      const int n = pv.echoCount;
      const double fb = pv.echoFeedback / 100.0;
      const bool ext = pv.echoExtend;
      waveEdit(a, c, "sample echo", [&](tp::Wave& x) { return tp::waveEcho(x, n, d, fb, ext); });
    } else if (w == "loopfade") {
      pv.fadeMs = static_cast<int>(arg(3, pv.fadeMs));
      const auto f = static_cast<int32_t>(std::max(1.0, std::round(pv.fadeMs * c.rate / 1000.0)));
      waveEdit(a, c, "crossfade sample loop", [&](tp::Wave& x) { return tp::waveCrossfadeLoop(x, f); });
    } else if (w == "show") {  // the box itself (photos)
      const std::string k = args.size() > 3 ? args[3] : "volume";
      openModal(k == "resample" ? Modal::Resample : k == "echo" ? Modal::Echo : k == "mix" ? Modal::Mix : k == "loopfade" ? Modal::LoopFade : k == "remove" ? Modal::Remove : Modal::Volume);
    }
  } else if (cmd == "fx") {  // K-436: fx add <type> · mix <row> <percent> · preset <row> <index> · param <row> <index> <value> · tail <tenths> · clear · show · preview · apply · dump
    const std::string w = args.size() > 2 ? args[2] : "dump";
    if (w == "add" && args.size() > 3 && static_cast<int>(s.fx.size()) < kMaxFx) {
      St::FxRow row;
      row.st = defaultPluginState(args[3]);
      s.fx.push_back(row);
      s.fxSel = static_cast<int>(s.fx.size()) - 1;
    } else if (w == "mix" && arg(3, -1) >= 0 && arg(3, -1) < static_cast<long long>(s.fx.size())) {
      s.fx[static_cast<size_t>(arg(3, 0))].mix = static_cast<int>(std::clamp<long long>(arg(4, 100), 0, 100));
    } else if (w == "preset" && arg(3, -1) >= 0 && arg(3, -1) < static_cast<long long>(s.fx.size())) {
      St::FxRow& row = s.fx[static_cast<size_t>(arg(3, 0))];
      if (const PluginInfo* info = findPlugin(row.st.type); info && arg(4, -1) >= 0 && arg(4, -1) < static_cast<long long>(info->presets.size())) {
        row.preset = static_cast<int>(arg(4, 0));
        row.st = presetState(*info, static_cast<size_t>(row.preset));
      }
    } else if (w == "param" && arg(3, -1) >= 0 && arg(3, -1) < static_cast<long long>(s.fx.size()) && args.size() > 5) {
      St::FxRow& row = s.fx[static_cast<size_t>(arg(3, 0))];
      normalizePluginState(row.st);
      if (arg(4, -1) >= 0 && arg(4, -1) < static_cast<long long>(row.st.params.size())) row.st.params[static_cast<size_t>(arg(4, 0))] = static_cast<float>(std::atof(args[5].c_str()));
    } else if (w == "sel") {
      s.fxSel = static_cast<int>(std::clamp<long long>(arg(3, 0), 0, std::max<long long>(0, static_cast<long long>(s.fx.size()) - 1)));
    } else if (w == "tail") {
      s.fxTail = static_cast<int>(std::clamp<long long>(arg(3, 0), 0, static_cast<long long>(tp::kMaxEffectTail * 10)));
    } else if (w == "clear") {
      s.fx.clear();
      s.fxSel = 0;
    } else if (w == "show") {
      openModal(Modal::Effects);
    } else if (w == "preview" && c.have()) {
      fxPreview(a, c);
    } else if (w == "fit") {  // every offered effect's settings grid at the box's width and this font size
      const float width = std::round(ImGui::GetFontSize() * 40) - 20;
      int two = 0, one = 0, short_ = 0;
      std::string longest;
      float lw = 0;
      for (const PluginInfo* p : fxList()) {
        PluginState ps = defaultPluginState(p->type);
        normalizePluginState(ps);
        const FxGrid g = fxGrid(a, *p, ps, width);
        (g.cols == 2 ? two : one) += 1;
        short_ += g.fits ? 0 : 1;
        if (g.labW > lw) lw = g.labW, longest = p->type;
      }
      std::printf("tracker editors frame %d: fx fit effects %d two %d one %d short %d widest %s %.0f of %.0f\n", frame, static_cast<int>(fxList().size()), two, one,
                  short_, longest.c_str(), static_cast<double>(lw), static_cast<double>(width));
    } else if (w == "apply" && c.have()) {
      std::printf("tracker editors frame %d: fx apply %s\n", frame, fxApply(a, c) ? "ok" : "nothing");
    } else {
      std::printf("tracker editors frame %d: fx rows %d sel %d tail %d effects %d frames %d stereo %d undo \"%s\"", frame, static_cast<int>(s.fx.size()), s.fxSel, s.fxTail,
                  static_cast<int>(fxList().size()), c.have() ? c.frames : -1, c.have() && c.sp && !c.sp->mono() ? 1 : 0,
                  a.doc.history().canUndo() ? a.doc.history().undoLabel().c_str() : "");
      for (const auto& r : s.fx) std::printf(" %s:%d", r.st.type.c_str(), r.mix);
      std::printf("\n");
    }
    std::fflush(stdout);
  } else if (cmd == "pencil" && c.have()) {  // pencil <frame> <value> <frame> <value> ...: a stroke as a right-drag draws it
    std::vector<std::pair<double, float>> pts;
    for (size_t i = 2; i + 1 < args.size(); i += 2) {
      pts.emplace_back(std::atof(args[i].c_str()), static_cast<float>(std::atof(args[i + 1].c_str())));
    }
    pencilCommit(a, c, pts);
  } else if (cmd == "remove" && c.have()) {
    stopPreview(a);
    tp::opRemoveSample(a, c.t, c.k);
  }
}

}  // namespace gl::trackereditors
