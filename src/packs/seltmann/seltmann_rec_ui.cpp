// Gridline — "seltmann" pack: J-218 the recorder on the start screen. "Record" next to "Load
// audio..." opens it in place of the drop zone: pick an input (the device inputs Gridline
// knows, stereo pairs or single inputs) with a live meter, then Record, Pause / Resume, Finish
// and Clear, with a running time and the take drawn as it grows. "Load recording" writes the
// take as a WAV into the project's Recorded folder (unsaved projects: the config's) and loads
// it exactly like a loaded file: the same state change, analysis, chop and undo step.
//
// Capture: the engine's input tap (AudioEngine::inputTap, a lock-free ring the audio thread
// writes each block's device input into, whatever the transport does) is drained on the UI
// thread every frame by tick() (from packs::drawWindows, so a hidden editor keeps recording).
// One recorder at a time; the take lives only here until it is loaded, so an unloaded take
// never reaches the project file.
//
// Monitoring and a count-in are left out on purpose: the take is not part of the song (a mixer
// track's input monitor is the place to hear an input, and a laptop microphone next to its
// speakers would feed back), and Seltmann finds the tempo and bar 1 of whatever is recorded,
// so a take does not have to start on a bar.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "../../app/AudioRecordUi.h"
#include "../../app/BrowserUi.h"
#include "../../app/DevSignal.h"
#include "../../app/LiveWaveUi.h"
#include "../../app/PluginSkin.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../engine/Dsp.h"
#include "../edison/edison.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann {

using dsp::gainToDb;
using theme::pal;

