// Gridline - "tracker" pack (K-76, K-120, K-129): the Tracker Sampler's editor.
//   playback panel (interpolation, pitch table, rows per beat, ticks, click removal, built-in
//   sound) and level · samples (list: a click selects and plays the sample, name, order, copy,
//   volume, pan, tuning) beside the selected sample's waveform (start, end, loop points and mode,
//   8 or 16 bits, a range dragged out on it, the edit menu, zoom and scroll; audio files dropped
//   there become samples) · volume and panning envelopes (points, sustain, loop, six slots each) ·
//   the instrument (name, fadeout, auto vibrato) and its MIDI settings · the key map (drag across
//   keys to give them the selected sample).
// Every edit rewrites the instrument text through setPluginState (one undo step per gesture);
// sample edits write the edited audio as a new file first (tracker_wave_dsp.cpp).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include "../../app/App.h"
#include "../../app/AudioRecordUi.h"
#include "../../app/BrowserUi.h"
#include "../../app/ContextMenus.h"
#include "../../app/ExternalDragUi.h"
#include "../../app/KeyLight.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/BrowserIndex.h"
#include "../../core/ExternalDrop.h"
#include "../../core/I18n.h"
#include "../../engine/Sample.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../PacksUi.h"
#include "tracker.h"

namespace gl::packs::trackerpack {

using theme::pal;
namespace fs = std::filesystem;

namespace {

ImU32 slotColour(int k, int alpha = 255) {
  static const ImU32 c[] = {IM_COL32(90, 143, 216, 255), IM_COL32(216, 119, 90, 255), IM_COL32(110, 181, 94, 255), IM_COL32(201, 162, 63, 255),
                            IM_COL32(154, 108, 208, 255), IM_COL32(79, 179, 176, 255), IM_COL32(208, 96, 126, 255), IM_COL32(140, 154, 91, 255)};
  const ImU32 v = c[k & 7];
  return (v & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
}
ImU32 withAlpha(ImU32 c, int alpha) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::clamp(alpha, 0, 255)) << 24); }

// Key names the way the rest of Gridline writes them (key 0 = C0, middle C = C4).
std::string keyName(int key) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (key < 0 || key > 95) return "-";
  return std::string(names[key % 12]) + std::to_string(key / 12);
}

int sampleCountOf(const Edit& e) {
  int n = 0;
  for (int k = 0; k < trk::kMaxSamples; ++k) n += (e.text.def.samples[static_cast<size_t>(k)].present() || e.text.fileOf[static_cast<size_t>(k)] >= 0) ? 1 : 0;
  return n;
}

// What the editor remembers per plugin window.
struct Ed {
  int sel = 0;
  double viewStart = 0, viewLen = 0;  // waveform view, file frames (0 = the whole file)
  int viewSlot = -1;
  int marker = -1;                    // waveform marker being dragged: 0 start, 1 end, 2 loop start, 3 loop end, 4 scroll
  double grab = 0;
  int envDrag[2] = {-1, -1};
  int envMenu[2] = {-1, -1};
  float envSpan[2] = {0, 0};          // ticks the graph shows (kept while a point is dragged)
  int keyFrom = -1, keyTo = -1;
  int keyHeld = -1;  // K-129: the MIDI key a press on the strip plays
  char name[96] = {};
  int nameSlot = -1;
  std::string nameData;
  // K-129
  int32_t selA = -1, selB = -1;       // the range dragged out on the waveform (file frames; -1 = none)
  int selSlot = -1;
  bool selecting = false;
  char insName[96] = {};
  std::string insNameData;
  bool insNameSet = false;
  int rampStart = 100, rampEnd = 100;  // volume popup (percent)
  int resTones = 12;                   // resample popup (semitones)
  int echoCount = 4, echoMs = 120, echoFeedback = 50;
  bool echoExtend = true;
  int mixSlot = -1, mixBalance = 50;
  int fadeMs = 20;                     // loop crossfade (ms)
};
std::map<uint64_t, Ed> gEd;

// ---- K-129: hearing one sample (a click in the list) ----
struct Preview {
  uint32_t serial = 0;
  ObjectId channel = kNoId;
  int pitch = -1, slot = -1;
  uint64_t key = 0;
  double started = 0;
};
Preview gPreview;

// The notes the editors hold (a strip key, a preview): released at the start of a frame when their
// editor did not show them on the frame before (closed, unfocused, scrolled away).
NoteGuard gHeld;

int frameNow() { return ImGui::GetFrameCount(); }

// Ends a held note the guard gives back (the editor is not drawing: only global state here).
void releaseHeld(App& a, const HeldNote& h) {
  if (h.serial) {
    cancelAudition(h.serial);
    if (gPreview.serial == h.serial) gPreview = Preview{};
  }
  if (h.channel != kNoId && h.pitch >= 0) a.engine.noteOff(h.channel, h.pitch);
  if (!h.serial) {
    auto it = gEd.find(h.editor);
    if (it != gEd.end()) {
      it->second.keyHeld = -1;
      it->second.keyFrom = it->second.keyTo = -1;
    }
  }
}

// Once per ImGui context: a hook at the start of every frame sweeps the held notes, so they end
// even when no Tracker Sampler editor is drawn any more.
void sweepHook(ImGuiContext* ctx, ImGuiContextHook* hook) {
  App* a = static_cast<App*>(hook->UserData);
  if (!a) return;
  gHeld.sweep(ctx->FrameCount, [a](const HeldNote& h) { releaseHeld(*a, h); });
}

void ensureSweep(App& a) {
  static ImGuiContext* installed = nullptr;
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx || ctx == installed) return;
  ImGuiContextHook hk;
  hk.Type = ImGuiContextHookType_NewFramePost;
  hk.Callback = sweepHook;
  hk.UserData = &a;
  ImGui::AddContextHook(ctx, &hk);
  installed = ctx;
}

// The editor window (and its children) has the keyboard focus and the app is in front.
bool editorFocused() {
  return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().AppFocusLost;
}

void stopPreview(App& a) {
  if (gPreview.serial == 0) return;
  cancelAudition(gPreview.serial);
  if (gPreview.channel != kNoId) a.engine.noteOff(gPreview.channel, gPreview.pitch);
  gPreview = Preview{};
  gHeld.drop(NoteGuard::kPreview);
}

// Plays slot k of the instrument once through its channel, at the key that plays it at its own rate.
void startPreview(App& a, const PluginTarget& t, const Edit& e, const InstrumentAux& inst, int k) {
  stopPreview(a);
  if (!t.isChannel() || k < 0 || k >= trk::kMaxSamples) return;
  const auto i = static_cast<size_t>(k);
  if (!inst.data[i]) return;
  const SamplePtr& sp = inst.files[i];
  const double rate = sp && sp->sampleRate > 0 ? sp->sampleRate : 8363.0;
  const bool builtinOnly = inst.builtin > 0 && sampleCountOf(e) == 0;
  const int pitch = std::clamp(builtinOnly ? 60 : auditionPitch(e, k, rate), 0, 127);
  gPreview.serial = requestAudition(&inst, k, pitch);
  gPreview.channel = t.owner;
  gPreview.pitch = pitch;
  gPreview.slot = k;
  gPreview.key = t.key();
  gPreview.started = ImGui::GetTime();
  ensureSweep(a);
  gHeld.hold(NoteGuard::kPreview, t.key(), t.owner, pitch, gPreview.serial, frameNow());
  a.engine.ownNoteOn(t.owner, pitch, 100.0f / 127.0f, AudioEngine::OwnPlay::EditorPreview);
}

// Once a frame from the editor: the note ends with the preview, or when no voice took it; it
// stays held only while this editor is on screen and focused (else the frame sweep ends it).
void tendPreview(App& a, const PluginTarget& t) {
  if (gPreview.serial == 0 || gPreview.key != t.key()) return;
  const bool sounding = auditionSounding(gPreview.serial);
  const bool stuck = sounding && ImGui::GetTime() - gPreview.started > 0.75 && !auditionPlaying(gPreview.serial);
  if (!sounding || stuck) stopPreview(a);
  else if (!editorFocused()) stopPreview(a);
  else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) stopPreview(a);
  else gHeld.seen(NoteGuard::kPreview, frameNow());
}

// The sample clipboard of every Tracker Sampler editor.
Wave& clipboard() {
  static Wave w;
  return w;
}

uint64_t mergeKey(const PluginTarget& t, int field) { return (t.key() * 2654435761ull) ^ (0x7A1C0000ull + static_cast<uint64_t>(field)); }

int soundOf(const PluginState& st) {
  return st.params.size() > static_cast<size_t>(trk::PSound) ? std::clamp(static_cast<int>(std::lround(st.params[trk::PSound])), 0, kSoundCount) : 0;
}

void commit(App& a, const PluginTarget& t, const Edit& e, const char* label, uint64_t merge = 0) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur) return;
  PluginState ns = *cur;
  ns.data = writeEdit(e);
  if (ns.data != cur->data) setPluginState(a, t, ns, label, merge);
}

bool hasSample(const Edit& e, int k) {
  const auto i = static_cast<size_t>(k);
  return e.text.def.samples[i].present() || e.text.fileOf[i] >= 0;
}
int sampleCount(const Edit& e) {
  int n = 0;
  for (int k = 0; k < trk::kMaxSamples; ++k) n += hasSample(e, k) ? 1 : 0;
  return n;
}

// A knob over a value of the instrument text: the value under the dial, the label under that.
bool valueKnob(App& a, const char* id, double& v, double lo, double hi, double def, const char* label, const std::string& text, float cell) {
  const ParamDesc d{label, "", lo, hi, def, ParamKind::Continuous, ParamScale::Linear, {}};
  const float radius = 13;
  ImGui::PushID(id);
  ImGui::BeginGroup();
  const float x = ImGui::GetCursorPosX();
  ImGui::SetCursorPosX(x + (cell - radius * 2) * 0.5f);
  const bool changed = ui::Knob("k", &v, d, radius, nullptr, lo < 0 && hi > 0);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(pal().text), "%s", text.c_str());
  ImGui::PopFont();
  ImGui::PushFont(nullptr, 12.0f);
  ts = ImGui::CalcTextSize(label);
  ImGui::SetCursorPosX(x + std::max(0.0f, (cell - ts.x) * 0.5f));
  ImGui::TextColored(theme::vec(pal().textDim), "%s", label);
  ImGui::PopFont();
  ImGui::SetCursorPosX(x);
  ImGui::Dummy(ImVec2(cell, 0));
  ImGui::EndGroup();
  ImGui::PopID();
  return changed;
}

void dimCaption(const char* s) {
  ImGui::PushStyleColor(ImGuiCol_Text, pal().textDim);
  ImGui::TextUnformatted(s);
  ImGui::PopStyleColor();
}

