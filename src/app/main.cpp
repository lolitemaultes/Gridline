// Gridline — application entry: SDL2 window + OpenGL 3 + Dear ImGui, audio
// device, default layout, typing keyboard, file drops.
#include "FreezeUi.h"
#include "CacheUi.h"
#include "DrumKitUi.h"
#include "Look.h"
#include "MidiUi.h"
#include "SettingsPages.h"
#include "GridlineVersion.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include "SysWm.h"
#include "VideoDriver.h"
#include "MainWindowRole.h"
#include "../core/VideoDriverChoice.h"
#include "../core/ChromeOs.h"
#if defined(_WIN32)
#include <shellapi.h>  // CommandLineToArgvW
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#if defined(__GLIBC__)
#include <execinfo.h>
#include <pthread.h>
#include <csignal>
#include <exception>
#endif
#include "../engine/ChannelIns.h"
#include "../engine/Sample.h"
#include "../engine/TimeStretch.h"
#include "DevSignal.h"
#include "FlpExportUi.h"
#include "GrooveUi.h"
#include "J59Ui.h"
#include "FrameProfile.h"
#include "FrameStats.h"
#include "McpUi.h"
#include "ThemeSettings.h"
#include "DisplayConfirmUi.h"
#include "LiveResize.h"
#include "LiveTakes.h"
#include "Viewports.h"
#include "../core/I18n.h"
#include "../core/UiScale.h"
#include "AlignAudit.h"
#include "AppIcon.h"
#include "WindowFrame.h"
#include "HostedUi.h"
#include "../host/HostedPlugins.h"
#include "../host/audio/AsioHost.h"
#include "../host/PluginPrefs.h"
#include "../host/vst3/Vst3Scan.h"
#include "../host/ui/NativeView.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_impl_sdl2.h"
#include "../../third_party/imgui/imgui_internal.h"
#include <cfloat>
#include <climits>
#include <stdexcept>
#include "../core/ClipSources.h"
#include "../core/NoteBend.h"
#include "../core/TrackModes.h"
#include "../core/Project.h"
#include "../core/Workspace.h"
#include "../packs/PacksUi.h"
#include "DialogLayout.h"
#include "ExportUi.h"
#include "GlEpoch.h"
#include "GlRecovery.h"
#include <fstream>
#include "../core/Log.h"
#include "DevStress.h"
#include "StemsUi.h"
#include "TrackReceiptUi.h"
#include "../core/MidiFile.h"
#include "Keys.h"
#include "App.h"
#include "../core/RemoteFs.h"
#include "../core/SingleInstance.h"
#include "WindowActivate.h"
#include "ForeignGl.h"
#include "CollabUi.h"
#include "PluginUI.h"
#include "EditorZoomUi.h"
#include "OpenDocuments.h"
#include "../core/EditorZoom.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "CommandPaletteUi.h"
#include "ContextMenus.h"
#include "EditTools.h"
#include "J31Ui.h"
#include "MenuParityUi.h"
#include "ScriptUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "TrackModeUi.h"
#include "TrackPlusUi.h"
#include "ChopUi.h"
#include "CrashUi.h"
#include "../core/Crash.h"
#include "../core/Settings.h"
#include "Shell.h"
#include "LayoutPresets.h"
#include "SmoothScrollUi.h"
#include "ExternalDragUi.h"
#include "SuiteUi.h"
#include "LauncherUi.h"
#include "ProjectLoadUi.h"
#include "UpdateUi.h"
#include "UrlImportUi.h"
#include "../host/net/Http.h"

namespace fs = std::filesystem;
using namespace gl;
using namespace gl::shell;

namespace gl {
void drawSplash(ImFont* bold, ImFont* mono, float progress, const char* status, double time);
}

namespace {

SDL_AudioDeviceID gDevice = 0;
SDL_AudioDeviceID gInDevice = 0;  // capture (J-8)
uisound::Player* gUiSounds = nullptr;  // J-48: interface sounds, mixed after the master

void initMidi(App& a) {
  a.midi = std::make_unique<MidiManager>(a.engine);
  a.midi->load((fs::path(a.configDir()) / "midi.json").string());
  // The system MIDI service is asked on a background thread: it can hang or refuse, and then
  // Gridline starts without MIDI devices and keeps trying (Settings > MIDI says so). A working
  // service answers well within this wait, so its devices are open before the first frame.
  a.midi->scan();  // new devices are enabled with the next free port number (opened on the answer)
  a.midi->waitForService(0.5);
  a.loadControllerTemplates();
  a.publishMidi();
}

// J-217 dev checks: GRIDLINE_SYNTH_INPUT=1 replaces the capture device with a
// generated stereo input (never a microphone), pushed from the output callback
// so it runs in real time with the engine: a square wave (period 100 frames)
// whose amplitude alternates every second between 0.2 and 0.7.
std::atomic<bool> gSynthInput{false};
void pushSynthInput(AudioEngine& eng, int frames) {
  static int64_t pos = 0;
  static float buf[2 * 1024];
  const double sr = eng.sampleRate() > 0 ? eng.sampleRate() : 48000.0;
  for (int done = 0; done < frames;) {
    const int n = std::min(frames - done, 1024);
    for (int i = 0; i < n; ++i, ++pos) {
      const float amp = (static_cast<int64_t>(static_cast<double>(pos) / sr) % 2) == 0 ? 0.2f : 0.7f;
      const float v = (pos % 100) < 50 ? amp : -amp;
      buf[2 * i] = v, buf[2 * i + 1] = v;
    }
    eng.pushInput(buf, n);
    done += n;
  }
}

// J-218: development / tests: GRIDLINE_TEST_INPUT=<audio file> stands in for an input device
// (never a microphone): the file loops into the engine's device input as stereo In 1 - In 2,
// paced by the output callback. Only when no input device is chosen.
struct TestInput {
  std::vector<float> L, R;
  size_t pos = 0;
  float chunk[2 * 1024] = {};
};
std::atomic<TestInput*> gTestInput{nullptr};

void feedTestInput(AudioEngine* eng, int frames) {
  TestInput* ti = gTestInput.load(std::memory_order_acquire);
  if (!ti || ti->L.empty()) return;
  while (frames > 0) {
    const int k = std::min(frames, 1024);
    for (int i = 0; i < k; ++i) {
      ti->chunk[2 * i] = ti->L[ti->pos];
      ti->chunk[2 * i + 1] = ti->R[ti->pos];
      if (++ti->pos >= ti->L.size()) ti->pos = 0;
    }
    eng->pushInput(ti->chunk, k);
    frames -= k;
  }
}

// K-10 dev capture, --video-clock <fps>: no audio device; every frame advances the engine by exactly one
// video frame of samples on this thread, and the interface by 1/fps, so footage and meters are frame exact
// however long a frame takes to draw. With --video-out <file or fifo> each frame goes out as raw RGB
// (top row first); --video-cursor draws a pointer, which an off-screen frame otherwise lacks.
int gVideoFps = 0;
long long gVideoFrames = 0;
long long gVideoRemainder = 0;
void videoClockStep(AudioEngine& eng) {
  static float block[2 * 256];
  gVideoRemainder += static_cast<long long>(eng.sampleRate());
  long long samples = gVideoRemainder / gVideoFps;
  gVideoRemainder %= gVideoFps;
  while (samples > 0) {
    const int k = static_cast<int>(std::min<long long>(samples, 256));
    eng.processInterleaved(block, k);
    samples -= k;
  }
  ++gVideoFrames;
}

void audioCallback(void* user, Uint8* stream, int len) {
  auto* eng = static_cast<AudioEngine*>(user);
  crash::noteAudioThread();  // K-39: which thread a report comes from (a relaxed store)
  if (crash::audioCrashRequested()) crash::crashNowForTest();
  const int frames = len / static_cast<int>(sizeof(float) * 2);
  const bool stats = fstats::enabled();
  const Uint64 t0 = stats ? SDL_GetPerformanceCounter() : 0;
  if (gSynthInput) pushSynthInput(*eng, frames);
  else feedTestInput(eng, frames);
  eng->processInterleaved(reinterpret_cast<float*>(stream), frames);
  // Interface sounds go on top of the finished output: outside the song mixer,
  // so they are never recorded, rendered or metered.
  if (gUiSounds) gUiSounds->mix(reinterpret_cast<float*>(stream), frames);
  if (stats && eng->sampleRate() > 0)  // J-66 --frame-stats: blocks that missed their deadline
    fstats::audioBlock(static_cast<double>(SDL_GetPerformanceCounter() - t0) / static_cast<double>(SDL_GetPerformanceFrequency()),
                       static_cast<double>(frames) / eng->sampleRate());
}

void inputCallback(void* user, Uint8* stream, int len) {
  auto* eng = static_cast<AudioEngine*>(user);
  const int ch = std::max(1, eng->inputChannels());
  eng->pushInput(reinterpret_cast<const float*>(stream), len / static_cast<int>(sizeof(float) * static_cast<size_t>(ch)));
}

// Capture device for mixer inputs (J-8). Opened only when an input device is
// chosen (Settings → Audio), at the output's rate.
void openAudioInput(App& a, int rate, int outFrames) {
  if (gInDevice) {
    SDL_CloseAudioDevice(gInDevice);
    gInDevice = 0;
  }
  a.audioInputDevices.clear();
  for (int i = 0, n = SDL_GetNumAudioDevices(1); i < n; ++i)
    if (const char* nm = SDL_GetAudioDeviceName(i, 1)) a.audioInputDevices.push_back(nm);
  a.audioInputLatency = 0;
  a.audioInputStatus.clear();
  gSynthInput = false;
  if (const char* synth = std::getenv("GRIDLINE_SYNTH_INPUT"); synth && *synth && *synth != '0') {  // J-217 dev checks
    a.audioInputDevice = "Synthetic input";
    a.engine.setInputChannels(2);
    a.audioInputChannels = 2;
    a.audioInputLatency = outFrames;
    a.audioInputStatus = "Recording from the synthetic input (development).";
    gSynthInput = true;
    return;
  }
  if (a.audioInputDevice.empty()) {
    a.engine.setInputChannels(0);
    if (const char* f = std::getenv("GRIDLINE_TEST_INPUT"); f && *f) {  // J-218 (see TestInput)
      static TestInput ti;
      gTestInput.store(nullptr, std::memory_order_release);
      ti.L.clear();
      ti.R.clear();
      ti.pos = 0;
      if (SamplePtr s = loadSample(f); s && s->frames() > 1 && rate > 0) {  // at the device's rate (linear)
        const std::vector<float> sr = s->right.vec();
        const double step = s->sampleRate / rate;
        const auto n = static_cast<size_t>(static_cast<double>(s->frames() - 1) / step);
        ti.L.resize(n);
        ti.R.resize(n);
        for (size_t i = 0; i < n; ++i) {
          const double x = static_cast<double>(i) * step;
          const auto k = static_cast<size_t>(x);
          const auto u = static_cast<float>(x - static_cast<double>(k));
          ti.L[i] = s->left[k] + (s->left[k + 1] - s->left[k]) * u;
          ti.R[i] = sr[k] + (sr[k + 1] - sr[k]) * u;
        }
      }
      if (!ti.L.empty()) {
        a.engine.setInputChannels(2);
        a.audioInputChannels = 2;
        a.audioInputStatus = "Recording from the test signal (GRIDLINE_TEST_INPUT).";
        gTestInput.store(&ti, std::memory_order_release);
      }
    }
    return;
  }
  gTestInput.store(nullptr, std::memory_order_release);
  const int want = std::clamp(static_cast<int>(a.settings.getNumber("audio.inputChannels")), 1, kMaxInputChannels);
  a.engine.setInputChannels(want);  // the callback's frame size; fixed while the device runs
  SDL_AudioSpec spec{}, have{};
  spec.freq = rate;
  spec.format = AUDIO_F32SYS;
  spec.channels = static_cast<Uint8>(want);
  spec.samples = static_cast<Uint16>(std::max(64, outFrames));
  spec.callback = inputCallback;
  spec.userdata = &a.engine;
  const char* name = a.audioInputDevice == "*" ? nullptr : a.audioInputDevice.c_str();
  gInDevice = SDL_OpenAudioDevice(name, 1, &spec, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  char buf[256];
  if (!gInDevice) {
    a.engine.setInputChannels(0);
    std::snprintf(buf, sizeof buf, "No audio input: %s", SDL_GetError());
    a.audioInputStatus = buf;
    a.notify(a.audioInputStatus, true);
    return;
  }
  a.engine.setInputPrefill(have.samples);
  a.audioInputChannels = have.channels;
  // Capture buffer + jitter buffer, plus the output buffer the performer hears through.
  a.audioInputLatency = 2 * have.samples + outFrames;
  std::snprintf(buf, sizeof buf, "Recording from %s: %d channel%s, %d-frame buffer.", name ? name : "the default input", have.channels,
                have.channels == 1 ? "" : "s", have.samples);
  a.audioInputStatus = buf;
  SDL_PauseAudioDevice(gInDevice, 0);
}

// ---- K-6: ASIO backend (Windows; the simulated drivers with GRIDLINE_FAKE_ASIO) ----
// The driver's thread calls asioRender for every buffer; everything it uses is sized when the
// driver opens (UI thread).
asio::Host gAsio;
struct AsioRender {
  AudioEngine* eng = nullptr;
  std::vector<float> inter;  // interleaved device input (frames * inputs)
  std::vector<float> mix;    // interleaved main pair (interface sounds)
  std::vector<float> monoR;  // right side of a one-output device
  int extraCount = 0;
  int extraMixer[asio::kMaxExtraOutputs] = {};
  int extraOut[asio::kMaxExtraOutputs] = {};  // index of the pair's left channel in out[]
  bool mono = false;
};
AsioRender gAsioRender;
bool gAsioActive = false;        // the engine runs on the ASIO driver
bool gAsioSkip = false;          // the next openAudio uses the system audio (the driver stopped)
bool gAsioFollowDriver = false;  // the driver asked for a reset: take its new preferred buffer
std::string gSdlInputDevice;     // the system input choice, kept while ASIO runs
int gAsioPreferred = 0;          // the driver's preferred buffer when it was opened
std::string gAsioPreferredOf;

void asioRender(void* user, const float* const* in, int nIn, float* const* out, int nOut, int frames) {
  auto* r = static_cast<AsioRender*>(user);
  AudioEngine& eng = *r->eng;
  const bool stats = fstats::enabled();
  const Uint64 t0 = stats ? SDL_GetPerformanceCounter() : 0;
  if (nIn > 0) {
    float* x = r->inter.data();
    for (int c = 0; c < nIn; ++c) {
      const float* src = in[c];
      for (int i = 0; i < frames; ++i) x[static_cast<size_t>(i) * static_cast<size_t>(nIn) + static_cast<size_t>(c)] = src[i];
    }
    eng.pushInput(x, frames);
  } else if (gSynthInput) {
    pushSynthInput(eng, frames);
  } else {
    feedTestInput(&eng, frames);
  }
  if (nOut <= 0) return;
  float* L = out[0];
  float* R = r->mono ? r->monoR.data() : out[1];
  for (int done = 0; done < frames;) {
    const int n = std::min(AudioEngine::kMaxBlock, frames - done);
    float* tl[asio::kMaxExtraOutputs];
    float* tr[asio::kMaxExtraOutputs];
    for (int k = 0; k < r->extraCount; ++k) {
      tl[k] = out[r->extraOut[k]] + done;
      tr[k] = out[r->extraOut[k] + 1] + done;
    }
    AudioEngine::OutputTaps taps;
    taps.count = r->extraCount;
    taps.mixer = r->extraMixer;
    taps.L = tl;
    taps.R = tr;
    eng.process(L + done, R + done, n, r->extraCount > 0 ? &taps : nullptr);
    done += n;
  }
  if (gUiSounds) {  // interface sounds on top of the main pair only (never recorded or metered)
    float* m = r->mix.data();
    for (int i = 0; i < frames; ++i) m[2 * i] = L[i], m[2 * i + 1] = R[i];
    gUiSounds->mix(m, frames);
    for (int i = 0; i < frames; ++i) L[i] = m[2 * i], R[i] = m[2 * i + 1];
  }
  if (r->mono)
    for (int i = 0; i < frames; ++i) L[i] = 0.5f * (L[i] + R[i]);
  if (stats && eng.sampleRate() > 0)
    fstats::audioBlock(static_cast<double>(SDL_GetPerformanceCounter() - t0) / static_cast<double>(SDL_GetPerformanceFrequency()),
                       static_cast<double>(frames) / eng.sampleRate());
}

void* nativeWindowHandle() {
#ifdef _WIN32
  if (SDL_Window* w = SDL_GL_GetCurrentWindow()) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(w, &info) && info.subsystem == SDL_SYSWM_WINDOWS) return info.info.win.window;
  }
#endif
  return nullptr;
}

asio::Config asioConfig(const Settings& st) {
  asio::Config c;
  c.driver = st.getString("audio.asio.driver");
  c.buffer = static_cast<int>(st.getNumber("audio.asio.buffer"));
  c.rate = static_cast<int>(st.getNumber("audio.asio.sampleRate"));
  c.channels.inFirst = static_cast<int>(st.getNumber("audio.asio.inFirst"));
  c.channels.inCount = static_cast<int>(st.getNumber("audio.asio.inCount"));
  c.channels.outMain = static_cast<int>(st.getNumber("audio.asio.outMain"));
  c.channels.extras = asio::parseExtraOuts(st.getString("audio.asio.extraOuts"));
  return c;
}

void publishAsio(App& a) {
  App::AsioState& s = a.asio;
  const auto& d = gAsio.device();
  const auto& p = gAsio.plan();
  s.active = gAsioActive;
  s.driver = gAsioActive ? d.driver : "";
  s.numIn = d.numIn, s.numOut = d.numOut;
  s.inNames = d.inNames, s.outNames = d.outNames;
  s.bufferChoices = d.bufferChoices, s.rates = d.rates;
  s.minBuffer = d.buffers.minSize, s.maxBuffer = d.buffers.maxSize, s.preferredBuffer = d.buffers.preferred, s.granularity = d.buffers.granularity;
  s.buffer = gAsio.bufferFrames(), s.rate = gAsio.sampleRate();
  s.inLatency = gAsio.inputLatency(), s.outLatency = gAsio.outputLatency();
  s.inputs = static_cast<int>(p.inputs.size());
  s.inFirst = p.inputs.empty() ? 0 : p.inputs.front();
  s.mainOut = std::max(0, p.mainL);
  s.mono = p.mainL >= 0 && p.mainL == p.mainR;
  s.extraOuts = static_cast<int>(p.extras.size());
  s.format = p.mainL >= 0 && p.mainL < static_cast<int>(d.outTypes.size()) ? asio::formatName(d.outTypes[static_cast<size_t>(p.mainL)]) : "";
  s.outputReady = gAsio.usesOutputReady();
}

// Opens the chosen ASIO driver and starts the engine on it; false (and a.asio.error) on failure.
bool openAsio(App& a, int depth = 0) {
  Settings& st = a.settings;
  const asio::Config cfg = asioConfig(st);
  gAsioRender.eng = &a.engine;
  std::string err;
  if (!gAsio.open(cfg, nativeWindowHandle(), asioRender, &gAsioRender, &err)) {
    a.asio.error = err;
    gAsioFollowDriver = false;
    return false;
  }
  const auto& dev = gAsio.device();
  // A reset after the driver's own panel changed its buffer: follow the driver's new size.
  if (gAsioFollowDriver && depth == 0 && gAsioPreferredOf == dev.driver && gAsioPreferred > 0 && dev.buffers.preferred != gAsioPreferred &&
      static_cast<int>(st.getNumber("audio.asio.buffer")) != 0) {
    st.set("audio.asio.buffer", 0);
    gAsio.close();
    gAsioFollowDriver = false;
    return openAsio(a, 1);
  }
  gAsioFollowDriver = false;
  gAsioPreferred = dev.buffers.preferred, gAsioPreferredOf = dev.driver;
  const asio::ChannelPlan& plan = gAsio.plan();
  const int frames = gAsio.bufferFrames();
  const int nIn = static_cast<int>(plan.inputs.size());
  gAsioRender.inter.assign(static_cast<size_t>(frames) * static_cast<size_t>(std::max(1, nIn)), 0.f);
  gAsioRender.mix.assign(2 * static_cast<size_t>(frames), 0.f);
  gAsioRender.monoR.assign(static_cast<size_t>(frames), 0.f);
  gAsioRender.mono = plan.mainL == plan.mainR;
  gAsioRender.extraCount = static_cast<int>(plan.extras.size());
  for (int k = 0; k < gAsioRender.extraCount; ++k) {
    gAsioRender.extraMixer[k] = plan.extras[static_cast<size_t>(k)].mixer;
    gAsioRender.extraOut[k] = plan.extraOut(k);
  }
  const int rate = gAsio.sampleRate();
  a.engine.setSampleRate(rate);  // the driver is not streaming yet
  gUiSounds = nullptr;
  if (!a.uiSoundsDisabled && !a.assetsDir.empty()) {
    a.loadSoundTheme(rate, true);
    gUiSounds = &a.uiSounds;
  }
  a.audioSampleRate = rate;
  a.audioBuffer = frames;
  a.audioInputStatus.clear();
  a.audioInputLatency = 0;
  gSynthInput = false;
  if (nIn > 0) {  // the driver's inputs feed the mixer's In 1.. (same callback: no jitter buffer)
    gTestInput.store(nullptr, std::memory_order_release);
    a.engine.setInputChannels(nIn);
    a.engine.setInputPrefill(0);
    a.audioInputDevice = "ASIO: " + dev.driver;
    a.audioInputChannels = nIn;
    // The driver's reported input and output latency: what a recording is moved earlier by.
    a.audioInputLatency = gAsio.inputLatency() + gAsio.outputLatency();
    char buf[256];
    const size_t first = static_cast<size_t>(plan.inputs.front());
    const std::string firstName = first < dev.inNames.size() && !dev.inNames[first].empty() ? dev.inNames[first] : "input " + std::to_string(first + 1);
    std::snprintf(buf, sizeof buf, "Recording from %s: %d input%s, In 1 is %s.", dev.driver.c_str(), nIn, nIn == 1 ? "" : "s", firstName.c_str());
    a.audioInputStatus = buf;
  }
  a.asio.error.clear();
  if (!gAsio.start(&err)) {
    gAsio.close();
    a.asio.error = err;
    return false;
  }
  gAsioActive = true;
  if (nIn == 0) {  // no driver inputs: the system input device (if one is chosen) feeds the mixer
    a.audioInputDevice = gSdlInputDevice;
    openAudioInput(a, rate, gAsio.outputLatency());
  }
  char buf[256];
  std::snprintf(buf, sizeof buf, "Running on the ASIO driver %s: %d Hz, %d-frame buffer, latency %d in / %d out.", dev.driver.c_str(), rate, frames,
                gAsio.inputLatency(), gAsio.outputLatency());
  a.audioStatus = buf;
  publishAsio(a);
  return true;
}

void closeAsio(App& a) {
  if (gAsio.isOpen()) gAsio.close();
  if (gAsioActive) {  // back to the system audio: its own settings again
    gAsioActive = false;
    if (a.audioInputDevice.rfind("ASIO: ", 0) == 0) a.audioInputDevice = gSdlInputDevice;
    a.audioSampleRate = static_cast<int>(a.settings.getNumber("audio.sampleRate"));
    a.audioBuffer = static_cast<int>(a.settings.getNumber("audio.bufferFrames"));
  }
  a.asio.active = false;
}

// UI thread, every frame: what the driver asked for (resets, latency changes, a stopped device).
void pollAsio(App& a) {
  if (a.asio.panelRequest) {
    a.asio.panelRequest = false;
    if (gAsioActive && !gAsio.controlPanel()) a.notify("This ASIO driver has no control panel.");
  }
  if (!gAsioActive) return;
  const int ev = gAsio.poll();
  if (ev & asio::Host::Latency) {
    if (a.audioInputDevice.rfind("ASIO: ", 0) == 0) a.audioInputLatency = gAsio.inputLatency() + gAsio.outputLatency();
    publishAsio(a);
  }
  if (ev & asio::Host::Stalled) {
    a.notify("The ASIO driver stopped delivering audio (was the device unplugged?). Using the system audio; choose the driver again to retry.", true);
    gAsioSkip = true;
    a.audioReopen = true;
    return;
  }
  // A new rate from the driver's own panel: the device's clock wins.
  if ((ev & asio::Host::RateChanged) && static_cast<int>(a.settings.getNumber("audio.asio.sampleRate")) != 0) a.settings.set("audio.asio.sampleRate", 0);
  if (ev & (asio::Host::Reset | asio::Host::Resync | asio::Host::RateChanged)) {
    gAsioFollowDriver = (ev & asio::Host::Reset) != 0;
    a.audioReopen = true;
  }
}