namespace sui {

namespace {

enum RecState { RecIdle = 0, RecRolling, RecPaused, RecDone };
const char* const kStateNames[4] = {"idle", "recording", "paused", "finished"};

struct Session {
  bool open = false;
  PluginTarget target;
  int state = RecIdle;
  TakeBuffer take;
  int first = 0;          // input channel (0 = In 1)
  bool stereo = true;     // first and first + 1
  bool inputPicked = false;
  size_t ringPos = 0;     // next tap frame to read
  float meter[2] = {0, 0};   // shown level (decays)
  float hold[2] = {0, 0};    // peak hold
  double holdAt[2] = {0, 0};
  double clipAt = -100;      // the last full-scale sample (s, steady clock)
  std::string msg;
  bool msgErr = false;
  bool askBack = false;      // "discard this recording?"
  size_t lastWritten = 0;
  double lastMove = 0;       // when the tap last advanced (steady clock)
  double lastTick = 0;       // the previous tick (steady clock)
  int nextSecond = 1;        // dev signal seltmann-take-<n>
  std::string lastTrace;
  std::vector<std::pair<std::string, ImVec2>> btns;  // trace: the visible buttons
  std::vector<float> bufL, bufR;
  // K-145: the take drawn as it records (fed from `take`, from the first frame each append
  // changed); the fit shows it at ten seconds across while it fits, then the whole take.
  LiveWave live;
  LiveFit fit;
  size_t liveDirty = 0;  // first take frame not (or no longer) in `live`
};

Session& ses() {
  static Session s;
  return s;
}

double clockNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool testInput() {
  const char* f = std::getenv("GRIDLINE_TEST_INPUT");
  return f && *f;
}

// Device input channels the engine receives (0: no input device set up).
int inputChannels(const App& a) {
  if (a.audioInputDevice.empty() && !testInput()) return 0;
  return a.engine.inputChannels();
}

void setMsg(Session& s, const std::string& m, bool err) {
  s.msg = m;
  s.msgErr = err;
}

void closeSession(App& a) {
  Session& s = ses();
  a.engine.setInputTap(-1, true);
  s.open = false;
  s.state = RecIdle;
  s.askBack = false;
  s.take.reset(44100, true);
  s.msg.clear();
  devsig::forget("seltmann-rec-open");
}

// K-145: brings the live waveform up to the take (after appends, a resume's crossfade, a clear).
void syncLive(Session& s) {
  const size_t n = s.take.frames();
  if (s.live.rate() != s.take.rate() || s.live.channels() != (s.take.stereo() ? 2 : 1) || n < s.live.frames()) s.liveDirty = std::min(s.liveDirty, n);
  if (s.live.levels() == 0 || s.live.rate() != s.take.rate() || s.live.channels() != (s.take.stereo() ? 2 : 1)) {
    s.live.reset(s.take.rate(), s.take.stereo() ? 2 : 1);
    s.liveDirty = 0;
  }
  const size_t kept = s.live.truncate(std::min(s.liveDirty, s.live.frames()));
  if (n > kept) s.live.add(s.take.L().data() + kept, s.take.R().data() + kept, n - kept);
  s.liveDirty = SIZE_MAX;
}

void startTake(App& a, Session& s) {
  s.take.reset(a.engine.sampleRate(), s.stereo);
  s.liveDirty = 0;
  s.fit.begin(s.take.rate() * 10.0, s.take.rate());
  s.ringPos = a.engine.inputTap().written();  // from now on
  s.state = RecRolling;
  s.nextSecond = 1;
  s.msg.clear();
  for (int k = 1; k < 1000; ++k) devsig::forget("seltmann-take-" + std::to_string(k));
}

void loadTake(App& a, Session& s) {
  const int rb = static_cast<int>(a.settings.getNumber("audio.recordBits"));
  std::string err;
  const std::string path = s.take.save(recordedDir(a), rb == 24 || rb == 16 ? rb : 32, &err);
  if (path.empty()) {
    setMsg(s, std::string(i18n::tr("The recording could not be saved")) + (err.empty() ? "" : ": " + err), true);
    return;
  }
  if (std::getenv("GRIDLINE_SELTMANN_TRACE")) {
    std::printf("seltmann: rec loaded \"%s\" frames %zu rate %.0f channels %d\n", path.c_str(), s.take.frames(), s.take.rate(),
                s.take.stereo() ? 2 : 1);
    std::fflush(stdout);
  }
  const PluginTarget t = s.target;
  closeSession(a);
  loadAudio(a, t, path, i18n::tr("Recording"));
  browserui::refreshIndex(a);
}

// A recorder button: the editor's button, or a quiet disabled face.
bool recButton(Session& s, const char* trace, const char* id, ImVec2 p0, ImVec2 size, const char* text, int icon, bool primary, bool enabled,
               const char* hint) {
  s.btns.push_back({trace, ImVec2(std::round(p0.x + size.x * 0.5f), std::round(p0.y + size.y * 0.5f))});
  if (enabled) return button(id, p0, size, text, false, hint, icon, primary);
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, size);
  if (ImGui::IsItemHovered() && hint) ui::Tooltip(i18n::tr(hint));
  const ImVec2 p1(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(p0, p1, P.surface800, theme::kRadius);
  dl->AddRect(p0, p1, theme::withAlpha(P.line, 0.6f), theme::kRadius);
  const ImU32 fg = theme::withAlpha(P.textFaint, 0.7f);
  const char* str = i18n::tr(text);
  const float isz = icon >= 0 ? std::min(14.0f, size.y * 0.55f) : -5.0f;  // no icon: the text alone, centred
  const float tw = ImGui::GetFont()->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, str).x;
  const float x0 = (p0.x + p1.x) * 0.5f - (isz + 5 + tw) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
  if (icon >= 0) ui::drawIcon(dl, static_cast<ui::Icon>(icon), ImVec2(x0 + isz * 0.5f, cy), isz, fg);
  dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(std::round(x0 + isz + 5), std::round(cy - theme::kFontCaption * 0.62f)), fg, str);
  return false;
}

