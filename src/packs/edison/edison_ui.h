// Gridline — "edison" pack: editor internals shared by the edison_*_ui.cpp
// files. One Session per open editor (a mixer slot, or a standalone window on
// a channel's sample); it owns the undo history, view state, caches and the
// worker job that runs edits off the UI thread.
#pragma once
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../core/SmoothScroll.h"
#include "../../app/PluginSkin.h"
#include "../PacksUi.h"
#include "../../core/PianoRollScript.h"
#include "edison.h"

namespace gl::packs::edison {

// ------------------------------------------------------------------ palette (Edison roles)
namespace col {
inline constexpr uint32_t Panel = 0x3B4247, PanelHi = 0x454D53, PanelLo = 0x30363A;
inline constexpr uint32_t Btn = 0x4A5359, BtnHi = 0x58636A, BtnLo = 0x2C3236, Glyph = 0xD5DCE0, GlyphDim = 0x8E999F;
inline constexpr uint32_t Lit = 0xFF9637;       // lit glyphs / active toggles (orange role)
inline constexpr uint32_t Led = 0x7AE4A3;       // LED "on" (check boxes)
inline constexpr uint32_t Rec = 0xE5484D;
inline constexpr uint32_t WaveBg = 0x1C2124, WaveGrid = 0x2A3135, WaveGridHi = 0x39434A;
inline constexpr uint32_t Wave = 0x8FD3A0, WaveDim = 0x4E7E5C, WaveSel = 0xD3F5DB;
inline constexpr uint32_t SelBg = 0x34495A, SelEdge = 0x6F9BC2;
inline constexpr uint32_t RulerBg = 0x272D31, RulerText = 0xA7B3B9;
inline constexpr uint32_t Marker = 0xE8C46A, Region = 0x6A8FD6, Loop = 0xD9895A;
inline constexpr uint32_t Playhead = 0x3D63DD, Cursor = 0xF2F2F2;
inline constexpr uint32_t Text = 0xD6DDE1, TextDim = 0x8F9AA0, Lcd = 0x1A1F22, LcdText = 0xB8F0C6;
inline constexpr uint32_t Brush = 0xF0A040;
}  // namespace col

ImU32 C(uint32_t rgb, float a = 1.f);

// ------------------------------------------------------------------ tools (dialogs)
enum class Tool {
  None, Stretch, Equalize, Reverb, Blur, Denoise, Gate, Declick, Resample, Amplify, Normalize, Envelope, Slice, Tune, Silence,
  Spectral, Rename, Fade, Script
};

struct Job {
  std::future<std::function<void()>> fut;  // the worker returns the UI-thread completion
  std::string label;
  double started = 0;
  bool active() const { return fut.valid(); }
};

struct Session {
  uint64_t key = 0;
  // Owner: a mixer slot (gl.edison), or a channel's sample (standalone).
  bool slot = false;
  PluginTarget target;
  ObjectId channel = kNoId;  // standalone: the channel whose sample is edited
  bool open = true;          // standalone window shown
  std::string title;

  History hist;
  std::string loadedFrom;  // file the history was loaded from ("" = empty / generated)
  std::string committed;   // file the current version is saved in
  uint64_t committedSerial = 0;
  bool pendingCommit = false;
  std::string commitLabel;

  // view
  double viewStart = 0, viewLen = 0;  // frames
  smoothscroll::Axis viewAx;          // J-67: wheel scrolling glides (no elastic edges: frames outside the take)
  float vZoom = 1.f;
  Range sel;
  int64_t cursor = 0;
  int64_t anchor = -1;  // selection drag anchor
  bool dragging = false;
  int dragMarker = -1;  // marker being moved
  bool spectral = false;
  bool brushMode = false;
  int snap = 1;          // 0 none, 1 zero crossings, 2 markers, 3 beat grid
  bool barsTime = false; // ruler in bars:beats
  bool autoScroll = true;
  bool scrubMode = false;
  bool scrubbing = false;
  float brushHz = 0;     // brush height (octaves fraction) — set on first use
  std::vector<SpectralRect> brush;

  // caches
  PeakCache peaks;
  uint64_t peaksSerial = 0;
  Spectrogram spec;
  uint64_t specSerial = 0;
  std::future<Spectrogram> specJob;
  unsigned tex = 0;
  int texW = 0, texH = 0;
  bool texDirty = false;
  Levels levels;
  PitchInfo pitch;
  uint64_t levelsKey = ~0ull;
  std::future<std::pair<Levels, PitchInfo>> levelsJob;