// A caption at the top of a group that sits beside another group: drawn at the cursor, so the
// text baseline left by the group before it (its frame padding) does not push it down.
void groupCaption(const char* s) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 sz = ImGui::CalcTextSize(s);
  ImGui::GetWindowDrawList()->AddText(p, ImGui::GetColorU32(pal().textDim), s);
  ImGui::Dummy(ImVec2(sz.x, sz.y));
}

// ---- dropping audio files ----

std::vector<std::string> droppedAudio(App& a, const ImGuiPayload* pl) {
  std::vector<std::string> out;
  if (!pl) return out;
  if (pl->IsDataType("GL_FILES") || xdragui::ownsPayload(pl)) {
    for (const xdrop::Item& it : xdragui::items())
      if (!it.dir && !it.pending && !it.path.empty() && browse::kindOf(it.path) == browse::Kind::Audio) out.push_back(it.path);
    if (!out.empty()) return out;
  }
  const std::string p = browserui::draggedAudioFile(a, pl);
  if (!p.empty()) out.push_back(p);
  return out;
}

// Adds the files as samples (as many as there are free slots); one undo step. Returns the first
// new slot, -1 when none was added.
int addFiles(App& a, const PluginTarget& t, const std::vector<std::string>& paths, int sound) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return -1;
  Edit e = readEdit(st->data, sound);
  int added = 0;
  bool full = false;
  const int first = addSampleFiles(e, paths, &added, &full);
  if (full) a.notify(i18n::tr("All 16 sample slots are taken"));
  if (added > 0) commit(a, t, e, added == 1 ? "add sample" : "add samples");
  return first;
}

// K-129: "Add file": Gridline's file dialog for one audio file, added exactly as a drop adds it.
void addFileDialog(App& a, const PluginTarget& t, int sound) {
  const PluginTarget tt = t;
  menus::fileDialog(a, i18n::tr("Add a sample"), false, {".wav", ".wave", ".flac", ".mp3", ".ogg", ".wv", ".aif", ".aiff", ".aifc"}, "",
             [&a, tt, sound](const std::string& p) {
               const int k = addFiles(a, tt, {p}, sound);
               if (k >= 0) gEd[tt.key()].sel = k;
             });
}

// The drop target over a rect: audio files from the browser or the desktop become samples.
void sampleDropTarget(App& a, const PluginTarget& t, Ed& ed, ImVec2 p0, ImVec2 p1, int sound, bool full) {
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl) return;
  const std::vector<std::string> files = droppedAudio(a, pl);
  if (files.empty()) return;
  if (!ImGui::BeginDragDropTargetCustom(ImRect(p0, p1), ImGui::GetID("##trkdrop"))) return;
  if (const ImGuiPayload* x = ImGui::AcceptDragDropPayload(pl->DataType, ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptBeforeDelivery)) {
    if (x->Delivery) {
      xdragui::markConsumed();
      const int k = addFiles(a, t, files, sound);
      if (k >= 0) ed.sel = k;
    } else {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      fg->AddRectFilled(p0, p1, withAlpha(pal().accent, 28), 6);
      fg->AddRect(p0, p1, pal().accent, 6, 0, 2.0f);
      char tip[256];
      if (full) std::snprintf(tip, sizeof tip, "%s", i18n::tr("All 16 sample slots are taken"));
      else if (files.size() == 1) std::snprintf(tip, sizeof tip, i18n::tr("Add %s as a sample"), fs::u8path(files[0]).filename().u8string().c_str());
      else std::snprintf(tip, sizeof tip, i18n::tr("Add %d files as samples"), static_cast<int>(files.size()));
      ImGui::SetTooltip("%s", tip);
      a.setHint(tip);
    }
  }
  ImGui::EndDragDropTarget();
}

// ---- waveform peaks (min / max per block of frames, made once per file) ----

struct Peaks {
  std::weak_ptr<const Sample> owner;
  std::vector<float> mn, mx;
};
constexpr int kPeakBlock = 256;
const Peaks& peaksOf(const SamplePtr& sp) {
  static std::map<const Sample*, Peaks> cache;
  for (auto it = cache.begin(); it != cache.end();) it = it->second.owner.expired() ? cache.erase(it) : std::next(it);
  Peaks& p = cache[sp.get()];
  if (p.owner.expired() || p.owner.lock() != sp) {
    p.owner = sp;
    const size_t n = sp->frames(), blocks = (n + kPeakBlock - 1) / kPeakBlock;
    p.mn.assign(blocks, 0.0f);
    p.mx.assign(blocks, 0.0f);
    const bool st = !sp->mono();
    for (size_t b = 0; b < blocks; ++b) {
      float lo = 1e9f, hi = -1e9f;
      for (size_t i = b * kPeakBlock; i < std::min(n, (b + 1) * kPeakBlock); ++i) {
        const float v = st ? 0.5f * (sp->left[i] + sp->right[i]) : sp->left[i];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      p.mn[b] = lo;
      p.mx[b] = hi;
    }
  }
  return p;
}

// ---- K-129: sample edits ----

std::string editName(const Edit& e, int k) {
  const std::string& nm = e.text.sampleNames[static_cast<size_t>(k)];
  if (!nm.empty()) return nm;
  const int f = e.text.fileOf[static_cast<size_t>(k)];
  if (f >= 0 && f < static_cast<int>(e.text.files.size())) return fs::u8path(e.text.files[static_cast<size_t>(f)]).stem().u8string();
  return "sample";
}

Span selectionOf(const Ed& ed, int k) {
  if (ed.selSlot != k || ed.selA < 0 || ed.selB < 0) return Span{};
  return Span{std::min(ed.selA, ed.selB), std::max(ed.selA, ed.selB)};
}

// Runs one edit on slot k's audio and keeps the result (one undo step). `op` returns false when it
// changed nothing; `after` sets the selection the edit leaves (frames of the new audio).
template <class Op>
bool editWave(App& a, const PluginTarget& t, Ed& ed, Edit& e, const InstrumentAux& inst, int k, const char* label, Op op) {
  Wave w;
  if (!waveOf(e, k, inst, w)) {
    a.notify(i18n::tr("The sample file is missing"));
    return false;
  }
  if (!op(w)) return false;
  const std::string path = commitWave(e, k, w, editedDir(a), editName(e, k));
  if (path.empty()) {
    a.notify(i18n::tr("The edited sample could not be written"), true);
    return false;
  }
  (void)sampleCache().get(path);
  if (gPreview.serial && gPreview.slot == k) stopPreview(a);
  commit(a, t, e, label);
  if (ed.viewLen > w.frames()) ed.viewLen = 0;
  return true;
}

// The edit menu (from the toolbar button and a right click on the waveform).
void editMenuItems(App& a, const PluginTarget& t, Ed& ed, Edit& e, const InstrumentAux& inst, int k, int32_t frames, const char*& openPopup) {
  const Span sel = selectionOf(ed, k);
  const bool hasSel = !sel.empty();
  const bool hasClip = !clipboard().L.empty();
  const trk::SampleDef& sd = e.text.def.samples[static_cast<size_t>(k)];
  const bool looped = sd.loop != trk::kLoopOff && sd.loopLength > 0;
  auto clear = [&] { ed.selA = ed.selB = -1; };
  if (ImGui::MenuItem(i18n::tr("Cut"), nullptr, false, hasSel && sel.b - sel.a < frames)) {
    Wave w;
    if (waveOf(e, k, inst, w)) clipboard() = waveCopy(w, sel);
    if (editWave(a, t, ed, e, inst, k, "cut sample range", [&](Wave& x) { return waveDelete(x, sel); })) clear();
  }
  if (ImGui::MenuItem(i18n::tr("Copy"), nullptr, false, true)) {
    Wave w;
    if (waveOf(e, k, inst, w)) clipboard() = waveCopy(w, sel);
  }
  if (ImGui::MenuItem(hasSel ? i18n::tr("Paste over the range") : i18n::tr("Paste at the range start"), nullptr, false, hasClip)) {
    const int32_t cursor = ed.selSlot == k && ed.selA >= 0 ? std::min(ed.selA, frames) : 0;
    const Span at = hasSel ? sel : Span{cursor, cursor};
    const auto n = static_cast<int32_t>(clipboard().L.size());
    if (editWave(a, t, ed, e, inst, k, "paste into sample", [&](Wave& x) { return wavePaste(x, at, clipboard()); })) {
      ed.selSlot = k;
      ed.selA = at.a;
      ed.selB = at.a + n;
    }
  }
  if (ImGui::MenuItem(i18n::tr("Crop to the range"), nullptr, false, hasSel && sel.b - sel.a < frames))
    if (editWave(a, t, ed, e, inst, k, "crop sample", [&](Wave& x) { return waveCrop(x, sel); })) clear();
  if (ImGui::MenuItem(i18n::tr("Delete the range"), nullptr, false, hasSel && sel.b - sel.a < frames))
    if (editWave(a, t, ed, e, inst, k, "delete sample range", [&](Wave& x) { return waveDelete(x, sel); })) clear();
  if (ImGui::MenuItem(i18n::tr("Silence the range"), nullptr, false, hasSel)) editWave(a, t, ed, e, inst, k, "silence sample range", [&](Wave& x) { return waveSilence(x, sel); });
  ImGui::Separator();
  const char* scope = hasSel ? i18n::tr("(the range)") : i18n::tr("(the whole sample)");
  char b[128];
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Reverse"), scope);
  if (ImGui::MenuItem(b)) editWave(a, t, ed, e, inst, k, "reverse sample", [&](Wave& x) { return waveReverse(x, sel); });
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Fade in"), scope);
  if (ImGui::MenuItem(b)) editWave(a, t, ed, e, inst, k, "fade in sample", [&](Wave& x) { return waveRamp(x, sel, 0.0, 1.0); });
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Fade out"), scope);
  if (ImGui::MenuItem(b)) editWave(a, t, ed, e, inst, k, "fade out sample", [&](Wave& x) { return waveRamp(x, sel, 1.0, 0.0); });
  if (ImGui::MenuItem(i18n::tr("Volume ramp..."))) openPopup = "##trkramp";
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Normalise"), scope);
  if (ImGui::MenuItem(b)) editWave(a, t, ed, e, inst, k, "normalise sample", [&](Wave& x) { return waveNormalise(x, sel); });
  std::snprintf(b, sizeof b, "%s %s", i18n::tr("Remove DC offset"), scope);
  if (ImGui::MenuItem(b)) editWave(a, t, ed, e, inst, k, "remove DC offset", [&](Wave& x) { return waveFixDc(x, sel); });
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Resample..."))) openPopup = "##trkres";
  if (ImGui::MenuItem(i18n::tr("Echo..."))) openPopup = "##trkecho";
  if (ImGui::MenuItem(i18n::tr("Mix in a sample..."), nullptr, false, sampleCountOf(e) > 1)) openPopup = "##trkmix";
  if (ImGui::MenuItem(i18n::tr("Crossfade the loop..."), nullptr, false, looped)) openPopup = "##trkxfade";
  if (ImGui::MenuItem(i18n::tr("Trim after the loop"), nullptr, false, looped && sd.loopStart + sd.loopLength < frames))
    editWave(a, t, ed, e, inst, k, "trim sample after loop", [&](Wave& x) { return waveMinimise(x); });
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Select all"))) {
    ed.selSlot = k;
    ed.selA = 0;
    ed.selB = frames;
  }
  if (ImGui::MenuItem(i18n::tr("Zoom to the range"), nullptr, false, hasSel)) {
    ed.viewStart = sel.a;
    ed.viewLen = std::max<double>(std::min<double>(frames, 64.0), sel.b - sel.a);
    ed.viewStart = std::clamp(ed.viewStart, 0.0, frames - ed.viewLen);
  }
  if (ImGui::MenuItem(i18n::tr("Clear the range"), nullptr, false, hasSel)) clear();
  if (ImGui::MenuItem(i18n::tr("Empty the clipboard"), nullptr, false, hasClip)) clipboard() = Wave{};
}