void openAudio(App& a) {
  gTestInput.store(nullptr, std::memory_order_release);  // J-218: before the output callback stops
  if (gInDevice) {
    SDL_CloseAudioDevice(gInDevice);
    gInDevice = 0;
  }
  if (gDevice) {
    SDL_CloseAudioDevice(gDevice);
    gDevice = 0;
  }
  closeAsio(a);
  if (gVideoFps > 0) {  // K-10: the engine runs from the frame loop (videoClockStep)
    a.engine.setSampleRate(a.audioSampleRate);
    a.audioStatus = "Video clock: no audio device.";
    return;
  }
  a.asio.available = asio::backendAvailable();
  a.asio.drivers.clear();
  if (a.asio.available)
    for (const auto& d : asio::listDrivers()) a.asio.drivers.push_back(d.name);
  if (a.asio.available && a.settings.getString("audio.backend") == "asio" && !gAsioSkip) {
    gSdlInputDevice = a.audioInputDevice;
    if (openAsio(a)) return;
    a.audioInputDevice = gSdlInputDevice;
    a.notify(a.asio.error + " Using the system audio.", true);
  }
  gAsioSkip = false;
  a.audioDevices.clear();
  for (int i = 0, n = SDL_GetNumAudioDevices(0); i < n; ++i)
    if (const char* nm = SDL_GetAudioDeviceName(i, 0)) a.audioDevices.push_back(nm);
  SDL_AudioSpec want{}, have{};
  want.freq = a.audioSampleRate;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = static_cast<Uint16>(a.audioBuffer);
  want.callback = audioCallback;
  want.userdata = &a.engine;
  const char* name = a.audioDevice.empty() ? nullptr : a.audioDevice.c_str();
  gDevice = SDL_OpenAudioDevice(name, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if (!gDevice && name) gDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  char buf[256];
  if (!gDevice) {
    std::snprintf(buf, sizeof buf, "No audio output: %s", SDL_GetError());
    a.audioStatus = buf;
    a.notify(a.audioStatus, true);
    return;
  }
  a.engine.setSampleRate(have.freq);
  // J-48: the device is still paused, so the callback cannot be reading the clips.
  gUiSounds = nullptr;
  if (!a.uiSoundsDisabled && !a.assetsDir.empty()) {
    a.loadSoundTheme(have.freq, true);  // J-133: the chosen sound theme (assets/sounds/<theme>/)
    gUiSounds = &a.uiSounds;
  }
  std::snprintf(buf, sizeof buf, "Running on %s via %s: %d Hz, %d-frame buffer.", name ? name : "the default device",
                SDL_GetCurrentAudioDriver(), have.freq, have.samples);
  a.audioStatus = buf;
  SDL_PauseAudioDevice(gDevice, 0);
  openAudioInput(a, have.freq, have.samples);
}

std::string findAssets(const char* argv0) {
  std::vector<fs::path> cands;
  if (const char* env = std::getenv("GRIDLINE_ASSETS")) cands.emplace_back(env);
  if (char* base = SDL_GetBasePath()) {
    fs::path b(base);
    SDL_free(base);
    cands.push_back(b / "assets");
    cands.push_back(b / ".." / "assets");
    cands.push_back(b / ".." / ".." / "assets");
    cands.push_back(b / ".." / "share" / "gridline" / "assets");
    cands.push_back(b / ".." / "Resources" / "assets");
  }
  cands.push_back(fs::path(argv0).parent_path() / ".." / "assets");
  cands.emplace_back("assets");
#ifdef GRIDLINE_SOURCE_ASSETS
  cands.emplace_back(GRIDLINE_SOURCE_ASSETS);
#endif
  cands.emplace_back("/usr/share/gridline/assets");
  cands.emplace_back("/usr/local/share/gridline/assets");
  for (const auto& c : cands) {
    std::error_code ec;
    if (fs::exists(c / "fonts" / "Inter-Regular.ttf", ec)) return fs::weakly_canonical(c, ec).string();
  }
  return "";
}


// Off-screen render target for --screenshot at sizes larger than the display
// (and for simulating HiDPI with --scale). GL 3 entry points come from SDL.
struct OffscreenTarget {
  using GenFB = void (*)(GLsizei, GLuint*);
  using BindFB = void (*)(GLenum, GLuint);
  using FBTex = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
  static constexpr GLenum kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0;
  GLuint fbo = 0, tex = 0;
  int w = 0, h = 0;
  BindFB bind = nullptr;
  bool create(int width, int height) {
    auto gen = reinterpret_cast<GenFB>(SDL_GL_GetProcAddress("glGenFramebuffers"));
    bind = reinterpret_cast<BindFB>(SDL_GL_GetProcAddress("glBindFramebuffer"));
    auto attach = reinterpret_cast<FBTex>(SDL_GL_GetProcAddress("glFramebufferTexture2D"));
    if (!gen || !bind || !attach || width <= 0 || height <= 0) return false;
    w = width;
    h = height;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    gen(1, &fbo);
    bind(kFramebuffer, fbo);
    attach(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, tex, 0);
    bind(kFramebuffer, 0);
    return true;
  }
  void use(bool on) const {
    if (bind) bind(kFramebuffer, on ? fbo : 0);
  }
};

void writePpm(const char* path, int w, int h) {
  std::vector<unsigned char> px(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  if (FILE* f = std::fopen(path, "wb")) {
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
      std::fwrite(px.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3, 1, static_cast<size_t>(w) * 3, f);
    std::fclose(f);
  }
}

void writeVideoFrame(FILE* f, int w, int h) {  // K-10: --video-out
  static std::vector<unsigned char> px;
  px.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  for (int y = h - 1; y >= 0; --y) std::fwrite(px.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3, 1, static_cast<size_t>(w) * 3, f);
  std::fflush(f);
}

void drawVideoCursor() {  // K-10: --video-cursor, a plain arrow at the dev pointer
  const ImGuiIO& io = ImGui::GetIO();
  if (!ImGui::IsMousePosValid(&io.MousePos)) return;
  // Filled only: a thin outline polyline breaks up on the arrow's sharp corners at this size. The
  // outline is the arrow drawn black a pixel out all round, under the white.
  static const ImVec2 kArrow[7] = {{0, 0}, {0, 16.5f}, {4.1f, 12.7f}, {6.9f, 19.1f}, {9.6f, 17.9f}, {6.8f, 11.6f}, {12.2f, 11.6f}};
  ImDrawList* d = ImGui::GetForegroundDrawList();
  // Convex pieces (the concave fill drops the stem): the head as a fan from the tip, the stem as a quad.
  auto arrow = [&](float dx, float dy, ImU32 col) {
    ImVec2 p[7];
    for (int i = 0; i < 7; ++i) p[i] = ImVec2(io.MousePos.x + kArrow[i].x + dx, io.MousePos.y + kArrow[i].y + dy);
    d->AddTriangleFilled(p[0], p[1], p[2], col);
    d->AddTriangleFilled(p[0], p[2], p[5], col);
    d->AddTriangleFilled(p[0], p[5], p[6], col);
    const ImVec2 stem[4] = {p[2], p[3], p[4], p[5]};
    d->AddConvexPolyFilled(stem, 4, col);
  };
  arrow(0.9f, 1.4f, IM_COL32(0, 0, 0, 60));  // soft shadow
  static const float kRing[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};
  for (const auto& o : kRing) arrow(o[0], o[1], IM_COL32(0, 0, 0, 255));
  arrow(0.1f, 0.25f, IM_COL32(255, 255, 255, 255));  // (in from the tip, so the white never passes the outline there)
}

// Typing keyboard: where a key sits → the key code TypingKeyboard uses, so the rows stay a piano
// on any layout. 0 = not a typing key.
int typingKey(SDL_Scancode s) { return TypingKeyboard::keyAtPosition(static_cast<int>(s)); }

struct HeldNote {
  ObjectId channel;
  int pitch;
  double startTick;
};

// J-109: ends every note held on the typing keyboard (on the channel each key started it on).
void releaseTypedNotes(App& app, std::map<SDL_Scancode, HeldNote>& held) {
  for (const auto& [k, h] : held)
    for (int p : app.typing.keyUp(typingKey(k))) {
      app.engine.noteOff(h.channel, p);
      app.playedNoteOff(h.channel, p);
    }
  for (int p : app.typing.releaseAllKeys()) {  // (keys not in `held`; K-175: every key counts as up)
    app.engine.noteOff(app.selChannel, p);
    app.playedNoteOff(app.selChannel, p);
  }
  held.clear();
}

// J-28 dev check: scripted input for interaction checks (--dev-input file).
// One command per line, "<frame> <command> <args>", in logical pixels:
//   move x y · down b · up b · mod ctrl|shift|alt|super 0|1 (super = Cmd: Ctrl + click is a right click on macOS) · wheel dy · key <letter>
//   audio <wav> <track> <bar> (browser drop path) · dump (playlist clips to stdout)
//   shot <out.ppm> (J-37: this frame is saved once rendered — frame sequences)
//   text <characters> (J-182: typed into the focused text field)
//   playmode song|pattern (K-4: the transport's play mode)
//   key space (J-49) · key escape · key left|right|up|down|enter|delete|tab (J-141) · key pageup|pagedown|home|end|kpenter · keyhold up|down 1|0 (K-160), notes (Q-3: piano roll notes to stdout) · transport (J-49: song position / playing / seek position to stdout)
//   scroll dx dy (J-67: one trackpad-style two-axis scroll event, wheel units)
//   momentum dx dy frames (J-67: a decaying scroll stream over the next frames, like an OS fling)
//   trace on|off (J-67: per frame, the drawn scroll positions to stdout)
//   J-75 drags from other applications (logical pixels): dragenter x y <file|promise:file|"path with spaces">...,
//   dragmove x y, drop [x y], dragleave · undo · channels (channel names / samples / routing / steps to stdout)
//   (J-97: a "plugin:<type>" item drags a plugin like the browser does; J-110: "clipsrc:last" (the newest audio
//   clip source, a take just recorded) or "clipsrc:<channel index>" drags it like the playlist picker does)
//   mixer (J-83: track volumes / pans, the strips pane's scroll and wheel policy to stdout)
//   rack (J-84: channel rack layout, step scroll, channel volumes / pans, step velocities / pitches to stdout)
//   launcher [home|back|close] (J-141: the welcome launcher's state and tiles to stdout, or show it / leave it /
//   Close project); signals launcher-shown / launcher-hidden
//   quit [discard] (J-103: an SDL_QUIT, the normal quit path: unsaved-changes guard, teardown, settings saved;
//   "discard" drops unsaved changes first, as the prompt's "Don't save")
//   J-109: sdlkey <key>[@<position>] down|up [ctrl|super] (K-175: #<scan code> for <key>, "repeat" among the mods: a key repeat) · focuslost · typing on|off|show · K-175: typing octave · K-113: typing layout <id> · typing labels · noteon|noteoff <channel index> <pitch> ·
//   previewfile <wav> · sound (output peak since the last "sound", live notes held, preview, panics, playing);
//   waitfor output-silent: the output stayed below 1e-5 for 0.2 s (forget output-silent to wait again)
//   J-110: stopvia button|menu|remote (the Stop button's action, Tools > Stop all sound, a MIDI controller's /
//   remote Stop) · epoch (the engine's stop epoch to stdout) · forget <name> (a later waitfor <name> waits for it anew)
//   J-112: playvia remote (the Play / Pause remote / MIDI action) · startsong (a start that is not the Play command, as a
//   playlist click) · playstate (transport, plugin playback, stop epoch, the focused window to stdout) · focus <window>;
//   reported each frame for waitfor: transport-playing / transport-stopped, plugin-playback, transport-moved (playing at
//   least a tick past the set position)
//   J-111: collab profile <name> · collab new <folder> · collab invite <file> [minutes] · collab join <code file> <folder>
//   (waits for the file) · collab open <file|folder> · collab edit <channel name> · collab reconnect-code <file> ·
//   collab reconnect <file> · collab stall <seconds> · collab report <file> (JSON: shared song digest, members, connected, unseen,
//   name, title) · collab links · J-111f: collab new <folder> [name] · collab rename "<name>" · collab rename-window ·
//   collab name-window · collab project-info [rename] · collab demo-rename "<name>" (the --show collab-demo collaborator renames);
//   J-111h: collab share-name "<name>" · collab share-folder <dir> · collab share · collab rename-type "<name>" [folder] ·
//   collab rename-ok · collab demo-cursor-edge <window> <cover> <dx> · collab window settings|info|none ·
//   collab section-probe · windows [frames] (window positions and sizes to stdout);
//   strip [on|off] [frames] (the plugin window's channel strip: shown fraction, heights, eye button and window rects; Windows.h) ·
//   plugwin chan <index> | fx <mixer index> <slot> | close (opens / closes plugin windows);
//   waitfor signals collab-active / -ready / -invite-ready / -joined / -join-failed / -connected / -all-connected /
//   -synced / -members:<n> / -left-members:<n> / -channel:<name> / -name:<project name>
//   J-111d: collab point <window> <kind> <a> <b> <cx> <cy> (the pointer to a cursor anchor, CursorAnchor.h) ·
//   collab demo-cursor <same> (the simulated collaborator's pointer, --show collab-cursors) · collab cursors <file>
//   (JSON: cursors shown and what each is over here) · collab pixel <file>; signals collab-cursor:<kind>:shown|hidden,
//   collab-cursor-window:<window>:shown|hidden
//   J-111g: collab picture <file|#RRGGBB> · collab panel · collab pictures <file> (JSON: members' pictures announced,
//   received, kept, drawn, a pixel) · collab open-editor chN · collab demo-cursor-plugin chN|none · collab trace-reset;
//   signals collab-picture:<name> (drawn here), collab-picture-kept:<name> (kept on this computer, the one drawn)
//   waitfor <name> <seconds> (J-108: the script and --frames stop counting until the app reports
//   devsig::reach(name) or the seconds pass; prints "waitfor <name>: reached|timed out after <s> s");
//   K-B: waitfor "file:<path>" <seconds> waits until that file exists (another app run's report)
//   K-15: loupe (the Loupe panel: pinned, docked with which window, size, its form's layout to stdout) ·
//   loupe pin tab|below|left|right|float (pins the panel's / first Loupe there) · loupe unpin
//   K-19: edzoom fxeditor|channelsettings (the plugin window's editor zoom, design size and window to stdout) ·
//   edzoomset <plugin type> <zoom> (the zoom remembered for a plugin type, 0 = automatic; atparam points at the magnified spot)
//   K-10: cursor 0|1 (hides or shows the --video-cursor pointer)
//   groove (the song's swing, the swing preset chip, its menu rows and the Groove editor's lanes and buttons to stdout)
//   K-9: save <file> (the project, plugin state included) · rmchan <index> (removes a channel, its plugin window open or not);
//   K-148: knobs (every knob / plugin editor control drawn last frame: item, window, id, name, value, normalized value and default,
//   centre, size, to stdout) · atknob <item hex> [dx dy] (the pointer to that knob's centre plus logical px) · closepopups
//   --dev-input-live <file>: a test harness drives a running app: whenever <file> exists, its lines ("0 <command> <args>")
//   run on the next frame and the file is removed (tests/yabridge)
std::string devShotPending;
bool devFitNoDraw = false;  // K-147: a fitall sweep without screenshots only lays windows out (no GL drawing)
bool devNoDraw = false;     // --no-draw: frames are laid out, not drawn (a slow software OpenGL spends most of a frame drawing)
bool devLoadHold = true;  // J-167: frames do not count while a project loads ("loadhold off")
bool devTrace = false;
bool devVideoCursor = false;  // K-10
ImVec2 devTraceWheel;  // this frame's wheel input (io clears it at the end of the frame)
struct DevWait {
  std::string name;
  double seconds = 0;
  Uint64 start = 0;
  bool on = false;
  double uiStart = 0;  // the interface's clock (ImGui::GetTime) when the wait began
} devWait;
// A --dev-input script counts frames, and two of its events a frame or two apart mean a quick
// double-click or one wheel gesture. On a slow machine (software OpenGL on a shared runner) a frame
// can take a few hundred ms; the interface's clock then moves on at most this much per frame, so
// the script means the same everywhere. (--fixed-dt sets every frame's step instead.)
constexpr float kDevInputMaxStep = 0.1f;
float devSoundPeak = 0;  // J-109: output peak since the last "sound" (also drained each frame for "output-silent")
double devLongestFrameMs = 0;  // K-71: the longest frame (its work, not the wait for the display) since the last "scan"
double devSecondFrameMs = 0;   // ... and the next longest (tests tell one outlier on a busy machine from a stall)
double devFrameMsSum = 0;      // K-73: ... and the sum and count of those frames (their average)
int devFrameCount = 0;
// J-108: true while a "waitfor" holds the script's frame count (checked once per frame).
bool devWaitHolds() {
  if (!devWait.on) return false;
  const double t = static_cast<double>(SDL_GetPerformanceCounter() - devWait.start) / static_cast<double>(SDL_GetPerformanceFrequency());
  if (devWait.name.rfind("file:", 0) == 0 && !devsig::reached(devWait.name)) {  // K-B: "waitfor file:<path>": the file exists
    std::error_code ec;
    if (fs::exists(fs::u8path(devWait.name.substr(5)), ec)) devsig::reach(devWait.name);
  }
  const bool reached = devsig::reached(devWait.name);
  // A wait that times out gives the interface that much of its own clock too (kDevInputMaxStep):
  // animations a script waits for get their time on a slow machine as well.
  if (!reached && (t < devWait.seconds || ImGui::GetTime() - devWait.uiStart < devWait.seconds)) return true;
  std::printf("waitfor %s: %s after %.2f s\n", devWait.name.c_str(), reached ? "reached" : "timed out", t);
  std::fflush(stdout);
  devWait.on = false;
  return false;
}
struct DevMomentum {
  float dx = 0, dy = 0;
  int left = 0;
} devMomentum;
struct DevInputLine {
  int frame = 0;
  std::string cmd, a0, a1, a2;
  std::vector<std::string> args;  // every argument (quotes group words)
};
std::pair<int, int> devSdlPointer{0, 0};  // J-91: last "sdlmove" position (window pixels)
float devDragScale = 1.0f;  // this frame's interface scale (drag commands post window coordinates)

std::vector<std::string> splitArgs(const char* line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false, any = false;
  for (const char* p = line; *p; ++p) {
    const char c = *p;
    if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if (!quoted && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
      if (any) out.push_back(cur);
      cur.clear();
      any = false;
    } else {
      cur += c;
      any = true;
    }
  }
  if (any) out.push_back(cur);
  return out;
}

std::vector<DevInputLine> loadDevInput(const std::string& path) {
  std::vector<DevInputLine> out;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return out;
  char line[1024];
  while (std::fgets(line, sizeof line, f)) {
    char c[64] = "", x[512] = "", y[64] = "", z[64] = "";
    int fr = 0;
    if (line[0] == '#' || std::sscanf(line, "%d %63s %511s %63s %63s", &fr, c, x, y, z) < 2) continue;
    std::vector<std::string> args = splitArgs(line);
    args.erase(args.begin(), args.begin() + static_cast<long>(std::min<size_t>(2, args.size())));
    out.push_back({fr, c, x, y, z, args});
  }
  std::fclose(f);
  return out;
}

// Q-4 piano roll checks on the edited pattern / channel (--edit p c):
//   mknotes <count> <step ticks> <length ticks>[x<chord size>] (velocity 64, pitches 60, 64, 68, ...)
//   selnotes <i,j,...|none> · notes (the notes to stdout) · lane (event lane geometry to stdout)
//   K-28: pianosnap <Line|Cell|None|"1/6 step"|"1/4 step"|...|Bar> (the snap mode, printed)
//   K-29: bendline <note index> <tick:semis[:curve[:amount]]> ... (sets that note's bend line, one undo step)
void devPianoCommand(App& app, const DevInputLine& l, int frame) {
  const Pattern* pat = app.doc.song().pattern(app.curPattern);
  const ObjectId ch = app.selChannel;
  if (l.cmd == "alane") {  // K-171: the automation lane (target, points, selection, rects) to stdout
    std::printf("alane frame %d: %s\n", frame, automationLaneDump(app, app.curPattern).c_str());
  } else if (l.cmd == "alanetarget") {  // K-171: alanetarget <owner> <index>: the lane shows that control (none: note properties)
    pianoAutomationLane() = l.args.size() >= 2 ? ParamId{static_cast<ObjectId>(std::atoi(l.args[0].c_str())), static_cast<uint32_t>(std::atoi(l.args[1].c_str()))}
                                               : ParamId{};
  } else if (l.cmd == "alaneshape") {  // K-171: alaneshape 0|1|2: the lane draws stepped / linear / smooth
    setLaneDrawShape(app, l.a0 == "2" ? AutoShape::Smooth : l.a0 == "1" ? AutoShape::Curve : AutoShape::Hold);
  } else if (l.cmd == "alanesimplify") {  // K-171: alanesimplify <tolerance %> <0 linear|1 smooth>: the Simplify popover's Apply
    simplifyAutomationLane(app, app.curPattern, std::atof(l.a0.c_str()) / 100.0, l.a1 == "1" ? AutoShape::Smooth : AutoShape::Curve);
  } else if (l.cmd == "pianotool") {  // K-171: pianotool draw|select|erase|zoom: the piano roll's tool
    app.piano.tool = l.a0 == "select" ? Tool::Select : l.a0 == "erase" ? Tool::Erase : l.a0 == "zoom" ? Tool::Zoom : Tool::Draw;
  } else if (l.cmd == "lane") {
    const PianoLaneGeometry& g = pianoLaneGeometry();
    std::printf("lane frame %d: x0 %.3f pxPerTick %.6f yTop %.2f yBottom %.2f valid %d pitchY0 %.2f rowH %.3f preview %d live %d"
                " view sx %.3f sy %.3f grid %.2f %.2f %.2f %.2f hthumb %.3f %.3f y %.2f vthumb %.3f %.3f x %.2f\n",
                frame, g.x0, g.pxPerTick, g.yTop, g.yBottom, g.valid ? 1 : 0, g.pitchY0, g.rowH, g.preview, app.engine.liveHeld(), g.scrollX,
                g.scrollY, g.gridX0, g.gridY0, g.gridX1, g.gridY1, g.hThumb0, g.hThumb1, g.hBarY, g.vThumb0, g.vThumb1, g.vBarX);
  } else if (!pat || ch == kNoId) {
    std::printf("%s frame %d: no pattern / channel\n", l.cmd.c_str(), frame);
  } else if (l.cmd == "pianosnap") {  // K-28: pianosnap <mode name, e.g. Line, None, "1/4 step">: the piano roll's snap
    for (int i = 0; i < kSnapCount; ++i)
      if (l.args.size() >= 1 && l.args[0] == snapName(static_cast<Snap>(i))) app.piano.snap = static_cast<Snap>(i);
    std::printf("pianosnap frame %d: %s grid %lld\n", frame, snapName(app.piano.snap), static_cast<long long>(app.piano.effGrid));
  } else if (l.cmd == "bendline") {  // K-29
    auto it = pat->notes.find(ch);
    const size_t ni = static_cast<size_t>(std::max(0, std::atoi(l.a0.c_str())));
    if (it != pat->notes.end() && ni < it->second.size()) {
      std::vector<Note> ns = it->second;
      ns[ni].bend.clear();
      for (size_t i = 1; i < l.args.size(); ++i) {
        float t = 0, v = 0, k = 0;
        int c = 0;
        if (std::sscanf(l.args[i].c_str(), "%f:%f:%d:%f", &t, &v, &c, &k) >= 2) ns[ni].bend.push_back(BendPoint(t, v, static_cast<uint8_t>(c), k));
      }
      bend::normalize(ns[ni].bend);
      app.doc.setNotes(pat->id, ch, ns, "dev bend line", 0);
    }
  } else if (l.cmd == "mknotes") {  // a2 = "<length>" or "<length>x<chord size>"
    const int count = std::atoi(l.a0.c_str());
    const Tick step = std::atoll(l.a1.c_str());
    char* e = nullptr;
    const Tick len = std::max<Tick>(1, std::strtoll(l.a2.c_str(), &e, 10));
    const int chord = e && *e == 'x' ? std::clamp(std::atoi(e + 1), 1, 8) : 1;
    std::vector<Note> ns;
    for (int i = 0; i < count; ++i)
      for (int k = 0; k < chord; ++k) ns.push_back(Note(static_cast<Tick>(i) * step, len, 60 + 4 * k, 64));
    app.doc.setNotes(pat->id, ch, ns, "dev notes", 0);
    app.piano.selected.clear();
  } else if (l.cmd == "selnotes") {
    app.piano.selected.clear();
    if (l.a0 != "none") {
      const char* p = l.a0.c_str();
      while (*p) {
        char* e = nullptr;
        const long v = std::strtol(p, &e, 10);
        if (e == p) break;
        app.piano.selected.insert(static_cast<size_t>(v));
        p = *e == ',' ? e + 1 : e;
      }
    }
  } else {  // notes
    auto it = pat->notes.find(ch);
    const size_t n = it == pat->notes.end() ? 0 : it->second.size();
    std::printf("notes frame %d: %zu notes, undo %zu\n", frame, n, app.doc.history().position());
    for (size_t i = 0; i < n; ++i) {
      const Note& x = it->second[i];
      std::printf("  note %zu start %lld len %lld pitch %d vel %d pan %d rel %d fine %d modx %d mody %d sel %d\n", i, static_cast<long long>(x.start),
                  static_cast<long long>(x.length), x.pitch, x.velocity, x.pan, x.release, x.fine, x.modX, x.modY, app.piano.selected.count(i) ? 1 : 0);
    }
  }
  std::fflush(stdout);
}

// J-111h: "windows [frames]": the main window's size and every shown root window's position and size
// (this frame, or each of the next `frames` frames) to stdout, to see a window's size change over frames.
int devWindowsLeft = 0;
bool devFlpAsk = false;  // K-110: --show flp-export-options
void devDumpWindows(int frame) {
  const ImGuiContext& g = *ImGui::GetCurrentContext();
  int ww = 0, wh = 0;
  if (SDL_Window* w = SDL_GL_GetCurrentWindow()) SDL_GetWindowSize(w, &ww, &wh);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  std::printf("windows frame %d: main %dx%d viewport %.0fx%.0f\n", frame, ww, wh, static_cast<double>(vp->Size.x), static_cast<double>(vp->Size.y));
  for (const ImGuiWindow* w : g.Windows)
    if (w->WasActive && !w->Hidden && !(w->Flags & ImGuiWindowFlags_ChildWindow) && !w->DockIsActive)
      std::printf("  window \"%s\" pos %.1f,%.1f size %.1fx%.1f\n", ui::stableWindowName(w->Name), static_cast<double>(w->Pos.x),
                  static_cast<double>(w->Pos.y), static_cast<double>(w->Size.x), static_cast<double>(w->Size.y));
  std::fflush(stdout);
}

// SDL key modifiers of a dev key command: "ctrl", "super", "shift", "alt", joined with '+'.
int devKeyMods(const std::string& m) {
  int r = KMOD_NONE;
  if (m.find("ctrl") != std::string::npos) r |= KMOD_LCTRL;
  if (m.find("super") != std::string::npos) r |= KMOD_LGUI;
  if (m.find("shift") != std::string::npos) r |= KMOD_LSHIFT;
  if (m.find("alt") != std::string::npos) r |= KMOD_LALT;
  return r;
}

void runDevInput(App& app, const std::vector<DevInputLine>& script, int frame) {
  ImGuiIO& io = ImGui::GetIO();
  // The scripted pointer stays put between commands (the backend would
  // otherwise report the real pointer whenever the window has focus).
  static ImVec2 pointer(-FLT_MAX, -FLT_MAX);
  if (pointer.x > -FLT_MAX) io.AddMousePosEvent(pointer.x, pointer.y);
  if (devMomentum.left > 0) {  // the fling decays like an OS momentum phase (~0.9 per 60 Hz frame);
    // events land on two of every three frames, like OS event timing against frame timing
    if (devMomentum.left % 3 != 0) io.AddMouseWheelEvent(devMomentum.dx, devMomentum.dy);
    devMomentum.dx *= 0.9f;
    devMomentum.dy *= 0.9f;
    --devMomentum.left;
  }
  static int lastFrame = INT_MIN;  // a "waitfor" holds the frame: its commands run once
  const bool fresh = frame != lastFrame;
  lastFrame = frame;
  // K-147: "fitall [frames]": every built-in plugin's editor opened in turn (effects in the
  // last slot of insert 1, instruments and note processors on a channel), each one's window
  // fit report (edfit) once its window has kept its size for two frames (at most `frames`
  // frames); "edfit done" at the end, and the app quits (the run never waits on its frame count).
  static struct {
    bool on = false;
    size_t i = 0;
    int f = 0, per = 12, stable = 0;
    ImVec2 last{0, 0};
    Uint32 t0 = 0;
    std::string shots;  // a directory: a screenshot of each one (<n>_<type>.ppm)
    ObjectId chan = kNoId;
    std::vector<std::string> types;
  } fit;
  for (const DevInputLine& l : script)
    if (fresh && l.frame == frame && l.cmd == "fitall") {
      fit = {};
      fit.on = true;
      fit.per = std::max(4, l.a0.empty() ? 12 : std::atoi(l.a0.c_str()));
      fit.shots = l.args.size() > 1 ? l.args[1] : std::string();
      const char* only = std::getenv("GRIDLINE_FIT_ONLY");  // comma separated types (dev aid)
      for (const auto& p : pluginCatalog())
        if (!p.hidden && !isHostedType(p.type) && (!only || (std::string(",") + only + ",").find("," + p.type + ",") != std::string::npos))
          fit.types.push_back(p.type);
    }
  devFitNoDraw = fit.on && fit.shots.empty();
  if (fresh && fit.on) {
    if (fit.i >= fit.types.size()) {
      std::printf("edfit done %zu\n", fit.types.size());
      std::fflush(stdout);
      fit.on = false;
      app.doc.markSaved();  // the plugins it added are not the user's: quit without asking
      SDL_Event q{};
      q.type = SDL_QUIT;
      SDL_PushEvent(&q);
    } else {
      const std::string& type = fit.types[fit.i];
      const PluginInfo* info = findPlugin(type);
      if (fit.f == 0) {
        fit.stable = 0;
        fit.last = ImVec2(0, 0);
        fit.t0 = SDL_GetTicks();
      }
      if (fit.f == 0 && info) {
        if (fit.chan != kNoId) app.doc.removeChannel(fit.chan);
        fit.chan = kNoId;
        app.settingsChannel = kNoId;
        app.fxEditor = {kNoId, -1};
        if (info->kind == PluginKind::Generator) {
          app.settingsChannel = fit.chan = app.addInstrument(type);
        } else if (info->kind == PluginKind::NoteFx) {
          app.settingsChannel = fit.chan = app.addInstrument("gl.hat");
          app.doc.addNoteFx(fit.chan, type);
        } else if (app.doc.song().mixer.size() > 1) {
          const ObjectId mt = app.doc.song().mixer[1].id;
          app.doc.setSlotPlugin(mt, kEffectSlots - 1, type);
          app.fxEditor = {mt, kEffectSlots - 1};
        }
        app.syncEngine();
      }
      const char* key = info && info->kind == PluginKind::Effect ? "fxeditor" : "channelsettings";
      const ImVec2 size = fit.f > 0 ? edzoomui::devWindowSize(key) : ImVec2(0, 0);
      fit.stable = size.x > 0 && size.x == fit.last.x && size.y == fit.last.y ? fit.stable + 1 : 0;
      fit.last = size;
      if (++fit.f >= fit.per || (fit.f >= 4 && fit.stable >= 2)) {
        std::printf("edfit plugin %s kind %d frames %d settled %d ms %llu\n", type.c_str(), info ? static_cast<int>(info->kind) : -1, fit.f,
                    fit.stable >= 2 ? 1 : 0, static_cast<unsigned long long>(SDL_GetTicks() - fit.t0));
        edzoomui::devDump(key);
        if (!fit.shots.empty()) devShotPending = fit.shots + "/" + std::to_string(fit.i) + "_" + type + ".ppm";
        fit.f = 0;
        ++fit.i;
      }
    }
  }
  if (fresh && devWindowsLeft > 0) {
    --devWindowsLeft;
    devDumpWindows(frame);
  }
  if (fresh && devFlpAsk && frame == 3) {  // K-110 layout check: the FL export options window
    devFlpAsk = false;
    flpexport::askAndStart(app, (std::filesystem::path(app.configDir()) / "dev-flp-export" / "check.flp").string(), false);
  }
  for (const auto& l : script) {
    if (!fresh || l.frame != frame) continue;
    if (l.cmd == "sdlwin") {  // K-153: the main window as the window system has it (Wayland checks)
      SDL_Window* w = SDL_GL_GetCurrentWindow();
      if (!w) continue;
      if (l.a0 == "size" && l.args.size() >= 3) {
        SDL_SetWindowSize(w, std::atoi(l.args[1].c_str()), std::atoi(l.args[2].c_str()));
      } else if (l.a0 == "fullscreen") {
        SDL_SetWindowFullscreen(w, l.a1 == "1" ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
      } else if (l.a0 == "maximize") {
        if (l.a1 == "1") SDL_MaximizeWindow(w); else SDL_RestoreWindow(w);
      } else if (l.a0 == "clipset" && l.args.size() >= 2) {
        std::printf("sdlwin frame %d: clipset %s\n", frame, SDL_SetClipboardText(l.args[1].c_str()) == 0 ? "ok" : SDL_GetError());
      } else if (l.a0 == "drop" && l.args.size() >= 2) {  // a file dropped by another program, as SDL reports it
        const Uint32 id = SDL_GetWindowID(w);
        SDL_Event e{};
        e.type = SDL_DROPBEGIN, e.drop.windowID = id;
        SDL_PushEvent(&e);
        e = SDL_Event{};
        e.type = SDL_DROPFILE, e.drop.windowID = id, e.drop.file = SDL_strdup(l.args[1].c_str());
        SDL_PushEvent(&e);
        e = SDL_Event{};
        e.type = SDL_DROPCOMPLETE, e.drop.windowID = id;
        SDL_PushEvent(&e);
      } else if (l.a0 == "clipget") {
        char* t = SDL_GetClipboardText();
        std::printf("sdlwin frame %d: clip \"%s\"\n", frame, t ? t : "");
        SDL_free(t);
      } else {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(w, &ww, &wh);
        SDL_GL_GetDrawableSize(w, &dw, &dh);
        const Uint32 f = SDL_GetWindowFlags(w);
        const ImGuiIO& gio = ImGui::GetIO();
        std::printf("sdlwin frame %d: driver %s window %dx%d drawable %dx%d imgui %.0fx%.0f fb-scale %.2f fullscreen %d maximized %d shown %d "
                    "focus %d displays %d display %d ui-scale %.2f\n",
                    frame, SDL_GetCurrentVideoDriver(), ww, wh, dw, dh, static_cast<double>(gio.DisplaySize.x), static_cast<double>(gio.DisplaySize.y),
                    static_cast<double>(gio.DisplayFramebufferScale.x), (f & SDL_WINDOW_FULLSCREEN) ? 1 : 0, (f & SDL_WINDOW_MAXIMIZED) ? 1 : 0,
                    (f & SDL_WINDOW_SHOWN) ? 1 : 0, (f & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0, SDL_GetNumVideoDisplays(), SDL_GetWindowDisplayIndex(w),
                    static_cast<double>(uiscale::detected()));
      }
      std::fflush(stdout);
      continue;
    }
    if (viewports::devCommand(app, l.cmd, l.args, frame)) continue;  // J-113
    if (pluginWindowDevCommand(app, l.cmd, l.args, frame)) continue;  // plugin windows, their channel strip
    if (packs::devCommand(app, l.cmd, l.args)) continue;  // K-15: loupe [pin <tab|below|left|right|float> | unpin]
    if (livetake::devCommand(app, l.cmd, l.args, frame)) continue;  // J-217: live recording checks
    // Two chains of commands: one long else-if chain goes past MSVC's nesting limit (C1061).
    bool secondHalf = false;
    if (l.cmd == "move") {
      pointer = ImVec2(std::strtof(l.a0.c_str(), nullptr), std::strtof(l.a1.c_str(), nullptr));
      io.AddMousePosEvent(pointer.x, pointer.y);
    }
    else if (l.cmd == "down" || l.cmd == "up") io.AddMouseButtonEvent(std::atoi(l.a0.c_str()), l.cmd == "down");
    else if (l.cmd == "sdlmove" || l.cmd == "sdldown" || l.cmd == "sdlup") {
      // J-91: real SDL mouse events in window pixels (read next frame like the OS's): the path a
      // click takes through the backend and the interface scale.
      SDL_Event e{};
      SDL_Window* w = SDL_GL_GetCurrentWindow();
      if (l.cmd == "sdlmove") {
        devSdlPointer = {std::atoi(l.a0.c_str()), std::atoi(l.a1.c_str())};
        e.type = SDL_MOUSEMOTION;
        e.motion.windowID = w ? SDL_GetWindowID(w) : 0;
        e.motion.x = devSdlPointer.first;
        e.motion.y = devSdlPointer.second;
      } else {
        e.type = l.cmd == "sdldown" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        e.button.windowID = w ? SDL_GetWindowID(w) : 0;
        e.button.button = static_cast<Uint8>(std::atoi(l.a0.c_str()) == 1 ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT);
        e.button.state = l.cmd == "sdldown" ? SDL_PRESSED : SDL_RELEASED;
        e.button.clicks = 1;
        e.button.x = devSdlPointer.first;
        e.button.y = devSdlPointer.second;
      }
      SDL_PushEvent(&e);
      pointer = ImVec2(-FLT_MAX, -FLT_MAX);  // the scripted logical pointer gives way to the SDL one
    }
    else if (l.cmd == "sdlat") {  // J-91: the SDL pointer to the centre of a top bar button ("play", "stop", "rec", "metro")
      float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      if (topBarButtonRect(l.a0.c_str(), x0, y0, x1, y1)) {
        if (l.a0.rfind("chroute", 0) == 0) {  // K-19: the channel window's targets show where its zoom puts them
          const ImVec2 p0 = edzoomui::toScreen("channelsettings", ImVec2(x0, y0)), p1 = edzoomui::toScreen("channelsettings", ImVec2(x1, y1));
          x0 = p0.x, y0 = p0.y, x1 = p1.x, y1 = p1.y;
        }
        devSdlPointer = {static_cast<int>(std::lround((x0 + x1) * 0.5f * devDragScale)), static_cast<int>(std::lround((y0 + y1) * 0.5f * devDragScale))};
        SDL_Event e{};
        SDL_Window* w = SDL_GL_GetCurrentWindow();
        e.type = SDL_MOUSEMOTION;
        e.motion.windowID = w ? SDL_GetWindowID(w) : 0;
        e.motion.x = devSdlPointer.first;
        e.motion.y = devSdlPointer.second;
        SDL_PushEvent(&e);
        pointer = ImVec2(-FLT_MAX, -FLT_MAX);
        std::printf("sdlat frame %d: %s at %d,%d (window pixels, scale %.2f)\n", frame, l.a0.c_str(), devSdlPointer.first, devSdlPointer.second,
                    static_cast<double>(devDragScale));
      } else {
        std::printf("sdlat frame %d: %s not shown\n", frame, l.a0.c_str());
      }
      std::fflush(stdout);
    }
    else if (l.cmd == "sdlctl" || l.cmd == "ctl") {
      // J-98: track sliders. "sdlctl <param> <frac|handle> [px]": the SDL pointer to a point of the
      // last drawn slider of parameter index <param> (fraction of its track, or its handle) plus px
      // logical pixels along the track. "ctl <param> <frac> [px]": prints the value and how far
      // (logical px) the handle is from where the pointer put it (frac, minus the grab offset px).
      pui::DevTrack tr;
      const uint32_t idx = static_cast<uint32_t>(std::atoi(l.a0.c_str()));
      if (!pui::devTrack(idx, tr)) {
        std::printf("%s frame %d: param %u not shown\n", l.cmd.c_str(), frame, idx);
      } else if (l.cmd == "sdlctl") {
        const double f = l.a1 == "handle" ? tr.norm : std::strtod(l.a1.c_str(), nullptr);
        const double px = l.a2.empty() ? 0.0 : std::strtod(l.a2.c_str(), nullptr);
        const double along = tr.horizontal ? tr.start + f * tr.len + px : tr.start + (1.0 - f) * tr.len - px;
        const double x = tr.horizontal ? along : tr.cross, y = tr.horizontal ? tr.cross : along;
        devSdlPointer = {static_cast<int>(std::lround(x * devDragScale)), static_cast<int>(std::lround(y * devDragScale))};
        SDL_Event e{};
        SDL_Window* w = SDL_GL_GetCurrentWindow();
        e.type = SDL_MOUSEMOTION;
        e.motion.windowID = w ? SDL_GetWindowID(w) : 0;
        e.motion.x = devSdlPointer.first;
        e.motion.y = devSdlPointer.second;
        SDL_PushEvent(&e);
        pointer = ImVec2(-FLT_MAX, -FLT_MAX);
      } else {
        const double want = std::strtod(l.a1.c_str(), nullptr) - (l.a2.empty() ? 0.0 : std::strtod(l.a2.c_str(), nullptr)) / std::max(1.0f, tr.len);
        const double off = (tr.norm - std::clamp(want, 0.0, 1.0)) * tr.len;
        std::printf("ctl frame %d: param %u value %.4f norm %.5f track %.1f px off %.3f px %s\n", frame, idx, tr.value, tr.norm,
                    static_cast<double>(tr.len), off, std::fabs(off) <= 1.0 ? "ok" : "MISS");
      }
      std::fflush(stdout);
    }
    else if (l.cmd == "sdlpt" || l.cmd == "pts") {
      // J-105: point editors (Convolver EQ / IR envelope). "sdlpt <name> <x> <v> [dx dy]": the SDL
      // pointer to x (0..1 of the editor's span) and value v (0..1), plus logical px; "sdlpt <name>
      // pt <i> [dx dy]": to point i. "pts <name>": the points, the drag and the undo position.
      pui::DevPoints g;
      if (!pui::devPoints(l.a0, g)) {
        std::printf("%s frame %d: %s not shown\n", l.cmd.c_str(), frame, l.a0.c_str());
      } else if (l.cmd == "sdlpt") {
        const auto num = [&](size_t i) { return i < l.args.size() ? std::strtod(l.args[i].c_str(), nullptr) : 0.0; };
        double fx = 0, fv = 0;
        if (l.a1 == "pt") {
          const size_t i = static_cast<size_t>(std::max(0, std::atoi(l.a2.c_str())));
          if (i < g.pts.size()) fx = g.pts[i][0] / std::max(1e-9, g.span), fv = g.pts[i][1];
        } else {
          fx = std::strtod(l.a1.c_str(), nullptr), fv = std::strtod(l.a2.c_str(), nullptr);
        }
        const double x = g.x0 + fx * (g.x1 - g.x0) + num(3), y = g.y1 - fv * (g.y1 - g.y0) + num(4);
        devSdlPointer = {static_cast<int>(std::lround(x * devDragScale)), static_cast<int>(std::lround(y * devDragScale))};
        SDL_Event e{};
        SDL_Window* w = SDL_GL_GetCurrentWindow();
        e.type = SDL_MOUSEMOTION;
        e.motion.windowID = w ? SDL_GetWindowID(w) : 0;
        e.motion.x = devSdlPointer.first;
        e.motion.y = devSdlPointer.second;
        SDL_PushEvent(&e);
        pointer = ImVec2(-FLT_MAX, -FLT_MAX);
      } else {
        std::printf("pts frame %d: %s n %zu drag %d hot %d undo %zu\n", frame, l.a0.c_str(), g.pts.size(), g.drag, g.hot,
                    app.doc.history().position());
        for (size_t i = 0; i < g.pts.size(); ++i)
          std::printf("  pt %zu x %.4f v %.4f k %.4f\n", i, g.pts[i][0] / std::max(1e-9, g.span), g.pts[i][1], g.pts[i][2]);
      }
      std::fflush(stdout);
    }
    else if (l.cmd == "playmode") {  // K-4: "playmode song|pattern": the transport's play mode (the L key)
      app.engine.setMode(l.a0 == "pattern" ? PlayMode::Pattern : PlayMode::Song);
    }
    else if (l.cmd == "seekbeat") {  // J-91: the song position to a beat of the metronome ("2.5")
      // "seekbeat <beat> seek": also the set position (as a ruler click), where Stop returns
      app.engine.setPosition(static_cast<Tick>(std::strtod(l.a0.c_str(), nullptr) * static_cast<double>(app.doc.song().beatTicks())), l.a1 == "seek");
    }
    else if (l.cmd == "metro") {  // J-91: metronome state and its glyph's pose (J-97: +1 / -1)
      // (the song plays on in real time: "drawn" is the beat the shown pose was drawn for, "beat" the one now)
      // (and whether it ticked then, from which beat: the one it was switched on in)
      double anchor = 0;
      const bool ticked = topBarMetronomeTicking(&anchor);
      std::printf("metro frame %d: on %d playing %d mirror %.3f beat %.3f drawn %.3f ticking %d from %.0f\n", frame, app.metronome ? 1 : 0,
                  app.engine.playing() ? 1 : 0, static_cast<double>(topBarMetronomeMirror()),
                  app.engine.position() / static_cast<double>(std::max<Tick>(1, app.doc.song().beatTicks())), topBarMetronomeBeat(), ticked ? 1 : 0, anchor);
      std::fflush(stdout);
    }
    else if (l.cmd == "recording") {
      std::printf("recording frame %d: armed %d\n", frame, app.recordArmed ? 1 : 0);
      std::fflush(stdout);
    }
    else if (l.cmd == "wheel") io.AddMouseWheelEvent(0, std::strtof(l.a0.c_str(), nullptr));
    else if (l.cmd == "scroll") io.AddMouseWheelEvent(std::strtof(l.a0.c_str(), nullptr), std::strtof(l.a1.c_str(), nullptr));
    else if (l.cmd == "momentum") devMomentum = {std::strtof(l.a0.c_str(), nullptr), std::strtof(l.a1.c_str(), nullptr), std::atoi(l.a2.c_str())};
    else if (l.cmd == "trace") devTrace = l.a0 != "off";
    else if (l.cmd == "theme" && (l.a0 == "dark" || l.a0 == "light")) {  // "theme dark|light": as View > Dark theme does
      const dispconf::Snapshot before = dispconf::take(app.settings, dispconf::themeKeys());
      theme::modeToSettings(l.a0 == "dark" ? theme::Mode::Dark : theme::Mode::Light, app.settings);
      theme::applyFromSettings(app.settings);
      ::gl::ui::confirmTheme(app, before, "Theme");
      std::fprintf(stderr, "DEV_THEME %s frame %d\n", l.a0.c_str(), ImGui::GetFrameCount());
    }
    else if (l.cmd == "theme" && (l.a0 == "brightness" || l.a0 == "whitetext")) {  // K-177: as Settings > Theme does
      const dispconf::Snapshot before = dispconf::take(app.settings, dispconf::themeKeys());
      theme::Tweaks t = theme::tweaksFromSettings(app.settings);
      if (l.a0 == "brightness") t.brightness = std::clamp(std::strtof(l.a1.c_str(), nullptr), -1.0f, 1.0f);
      else t.whiteText = l.a1 == "on";
      theme::tweaksToSettings(t, app.settings);
      theme::applyFromSettings(app.settings);
      ::gl::ui::confirmTheme(app, before, "Theme");
      std::fprintf(stderr, "DEV_THEME %s %s frame %d\n", l.a0.c_str(), l.a1.c_str(), ImGui::GetFrameCount());
    }
    else if (l.cmd == "setting") {  // J-188: "setting <key> on|off": a switch changed as the Settings dialog does (live)
      if (l.a1 == "on" || l.a1 == "off") app.settings.set(l.a0, l.a1 == "on");
      else if (!l.a1.empty()) app.settings.set(l.a0, std::strtod(l.a1.c_str(), nullptr));  // K-136: "setting <key> <number>"
    } else if (l.cmd == "toasts") app.toasts.clear();  // J-78: "toasts clear": no notification cards (control runs)
    else if (l.cmd == "mod") {
      const bool on = std::atoi(l.a1.c_str()) != 0;
      // (macOS: "ctrl" is the physical Ctrl key, "super" the Cmd key — ImGui swaps them)
      ImGuiKey k = l.a0 == "ctrl" ? ImGuiMod_Ctrl : l.a0 == "shift" ? ImGuiMod_Shift : l.a0 == "super" ? ImGuiMod_Super : ImGuiMod_Alt;
      io.AddKeyEvent(k, on);
    } else if (l.cmd == "key" && l.a0.size() == 1 && l.a0[0] >= 'a' && l.a0[0] <= 'z') {
      ImGuiKey k = static_cast<ImGuiKey>(ImGuiKey_A + (l.a0[0] - 'a'));
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
    } else if (l.cmd == "key" && l.a0 == "space") {
      io.AddKeyEvent(ImGuiKey_Space, true);
      io.AddKeyEvent(ImGuiKey_Space, false);
    } else if (l.cmd == "key" && l.a0.size() >= 2 && l.a0.size() <= 3 && l.a0[0] == 'f' && std::atoi(l.a0.c_str() + 1) >= 1 &&
               std::atoi(l.a0.c_str() + 1) <= 12) {  // K-124: function keys (f2 rename, f12 close all)
      const ImGuiKey k = static_cast<ImGuiKey>(ImGuiKey_F1 + (std::atoi(l.a0.c_str() + 1) - 1));
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
    } else if (l.cmd == "key" && (l.a0 == "pageup" || l.a0 == "pagedown" || l.a0 == "home" || l.a0 == "end" || l.a0 == "kpenter")) {  // K-160
      const ImGuiKey k = l.a0 == "pageup" ? ImGuiKey_PageUp : l.a0 == "pagedown" ? ImGuiKey_PageDown : l.a0 == "home" ? ImGuiKey_Home
                       : l.a0 == "end" ? ImGuiKey_End : ImGuiKey_KeypadEnter;
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
    } else if (l.cmd == "keyhold" && (l.a0 == "up" || l.a0 == "down")) {  // K-160: "keyhold down 1" presses an arrow and keeps it held ("0" lets go)
      io.AddKeyEvent(l.a0 == "up" ? ImGuiKey_UpArrow : ImGuiKey_DownArrow, l.a1 != "0");
    } else if (l.cmd == "key" && l.a0 == "escape") {  // Q-3: cancel a drag
      io.AddKeyEvent(ImGuiKey_Escape, true);
      io.AddKeyEvent(ImGuiKey_Escape, false);
    } else if (l.cmd == "key" && (l.a0 == "left" || l.a0 == "right" || l.a0 == "up" || l.a0 == "down" || l.a0 == "enter" || l.a0 == "delete" || l.a0 == "tab")) {
      // J-141: launcher keyboard checks
      const ImGuiKey k = l.a0 == "left" ? ImGuiKey_LeftArrow : l.a0 == "right" ? ImGuiKey_RightArrow : l.a0 == "up" ? ImGuiKey_UpArrow
                       : l.a0 == "down" ? ImGuiKey_DownArrow : l.a0 == "enter" ? ImGuiKey_Enter : l.a0 == "tab" ? ImGuiKey_Tab : ImGuiKey_Delete;
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
    } else if (l.cmd == "text") {  // J-182: typed characters into the focused text field ("text 2:1:000", quotes group words)
      std::string typed;
      for (size_t i = 0; i < l.args.size(); ++i) typed += (i ? " " : "") + l.args[i];
      io.AddInputCharactersUTF8(typed.c_str());
    } else if (l.cmd == "notes") {  // Q-3: the piano roll's notes (current pattern, selected channel) and selection
      const Song& s = app.doc.song();
      const Pattern* p = s.pattern(app.curPattern);
      const auto* ns = p && p->notes.count(app.selChannel) ? &p->notes.at(app.selChannel) : nullptr;
      std::printf("notes frame %d: %zu notes, %zu selected\n", frame, ns ? ns->size() : size_t(0), app.piano.selected.size());
      if (ns)
        for (size_t i = 0; i < ns->size(); ++i) {
          const Note& n = (*ns)[i];
          std::printf("  note %zu start %lld len %lld pitch %d vel %d pan %d color %d slide %d group %d sel %d bend %zu", i, static_cast<long long>(n.start),
                      static_cast<long long>(n.length), n.pitch, n.velocity, n.pan, n.color, n.slide ? 1 : 0, n.group, app.piano.selected.count(i) ? 1 : 0,
                      n.bend.size());
          for (const auto& b : n.bend) {  // K-25 (K-28: the curve amount when set)
            std::printf(" %.2f:%.2f:%d", static_cast<double>(b.tick), static_cast<double>(b.semis), b.curve);
            if (b.tension != 0.f) std::printf(":%.2f", static_cast<double>(b.tension));
          }
          std::printf("\n");
        }
      std::fflush(stdout);
    } else if (l.cmd == "rackrow") {  // K-22: the selected channel's rack steps in the current pattern (velocities, 0 = off)
      const Pattern* p = app.doc.song().pattern(app.curPattern);
      const auto* row = p && p->steps.count(app.selChannel) ? &p->steps.at(app.selChannel) : nullptr;
      size_t on = 0;
      if (row)
        for (uint8_t v : *row) on += v > 0;
      std::printf("rackrow frame %d: %zu steps, undo %zu, row", frame, on, app.doc.history().position());
      if (row)
        for (uint8_t v : *row) std::printf(" %d", v);
      std::printf("\n");
      std::fflush(stdout);
    } else if (l.cmd == "transport") {
      std::printf("transport frame %d: pos %.0f playing %d seek %.0f mode %d\n", frame, app.engine.position(), app.engine.playing() ? 1 : 0,
                  app.engine.seekPosition(), static_cast<int>(app.engine.mode()));
      std::fflush(stdout);
    } else if (l.cmd == "sdlkey" || l.cmd == "focuslost") {
      // J-109: a real SDL key down / up ("sdlkey z down", "sdlkey z up ctrl") or the
      // main window losing focus (to another application or window).
      SDL_Event e{};
      SDL_Window* w = SDL_GL_GetCurrentWindow();
      if (l.cmd == "focuslost" && !l.a0.empty()) w = SDL_GetWindowFromID(viewports::sdlWindowOf(l.a0));  // J-113: "focuslost <window>"
      if (l.cmd == "focuslost") {
        e.type = SDL_WINDOWEVENT;
        e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        e.window.windowID = w ? SDL_GetWindowID(w) : 0;
      } else {
        const bool down = l.a1 != "up";
        e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.windowID = w ? SDL_GetWindowID(w) : 0;
        e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        e.key.keysym.sym = l.a0.empty() ? 0 : static_cast<SDL_Keycode>(l.a0[0]);
        e.key.keysym.scancode = SDL_GetScancodeFromKey(e.key.keysym.sym);
        if (l.a0.size() == 3 && l.a0[1] == '@')  // ";@z": the key in the Z position types ';' (another layout)
          e.key.keysym.scancode = static_cast<SDL_Scancode>(TypingKeyboard::positionOfKey(l.a0[2]));
        if (l.a0.size() > 1 && l.a0[0] == '#') {  // K-175: "#86", a key by its scan code (keypad -, a modifier...)
          e.key.keysym.scancode = static_cast<SDL_Scancode>(std::clamp(std::atoi(l.a0.c_str() + 1), 0, SDL_NUM_SCANCODES - 1));
          e.key.keysym.sym = SDL_GetKeyFromScancode(e.key.keysym.scancode);
        }
        e.key.keysym.mod = static_cast<Uint16>(devKeyMods(l.a2));  // "ctrl", "super", "shift", "ctrl+shift"...
        e.key.repeat = down && l.a2.find("repeat") != std::string::npos ? 1 : 0;  // K-175: "sdlkey ] down repeat"
      }
      SDL_PushEvent(&e);
    } else if (l.cmd == "typing" && l.a0 == "show") {  // the notes held, octave and velocity
      std::printf("typing notes");
      for (int p : app.typing.heldNotes()) std::printf(" %d", p);
      std::printf(" octave %d velocity %d chord %d\n", app.typing.octave, app.typing.velocity, app.typing.chordMode ? 1 : 0);
      std::fflush(stdout);
    } else if (l.cmd == "typing" && l.a0 == "octave") {  // K-175: the octave, the steps keys made, the pattern, zoom
      std::printf("typing octave %d steps %d pattern %d notes %zu\n", app.typing.octave, app.typing.octaveSteps(),
                  app.doc.song().patternIndex(app.curPattern), app.typing.heldNotes().size());
      std::fflush(stdout);
    } else if (l.cmd == "typing" && l.a0 == "layout") {  // K-113: typing layout <id> (standard, qwertz, azerty, dvorak, colemak, computer)
      setTypingLayout(app, typingLayoutFromId(l.a1));
    } else if (l.cmd == "typing" && l.a0 == "labels") {  // K-113: the layout in use and every typing key's name
      std::printf("typing labels %s:", typingLayoutId(typingLayout));
      for (const char* k = "zsxdcvgbhnjm,l.;/q2w3er5t6y7ui9o0p[]-=\\'"; *k; ++k) std::printf(" %s", typingKeyName(*k).c_str());
      std::printf("\n");
      std::fflush(stdout);
    } else if (l.cmd == "typing") {  // J-109: the typing keyboard on / off
      app.typingKeyboard = l.a0 != "off";
    } else if (l.cmd == "noteon" || l.cmd == "noteoff") {  // J-109: a live note on channel <index> (pads, preview keys)
      const auto& chs = app.doc.song().channels;
      const size_t ci = static_cast<size_t>(std::max(0, std::atoi(l.a0.c_str())));
      if (ci < chs.size()) {
        if (l.cmd == "noteon") app.engine.noteOn(chs[ci].id, std::atoi(l.a1.c_str()), 0.9f);
        else app.engine.noteOff(chs[ci].id, std::atoi(l.a1.c_str()));
      }
    } else if (l.cmd == "stopvia") {  // J-110: the other ways to stop
      if (l.a0 == "menu") menuparity::panicPressed(app);
      else if (l.a0 == "remote") app.runAction(RemoteAction::Stop);
      else if (l.a0 == "button") menuparity::stopButtonPressed(app);
      else menuparity::stopPressed(app);
      std::printf("stopvia frame %d: %s epoch %u\n", frame, l.a0.c_str(), app.engine.stopEpoch());
      std::fflush(stdout);
    } else if (l.cmd == "playvia") {  // J-112
      if (l.a0 == "remote") app.runAction(RemoteAction::PlayPause);
      else menuparity::playPressed(app);
    } else if (l.cmd == "startsong") {
      app.startPlayback();
    } else if (l.cmd == "focus") {
      app.focusWindow = l.args.empty() ? l.a0 : l.args[0];
    } else if (l.cmd == "playstate") {
      const ImGuiContext& g = *ImGui::GetCurrentContext();
      const ImGuiWindow* nav = g.NavWindow ? g.NavWindow->RootWindow : nullptr;
      std::printf("playstate frame %d: playing %d own %d epoch %u focus \"%s\"\n", frame, app.engine.playing() ? 1 : 0,
                  static_cast<int>(app.engine.pluginPlayback().kind), app.engine.stopEpoch(), nav ? nav->Name : "-");
      std::fflush(stdout);
    } else if (l.cmd == "midistate") {  // the system MIDI service: answered, refused or hung
      if (app.midi)
        std::printf("midistate frame %d: state %s attempts %d inputs %zu outputs %zu retryin %.1f age %.2f error \"%s\"\n", frame,
                    midiOpenStateName(app.midi->serviceState()), app.midi->serviceAttempts(), app.midi->inputs.size(),
                    app.midi->outputs.size(), app.midi->serviceRetryIn(), app.midi->serviceAge(), app.midi->serviceError().c_str());
      std::fflush(stdout);
    } else if (l.cmd == "midiretry") {  // the Retry button in Settings > MIDI
      if (app.midi) app.midi->retry();
    } else if (l.cmd == "forget") {
      devsig::forget(l.a0);
    } else if (l.cmd == "epoch") {
      std::printf("epoch frame %d: %u playing %d\n", frame, app.engine.stopEpoch(), app.engine.playing() ? 1 : 0);
      std::fflush(stdout);
    } else if (l.cmd == "previewfile") {  // J-109: the browser's sample preview
      app.previewSample(l.args.empty() ? l.a0 : l.args[0]);  // (a quoted path may hold spaces)
    } else if (l.cmd == "sound") {  // J-109: what is sounding (output peak since the last "sound")
      const float peak = std::max(devSoundPeak, app.engine.takeOutputPeak());
      devSoundPeak = 0;
      std::printf("sound frame %d: peak %.6f live %d previewing %d panics %u playing %d\n", frame, static_cast<double>(peak),
                  app.engine.liveHeld(), app.engine.previewing() ? 1 : 0, app.engine.panicsDone(), app.engine.playing() ? 1 : 0);
      std::fflush(stdout);
    } else if (l.cmd == "audio") {
      browserui::DropPlace place;
      // (J-110: a quoted path may hold spaces)
      const bool q = l.args.size() >= 3;
      place.track = std::atoi((q ? l.args[1] : l.a1).c_str());
      place.tick = static_cast<Tick>(std::atoi((q ? l.args[2] : l.a2).c_str())) * app.doc.song().barTicks();
      browserui::performDrop(app, q ? l.args[0] : l.a0, browse::DropTarget::Playlist, place);
    } else if (l.cmd == "dragenter" && l.args.size() >= 3) {  // J-75
      std::vector<std::string> files(l.args.begin() + 2, l.args.end());
      xdragui::devEnter(std::strtof(l.a0.c_str(), nullptr) * devDragScale, std::strtof(l.a1.c_str(), nullptr) * devDragScale, files);
    } else if (l.cmd == "dragmove") {
      xdragui::devMove(std::strtof(l.a0.c_str(), nullptr) * devDragScale, std::strtof(l.a1.c_str(), nullptr) * devDragScale);
    } else if (l.cmd == "drop") {
      xdragui::devDrop(std::strtof(l.a0.c_str(), nullptr) * devDragScale, std::strtof(l.a1.c_str(), nullptr) * devDragScale, l.args.size() >= 2);
    } else if (l.cmd == "dragleave") {
      xdragui::devLeave();
    } else if (l.cmd == "mixer") {  // J-83: mixer track volumes / pans and the strips pane's scroll to stdout
      const Song& s = app.doc.song();
      std::string selected;  // K-176: the selected tracks (mixer indexes) and the focused one
      for (ObjectId id : mixerSelection(app)) selected += (selected.empty() ? "" : ",") + std::to_string(s.mixerIndex(id));
      std::printf("mixer frame %d: scroll %.1f wheelScrolls %d undo %zu scrollX %.1f sel %s focus %d\n", frame, mixerStripsScrollY(), mixerWheelScrolls() ? 1 : 0,
                  app.doc.history().position(), mixerStripsScrollX(), selected.empty() ? "-" : selected.c_str(), s.mixerIndex(app.selMixer));
      for (size_t i = 0; i < s.mixer.size(); ++i)
        std::printf("  track %zu volume %.4f pan %.4f sep %.4f eqlow %.4f\n", i, static_cast<double>(s.mixer[i].volume), static_cast<double>(s.mixer[i].pan),
                    static_cast<double>(s.mixer[i].stereoSep), static_cast<double>(s.mixer[i].eqGain[0]));
      // The tracks' ids, names and sends, the channels' routing and the strip / menu rects (after the lines above).
      for (size_t i = 0; i < s.mixer.size(); ++i) {
        std::printf("  mtrack %zu id %llu name [%s] sends", i, static_cast<unsigned long long>(s.mixer[i].id), s.mixer[i].name.c_str());
        for (const auto& sd : s.mixer[i].sends) std::printf(" %llu", static_cast<unsigned long long>(sd.to));
        std::printf("\n");
      }
      for (const auto& c : s.channels)
        std::printf("  mchannel [%s] mixer %llu\n", c.name.c_str(), static_cast<unsigned long long>(c.mixerTrack));
      devMenuRects();
      std::fflush(stdout);
    } else if (l.cmd == "rack") {  // J-84: channel rack layout, step scroll and values to stdout
      rackDevDump(app, frame);
    } else if (l.cmd == "groove") {  // the song's swing and the swing preset chip / menu / Groove editor geometry
      grooveui::devCommand(app, frame);
    } else if (l.cmd == "quit") {  // J-103: the window's close request (SDL_QUIT), like the user quitting Gridline
      if (l.a0 == "discard") app.doc.markSaved();  // "quit discard": unsaved changes dropped (as "Don't save")
      SDL_Event q{};
      q.type = SDL_QUIT;
      SDL_PushEvent(&q);
    } else if (l.cmd == "browser") {  // K-17 / K-18: browser rows and file operations (BrowserUi.h)
      browserui::devCommand(app, l.args, frame);
    } else if (l.cmd == "palette") {  // K-93: the command palette (CommandPaletteUi.h): open, close, dump, state, run <id>
      cmdpalette::devCommand(app, l.args, frame);
    } else if (l.cmd == "freeze") {  // K-87: freeze / hibernate (FreezeUi.h): state, freeze, unfreeze, hibernate, wait
      freezeui::devCommand(app, l.args);
    } else if (l.cmd == "drumkit") {  // K-162: drumkit load <path> / report / close (DrumKitUi.h)
      drumkitui::devCommand(app, l.args);
    } else if (l.cmd == "cache") {  // K-45: the unpacked project cache (CacheUi.h)
      cacheui::devCommand(l.args);
    } else if (l.cmd == "collab") {  // J-111: collaboration (CollabUi.h)
      ImVec2 to;
      if (collabui::devCommand(app, l.args, frame, &to)) {  // "collab point": the pointer to an anchor
        pointer = to;
        io.AddMousePosEvent(to.x, to.y);
      }
    } else if (l.cmd == "cursor") {  // K-10
      devVideoCursor = l.a0 != "0";
    } else if (l.cmd == "save") {  // K-9: save <file>
      const std::string to = l.args.empty() ? l.a0 : l.args[0];
      const bool saved = app.saveProject(to);
      exportui::waitFileJob();  // K-47: a zipped project saves in the background; its card follows on the next frame
      std::printf("save frame %d: %s %s\n", frame, saved ? "saved" : "failed", to.c_str());
      std::fflush(stdout);
    } else if (l.cmd == "ppmdiff" && l.args.size() >= 2) {  // K-47: ppmdiff <a> <b>: how many pixels of two "shot" pictures differ
      auto readPpm = [](const std::string& file) {
        std::ifstream f(fs::u8path(file), std::ios::binary);
        std::string magic;
        int w = 0, h = 0, depth = 0;
        f >> magic >> w >> h >> depth;
        f.get();
        std::vector<char> px(static_cast<size_t>(std::max(w, 0)) * static_cast<size_t>(std::max(h, 0)) * 3);
        f.read(px.data(), static_cast<std::streamsize>(px.size()));
        return px;
      };
      const std::vector<char> pa = readPpm(l.args[0]), pb = readPpm(l.args[1]);
      size_t differ = 0;
      for (size_t i = 0; i + 2 < pa.size() && i + 2 < pb.size(); i += 3) differ += pa[i] != pb[i] || pa[i + 1] != pb[i + 1] || pa[i + 2] != pb[i + 2] ? 1 : 0;
      std::printf("ppmdiff pixels %zu differ %zu\n", pa.size() / 3, pa.size() == pb.size() ? differ : pa.size() / 3);
      std::fflush(stdout);
    } else if (l.cmd == "knobs") {  // K-148: every knob and plugin editor control of the last frame (window, id, value, centre)
      std::printf("knobs frame %d undo %zu\n%sknobs end\n", frame, app.doc.history().position(), ui::devKnobReport().c_str());
      std::fflush(stdout);
    } else if (l.cmd == "closepopups") {  // K-148: closes every open popup (a knob's type-in or menu) but modal ones
      ImGui::ClosePopupsExceptModals();
    } else if (l.cmd == "atknob") {  // K-148: atknob <item hex> [dx dy]: the pointer to that knob's centre (last frame) plus logical px
      ImVec2 c;
      std::string win;
      if (ui::devKnobCentre(static_cast<unsigned>(std::strtoul(l.a0.c_str(), nullptr, 16)), c, &win)) {
        c = ImVec2(c.x + std::strtof(l.a1.c_str(), nullptr), c.y + std::strtof(l.a2.c_str(), nullptr));  // (the editor's own pixels)
        if (win == "channelsettings" || win == "fxeditor") c = edzoomui::toScreen(win.c_str(), c);  // K-19: a magnified editor
        pointer = c;
        io.AddMousePosEvent(pointer.x, pointer.y);
      } else {
        std::printf("atknob frame %d: %s not shown\n", frame, l.a0.c_str());
        std::fflush(stdout);
      }
    } else if (l.cmd == "scriptform") {  // the script window's knobs: value and where each one is
      std::printf("scriptform frame %d\n%s", frame, devScriptForm().c_str());
      std::fflush(stdout);
    } else if (l.cmd == "docstate") {  // K-47: is the project modified, and where does it live
      std::printf("docstate modified %d path %s\n", app.doc.modified() ? 1 : 0, app.projectFile().c_str());
      std::fflush(stdout);
    } else if (l.cmd == "glreset") {  // K-47: glreset: the next frame finds the OpenGL context lost; all that follows is real
      glrecovery::simulateLoss();
    } else if (l.cmd == "filejob") {  // K-47: filejob data|bones|zip|clip|sample <target> · cancel · wait · state
      exportui::devFileJob(app, l.args);
    } else if (l.cmd == "scan") {  // K-71: scan start|force [scanner] · folder <format|*> <folder> (a Rescan button) · state (and the longest frame since)
      // K-73: tests name a stand-in scanner (fake plugins); the app's own scanner otherwise.
      const std::string scanner = (l.a0 == "start" || l.a0 == "force") && l.args.size() >= 2 ? l.args[1] : host::selfExecutablePath();
      if (l.a0 == "start" || l.a0 == "force") host::startScan(scanner, l.a0 == "force");
      else if (l.a0 == "folder" && l.args.size() >= 3) host::rescanFolder(host::selfExecutablePath(), l.args[1], l.args[2]);
      const host::ScanStatus st = host::scanStatus();
      // lock-wait: the longest the interface waited for the scan's lock since the last "scan" (a
      // stall caused by the scan, whatever the machine's speed)
      std::printf("scan running %d done %d total %d active %d searching %d failed %d classes %d longest-frame %.1f ms average-frame %.2f ms frames %d second-frame %.1f ms lock-wait %.1f ms\n",
                  st.running ? 1 : 0, st.done, st.total, st.active, st.searching.empty() ? 0 : 1, st.failed, st.classes, devLongestFrameMs,
                  devFrameCount ? devFrameMsSum / devFrameCount : 0.0, devFrameCount, devSecondFrameMs, 1e3 * host::scanReadLongestWait(true));
      std::fflush(stdout);
      devLongestFrameMs = 0;
      devSecondFrameMs = 0;
      devFrameMsSum = 0;
      devFrameCount = 0;
    } else if (l.cmd == "edit") {  // K-47: edit copy|paste <mixer|rack> [mixer index], through the Edit menu's own code
      const std::string what = l.args.empty() ? "" : l.args[0];
      if (l.args.size() > 1 && l.args[1] == "mixer") app.editFocus = App::EditFocus::Mixer;
      if (l.args.size() > 2) {
        const int m = std::atoi(l.args[2].c_str());
        if (m >= 0 && m < static_cast<int>(app.doc.song().mixer.size())) app.selMixer = app.doc.song().mixer[static_cast<size_t>(m)].id;
      }
      if (what == "copy") menus::editCopy(app);
      else if (what == "paste") menus::editPaste(app);
      std::printf("edit %s frame %d\n", what.c_str(), frame);
      std::fflush(stdout);
    } else if (l.cmd == "stress") {  // a seeded stress run (DevStress.h)
      devstress::start(l.args);
    } else if (l.cmd == "export") {  // export: the render job's state to stdout (ExportUi.h)
      std::printf("export frame %d: %s\n", frame, exportui::devState().c_str());
      std::fflush(stdout);
    } else if (l.cmd == "exportstart") {  // exportstart <formats mask> <folder>: a render of the song with the dialog's other settings
      exportui::DevRender r;
      r.formats = std::atoi(l.a0.c_str());
      r.folder = l.args.size() > 1 ? l.args[1] : l.a1;
      exportui::devStart(app, r);
    } else if (l.cmd == "exportcancel") {
      exportui::devCancel();
    } else if (l.cmd == "rmchan") {  // K-9: rmchan <index>, as the rack's Delete
      const auto& chs = app.doc.song().channels;
      const size_t ci = static_cast<size_t>(std::max(0, std::atoi(l.a0.c_str())));
      if (ci < chs.size()) app.doc.removeChannel(chs[ci].id);
    } else {
      secondHalf = true;  // the rest of the commands, in a chain of their own
    }
    if (secondHalf) {
    if (l.cmd == "rackzip") {  // K-136: rackzip <index> [off]: the rack row zipped (a thin strip) or unzipped
      const auto& chs = app.doc.song().channels;
      const size_t ci = static_cast<size_t>(std::max(0, std::atoi(l.a0.c_str())));
      if (ci < chs.size()) {
        if (l.a1 == "off") app.rack.zipped.erase(chs[ci].id);
        else app.rack.zipped.insert(chs[ci].id);
      }
    } else if (l.cmd == "osopen") {  // K-119: osopen <file>...: the open request macOS sends to a running Gridline
      gl::opendocs::post(l.args.empty() ? std::vector<std::string>{l.a0} : l.args);
    } else if (l.cmd == "open") {  // J-167: open <file>: the normal open path (unsaved changes dropped first)
      app.doc.markSaved();
      app.openProject(l.args.empty() ? l.a0 : l.args[0]);
    } else if (l.cmd == "dialogopen") {  // J-167: File > Open, then OK on <file> the frame after the dialog shows
      app.doc.markSaved();
      app.openDialog();
      app.fileDialog.devAccept = l.args.empty() ? l.a0 : l.args[0];
    } else if (l.cmd == "dialogok") {  // J-184: OK with <file> on the file dialog that is (or next) open, whoever opened it
      app.fileDialog.devAccept = l.args.empty() ? l.a0 : l.args[0];
    } else if (l.cmd == "loadchoose") {  // K-42: answers the "which project in this archive" question
      app.projectLoad.choose(std::atoi(l.a0.c_str()));
    } else if (l.cmd == "loadstate") {  // J-167: the project load's phase, counts, log and the open popups to stdout
      projloadui::devReport(app, frame);
    } else if (l.cmd == "loadhold") {  // J-167: loadhold on|off: frames count during a load (off) or wait for it (on)
      devLoadHold = l.a0 != "off";
    } else if (l.cmd == "loadcancel") {  // J-167: the window's Cancel
      app.cancelProjectLoad();
    } else if (l.cmd == "loadfilter") {  // J-167: loadfilter on|off: the log shows only the warnings (the footer's toggle)
      projloadui::setWarningsOnly(l.a0 != "off");
    } else if (l.cmd == "urlimport") {  // J-192: Import from URL (UrlImportUi.h)
      urlimportui::devCommand(app, l.args);
    } else if (l.cmd == "waitfor") {  // J-108
      // (J-111f: a quoted name may hold spaces, "collab-name:Night Drive")
      const std::string& name = l.args.empty() ? l.a0 : l.args[0];
      const std::string& secs = l.args.size() > 1 ? l.args[1] : l.a1;
      devWait = {name, std::max(0.0, std::strtod(secs.c_str(), nullptr)), SDL_GetPerformanceCounter(), true, ImGui::GetTime()};
    } else if (l.cmd == "taptempo") {  // K-48: "taptempo": the shared tapper to stdout; "taptempo action": the controller action; "taptempo bar": where the top bar's groups sit
      if (l.a0 == "action") {
        app.runAction(RemoteAction::TapTempo);
      } else if (l.a0 == "fit") {  // K-132: "taptempo fit <px>": the bar fits into that width (the overflow popup)
        devTopBarFitWidth(static_cast<float>(std::strtod(l.a1.c_str(), nullptr)));
      } else if (l.a0 == "bar") {
        for (const char* n : {"stop", "bar_transport", "bar_tempo", "taptempo", "bar_pos", "bar_pattern", "bar_master", "bar_monitor", "bar_windows", "bar_more"}) {
          float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
          if (topBarButtonRect(n, x0, y0, x1, y1)) std::printf("bar frame %d: %s %.1f %.1f %.1f %.1f\n", frame, n, static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1), static_cast<double>(y1));
        }
      } else {
        std::printf("taptempo frame %d: taps %d bpm %.3f tempo %.3f undo %zu listening %d\n", frame, static_cast<int>(app.tapper.taps.size()), app.tapper.bpm(), app.doc.song().tempo, app.doc.history().position(), app.tapListening() ? 1 : 0);
      }
      std::fflush(stdout);
    } else if (l.cmd == "tempo" && l.a0.empty()) {  // J-198: the song tempo to stdout
      std::printf("tempo frame %d: %.3f undo %zu\n", frame, app.doc.song().tempo, app.doc.history().position());
      std::fflush(stdout);
    } else if (l.cmd == "tempo") {  // J-193: tempo <bpm>: the song tempo, as the tempo display sets it (one undo step)
      app.doc.setParam(songParam(SongParam::Tempo), std::strtod(l.a0.c_str(), nullptr), false);
      app.syncEngine();
    } else if (l.cmd == "undo") {
      app.doc.undo();
      app.syncEngine();
    } else if (l.cmd == "redo") {  // J-213
      app.doc.redo();
      app.syncEngine();
    } else if (l.cmd == "replace") {  // J-213: replace <type>: the open channel's instrument, as "Replace instrument" picks it
      const ObjectId ch = app.settingsChannel != kNoId ? app.settingsChannel : app.selChannel;
      if (app.doc.setChannelPlugin(ch, l.a0)) pui::pluginReplaced(PluginTarget{ch, -1});
      app.syncEngine();
    } else if (l.cmd == "addvia") {  // K-125: addvia <rack|below|replace|droprack|dropplaylist|picker> <type>: an instrument added the way that path adds it
      const std::string type = l.a1;
      if (l.a0 == "rack") menus::addInstrumentPick(app, type);
      else if (l.a0 == "below") menus::addInstrumentPick(app, type, app.selChannel);
      else if (l.a0 == "replace") menus::replaceInstrumentPick(app, app.selChannel, type);
      else if (l.a0 == "droprack") browserui::performPluginDrop(app, type, browse::DropTarget::Rack, {});
      else if (l.a0 == "dropplaylist") browserui::performPluginDrop(app, type, browse::DropTarget::Playlist, {});
      else if (l.a0 == "picker") browserui::pickerAdd(app, type);
      app.syncEngine();
    } else if (l.cmd == "closeplugins") {  // K-125: every plugin window closed, as View > Close plugin windows
      app.closePluginWindows();
    } else if (l.cmd == "instview") {  // K-124 / K-125: the open channel settings and the focused window
      const Channel* c = app.doc.song().channel(app.settingsChannel);
      const ImGuiContext& g = *ImGui::GetCurrentContext();
      const ImGuiWindow* nav = g.NavWindow ? g.NavWindow->RootWindow : nullptr;
      std::printf("instview frame %d: open \"%s\" type %s focus \"%s\" channels %zu undo %zu popup %d text %d\n", frame,
                  c ? c->name.c_str() : "-", c ? c->plugin.type.c_str() : "-", nav ? ui::stableWindowName(nav->Name) : "-",
                  app.doc.song().channels.size(), app.doc.history().position(),
                  ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ? 1 : 0,
                  ImGui::GetIO().WantTextInput ? 1 : 0);
      std::fflush(stdout);
    } else if (l.cmd == "pickpreset") {  // J-213: pickpreset <index>: the open plugin header loads that entry of its preset list
      const bool slot = app.fxEditor.first != kNoId && app.fxEditor.second >= 0;
      pui::devPickPreset(slot ? PluginTarget{app.fxEditor.first, app.fxEditor.second} : PluginTarget{app.settingsChannel, -1}, std::atoi(l.a0.c_str()));
    } else if (l.cmd == "renamechannel") {  // J-213: renamechannel <name>: the open channel, as its name field does
      app.doc.renameChannel(app.settingsChannel, l.args.empty() ? l.a0 : l.args[0]);
    } else if (l.cmd == "pluginview") {  // J-213: what the open plugin window shows: title, plugin, preset selector
      const bool slot = app.fxEditor.first != kNoId && app.fxEditor.second >= 0;
      const PluginTarget t = slot ? PluginTarget{app.fxEditor.first, app.fxEditor.second} : PluginTarget{app.settingsChannel, -1};
      const Song& s = app.doc.song();
      const PluginState* st = t.state(s);
      const PluginInfo* info = st ? findPlugin(st->type) : nullptr;
      const Channel* c = slot ? nullptr : s.channel(t.owner);
      std::string headerType;
      bool onB = false;
      const std::string preset = pui::devHeaderPreset(t, &headerType, &onB);
      std::printf("pluginview frame %d: window \"%s\" plugin %s \"%s\" header %s preset \"%s\" ab %s undo %zu \"%s\"\n", frame,
                  c ? c->name.c_str() : "-", st ? st->type.c_str() : "-", info ? info->name.c_str() : "-", headerType.empty() ? "-" : headerType.c_str(),
                  preset.c_str(), onB ? "B" : "A", app.doc.history().position(), app.doc.history().undoLabel().c_str());
      std::fflush(stdout);
    } else if (l.cmd == "atparam" || l.cmd == "plug") {
      // "atparam <k> [dx dy]": the scripted pointer to the centre of the last drawn control of
      // parameter k of the open plugin editor (the --plugin / --fx / --chan target), plus logical px.
      // "plug [k...]": the undo position, the labels around it, a hash of the plugin's data and
      // the values of parameters k (undo tests for plugin editors).
      const bool slot = app.fxEditor.first != kNoId && app.fxEditor.second >= 0;
      auto pidOf = [&](const std::string& k) {
        const int i = std::atoi(k.c_str());
        return slot ? slotParam(app.fxEditor.first, app.fxEditor.second, i) : channelPluginParam(app.settingsChannel, i);
      };
      if (l.cmd == "atparam") {
        pui::DevItem it;
        if (!pui::devItem(pidOf(l.a0), it)) {
          std::printf("atparam frame %d: param %s not shown\n", frame, l.a0.c_str());
        } else {
          pointer = ImVec2((it.x0 + it.x1) * 0.5f + std::strtof(l.a1.c_str(), nullptr), (it.y0 + it.y1) * 0.5f + std::strtof(l.a2.c_str(), nullptr));
          pointer = edzoomui::toScreen(slot ? "fxeditor" : "channelsettings", pointer);  // K-19: where it shows in a magnified editor
          io.AddMousePosEvent(pointer.x, pointer.y);
          std::printf("atparam frame %d: param %s at %.1f %.1f\n", frame, l.a0.c_str(), static_cast<double>(pointer.x), static_cast<double>(pointer.y));
        }
      } else {
        const Song& s = app.doc.song();
        const Channel* ch = slot ? nullptr : s.channel(app.settingsChannel);
        const MixerTrack* mt = slot ? s.mixerTrack(app.fxEditor.first) : nullptr;
        const PluginState* st = ch ? &ch->plugin : mt ? &mt->slots[static_cast<size_t>(app.fxEditor.second)].plugin : nullptr;
        const UndoStack& h = app.doc.history();
        std::printf("plug frame %d: undo %zu of %zu undoLabel \"%s\" redoLabel \"%s\" data %016llx\n", frame, h.position(), h.size(),
                    h.undoLabel().c_str(), h.redoLabel().c_str(),
                    static_cast<unsigned long long>(st ? std::hash<std::string>{}(st->data) : 0));
        for (const std::string& k : l.args)
          std::printf("  param %s %.6f\n", k.c_str(), getParam(s, pidOf(k)).value_or(-1.0));
      }
      std::fflush(stdout);
    } else if (l.cmd == "channels") {
      const Song& s = app.doc.song();
      std::printf("channels frame %d: %zu channels, %zu patterns, undo %zu\n", frame, s.channels.size(), s.patterns.size(), app.doc.history().position());
      const Pattern* pat = s.pattern(app.curPattern);
      for (const auto& c : s.channels) {
        // J-97: routing, volume, pan and the current pattern's steps / notes (kept by drop-to-replace).
        int steps = 0;
        size_t notes = 0;
        if (pat) {
          if (auto it = pat->steps.find(c.id); it != pat->steps.end())
            for (uint8_t v : it->second) steps += v > 0;
          if (auto it = pat->notes.find(c.id); it != pat->notes.end()) notes = it->second.size();
        }
        std::printf("  channel %u %s type %s data %s mixer %d vol %.3f pan %.3f steps %d notes %zu\n", static_cast<unsigned>(c.id), c.name.c_str(),
                    c.plugin.type.c_str(), std::filesystem::path(c.plugin.data).filename().string().c_str(), s.mixerIndex(c.mixerTrack),
                    static_cast<double>(c.gain), static_cast<double>(c.pan), steps, notes);
      }
      std::fflush(stdout);
    } else if (l.cmd == "routes") {  // J-195: each channel's mixer track (index and name), the mixer size, the selected track
      const Song& s = app.doc.song();
      std::printf("routes frame %d: %zu mixer tracks, selected %d, undo %zu \"%s\"\n", frame, s.mixer.size(), s.mixerIndex(app.selMixer),
                  app.doc.history().position(), app.doc.history().undoLabel().c_str());
      for (const auto& c : s.channels) {
        const int mi = std::max(0, s.mixerIndex(c.mixerTrack));
        std::printf("  route %s -> %d \"%s\"\n", c.name.c_str(), mi, mixerTrackName(s, static_cast<size_t>(mi)).c_str());
      }
      std::fflush(stdout);
    } else if (l.cmd == "edzoom") {  // K-19: edzoom <window key> (fxeditor | channelsettings): the editor zoom drawn last frame
      edzoomui::devDump(l.a0.c_str());
    } else if (l.cmd == "edzoomset") {  // K-19: edzoomset <plugin type> <zoom, 0 = automatic>: as the Editor size menu sets it
      app.settings.set("ui.editorZoom", edzoom::withZoom(app.settings.getString("ui.editorZoom"), l.a0, std::strtof(l.a1.c_str(), nullptr)));
    } else if (l.cmd == "windows") {  // J-111h: windows [frames]
      devWindowsLeft = std::max(1, std::atoi(l.a0.c_str()));
    } else if (l.cmd == "shot") {
      devShotPending = l.args.empty() ? l.a0 : l.args[0];  // J-97: a quoted path may hold spaces
    } else if (l.cmd == "mknotes" || l.cmd == "notes" || l.cmd == "selnotes" || l.cmd == "lane" || l.cmd == "pianosnap" || l.cmd == "bendline" ||
               l.cmd == "alane" || l.cmd == "alanetarget" || l.cmd == "alaneshape" || l.cmd == "alanesimplify" || l.cmd == "pianotool") {
      devPianoCommand(app, l, frame);
    } else if (l.cmd == "launcher") {  // J-141: launcher [home|back|close]: the launcher's state and tiles to stdout
      if (l.a0 == "home") app.home.showHome(app.now);
      else if (l.a0 == "back") app.home.back(app.now);
      else if (l.a0 == "close") app.closeProject();
      else launcherui::devReport(app);
    } else if (l.cmd == "dump") {
      const Song& s = app.doc.song();
      std::printf("dump frame %d: %zu clips, %zu selected, undo %zu\n", frame, s.playlist.size(), app.playlist.selected.size(),
                  app.doc.history().position());
      for (const auto& c : s.playlist)
        std::printf("  clip %u kind %d track %d start %lld len %lld offset %lld audioStart %.6f muted %d pattern %u group %u sel %d"
                    " source %u audioEnd %.6f fadeIn %.6f fadeOut %.6f gain %.4f pitch %.2f stretch %.4f reverse %d syncBpm %.4f tempoLock %d loop %d"
                    " loopStart %lld loopLength %lld warp %zu\n",
                    static_cast<unsigned>(c.id), static_cast<int>(c.kind), c.track, static_cast<long long>(c.start),
                    static_cast<long long>(s.clipLength(c)), static_cast<long long>(c.offset), c.audioStart, c.muted ? 1 : 0,
                    static_cast<unsigned>(c.pattern), static_cast<unsigned>(c.group), app.playlist.selected.count(c.id) ? 1 : 0,
                    static_cast<unsigned>(c.source), c.audioEnd, c.fadeIn, c.fadeOut, static_cast<double>(c.gain),
                    static_cast<double>(c.pitch), c.stretch, c.reverse ? 1 : 0, c.syncBpm, c.tempoLock ? 1 : 0, c.loop ? 1 : 0,
                    static_cast<long long>(c.loopStart), static_cast<long long>(c.loopLength), c.warp.size());  // (K-114: loop; K-140: its own loop; K-85: warp markers)
      std::fflush(stdout);
    } else if (l.cmd == "builds") {  // J-196: program builds and clip level patches so far, renders waiting for an edit to end
      std::printf("builds frame %d: builds %llu patches %llu deferred %d voices made %llu kept %llu\n", frame,
                  static_cast<unsigned long long>(app.builder.builds()), static_cast<unsigned long long>(app.builder.patches()),
                  app.builder.deferredRenders() ? 1 : 0, static_cast<unsigned long long>(app.builder.voicesMade()),
                  static_cast<unsigned long long>(app.builder.voicesReused()));
      std::fflush(stdout);
    } else if (l.cmd == "autoedit") {  // J-173: what the automation editor shows (a playlist clip or a pattern's clip)
      ObjectId aid = kNoId, pid = kNoId;
      ParamId tgt{};
      const bool open = automationEditorTarget(&aid, &pid, &tgt);
      const AutomationEditorView& av = automationEditorView();
      std::printf("autoedit frame %d: open %d automation %u pattern %u target %u %u view sx %.3f pxPerTick %.6f hthumb %.3f %.3f y %.2f grid %.2f %.2f\n",
                  frame, open ? 1 : 0, static_cast<unsigned>(aid), static_cast<unsigned>(pid), static_cast<unsigned>(tgt.owner),
                  static_cast<unsigned>(tgt.index), av.scrollX, av.pxPerTick, av.thumb0, av.thumb1, av.barY, av.gridX0, av.gridX1);
      std::fflush(stdout);
    } else if (l.cmd == "selpattern") {  // J-173: selpattern <id>: the current pattern
      app.selectPattern(static_cast<ObjectId>(std::atoi(l.a0.c_str())));
    } else if (l.cmd == "patauto") {  // J-173: every pattern's automation clips to stdout
      const Song& s = app.doc.song();
      std::printf("patauto frame %d: undo %zu\n", frame, app.doc.history().position());
      for (const auto& p : s.patterns)
        for (const auto& c : p.automation)
          std::printf("  pa %u %u %u name \"%s\" points %zu\n", static_cast<unsigned>(p.id), static_cast<unsigned>(c.owner),
                      static_cast<unsigned>(c.index), c.name.c_str(), c.points.size());
      std::printf("  automations %zu playlist %zu\n", s.automations.size(), s.playlist.size());
      std::fflush(stdout);
    } else if (l.cmd == "plgeom") {  // J-118: the playlist's geometry (Windows.h PlaylistGeometry)
      const PlaylistGeometry& g = playlistGeometry();
      std::printf("plgeom frame %d: valid %d x0 %.3f pxPerTick %.6f tabs %.1f %.1f %.1f tabY %.1f\n", frame, g.valid ? 1 : 0,
                  static_cast<double>(g.x0), static_cast<double>(g.pxPerTick), static_cast<double>(g.tabX[0]),
                  static_cast<double>(g.tabX[1]), static_cast<double>(g.tabX[2]), static_cast<double>(g.tabY));
      std::printf("  plus %.1f %.1f droptrack %d patterns %zu current %u undo %zu \"%s\"\n", static_cast<double>(g.plusX),
                  static_cast<double>(g.plusY), g.dropTrack, app.doc.song().patterns.size(), static_cast<unsigned>(app.curPattern),
                  app.doc.history().position(), app.doc.history().undoLabel().c_str());  // J-208
      std::printf("  view scrollY %.3f trackHeight %.4f grid %.1f %.1f %.1f vthumb %.2f %.2f x %.1f hthumb %.2f %.2f y %.1f\n",
                  static_cast<double>(g.scrollY), static_cast<double>(g.trackHeight), static_cast<double>(g.gridX0), static_cast<double>(g.gridY0),
                  static_cast<double>(g.gridY1), static_cast<double>(g.vThumb0), static_cast<double>(g.vThumb1), static_cast<double>(g.vBarX),
                  static_cast<double>(g.hThumb0), static_cast<double>(g.hThumb1), static_cast<double>(g.hBarY));  // K-24
      std::printf("  head settings %u renaming %d\n", static_cast<unsigned>(app.settingsChannel), g.renaming);  // header double-clicks
      std::printf("  loopbadge %d x %.1f y %.1f\n", g.loopBadge ? 1 : 0, static_cast<double>(g.loopBadgeX), static_cast<double>(g.loopBadgeY));  // K-140
      for (size_t i = 0; i < g.trackTop.size(); ++i) {
        const Song& s = app.doc.song();
        const PlaylistTrack* t = i < s.tracks.size() ? &s.tracks[i] : nullptr;  // J-208: the mode, the linked channel, the name
        int fx = 0;
        if (const MixerTrack* mt = trackmodes::linkedMixer(s, static_cast<int>(i)))
          for (const auto& sl : mt->slots) fx += sl.plugin.empty() ? 0 : 1;
        const Channel* lc = trackmodes::linkedChannel(s, static_cast<int>(i));
        std::printf("  track %zu top %.1f h %.1f mode %d channel %u type %s fx %d name \"%s\"\n", i, static_cast<double>(g.trackTop[i]),
                    static_cast<double>(g.trackH[i]), t ? static_cast<int>(t->mode) : 0, t ? static_cast<unsigned>(t->channel) : 0u,
                    lc ? lc->plugin.type.c_str() : "-", fx, t ? t->name.c_str() : "");
      }
      {  // K-12: the track "+" (button and its neighbours on the hovered header), its menu rows, the piano roll
        const trackplusui::DevGeom& tp = trackplusui::devGeom();
        std::printf("  tplus on %d track %d box %.1f %.1f %.1f %.1f name %.1f %.1f %.1f %.1f handle %.1f %.1f led %.1f %.1f ctl %.1f %.1f icon %.1f"
                    " menu %d piano %d pattern %u channel %u start %d patterns %zu channels %zu\n",
                    tp.on ? 1 : 0, tp.track, static_cast<double>(tp.x0), static_cast<double>(tp.y0), static_cast<double>(tp.x1),
                    static_cast<double>(tp.y1), static_cast<double>(tp.nameX0), static_cast<double>(tp.nameX1), static_cast<double>(tp.nameY0),
                    static_cast<double>(tp.nameY1), static_cast<double>(tp.handleX0), static_cast<double>(tp.handleX1), static_cast<double>(tp.ledX0),
                    static_cast<double>(tp.ledX1), static_cast<double>(tp.ctlX0), static_cast<double>(tp.ctlX1), static_cast<double>(tp.iconX1),
                    tp.menu, app.showPiano ? 1 : 0, static_cast<unsigned>(app.curPattern), static_cast<unsigned>(app.selChannel),
                    app.settings.getBool("ui.trackPlusPattern") ? 1 : 0, app.doc.song().patterns.size(), app.doc.song().channels.size());
        for (const auto& r : tp.rows) std::printf("  tprow %s x %.1f y %.1f\n", r.key.c_str(), static_cast<double>(r.x), static_cast<double>(r.y));
      }
      for (const auto& r : g.rows)
        std::printf("  row %u x %.1f y %.1f\n", r.id, static_cast<double>(r.x), static_cast<double>(r.y));
      for (const auto& c : g.clips)  // J-142: clip, title strip, menu arrow and name box spans (0 0 when not drawn)
        std::printf("  clip %u x %.1f %.1f y %.1f %.1f head %.1f menu %.1f %.1f name %.1f %.1f\n", c.id, static_cast<double>(c.g.x0),
                    static_cast<double>(c.g.x1), static_cast<double>(c.g.y0), static_cast<double>(c.g.y1), static_cast<double>(c.g.headH),
                    static_cast<double>(c.g.hasMenu ? c.g.menuX0 : 0), static_cast<double>(c.g.hasMenu ? c.g.menuX1 : 0),
                    static_cast<double>(c.g.hasName ? c.g.nameX0 : 0), static_cast<double>(c.g.hasName ? c.g.nameX1 : 0));
      for (const auto& c : g.curves)  // J-173: pattern automation curves on pattern clips
        std::printf("  pcurve %u %u %u x %.1f y %.1f\n", c.clip, c.owner, c.index, static_cast<double>(c.x), static_cast<double>(c.y));
      std::fflush(stdout);
    }
    }  // (end of the second half)
  }
}

}  // namespace

#if defined(__GLIBC__)
// K-9, GRIDLINE_DEV_BACKTRACE (the yabridge test): an uncaught exception, or SIGUSR2 for a process
// that hangs, prints where the thread is. Development only; the process reports itself.
static void devBacktrace(const char* why) {
  char name[32] = "";
  pthread_getname_np(pthread_self(), name, sizeof name);
  std::fprintf(stderr, "backtrace (%s) in thread %s\n", why, name);
  void* frames[64];
  backtrace_symbols_fd(frames, backtrace(frames, 64), 2);
  std::fprintf(stderr, "backtrace end\n");
}
static void devInstallBacktrace() {
  if (!std::getenv("GRIDLINE_DEV_BACKTRACE")) return;
  std::set_terminate([] {
    devBacktrace("uncaught exception");
    std::abort();
  });
  std::signal(SIGUSR2, [](int) { devBacktrace("asked"); });
}
#else
static void devInstallBacktrace() {}
#endif

// K-133: a lost OpenGL context while one window draws alone (the splash, the crash pop-up): what the
// main window's recovery does (K-47), without what exists only later (plugin windows, the detached
// windows' contexts). False: no new context could be had (ImGui then has no renderer).
static bool rebuildLoneContext(SDL_Window* window, SDL_GLContext& gl, const char* where, const char* why) {
  crash::crumb("graphics reset", why);
  logging::warning("graphics", std::string("The OpenGL context was lost (") + where + "): " + why);
  const int interval = SDL_GL_GetSwapInterval();
  glrecovery::waitForResetToEnd(10000);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  SDL_GL_MakeCurrent(window, nullptr);
  SDL_GL_DeleteContext(gl);
  gl = glrecovery::recreateContext(window, 10000);
  if (!gl) {
    std::fprintf(stderr, "[graphics] no new context (%s)\n", where);
    return false;
  }
  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(interval);
  glrecovery::attach();
  crash::setGpu(reinterpret_cast<const char*>(glGetString(GL_VENDOR)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));
  ImGui_ImplSDL2_InitForOpenGL(window, gl);
  ImGui_ImplOpenGL3_Init(glrecovery::glsl());
  if (std::getenv("GRIDLINE_DEV_NO_VTX_OFFSET")) ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
  glepoch::next();
  glrecovery::restoreTextures();
  std::fprintf(stderr, "[graphics] context rebuilt (%s)\n", where);
  return true;
}

// K-133 dev checks: GRIDLINE_DEV_GL_LOSS_EARLY=splash|dialog loses the context once, on the third
// frame of the splash or of the crash pop-up; all that follows is the real path.
static void devEarlyLoss(const char* where, int frame) {
  static bool done = false;
  const char* e = std::getenv("GRIDLINE_DEV_GL_LOSS_EARLY");
  if (done || !e || std::strcmp(e, where) != 0 || frame != 2) return;
  done = true;
  glrecovery::simulateLoss();
}

// K-39: the pop-up as a small process of its own, right after a crash or during a freeze (see
// core/Crash.h). Only SDL, OpenGL, ImGui and the theme: nothing that may be what failed.
static int runCrashDialog(SDL_Window* window, SDL_GLContext gl, const std::string& report, const std::string& session, const std::string& shot,
                          int shotFrames, float forcedScale, ImFont* fBold, ImFont* fMono, const std::string& assets) {
  Settings st;
  st.load(Settings::defaultPath());
  theme::applyFromSettings(st);
  if (!assets.empty()) i18n::setLanguage(st.getString("general.language"), (fs::path(assets) / "i18n").string());
  const std::string configDir = fs::u8path(Settings::defaultPath()).parent_path().u8string();
  const float uiScale = forcedScale > 0 ? forcedScale : uiscale::effectiveScale(st.getNumber("ui.scale"), false, 1.0f, 0, 0);
  int w = 0, h = 0;
  crashui::dialogSize(w, h);
  w = static_cast<int>(static_cast<float>(w) * uiScale);
  h = static_cast<int>(static_cast<float>(h) * uiScale);
  OffscreenTarget offscreen;
  const bool useOffscreen = !shot.empty() && offscreen.create(w, h);
  if (shot.empty()) {
    SDL_SetWindowBordered(window, SDL_TRUE);
    SDL_SetWindowResizable(window, SDL_TRUE);
    SDL_SetWindowMinimumSize(window, 480, 400);
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetWindowTitle(window, "Gridline");
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
  }
  crashui::DialogState* state = crashui::dialogOpen(configDir, report, session);
  crashui::DialogFonts fonts{fBold, fMono};
  ImGuiIO& io = ImGui::GetIO();
  for (int frameNo = 0;; ++frameNo) {
    SDL_Event e;
    bool closing = false;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)) closing = true;
    }
    int winW = w, winH = h, fbW = w, fbH = h;
    if (!useOffscreen) {
      SDL_GetWindowSize(window, &winW, &winH);
      SDL_GL_GetDrawableSize(window, &fbW, &fbH);
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    io.DisplaySize = ImVec2(static_cast<float>(winW) / uiScale, static_cast<float>(winH) / uiScale);
    io.DisplayFramebufferScale = ImVec2(static_cast<float>(fbW) / io.DisplaySize.x, static_cast<float>(fbH) / io.DisplaySize.y);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiInputEvent& ev : g.InputEventsQueue)
      if (ev.Type == ImGuiInputEventType_MousePos && ev.MousePos.PosX > -FLT_MAX * 0.5f && ev.EventId > 0) {
        ev.MousePos.PosX /= uiScale;
        ev.MousePos.PosY /= uiScale;
        ev.EventId = 0;  // done once
      }
    ImGui::NewFrame();
    const bool more = crashui::dialogFrame(state, fonts);
    ImGui::Render();
    ImGui::UpdatePlatformWindows();
    if (useOffscreen) offscreen.use(true);
    glViewport(0, 0, fbW, fbH);
    const ImU32 bg = theme::pal().surface900;
    glClearColor(static_cast<float>(bg & 255) / 255.0f, static_cast<float>((bg >> 8) & 255) / 255.0f, static_cast<float>((bg >> 16) & 255) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!shot.empty() && frameNo >= shotFrames) {
      writePpm(shot.c_str(), fbW, fbH);
      closing = true;
    }
    if (useOffscreen) offscreen.use(false);
    devEarlyLoss("dialog", frameNo);
    const char* lost = glrecovery::poll();  // K-133: as the main window does (K-47)
    if (!lost) SDL_GL_SwapWindow(window);
    if (lost && !closing) {
      if (!rebuildLoneContext(window, gl, "crash pop-up", lost)) {
        // Nothing can be drawn any more. The report stays unread, so the next start shows it and
        // offers its recovery copy; a box from the system says so now.
        crashui::dialogClose(state);
        std::fprintf(stderr, "Gridline's crash pop-up lost its graphics driver; the report is shown at the next start.\n");
        if (shot.empty())
          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gridline",
                                   i18n::tr("Gridline stopped unexpectedly, and the graphics driver restarted while its report was shown. "
                                            "The report and any recovered work are shown the next time Gridline starts."),
                                   nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 3;
      }
      if (useOffscreen) offscreen.create(w, h);
      continue;  // the frame again, on the new context
    }
    if (!more || closing) break;
  }
  crashui::dialogClose(state);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