// Level meter: two bars (-60..0 dBFS), peak hold, a clip light.
void drawMeter(App& a, Session& s, ImVec2 p0, float w, float h) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const int rows = s.stereo ? 2 : 1;
  const float gap = 3, bh = std::floor((h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows));
  const float clipW = 10, bw = w - clipW - 6;
  auto xOf = [&](float g) {
    const double db = gainToDb(g);
    return static_cast<float>(std::clamp((db + 60.0) / 60.0, 0.0, 1.0)) * bw;
  };
  for (int r = 0; r < rows; ++r) {
    const ImVec2 b0(p0.x, p0.y + static_cast<float>(r) * (bh + gap)), b1(p0.x + bw, b0.y + bh);
    skin::insetField(dl, b0, b1, P.well, 2);
    const float x = xOf(s.meter[r]);
    const float x12 = xOf(0.25f), x3 = xOf(0.7f);  // -12 / -3 dBFS
    if (x > 0) dl->AddRectFilled(ImVec2(b0.x + 1, b0.y + 1), ImVec2(b0.x + std::min(x, x12), b1.y - 1), P.meterLow, 1);
    if (x > x12) dl->AddRectFilled(ImVec2(b0.x + x12, b0.y + 1), ImVec2(b0.x + std::min(x, x3), b1.y - 1), P.meterMid, 1);
    if (x > x3) dl->AddRectFilled(ImVec2(b0.x + x3, b0.y + 1), ImVec2(b0.x + x, b1.y - 1), P.meterHigh, 1);
    const float hx = xOf(s.hold[r]);
    if (hx > 1) dl->AddLine(ImVec2(b0.x + hx, b0.y + 1), ImVec2(b0.x + hx, b1.y - 1), P.text, 1);
  }
  const bool clip = clockNow() - s.clipAt < 1.5;
  const ImVec2 c0(p0.x + bw + 6, p0.y), c1(p0.x + w, p0.y + h);
  dl->AddRectFilled(c0, c1, clip ? P.danger : theme::withAlpha(P.danger, 0.18f), 2);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##selrecmeter", ImVec2(w, h));
  if (ImGui::IsItemHovered()) {
    char b[96];
    std::snprintf(b, sizeof b, "%s: %.1f dBFS", i18n::tr("Input level (peak)"), std::max(-60.0, gainToDb(std::max(s.hold[0], s.hold[1]))));
    ui::Tooltip(b);
  }
  (void)a;
}

// K-145 dev checks (GRIDLINE_SELTMANN_TRACE): the take's view each frame while it changes.
void traceTake(const Session& s, float width, double span, float headX) {
  static const bool on = std::getenv("GRIDLINE_SELTMANN_TRACE") != nullptr;
  if (!on) return;
  std::printf("seltmann: take %s frames %zu shown %.0f span %.0f head %.2f width %.2f gap %.3f fit %d live %d\n", kStateNames[s.state],
              s.live.frames(), s.fit.active ? s.fit.shown : static_cast<double>(s.live.frames()), span, static_cast<double>(headX),
              static_cast<double>(width), static_cast<double>(width - 8 - headX), span > s.fit.base + 0.5 ? 1 : 0, s.fit.active ? 1 : 0);
  std::fflush(stdout);
}