// The edits that ask for values first.
void editPopups(App& a, const PluginTarget& t, Ed& ed, Edit& e, const InstrumentAux& inst, int k) {
  const Span sel = selectionOf(ed, k);
  auto apply = [&](const char* text) {
    ImGui::Dummy(ImVec2(0, 4));
    const bool go = ui::PrimaryButton(text, 96);
    ImGui::SameLine(0, 6);
    if (ui::TextButton(i18n::tr("Cancel"), false, 96)) ImGui::CloseCurrentPopup();
    return go;
  };
  if (ImGui::BeginPopup("##trkramp")) {
    dimCaption(sel.empty() ? i18n::tr("Volume from the start to the end of the sample") : i18n::tr("Volume from the start to the end of the range"));
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("Start"), &ed.rampStart, 0, 400, "%d%%");
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("End"), &ed.rampEnd, 0, 400, "%d%%");
    if (ui::TextButton(i18n::tr("Loudest without clipping"), false, 0)) {
      Wave w;
      if (waveOf(e, k, inst, w)) ed.rampStart = ed.rampEnd = std::clamp(static_cast<int>(std::floor(waveMaxScale(w, sel) * 100.0)), 0, 400);
    }
    if (apply(i18n::tr("Apply"))) {
      const double g0 = ed.rampStart / 100.0, g1 = ed.rampEnd / 100.0;
      editWave(a, t, ed, e, inst, k, "sample volume ramp", [&](Wave& x) { return waveRamp(x, sel, g0, g1); });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##trkres")) {
    dimCaption(i18n::tr("More or fewer frames for the same pitch (the transpose follows)"));
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("Semitones"), &ed.resTones, -36, 36, "%+d");
    if (apply(i18n::tr("Resample"))) {
      const int st = ed.resTones;
      if (editWave(a, t, ed, e, inst, k, "resample sample", [&](Wave& x) { return waveResample(x, st); })) ed.selA = ed.selB = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##trkecho")) {
    dimCaption(i18n::tr("Repeats of the sample, each quieter than the last"));
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("Repeats"), &ed.echoCount, 1, 32);
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("Distance"), &ed.echoMs, 1, 2000, "%d ms");
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("Each repeat"), &ed.echoFeedback, 0, 100, "%d%%");
    ImGui::Checkbox(i18n::tr("Lengthen the sample to hold the repeats"), &ed.echoExtend);
    if (apply(i18n::tr("Apply"))) {
      Wave probe;
      const double rate = waveOf(e, k, inst, probe) ? probe.rate : 44100.0;
      const int n = ed.echoCount;
      const auto d = static_cast<int32_t>(std::max(1.0, std::round(ed.echoMs * rate / 1000.0)));
      const double fb = ed.echoFeedback / 100.0;
      const bool ext = ed.echoExtend;
      editWave(a, t, ed, e, inst, k, "sample echo", [&](Wave& x) { return waveEcho(x, n, d, fb, ext); });
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##trkmix")) {
    dimCaption(i18n::tr("Another sample of this instrument mixed in from the start"));
    if (ed.mixSlot == k || !hasSample(e, ed.mixSlot)) {
      ed.mixSlot = -1;
      for (int j = 0; j < trk::kMaxSamples && ed.mixSlot < 0; ++j)
        if (j != k && hasSample(e, j)) ed.mixSlot = j;
    }
    char cur[128];
    std::snprintf(cur, sizeof cur, "%02d %s", ed.mixSlot + 1, ed.mixSlot >= 0 ? editName(e, ed.mixSlot).c_str() : "");
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo(i18n::tr("Sample"), cur)) {
      for (int j = 0; j < trk::kMaxSamples; ++j) {
        if (j == k || !hasSample(e, j)) continue;
        char it[128];
        std::snprintf(it, sizeof it, "%02d %s##m%d", j + 1, editName(e, j).c_str(), j);
        if (ImGui::Selectable(it, ed.mixSlot == j)) ed.mixSlot = j;
      }
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("Balance"), &ed.mixBalance, 0, 100, "%d%%");
    if (apply(i18n::tr("Mix")) && ed.mixSlot >= 0) {
      Wave other;
      if (waveOf(e, ed.mixSlot, inst, other)) {
        const double bal = ed.mixBalance / 100.0;
        editWave(a, t, ed, e, inst, k, "mix samples", [&](Wave& x) { return waveMix(x, other, bal); });
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##trkxfade")) {
    dimCaption(i18n::tr("The loop's end fades into the sound before its start"));
    ImGui::SetNextItemWidth(220);
    ImGui::SliderInt(i18n::tr("Length"), &ed.fadeMs, 1, 500, "%d ms");
    if (apply(i18n::tr("Apply"))) {
      Wave probe;
      const double rate = waveOf(e, k, inst, probe) ? probe.rate : 44100.0;
      const auto f = static_cast<int32_t>(std::max(1.0, std::round(ed.fadeMs * rate / 1000.0)));
      if (!editWave(a, t, ed, e, inst, k, "crossfade sample loop", [&](Wave& x) { return waveCrossfadeLoop(x, f); }))
        a.notify(i18n::tr("The loop needs sound before its start to fade into"));
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// ---- the selected sample's waveform ----

void waveformPanel(App& a, const PluginTarget& t, Ed& ed, Edit& e, const InstrumentAux& inst, int sound, float w, float h) {
  pui::beginPanel(i18n::tr("SAMPLE##trkwave"), w, h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  const float inner = w - 20;
  const int n = sampleCount(e);
  sampleDropTarget(a, t, ed, ImVec2(wp.x + 1, wp.y + 1), ImVec2(wp.x + ws.x - 1, wp.y + ws.y - 1), sound, n >= trk::kMaxSamples);
  const int k = ed.sel;
  const bool have = hasSample(e, k);
  const SamplePtr sp = have ? inst.files[static_cast<size_t>(k)] : nullptr;
  if (!have || !sp) {
    // Nothing to show: the drop area.
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + inner, wp.y + ws.y - 10);
    dl->AddRectFilled(p0, p1, withAlpha(pal().surface900, 160), 6);
    // dashed outline
    const ImU32 dash = withAlpha(pal().textFaint, 200);
    for (float x = p0.x + 6; x < p1.x - 6; x += 12) {
      dl->AddLine(ImVec2(x, p0.y), ImVec2(std::min(x + 6, p1.x - 6), p0.y), dash);
      dl->AddLine(ImVec2(x, p1.y), ImVec2(std::min(x + 6, p1.x - 6), p1.y), dash);
    }
    for (float y = p0.y + 6; y < p1.y - 6; y += 12) {
      dl->AddLine(ImVec2(p0.x, y), ImVec2(p0.x, std::min(y + 6, p1.y - 6)), dash);
      dl->AddLine(ImVec2(p1.x, y), ImVec2(p1.x, std::min(y + 6, p1.y - 6)), dash);
    }
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    ui::drawIconBox(dl, ui::Icon::Audio, ImVec2(c.x, c.y - 30), 26, pal().textDim);
    const char* title = have ? i18n::tr("The sample file is missing") : i18n::tr("Drop a sample here");
    ImGui::PushFont(a.fontBold, 14.0f);
    ImVec2 ts = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - 8), pal().text, title);
    ImGui::PopFont();
    char body[200];
    if (have) std::snprintf(body, sizeof body, "%s", i18n::tr("Drop an audio file to add it again"));
    else if (sound > 0 && n == 0) std::snprintf(body, sizeof body, i18n::tr("Audio files from the browser or the desktop. Until then: %s"), soundName(sound));
    else if (n == 0) std::snprintf(body, sizeof body, "%s", i18n::tr("Audio files from the browser or the desktop, or pick a built-in sound"));
    else std::snprintf(body, sizeof body, "%s", i18n::tr("Audio files from the browser or the desktop, up to 16"));
    ImGui::PushFont(nullptr, 12.0f);
    ts = ImGui::CalcTextSize(body);
    if (ts.x > inner - 16) {  // narrow: the short form
      std::snprintf(body, sizeof body, "%s", i18n::tr("From the browser or the desktop"));
      ts = ImGui::CalcTextSize(body);
    }
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + 14), pal().textDim, body);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(inner, std::max(1.0f, p1.y - p0.y)));
    pui::endPanel();
    return;
  }
  trk::SampleDef& sd = e.text.def.samples[static_cast<size_t>(k)];
  const int32_t frames = static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF));
  int32_t& cutA = e.cuts.start[static_cast<size_t>(k)];
  int32_t& cutB = e.cuts.end[static_cast<size_t>(k)];
  const int32_t s0 = std::clamp(cutA, 0, frames - 1);
  const int32_t s1 = cutB > 0 ? std::clamp(cutB, s0 + 1, frames) : frames;
  if (ed.viewSlot != k || ed.viewLen <= 0 || ed.viewLen > frames) {
    ed.viewSlot = k;
    ed.viewStart = 0;
    ed.viewLen = frames;
  }

  // Toolbar: loop mode, bits, the edit menu, zoom.
  const char* openPopup = nullptr;
  {
    const char* modes[] = {i18n::tr("No loop"), i18n::tr("Forward"), i18n::tr("Ping-pong")};
    int mode = sd.loop;
    const float right = 4 * 24 + 3 * 2 + 4;  // edit menu and zoom buttons
    const float segW = std::clamp((inner - right - 2 * 44 - 16) / 3.0f, 52.0f, 84.0f);
    if (ui::Segmented("##loopmode", modes, 3, &mode, segW, 22)) {
      sd.loop = static_cast<uint8_t>(mode);
      if (mode != trk::kLoopOff && sd.loopLength < 1) {
        sd.loopStart = s0;
        sd.loopLength = s1 - s0;
      }
      commit(a, t, e, "sample loop mode");
    }
    ImGui::SameLine(0, 8);
    const char* depths[] = {i18n::tr("16-bit"), i18n::tr("8-bit")};
    const char* depthHints[] = {i18n::tr("The sample plays at full resolution"), i18n::tr("The sample plays rounded to 256 steps, as an 8-bit sample")};
    int depth = e.eightBit[static_cast<size_t>(k)] ? 1 : 0;
    if (ui::Segmented("##bits", depths, 2, &depth, 44, 22, depthHints)) {
      e.eightBit[static_cast<size_t>(k)] = depth == 1;
      commit(a, t, e, "sample bit depth");
    }
    const float bx = wp.x + ws.x - 10 - right + 4;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(std::max(bx, ImGui::GetCursorScreenPos().x), ImGui::GetCursorScreenPos().y));
    if (ui::IconButton("##sedit", ui::Icon::More, false, 22, i18n::tr("Edit the sample: cut, copy, paste, fades, normalise, resample, echo and more")))
      ImGui::OpenPopup("##trkeditmenu");
    if (ImGui::BeginPopup("##trkeditmenu")) {
      editMenuItems(a, t, ed, e, inst, k, frames, openPopup);
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2);
    auto zoom = [&](double f, double centre) {
      const double len = std::clamp(ed.viewLen * f, std::min<double>(frames, 64.0), static_cast<double>(frames));
      ed.viewStart = std::clamp(centre - (centre - ed.viewStart) * len / ed.viewLen, 0.0, frames - len);
      ed.viewLen = len;
    };
    if (ui::IconButton("##zin", ui::Icon::Plus, false, 22, i18n::tr("Zoom in"))) zoom(0.5, ed.viewStart + ed.viewLen * 0.5);
    ImGui::SameLine(0, 2);
    if (ui::IconButton("##zout", ui::Icon::Minus, false, 22, i18n::tr("Zoom out"))) zoom(2.0, ed.viewStart + ed.viewLen * 0.5);
    ImGui::SameLine(0, 2);
    if (ui::IconButton("##zfit", ui::Icon::Zoom, false, 22, i18n::tr("Show the whole sample"))) {
      ed.viewStart = 0;
      ed.viewLen = frames;
    }
  }

  // The waveform.
  const float readH = ImGui::GetTextLineHeight() + 4;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float dispH = std::max(40.0f, wp.y + ws.y - 8 - readH - 12 - p0.y);
  const ImVec2 p1(p0.x + inner, p0.y + dispH);
  pui::displayFrame(dl, p0, p1);
  const float W = p1.x - p0.x;
  auto xOf = [&](double f) { return p0.x + static_cast<float>((f - ed.viewStart) / ed.viewLen * W); };
  auto fOf = [&](float x) { return ed.viewStart + static_cast<double>(x - p0.x) / W * ed.viewLen; };
  dl->PushClipRect(p0, p1, true);
  const float mid = (p0.y + p1.y) * 0.5f, amp = (p1.y - p0.y) * 0.5f - 4;
  dl->AddLine(ImVec2(p0.x, mid), ImVec2(p1.x, mid), withAlpha(pal().textFaint, 90));
  const bool looping = sd.loop != trk::kLoopOff && sd.loopLength > 0;
  const double lA = sd.loopStart, lB = static_cast<double>(sd.loopStart) + sd.loopLength;
  if (looping) dl->AddRectFilled(ImVec2(xOf(lA), p0.y), ImVec2(xOf(lB), p1.y), withAlpha(pal().accent, 34));
  const Span selNow = selectionOf(ed, k);
  if (selNow.empty() && ed.selSlot == k && ed.selA >= 0) {  // the paste place
    const float cx = std::round(xOf(ed.selA)) + 0.5f;
    if (cx >= p0.x && cx <= p1.x)
      for (float y = p0.y + 2; y < p1.y - 2; y += 6) dl->AddLine(ImVec2(cx, y), ImVec2(cx, std::min(y + 3, p1.y - 2)), withAlpha(pal().text, 160));
  }
  if (!selNow.empty()) {
    const float sa = std::max(p0.x, xOf(selNow.a)), sb = std::min(p1.x, std::max(xOf(selNow.b), xOf(selNow.a) + 1));
    dl->AddRectFilled(ImVec2(sa, p0.y), ImVec2(sb, p1.y), withAlpha(pal().text, 40));
    dl->AddLine(ImVec2(sa, p0.y), ImVec2(sa, p1.y), withAlpha(pal().text, 150));
    dl->AddLine(ImVec2(sb, p0.y), ImVec2(sb, p1.y), withAlpha(pal().text, 150));
  }
  {
    const Peaks& pk = peaksOf(sp);
    const bool st = !sp->mono();
    const ImU32 wave = slotColour(k, 230);
    for (int px = 0; px < static_cast<int>(W); ++px) {
      const double fa = fOf(p0.x + px), fb = fOf(p0.x + px + 1);
      int64_t ia = std::max<int64_t>(0, static_cast<int64_t>(fa)), ib = std::min<int64_t>(frames, static_cast<int64_t>(std::ceil(fb)));
      if (ib <= ia) ib = std::min<int64_t>(frames, ia + 1);
      if (ia >= frames) break;
      float lo = 1e9f, hi = -1e9f;
      if (ib - ia > 2 * kPeakBlock) {
        for (int64_t b = ia / kPeakBlock; b <= (ib - 1) / kPeakBlock && b < static_cast<int64_t>(pk.mn.size()); ++b) {
          lo = std::min(lo, pk.mn[static_cast<size_t>(b)]);
          hi = std::max(hi, pk.mx[static_cast<size_t>(b)]);
        }
      } else {
        for (int64_t i = ia; i < ib; ++i) {
          const float v = st ? 0.5f * (sp->left[static_cast<size_t>(i)] + sp->right[static_cast<size_t>(i)]) : sp->left[static_cast<size_t>(i)];
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
      }
      if (lo > hi) continue;
      const float x = p0.x + static_cast<float>(px) + 0.5f;
      dl->AddLine(ImVec2(x, mid - std::clamp(hi, -1.0f, 1.0f) * amp), ImVec2(x, mid - std::clamp(lo, -1.0f, 1.0f) * amp + 1), wave);
    }
  }
  // What is cut away: shaded.
  const ImU32 shade = withAlpha(pal().surface950, 170);
  if (s0 > 0) dl->AddRectFilled(p0, ImVec2(std::max(p0.x, xOf(s0)), p1.y), shade);
  if (s1 < frames) dl->AddRectFilled(ImVec2(std::min(p1.x, xOf(s1)), p0.y), p1, shade);
  // Markers: sample start / end (flags at the bottom), loop start / end (flags at the top).
  struct Mk {
    double f;
    bool top;
    ImU32 col;
  };
  const Mk mk[4] = {{static_cast<double>(s0), false, pal().text}, {static_cast<double>(s1), false, pal().text}, {lA, true, pal().accent}, {lB, true, pal().accent}};
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  int hot = -1;
  if (ed.marker < 0 && ImGui::IsWindowHovered() && mouse.y >= p0.y && mouse.y <= p1.y) {
    float best = 7;
    for (int m = 0; m < 4; ++m) {
      if (m >= 2 && !looping) continue;
      const float d = std::abs(mouse.x - xOf(mk[m].f));
      if (d < best || (d <= best && m >= 2 && (mouse.y < mid) == mk[m].top)) {
        best = d;
        hot = m;
      }
    }
  }
  for (int m = 0; m < 4; ++m) {
    if (m >= 2 && !looping) continue;
    const float x = std::round(xOf(mk[m].f)) + 0.5f;
    const bool on = hot == m || ed.marker == m;
    dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), withAlpha(mk[m].col, on ? 255 : 200), on ? 2.0f : 1.0f);
    const float fy = mk[m].top ? p0.y : p1.y;
    const float dir = mk[m].top ? 1.0f : -1.0f;
    const bool right = m == 0 || m == 2;  // start flags point into the region
    const float fx = right ? 7.0f : -7.0f;
    dl->AddTriangleFilled(ImVec2(x, fy), ImVec2(x + fx, fy), ImVec2(x, fy + dir * 9), withAlpha(mk[m].col, on ? 255 : 220));
  }
  dl->PopClipRect();
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##wave", ImVec2(W, dispH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered();
  if (hot >= 0 || ed.marker >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hot >= 0) {
      ed.marker = hot;
    } else {  // K-129: away from the flags a drag marks out a range
      ed.selecting = true;
      ed.selSlot = k;
      ed.selA = ed.selB = static_cast<int32_t>(std::lround(std::clamp(fOf(mouse.x), 0.0, static_cast<double>(frames))));
    }
  }
  if (ed.selecting) {
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ed.selB = static_cast<int32_t>(std::lround(std::clamp(fOf(mouse.x), 0.0, static_cast<double>(frames))));
    } else {
      ed.selecting = false;
      // A click leaves no range, only the place a paste goes.
    }
  }
  if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##trkeditmenu2");
  if (ImGui::BeginPopup("##trkeditmenu2")) {
    editMenuItems(a, t, ed, e, inst, k, frames, openPopup);
    ImGui::EndPopup();
  }
  if (ImGui::IsItemActive() && ed.marker >= 0 && ed.marker < 4) {
    const auto f = static_cast<int32_t>(std::lround(std::clamp(fOf(mouse.x), 0.0, static_cast<double>(frames))));
    const int32_t ls = sd.loopStart, le = sd.loopStart + sd.loopLength;
    switch (ed.marker) {
      case 0: cutA = std::clamp(f, 0, s1 - 1); break;
      case 1: {
        const int32_t v = std::clamp(f, s0 + 1, frames);
        cutB = v >= frames ? 0 : v;
        break;
      }
      case 2: {
        const int32_t v = std::clamp(f, s0, le - 1);
        sd.loopStart = v;
        sd.loopLength = le - v;
        break;
      }
      default: sd.loopLength = std::clamp(f, ls + 1, s1) - ls; break;
    }
    static const char* labels[] = {"sample start", "sample end", "loop start", "loop end"};
    commit(a, t, e, labels[ed.marker], mergeKey(t, 100 + ed.marker));
  }
  if (ImGui::IsItemDeactivated()) ed.marker = -1;
  if (hov) {
    const ImGuiIO& io = ImGui::GetIO();
    float wh = io.MouseWheel, wx = io.MouseWheelH;
    if (io.KeyShift && wx == 0) std::swap(wh, wx);
    if (wh != 0) {
      const double c = fOf(mouse.x);
      const double len = std::clamp(ed.viewLen * (wh > 0 ? 0.8 : 1.25), std::min<double>(frames, 64.0), static_cast<double>(frames));
      ed.viewStart = std::clamp(c - (c - ed.viewStart) * len / ed.viewLen, 0.0, frames - len);
      ed.viewLen = len;
      ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }
    if (wx != 0) {
      ed.viewStart = std::clamp(ed.viewStart - wx * ed.viewLen * 0.1, 0.0, frames - ed.viewLen);
      ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
    }
    a.setHint(i18n::tr("Drag the flags: sample start and end (below), loop start and end (above). Drag elsewhere: a range. Right-click: edit. Wheel: zoom"));
  }
  // Scroll bar.
  const ImVec2 b0(p0.x, p1.y + 4), b1(p1.x, p1.y + 10);
  dl->AddRectFilled(b0, b1, withAlpha(pal().surface900, 255), 3);
  const float t0 = b0.x + static_cast<float>(ed.viewStart / frames) * W, t1 = b0.x + static_cast<float>((ed.viewStart + ed.viewLen) / frames) * W;
  ImGui::SetCursorScreenPos(b0);
  ImGui::InvisibleButton("##wscroll", ImVec2(W, b1.y - b0.y));
  const bool sh = ImGui::IsItemHovered() || ImGui::IsItemActive();
  dl->AddRectFilled(ImVec2(t0, b0.y), ImVec2(std::max(t0 + 6, t1), b1.y), sh ? pal().textDim : pal().textFaint, 3);
  if (ImGui::IsItemActivated()) {
    ed.grab = ImGui::GetIO().MousePos.x < t0 || ImGui::GetIO().MousePos.x > t1 ? -1 : ImGui::GetIO().MousePos.x - t0;
  }
  if (ImGui::IsItemActive()) {
    const float mx = ImGui::GetIO().MousePos.x;
    const double g = ed.grab < 0 ? (t1 - t0) * 0.5 : ed.grab;
    ed.viewStart = std::clamp((mx - g - b0.x) / W * frames, 0.0, frames - ed.viewLen);
  }
  // Readout.
  ImGui::SetCursorScreenPos(ImVec2(p0.x, b1.y + 4));
  char rd[200];
  const double rate = sp->sampleRate > 0 ? sp->sampleRate : 44100.0;
  const Span shown = selectionOf(ed, k);
  if (!shown.empty())
    std::snprintf(rd, sizeof rd, i18n::tr("Range %d to %d   %d frames   %.3f s"), shown.a, shown.b, shown.b - shown.a, (shown.b - shown.a) / rate);
  else if (looping)
    std::snprintf(rd, sizeof rd, i18n::tr("Start %d   End %d   Loop %d to %d   %.2f s at %.1f kHz"), s0, s1, sd.loopStart, sd.loopStart + sd.loopLength,
                  (s1 - s0) / rate, rate / 1000.0);
  else
    std::snprintf(rd, sizeof rd, i18n::tr("Start %d   End %d   %.2f s at %.1f kHz"), s0, s1, (s1 - s0) / rate, rate / 1000.0);
  ImGui::PushFont(a.fontMono, 10.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, pal().textDim);
  ImGui::TextUnformatted(rd);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (openPopup) ImGui::OpenPopup(openPopup);
  editPopups(a, t, ed, e, inst, k);
  pui::endPanel();
}