// K-121: one Gridline per user on Linux, the BSDs and Windows (core/SingleInstance.h). A launch
// with only files (the file manager, Open with) or with nothing (the menu, a launcher icon) hands
// them to the Gridline that is running and exits; that one opens them as macOS does (the
// unsaved-changes prompt first) and comes to the front. Without files the running one only
// comes forward: each Gridline has the sound card, the MIDI ports and the window layout to
// itself, so a second one would compete for them. GRIDLINE_SINGLE_INSTANCE=0 starts every launch
// on its own. Runs with options (tests, tools, --hidden) never meet the user's Gridline; the tests
// use their own endpoint (GRIDLINE_TEST_INSTANCE_ENDPOINT; K-153: visible runs too, for the Wayland
// window checks).
namespace {
struct InstancePlan {
  std::string endpoint;            // "" = this launch runs on its own
  std::vector<std::string> files;  // absolute paths to hand over
  bool claimOnly = false;          // a restart after a crash: takes the endpoint if free, never hands over
};

InstancePlan planInstance(int argc, char** argv, bool hiddenRun) {
  InstancePlan p;
  if (const char* o = std::getenv("GRIDLINE_SINGLE_INSTANCE"); o && std::string(o) == "0") return p;
  const char* test = std::getenv("GRIDLINE_TEST_INSTANCE_ENDPOINT");
  const bool testRun = test && *test;
  if (hiddenRun && !testRun) return p;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("-psn_", 0) == 0) continue;  // (macOS process serial)
    if (a == "--crash-recover") {
      ++i;
      p.claimOnly = true;
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      if (!testRun) return InstancePlan{};  // a run with options
      for (const char* v : {"--dev-input", "--dev-input-live", "--size", "--ui-scale", "--frames", "--screenshot"})
        if (a == v) ++i;
      continue;
    }
    std::error_code ec;
    const fs::path abs = a.find("://") != std::string::npos ? fs::u8path(a) : fs::absolute(fs::u8path(a), ec);
    p.files.push_back(ec ? a : abs.u8string());
  }
  p.endpoint = testRun ? gl::single::testEndpoint(test)
                       : gl::single::defaultEndpoint(fs::u8path(Settings::defaultPath()).parent_path().u8string());
  return p;
}
}  // namespace