// The take as it grows: mirrored peaks (K-145: the shared live waveform). While recording or
// paused, ten seconds across while the take fits, then the whole take fitted to the width
// every frame; finished, the whole take in its normal colours.
void drawTake(App& a, Session& s, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::insetField(dl, p0, p1, P.well, theme::kRadius);
  const float cy = std::round((p0.y + p1.y) * 0.5f), half = (p1.y - p0.y) * 0.5f - 10;
  const float x0 = p0.x + 12, x1 = p1.x - 12, w = x1 - x0;
  const double rate = std::max(1.0, s.take.rate());
  const bool live = s.fit.active;
  const double frames = static_cast<double>(s.live.frames());
  const double spanFrames = live ? s.fit.span(w, 8) : std::max(rate * 10.0, frames);
  const double span = spanFrames / rate;
  // second ticks
  const double step = span <= 20 ? 1 : span <= 60 ? 5 : span <= 180 ? 15 : span <= 900 ? 60 : 300;
  for (double t = step; t < span; t += step) {
    const float x = std::round(x0 + static_cast<float>(t / span) * w) + 0.5f;
    dl->AddLine(ImVec2(x, p0.y + 6), ImVec2(x, p1.y - 6), theme::withAlpha(P.text, 0.05f), 1);
  }
  dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), theme::withAlpha(P.text, 0.10f), 1);
  const ImU32 col = s.state == RecRolling ? theme::mix(accent(), P.danger, 0.35f) : s.state == RecPaused ? theme::withAlpha(accent(), 0.6f) : accent();
  float headX = x0;
  if (s.live.frames() > 0) {
    LiveWaveView v;
    v.start = 0;
    v.span = spanFrames;
    v.end = live ? s.fit.shown : frames;
    v.pxScale = ImGui::GetIO().DisplayFramebufferScale.x;
    livewaveui::Style st;
    st.wave = col;
    st.channel = -2;  // mirrored: the louder channel
    st.fill = half / std::max(1.0f, p1.y - p0.y);
    headX = livewaveui::drawLane(dl, s.live, ImVec2(x0, p0.y), ImVec2(x1, p1.y), v, st);
  }
  if (s.state == RecRolling || s.state == RecPaused) {
    const ImU32 hc = s.state == RecRolling ? P.danger : theme::withAlpha(P.textDim, 0.8f);
    dl->AddLine(ImVec2(headX, p0.y + 6), ImVec2(headX, p1.y - 6), hc, 2);
  }
  traceTake(s, w, spanFrames, headX - x0);
  if (s.take.frames() == 0) {
    const char* hint = s.state == RecRolling ? i18n::tr("Listening...")
                                             : i18n::tr("Press Record and play. When you finish, load the recording and Seltmann analyses it like any file.");
    textC(dl, a.fontRegular, theme::kFontBody, ImVec2((p0.x + p1.x) * 0.5f, cy), P.textDim, hint);
  }
}

void traceRec(Session& s, const char* screen) {
  static const bool on = std::getenv("GRIDLINE_SELTMANN_TRACE") != nullptr;
  if (!on) return;
  char b[256];
  std::snprintf(b, sizeof b, "seltmann: rec %s state %s input %d %s ask %d msg \"%s\"", screen, kStateNames[s.state], s.first + 1,
                s.stereo ? "stereo" : "mono", s.askBack ? 1 : 0, s.msg.c_str());
  std::string line = b;
  for (const auto& [n, p] : s.btns) {
    std::snprintf(b, sizeof b, "\nseltmann: recbtn %s %.0f %.0f", n.c_str(), static_cast<double>(p.x), static_cast<double>(p.y));
    line += b;
  }
  if (line != s.lastTrace) {
    s.lastTrace = line;
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  }
}

}  // namespace

// ------------------------------------------------------------------ entry points
bool recorderOpenFor(const PluginTarget& t) {
  const Session& s = ses();
  return s.open && s.target.key() == t.key();
}

void openRecorder(App& a, const PluginTarget& t) {
  Session& s = ses();
  if (s.open && s.target.key() != t.key() && (s.state != RecIdle || s.take.frames() > 0)) {
    a.notify(i18n::tr("Another Seltmann is recording. Finish or clear that recording first."));
    return;
  }
  s.open = true;
  s.target = t;
  s.state = RecIdle;
  s.askBack = false;
  s.msg.clear();
  s.take.reset(a.engine.sampleRate(), s.stereo);
  const int ch = inputChannels(a);
  if (!s.inputPicked || s.first >= std::max(ch, 2)) {
    s.first = 0;
    s.stereo = ch != 1;
  }
  a.engine.setInputTap(s.first, s.stereo);
  s.ringPos = a.engine.inputTap().written();
  s.lastWritten = s.ringPos;
  s.lastMove = clockNow();
  s.meter[0] = s.meter[1] = s.hold[0] = s.hold[1] = 0;
  devsig::reach("seltmann-rec-open");
}