// ---- the samples list and the selected sample's settings ----

void samplesPanel(App& a, const PluginTarget& t, Ed& ed, Edit& e, const InstrumentAux& inst, int sound, float w, float h) {
  const int n = sampleCount(e);
  char cap[48];
  std::snprintf(cap, sizeof cap, "%d / %d", n, trk::kMaxSamples);
  pui::beginPanel(i18n::tr("SAMPLES"), w, h, cap);
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  sampleDropTarget(a, t, ed, ImVec2(wp.x + 1, wp.y + 1), ImVec2(wp.x + ws.x - 1, wp.y + ws.y - 1), sound, n >= trk::kMaxSamples);
  const float inner = w - 20;
  const float lineH = ImGui::GetTextLineHeight() + 6;
  const float knobsH = 26 + 2 * ImGui::GetTextLineHeight() + 24;
  const float rowH = 24;
  const float listH = std::max(lineH * 2, wp.y + ws.y - 8 - knobsH - rowH - 8 - ImGui::GetCursorScreenPos().y);
  // The list.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, pal().surface900);
  ImGui::BeginChild("##trklist", ImVec2(inner, listH), ImGuiChildFlags_None);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lw = ImGui::GetContentRegionAvail().x;
    if (n == 0) {
      ImGui::SetCursorPos(ImVec2(8, 6));
      if (inst.builtin > 0) {
        ImGui::SetCursorPos(ImVec2(0, 0));
        const ImVec2 r0 = ImGui::GetCursorScreenPos();
        const bool playingIt = gPreview.serial && gPreview.key == t.key();
        if (ImGui::InvisibleButton("##builtinrow", ImVec2(lw, lineH))) {
          if (playingIt) stopPreview(a);
          else startPreview(a, t, e, inst, 0);
        }
        if (ImGui::IsItemHovered()) {
          dl->AddRectFilled(r0, ImVec2(r0.x + lw, r0.y + lineH), withAlpha(pal().text, 14));
          a.setHint(i18n::tr("Click to hear the built-in sound; click again to stop"));
        }
        const ImVec2 c(r0.x + 8, r0.y + 3);
        dl->AddRectFilled(ImVec2(c.x, c.y + 2), ImVec2(c.x + 6, c.y + lineH - 8), slotColour(0), 2.0f);
        char b[96];
        std::snprintf(b, sizeof b, i18n::tr("Built-in: %s"), soundName(inst.builtin));
        dl->AddText(ImVec2(c.x + 14, r0.y + (lineH - ImGui::GetTextLineHeight()) * 0.5f), pal().text, b);
        if (playingIt) ui::drawIconBox(dl, ui::Icon::Play, ImVec2(r0.x + lw - 16, r0.y + lineH * 0.5f), 9, pal().hot);
      } else {
        dimCaption(i18n::tr("No samples yet"));
      }
    }
    for (int k = 0; k < trk::kMaxSamples; ++k) {
      if (!hasSample(e, k)) continue;
      ImGui::PushID(k);
      const ImVec2 c = ImGui::GetCursorScreenPos();
      const bool sel = ed.sel == k;
      if (ImGui::InvisibleButton("##row", ImVec2(lw, lineH))) {
        // K-129: a click selects the sample and plays it; a click on the one playing stops it.
        const bool playingThis = gPreview.serial && gPreview.key == t.key() && gPreview.slot == k;
        ed.sel = k;
        if (playingThis) stopPreview(a);
        else startPreview(a, t, e, inst, k);
      }
      const bool hv = ImGui::IsItemHovered();
      const bool playingRow = gPreview.serial && gPreview.key == t.key() && gPreview.slot == k;
      if (sel) dl->AddRectFilled(c, ImVec2(c.x + lw, c.y + lineH), withAlpha(pal().accent, 56));
      else if (hv) dl->AddRectFilled(c, ImVec2(c.x + lw, c.y + lineH), withAlpha(pal().text, 14));
      dl->AddRectFilled(ImVec2(c.x + 6, c.y + 5), ImVec2(c.x + 12, c.y + lineH - 5), slotColour(k), 2.0f);
      char num[8];
      std::snprintf(num, sizeof num, "%02d", k + 1);
      ImGui::PushFont(a.fontMono, 10.5f);
      const float ty = c.y + (lineH - ImGui::GetTextLineHeight()) * 0.5f;
      dl->AddText(ImVec2(c.x + 18, ty), pal().textDim, num);
      ImGui::PopFont();
      const std::string& nm = e.text.sampleNames[static_cast<size_t>(k)];
      const trk::SampleDef& s = e.text.def.samples[static_cast<size_t>(k)];
      const bool missing = !inst.files[static_cast<size_t>(k)];
      char info[48];
      if (missing) std::snprintf(info, sizeof info, "%s", i18n::tr("missing"));
      else std::snprintf(info, sizeof info, "%s", s.loop == trk::kLoopPingPong ? i18n::tr("ping-pong") : s.loop == trk::kLoopForward ? i18n::tr("loop") : "");
      const float iw = ImGui::CalcTextSize(info).x;
      const float nx = c.x + 42, nmax = c.x + lw - 10 - iw - (iw > 0 ? 8 : 0);
      const float ny = c.y + (lineH - ImGui::GetTextLineHeight()) * 0.5f;
      dl->PushClipRect(ImVec2(nx, c.y), ImVec2(std::max(nx, nmax), c.y + lineH), true);
      dl->AddText(ImVec2(nx, ny), missing ? pal().danger : pal().text, nm.empty() ? i18n::tr("(no name)") : nm.c_str());
      dl->PopClipRect();
      if (iw > 0) dl->AddText(ImVec2(c.x + lw - 10 - iw, ny), missing ? pal().danger : pal().textFaint, info);
      if (playingRow) ui::drawIconBox(dl, ui::Icon::Play, ImVec2(c.x + 32, c.y + lineH * 0.5f), 9, pal().hot);
      if (hv && !playingRow) a.setHint(i18n::tr("Click to select the sample and hear it; click again to stop"));
      if (hv) {
        const std::string file = e.text.fileOf[static_cast<size_t>(k)] >= 0 ? e.text.files[static_cast<size_t>(e.text.fileOf[static_cast<size_t>(k)])] : std::string();
        if (!file.empty() && playingRow) a.setHint(file);
      }
      ImGui::PopID();
    }
    // K-129: "Add file" after the samples (Gridline's file dialog; the file is added as a drop adds it).
    if (n < trk::kMaxSamples) {
      ImGui::Dummy(ImVec2(0, 2));
      ImGui::SetCursorPosX(6);
      if (ui::TextButton(i18n::tr("Add file..."), false, 0, i18n::tr("Choose an audio file to add as a sample")))
        addFileDialog(a, t, sound);
      ImGui::Dummy(ImVec2(0, 2));
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  const int k = ed.sel;
  const bool have = hasSample(e, k);
  // Name, order, delete.
  {
    ImGui::BeginDisabled(!have);
    if (ed.nameSlot != k || (!ImGui::IsAnyItemActive() && ed.nameData != e.text.sampleNames[static_cast<size_t>(k)])) {
      ed.nameSlot = k;
      ed.nameData = e.text.sampleNames[static_cast<size_t>(k)];
      std::snprintf(ed.name, sizeof ed.name, "%s", ed.nameData.c_str());
    }
    const float bw = 24;
    ImGui::SetNextItemWidth(inner - 4 * (bw + 2) - 4);
    ImGui::InputTextWithHint("##sname", i18n::tr("Sample name"), ed.name, sizeof ed.name);
    if (ImGui::IsItemDeactivatedAfterEdit() && have) {
      e.text.sampleNames[static_cast<size_t>(k)] = ed.name;
      ed.nameData = ed.name;
      commit(a, t, e, "rename sample");
    }
    int prev = -1, next = -1;
    for (int j = k - 1; j >= 0 && prev < 0; --j)
      if (hasSample(e, j)) prev = j;
    for (int j = k + 1; j < trk::kMaxSamples && next < 0; ++j)
      if (hasSample(e, j)) next = j;
    if (prev < 0 && k > 0) prev = k - 1;
    if (next < 0 && k + 1 < n) next = k + 1;
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(k == 0);
    if (ui::IconButton("##sup", ui::Icon::Up, false, bw, i18n::tr("Move up")) && k > 0) {
      const int to = prev >= 0 ? prev : k - 1;
      swapSamples(e, k, to);
      ed.sel = to;
      commit(a, t, e, "move sample");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(k >= trk::kMaxSamples - 1);
    if (ui::IconButton("##sdown", ui::Icon::Down, false, bw, i18n::tr("Move down")) && k < trk::kMaxSamples - 1) {
      const int to = next >= 0 ? next : k + 1;
      swapSamples(e, k, to);
      ed.sel = to;
      commit(a, t, e, "move sample");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(n >= trk::kMaxSamples);
    if (ui::IconButton("##sdup", ui::Icon::Clone, false, bw, i18n::tr("Copy the sample into a free slot")) && have) {
      const int to = duplicateSample(e, k);
      if (to >= 0) {
        ed.sel = to;
        commit(a, t, e, "copy sample");
      }
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    if (ui::IconButton("##sdel", ui::Icon::Trash, false, bw, i18n::tr("Remove the sample")) && have) {
      removeSample(e, k);
      commit(a, t, e, "remove sample");
      for (int j = 0; j < trk::kMaxSamples; ++j)
        if (hasSample(e, j)) {
          ed.sel = j;
          break;
        }
    }
    ImGui::EndDisabled();
  }
  // Volume, pan, fine tune, relative note, root.
  {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::BeginDisabled(!have);
    trk::SampleDef& s = e.text.def.samples[static_cast<size_t>(k)];
    const SamplePtr sp = inst.files[static_cast<size_t>(k)];
    const double rate = sp && sp->sampleRate > 0 ? sp->sampleRate : 8363.0;
    const float cell = std::floor(inner / 5.0f);
    char b[32];
    double v = s.volume;
    std::snprintf(b, sizeof b, "%d", s.volume);
    if (valueKnob(a, "vol", v, 0, 64, 64, i18n::tr("Volume"), b, cell)) {
      s.volume = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 64L));
      commit(a, t, e, "sample volume", mergeKey(t, 1));
    }
    ImGui::SameLine(0, 0);
    v = static_cast<int>(s.pan) - 128;
    if (s.pan == 128) std::snprintf(b, sizeof b, "C");
    else std::snprintf(b, sizeof b, "%s%d", s.pan < 128 ? "L" : "R", std::abs(static_cast<int>(s.pan) - 128));
    if (valueKnob(a, "pan", v, -128, 127, 0, i18n::tr("Pan"), b, cell)) {
      s.pan = static_cast<uint8_t>(std::clamp(std::lround(v) + 128, 0L, 255L));
      commit(a, t, e, "sample pan", mergeKey(t, 2));
    }
    ImGui::SameLine(0, 0);
    v = s.finetune;
    std::snprintf(b, sizeof b, "%+d", s.finetune);
    if (valueKnob(a, "fine", v, -128, 127, 0, i18n::tr("Fine"), b, cell)) {
      s.finetune = static_cast<int8_t>(std::clamp(std::lround(v), -128L, 127L));
      commit(a, t, e, "sample fine tune", mergeKey(t, 3));
    }
    ImGui::SameLine(0, 0);
    v = s.relNote;
    std::snprintf(b, sizeof b, "%+d", s.relNote);
    if (valueKnob(a, "rel", v, -48, 71, 0, i18n::tr("Transpose"), b, cell)) {
      s.relNote = static_cast<int8_t>(std::clamp(std::lround(v), -48L, 71L));
      commit(a, t, e, "sample transpose", mergeKey(t, 4));
    }
    ImGui::SameLine(0, 0);
    const int root = rootNote(s, rate);
    v = root;
    if (valueKnob(a, "root", v, 1, 96, 49, i18n::tr("Root"), keyName(root - 1), cell)) {
      setRootNote(s, rate, static_cast<int>(std::lround(v)));
      commit(a, t, e, "sample root", mergeKey(t, 5));
    }
    ImGui::EndDisabled();
  }
  pui::endPanel();
}

// ---- envelopes ----

// K-129: the envelope slots, kept with the user's settings.
std::string envSlotsPath(App& a) { return (fs::u8path(a.configDir()) / "tracker-envelopes.txt").u8string(); }
EnvSlots& envSlots(App& a) {
  static EnvSlots s;
  static bool loaded = false;
  if (!loaded) {
    s = loadEnvSlots(envSlotsPath(a));
    loaded = true;
  }
  return s;
}
int gSlotMenu = -1;

void envelopePanel(App& a, const PluginTarget& t, Ed& ed, Edit& e, bool pan, float w, float h) {
  const int which = pan ? 1 : 0;
  trk::Envelope& env = pan ? e.text.def.pan : e.text.def.vol;
  pui::beginPanel(pan ? i18n::tr("PANNING ENVELOPE") : i18n::tr("VOLUME ENVELOPE"), w, h);
  ImGui::PushID(which);
  const float inner = w - 20;
  const char* label = pan ? "panning envelope" : "volume envelope";
  auto toggle = [&](const char* id, const char* text, uint8_t flag, const char* hint) {
    const bool on = (env.flags & flag) != 0;
    ImGui::PushID(id);
    if (ui::TextButton(text, on, 0, hint)) {
      env.flags = static_cast<uint8_t>(on ? env.flags & ~flag : env.flags | flag);
      if (env.count == 0 && !on) {  // switched on with no points: a flat line to start from
        insertEnvPoint(env, 0, pan ? 32 : 64);
        insertEnvPoint(env, 16, pan ? 32 : 64);
      }
      commit(a, t, e, label);
    }
    ImGui::PopID();
  };
  toggle("on", i18n::tr("On"), trk::kEnvOn, pan ? i18n::tr("The panning envelope moves the sound while the note plays")
                                               : i18n::tr("The volume envelope shapes every note; off: notes stop when let go"));
  ImGui::SameLine(0, 4);
  toggle("sus", i18n::tr("Sustain"), trk::kEnvSustain, i18n::tr("The envelope holds at the sustain point while the key is down"));
  ImGui::SameLine(0, 4);
  toggle("loop", i18n::tr("Loop"), trk::kEnvLoop, i18n::tr("The envelope repeats between the loop points"));
  // K-129: six slots: a click puts the slot's envelope in, a right click keeps this one there.
  {
    EnvSlots& slots = envSlots(a);
    ImGui::SameLine(0, 10);
    for (int k = 0; k < kEnvSlots; ++k) {
      if (k > 0) ImGui::SameLine(0, 2);
      char lb[16];
      std::snprintf(lb, sizeof lb, "%d##slot%d", k + 1, k);
      char hint[200];
      std::snprintf(hint, sizeof hint,
                    pan ? i18n::tr("Slot %d: click to use its panning envelope, right-click to keep this one there")
                        : i18n::tr("Slot %d: click to use its volume envelope, fadeout and vibrato, right-click to keep these there"),
                    k + 1);
      if (ui::TextButton(lb, false, 20, hint)) {
        applyEnvSlot(e.text.def, pan ? slots.pan[static_cast<size_t>(k)] : slots.vol[static_cast<size_t>(k)], pan);
        commit(a, t, e, pan ? "panning envelope slot" : "volume envelope slot");
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        gSlotMenu = k;
        ImGui::OpenPopup("##envslot");
      }
    }
    if (ImGui::BeginPopup("##envslot")) {
      const int k = std::clamp(gSlotMenu, 0, kEnvSlots - 1);
      char it[96];
      std::snprintf(it, sizeof it, i18n::tr("Keep this envelope in slot %d"), k + 1);
      if (ImGui::MenuItem(it)) {
        (pan ? slots.pan : slots.vol)[static_cast<size_t>(k)] = takeEnvSlot(e.text.def, pan);
        if (!saveEnvSlots(envSlotsPath(a), slots)) a.notify(i18n::tr("The envelope slots could not be saved"), true);
      }
      std::snprintf(it, sizeof it, i18n::tr("Give slot %d its starting shape again"), k + 1);
      if (ImGui::MenuItem(it)) {
        const EnvSlots f = factoryEnvSlots();
        (pan ? slots.pan : slots.vol)[static_cast<size_t>(k)] = (pan ? f.pan : f.vol)[static_cast<size_t>(k)];
        if (!saveEnvSlots(envSlotsPath(a), slots)) a.notify(i18n::tr("The envelope slots could not be saved"), true);
      }
      ImGui::EndPopup();
    }
  }
  {
    char c[48];
    std::snprintf(c, sizeof c, i18n::tr("%d of %d points"), static_cast<int>(env.count), trk::kEnvPoints);
    const float tw = ImGui::CalcTextSize(c).x;
    ImGui::SameLine();
    if (ImGui::GetCursorPosX() + 8 + tw <= 10 + inner) {  // only where it fits beside the slots
      ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), 10 + inner - tw));
      ImGui::AlignTextToFramePadding();
      dimCaption(c);
    } else {
      ImGui::Dummy(ImVec2(0, 0));
    }
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + inner, wp.y + ws.y - 10);
  pui::displayFrame(dl, p0, p1);
  pui::drawGrid(dl, p0, p1, 8, 4, pan);
  const bool on = env.on();
  // The ticks shown: past the last point, steady while dragging.
  int lastX = 0;
  for (int i = 0; i < env.count; ++i) lastX = std::max<int>(lastX, env.x[static_cast<size_t>(i)]);
  if (ed.envDrag[which] < 0 || ed.envSpan[which] <= 0) ed.envSpan[which] = static_cast<float>(std::max(48, static_cast<int>(std::ceil(lastX * 1.25 / 16.0)) * 16));
  const float span = ed.envSpan[which];
  const float gx0 = p0.x + 8, gx1 = p1.x - 8, gy0 = p0.y + 8, gy1 = p1.y - 8;
  auto pt = [&](int i) {
    return ImVec2(gx0 + (gx1 - gx0) * static_cast<float>(env.x[static_cast<size_t>(i)]) / span,
                  gy1 - (gy1 - gy0) * static_cast<float>(env.y[static_cast<size_t>(i)]) / 64.0f);
  };
  const ImU32 line = on ? pal().hot : pal().textFaint;
  if ((env.flags & trk::kEnvLoop) && env.loopStart < env.count && env.loopEnd < env.count) {
    const ImVec2 la = pt(env.loopStart), lb = pt(env.loopEnd);
    dl->AddRectFilled(ImVec2(la.x, p0.y + 1), ImVec2(std::max(la.x + 2, lb.x), p1.y - 1), withAlpha(pal().accent, on ? 40 : 20));
    dl->AddLine(ImVec2(la.x, p0.y + 1), ImVec2(la.x, p1.y - 1), withAlpha(pal().accent, 160));
    dl->AddLine(ImVec2(lb.x, p0.y + 1), ImVec2(lb.x, p1.y - 1), withAlpha(pal().accent, 160));
  }
  if ((env.flags & trk::kEnvSustain) && env.sustain < env.count) {
    const ImVec2 s = pt(env.sustain);
    for (float y = p0.y + 2; y < p1.y - 2; y += 6) dl->AddLine(ImVec2(s.x, y), ImVec2(s.x, std::min(y + 3, p1.y - 2)), pal().text, 1.0f);
  }
  if (env.count == 0) {
    const char* empty = i18n::tr("Click to add the first points");
    const ImVec2 ts = ImGui::CalcTextSize(empty);
    dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, p0.y + (p1.y - p0.y) * 0.25f - ts.y * 0.5f), pal().textDim, empty);
  }
  for (int i = 1; i < env.count; ++i) dl->AddLine(pt(i - 1), pt(i), line, 1.75f);
  if (env.count > 0) {
    const ImVec2 lp = pt(env.count - 1);
    dl->AddLine(lp, ImVec2(gx1, lp.y), withAlpha(line, 90), 1.0f);
  }
  // Points and interaction.
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##envg", ImVec2(p1.x - p0.x, p1.y - p0.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 m = ImGui::GetIO().MousePos;
  int hot = -1;
  if (hov && ed.envDrag[which] < 0) {
    float best = 8;
    for (int i = 0; i < env.count; ++i) {
      const ImVec2 q = pt(i);
      const float d = std::hypot(q.x - m.x, q.y - m.y);
      if (d < best) {
        best = d;
        hot = i;
      }
    }
  }
  auto toTicks = [&](float x) { return static_cast<int>(std::lround((x - gx0) / (gx1 - gx0) * span)); };
  auto toLevel = [&](float y) { return std::clamp(static_cast<int>(std::lround((gy1 - y) / (gy1 - gy0) * 64.0f)), 0, 64); };
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hot >= 0) {
      ed.envDrag[which] = hot;
    } else {
      const bool empty = env.count == 0;
      if (empty) insertEnvPoint(env, 0, toLevel(m.y));  // the first point sits at the start
      const int i = insertEnvPoint(env, std::max(1, toTicks(m.x)), toLevel(m.y));
      if (i >= 0) {
        if (empty) env.flags |= trk::kEnvOn;
        ed.envDrag[which] = i;
        commit(a, t, e, label, mergeKey(t, 20 + which));
      }
    }
  }
  if (ImGui::IsItemActive() && ed.envDrag[which] >= 0 && ed.envDrag[which] < env.count && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const int i = ed.envDrag[which];
    const int lo = i == 0 ? 0 : env.x[static_cast<size_t>(i - 1)] + 1;
    const int hi = i == 0 ? 0 : i + 1 < env.count ? env.x[static_cast<size_t>(i + 1)] - 1 : 32767;
    const int nx = std::clamp(toTicks(m.x), lo, std::max(lo, hi));
    const int ny = toLevel(m.y);
    if (nx != env.x[static_cast<size_t>(i)] || ny != env.y[static_cast<size_t>(i)]) {
      env.x[static_cast<size_t>(i)] = static_cast<int16_t>(nx);
      env.y[static_cast<size_t>(i)] = static_cast<int16_t>(ny);
      commit(a, t, e, label, mergeKey(t, 20 + which));
    }
  }
  if (!ImGui::IsItemActive()) ed.envDrag[which] = -1;
  if (hov && hot >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot > 0 && env.count > 2) {
    deleteEnvPoint(env, hot);
    ed.envDrag[which] = -1;
    commit(a, t, e, label);
  }
  if (hov && hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ed.envMenu[which] = hot;
    ImGui::OpenPopup("##envpt");
  }
  if (ImGui::BeginPopup("##envpt")) {
    const int i = ed.envMenu[which];
    if (i >= 0 && i < env.count) {
      if (ImGui::MenuItem(i18n::tr("Sustain point"), nullptr, (env.flags & trk::kEnvSustain) && env.sustain == i)) {
        env.sustain = static_cast<uint8_t>(i);
        env.flags |= trk::kEnvSustain;
        commit(a, t, e, label);
      }
      if (ImGui::MenuItem(i18n::tr("Loop start"), nullptr, (env.flags & trk::kEnvLoop) && env.loopStart == i)) {
        env.loopStart = static_cast<uint8_t>(i);
        if (env.loopEnd < i) env.loopEnd = static_cast<uint8_t>(i);
        env.flags |= trk::kEnvLoop;
        commit(a, t, e, label);
      }
      if (ImGui::MenuItem(i18n::tr("Loop end"), nullptr, (env.flags & trk::kEnvLoop) && env.loopEnd == i)) {
        env.loopEnd = static_cast<uint8_t>(i);
        if (env.loopStart > i) env.loopStart = static_cast<uint8_t>(i);
        env.flags |= trk::kEnvLoop;
        commit(a, t, e, label);
      }
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Delete point"), nullptr, false, i > 0 && env.count > 2)) {
        deleteEnvPoint(env, i);
        commit(a, t, e, label);
      }
    }
    ImGui::EndPopup();
  }
  for (int i = 0; i < env.count; ++i) {
    const ImVec2 q = pt(i);
    const bool hi = i == hot || i == ed.envDrag[which];
    dl->AddCircleFilled(q, hi ? 5.0f : 3.5f, hi ? pal().text : line);
    if ((env.flags & trk::kEnvSustain) && env.sustain == i) dl->AddCircle(q, 7.0f, pal().text, 0, 1.25f);
  }
  if (hov) {
    char hint[200];
    const int i = hot >= 0 ? hot : ed.envDrag[which];
    if (i >= 0 && i < env.count)
      std::snprintf(hint, sizeof hint, i18n::tr("Point %d: tick %d, level %d   Drag to move, double-click to delete, right-click: sustain and loop"), i + 1,
                    static_cast<int>(env.x[static_cast<size_t>(i)]), static_cast<int>(env.y[static_cast<size_t>(i)]));
    else
      std::snprintf(hint, sizeof hint, "%s", i18n::tr("Click to add a point (12 at most)"));
    a.setHint(hint);
  }
  ImGui::PopID();
  pui::endPanel();
}