int main(int argc, char** argv) {
  devInstallBacktrace();
#if defined(_WIN32)
  // A GUI program has no console: --version / --help / errors go to the one it was started from.
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    std::FILE* f = nullptr;
    if (_fileno(stdout) < 0) freopen_s(&f, "CONOUT$", "w", stdout);
    if (_fileno(stderr) < 0) freopen_s(&f, "CONOUT$", "w", stderr);
  }
  SDL_SetMainReady();  // SDL_MAIN_HANDLED: this is the real entry (WinMain below)
#endif
  // Child processes (see host/PluginScan.h): plugin scanner and plugin
  // sandbox; no window, no audio.
  if (int rc = host::childMain(argc, argv); rc >= 0) return rc;
  if (argc >= 2 && std::string(argv[1]) == "--version") {
    std::printf("Gridline " GRIDLINE_VERSION "\n");
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "--check-updates")  // J-89: result on stdout, no window
    return http::checkCommand(argc >= 3 && std::string(argv[2]) == "--beta");
  // K-39: the separate process that outlives a crash; no window, no audio.
  if (argc >= 2 && std::string(argv[1]) == "--crash-sentinel") return crash::sentinelMain(argc, argv);
  std::string crashDialogReport, crashDialogSession, crashDialogShot;
  int crashDialogFrames = 6;
  float crashDialogScale = 0.0f;
  if (argc >= 3 && std::string(argv[1]) == "--crash-dialog") {
    crashDialogReport = argv[2];
    for (int i = 3; i + 1 < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--crash-session") crashDialogSession = argv[++i];
      else if (a == "--screenshot") crashDialogShot = argv[++i];
      else if (a == "--frames") crashDialogFrames = std::atoi(argv[++i]);
      else if (a == "--ui-scale") crashDialogScale = static_cast<float>(std::atof(argv[++i]));
    }
  }
  bool hiddenRun = false;
  for (int i = 1; i < argc; ++i) hiddenRun = hiddenRun || std::string(argv[i]) == "--hidden";
  // Forced failures for the crash tests: a --hidden run with the variable set, and nothing else.
  std::string crashForce;
  int crashForceFrame = 5;
  if (const char* f = std::getenv("GRIDLINE_CRASH_TEST_FORCE"); f && *f && hiddenRun) {
    crashForce = f;
    if (const size_t c = crashForce.rfind('@'); c != std::string::npos) {
      crashForceFrame = std::atoi(crashForce.c_str() + c + 1);
      crashForce.resize(c);
    }
  }
  // K-121: hand the files to the Gridline that is running, or become the one later launches find.
  gl::single::Server instance;
  if (const InstancePlan plan = crashDialogReport.empty() ? planInstance(argc, argv, hiddenRun) : InstancePlan{}; !plan.endpoint.empty()) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::string err;
      const gl::single::Server::Claim c = instance.claim(plan.endpoint, &err);
      if (c == gl::single::Server::Claim::Taken) {
        if (hiddenRun) std::printf("single instance: listening\n");
        break;
      }
      if (c == gl::single::Server::Claim::Failed) {
        std::fprintf(stderr, "[instance] %s: this Gridline runs on its own\n", err.c_str());
        break;
      }
      if (plan.claimOnly) break;
      const gl::single::Handover h = gl::single::handOver(plan.endpoint, plan.files, &err);
      if (h == gl::single::Handover::Sent) {
        if (hiddenRun) std::printf("single instance: handed over %zu file(s)\n", plan.files.size());
        std::fflush(stdout);
        return 0;
      }
      if (h == gl::single::Handover::Failed) {
        std::fprintf(stderr, "[instance] the running Gridline did not answer (%s): starting another one\n", err.c_str());
        break;
      }
      // NoServer: it has just quit; this launch takes its place
    }
    std::fflush(stdout);
    instance.start({});  // (the interface thread takes the requests every frame)
  }
  if (crashDialogReport.empty() && !std::getenv("GRIDLINE_NO_CRASH_HANDLER")) {
    crash::Config cc;
    cc.configDir = fs::u8path(Settings::defaultPath()).parent_path().u8string();
    cc.sentinel = !hiddenRun || std::getenv("GRIDLINE_CRASH_TEST_SENTINEL");
    cc.devBacktrace = std::getenv("GRIDLINE_DEV_BACKTRACE") != nullptr;
    if (const char* t = std::getenv("GRIDLINE_CRASH_TEST_FREEZE_SECONDS"); t && hiddenRun) cc.frozenAfterSeconds = std::atoi(t);
    crash::install(cc);
  }
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  gl::opendocs::setCommandLine(argc, argv);  // K-119: (AppKit repeats them as an open request)
  appicon::prepareIdentity();  // K-14: X11 WM_CLASS / Wayland app_id = the desktop entry id
  {  // X11 where an X server answers: what plugin interfaces can open on (K-153: unless the setting says Wayland)
    Settings early;
    early.load(Settings::defaultPath());
    videodriver::prepare(early.getString("ui.windowSystem"));
  }
  bool sdlStarted = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0;
  if (!sdlStarted && videodriver::dropRequest()) sdlStarted = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0;
  videodriver::finish();
  if (const std::string sys = chromeos::describe(chromeos::current()); !sys.empty()) std::fprintf(stderr, "[system] %s\n", sys.c_str());  // K-75
  if (!sdlStarted) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    crash::markCleanExit();  // said so; the pop-up has nothing to add
    return 1;
  }
  for (int i = 1; i + 1 < argc; ++i)  // K-119 dev check: the open request macOS sends when a file is double-clicked
    if (std::string(argv[i]) == "--dev-os-open") gl::opendocs::post({argv[i + 1]});