void devAutoRecord(App& a, const PluginTarget& t) {
  static const char* v = std::getenv("GRIDLINE_SELTMANN_AUTOREC");
  if (!v || !*v) return;
  static double pauseAt = 0, resumeAfter = 0, finishAt = 0, pausedAt = 0;
  static int stage = -1;
  if (stage < 0) {
    if (std::sscanf(v, "%lf %lf %lf", &pauseAt, &resumeAfter, &finishAt) != 3) return;
    stage = 0;
  }
  Session& s = ses();
  if (stage == 0 && ImGui::GetFrameCount() > 20) {
    openRecorder(a, t);
    startTake(a, s);
    stage = 1;
  } else if (stage == 1 && pauseAt > 0 && s.take.seconds() >= pauseAt) {
    s.state = RecPaused;
    pausedAt = clockNow();
    stage = 2;
  } else if (stage == 2 && clockNow() - pausedAt >= resumeAfter) {  // as Resume does
    s.take.join();
    s.ringPos = a.engine.inputTap().written();
    s.lastMove = clockNow();
    s.state = RecRolling;
    stage = 3;
  } else if ((stage == 1 || stage == 3) && s.state == RecRolling && s.take.seconds() >= finishAt) {
    s.state = RecDone;
    stage = 4;
    devsig::reach("seltmann-autorec-done");
  }
}

void tick(App& a) {
  Session& s = ses();
  if (!s.open) return;
  const PluginState* st = s.target.state(a.doc.song());
  if (!st || st->type != kType) {  // the instrument went away
    closeSession(a);
    return;
  }
  if (!st->data.empty()) {  // a file was loaded into it meanwhile
    if (s.take.frames() > 0) a.notify(i18n::tr("The unloaded recording was discarded: a file was loaded into Seltmann."));
    closeSession(a);
    return;
  }
  const double now = clockNow();
  const auto& ring = a.engine.inputTap();
  // drain the tap (the meter always; the take while recording)
  if (s.bufL.size() < 4096) {
    s.bufL.resize(4096);
    s.bufR.resize(4096);
  }
  float pk[2] = {0, 0};
  bool lost = false;
  for (int guard = 0; guard < 4096; ++guard) {
    size_t from = s.ringPos;
    const size_t n = ring.read(from, s.bufL.size(), s.bufL.data(), s.bufR.data(), nullptr);
    if (from - n != s.ringPos) lost = true;  // the ring overtook the reader
    s.ringPos = from;
    if (n == 0) break;
    for (size_t i = 0; i < n; ++i) {
      pk[0] = std::max(pk[0], std::fabs(s.bufL[i]));
      pk[1] = std::max(pk[1], std::fabs(s.bufR[i]));
    }
    if (s.state == RecRolling) {
      if (lost && s.take.frames() > 0) s.take.join();
      s.take.append(s.bufL.data(), s.bufR.data(), n);
      s.liveDirty = std::min(s.liveDirty, s.take.changedFrom());
    }
  }
  if (lost && s.state == RecRolling) setMsg(s, i18n::tr("Some input was lost while Gridline was busy; the gap is crossfaded."), true);
  // meter: instant attack, ~20 dB/s fall, a 1.5 s hold
  const double dt = std::clamp(now - s.lastTick, 0.0, 0.25);
  s.lastTick = now;
  for (int c = 0; c < 2; ++c) {
    const float v = s.stereo ? pk[c] : pk[0];
    s.meter[c] = std::max(v, s.meter[c] * static_cast<float>(std::pow(10.0, -1.0 * std::min(dt, 0.1))));
    if (v >= s.hold[c] || now - s.holdAt[c] > 1.5) {
      s.hold[c] = v;
      s.holdAt[c] = now;
    }
    if (v >= 0.999f) s.clipAt = now;
  }
  // device problems while recording
  const size_t w = ring.written();
  if (w != s.lastWritten) {
    s.lastWritten = w;
    s.lastMove = now;
  }
  if (s.state == RecRolling || s.state == RecPaused) {
    if (std::fabs(a.engine.sampleRate() - s.take.rate()) > 0.5) {
      s.state = s.take.frames() > 0 ? RecDone : RecIdle;
      setMsg(s, i18n::tr("The sample rate changed, so the recording stopped. What was recorded so far is kept."), true);
    } else if (inputChannels(a) == 0 && s.state == RecRolling) {
      s.state = RecPaused;
      setMsg(s, i18n::tr("The input device is gone. Recording paused: choose an input in Settings > Audio, then resume."), true);
    } else if (s.state == RecRolling && now - s.lastMove > 1.5) {
      s.state = RecPaused;
      setMsg(s, i18n::tr("No audio is arriving from the audio device. Recording paused."), true);
    }
  }
  if (s.state == RecRolling && s.take.full()) {
    s.state = RecDone;
    setMsg(s, i18n::tr("The recording reached its ten-minute limit and stopped."), false);
  }
  // K-145: the live waveform follows the take; the fit runs while a take is recording or paused
  syncLive(s);
  if (s.state == RecRolling || s.state == RecPaused) {
    if (!s.fit.active) s.fit.begin(s.take.rate() * 10.0, s.take.rate(), static_cast<double>(s.take.frames()));
    s.fit.step(static_cast<double>(s.live.frames()), dt);
  } else {
    s.fit.end();
  }
  // scripted runs: whole seconds of the take
  while (s.state == RecRolling && s.take.seconds() >= s.nextSecond) devsig::reach("seltmann-take-" + std::to_string(s.nextSecond++));
}