// ---- the instrument: name, fadeout and auto vibrato ----

void instrumentPanel(App& a, const PluginTarget& t, Ed& ed, Edit& e, float w) {
  pui::beginPanel(i18n::tr("INSTRUMENT"), w, 0);
  trk::InstrumentDef& d = e.text.def;
  const float cell = 60;
  // The name (K-129).
  ImGui::BeginGroup();
  groupCaption(i18n::tr("Name"));
  if (!ed.insNameSet || (!ImGui::IsAnyItemActive() && ed.insNameData != e.text.name)) {
    ed.insNameData = e.text.name;
    std::snprintf(ed.insName, sizeof ed.insName, "%s", ed.insNameData.c_str());
    ed.insNameSet = true;
  }
  const float nameW = std::clamp(w - 20 - cell * 4 - 4 * 52 - 60, 96.0f, 170.0f);
  ImGui::SetNextItemWidth(nameW);
  ImGui::InputTextWithHint("##insname", i18n::tr("Instrument name"), ed.insName, sizeof ed.insName);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    e.text.name = ed.insName;
    ed.insNameData = e.text.name;
    commit(a, t, e, "rename instrument");
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, 14);
  char b[32];
  double v = d.fadeout;
  std::snprintf(b, sizeof b, "%d", d.fadeout);
  if (valueKnob(a, "fade", v, 0, 4095, 0, i18n::tr("Fadeout"), b, cell)) {
    d.fadeout = static_cast<uint16_t>(std::clamp(std::lround(v), 0L, 4095L));
    commit(a, t, e, "fadeout", mergeKey(t, 30));
  }
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("How fast notes fade after the key is let go (with the volume envelope on)"));
  ImGui::SameLine(0, 12);
  ImGui::BeginGroup();
  groupCaption(i18n::tr("Vibrato shape"));
  const char* shapes[] = {i18n::tr("Sine"), i18n::tr("Square"), i18n::tr("Ramp up"), i18n::tr("Ramp down")};
  int shape = d.vibType;
  const float segW = std::clamp((w - 20 - nameW - cell * 4 - 50) / 4.0f, 46.0f, 72.0f);
  if (ui::Segmented("##vibshape", shapes, 4, &shape, segW, ImGui::GetFrameHeight())) {  // the name field's height: one row
    d.vibType = static_cast<uint8_t>(shape);
    commit(a, t, e, "vibrato shape");
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, 10);
  v = d.vibSweep;
  std::snprintf(b, sizeof b, "%d", d.vibSweep);
  if (valueKnob(a, "sweep", v, 0, 255, 0, i18n::tr("Sweep"), b, cell)) {
    d.vibSweep = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    commit(a, t, e, "vibrato sweep", mergeKey(t, 31));
  }
  ImGui::SameLine(0, 0);
  v = d.vibDepth;
  std::snprintf(b, sizeof b, "%d", d.vibDepth);
  if (valueKnob(a, "depth", v, 0, 15, 0, i18n::tr("Depth"), b, cell)) {
    d.vibDepth = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 15L));
    commit(a, t, e, "vibrato depth", mergeKey(t, 32));
  }
  ImGui::SameLine(0, 0);
  v = d.vibRate;
  std::snprintf(b, sizeof b, "%d", d.vibRate);
  if (valueKnob(a, "rate", v, 0, 63, 0, i18n::tr("Rate"), b, cell)) {
    d.vibRate = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 63L));
    commit(a, t, e, "vibrato rate", mergeKey(t, 33));
  }
  pui::endPanel();
}