#if defined(__APPLE__)
  const char* glsl = "#version 150";
#else
  const char* glsl = "#version 130";
#endif
  const auto setGlAttributes = [] {
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  };
  setGlAttributes();
  // Start as a compact borderless splash; becomes the main window once loaded.
  // J-78: --hidden (automated off-screen runs with --size / --screenshot): the
  // window is never shown on the user's desktop.
  bool startHidden = false;
  for (int i = 1; i < argc; ++i) startHidden = startHidden || std::string(argv[i]) == "--hidden";
  // J-216: a run started with any "--" option (tests, screenshots, audits, the dev layout options)
  // never writes the saved window layout; only a plain start (no options, or a project to open) does.
  bool devOptions = false;
  // (K-39: a restart that recovers a project is an ordinary start.)
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--crash-recover") {
      ++i;
      continue;
    }
    devOptions = devOptions || std::string(argv[i]).rfind("--", 0) == 0;
  }
  const auto createSplashWindow = [&] { return winrole::createSplash(startHidden); };
  SDL_Window* window = createSplashWindow();
  // An X server that answers but cannot give an OpenGL window: SDL's own choice gets a try.
  if (!window && videodriver::dropRequest()) {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
      setGlAttributes();
      window = createSplashWindow();
    }
    videodriver::finish();
  }
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    crash::markCleanExit();
    return 1;
  }
  SDL_GLContext gl = glrecovery::createContext(window);  // K-47: robust where the driver offers it
  if (!gl) {
    std::fprintf(stderr, "No OpenGL context (3.0 core, 3.0 compatibility or 2.1): %s\n", SDL_GetError());
    crash::markCleanExit();
    return 1;
  }
  SDL_GL_MakeCurrent(window, gl);
  // K-153: a hidden Wayland window is never shown, so the compositor never says "draw the next one":
  // with vsync every swap would wait for that (for ever with SDL 2.0's driver, a second with later ones).
  const char* videoNow = SDL_GetCurrentVideoDriver();
  SDL_GL_SetSwapInterval(startHidden && videoNow && std::strcmp(videoNow, "wayland") == 0 ? 0 : 1);
  glrecovery::attach();
  glsl = glrecovery::glsl();  // K-75: GLSL 1.20 on an OpenGL 2.x context
  {
    // The window system goes with the driver line of a crash report.
    const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* videoName = SDL_GetCurrentVideoDriver();
    const std::string versionLine = std::string(glVersion ? glVersion : "?") + " / " + (videoName ? videoName : "?");
    crash::setGpu(reinterpret_cast<const char*>(glGetString(GL_VENDOR)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)), versionLine.c_str());
    std::fprintf(stderr, "[graphics] OpenGL %s, shaders %s\n", versionLine.c_str(), glsl);  // K-75
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ui::installGlyphHook();  // ImGui's own arrows, checks and crosses as SVG icons from the first frame
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // layout.ini is loaded once the main window is placed (below)
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  io.ConfigDragClickToInputText = true;

  // ---- fonts (needed to draw the splash) ----
  std::string assets = findAssets(argv[0]);
  if (assets.empty()) std::fprintf(stderr, "warning: assets folder not found; using the built-in font\n");
  appicon::apply(window);  // K-14: window / dock icon, built into the program (AppIcon.h)
  auto font = [&](const char* file, float size) -> ImFont* {
    if (assets.empty()) return nullptr;
    std::string p = (fs::path(assets) / "fonts" / file).string();
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    return io.Fonts->AddFontFromFileTTF(p.c_str(), size, &cfg);
  };
  ImFont* fRegular = font("Inter-Regular.ttf", 14.0f);
  ImFont* fBold = font("Inter-SemiBold.ttf", 14.0f);
  ImFont* fLarge = font("Inter-Bold.ttf", 20.0f);
  ImFont* fMono = font("JetBrainsMono-Regular.ttf", 13.0f);
  ImFont* fMonoBold = font("JetBrainsMono-Bold.ttf", 13.0f);
  if (!fRegular) fRegular = io.Fonts->AddFontDefault();
  if (!fBold) fBold = fRegular;
  if (!fLarge) fLarge = fBold;
  if (!fMono) fMono = fRegular;
  if (!fMonoBold) fMonoBold = fMono;
  io.FontDefault = fRegular;
  theme::apply(true);
  ImGui_ImplSDL2_InitForOpenGL(window, gl);
  ImGui_ImplOpenGL3_Init(glsl);
  // K-24 dev check: render as on an OpenGL 3.0/3.1 driver, where the renderer cannot honour draw
  // command vertex offsets (large meshes then rely on 32-bit indices alone).
  if (std::getenv("GRIDLINE_DEV_NO_VTX_OFFSET")) ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
  if (!crashDialogReport.empty()) {
    theme::setMarkTextureProvider(&markTexture);
    return runCrashDialog(window, gl, crashDialogReport, crashDialogSession, crashDialogShot, crashDialogFrames, crashDialogScale, fBold, fMono, assets);
  }
  viewports::install(window, gl, {startHidden});  // J-113: detachable windows
  auto keepDevClipboard = [startHidden]() {
    if (!startHidden) return;
    // J-78: automated runs keep their own clipboard (a scripted click on a
    // notification must not replace what the user copied); stdout shows it.
    static std::string devClipboard;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
      devClipboard = text ? text : "";
      std::printf("clipboard: %s\n", devClipboard.c_str());
    };
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* { return devClipboard.c_str(); };
  };
  keepDevClipboard();
  theme::setMarkTextureProvider(&markTexture);

  // ---- splash: each real start-up step advances the bar ----
  Uint64 splashT0 = SDL_GetPerformanceCounter();
  float shown = 0.0f;
  int splashFrames = 0;
  auto splash = [&](float target, const char* status) {
    // Render until the bar has caught up (min ~4 frames per step).
    for (int f = 0; f < 60; ++f) {
      SDL_Event e;
      while (SDL_PollEvent(&e))
        if (!gl::opendocs::take(e)) ImGui_ImplSDL2_ProcessEvent(&e);  // K-119: a file macOS asks to open is kept
      shown += (target - shown) * 0.35f;
      double tt = static_cast<double>(SDL_GetPerformanceCounter() - splashT0) / static_cast<double>(SDL_GetPerformanceFrequency());
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      drawSplash(fBold, fMono, shown, status, tt);
      ImGui::Render();
      ImGui::UpdatePlatformWindows();  // no detached windows yet; the first frame with them checks this was called
      int w, h;
      SDL_GL_GetDrawableSize(window, &w, &h);
      glViewport(0, 0, w, h);
      glClearColor(0.043f, 0.043f, 0.043f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      if (const char* shot = std::getenv("GRIDLINE_SPLASH_SHOT"); shot && target > 0.4f && target < 0.5f && f == 5)
        writePpm(shot, w, h);
      devEarlyLoss("splash", splashFrames++);
      if (const char* lost = glrecovery::poll()) {  // K-133: as the main window does (K-47)
        if (!rebuildLoneContext(window, gl, "splash", lost)) crash::endAfterGraphicsLoss(lost);  // no project yet: a report, the pop-up
        glsl = glrecovery::glsl();
        viewports::backendRestarted(gl);
        keepDevClipboard();
        continue;  // the frame again, on the new context
      }
      SDL_GL_SwapWindow(window);
      if (f >= 4 && target - shown < 0.01f) break;
    }
  };
  splash(0.12f, "Loading interface");
  splash(0.22f, "Reading settings");
  typingLayoutKey = [](int position) -> int { return SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(position)); };
  App app;
  app.fontRegular = fRegular;
  app.fontBold = fBold;
  app.fontLarge = fLarge;
  app.fontMono = fMono;
  app.fontMonoBold = fMonoBold;
  theme::applyFromSettings(app.settings);
  std::string iniPath = (fs::path(app.configDir()) / "layout.ini").string();
  {
    std::error_code ec;
    fs::create_directories(app.configDir(), ec);
  }
  bool firstRun = !fs::exists(iniPath);

  splash(0.42f, "Starting audio engine");
  app.audioSampleRate = static_cast<int>(app.settings.getNumber("audio.sampleRate"));
  gl::setLegacyRateReference(app.audioSampleRate);  // J-104: old Bitcrusher "Downsample N" loads as this rate / N
  app.audioBuffer = static_cast<int>(app.settings.getNumber("audio.bufferFrames"));
  app.audioInputDevice = app.settings.getString("audio.inputDevice");
  for (int i = 1; i < argc; ++i)  // screenshot runs never open the microphone
    if (std::string(argv[i]) == "--screenshot") app.audioInputDevice.clear();
  // J-48: interface sounds are off for --no-ui-sounds, GRIDLINE_NO_UI_SOUNDS (tests, CI)
  // and automated runs (screenshots, profiling, thumbnails).
  app.uiSoundsDisabled = std::getenv("GRIDLINE_NO_UI_SOUNDS") != nullptr;
  app.windowSystem = videodriver::currentName();  // K-153: Settings > Interface shows it
  setRunLoopGlBracket(foreigngl::runLoopBracket);  // K-153: GLX interfaces while Gridline draws with EGL
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--no-ui-sounds" || arg == "--screenshot" || arg == "--profile-frames") app.uiSoundsDisabled = true;
    if (arg == "--video-clock" && i + 1 < argc) gVideoFps = std::max(0, std::atoi(argv[i + 1]));  // K-10: before the audio device opens
  }
  app.assetsDir = assets;
  openAudio(app);
  splash(0.58f, "Scanning MIDI devices");
  initMidi(app);
  splash(0.70f, "Loading the plugin list");
  host::initHosting(app.configDir());
  splash(0.74f, "Preparing instruments and effects");
  app.syncEngine();
  {  // J-24: Settings > General > On startup (screenshot runs keep the built-in starter project)
    bool shot = false;
    for (int i = 1; i < argc; ++i) shot = shot || std::string(argv[i]) == "--screenshot";
    // J-141: a plain launch (no files, no options; macOS may add a -psn_ process serial) may show
    // the welcome launcher; files and options (tests, tools) keep opening straight into a project.
    bool plainLaunch = true;
    for (int i = 1; i < argc; ++i) plainLaunch = plainLaunch && std::string(argv[i]).rfind("-psn_", 0) == 0;
    // K-119: a launch to open a document (macOS sends it as an open request, not an argument) is no plain launch.
    plainLaunch = plainLaunch && !gl::opendocs::pending();
    if (shot)
      app.applySettingsHooks();
    else
      app.applyStartup(plainLaunch && !app.embedded);
  }
  crashui::startup(app);  // K-39: reports from earlier runs, the recovery copies
  // Opened with files (file manager, "Open with", the Linux desktop entry's %F): no options,
  // only paths; the first project among them opens (J-167: in the loading window, over the
  // starting interface).
  bool onlyPaths = argc > 1;
  for (int i = 1; i < argc; ++i) onlyPaths = onlyPaths && argv[i][0] != '-';
  bool argOpened = false;
  for (int i = 1; onlyPaths && i < argc; ++i) {
    std::error_code ec;
    // K-42: a project on a web drive is not stat-ed while starting: the loading window reports it.
    if ((gl::remotefs::isRemote(argv[i]) || fs::is_regular_file(fs::u8path(argv[i]), ec)) && app.openProject(argv[i])) {
      argOpened = true;
      break;
    }
  }
  // K-119: macOS started Gridline to open documents: the first project among them, as from the command line.
  for (const std::string& f : gl::opendocs::takeReady()) {
    if (hiddenRun) std::printf("os open: %s (starting)\n", f.c_str());
    std::error_code ec;
    if (!argOpened && fs::is_regular_file(fs::u8path(f), ec) && app.openProject(f)) argOpened = true;
  }
  gl::opendocs::startupDone();

  // Command line: [project] [--screenshot out.ppm] [--frames N] [--play] [--fx mixer slot] [--chan index]
  //               [--plugin type] [--size WxH]
  splash(0.88f, "Opening project");
  std::string shotPath;
  int shotFrames = 45;
  std::string videoOutPath;  // K-10: raw RGB frames
  FILE* videoOut = nullptr;
  std::pair<int, int> forcedSize{0, 0};  // --size WxH (window size for layout checks)
  std::pair<int, int> resizeTo{0, 0};    // --resize WxH (applied halfway through --frames)
  std::string devMenu;                    // --menu name: open a menu for screenshots
  bool devLight = false;                  // --light (kept when --layout re-reads the theme)
  std::string devLayoutPreset;            // K-79 --layout studio|performance|tracks: this run only (nothing saved)
  std::vector<std::string> devPanes;      // K-79 --pane mixer|piano|rack|history|pads|settings: open a window at frame 3
  bool devSwitchCheck = false;
  int devSwitchFrame = -100;  // the frame a layout switch was applied on (for --switch-check)
  std::vector<std::pair<int, std::string>> devSwitches;  // K-79 --switch-layout <id>@<frame>: choose a preset while running
  std::string devFocus;                   // --show <window>: focus a docked window once the layout exists
  bool devLive = false;                   // --live: launch live clips once the program exists
  int devAutoEdit = 0;                    // --show autoedit / autolfo
  int devExport = 0;                      // --show stems / renderclip / missing / midiimport (J-20)
  int devRemote = 0;                      // --show remote / remote-action: remote control dialog (frame 3)
  std::string devTool;                    // --tool name: J-11 tool window (frame 3) on the --edit target
  std::string devDialog;                 // --show open / saveas / prompt (J-54, frame 3; file dialogs browse the source tree)
  bool devChords = false;                // --show chords: chord progression editor (frame 3)
  std::string devScript;                  // --script id: J-24 piano roll script window (frame 3) on the --edit target
  bool devPlaylistOnly = false;           // --show playlist: other panels closed
  bool devMixerOnly = false;              // --show mixer: the mixer fills the window
  std::string devCollabDemo;              // --show collab-demo / collab-review / collab-demo-panel (J-111)
  float shotScale = 1.0f;                 // --scale F: framebuffer scale of an off-screen shot (HiDPI check)
  std::string devAlignAudit;              // --align-audit <file> (J-128)
  int devGallery = -1, devGalleryPage = 1;  // J-129 --gallery
  float devUiScale = 0.0f;                // --ui-scale F: interface scale for this run only (not saved; J-54 layout checks)
  std::string devInputLive;   // --dev-input-live file (K-9)
  std::string devSaveAtExit;  // --save-at-exit path: project saved when the run ends (plugin state round trips)
  std::string devSaveAt;      // --save-at path: project saved at frame 3 (J-78: the notification shows in the shot)
  int devWindowCycle = 0;  // --plugin-window-cycle N: close / reopen the plugin window N times
  bool devHostedEditor = false;           // --hosted-editor: open the native editor of the --plugin target
  std::vector<DevInputLine> devInput;     // --dev-input file: scripted mouse / key input (J-28)
  std::string profilePath;                // --profile-frames out.json: per-window UI CPU time per frame (J-25)
  int profileWarmup = 30;                 // --profile-warmup N: frames not recorded (layout, caches)
  double fixedDt = 0;                     // --fixed-dt ms: fixed UI frame time (J-37 animation frame sequences)
  std::vector<std::string> devHide;       // --hide rack|browser|mixer|playlist|piano: closed once the layout exists
  updui::Options updateOpts;              // J-89: --no-update-check, --fake-update <version>
  int devLauncher = -1;                   // J-141: --show launcher (0) / launcher-templates (1) / launcher-collab (2, J-146)
  bool devLauncherCollabDemo = false;     // J-146: --show launcher-collab-demo (simulated collaborators)
  bool argProject = false;                // a project path on the command line opened
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--screenshot" && i + 1 < argc) shotPath = argv[++i];
    else if (arg == "--profile-frames" && i + 1 < argc) profilePath = argv[++i];
    else if (arg == "--profile-warmup" && i + 1 < argc) profileWarmup = std::max(0, std::atoi(argv[++i]));
    else if (arg == "--frames" && i + 1 < argc) shotFrames = std::atoi(argv[++i]);
    else if (arg == "--video-clock" && i + 1 < argc) ++i;  // K-10: read before the audio device opens
    else if (arg == "--dev-os-open" && i + 1 < argc) ++i;  // K-119: posted after SDL_Init
    else if (arg == "--video-out" && i + 1 < argc) videoOutPath = argv[++i];
    else if (arg == "--video-cursor") devVideoCursor = true;
    else if (arg == "--menu" && i + 1 < argc) devMenu = argv[++i];
    else if (arg == "--layout" && i + 1 < argc) devLayoutPreset = argv[++i];
    else if (arg == "--pane" && i + 1 < argc) devPanes.push_back(argv[++i]);
    else if (arg == "--switch-check") devSwitchCheck = true;  // K-79: report each layout switch's frames (stderr)
    else if (arg == "--switch-layout" && i + 1 < argc) {
      const std::string v = argv[++i];
      const size_t at = v.find('@');
      devSwitches.push_back({at == std::string::npos ? 10 : std::atoi(v.c_str() + at + 1), v.substr(0, at)});
    }
    else if (arg == "--tool" && i + 1 < argc) devTool = argv[++i];
    else if (arg == "--script" && i + 1 < argc) devScript = argv[++i];
    else if (arg == "--dev-input" && i + 1 < argc) devInput = loadDevInput(argv[++i]), ui::devKnobsEnable(true);  // K-148: "knobs"
    else if (arg == "--dev-input-live" && i + 1 < argc) {  // K-9: see the command list above runDevInput
      devInputLive = argv[++i];
      devInput.push_back({0, "#", "", "", "", {}});  // the session counts frames from the start
    }
    else if (arg == "--play") app.engine.play();
    else if (arg == "--fx" && i + 2 < argc) {
      size_t mi = static_cast<size_t>(std::atoi(argv[++i]));
      int slot = std::atoi(argv[++i]);
      if (mi < app.doc.song().mixer.size()) app.fxEditor = {app.doc.song().mixer[mi].id, slot};
    } else if (arg == "--chan" && i + 1 < argc) {
      const std::string v = argv[++i];
      size_t ci = static_cast<size_t>(std::atoi(v.c_str()));
      if (ci < app.doc.song().channels.size()) app.settingsChannel = app.doc.song().channels[ci].id;
      if (v.find(":misc") != std::string::npos) app.channelTab = 1;  // K-144: "N:misc" opens the channel's Misc page
    } else if (arg == "--edit" && i + 2 < argc) {  // dev check: select pattern <p> and channel <c>, focus the piano roll
      size_t pi = static_cast<size_t>(std::atoi(argv[++i]));
      size_t ci = static_cast<size_t>(std::atoi(argv[++i]));
      if (pi < app.doc.song().patterns.size()) app.selectPattern(app.doc.song().patterns[pi].id);
      if (ci < app.doc.song().channels.size()) app.selectChannel(app.doc.song().channels[ci].id);
      devFocus = "Piano roll";
    } else if (arg == "--plugin" && i + 1 < argc) {
      // Opens a plugin's editor on the demo song: instruments get a new channel,
      // effects go into the last slot of insert 1.
      std::string type = argv[++i];
      if (const PluginInfo* info = findPlugin(type)) {
        if (info->kind == PluginKind::Generator) {
          app.settingsChannel = app.addInstrument(type);
        } else if (info->kind == PluginKind::NoteFx) {  // note processor: on a new synth channel
          app.settingsChannel = app.addInstrument("gl.hat");  // small editor: the chain stays in view
          app.doc.addNoteFx(app.settingsChannel, type);
        } else if (app.doc.song().mixer.size() > 1) {
          ObjectId mt = app.doc.song().mixer[1].id;
          app.doc.setSlotPlugin(mt, kEffectSlots - 1, type);
          app.fxEditor = {mt, kEffectSlots - 1};
        }
        app.syncEngine();
      } else {
        std::fprintf(stderr, "unknown plugin type: %s\n", type.c_str());
      }
    } else if (arg == "--plugin-data" && i + 1 < argc) {  // dev check (K-144): the data of the instrument opened by --plugin
      const std::string data = argv[++i];
      if (app.settingsChannel != kNoId) app.doc.editChannel(app.settingsChannel, "", [&](Channel& c) { c.plugin.data = data; });
      app.syncEngine();
    } else if (arg == "--wrapper-page" && i + 1 < argc) {  // dev check: hosted plugin window page
      setHostedWrapperPage(std::atoi(argv[++i]));
    } else if (arg == "--param" && i + 2 < argc) {  // dev check: set a parameter of the plugin opened by --plugin
      int k = std::atoi(argv[++i]);
      double v = std::atof(argv[++i]);
      if (app.fxEditor.second >= 0) app.doc.setParam(slotParam(app.fxEditor.first, app.fxEditor.second, k), v, false);
      else if (app.settingsChannel != kNoId) app.doc.setParam(channelPluginParam(app.settingsChannel, k), v, false);
      app.syncEngine();
    } else if (arg == "--preset" && i + 1 < argc) {
      // Dev check: load factory preset N into the effect opened by --plugin.
      int idx = std::atoi(argv[++i]);
      if (app.fxEditor.first != kNoId) {
        const MixerTrack* m = app.doc.song().mixerTrack(app.fxEditor.first);
        const PluginInfo* info = m ? findPlugin(m->slots[static_cast<size_t>(app.fxEditor.second)].plugin.type) : nullptr;
        if (info && idx >= 0 && static_cast<size_t>(idx) < info->presets.size()) {
          PluginState ps = presetState(*info, static_cast<size_t>(idx));
          app.doc.editMixerTrack(app.fxEditor.first, "preset", [&](MixerTrack& t) { t.slots[static_cast<size_t>(app.fxEditor.second)].plugin = ps; });
          app.syncEngine();
        }
      }
    } else if (arg == "--zoom" && i + 1 < argc) {
      // Dev check: zoom factor applied to the playlist and piano roll (e.g. 50, or "max").
      std::string z = argv[++i];
      float f = z == "max" ? 1e9f : static_cast<float>(std::atof(z.c_str()));
      if (f > 0) app.playlist.zoomRequest = app.piano.zoomRequest = f;
    } else if (arg == "--no-draw") {  // dev check: every frame is laid out but nothing is drawn (long scripted sessions that read state, not pixels)
      devNoDraw = true;
    } else if (arg == "--fixed-dt" && i + 1 < argc) {  // J-37 dev check: every frame advances UI time by N ms (reproducible animation)
      fixedDt = std::max(0.0, std::atof(argv[++i])) / 1000.0;
    } else if (arg == "--scrollx" && i + 1 < argc) {  // dev check: playlist horizontal scroll (px)
      app.playlist.scrollX = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--size" && i + 1 < argc) {
      int w = 0, h = 0;
      if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) forcedSize = {w, h};
    } else if (arg == "--settings-size" && i + 1 < argc) {  // J-137: settings window size for layout checks
      int w = 0, h = 0;
      if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) setDevSettingsSize(static_cast<float>(w), static_cast<float>(h));
    } else if (arg == "--resize" && i + 1 < argc) {
      // Dev check for live resizing: switch to this size halfway through --frames.
      int w = 0, h = 0;
      if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) resizeTo = {w, h};
    } else if (arg == "--show" && i + 1 < argc) {  // open a window for layout checks
      std::string w = argv[++i];
      if (w == "about") app.showAbout = true;
      else if (w == "launcher") devLauncher = 0;            // J-141: the welcome launcher
      else if (w == "launcher-templates") devLauncher = 1;  // ... on its Templates view
      else if (w == "launcher-collab") devLauncher = 2;     // J-146: ... on its Collaborative view
      else if (w == "launcher-collab-demo") devLauncher = 2, devLauncherCollabDemo = true;  // ... with simulated collaborators
      else if (w == "settings") app.showSettings = true;
      else if (w == "shortcuts") app.showShortcuts = true;
      else if (w == "export") app.showExport = true;
      else if (w == "info") app.showInfo = true;
      else if (w == "history") app.showHistory = true, devFocus = "Undo history";  // docked behind the browser: bring its tab to the front
      else if (w == "performance") app.showPerformance = true;
      else if (w == "groove") app.showGroove = true;  // K-151: the Groove editor
      else if (w == "plugins") app.showPluginManager = true;
      else if (w == "piano") devFocus = "Piano roll";
      else if (w == "playlist") devPlaylistOnly = true;  // the playlist fills the window
      else if (w == "mixer") devMixerOnly = true;        // the mixer fills the window
      else if (w == "open" || w == "saveas" || w == "prompt" || w == "unsaved" || w == "ppqwarn" || w == "delsource") devDialog = w;  // J-54: file dialogs / one-line prompt (frame 3)
      else if (w == "presetprogress") {  // J-54 layout check: FL preset import progress (no worker runs)
        app.showSettings = true, app.settingsTab = 2;
        app.presetImport.running = true, app.presetImport.done = 1234, app.presetImport.total = 5678;
      }
      else if (w == "livepads") app.showLivePads = true;
      else if (w == "master") app.showMaster = true;  // RN-09 (GRIDLINE_MASTER_DEMO=1 adds the chain to the demo song)
      else if (w == "autoedit") devAutoEdit = 1;  // automation clip editor on a demo clip
      else if (w == "autolfo") devAutoEdit = 2;   // … with the LFO tool open
      else if (w == "autolane") devAutoEdit = 3, devFocus = "Piano roll";  // pattern automation lane
      else if (w == "midi") app.showSettings = true, app.settingsTab = 1;
      else if (w == "project-settings") app.showSettings = true, app.settingsTab = 2;
      else if (w == "backup") app.showSettings = true, app.settingsTab = 3;
      else if (w == "files") app.showSettings = true, app.settingsTab = 6;  // K-45 Files page
      else if (w == "interface") app.showSettings = true, app.settingsTab = 4;
      else if (w == "general") app.showSettings = true, app.settingsTab = 5;
      else if (w == "files") app.showSettings = true, app.settingsTab = 6;
      else if (w == "debug") app.showSettings = true, app.settingsTab = 7;
      else if (w == "keys") app.showSettings = true, app.settingsTab = 8;   // J-55 shortcut editor
      else if (w == "theme") app.showSettings = true, app.settingsTab = 9;  // J-55 theme page
      else if (w == "integrations") app.showSettings = true, app.settingsTab = 10;  // SC-07 AI assistants page
      else if (w == "collab-settings") app.showSettings = true, app.settingsTab = 11;  // J-111 Collaboration page
      else if (w == "plugin-folders") app.showSettings = true, app.settingsTab = 12;  // K-13 Plugins page
      else if (w == "project-settings") app.showSettings = true, app.settingsTab = 2;  // K-110 FL Studio export section
      else if (w == "flp-export-options") devFlpAsk = true;  // K-110 export options window (needs the song loaded first and a --dev-input script)
      else if (w == "collab") app.showCollab = true;                                // J-111 panel (no session)
      else if (w == "collab-join") gl::collabui::openJoin(app);                      // J-111 join dialog
      else if (w == "collab-demo" || w == "collab-review" || w == "collab-demo-panel" || w == "collab-cursors") {  // J-111: a simulated collaborator
        devCollabDemo = w;
      }
      else if (w == "mcplog") gl::mcpui::showActivity();                            // SC-07 activity log
      else if (w == "newproject") app.showNewProject = true;
      else if (w == "loopstarter") gl::j59::openLoopStarter(app);  // J-59
      else if (w.rfind("loopmade", 0) == 0) gl::j59::createFromLoopStarter(app, std::atoi(w.c_str() + 8));  // J-59: "loopmade<style>"
      else if (w == "rendersampler") {                             // J-59: on the selected (else the first) channel
        const gl::ObjectId sel = app.doc.song().channel(app.selChannel) ? app.selChannel : gl::kNoId;
        if (sel || !app.doc.song().channels.empty()) gl::j59::openRenderSampler(app, sel ? sel : app.doc.song().channels[0].id);
      }
      else if (w == "savetemplate") app.showSaveTemplate = true;
      else if (w == "rackgraph") app.rack.showGraph = true, app.rack.graphLane = 1;   // J-11 graph editor (pan lane)
      else if (w == "rackkeys") app.rack.showGraph = true, app.rack.graphLane = 7;    // J-11 keyboard editor
      else if (w == "remote") devRemote = 1;
      else if (w == "remote-action") devRemote = 2;
      else if (w == "stems") devExport = 1;         // J-20: render dialog with split mixer tracks
      else if (w == "renderclip") devExport = 2;    // render dialog → audio clip
      else if (w == "missing") devExport = 3;       // missing samples window (two references broken)
      else if (w == "midiimport") devExport = 4;    // MIDI import options (the demo song as MIDI)
      else if (w == "rendersummary") devExport = 5; // renders the song (WAV + FLAC + MIDI) into the temp folder, then the summary
      else if (w == "importreport") {  // layout check: a sample FL import summary
        app.showImportReport = true;
        app.importReport.source = "Sample song.flp";
        app.importReport.counts = {{"Channels", 14}, {"Patterns", 9}, {"Playlist clips", 41}, {"Mixer inserts", 12}, {"Effects", 23}};
        app.importReport.unmapped = {{"Third-party plugin (no installed match)", 2},
                                     {"Wrapped third-party instrument whose plugin file is not installed on this computer", 1}};
        app.importReport.warnings = {"Two automation clips target parameters that have no Gridline equivalent; they were kept but do nothing.",
                                     "Time stretching of one audio clip is approximated."};
      } else if (w == "steprec") {  // layout check: step record cursor in the piano roll
        app.stepRecord = true;
        app.stepRecorder.cursor = app.doc.song().ppq * 2;
        devFocus = "Piano roll";
      } else if (w == "chanmisc") {  // layout check: channel settings, misc tab
        if (!app.doc.song().channels.empty()) app.settingsChannel = app.doc.song().channels[0].id;
        app.channelTab = 1;
      } else if (w == "chanenv") {  // layout check: channel settings, envelope page (J-72) of the first channel that has one
        app.showImportReport = false;
        for (const auto& ch : app.doc.song().channels)
          if (insSupported(ch.plugin.type)) {
            app.settingsChannel = ch.id;
            app.channelTab = 2;
            break;
          }
      } else if (w == "chords") {  // J-36 QA: chord progression editor on the selected pattern / channel (frame 3)
        devChords = true;
      } else if (w == "flpexport") {  // J-80 layout check: FL export of the song into the config folder, then its report
        const std::string out = (std::filesystem::path(app.configDir()) / "dev-flp-export" / "check.flp").string();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(out).parent_path(), ec);
        flpexport::start(app, out, false, [](bool) { flpexport::openReport(); });
      } else if (w == "toast") {  // layout check: notification toasts
        app.notify("Saved the project");
        app.notify("Cannot decode sample: missing.wav", true);
      } else if (w == "toast-plugin") {  // layout check: why a plugin's interface cannot open
        app.notify(std::string("Long Plugin Name 2: ") + videodriver::blockMessage(videodriver::Block::NoXServer), true);
        app.notify(std::string("Long Plugin Name 2: ") + videodriver::blockMessage(videodriver::Block::WaylandByChoice), true);
        app.notify("Long Plugin Name 2: the plugin's bridge refused to open its interface. Check that it works in another host, and its logs", true);
      }
    } else if (arg == "--rack" && i + 1 < argc) {  // J-84 dev check: rack pattern length / size / docking
      rackDevSpec(argv[++i]);
    } else if (arg == "--hide" && i + 1 < argc) {  // dev check (README shots): close a main window
      devHide.push_back(argv[++i]);
    } else if (arg == "--mixer-view" && i + 1 < argc) {
      // Dev check: mixer view options, comma separated: compact, normal, wide, rows2..4, ext, wave,
      // rms, both, nohold, routing, arm1 (arm insert 1), sel1-3 (select inserts 1..3).
      std::string o = argv[++i] + std::string(",");
      MixerViewState& mv = app.mixerView;
      for (size_t a0 = 0, b0; (b0 = o.find(',', a0)) != std::string::npos; a0 = b0 + 1) {
        std::string k = o.substr(a0, b0 - a0);
        const Song& song = app.doc.song();
        if (k == "compact") mv.layout = MixerViewState::Compact, mv.layoutOverride = true;
        else if (k == "normal") mv.layout = MixerViewState::Wide, mv.layoutOverride = true;
        else if (k == "wide") mv.layout = MixerViewState::ExtraLarge, mv.layoutOverride = true;
        else if (k.rfind("rows", 0) == 0) mv.rows = std::clamp(std::atoi(k.c_str() + 4), 1, 4);
        else if (k == "ext") mv.extended = true;
        else if (k == "wave") mv.waveforms = true;
        else if (k == "rms") mv.meterMode = MixerViewState::Rms;
        else if (k == "both") mv.meterMode = MixerViewState::PeakRms;
        else if (k == "nohold") mv.peakHold = false;
        else if (k == "routing") mv.showRouting = true;
        else if (k.rfind("arm", 0) == 0 && static_cast<size_t>(std::atoi(k.c_str() + 3)) < song.mixer.size())
          app.doc.editMixerTrack(song.mixer[static_cast<size_t>(std::atoi(k.c_str() + 3))].id, "arm", [](MixerTrack& t) { t.armed = true; });
        else if (k.rfind("sel", 0) == 0) {
          int a1 = 0, b1 = 0;
          if (std::sscanf(k.c_str() + 3, "%d-%d", &a1, &b1) == 2)
            for (int q = a1; q <= b1 && q < static_cast<int>(song.mixer.size()); ++q) mv.sel.insert(song.mixer[static_cast<size_t>(q)].id);
          if (!mv.sel.empty()) app.selMixer = *mv.sel.begin();
        }
      }
    } else if (arg == "--layout-audit" && i + 1 < argc) {  // Q-7: inset check of every editor + contact sheets
      browserui::beginAuditMode(app, argv[++i]);
    } else if (arg == "--align-audit" && i + 1 < argc) {  // J-128: alignment check of every window on the last frame (TSV)
      devAlignAudit = argv[++i];
    } else if (arg == "--binding-crawl") {  // J-92: --layout-audit also clicks through every editor (param_bindings.tsv)
      browserui::setBindingCrawl(true);
    } else if (arg == "--make-thumbs") {  // render every built-in editor into <config>/thumbs (plugin picker)
      browserui::beginThumbMode(app);
    } else if (arg == "--make-thumb" && i + 1 < argc) {
      browserui::beginThumbMode(app, argv[++i]);
    } else if (arg == "--picker") {  // dev check: plugin picker open
      browserui::togglePluginPicker();
    } else if (arg == "--browser-search" && i + 1 < argc) {
      app.browserSearch = argv[++i];
    } else if (arg == "--plugin-window-keys") {  // J-66 dev check: Space typed into the plugin's interface
      setPluginWindowDevKeys(true);
    } else if (arg == "--frame-stats") {  // J-66: frame rate, frame time parts, CPU, plugin interface draws (stderr, each second)
      fstats::enable(true);
      setPluginWindowDrawStats(true);
    } else if (arg == "--hosted-editor") {
      devHostedEditor = true;
      setPluginWindowLog(true);
    } else if (arg == "--plugin-panel" && i + 1 < argc) {  // dev check: plugin window settings unfolded on page N
      setPluginWindowPanel(std::atoi(argv[++i]));
    } else if (arg == "--save-at" && i + 1 < argc) {  // dev check: save the project at frame 3
      devSaveAt = argv[++i];
    } else if (arg == "--save-at-exit" && i + 1 < argc) {  // dev check: save the project when the run ends
      devSaveAtExit = argv[++i];
    } else if (arg == "--plugin-window-action" && i + 1 < argc) {  // dev check: press a plugin window bar button
      setPluginWindowDevAction(argv[++i]);
    } else if (arg == "--plugin-window-cycle" && i + 1 < argc) {
      devWindowCycle = std::atoi(argv[++i]);
    } else if (arg == "--dev-sandbox" && i + 2 < argc) {  // dev check: "Run in sandbox" for a hosted type (before --plugin)
      const std::string type = argv[++i];
      host::setRunInSandbox(type, std::atoi(argv[++i]) != 0);
    } else if (arg == "--plugin-window-resize" && i + 1 < argc) {  // dev check: drag plugin windows to WxH
      int rw = 0, rh = 0;
      if (std::sscanf(argv[++i], "%dx%d", &rw, &rh) == 2) setPluginWindowDevResize(rw, rh);
    } else if (arg == "--live") {  // dev check: live mode, first live clip of every track launched (frame 3)
      devLive = true;
    } else if (arg == "--no-ui-sounds") {  // J-48: handled before the audio device opens
    } else if (arg == "--lang" && i + 1 < argc) {  // J-55 dev check: interface language for this run only (not saved)
      i18n::setLanguage(argv[++i], app.languageDir());
    } else if (arg == "--gallery" && i + 1 < argc) {  // J-129 dev: element gallery of a look ("graphite", "lumen:2", ...)
      std::string g = argv[++i];
      const size_t colon = g.find(':');
      if (colon != std::string::npos) devGalleryPage = std::atoi(g.c_str() + colon + 1), g.resize(colon);
      look::Dir d{};
      if (look::parseDir(g.c_str(), d)) devGallery = static_cast<int>(d);
    } else if (arg == "--look" && i + 1 < argc) {  // J-129 dev: draw the shared elements in a candidate look
      look::Dir d{};
      if (look::parseDir(argv[++i], d)) ui::forceControlStyle(d);
    } else if (arg == "--light") {
      theme::apply(false);
      devLight = true;
    } else if (arg == "--ui-scale" && i + 1 < argc) {
      devUiScale = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.75f, 2.0f);
    } else if (arg == "--scale" && i + 1 < argc) {
      shotScale = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.5f, 4.0f);
    } else if (arg == "--no-update-check") {  // J-89
      updateOpts.disabled = true;
    } else if (arg == "--fake-update" && i + 1 < argc) {  // J-89 dev check: a made-up newer release is offered (no network)
      updateOpts.fakeVersion = argv[++i];
    } else if (arg == "--crash-recover" && i + 1 < argc) {  // K-39: restart from the crash pop-up
      crashui::recoverFrom(app, argv[++i]);
    } else if (arg == "--hidden") {
      // J-78: read before the window is created (see startHidden)
      // K-45: scripted runs leave the default cache alone (their own has GRIDLINE_CACHE_DIR).
      if (!std::getenv("GRIDLINE_CACHE_DIR")) cacheui::skipStartupSweep();
    } else if ((gl::remotefs::isRemote(arg) || fs::exists(arg)) && !onlyPaths) {
      // Options after the project (--chan, --fx, --edit, --plugin) act on it: it loads now (J-167).
      argProject = app.openProject(arg) || argProject;
      app.finishProjectLoad();
    }
  }
  if (devLauncher >= 0) {  // J-141: with a project argument, that project stays open behind it
    app.finishProjectLoad();  // J-167: (opened before the launcher shows over it)
    launcherui::setView(devLauncher);
    if (devLauncherCollabDemo) gl::collabui::startLauncherDemo(app, (std::filesystem::path(app.configDir()) / "launcher-collab-demo").string());
    if (argProject) app.home.showHome(app.now);
    else app.home.showAtStartup(app.now);
  }
  int frameNo = 0;
  int liveFrames = 0;  // J-48: frames drawn (startup sound after the third)
  workspace::Rect rackPlacement;  // floating rack rectangle of the default layout (screen coordinates)
  bool placeRack = false;
  if (!profilePath.empty()) {
    // Profiling: no vsync wait, the measured frames come after the warm-up.
    prof::enable(true);
    prof::setWarmup(profileWarmup);
    SDL_GL_SetSwapInterval(0);
  }
  splash(1.0f, "Ready");
  // New or changed plugins are scanned in the background, each in a child
  // process (screenshot runs and GRIDLINE_NO_PLUGIN_SCAN skip it).
  if (shotPath.empty() && profilePath.empty() && !std::getenv("GRIDLINE_NO_PLUGIN_SCAN")) host::startScan(host::selfExecutablePath());
  const PluginTarget devEditorTarget = app.fxEditor.first != kNoId ? PluginTarget{app.fxEditor.first, app.fxEditor.second}
                                                                   : PluginTarget{app.settingsChannel, -1};

  // ---- become the main window ----
  {
    SDL_Rect usable{0, 0, 1600, 960};
    SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(window), &usable);
    int w = std::min(1600, usable.w - 40), h = std::min(960, usable.h - 60);
    if (forcedSize.first > 0) {
      w = forcedSize.first;
      h = forcedSize.second;
    }
    if (browserui::thumbMode() && !shotPath.empty() && forcedSize.first > 0) SDL_HideWindow(window);  // background thumbnail run
    // GitHub issue #33: an ordinary resizable top-level window, mapped again so that a tiling window
    // manager tiles it (MainWindowRole.h). Size and position are requests the manager may ignore.
    winrole::becomeMain(window, {w, h, usable.x + (usable.w - w) / 2, usable.y + (usable.h - h) / 2});
  }

  // Screenshots with an explicit size render off-screen at exactly that size
  // (the OS may clamp a real window to the display). K-153: so do hidden runs with a size on Wayland,
  // where a window that is never shown keeps the size it was created with (the splash's) whatever
  // it is resized to, and scripted pictures and pixel checks would read that.
  OffscreenTarget offscreen;
  const char* driverNow = SDL_GetCurrentVideoDriver();
  const bool hiddenOnWayland = startHidden && driverNow && std::strcmp(driverNow, "wayland") == 0;
  const bool useOffscreen = (!shotPath.empty() || !profilePath.empty() || hiddenOnWayland) && forcedSize.first > 0 &&
                            offscreen.create(static_cast<int>(static_cast<float>(forcedSize.first) * shotScale),
                                             static_cast<int>(static_cast<float>(forcedSize.second) * shotScale));
  // J-91: the system's interface scale (core/UiScale.h): the desktop's own
  // scale setting, never the panel's physical pixel density.
  float autoDpi = 1.0f;
  {
    uiscale::Inputs si;
#if defined(__APPLE__)
    si.desktop = uiscale::Desktop::MacOS;
#elif defined(_WIN32)
    si.desktop = uiscale::Desktop::Windows;
    SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), &si.displayDpi, nullptr, nullptr);
