// Gridline — application entry: SDL2 window + OpenGL 3 + Dear ImGui, audio
// device, default layout, typing keyboard, file drops.
#include "GridlineVersion.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include "SysWm.h"
#if defined(_WIN32)
#include <shellapi.h>  // CommandLineToArgvW
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../engine/ChannelIns.h"
#include "../engine/Sample.h"
#include "../engine/TimeStretch.h"
#include "DevSignal.h"
#include "FlpExportUi.h"
#include "J59Ui.h"
#include "FrameProfile.h"
#include "FrameStats.h"
#include "McpUi.h"
#include "ThemeSettings.h"
#include "LiveResize.h"
#include "Viewports.h"
#include "../core/I18n.h"
#include "../core/UiScale.h"
#include "HostedUi.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/vst3/Vst3Scan.h"
#include "../host/ui/NativeView.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_impl_sdl2.h"
#include "../../third_party/imgui/imgui_internal.h"
#include <cfloat>
#include <climits>
#include "../core/ClipSources.h"
#include "../core/Project.h"
#include "../core/Workspace.h"
#include "../packs/PacksUi.h"
#include "DialogLayout.h"
#include "ExportUi.h"
#include "../core/MidiFile.h"
#include "App.h"
#include "CollabUi.h"
#include "PluginUI.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "ContextMenus.h"
#include "EditTools.h"
#include "J31Ui.h"
#include "MenuParityUi.h"
#include "ScriptUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"
#include "TrackModeUi.h"
#include "ChopUi.h"
#include "Shell.h"
#include "SmoothScrollUi.h"
#include "ExternalDragUi.h"
#include "SuiteUi.h"
#include "UpdateUi.h"
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
  a.midi->scan();  // new devices are enabled with the next free port number
  a.midi->apply();
  a.loadControllerTemplates();
  a.publishMidi();
}