// ---- K-129: MIDI: the notes out to a MIDI device, the wheel range, muting the own sound ----

void midiPanel(App& a, const PluginTarget& t, Edit& e, float w) {
  pui::beginPanel(i18n::tr("MIDI"), w, 0);
  MidiSettings& m = e.midi;
  const float cell = 56;
  ImGui::BeginGroup();
  groupCaption(i18n::tr("Output"));
  if (ui::TextButton(i18n::tr("Send notes"), m.on, 96, i18n::tr("The notes this instrument plays also go to the MIDI output, on its channel and program"))) {
    m.on = !m.on;
    commit(a, t, e, "MIDI output");
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, 12);
  char b[32];
  ImGui::BeginDisabled(!m.on);
  double v = m.channel + 1;
  std::snprintf(b, sizeof b, "%d", m.channel + 1);
  if (valueKnob(a, "mch", v, 1, 16, 1, i18n::tr("Channel"), b, cell)) {
    m.channel = static_cast<int>(std::clamp(std::lround(v), 1L, 16L)) - 1;
    commit(a, t, e, "MIDI channel", mergeKey(t, 40));
  }
  ImGui::SameLine(0, 0);
  v = m.program + 1;
  std::snprintf(b, sizeof b, "%d", m.program + 1);
  if (valueKnob(a, "mprg", v, 1, 128, 1, i18n::tr("Program"), b, cell)) {
    m.program = static_cast<int>(std::clamp(std::lround(v), 1L, 128L)) - 1;
    commit(a, t, e, "MIDI program", mergeKey(t, 41));
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, 0);
  v = m.bend;
  std::snprintf(b, sizeof b, "%d", m.bend);
  if (valueKnob(a, "mbend", v, 0, 36, 2, i18n::tr("Bend range"), b, cell)) {
    m.bend = static_cast<int>(std::clamp(std::lround(v), 0L, 36L));
    commit(a, t, e, "pitch wheel range", mergeKey(t, 42));
  }
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Semitones the pitch wheel bends played notes"));
  ImGui::SameLine(0, 12);
  ImGui::BeginGroup();
  groupCaption(i18n::tr("Own sound"));
  if (ui::TextButton(i18n::tr("Mute"), m.mute, 64, i18n::tr("Silences the instrument's own sound; notes sent to MIDI still go out"))) {
    m.mute = !m.mute;
    e.text.def.mute = m.mute;
    commit(a, t, e, "mute instrument");
  }
  ImGui::EndGroup();
  pui::endPanel();
}