#else
    const char* driver = SDL_GetCurrentVideoDriver();
    si.desktop = driver && std::strcmp(driver, "wayland") == 0 ? uiscale::Desktop::Wayland
                 : driver && std::strcmp(driver, "x11") == 0  ? uiscale::Desktop::X11
                                                                : uiscale::Desktop::Other;
    si.gdkScale = std::getenv("GDK_SCALE");
    si.gdkDpiScale = std::getenv("GDK_DPI_SCALE");
    si.qtScaleFactor = std::getenv("QT_SCALE_FACTOR");
#if defined(SDL_VIDEO_DRIVER_X11)
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(window, &wm) && wm.subsystem == SDL_SYSWM_X11) {
      host::native::Window xw;
      xw.display = static_cast<void*>(wm.info.x11.display);
      xw.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(wm.info.x11.window));
      const std::string res = host::native::xResources(xw);
      si.xftDpi = uiscale::parseXftDpi(res.c_str());
    }
#endif
#endif
    int dh = 0, wh = 0;
    SDL_GL_GetDrawableSize(window, &si.drawableW, &dh);
    SDL_GetWindowSize(window, &si.windowW, &wh);
    // Off-screen runs (tests, screenshots at a given size) lay out the same on every machine.
    autoDpi = useOffscreen ? 1.0f : uiscale::systemScale(si);
    uiscale::setDetected(uiscale::systemScale(si));
    if (uiscale::systemScale(si) != 1.0f)
      std::fprintf(stderr, "[ui-scale] system scale %.2f (%s)\n", static_cast<double>(uiscale::systemScale(si)), uiscale::systemScaleSource(si).c_str());
  }
  ImU32 lastScaledEvent = 0;
  // J-66 (macOS): no blocking vsync swap; the frame clock waits for the
  // display inside the Cocoa run loop, where hosted plugin interfaces live.
  host::native::Window mainNative;
  {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
#if defined(__APPLE__)
    if (SDL_GetWindowWMInfo(window, &wm) && wm.subsystem == SDL_SYSWM_COCOA) mainNative.handle = static_cast<void*>(wm.info.cocoa.window);
#elif defined(_WIN32)
    if (SDL_GetWindowWMInfo(window, &wm) && wm.subsystem == SDL_SYSWM_WINDOWS) mainNative.handle = static_cast<void*>(wm.info.win.window);
#endif
  }
  // J-111g: collaborators' cursors are drawn the size of this computer's pointer
  // (host/ui/NativeView.h pointerSize; X11 reads the main window's display).
  host::native::Window pointerNative = mainNative;