  // playback (standalone: engine preview of a temporary sample)
  bool pvPlaying = false;
  double pvT0 = 0;
  int64_t pvFrom = 0, pvTo = 0;
  SamplePtr pvSample;

  // recording (slot)
  Recorder rec;
  size_t ringPos = 0;
  bool ringStarted = false;
  float inMeter = 0, outMeter = 0;

  NoiseProfile noise;
  Job job;
  std::string status;  // last tool message (hint bar line)
  double statusUntil = 0;

  // tool dialog state
  Tool tool = Tool::None;
  bool toolOpenReq = false;
  float tStretch = 1.f, tSemis = 0.f;
  int tMode = 0, tQuality = 1;
  std::vector<EqPoint> eq = {{30, 0}, {120, 0}, {500, 0}, {2000, 0}, {8000, 0}, {16000, 0}};
  ReverbSettings rv;
  float blurMs = 40, blurMix = 0.5f;
  float dnReduce = 24, dnSens = 0.5f;
  float gThr = -45, gAtt = 1, gHold = 30, gRel = 80, gFloor = -90;
  float dcSens = 0.5f;
  int rsRate = 48000, rsQuality = 2;
  float ampDb = 3, normDb = 0;
  std::vector<std::pair<double, double>> env = {{0, 1}, {1, 1}};
  float sliceSens = 0.5f;
  int tuneNote = -1;
  float silenceSec = 1.f;
  float spectralDb = -30;
  int fadeCurve = 0;
  int renameMarker = -1, renameRegion = -1;
  char renameBuf[64] = {};

  const Content& cur() const { return hist.cur(); }
  int64_t frames() const { return hist.cur().frames(); }
  double rate() const { return hist.cur().rate(); }
};

// Sessions (edison_ui.cpp).
Session& sessionFor(App& a, const PluginTarget& t);
EdisonFx* slotFx(App& a, const Session& s);
const PluginState* slotState(App& a, const Session& s);
double param(App& a, const Session& s, int k, double dflt);
void setParam(App& a, const Session& s, int k, double v);

// Edits: run `op` on the worker; its result becomes one undo step.
void runEdit(App& a, Session& s, const std::string& label, std::function<Content(const Content&)> op);
// Same, immediately on the UI thread (cheap edits: markers, loop points).
void applyNow(App& a, Session& s, const std::string& label, const Content& next);
void undo(App& a, Session& s);
void redo(App& a, Session& s);
void say(Session& s, const std::string& text);
Range target(const Session& s);  // selection or whole file
// Clipboard shared by every editor.
Audio& clipboard();

// Transport.
void play(App& a, Session& s);
void stopPlay(App& a, Session& s);
bool isPlaying(App& a, Session& s);
double playPos(App& a, Session& s);  // frames, -1 when stopped
void toggleRecord(App& a, Session& s);

// Actions shared by the toolbar and the menus.
void saveAs(App& a, Session& s, FileFormat f);
void exportSelection(App& a, Session& s);
void openFile(App& a, Session& s);
void sendToPlaylist(App& a, Session& s);
// J-191: the current version into the Split stems window (files in the Stems folder).
void splitStems(App& a, Session& s);
void sendToSampler(App& a, Session& s, bool slicer);
void detectSlices(App& a, Session& s, double sensitivity);
void addMarkerAtCursor(App& a, Session& s);

// Drawing (edison_form_ui.cpp / edison_wave_ui.cpp / edison_tools_ui.cpp).
void drawForm(App& a, Session& s, float width, float maxHeight);
void drawMenus(App& a, Session& s, const skin::Form& F, ImDrawList* dl);
void drawWaveArea(App& a, Session& s, ImVec2 p0, ImVec2 p1, float scale);
void drawToolDialogs(App& a, Session& s);
void openTool(Session& s, Tool t);
void handleKeys(App& a, Session& s);

// Audio editor scripts (edison_script_ui.cpp, SC-05 / ED-09).
const std::vector<script::ScriptInfo>& audioScripts(App& a, bool rescan = false);
void audioScriptMenuItems(App& a, Session& s);  // body of Tools > Scripts
void openAudioScript(App& a, Session& s, const std::string& path);
void drawScriptDialog(App& a, Session& s);      // inside the tool dialog

// View helpers.
void zoomAll(Session& s);
void zoomTo(Session& s, Range r);
void zoomBy(Session& s, double factor, double centreFrame);
std::string timeText(App& a, const Session& s, int64_t frame, bool precise = true);

}  // namespace gl::packs::edison