// ---- key map ----

void keyMapPanel(App& a, const PluginTarget& t, Ed& ed, Edit& e, const InstrumentAux& inst, float w) {
  pui::beginPanel(i18n::tr("KEY MAP"), w, 0, i18n::tr("C0 to B7"));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float kw = (w - 20) / 96.0f, kh = 28;
  ImGui::InvisibleButton("##keys", ImVec2(kw * 96, kh));
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 m = ImGui::GetIO().MousePos;
  const int under = std::clamp(static_cast<int>((m.x - p0.x) / kw), 0, 95);
  const bool canMap = hasSample(e, ed.sel);
  if (ImGui::IsItemActivated() && canMap) ed.keyFrom = ed.keyTo = under;
  // K-129: pressing a key also plays it (the strip lights it like every sounding key).
  if (ImGui::IsItemActivated() && t.isChannel()) {
    ed.keyHeld = stripKeyPitch(under);
    ensureSweep(a);
    gHeld.hold(NoteGuard::kStrip, t.key(), t.owner, ed.keyHeld, 0, frameNow());
    a.engine.noteOn(t.owner, ed.keyHeld, 100.0f / 127.0f);
  }
  // The key sounds while it is pressed, the strip is in view and the editor has the focus; the
  // frame sweep ends it when the editor is not drawn at all.
  if (ed.keyHeld >= 0) {
    if (ImGui::IsItemActive() && ImGui::IsItemVisible() && editorFocused()) {
      gHeld.seen(NoteGuard::kStrip, frameNow());
    } else {
      a.engine.noteOff(t.owner, ed.keyHeld);
      ed.keyHeld = -1;
      gHeld.drop(NoteGuard::kStrip);
    }
  }
  if (ImGui::IsItemActive() && ed.keyFrom >= 0) ed.keyTo = under;
  if (ImGui::IsItemDeactivated() && ed.keyFrom >= 0) {
    mapKeys(e, ed.keyFrom, ed.keyTo, ed.sel);
    commit(a, t, e, "map keys");
    ed.keyFrom = ed.keyTo = -1;
  }
  const int lo = std::min(ed.keyFrom, ed.keyTo), hi = std::max(ed.keyFrom, ed.keyTo);
  const bool builtin = inst.builtin > 0 && sampleCount(e) == 0;
  for (int n = 0; n < 96; ++n) {
    const int pc = n % 12;
    const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    const ImVec2 a0(p0.x + kw * static_cast<float>(n), p0.y), a1(p0.x + kw * static_cast<float>(n + 1) - 1, p0.y + kh);
    const bool picking = ed.keyFrom >= 0 && n >= lo && n <= hi;
    const int slot = picking ? ed.sel : builtin ? 0 : e.text.def.keymap[static_cast<size_t>(n)] & 15;
    const bool has = builtin || hasSample(e, slot);
    ImU32 col = has ? slotColour(slot, black ? 150 : 225) : (black ? pal().surface900 : pal().surface700);
    dl->AddRectFilled(a0, a1, col);
    if (picking) dl->AddRect(ImVec2(a0.x, a0.y), ImVec2(a1.x, a1.y), pal().text);
    if (black) dl->AddRectFilled(ImVec2(a0.x, a0.y), ImVec2(a1.x, a0.y + kh * 0.55f), withAlpha(pal().surface950, 70));
    // K-129: sounding keys light up (every held key at once, fading back on release). The wash
    // covers the lower part so the sample colour stays readable above it.
    const float lit = t.isChannel() ? keylight::key(a, t.owner, stripKeyPitch(n)) : 0.0f;
    if (lit > 0.001f) {
      dl->AddRectFilled(ImVec2(a0.x, a0.y + kh * 0.4f), a1, withAlpha(pal().accent, static_cast<int>(200 * lit)));
      dl->AddRectFilled(ImVec2(a0.x, a1.y - 4), a1, withAlpha(pal().text, static_cast<int>(230 * lit)));
    }
  }
  // Octave marks and names.
  ImGui::PushFont(a.fontMono, 10.0f);
  for (int o = 0; o < 8; ++o) {
    const float x = p0.x + kw * static_cast<float>(o * 12);
    dl->AddLine(ImVec2(x, p0.y + kh + 1), ImVec2(x, p0.y + kh + 5), pal().textFaint);
    char nm[8];
    std::snprintf(nm, sizeof nm, "C%d", o);
    dl->AddText(ImVec2(x + 2, p0.y + kh + 3), pal().textFaint, nm);
  }
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(w - 20, 14));
  if (hov) {
    char b[160];
    const int slot = builtin ? 0 : e.text.def.keymap[static_cast<size_t>(under)] & 15;
    if (builtin) std::snprintf(b, sizeof b, i18n::tr("%s plays the built-in sound"), keyName(under).c_str());
    else if (hasSample(e, slot)) std::snprintf(b, sizeof b, i18n::tr("%s plays sample %02d   Drag across keys to give them the selected sample"), keyName(under).c_str(), slot + 1);
    else std::snprintf(b, sizeof b, i18n::tr("%s plays nothing   Drag across keys to give them the selected sample"), keyName(under).c_str());
    a.setHint(b);
    ImGui::SetTooltip("%s", keyName(under).c_str());
  }
  pui::endPanel();
}

}  // namespace

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != trk::kInstrumentType) return false;
  const auto inst = instrumentFor(*st);
  const int sound = soundOf(*st);
  Edit e = readEdit(st->data, sound);
  Ed& ed = gEd[t.key()];
  tendPreview(a, t);
  if (!hasSample(e, ed.sel))
    for (int k = 0; k < trk::kMaxSamples; ++k)
      if (hasSample(e, k)) {
        ed.sel = k;
        break;
      }
  const float w = std::max(640.0f, width);

  // ---- playback: captions over a row of equal-height controls, two rows; the level on its own ----
  const float levelW = 96;
  const float playW = w - levelW - 8;
  pui::beginPanel(i18n::tr("PLAYBACK"), playW, 0, e.text.name.empty() ? nullptr : e.text.name.c_str());
  {
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float inner = playW - 20;
    const float capH = ImGui::GetTextLineHeight() + 2, rowH = capH + 22 + 8;
    const float colX[3] = {0, std::floor(inner * 0.40f), std::floor(inner * 0.66f)};
    auto caption = [&](int col, int row, const char* text) {
      ImGui::SetCursorScreenPos(ImVec2(o.x + colX[col], o.y + row * rowH));
      dimCaption(text);
    };
    auto at = [&](int col, int row) { ImGui::SetCursorScreenPos(ImVec2(o.x + colX[col], o.y + row * rowH + capH)); };
    caption(0, 0, i18n::tr("Interpolation"));
    caption(1, 0, i18n::tr("Rows per beat"));
    caption(2, 0, i18n::tr("Click removal"));
    at(0, 0);
    pui::segmented(a, t, trk::PInterp, colX[1] - 16);
    at(1, 0);
    pui::stepper(a, t, trk::PRowsPerBeat, std::min(140.0f, colX[2] - colX[1] - 16));
    at(2, 0);
    pui::led(a, t, trk::PRamp, i18n::tr("On"));
    caption(0, 1, i18n::tr("Pitch table"));
    caption(1, 1, i18n::tr("Ticks per row"));
    caption(2, 1, i18n::tr("Built-in sound"));
    at(0, 1);
    pui::segmented(a, t, trk::PAmiga, colX[1] - 16);
    at(1, 1);
    pui::stepper(a, t, trk::PTicks, std::min(140.0f, colX[2] - colX[1] - 16));
    at(2, 1);
    {  // the combo is a frame taller than the steppers: centred on their row
      const float comboH = ImGui::GetFrameHeight();
      const ImVec2 c = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + std::floor((22 - comboH) * 0.5f)));
    }
    pui::combo(a, t, trk::PSound, inner - colX[2]);
    if (ImGui::IsItemHovered()) a.setHint(i18n::tr("The sound the instrument plays while it has no sample of its own"));
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + 2 * rowH));
    ImGui::Dummy(ImVec2(inner, 1));
  }
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel(i18n::tr("LEVEL"), levelW, 0);
  pui::knob(a, t, trk::PLevel, i18n::tr("Level"));
  pui::endPanel();

  // ---- samples and the selected one's waveform ----
  const float rowH = 268;
  const float listW = std::clamp(std::floor(w * 0.36f), 280.0f, 330.0f);
  samplesPanel(a, t, ed, e, *inst, sound, listW, rowH);
  ImGui::SameLine(0, 8);
  waveformPanel(a, t, ed, e, *inst, sound, w - listW - 8, rowH);

  // ---- envelopes ----
  const float half = std::floor((w - 8) * 0.5f);
  envelopePanel(a, t, ed, e, false, half, 196);
  ImGui::SameLine(0, 8);
  envelopePanel(a, t, ed, e, true, w - half - 8, 196);

  // ---- the instrument and its MIDI settings (side by side where they fit), key map ----
  const float midiW = 420;
  if (w >= 1080) {
    instrumentPanel(a, t, ed, e, w - midiW - 8);
    ImGui::SameLine(0, 8);
    midiPanel(a, t, e, midiW);
  } else {
    instrumentPanel(a, t, ed, e, w);
    midiPanel(a, t, e, w);
  }
  keyMapPanel(a, t, ed, e, *inst, w);
  return true;
}

}  // namespace gl::packs::trackerpack