#if defined(SDL_VIDEO_DRIVER_X11) && !defined(__APPLE__) && !defined(_WIN32)
  {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(window, &wm) && wm.subsystem == SDL_SYSWM_X11) {
      pointerNative.display = static_cast<void*>(wm.info.x11.display);
      pointerNative.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(wm.info.x11.window));
    }
  }
#endif
  host::native::PointerSize pointerSize;
  double pointerAt = -1;
  if (mainNative.valid() && !xdrop::native::install(mainNative.handle))  // J-75: hover, promises, links (macOS)
    std::fprintf(stderr, "[drop] native drop target unavailable; using SDL drop events\n");
  {  // J-89: update check once the window is up; automated runs never touch the network
    updateOpts.hidden = startHidden;
    updateOpts.disabled = updateOpts.disabled || update::disabledByEnvironment();
    updateOpts.automated = startHidden || !shotPath.empty() || !profilePath.empty() || browserui::thumbMode() || !devInput.empty();
    updateOpts.openUrl = [](const std::string& url) { return SDL_OpenURL(url.c_str()) == 0; };
    collabui::setOpenUrl(updateOpts.openUrl);  // J-111e: Show in Finder / Files
    updui::start(app, updateOpts);
    suiteui::start(app, updateOpts.automated);  // J-131: Gridline Suite into the plug-in folders
    urlimportui::start(updateOpts.automated);    // J-192: scripted runs never download the tools
  }
  if (gVideoFps > 0) {
    fixedDt = 1.0 / gVideoFps;
    SDL_GL_SetSwapInterval(0);
  }
  const bool frameClock = gVideoFps == 0 && profilePath.empty() && mainNative.valid() && !std::getenv("GRIDLINE_VSYNC_SWAP") && host::native::frameClockStart(mainNative);
  if (frameClock) SDL_GL_SetSwapInterval(0);
  // The saved layout, read now that the main window is where it will be (J-113: detached
  // windows are saved relative to it), then saved there on exit.
  viewports::setVirtualMonitors(useOffscreen);
  // K-79: the layout preset (Settings > Interface > Layout) decides which layout file is read;
  // Studio keeps layout.ini. --layout shows another one for this run without saving anything.
  layout::Preset layoutPreset = layout::current(app.settings);
  if (!devLayoutPreset.empty()) {
    layoutPreset = layout::fromId(devLayoutPreset);
    layoutui::setDevOverride(layoutPreset);
    layoutui::applyKeyProfile(app);
    theme::applyFromSettings(app.settings);
    if (devLight) theme::apply(false);
  }
  layoutui::setActive(layoutPreset);
  iniPath = (fs::path(app.configDir()) / layout::iniFileName(layoutPreset)).string();
  firstRun = !fs::exists(iniPath);
  bool layoutRebuild = false;  // K-79: a preset switch found no saved layout file
  bool layoutRebuildKeep = false;  // ... and the windows open before the switch stay open
  ImGui::LoadIniSettingsFromDisk(iniPath.c_str());
  // Automated runs and the dev options (--show mixer rebuilds the layout around one panel) read the
  // saved layout but never write it back: a test run must not leave its layout in the user's own.
  // (GRIDLINE_SAVE_LAYOUT=1: tests of the saved layout itself, in their own config folder.)
  const bool keepSavedLayout = (devOptions || updateOpts.automated) && !std::getenv("GRIDLINE_SAVE_LAYOUT");
  io.IniFilename = keepSavedLayout ? nullptr : iniPath.c_str();
  if (keepSavedLayout) std::fprintf(stderr, "layout: %s is read, not saved (a run with options)\n", iniPath.c_str());

  std::map<SDL_Scancode, HeldNote> held;
  std::string lastTitle;
  bool running = true;
  Uint64 t0 = SDL_GetPerformanceCounter();
  // One complete frame after the events: tick, interface, render and swap of every window. The
  // main loop draws it after each event pump; J-116: SDL's event watch also draws it from inside
  // a pump the window system blocks while a window is resized (`live`, see LiveResize.h).
  int crashForceCount = 0;
  float lastUiScale = 1.0f;  // the failing-interface pop-up draws at the same size
  bool crashTestDirty = hiddenRun && std::getenv("GRIDLINE_CRASH_TEST_DIRTY") != nullptr;
  const char* graphicsLost = nullptr;  // K-47: why, until it is dealt with
  auto frame = [&](bool live) {
    const Uint64 frameStart = SDL_GetPerformanceCounter();  // K-71: "scan state"
    liveresize::frameBegin(live);
    uint64_t statT = 0;
    // J-109: Gridline lost focus (another application, a plugin window, a key released
    // over another window): no key-up will come, so every note held from the interface
    // ends now (typed keys, clicked keys, pads). J-113: not when the focus only moved
    // between the main window and a detached one.
    if (viewports::takeAppFocusLost()) {
      releaseTypedNotes(app, held);
      app.engine.releaseLiveNotes();
      // K-175: the modifiers' key-ups went to the other application too: none counts as held now
      // (Caps Lock and Num Lock stay as they are).
      SDL_SetModState(static_cast<SDL_Keymod>(SDL_GetModState() & (KMOD_NUM | KMOD_CAPS | KMOD_MODE)));
    }
    xdragui::pump(app);  // J-75: drags from other applications
    if (app.quitRequested) {
      app.quitRequested = false;
      app.cancelProjectLoad();  // J-167: quitting while a project loads: the load stops, the open project is the one asked about
      app.guardUnsaved([&running]() { running = false; });
    }
    pollAsio(app);  // K-6: driver resets, latency changes, the control panel
    if (app.audioReopen) {
      app.audioReopen = false;
      openAudio(app);
    }
    if (!app.typingKeyboard && app.typing.anyHeld()) releaseTypedNotes(app, held);

    double now = static_cast<double>(SDL_GetPerformanceCounter() - t0) / static_cast<double>(SDL_GetPerformanceFrequency());
    if (gVideoFps > 0) now = static_cast<double>(gVideoFrames) / gVideoFps;
    {
      prof::Scope ps("tick");
      fstats::Timer ft(fstats::Tick);
      app.tick(now);
      collabui::frame(app, now);  // J-111: network, collaborators' edits, saving
      pumpHostedPlugins(app);
      crashui::frame(app);  // K-39: the recovery copy
      app.processMidi();
      app.processAudioRecording();  // J-8
    }
    app.engine.setMetronome(app.metronome);
    app.engine.setReturnToSeek(app.settings.getBool("transport.rememberSeek"));  // J-49
    app.hint.clear();
    statT = fstats::stamp();
    // Interface scale: the UI is laid out in logical pixels on a virtual display
    // of window/scale and rendered at the real framebuffer density, so every
    // window (including fixed-size editors) scales as one and stays sharp.
    int winW, winH, fbW, fbH;
    SDL_GetWindowSize(window, &winW, &winH);
    SDL_GL_GetDrawableSize(window, &fbW, &fbH);
    if (useOffscreen && resizeTo.first > 0 && frameNo == shotFrames / 2) {
      forcedSize = resizeTo;
      offscreen.create(static_cast<int>(static_cast<float>(forcedSize.first) * shotScale),
                       static_cast<int>(static_cast<float>(forcedSize.second) * shotScale));
    }
    if (useOffscreen) {
      winW = forcedSize.first;
      winH = forcedSize.second;
      fbW = offscreen.w;
      fbH = offscreen.h;
    }
    // The user's interface scale, times the system's unless turned off (J-91); --ui-scale is exact.
    const float uiScale = devUiScale > 0 ? uiscale::effectiveScale(devUiScale, false, 1.0f, winW, winH)
                                         : uiscale::effectiveScale(app.settings.getNumber("ui.scale"), app.settings.getBool("ui.scaleAuto"), autoDpi, winW, winH);

    lastUiScale = uiScale;
    // K-35: the plugin windows' bars follow the scale asked for; the limit above is this window's size.
    setPluginWindowUiScale(devUiScale > 0 ? uiscale::effectiveScale(devUiScale, false, 1.0f, 0, 0)
                                          : uiscale::effectiveScale(app.settings.getNumber("ui.scale"), app.settings.getBool("ui.scaleAuto"), autoDpi, 0, 0));
    viewports::setAllowed(app.settings.getBool("ui.detachWindows"));
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    if (winW > 0 && winH > 0) viewports::beginFrame(uiScale, ImVec2(static_cast<float>(winW) / uiScale, static_cast<float>(winH) / uiScale));
    xdragui::feedPointer();  // J-75: the drag position wins over the backend's pointer fallback
    if (fixedDt > 0) io.DeltaTime = static_cast<float>(fixedDt);
    else if (!devInput.empty()) io.DeltaTime = std::min(io.DeltaTime, kDevInputMaxStep);
    if (winW > 0 && winH > 0) {
      io.DisplaySize = ImVec2(static_cast<float>(winW) / uiScale, static_cast<float>(winH) / uiScale);
      io.DisplayFramebufferScale = ImVec2(static_cast<float>(fbW) / io.DisplaySize.x, static_cast<float>(fbH) / io.DisplaySize.y);
    }
    {  // queued mouse positions are in window pixels: map them to logical pixels
      ImGuiContext& g = *ImGui::GetCurrentContext();
      for (ImGuiInputEvent& e : g.InputEventsQueue) {
        if (e.EventId <= lastScaledEvent) continue;
        lastScaledEvent = e.EventId;
        if (e.Type == ImGuiInputEventType_MousePos && e.MousePos.PosX > -FLT_MAX * 0.5f) {
          e.MousePos.PosX /= uiScale;
          e.MousePos.PosY /= uiScale;
        }
      }
      if (!devInput.empty()) {
        devDragScale = uiScale;
        if (!devInputLive.empty()) {  // K-9: commands of a test harness, for the next frame
          std::error_code liveEc;
          if (fs::exists(fs::u8path(devInputLive), liveEc)) {
            for (DevInputLine& l : loadDevInput(devInputLive)) {
              l.frame = frameNo + 1;
              devInput.push_back(std::move(l));
            }
            fs::remove(fs::u8path(devInputLive), liveEc);
          }
        }
        devstress::frame(app);  // a seeded stress run, if one was asked for
        runDevInput(app, devInput, frameNo);
        lastScaledEvent = g.InputEventsNextEventId - 1;  // already in logical pixels
      }
    }
    if (gVideoFps > 0) videoClockStep(app.engine);  // K-10: after the frame's commands, so a seek holds for this frame
    if (pointerAt < 0 || now - pointerAt >= 3.0) {  // J-111g: the system pointer's size (Settings may change it)
      pointerSize = host::native::pointerSize(pointerNative);
      pointerAt = now;
    }
    collabui::setPointerSize(static_cast<float>(pointerSize.arrowH) / uiScale, pointerSize.source);
    scrollui::configure(app.settings);  // J-67: smooth scrolling / elastic edges for every ImGui scroll region
    ui::SetWindowRubber(scrollui::params().elastic);  // J-117: the same switch for the windows' elastic boundary
    scrollui::beforeNewFrame();
    edzoomui::mapPointer();  // K-19: the pointer over a magnified plugin editor, in its design coordinates
    if (layout::Preset to; layoutui::takeSwitch(&to)) {
      // K-79: another layout preset. The current arrangement is kept in its own file, the other
      // preset's file is read back (ImGui 1.92 rebuilds the dock nodes and re-applies every
      // window's saved place on a runtime load), or its default is built when it has none yet.
      static std::string iniCache[layout::kPresetCount];  // this run's arrangement of each preset
      const double switchStart = SDL_GetPerformanceCounter() / static_cast<double>(SDL_GetPerformanceFrequency());
      {
        size_t n = 0;
        const char* d = ImGui::SaveIniSettingsToMemory(&n);
        iniCache[static_cast<int>(layoutui::active())].assign(d, n);
      }
      if (io.IniFilename) ImGui::SaveIniSettingsToDisk(io.IniFilename);
      layoutui::setActive(to);
      if (to == layout::Preset::Studio) app.showLivePads = false;  // a floating window there, opened from View
      iniPath = (fs::path(app.configDir()) / layout::iniFileName(to)).string();
      io.IniFilename = keepSavedLayout ? nullptr : iniPath.c_str();
      const bool current = static_cast<int>(app.settings.getNumber(layout::versionKey(to))) >= layout::defaultVersion(to);
      std::error_code ec;
      const std::string& cached = iniCache[static_cast<int>(to)];
      // The open project, its selection, playback and the open windows stay as they are; only
      // the arrangement changes (the live layout adds its pads view behind the playlist).
      if (!cached.empty()) {
        ImGui::LoadIniSettingsFromMemory(cached.c_str(), cached.size());
      } else if (current && fs::exists(iniPath, ec)) {
        ImGui::LoadIniSettingsFromDisk(iniPath.c_str());
      } else {
        layoutRebuild = layoutRebuildKeep = true;
      }
      layoutui::enterPreset(app, to);
      layoutui::noteSwitched(ImGui::GetTime());
      if (devSwitchCheck) {
        const double ms = (SDL_GetPerformanceCounter() / static_cast<double>(SDL_GetPerformanceFrequency()) - switchStart) * 1000.0;
        std::fprintf(stderr, "[layout-switch] frame %d to %s took %.2f ms\n", frameNo, layout::id(to), ms);
        devSwitchFrame = frameNo;
      }
    }
    winframe::sync();  // K-177: the desktop title bar follows the theme (new windows, theme changes)
    if (static const char* dumpAt = std::getenv("GL_THEME_DUMP"); dumpAt && std::atoi(dumpAt) == ImGui::GetFrameCount()) {
      // K-177 tests: the palette and the colours the top bar, browser and playlist drew last frame.
      static char buf[2048];
      theme::devDump(buf, sizeof buf);
      const winframe::Look fl = winframe::lastApplied();
      std::fprintf(stderr, "THEME_DUMP frame %d %s frame.dark=%d frame.caption=#%06X frame.count=%d\n", ImGui::GetFrameCount(), buf, fl.dark ? 1 : 0,
                   static_cast<unsigned>(fl.caption), winframe::applyCount());
    }
    ImGui::NewFrame();
    keys::escapeFrameBegin();  // K-124: what Esc may meet this frame (popups, drags, text fields)
    app.runLater();  // song edits picked in menus last frame (App::later)
    if (crashTestDirty && !app.doc.song().channels.empty()) {  // K-39 tests: unsaved work to recover
      crashTestDirty = false;
      app.doc.renameChannel(app.doc.song().channels[0].id, "k39-unsaved-marker");
    }
    if (!crashForce.empty() && ++crashForceCount >= crashForceFrame) {  // K-39 tests only (see above)
      if (crashForce == "exception-always") throw std::runtime_error("forced repeating failure");
      const std::string k = crashForce;
      crashForce.clear();
      crash::forceForTest(k);
    }
    const bool alignFrame = !devAlignAudit.empty() && !shotPath.empty() && frameNo == shotFrames - 1;
    if (alignFrame) alignaudit::begin();  // J-128
    pointerGestureFrame(app);  // one undo step per pointer gesture, in every editor
    viewports::afterNewFrame();
    scrollui::afterNewFrame();
    collabui::beginFrame(app);  // J-111d: pointer anchors collected while the windows draw
    ui::ClampTrackedWindows();  // J-91 / J-117: floating windows kept in the work area (elastic) before they are laid out
    ui::wheelFrameBegin();     // J-83: wheel routing over controls in scrolling panes
    xdragui::beginFrame(app);  // J-75: an external drag is an ImGui drag from outside
    devTraceWheel = ImVec2(io.MouseWheelH, io.MouseWheel);
    {  // splash -> main window: the workspace fades in from the splash surface over the first frames
      static int introFrame = 0;
      if (introFrame < 12) {
        const float t = static_cast<float>(introFrame++) / 12.0f;
        ImGuiViewport* fvp = ImGui::GetMainViewport();
        ImGui::GetForegroundDrawList(fvp)->AddRectFilled(fvp->Pos, ImVec2(fvp->Pos.x + fvp->Size.x, fvp->Pos.y + fvp->Size.y),
                                                         theme::rgb(0x0B0B0B, (1.0f - t) * (1.0f - t)));
      }
    }

    // J-141: the welcome launcher replaces the project view while it shows (the project stays
    // open behind it); dialogs, settings and notifications still draw over it.
    const bool homeShown = app.home.visible();
    if (homeShown) {
      prof::Scope ps("launcher");
      launcherui::draw(app);
    } else {
    launcherui::drawReturnFade(app);
    float topH = 0;
    {
      prof::Scope ps("topbar");
      topH = drawTopBar(app);
    }
    // Dock space between the top bar and the status bar.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 dpos(vp->WorkPos.x, vp->WorkPos.y + topH), dsize(vp->WorkSize.x, vp->WorkSize.y - topH - theme::kStatusBarH);
    ImGui::SetNextWindowPos(dpos);
    ImGui::SetNextWindowSize(dsize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::pal().surface950));
    ImGui::Begin("##dockhost", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGuiID dockId = ImGui::GetID("GridlineDock");
    // J-28: saved layouts from before the current default are replaced once (K-79: per preset).
    const layout::Preset preset = layoutui::active();
    static bool builtThisRun[layout::kPresetCount] = {};  // (a --layout run stores no version)
    const bool migrate = static_cast<int>(app.settings.getNumber(layout::versionKey(preset))) < layout::defaultVersion(preset) &&
                         !builtThisRun[static_cast<int>(preset)];
    // Dev layout options (--show mixer / playlist, --hide) ask for the default layout they adjust,
    // whatever layout.ini holds (it is read since J-113).
    static bool devLayout = devMixerOnly || devPlaylistOnly || !devHide.empty();
    if (firstRun || migrate || app.resetLayout || devLayout || layoutRebuild || !ImGui::DockBuilderGetNode(dockId)) {
      devLayout = false;
      layoutRebuild = false;
      const ImVec2 inner(dsize.x - 8, dsize.y - 8);  // dock host padding
      if (app.resetLayout) viewports::gatherAll();  // J-113: detached windows come back too
      const workspace::Rect r = buildLayout(preset, dockId, inner, static_cast<int>(app.doc.song().channels.size()));
      rackPlacement = {dpos.x + 4 + r.x, dpos.y + 4 + r.y, r.w, r.h};
      placeRack = preset == layout::Preset::Studio;  // the other presets dock the rack
      if (!layoutui::hasDevOverride()) app.settings.set(layout::versionKey(preset), layout::defaultVersion(preset));
      builtThisRun[static_cast<int>(preset)] = true;
      firstRun = false;
      app.resetLayout = false;
      if (!layoutRebuildKeep) layoutui::showPresetWindows(app, preset);
      layoutRebuildKeep = false;
      if (devPlaylistOnly) app.showBrowser = app.showRack = app.showPiano = app.showMixer = false;
      for (const std::string& w : devHide) {
        if (w == "rack") app.showRack = false;
        else if (w == "browser") app.showBrowser = false;
        else if (w == "mixer") app.showMixer = false;
        else if (w == "playlist") app.showPlaylist = false;
        else if (w == "piano") app.showPiano = false;
      }
      if (devMixerOnly) {
        app.showBrowser = app.showRack = app.showPiano = app.showPlaylist = false;
        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, dsize);
        ImGui::DockBuilderDockWindow("Mixer", dockId);
        ImGui::DockBuilderFinish(dockId);
      }
      if (app.focusWindow.empty()) app.focusWindow = devMixerOnly ? "Mixer" : "Playlist";
    }
    {  // window/scale changed since last frame: refit the docked panels
      static ImVec2 lastDock(0, 0);
      ImVec2 avail(dsize.x - 8, dsize.y - 8);  // dock host padding
      if (lastDock.x > 0 && (std::fabs(avail.x - lastDock.x) > 0.5f || std::fabs(avail.y - lastDock.y) > 0.5f))
        fitDockTree(ImGui::DockBuilderGetNode(dockId), ImVec2(avail.x / lastDock.x, avail.y / lastDock.y), avail);
      lastDock = avail;
    }
    viewports::beginDockSpace(dockId);  // J-114: tab bars get the pop-out / dock-back button
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    viewports::endDockSpace();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    {
      prof::Scope ps("browser");
      drawBrowser(app);
    }
    {
      prof::Scope ps("playlist");
      drawPlaylist(app);
    }
    layoutui::paneGroupsFrame(app);  // K-79: the tracks preset's one-at-a-time panes
    drawLivePads(app);
    trackui::devDemo(app);  // PL-08 / RN-09 screenshot set-ups
    chopui::devDemo(app);   // PL-13 screenshot set-ups
    drawMasterPanel(app);  // RN-09
    {
      prof::Scope ps("pianoroll");
      drawPianoRoll(app);
    }
    {
      prof::Scope ps("rack");
      // J-77: choosing PAT shows the rack: a closed rack reopens where it was, an open one comes to the
      // front (not moved; the keyboard focus stays where it is, e.g. in a text field).
      const RackReveal::Action reveal = app.rackReveal.step(app.engine.mode(), app.showRack);
      if (reveal == RackReveal::Action::Show) app.showRack = true;
      if (placeRack && app.showRack) ImGui::SetNextWindowDockID(0, ImGuiCond_Always);  // default layout: the rack floats
      rackDevBefore(app, frameNo);  // J-84 dev check (--rack)
      drawChannelRack(app);
      if (reveal != RackReveal::Action::None)
        if (ImGuiWindow* rw = ImGui::FindWindowByName("Channel rack")) {
          if (rw->DockIsActive && rw->DockNode) {
            if (rw->DockNode->TabBar) rw->DockNode->TabBar->NextSelectedTabId = rw->TabId;  // docked: its tab comes forward
          } else {
            ImGui::BringWindowToDisplayFront(rw);
          }
        }
      if (placeRack && app.showRack) {  // ... over the playlist's left part (once the window exists)
        if (ImGuiWindow* rw = ImGui::FindWindowByName("Channel rack")) {
          ImGui::SetWindowPos(rw, ImVec2(rackPlacement.x, rackPlacement.y), ImGuiCond_Always);
          ImGui::SetWindowSize(rw, ImVec2(rackPlacement.w, rackPlacement.h), ImGuiCond_Always);
          placeRack = false;
        }
      }
      rackDevAfter(app, frameNo);
    }
    {
      prof::Scope ps("mixer");
      drawMixer(app);
    }
    {
      prof::Scope ps("plugin_editors");
      drawChannelSettings(app);
      drawEffectEditor(app);
      packs::drawWindows(app);  // pack-owned windows (audio editor) and their background work
    }
    }  // J-141: !homeShown
    {
      prof::Scope ps("dialogs");
      drawAutomationEditor(app);
      drawDialogs(app);
      browserui::drawPluginPicker(app);
      cmdpalette::draw(app);  // K-93: over the windows and dialogs, under collaboration and load pop-ups
      collabui::draw(app);  // J-111
      projloadui::draw(app);  // J-167: last, at the root level (it replaces any popup open when a load starts)
      crashui::draw(app);     // K-39: a failure's pop-up goes over everything
    }
    browserui::thumbDraw(app);
    if (!devMenu.empty()) drawDevMenu(app, devMenu);
    if (!devFocus.empty() && frameNo == 3) app.focusWindow = devFocus;
    for (const auto& [f, id] : devSwitches)
      if (frameNo == f) layoutui::choose(app, layout::fromId(id), layout::linkedFromSettings(app.settings));
    if (frameNo == 3)
      for (const std::string& w : devPanes) {  // K-79: screenshots of the presets' panes
        if (w == "mixer") app.showMixer = true;
        else if (w == "piano") app.showPiano = true;
        else if (w == "rack") app.showRack = true;
        else if (w == "history") app.showHistory = true;
        else if (w == "pads") layoutui::toggleMainView(app);
        else if (w == "settings" && !app.doc.song().channels.empty()) app.settingsChannel = app.doc.song().channels.front().id;
        else if (w == "launcher-layout") launcherui::openLayoutChooser();
      }
    if (devHostedEditor && frameNo == 3) openHostedEditor(app, devEditorTarget);
    if (devWindowCycle > 0 && frameNo >= 10 && frameNo % 10 == 0 && frameNo / 10 <= 2 * devWindowCycle) {
      togglePluginWindow(app, devEditorTarget);  // channel button / slot click
      std::fprintf(stderr, "[plugin-window] toggled: %s\n", hostedEditorOpen(devEditorTarget) ? "open" : "closed");
    }
    if (devAutoEdit && frameNo == 2) devAutomationEditor(app, devAutoEdit);
    if (!devCollabDemo.empty() && frameNo == 2) {  // J-111
      gl::collabui::startDemo(app, (std::filesystem::path(app.configDir()) / "collab-demo").string(), devCollabDemo == "collab-cursors" || devCollabDemo == "collab-demo-panel");
      if (devCollabDemo == "collab-review") app.showSyncReview = true;
      if (devCollabDemo == "collab-demo-panel") app.showCollab = true;
    }
    if (devExport && frameNo == 2) {
      if (devExport == 1) gl::exportui::openRender(app, gl::exportui::Preset::Stems);
      if (devExport == 2) gl::exportui::openRender(app, gl::exportui::Preset::PatternReplace);
      if (devExport == 3) {
        app.doc.editSong("dev: missing samples", [](gl::Song& s) {
          const char* paths[] = {"C:/Samples/Drums/Kick 03.wav", "/Volumes/Old drive/Kits/Snare tight.wav", "/Volumes/Old drive/Loops/Breakbeat 92.flac"};
          for (const char* p : paths) {
            gl::Channel c;
            c.id = s.allocateId();
            c.name = std::filesystem::path(p).stem().string();
            c.plugin = gl::defaultPluginState("gl.sampler");
            c.plugin.data = p;
            c.mixerTrack = s.masterId();
            s.channels.push_back(c);
          }
        });
        gl::exportui::openMissingSamples(app);
      }
      if (devExport == 5) {
        gl::exportui::openRender(app, gl::exportui::Preset::Audio);
        gl::exportui::startRenderTo(app, (std::filesystem::temp_directory_path() / "gridline-dev-render").string());
      }
      if (devExport == 4) {
        const std::string mid = (std::filesystem::temp_directory_path() / "gridline-dev-import.mid").string();
        gl::writeMidiFile(mid, gl::songToMidi(app.doc.song()));
        gl::exportui::importMidiFile(app, mid);
      }
    }
    if (!devTool.empty() && frameNo == 3) devOpenTool(app, devTool);
    if (!devScript.empty() && frameNo == 3) devOpenScript(app, devScript);
    if (!devSaveAt.empty() && frameNo == 3) {
      app.saveProject(devSaveAt);
      flpexport::wait();  // J-80: an FL format saves in the background; its card is up on the next frame
      exportui::waitFileJob();  // K-47: so does a zipped project
    }
    if (devChords && frameNo == 3) j31::openChordEditor(app);
    if (!devDialog.empty() && frameNo == 3) {  // a popup opened before frame 3 is closed again
      if (devDialog == "open") app.openDialog();
      if (devDialog == "saveas") app.saveAs();
      if (devDialog == "unsaved") app.confirmDiscard = true;
      if (devDialog == "ppqwarn") devShowPpqWarning(app, 48);
      if (devDialog == "delsource")  // J-82: delete the first used audio clip source (asks first)
        for (const auto& c : app.doc.song().channels)
          if (gl::ops::isAudioClipSource(c) && gl::ops::clipSourceUses(app.doc.song(), gl::ClipKind::Audio, c.id) > 0) {
            gl::requestDeleteSources(app, gl::ClipKind::Audio, {c.id});
            break;
          }
      if (devDialog == "prompt") menus::openPrompt("Rename pattern", "Pattern 1", [](const std::string&) {});
      // Layout checks never list the user's own folders: browse the Gridline source tree.
#ifdef GRIDLINE_SOURCE_ASSETS
      if (devDialog == "open" || devDialog == "saveas") app.fileDialog.dir = fs::path(GRIDLINE_SOURCE_ASSETS).parent_path().string();
#else
      if (devDialog == "open" || devDialog == "saveas") app.fileDialog.dir = fs::path(app.assetsDir).parent_path().string();
#endif
    }
    if (devRemote && frameNo == 3 && !app.doc.song().channels.empty()) {
      if (devRemote == 1) {
        app.openRemoteDialog(channelParam(app.doc.song().channels[0].id, ChannelParam::Volume));
        app.remoteEdit.cc = 74;
        app.remoteEdit.curve = ModCurve::Formula;
        app.remoteEdit.formula = "smoothstep(0, 1, input)";
        app.remoteEdit.takeover = RemoteTakeover::Pickup;
        app.remoteEdit.min = 0.1f;
        app.remoteEdit.max = 0.9f;
      } else {
        app.openRemoteDialog(RemoteAction::PlayPause);
        app.remoteEdit.source = RemoteSource::Note;
        app.remoteEdit.cc = 93;
        app.remoteListening = false;
      }
    }
    if (devLive && frameNo == 3) {
      app.engine.setMode(PlayMode::Live);
      const Song& ls = app.doc.song();
      std::set<int> seen;
      for (const auto& c : ls.playlist)
        if (c.start < ls.live.areaEnd && seen.insert(c.track).second) app.engine.launchClip(c.id);
    }
    if (!app.focusWindow.empty()) {
      ImGui::SetWindowFocus(app.focusWindow.c_str());
      app.focusWindow.clear();
    }
    if (app.typingKeyboard && app.hint.empty()) {
      std::string h = "Typing keyboard: " + typingKeyName('z') + "…" + typingKeyName('/') + " and " + typingKeyName('q') + "…" + typingKeyName('p') +
                      " play from " + midiNoteName(app.typing.baseNote()) + " · " + typingKeyName('[') + " " + typingKeyName(']') + " octave · " +
                      typingKeyName('-') + " " + typingKeyName('=') + " velocity " + std::to_string(app.typing.velocity) + " · " + typingKeyName('\\') + " chord mode";
      if (app.typing.chordMode) h += std::string(" (") + chordTypeName(app.typing.chord) + ", " + typingKeyName('\'') + " next)";
      if (app.stepRecord) h += " · step record: → rest, ⌫ back";
      app.setHint(h);
    }
    if (!homeShown) {
      prof::Scope ps("statusbar");
      drawStatusBar(app);
      if (!app.projectLoadBlocking()) handleGlobalShortcuts(app);  // J-167: no shortcut reaches the project behind
      else pointerGestureFrameEnd(app);
    } else {
      pointerGestureFrameEnd(app);
    }

    xdragui::endFrame(app);  // J-75: a drop no window took opens the files
    ui::wheelFrameEnd();     // J-83
    collabui::drawCursors(app);  // J-111: collaborators' cursors, last: on top of every window, popup and menu
    {
    prof::Scope psRender("render");
    viewports::drawTitleButtons();  // J-114
    if (alignFrame) alignaudit::finish(devAlignAudit);  // J-128
    if (devGallery >= 0) look::drawGallery(static_cast<look::Dir>(devGallery), ImGui::GetTime(), devGalleryPage);  // J-129
    if (!homeShown) layoutui::drawSwitchFade(ImGui::GetTime());  // K-79: a short fade over a layout switch
    if (devSwitchCheck && frameNo > devSwitchFrame && frameNo <= devSwitchFrame + 4) {
      const std::string bad = layoutui::switchCheck(app);
      std::fprintf(stderr, "[layout-switch] frame %d %s playing %d tick %.0f open %s%s%s\n", frameNo, layout::id(layoutui::active()),
                   app.engine.playing() ? 1 : 0, static_cast<double>(app.engine.position()), layoutui::openWindows(app).c_str(),
                   bad.empty() ? " ok" : " BLANK", bad.c_str());
    }
    if (devVideoCursor) drawVideoCursor();  // K-10
    ImGui::Render();
    edzoomui::afterRender();  // K-19: magnified plugin editors
    if (useOffscreen) offscreen.use(true);
    glViewport(0, 0, fbW, fbH);
    ImVec4 bg = theme::vec(theme::pal().chrome);
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!devFitNoDraw && !devNoDraw) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (browserui::thumbMode() && !browserui::thumbCapture(app, fbW, fbH)) running = false;
    if (!profilePath.empty() && shotPath.empty() && ++frameNo >= shotFrames + profileWarmup) running = false;
    if (!devInput.empty()) {  // J-112: for "waitfor"
      devsig::reach(app.engine.playing() ? "transport-playing" : "transport-stopped");
      // K-B: playback has moved on from where it started (the set position), for scripts that check it ran
      if (app.engine.playing() && app.engine.position() >= app.engine.seekPosition() + 1.0) devsig::reach("transport-moved");
      if (app.engine.pluginPlaybackActive()) devsig::reach("plugin-playback");
      {  // J-109: "output-silent" once the output stayed below 1e-5 for 0.2 s (the peak still counts for "sound")
        static double quietSince = -1;
        const float p = app.engine.takeOutputPeak();
        devSoundPeak = std::max(devSoundPeak, p);
        const double now = static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
        if (p >= 1e-5f) quietSince = -1;
        else if (quietSince < 0) quietSince = now;
        else if (now - quietSince >= 0.2) devsig::reach("output-silent");
      }
      collabui::devFrame(app);  // J-111: collab-connected, collab-joined, collab-synced, ...
      if (!host::scanStatus().running) devsig::reach("scan-idle");  // K-71
      const double frameMs = 1e3 * static_cast<double>(SDL_GetPerformanceCounter() - frameStart) / static_cast<double>(SDL_GetPerformanceFrequency());
      if (frameMs > devLongestFrameMs) devSecondFrameMs = devLongestFrameMs, devLongestFrameMs = frameMs;
      else devSecondFrameMs = std::max(devSecondFrameMs, frameMs);
      devFrameMsSum += frameMs;
      ++devFrameCount;
    }
    // J-108: "waitfor" keeps the script (and --frames) on this frame; J-167: so does a project load
    // (scripts and screenshots see the project loaded; "loadhold off" watches a load frame by frame).
    const bool devHold = devWaitHolds() || (devLoadHold && app.projectLoading());
    // J-103: a --dev-input session without --screenshot counts its frames too (its script ends it with "quit").
    if (!devInput.empty() && shotPath.empty() && profilePath.empty() && frameNo < INT_MAX && !devHold) ++frameNo;
    if (devTrace) {  // J-67: the scroll positions this frame was drawn with
      ImGuiContext& g = *ImGui::GetCurrentContext();
      const ImGuiWindow* hw = g.HoveredWindow;
      std::printf("trace frame %d: wheel %.3f %.3f playlist %.2f %.2f piano %.2f %.2f window %s %.2f %.2f\n", frameNo, devTraceWheel.x, devTraceWheel.y,
                  app.playlist.scrollX, app.playlist.scrollY, app.piano.scrollX, app.piano.scrollY, hw ? hw->Name : "-", hw ? hw->Scroll.x : 0.0f,
                  hw ? hw->Scroll.y : 0.0f);
      std::fflush(stdout);
    }
    if (!videoOutPath.empty() && !devHold) {  // K-10: frames the script counts, so its frame numbers are the video's
      if (!videoOut) videoOut = std::fopen(videoOutPath.c_str(), "wb");
      if (videoOut) writeVideoFrame(videoOut, fbW, fbH);
    }
    if (!devShotPending.empty()) {  // J-37: dev-input "shot" (frame sequences)
      writePpm(devShotPending.c_str(), fbW, fbH);
      devShotPending.clear();
    }
    if (!shotPath.empty() && !devHold && ++frameNo >= shotFrames) {
      // Save the rendered frame (binary PPM) and quit — used for visual checks.
      writePpm(shotPath.c_str(), fbW, fbH);
      if (devHostedEditor && !screenshotHostedEditor(devEditorTarget, shotPath + ".editor.png"))
        std::fprintf(stderr, "no editor screenshot\n");
      running = false;
    }
    if (useOffscreen) offscreen.use(false);
    if (const char* why = glrecovery::poll()) graphicsLost = why;  // K-47: seen on the main context, dealt with between frames
    fstats::since(fstats::Ui, statT);
    statT = fstats::stamp();
    viewports::renderWindows();  // J-113: the detached windows
    SDL_GL_SwapWindow(window);
    fstats::since(fstats::Present, statT);
    statT = fstats::stamp();
    renderHostedEditors(app);  // control bars of open plugin windows
    fstats::since(fstats::PluginWindows, statT);
    if (frameClock && !live) {  // J-66: wait for the next refresh inside the run loop (plugin interfaces draw meanwhile; not from a blocked pump, J-116)
      statT = fstats::stamp();
      static unsigned followTick = 0;
      if ((++followTick & 63u) == 1u) host::native::frameClockFollow(mainNative);  // the window may move to another display
      host::native::frameClockWait();
      fstats::since(fstats::Wait, statT);
    }
    }
    if (!app.uiSoundsReady && ++liveFrames >= 3) {  // J-48: the window is on screen: startup sound, once
      app.uiSoundsReady = true;
      app.uiSound(uisound::Event::Startup);
    }

    {
      prof::Scope ps("sync");
      // Sync the engine after the UI's edits this frame.
      app.syncEngine();
      std::string title = app.windowTitle();
      if (title != lastTitle) {
        SDL_SetWindowTitle(window, title.c_str());
        lastTitle = title;
      }
    }
    liveresize::frameEnd();
  };
  // K-39: a frame that throws is reported and left behind; the app goes on. Three in a row: what
  // draws the interface is broken for now, so the pop-up alone, until the user decides.
  int failedFrames = 0, goodFrames = 0;
  auto guardedFrame = [&](bool live) {
    crash::uiHeartbeat();
    try {
      frame(live);
      if (failedFrames && ++goodFrames > 30) failedFrames = 0;
    } catch (...) {
      crash::reportCurrentException("Drawing the interface");
      if (ImGui::GetCurrentContext()->WithinFrameScope) ImGui::EndFrame();  // closes what the frame left open
      if (ImGui::GetCurrentContext()->FrameCountPlatformEnded < ImGui::GetFrameCount()) ImGui::UpdatePlatformWindows();
      liveresize::frameEnd();
      goodFrames = 0;
      ++failedFrames;
    }
  };
  auto keepsFailing = [&]() {
    // Until the user decides. An automated run takes its picture and ends.
    for (int shown = 0;; ++shown) {
      crash::uiHeartbeat();
      SDL_Event e;
      while (SDL_PollEvent(&e)) ImGui_ImplSDL2_ProcessEvent(&e);
      int winW = 0, winH = 0, fbW = 0, fbH = 0;
      SDL_GetWindowSize(window, &winW, &winH);
      SDL_GL_GetDrawableSize(window, &fbW, &fbH);
      if (useOffscreen) {
        winW = fbW = offscreen.w;
        winH = fbH = offscreen.h;
      }
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      if (winW > 0 && winH > 0) {
        io.DisplaySize = ImVec2(static_cast<float>(winW) / lastUiScale, static_cast<float>(winH) / lastUiScale);
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(fbW) / io.DisplaySize.x, static_cast<float>(fbH) / io.DisplaySize.y);
      }
      ImGui::NewFrame();
      const crashui::KeepsFailingResult r = crashui::drawKeepsFailing(app);
      ImGui::Render();
      ImGui::UpdatePlatformWindows();
      if (useOffscreen) offscreen.use(true);
      glViewport(0, 0, fbW, fbH);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      const bool automated = !shotPath.empty() && shown >= 5;
      if (automated) writePpm(shotPath.c_str(), fbW, fbH);
      if (useOffscreen) offscreen.use(false);
      SDL_GL_SwapWindow(window);
      if (automated) {
        crash::markCleanExit();
        std::fflush(nullptr);
        std::_Exit(0);
      }
      if (r.done) {
        if (r.retry) {
          failedFrames = 0;
          return;
        }
        crash::markCleanExit();  // the copy is saved and the new start is under way
        std::fflush(nullptr);
        std::_Exit(0);
      }
    }
  };
  liveresize::install(window, [&guardedFrame]() { guardedFrame(true); });
  // K-47: the context is gone (a driver update, a driver timeout): it and what lives in it are made
  // again; the project and the sound never noticed. False: no new context could be had.
  int graphicsRecoveries = 0;
  std::vector<double> graphicsLosses;
  auto recoverGraphics = [&](const char* why) {
    crash::BusyScope busy("graphics driver reset", 60);
    crash::crumb("graphics reset", why);
    logging::warning("graphics", std::string("The OpenGL context was lost: ") + why);
    const int interval = SDL_GL_GetSwapInterval();
    glrecovery::waitForResetToEnd(10000);
    pluginWindowsGraphicsLost();
    ImGui_ImplOpenGL3_Shutdown();  // (the detached windows go here; they come back on the next frame)
    ImGui_ImplSDL2_Shutdown();
    SDL_GL_MakeCurrent(window, nullptr);
    SDL_GL_DeleteContext(gl);
    gl = glrecovery::recreateContext(window, 10000);
    if (!gl) return false;
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(interval);
    glrecovery::attach();
    glsl = glrecovery::glsl();
    crash::setGpu(reinterpret_cast<const char*>(glGetString(GL_VENDOR)), reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                  reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init(glsl);
    if (std::getenv("GRIDLINE_DEV_NO_VTX_OFFSET")) ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasVtxOffset;
    keepDevClipboard();
    viewports::backendRestarted(gl);
    glepoch::next();
    glrecovery::restoreTextures();
    pluginWindowsGraphicsBack(window, gl);
    if (useOffscreen) offscreen.create(offscreen.w, offscreen.h);
    reopenHostedEditorsAfterGraphicsReset(app);  // K-133: a plugin's own OpenGL is built again by the plugin
    SDL_GL_MakeCurrent(window, gl);
    ++graphicsRecoveries;
    devsig::reach("graphics-recovered");
    return true;
  };
  auto dealWithGraphicsLoss = [&]() {
    const char* why = graphicsLost;
    graphicsLost = nullptr;
    // A driver that keeps resetting is not coming back either.
    const double nowSeconds = static_cast<double>(SDL_GetTicks()) / 1000.0;
    graphicsLosses.erase(std::remove_if(graphicsLosses.begin(), graphicsLosses.end(), [&](double t) { return nowSeconds - t > 60.0; }), graphicsLosses.end());
    graphicsLosses.push_back(nowSeconds);
    bool back = false;
    if (graphicsLosses.size() <= 3) {
      try {
        back = recoverGraphics(why);
      } catch (...) {
        crash::reportCurrentException("Rebuilding the display");
      }
    }
    if (!back) {
      try {
        captureHostedStates(app);  // (the copy below holds what the plugins say now)
      } catch (...) {
        crash::crumb("graphics: plugin state not read");
      }
      crash::endAfterGraphicsLoss(why);
    }
    app.notify("The graphics driver restarted. Gridline rebuilt its display, and your project and the sound are as they were.");
  };
  crash::armWatchdog();
  // K-153: on SDL's Wayland driver a drop waits for the pointer to come back into the window (no
  // pointer position during a drag there; core/ExternalDrop.h DropTiming).
  xdrop::DropTiming dropTiming;
  const bool waylandDriver = [] {
    const char* d = SDL_GetCurrentVideoDriver();
    return d && std::strcmp(d, "wayland") == 0;
  }();
  while (running) {
    prof::beginFrame();
    SDL_Event ev;
    uint64_t statT = fstats::stamp();
    liveresize::pumpBegin();
    std::vector<SDL_Scancode> pendingUps;  // K-175: typed keys let go in this batch of events
    auto endTypedKey = [&](SDL_Scancode sc) {
      const auto h = held.find(sc);
      const ObjectId chId = h != held.end() ? h->second.channel : app.selChannel;
      if (h != held.end()) held.erase(h);
      for (int p : app.typing.release(sc)) {
        app.engine.noteOff(chId, p);
        app.playedNoteOff(chId, p);
      }
    };
    while (SDL_PollEvent(&ev)) {
      if (gl::opendocs::take(ev)) continue;  // K-119: a file macOS asks the running Gridline to open (below)
      ImGui_ImplSDL2_ProcessEvent(&ev);
      viewports::onEvent(ev);  // J-113: focus over Gridline's windows, minimise
      // K-175: a held typed key ends on its key-up whatever window gets it (a plugin window took the
      // focus meanwhile) and whatever changed meanwhile (a modifier pressed, a text field focused,
      // the typing keyboard switched off, a project loading, no channel selected); at the end of
      // this batch of events, see below.
      if (ev.type == SDL_KEYUP && (held.count(ev.key.keysym.scancode) || app.typing.isDown(ev.key.keysym.scancode)) &&
          std::find(pendingUps.begin(), pendingUps.end(), ev.key.keysym.scancode) == pendingUps.end())
        pendingUps.push_back(ev.key.keysym.scancode);
      // K-175: as the keyboard focus moves between windows the system may let go of the keys held and
      // report them down again in the new window (SDL on X11 does). A key that comes back down that way
      // never went up: it keeps its notes and is no new press. Other keys reported down then are keys
      // held from before (pressed in another application): they neither play nor move the octave.
      if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) app.typing.focusGained();
      if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
        const auto up = std::find(pendingUps.begin(), pendingUps.end(), ev.key.keysym.scancode);
        if (up != pendingUps.end()) {
          pendingUps.erase(up);
          if (app.typing.settling()) continue;
          endTypedKey(ev.key.keysym.scancode);  // released and pressed again in one batch: both count
        }
      }
      if (hostedEditorEvent(app, ev)) continue;  // plugin editor windows
      if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                                  ev.window.windowID == SDL_GetWindowID(window)))
        app.quitRequested = true;
      // J-55: "Follow system" picks up a desktop appearance change when Gridline is focused again.
      if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED &&
          ev.window.windowID == SDL_GetWindowID(window) && theme::modeFromSettings(app.settings) == theme::Mode::System)
        theme::applyFromSettings(app.settings, true);
      // J-75: files dropped by other applications. macOS reports them (with
      // the hover) through xdrop::native; elsewhere SDL reports them on release:
      // collected, then dropped at the pointer like any drag.
      if (ev.type == SDL_DROPBEGIN) xdragui::sdlDropBegin();
      if (ev.type == SDL_DROPFILE || ev.type == SDL_DROPTEXT) {
        const std::string text = ev.drop.file ? ev.drop.file : "";
        SDL_free(ev.drop.file);
        if (ev.type == SDL_DROPFILE) xdragui::sdlDropFile(text);
        else xdragui::sdlDropText(text);
      }
      if (ev.type == SDL_DROPCOMPLETE && dropTiming.completed(waylandDriver, SDL_GetTicks() / 1000.0)) {
        int mx = 0, my = 0, wx = 0, wy = 0;
        SDL_GetGlobalMouseState(&mx, &my);
        SDL_GetWindowPosition(window, &wx, &wy);
        xdragui::sdlDropComplete(static_cast<float>(mx - wx), static_cast<float>(my - wy));
      }
      if (ev.type == SDL_MOUSEMOTION && ev.motion.windowID == SDL_GetWindowID(window) && dropTiming.pointerMoved())
        xdragui::sdlDropComplete(static_cast<float>(ev.motion.x), static_cast<float>(ev.motion.y));
      // Typing keyboard (key events straight from SDL so key-up is never missed; key-ups above).
      // K-175: one press, one step: TypingKeyboard::press ignores repeats and keys already down.
      if (ev.type == SDL_KEYDOWN && app.typingKeyboard && !io.WantTextInput && !app.projectLoadBlocking() &&
          !(ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI | KMOD_ALT))) {
        const SDL_Keycode k = ev.key.keysym.sym;
        const SDL_Scancode sc = ev.key.keysym.scancode;
        if (!ev.key.repeat && app.stepRecord && !app.engine.playing() && (k == SDLK_RIGHT || k == SDLK_BACKSPACE)) {
          if (k == SDLK_RIGHT)
            app.stepRecorder.rest();
          else
            app.stepRecorder.backspace(app.doc, app.curPattern, app.selChannel);
        }
        if (typingKey(sc) > 0 && app.selChannel != kNoId) {
          std::vector<int> on = app.typing.press(sc, ev.key.repeat != 0);
          for (int p : on) {
            app.engine.noteOn(app.selChannel, p, app.typing.velocity / 127.0f);
            app.playedNoteOn(app.selChannel, p, app.typing.velocity);
          }
          if (!on.empty()) held[sc] = {app.selChannel, 0, 0};
        }
      }
    }
    for (SDL_Scancode sc : pendingUps) endTypedKey(sc);
    pendingUps.clear();
    app.typing.settle();  // K-175: keys pressed from the next batch of events on are presses again
    // K-175: while the typing keyboard plays, its keys run no other command (keypad - + also
    // switching the pattern, - = zooming, a single-key shortcut set's letters).
    keys::setTypingKeyboard(app.typingKeyboard && app.selChannel != kNoId);
    liveresize::pumpEnd();
    if (dropTiming.expired(SDL_GetTicks() / 1000.0)) {  // K-153: Wayland, the pointer did not come back
      int mx = 0, my = 0;
      SDL_GetMouseState(&mx, &my);
      xdragui::sdlDropComplete(static_cast<float>(mx), static_cast<float>(my));
    }
    // K-121: a later launch handed its files over (Linux, Windows): they open as macOS's requests do
    // below. Every launch brings the window forward (K-153: with the launch's activation token, which
    // a Wayland compositor wants before it lets a window come forward).
    for (gl::single::Request& r : instance.take()) {
      std::printf("single instance: request with %zu file(s), activation token %s\n", r.files.size(), r.activation.empty() ? "none" : "received");
      if (!r.files.empty()) gl::opendocs::queue(r.files);
      if (!startHidden)
        std::printf("single instance: brought forward (%s)\n", gl::activate::bringForward(window, r.activation) ? "activation token to the compositor" : "raise");
      std::fflush(stdout);
    }
    // K-119: files the system asked to open (macOS: a double click in the Finder, Open with, the Dock icon),
    // once no project is loading: as a double click in the browser, the unsaved-changes prompt first.
    if (gl::opendocs::pending() && !app.projectLoadBlocking()) {
      const std::vector<std::string> files = gl::opendocs::takeReady();
      if (hiddenRun)
        for (const std::string& f : files) std::printf("os open: %s (running)\n", f.c_str());
      std::fflush(stdout);
      if (!startHidden) {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(window);  // K-121
        SDL_RaiseWindow(window);
      }
      gl::opendocs::open(app, files);
    }
    fstats::since(fstats::Events, statT);
    if (graphicsLost) dealWithGraphicsLoss();
    guardedFrame(false);
    if (failedFrames >= 3) keepsFailing();
    prof::endFrame();
    fstats::endFrame();
  }
  liveresize::uninstall();
  instance.stop();          // K-121: the next launch starts a Gridline of its own (its socket goes)
  crashui::shutdown(app);   // K-39: the user answered "save changes?"; this run's copy goes
  crash::markCleanExit();   // what follows is teardown: a failure there still gets a report
  if (!profilePath.empty()) {
    std::string label = std::to_string(forcedSize.first) + "x" + std::to_string(forcedSize.second);
    if (prof::writeJson(profilePath, label))
      std::fprintf(stderr, "profile: %d frames -> %s\n", prof::recordedFrames(), profilePath.c_str());
  }

  if (!devSaveAtExit.empty()) {
    captureHostedStates(app);
    std::fprintf(stderr, "%s %s\n", app.saveProject(devSaveAtExit) && (flpexport::wait(), exportui::waitFileJob(), true) ? "saved" : "could not save", devSaveAtExit.c_str());
  }
  // J-103: no background thread may run into static teardown (a plugin scan finishing then
  // locked the destroyed class registry mutex: "mutex lock failed", abort on quit).
  host::shutdownScan();        // the scanner child is killed; unscanned entries wait for the next start
  exportui::shutdown();        // a running audio export / render to sampler is cancelled and joined
  stemsui::shutdown();         // J-191: a running stem split / model download likewise
  receiptui::shutdown();       // J-219: a running Track Receipt measurement likewise
  j59::shutdownRenderSampler();
  browserui::shutdownIndex();  // J-46: no worker left running into static teardown
  updui::shutdown();           // J-89: a running installer download is cancelled
  urlimportui::shutdown();     // J-192: running downloads / conversions are stopped
  app.shutdownProjectLoad();   // J-167: a load in progress stops; its reader is joined
  // K-9: the project is dealt with by now; a plugin that never returns from closing (a bridged
  // one whose Windows side hangs) must not leave an invisible process behind.
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(20));
    std::fprintf(stderr, "quit: a plugin did not close in 20 seconds, leaving anyway\n");
    std::_Exit(0);
  }).detach();
  closeAllHostedEditors(app);
  settleClosingHostedEditors();
  if (videoOut) std::fclose(videoOut);
  if (frameClock) host::native::frameClockStop();
  if (gDevice) SDL_CloseAudioDevice(gDevice);
  if (gInDevice) SDL_CloseAudioDevice(gInDevice);  // (SDL_Quit would too; explicit, like the output)
  gAsio.close();  // K-6: the driver stops before the engine goes
  // J-103: the static sample cache's decoders and the clip render thread are joined here,
  // not by their static destructors (a decode or render still running then would use
  // statics already destroyed). Offline work after this point decodes / renders inline.
  sampleCache().shutdown();
  stretch::shutdownClipRenderer();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  const int exitCode = browserui::auditIssueCount() > 0 ? 1 : 0;  // Q-7: --layout-audit fails on any inset issue
  if (hostedEditorAbandoned()) {  // K-37: nothing may tear the bridge down around that thread
    std::fprintf(stderr, "quit: a plugin's interface never closed, leaving without teardown\n");
    std::fflush(nullptr);
    std::_Exit(exitCode);
  }
  return exitCode;
}

#if defined(_WIN32)
// Windows GUI entry: the command line as UTF-8 arguments (the process code page is UTF-8 too,
// see assets/windows/gridline.manifest), then main().
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string a(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
    args.push_back(a);
  }
  LocalFree(wargv);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  return main(argc, argv.data());
}
#endif