void drawRecorder(App& a, const PluginTarget& t, Ed& e, ImVec2 p0, ImVec2 p1) {
  (void)t;
  (void)e;
  Session& s = ses();
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  s.btns.clear();
  const int ch = inputChannels(a);
  const char* caption = s.state == RecRolling ? i18n::tr("RECORDING") : s.state == RecPaused ? i18n::tr("PAUSED") : s.state == RecDone ? i18n::tr("FINISHED")
                                                                                                                                        : i18n::tr("READY");
  const float strip = 30;
  panel(dl, a, p0, p1, i18n::tr("RECORD FROM AN INPUT"), caption, strip);
  if (s.state == RecRolling) {  // a red light before the caption
    const float tw = (a.fontMonoBold ? a.fontMonoBold : a.fontMono)->CalcTextSizeA(10.5f, 1e9f, 0, caption).x;
    const float pulse = 0.55f + 0.45f * static_cast<float>(std::fabs(std::sin(a.now * 3.0)));
    dl->AddCircleFilled(ImVec2(p1.x - 12 - tw - 10, std::round(p0.y + strip * 0.5f)), 4, theme::withAlpha(P.danger, pulse), 12);
  }
  const float L = p0.x + 16, R = p1.x - 16;
  const float ctl = 28;
  // ---- row 1: the input, its meter; Back on the right
  float y = p0.y + strip + 14;
  float x = L;
  const bool idle = s.state == RecIdle;
  x += inlineLabel(dl, a, ImVec2(x, y), ctl, i18n::N_("INPUT"));
  if (ch > 0) {
    const std::string cur = inputName(s.first, s.stereo);
    const bool openIn = recButton(s, "input", "##selrecin", ImVec2(x, y), ImVec2(150, ctl), cur.c_str(), -1, false, idle,
                                  idle ? i18n::N_("The device input to record (stereo pairs or single inputs)")
                                       : i18n::N_("The input is fixed while a recording exists (Clear to change it)"));
    ui::drawIcon(dl, ui::Icon::ChevronDown, ImVec2(x + 150 - 14, y + ctl * 0.5f), 10, idle ? P.textDim : theme::withAlpha(P.textFaint, 0.6f));
    if (openIn) ImGui::OpenPopup("##selrecinpop");
    if (ImGui::BeginPopup("##selrecinpop")) {
      const int list = std::max(ch, 1);
      auto pick = [&](int first, bool stereo) {
        s.first = first;
        s.stereo = stereo;
        s.inputPicked = true;
        s.take.reset(a.engine.sampleRate(), stereo);
        a.engine.setInputTap(first, stereo);
        s.meter[0] = s.meter[1] = s.hold[0] = s.hold[1] = 0;
      };
      for (int c = 0; c + 1 < list; c += 2)
        if (ImGui::Selectable(inputName(c, true).c_str(), s.first == c && s.stereo)) pick(c, true);
      for (int c = 0; c < list; ++c)
        if (ImGui::Selectable(inputName(c, false).c_str(), s.first == c && !s.stereo)) pick(c, false);
      ImGui::EndPopup();
    }
    x += 150 + 12;
    drawMeter(a, s, ImVec2(x, y + 4), 240, ctl - 8);
    x += 240 + 10;
    char b[48];
    std::snprintf(b, sizeof b, "%.1f dB", std::max(-60.0, gainToDb(std::max(s.hold[0], s.hold[1]))));
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x, std::round(y + (ctl - theme::kFontLabel) * 0.5f - 1)), P.textDim, b);
  } else {
    // no input device: say so and lead to the audio settings
    dl->AddCircleFilled(ImVec2(x + 6, y + ctl * 0.5f), 4, P.meterHigh, 12);
    const char* m = i18n::tr("No audio input is set up. Choose an input device in the audio settings to record.");
    dl->AddText(a.fontRegular, theme::kFontBody, ImVec2(x + 16, std::round(y + (ctl - theme::kFontBody) * 0.5f - 1)), P.text, m);
    x += 16 + a.fontRegular->CalcTextSizeA(theme::kFontBody, 1e9f, 0, m).x + 12;
    if (recButton(s, "settings", "##selrecset", ImVec2(x, y), ImVec2(140, ctl), i18n::N_("Audio settings..."), static_cast<int>(ui::Icon::Settings),
                  true, true, i18n::N_("Open Settings > Audio to choose the input device"))) {
      a.showSettings = true;
      a.settingsTab = 0;
      a.focusWindow = "Settings";
    }
  }
  bool back = recButton(s, "back", "##selrecback", ImVec2(R - 96, y), ImVec2(96, ctl), i18n::N_("Back"), static_cast<int>(ui::Icon::Left), false,
                        !s.askBack, i18n::N_("Back to the start screen (an unloaded recording is discarded)"));
  // ---- the take
  const float waveTop = y + ctl + 14, waveBot = p1.y - 16 - 40 - 16 - 30;
  drawTake(a, s, ImVec2(L, waveTop), ImVec2(R, waveBot));
  // ---- time and message
  y = waveBot + 8;
  {
    ImFont* f = a.fontMonoBold ? a.fontMonoBold : a.fontBold;
    const std::string tm = fmtTime(s.take.seconds());
    dl->AddText(f, 20, ImVec2(L, y), s.state == RecRolling ? P.text : P.textDim, tm.c_str());
    const float tw = f->CalcTextSizeA(20, 1e9f, 0, tm.c_str()).x;
    char b[96];
    std::snprintf(b, sizeof b, "%s %.0f Hz  \xC2\xB7  %s", i18n::tr("of 10:00 at"), s.take.rate(), s.stereo ? i18n::tr("stereo") : i18n::tr("mono"));
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(L + tw + 10, y + 7), P.textFaint, b);
    if (!s.msg.empty())
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(L + tw + 230, y + 5), R - (L + tw + 230), s.msgErr ? P.meterHigh : P.textDim,
                          s.msg.c_str());
  }
  // ---- the transport row
  y = p1.y - 16 - 40;
  const ImVec2 bs(132, 40);
  if (s.askBack) {
    const char* q = i18n::tr("Discard this recording?");
    dl->AddText(a.fontBold, theme::kFontBody, ImVec2(L, std::round(y + (40 - theme::kFontBody) * 0.5f - 1)), P.text, q);
    const float qx = L + a.fontBold->CalcTextSizeA(theme::kFontBody, 1e9f, 0, q).x + 16;
    if (recButton(s, "discard", "##selrecdiscard", ImVec2(qx, y), bs, i18n::N_("Discard"), static_cast<int>(ui::Icon::Trash), false, true,
                  i18n::N_("Throw the recording away and go back"))) {
      back = true;
      s.take.reset(s.take.rate(), s.stereo);
    }
    if (recButton(s, "keep", "##selreckeep", ImVec2(qx + bs.x + 8, y), bs, i18n::N_("Keep"), -1, true, true, i18n::N_("Stay with the recording")))
      s.askBack = false;
  } else {
    float bx = L;
    const bool canRec = ch > 0;
    if (s.state == RecIdle) {
      if (recButton(s, "record", "##selrecgo", ImVec2(bx, y), bs, i18n::N_("Record"), static_cast<int>(ui::Icon::Record), true, canRec,
                    i18n::N_("Start recording the input")))
        startTake(a, s);
    } else if (s.state == RecRolling) {
      if (recButton(s, "record", "##selrecgo", ImVec2(bx, y), bs, i18n::N_("Pause"), static_cast<int>(ui::Icon::Pause), true, true,
                    i18n::N_("Pause; Resume carries on in the same recording")))
        s.state = RecPaused;
    } else if (s.state == RecPaused) {
      if (recButton(s, "record", "##selrecgo", ImVec2(bx, y), bs, i18n::N_("Resume"), static_cast<int>(ui::Icon::Record), true, canRec,
                    i18n::N_("Carry on recording at the end of the take"))) {
        s.take.join();
        s.ringPos = a.engine.inputTap().written();
        s.lastMove = clockNow();
        s.state = RecRolling;
        s.msg.clear();
      }
    } else {
      if (recButton(s, "record", "##selrecgo", ImVec2(bx, y), bs, i18n::N_("Record again"), static_cast<int>(ui::Icon::Record), false, canRec,
                    i18n::N_("Discard this recording and record a new one")))
        startTake(a, s);
    }
    bx += bs.x + 8;
    const bool live = s.state == RecRolling || s.state == RecPaused;
    if (recButton(s, "finish", "##selrecfin", ImVec2(bx, y), bs, i18n::N_("Finish"), static_cast<int>(ui::Icon::Stop), false,
                  live && s.take.frames() > 0, i18n::N_("Stop recording; then load the recording into Seltmann")))
      s.state = RecDone;
    bx += bs.x + 8;
    if (recButton(s, "clear", "##selrecclr", ImVec2(bx, y), bs, i18n::N_("Clear"), static_cast<int>(ui::Icon::Trash), false,
                  s.state != RecIdle || s.take.frames() > 0, i18n::N_("Discard the recording and start again"))) {
      s.take.reset(a.engine.sampleRate(), s.stereo);
      s.state = RecIdle;
      s.msg.clear();
    }
    const ImVec2 ls(176, 40);
    if (recButton(s, "load", "##selrecload", ImVec2(R - ls.x, y), ls, i18n::N_("Load recording"), static_cast<int>(ui::Icon::Open), true,
                  s.state == RecDone && s.take.frames() > 0, i18n::N_("Save the recording into the project's Recorded folder and analyse it"))) {
      traceRec(s, "open");
      loadTake(a, s);
      return;
    }
  }
  if (back) {
    if (s.take.frames() > 0 && !s.askBack) s.askBack = true;
    else {
      traceRec(s, "open");
      closeSession(a);
      return;
    }
  }
  traceRec(s, "open");
}

}  // namespace sui

void tick(App& a) { sui::tick(a); }

}  // namespace gl::packs::seltmann