void audioCallback(void* user, Uint8* stream, int len) {
  auto* eng = static_cast<AudioEngine*>(user);
  const int frames = len / static_cast<int>(sizeof(float) * 2);
  const bool stats = fstats::enabled();
  const Uint64 t0 = stats ? SDL_GetPerformanceCounter() : 0;
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
  if (a.audioInputDevice.empty()) {
    a.engine.setInputChannels(0);
    return;
  }
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

void openAudio(App& a) {
  if (gInDevice) {
    SDL_CloseAudioDevice(gInDevice);
    gInDevice = 0;
  }
  if (gDevice) {
    SDL_CloseAudioDevice(gDevice);
    gDevice = 0;
  }
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
    a.uiSounds.loadAll((fs::path(a.assetsDir) / "sounds").string(), have.freq);
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


// Fallback window icon when the assets folder is missing: the Ink mark.
SDL_Surface* makeIcon(int size) {
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
  if (!s) return nullptr;
  auto cov = markCoverage(size);
  auto* px = static_cast<Uint8*>(s->pixels);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      Uint8* p = px + y * s->pitch + x * 4;
      p[0] = p[1] = p[2] = 0;
      p[3] = cov[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)];
    }
  return s;
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

// Typing keyboard: SDL key → the ASCII key code TypingKeyboard uses (keypad
// −/+ change the octave, keypad / * the velocity). 0 = not a typing key.
int typingKey(SDL_Keycode k) {
  switch (k) {
    case SDLK_KP_MINUS: return '[';
    case SDLK_KP_PLUS: return ']';
    case SDLK_KP_DIVIDE: return '-';
    case SDLK_KP_MULTIPLY: return '=';
    default: break;
  }
  if (k > 0 && k < 128 && (TypingKeyboard::keyOffset(static_cast<int>(k)) >= 0 || TypingKeyboard::isControlKey(static_cast<int>(k))))
    return static_cast<int>(k);
  return 0;
}

struct HeldNote {
  ObjectId channel;
  int pitch;
  double startTick;
};

// J-109: ends every note held on the typing keyboard (on the channel each key started it on).
void releaseTypedNotes(App& app, std::map<SDL_Keycode, HeldNote>& held) {
  for (const auto& [k, h] : held)
    for (int p : app.typing.keyUp(typingKey(k))) {
      app.engine.noteOff(h.channel, p);
      app.playedNoteOff(h.channel, p);
    }
  for (int p : app.typing.releaseAll()) {  // (keys not in `held`)
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
//   key space (J-49) · key escape, notes (Q-3: piano roll notes to stdout) · transport (J-49: song position / playing / seek position to stdout)
//   scroll dx dy (J-67: one trackpad-style two-axis scroll event, wheel units)
//   momentum dx dy frames (J-67: a decaying scroll stream over the next frames, like an OS fling)
//   trace on|off (J-67: per frame, the drawn scroll positions to stdout)
//   J-75 drags from other applications (logical pixels): dragenter x y <file|promise:file|"path with spaces">...,
//   dragmove x y, drop [x y], dragleave · undo · channels (channel names / samples / routing / steps to stdout)
//   (J-97: a "plugin:<type>" item drags a plugin like the browser does; J-110: "clipsrc:last" (the newest audio
//   clip source, a take just recorded) or "clipsrc:<channel index>" drags it like the playlist picker does)
//   mixer (J-83: track volumes / pans, the strips pane's scroll and wheel policy to stdout)
//   rack (J-84: channel rack layout, step scroll, channel volumes / pans, step velocities / pitches to stdout)
//   quit [discard] (J-103: an SDL_QUIT, the normal quit path: unsaved-changes guard, teardown, settings saved;
//   "discard" drops unsaved changes first, as the prompt's "Don't save")
//   J-109: sdlkey <key> down|up [ctrl|super] · focuslost · typing on|off · noteon|noteoff <channel index> <pitch> ·
//   previewfile <wav> · sound (output peak since the last "sound", live notes held, preview, panics, playing)
//   J-110: stopvia button|menu|remote (the Stop button's action, Tools > Stop all sound, a MIDI controller's /
//   remote Stop) · epoch (the engine's stop epoch to stdout) · forget <name> (a later waitfor <name> waits for it anew)
//   J-112: playvia remote (the Play / Pause remote / MIDI action) · startsong (a start that is not the Play command, as a
//   playlist click) · playstate (transport, plugin playback, stop epoch, the focused window to stdout) · focus <window>;
//   reported each frame for waitfor: transport-playing / transport-stopped, plugin-playback
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
//   -synced / -members:<n> / -channel:<name> / -name:<project name>
//   J-111d: collab point <window> <kind> <a> <b> <cx> <cy> (the pointer to a cursor anchor, CursorAnchor.h) ·
//   collab demo-cursor <same> (the simulated collaborator's pointer, --show collab-cursors) · collab cursors <file>
//   (JSON: cursors shown and what each is over here) · collab pixel <file>; signals collab-cursor:<kind>:shown|hidden,
//   collab-cursor-window:<window>:shown|hidden
//   J-111g: collab picture <file|#RRGGBB> · collab panel · collab pictures <file> (JSON: members' pictures announced,
//   received, kept, drawn, a pixel) · collab open-editor chN · collab demo-cursor-plugin chN|none · collab trace-reset;
//   signals collab-picture:<name> (drawn here), collab-picture-kept:<name> (kept on this computer, the one drawn)
//   waitfor <name> <seconds> (J-108: the script and --frames stop counting until the app reports
//   devsig::reach(name) or the seconds pass; prints "waitfor <name>: reached|timed out after <s> s")
std::string devShotPending;
bool devTrace = false;
ImVec2 devTraceWheel;  // this frame's wheel input (io clears it at the end of the frame)
struct DevWait {
  std::string name;
  double seconds = 0;
  Uint64 start = 0;
  bool on = false;
} devWait;
// J-108: true while a "waitfor" holds the script's frame count (checked once per frame).
bool devWaitHolds() {
  if (!devWait.on) return false;
  const double t = static_cast<double>(SDL_GetPerformanceCounter() - devWait.start) / static_cast<double>(SDL_GetPerformanceFrequency());
  const bool reached = devsig::reached(devWait.name);
  if (!reached && t < devWait.seconds) return true;
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
void devPianoCommand(App& app, const DevInputLine& l, int frame) {
  const Pattern* pat = app.doc.song().pattern(app.curPattern);
  const ObjectId ch = app.selChannel;
  if (l.cmd == "lane") {
    const PianoLaneGeometry& g = pianoLaneGeometry();
    std::printf("lane frame %d: x0 %.3f pxPerTick %.6f yTop %.2f yBottom %.2f valid %d\n", frame, g.x0, g.pxPerTick, g.yTop, g.yBottom,
                g.valid ? 1 : 0);
  } else if (!pat || ch == kNoId) {
    std::printf("%s frame %d: no pattern / channel\n", l.cmd.c_str(), frame);
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
  if (fresh && devWindowsLeft > 0) {
    --devWindowsLeft;
    devDumpWindows(frame);
  }
  for (const auto& l : script) {
    if (!fresh || l.frame != frame) continue;
    if (viewports::devCommand(app, l.cmd, l.args, frame)) continue;  // J-113
    if (pluginWindowDevCommand(app, l.cmd, l.args, frame)) continue;  // plugin windows, their channel strip
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
    else if (l.cmd == "seekbeat") {  // J-91: the song position to a beat of the metronome ("2.5")
      app.engine.setPosition(static_cast<Tick>(std::strtod(l.a0.c_str(), nullptr) * static_cast<double>(app.doc.song().beatTicks())), false);
    }
    else if (l.cmd == "metro") {  // J-91: metronome state and its glyph's pose (J-97: +1 / -1)
      std::printf("metro frame %d: on %d playing %d mirror %.3f beat %.3f\n", frame, app.metronome ? 1 : 0, app.engine.playing() ? 1 : 0,
                  static_cast<double>(topBarMetronomeMirror()), app.engine.position() / static_cast<double>(std::max<Tick>(1, app.doc.song().beatTicks())));
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
    else if (l.cmd == "toasts") app.toasts.clear();  // J-78: "toasts clear": no notification cards (control runs)
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
    } else if (l.cmd == "key" && l.a0 == "escape") {  // Q-3: cancel a drag
      io.AddKeyEvent(ImGuiKey_Escape, true);
      io.AddKeyEvent(ImGuiKey_Escape, false);
    } else if (l.cmd == "notes") {  // Q-3: the piano roll's notes (current pattern, selected channel) and selection
      const Song& s = app.doc.song();
      const Pattern* p = s.pattern(app.curPattern);
      const auto* ns = p && p->notes.count(app.selChannel) ? &p->notes.at(app.selChannel) : nullptr;
      std::printf("notes frame %d: %zu notes, %zu selected\n", frame, ns ? ns->size() : size_t(0), app.piano.selected.size());
      if (ns)
        for (size_t i = 0; i < ns->size(); ++i) {
          const Note& n = (*ns)[i];
          std::printf("  note %zu start %lld len %lld pitch %d vel %d pan %d color %d slide %d group %d sel %d\n", i, static_cast<long long>(n.start),
                      static_cast<long long>(n.length), n.pitch, n.velocity, n.pan, n.color, n.slide ? 1 : 0, n.group, app.piano.selected.count(i) ? 1 : 0);
        }
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
        e.key.keysym.mod = static_cast<Uint16>(devKeyMods(l.a2));  // "ctrl", "super", "shift", "ctrl+shift"...
      }
      SDL_PushEvent(&e);
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
    } else if (l.cmd == "forget") {
      devsig::forget(l.a0);
    } else if (l.cmd == "epoch") {
      std::printf("epoch frame %d: %u playing %d\n", frame, app.engine.stopEpoch(), app.engine.playing() ? 1 : 0);
      std::fflush(stdout);
    } else if (l.cmd == "previewfile") {  // J-109: the browser's sample preview
      app.previewSample(l.args.empty() ? l.a0 : l.args[0]);  // (a quoted path may hold spaces)
    } else if (l.cmd == "sound") {  // J-109: what is sounding (output peak since the last "sound")
      std::printf("sound frame %d: peak %.6f live %d previewing %d panics %u playing %d\n", frame,
                  static_cast<double>(app.engine.takeOutputPeak()), app.engine.liveHeld(), app.engine.previewing() ? 1 : 0,
                  app.engine.panicsDone(), app.engine.playing() ? 1 : 0);
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
      std::printf("mixer frame %d: scroll %.1f wheelScrolls %d undo %zu\n", frame, mixerStripsScrollY(), mixerWheelScrolls() ? 1 : 0,
                  app.doc.history().position());
      for (size_t i = 0; i < s.mixer.size(); ++i)
        std::printf("  track %zu volume %.4f pan %.4f\n", i, static_cast<double>(s.mixer[i].volume), static_cast<double>(s.mixer[i].pan));
      std::fflush(stdout);
    } else if (l.cmd == "rack") {  // J-84: channel rack layout, step scroll and values to stdout
      rackDevDump(app, frame);
    } else if (l.cmd == "quit") {  // J-103: the window's close request (SDL_QUIT), like the user quitting Gridline
      if (l.a0 == "discard") app.doc.markSaved();  // "quit discard": unsaved changes dropped (as "Don't save")
      SDL_Event q{};
      q.type = SDL_QUIT;
      SDL_PushEvent(&q);
    } else if (l.cmd == "collab") {  // J-111: collaboration (CollabUi.h)
      ImVec2 to;
      if (collabui::devCommand(app, l.args, frame, &to)) {  // "collab point": the pointer to an anchor
        pointer = to;
        io.AddMousePosEvent(to.x, to.y);
      }
    } else if (l.cmd == "waitfor") {  // J-108
      // (J-111f: a quoted name may hold spaces, "collab-name:Night Drive")
      const std::string& name = l.args.empty() ? l.a0 : l.args[0];
      const std::string& secs = l.args.size() > 1 ? l.args[1] : l.a1;
      devWait = {name, std::max(0.0, std::strtod(secs.c_str(), nullptr)), SDL_GetPerformanceCounter(), true};
    } else if (l.cmd == "undo") {
      app.doc.undo();
      app.syncEngine();
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
    } else if (l.cmd == "windows") {  // J-111h: windows [frames]
      devWindowsLeft = std::max(1, std::atoi(l.a0.c_str()));
    } else if (l.cmd == "shot") {
      devShotPending = l.args.empty() ? l.a0 : l.args[0];  // J-97: a quoted path may hold spaces
    } else if (l.cmd == "mknotes" || l.cmd == "notes" || l.cmd == "selnotes" || l.cmd == "lane") {
      devPianoCommand(app, l, frame);
    } else if (l.cmd == "dump") {
      const Song& s = app.doc.song();
      std::printf("dump frame %d: %zu clips, %zu selected, undo %zu\n", frame, s.playlist.size(), app.playlist.selected.size(),
                  app.doc.history().position());
      for (const auto& c : s.playlist)
        std::printf("  clip %u kind %d track %d start %lld len %lld offset %lld audioStart %.6f muted %d pattern %u group %u sel %d"
                    " source %u audioEnd %.6f fadeIn %.6f fadeOut %.6f gain %.4f pitch %.2f stretch %.4f reverse %d\n",
                    static_cast<unsigned>(c.id), static_cast<int>(c.kind), c.track, static_cast<long long>(c.start),
                    static_cast<long long>(s.clipLength(c)), static_cast<long long>(c.offset), c.audioStart, c.muted ? 1 : 0,
                    static_cast<unsigned>(c.pattern), static_cast<unsigned>(c.group), app.playlist.selected.count(c.id) ? 1 : 0,
                    static_cast<unsigned>(c.source), c.audioEnd, c.fadeIn, c.fadeOut, static_cast<double>(c.gain),
                    static_cast<double>(c.pitch), c.stretch, c.reverse ? 1 : 0);
      std::fflush(stdout);
    } else if (l.cmd == "plgeom") {  // J-118: the playlist's geometry (Windows.h PlaylistGeometry)
      const PlaylistGeometry& g = playlistGeometry();
      std::printf("plgeom frame %d: valid %d x0 %.3f pxPerTick %.6f tabs %.1f %.1f %.1f tabY %.1f\n", frame, g.valid ? 1 : 0,
                  static_cast<double>(g.x0), static_cast<double>(g.pxPerTick), static_cast<double>(g.tabX[0]),
                  static_cast<double>(g.tabX[1]), static_cast<double>(g.tabX[2]), static_cast<double>(g.tabY));
      for (size_t i = 0; i < g.trackTop.size(); ++i)
        std::printf("  track %zu top %.1f h %.1f\n", i, static_cast<double>(g.trackTop[i]), static_cast<double>(g.trackH[i]));
      for (const auto& r : g.rows)
        std::printf("  row %u x %.1f y %.1f\n", r.id, static_cast<double>(r.x), static_cast<double>(r.y));
      std::fflush(stdout);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
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
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
#if defined(__APPLE__)
  const char* glsl = "#version 150";
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
  const char* glsl = "#version 130";
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  // Start as a compact borderless splash; becomes the main window once loaded.
  // J-78: --hidden (automated off-screen runs with --size / --screenshot): the
  // window is never shown on the user's desktop.
  bool startHidden = false;
  for (int i = 1; i < argc; ++i) startHidden = startHidden || std::string(argv[i]) == "--hidden";
  SDL_Window* window = SDL_CreateWindow("Gridline", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 380,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI |
                                            (startHidden ? SDL_WINDOW_HIDDEN : 0));
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(1);

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
  {  // Window / dock icon: the app icon (tools/make_app_icon.py), else the plain mark drawn here.
    SDL_Surface* icon = assets.empty() ? nullptr : SDL_LoadBMP((fs::path(assets) / "icons" / "gridline-app-256.bmp").string().c_str());
    if (!icon) icon = makeIcon(256);
    if (icon) {
      SDL_SetWindowIcon(window, icon);
      SDL_FreeSurface(icon);
    }
  }
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
  viewports::install(window, gl, {startHidden});  // J-113: detachable windows
  if (startHidden) {
    // J-78: automated runs keep their own clipboard (a scripted click on a
    // notification must not replace what the user copied); stdout shows it.
    static std::string devClipboard;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
      devClipboard = text ? text : "";
      std::printf("clipboard: %s\n", devClipboard.c_str());
    };
    pio.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* { return devClipboard.c_str(); };
  }
  theme::setMarkTextureProvider(&markTexture);

  // ---- splash: each real start-up step advances the bar ----
  Uint64 splashT0 = SDL_GetPerformanceCounter();
  float shown = 0.0f;
  auto splash = [&](float target, const char* status) {
    // Render until the bar has caught up (min ~4 frames per step).
    for (int f = 0; f < 60; ++f) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) ImGui_ImplSDL2_ProcessEvent(&e);
      shown += (target - shown) * 0.35f;
      double tt = static_cast<double>(SDL_GetPerformanceCounter() - splashT0) / static_cast<double>(SDL_GetPerformanceFrequency());
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplSDL2_NewFrame();
      ImGui::NewFrame();
      drawSplash(fBold, fMono, shown, status, tt);
      ImGui::Render();
      int w, h;
      SDL_GL_GetDrawableSize(window, &w, &h);
      glViewport(0, 0, w, h);
      glClearColor(0.043f, 0.043f, 0.043f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      if (const char* shot = std::getenv("GRIDLINE_SPLASH_SHOT"); shot && target > 0.4f && target < 0.5f && f == 5)
        writePpm(shot, w, h);
      SDL_GL_SwapWindow(window);
      if (f >= 4 && target - shown < 0.01f) break;
    }
  };
  splash(0.12f, "Loading interface");
  splash(0.22f, "Reading settings");
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
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--no-ui-sounds" || arg == "--screenshot" || arg == "--profile-frames") app.uiSoundsDisabled = true;
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
    if (shot)
      app.applySettingsHooks();
    else
      app.applyStartup();
  }
  {  // Opened with files (file manager, "Open with", the Linux desktop entry's %F): no options,
     // only paths; the first project among them opens.
    bool onlyPaths = argc > 1;
    for (int i = 1; i < argc; ++i) onlyPaths = onlyPaths && argv[i][0] != '-';
    for (int i = 1; onlyPaths && i < argc; ++i) {
      std::error_code ec;
      if (fs::is_regular_file(fs::u8path(argv[i]), ec) && app.openProject(argv[i])) break;
    }
  }

  // Command line: [project] [--screenshot out.ppm] [--frames N] [--play] [--fx mixer slot] [--chan index]
  //               [--plugin type] [--size WxH]
  splash(0.88f, "Opening project");
  std::string shotPath;
  int shotFrames = 45;
  std::pair<int, int> forcedSize{0, 0};  // --size WxH (window size for layout checks)
  std::pair<int, int> resizeTo{0, 0};    // --resize WxH (applied halfway through --frames)
  std::string devMenu;                    // --menu name: open a menu for screenshots
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
  float devUiScale = 0.0f;                // --ui-scale F: interface scale for this run only (not saved; J-54 layout checks)
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
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--screenshot" && i + 1 < argc) shotPath = argv[++i];
    else if (arg == "--profile-frames" && i + 1 < argc) profilePath = argv[++i];
    else if (arg == "--profile-warmup" && i + 1 < argc) profileWarmup = std::max(0, std::atoi(argv[++i]));
    else if (arg == "--frames" && i + 1 < argc) shotFrames = std::atoi(argv[++i]);
    else if (arg == "--menu" && i + 1 < argc) devMenu = argv[++i];
    else if (arg == "--tool" && i + 1 < argc) devTool = argv[++i];
    else if (arg == "--script" && i + 1 < argc) devScript = argv[++i];
    else if (arg == "--dev-input" && i + 1 < argc) devInput = loadDevInput(argv[++i]);
    else if (arg == "--play") app.engine.play();
    else if (arg == "--fx" && i + 2 < argc) {
      size_t mi = static_cast<size_t>(std::atoi(argv[++i]));
      int slot = std::atoi(argv[++i]);
      if (mi < app.doc.song().mixer.size()) app.fxEditor = {app.doc.song().mixer[mi].id, slot};
    } else if (arg == "--chan" && i + 1 < argc) {
      size_t ci = static_cast<size_t>(std::atoi(argv[++i]));
      if (ci < app.doc.song().channels.size()) app.settingsChannel = app.doc.song().channels[ci].id;
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
    } else if (arg == "--fixed-dt" && i + 1 < argc) {  // J-37 dev check: every frame advances UI time by N ms (reproducible animation)
      fixedDt = std::max(0.0, std::atof(argv[++i])) / 1000.0;
    } else if (arg == "--scrollx" && i + 1 < argc) {  // dev check: playlist horizontal scroll (px)
      app.playlist.scrollX = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--size" && i + 1 < argc) {
      int w = 0, h = 0;
      if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) forcedSize = {w, h};
    } else if (arg == "--resize" && i + 1 < argc) {
      // Dev check for live resizing: switch to this size halfway through --frames.
      int w = 0, h = 0;
      if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) resizeTo = {w, h};
    } else if (arg == "--show" && i + 1 < argc) {  // open a window for layout checks
      std::string w = argv[++i];
      if (w == "about") app.showAbout = true;
      else if (w == "settings") app.showSettings = true;
      else if (w == "shortcuts") app.showShortcuts = true;
      else if (w == "export") app.showExport = true;
      else if (w == "info") app.showInfo = true;
      else if (w == "history") app.showHistory = true, devFocus = "Undo history";  // docked behind the browser: bring its tab to the front
      else if (w == "performance") app.showPerformance = true;
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
      else if (w == "interface") app.showSettings = true, app.settingsTab = 4;
      else if (w == "general") app.showSettings = true, app.settingsTab = 5;
      else if (w == "files") app.showSettings = true, app.settingsTab = 6;
      else if (w == "debug") app.showSettings = true, app.settingsTab = 7;
      else if (w == "keys") app.showSettings = true, app.settingsTab = 8;   // J-55 shortcut editor
      else if (w == "theme") app.showSettings = true, app.settingsTab = 9;  // J-55 theme page
      else if (w == "integrations") app.showSettings = true, app.settingsTab = 10;  // SC-07 AI assistants page
      else if (w == "collab-settings") app.showSettings = true, app.settingsTab = 11;  // J-111 Collaboration page
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
    } else if (arg == "--light") {
      theme::apply(false);
    } else if (arg == "--ui-scale" && i + 1 < argc) {
      devUiScale = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.75f, 2.0f);
    } else if (arg == "--scale" && i + 1 < argc) {
      shotScale = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.5f, 4.0f);
    } else if (arg == "--no-update-check") {  // J-89
      updateOpts.disabled = true;
    } else if (arg == "--fake-update" && i + 1 < argc) {  // J-89 dev check: a made-up newer release is offered (no network)
      updateOpts.fakeVersion = argv[++i];
    } else if (arg == "--hidden") {
      // J-78: read before the window is created (see startHidden)
    } else if (fs::exists(arg)) {
      app.openProject(arg);
    }
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
    SDL_SetWindowBordered(window, SDL_TRUE);
    SDL_SetWindowResizable(window, SDL_TRUE);
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, usable.x + (usable.w - w) / 2, usable.y + (usable.h - h) / 2);
    SDL_SetWindowMinimumSize(window, 960, 600);
    if (browserui::thumbMode() && !shotPath.empty() && forcedSize.first > 0) SDL_HideWindow(window);  // background thumbnail run
  }

  // Screenshots with an explicit size render off-screen at exactly that size
  // (the OS may clamp a real window to the display).
  OffscreenTarget offscreen;
  const bool useOffscreen = (!shotPath.empty() || !profilePath.empty()) && forcedSize.first > 0 &&
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
  }
  const bool frameClock = profilePath.empty() && mainNative.valid() && !std::getenv("GRIDLINE_VSYNC_SWAP") && host::native::frameClockStart(mainNative);
  if (frameClock) SDL_GL_SetSwapInterval(0);
  // The saved layout, read now that the main window is where it will be (J-113: detached
  // windows are saved relative to it), then saved there on exit.
  viewports::setVirtualMonitors(useOffscreen);
  ImGui::LoadIniSettingsFromDisk(iniPath.c_str());
  io.IniFilename = iniPath.c_str();

  std::map<SDL_Keycode, HeldNote> held;
  std::string lastTitle;
  bool running = true;
  Uint64 t0 = SDL_GetPerformanceCounter();
  // One complete frame after the events: tick, interface, render and swap of every window. The
  // main loop draws it after each event pump; J-116: SDL's event watch also draws it from inside
  // a pump the window system blocks while a window is resized (`live`, see LiveResize.h).
  auto frame = [&](bool live) {
    liveresize::frameBegin(live);
    uint64_t statT = 0;
    // J-109: Gridline lost focus (another application, a plugin window, a key released
    // over another window): no key-up will come, so every note held from the interface
    // ends now (typed keys, clicked keys, pads). J-113: not when the focus only moved
    // between the main window and a detached one.
    if (viewports::takeAppFocusLost()) {
      releaseTypedNotes(app, held);
      app.engine.releaseLiveNotes();
    }
    xdragui::pump(app);  // J-75: drags from other applications
    if (app.quitRequested) {
      app.quitRequested = false;
      app.guardUnsaved([&running]() { running = false; });
    }
    if (app.audioReopen) {
      app.audioReopen = false;
      openAudio(app);
    }
    if (!app.typingKeyboard && app.typing.anyHeld()) releaseTypedNotes(app, held);

    double now = static_cast<double>(SDL_GetPerformanceCounter() - t0) / static_cast<double>(SDL_GetPerformanceFrequency());
    {
      prof::Scope ps("tick");
      fstats::Timer ft(fstats::Tick);
      app.tick(now);
      collabui::frame(app, now);  // J-111: network, collaborators' edits, saving
      pumpHostedPlugins(app);
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

    viewports::setAllowed(app.settings.getBool("ui.detachWindows"));
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    if (winW > 0 && winH > 0) viewports::beginFrame(uiScale, ImVec2(static_cast<float>(winW) / uiScale, static_cast<float>(winH) / uiScale));
    xdragui::feedPointer();  // J-75: the drag position wins over the backend's pointer fallback
    if (fixedDt > 0) io.DeltaTime = static_cast<float>(fixedDt);
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
        runDevInput(app, devInput, frameNo);
        lastScaledEvent = g.InputEventsNextEventId - 1;  // already in logical pixels
      }
    }
    if (pointerAt < 0 || now - pointerAt >= 3.0) {  // J-111g: the system pointer's size (Settings may change it)
      pointerSize = host::native::pointerSize(pointerNative);
      pointerAt = now;
    }
    collabui::setPointerSize(static_cast<float>(pointerSize.arrowH) / uiScale, pointerSize.source);
    scrollui::configure(app.settings);  // J-67: smooth scrolling / elastic edges for every ImGui scroll region
    ui::SetWindowRubber(scrollui::params().elastic);  // J-117: the same switch for the windows' elastic boundary
    scrollui::beforeNewFrame();
    ImGui::NewFrame();
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
    // J-28: saved layouts from before the current default are replaced once.
    const bool migrate = static_cast<int>(app.settings.getNumber("ui.layoutVersion")) < workspace::kLayoutVersion;
    // Dev layout options (--show mixer / playlist, --hide) ask for the default layout they adjust,
    // whatever layout.ini holds (it is read since J-113).
    static bool devLayout = devMixerOnly || devPlaylistOnly || !devHide.empty();
    if (firstRun || migrate || app.resetLayout || devLayout || !ImGui::DockBuilderGetNode(dockId)) {
      devLayout = false;
      const ImVec2 inner(dsize.x - 8, dsize.y - 8);  // dock host padding
      if (app.resetLayout) viewports::gatherAll();  // J-113: detached windows come back too
      const workspace::Rect r = buildDefaultLayout(dockId, inner, static_cast<int>(app.doc.song().channels.size()));
      rackPlacement = {dpos.x + 4 + r.x, dpos.y + 4 + r.y, r.w, r.h};
      placeRack = true;
      app.settings.set("ui.layoutVersion", workspace::kLayoutVersion);
      firstRun = false;
      app.resetLayout = false;
      app.showBrowser = app.showRack = app.showPiano = app.showPlaylist = app.showMixer = true;
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
    {
      prof::Scope ps("dialogs");
      drawAutomationEditor(app);
      drawDialogs(app);
      browserui::drawPluginPicker(app);
      collabui::draw(app);  // J-111
    }
    browserui::thumbDraw(app);
    if (!devMenu.empty()) drawDevMenu(app, devMenu);
    if (!devFocus.empty() && frameNo == 3) app.focusWindow = devFocus;
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
      std::string h = "Typing keyboard: Z…/ and Q…P play from " + midiNoteName(app.typing.baseNote()) +
                      " · [ ] octave · - = velocity " + std::to_string(app.typing.velocity) + " · \\ chord mode";
      if (app.typing.chordMode) h += std::string(" (") + chordTypeName(app.typing.chord) + ", ' next)";
      if (app.stepRecord) h += " · step record: → rest, ⌫ back";
      app.setHint(h);
    }
    {
      prof::Scope ps("statusbar");
      drawStatusBar(app);
      handleGlobalShortcuts(app);
    }

    xdragui::endFrame(app);  // J-75: a drop no window took opens the files
    ui::wheelFrameEnd();     // J-83
    collabui::drawCursors(app);  // J-111: collaborators' cursors, last: on top of every window, popup and menu
    {
    prof::Scope psRender("render");
    viewports::drawTitleButtons();  // J-114
    ImGui::Render();
    if (useOffscreen) offscreen.use(true);
    glViewport(0, 0, fbW, fbH);
    ImVec4 bg = theme::vec(theme::pal().chrome);
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (browserui::thumbMode() && !browserui::thumbCapture(app, fbW, fbH)) running = false;
    if (!profilePath.empty() && shotPath.empty() && ++frameNo >= shotFrames + profileWarmup) running = false;
    if (!devInput.empty()) {  // J-112: for "waitfor"
      devsig::reach(app.engine.playing() ? "transport-playing" : "transport-stopped");
      if (app.engine.pluginPlaybackActive()) devsig::reach("plugin-playback");
      collabui::devFrame(app);  // J-111: collab-connected, collab-joined, collab-synced, ...
    }
    const bool devHold = devWaitHolds();  // J-108: "waitfor" keeps the script (and --frames) on this frame
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
  liveresize::install(window, [&frame]() { frame(true); });
  while (running) {
    prof::beginFrame();
    SDL_Event ev;
    uint64_t statT = fstats::stamp();
    liveresize::pumpBegin();
    while (SDL_PollEvent(&ev)) {
      ImGui_ImplSDL2_ProcessEvent(&ev);
      viewports::onEvent(ev);  // J-113: focus over Gridline's windows, minimise
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
      if (ev.type == SDL_DROPCOMPLETE) {
        int mx = 0, my = 0, wx = 0, wy = 0;
        SDL_GetGlobalMouseState(&mx, &my);
        SDL_GetWindowPosition(window, &wx, &wy);
        xdragui::sdlDropComplete(static_cast<float>(mx - wx), static_cast<float>(my - wy));
      }
      // A held typed note ends on its key-up whatever changed meanwhile
      // (a modifier pressed, a text field focused, the typing keyboard switched off).
      if (ev.type == SDL_KEYUP && held.count(ev.key.keysym.sym) && !(app.typingKeyboard && !io.WantTextInput &&
                                                                     !(ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI | KMOD_ALT)))) {
        const SDL_Keycode k = ev.key.keysym.sym;
        const ObjectId chId = held[k].channel;
        held.erase(k);
        for (int p : app.typing.keyUp(typingKey(k))) {
          app.engine.noteOff(chId, p);
          app.playedNoteOff(chId, p);
        }
      }
      // Typing keyboard (key events straight from SDL so key-up is never missed).
      if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && app.typingKeyboard && !io.WantTextInput &&
          !(ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI | KMOD_ALT))) {
        SDL_Keycode k = ev.key.keysym.sym;
        int key = typingKey(k);
        const bool down = ev.type == SDL_KEYDOWN && !ev.key.repeat;
        if (down && app.stepRecord && !app.engine.playing() && (k == SDLK_RIGHT || k == SDLK_BACKSPACE)) {
          if (k == SDLK_RIGHT)
            app.stepRecorder.rest();
          else
            app.stepRecorder.backspace(app.doc, app.curPattern, app.selChannel);
        }
        if (key > 0 && app.selChannel != kNoId) {
          if (down) {
            std::vector<int> on = app.typing.keyDown(key);
            for (int p : on) {
              app.engine.noteOn(app.selChannel, p, app.typing.velocity / 127.0f);
              app.playedNoteOn(app.selChannel, p, app.typing.velocity);
            }
            if (!on.empty()) held[k] = {app.selChannel, 0, 0};
          } else if (ev.type == SDL_KEYUP) {
            ObjectId chId = held.count(k) ? held[k].channel : app.selChannel;
            held.erase(k);
            for (int p : app.typing.keyUp(key)) {
              app.engine.noteOff(chId, p);
              app.playedNoteOff(chId, p);
            }
          }
        }
      }
    }
    liveresize::pumpEnd();
    fstats::since(fstats::Events, statT);
    frame(false);
    prof::endFrame();
    fstats::endFrame();
  }
  liveresize::uninstall();
  if (!profilePath.empty()) {
    std::string label = std::to_string(forcedSize.first) + "x" + std::to_string(forcedSize.second);
    if (prof::writeJson(profilePath, label))
      std::fprintf(stderr, "profile: %d frames -> %s\n", prof::recordedFrames(), profilePath.c_str());
  }

  if (!devSaveAtExit.empty()) {
    captureHostedStates(app);
    std::fprintf(stderr, "%s %s\n", app.saveProject(devSaveAtExit) && (flpexport::wait(), true) ? "saved" : "could not save", devSaveAtExit.c_str());
  }
  // J-103: no background thread may run into static teardown (a plugin scan finishing then
  // locked the destroyed class registry mutex: "mutex lock failed", abort on quit).
  host::shutdownScan();        // the scanner child is killed; unscanned entries wait for the next start
  exportui::shutdown();        // a running audio export / render to sampler is cancelled and joined
  j59::shutdownRenderSampler();
  browserui::shutdownIndex();  // J-46: no worker left running into static teardown
  updui::shutdown();           // J-89: a running installer download is cancelled
  closeAllHostedEditors(app);
  if (frameClock) host::native::frameClockStop();
  if (gDevice) SDL_CloseAudioDevice(gDevice);
  if (gInDevice) SDL_CloseAudioDevice(gInDevice);  // (SDL_Quit would too; explicit, like the output)
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
  return browserui::auditIssueCount() > 0 ? 1 : 0;  // Q-7: --layout-audit fails on any inset issue
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
