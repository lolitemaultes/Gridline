// Gridline — application entry: SDL2 window + OpenGL 3 + Dear ImGui, audio
// device, default layout, typing keyboard, file drops.
#include <SDL.h>
#include <SDL_opengl.h>
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
#include "FrameProfile.h"
#include "HostedUi.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/vst3/Vst3Scan.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_impl_sdl2.h"
#include "../../third_party/imgui/imgui_internal.h"
#include <cfloat>
#include "../core/Project.h"
#include "../core/Workspace.h"
#include "../packs/PacksUi.h"
#include "ExportUi.h"
#include "../core/MidiFile.h"
#include "App.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "EditTools.h"
#include "J31Ui.h"
#include "ScriptUi.h"
#include "Theme.h"
#include "Windows.h"

namespace fs = std::filesystem;
using namespace gl;

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
  eng->processInterleaved(reinterpret_cast<float*>(stream), frames);
  // Interface sounds go on top of the finished output: outside the song mixer,
  // so they are never recorded, rendered or metered.
  if (gUiSounds) gUiSounds->mix(reinterpret_cast<float*>(stream), frames);
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

// Tile mark coverage (guide §05 construction: 120u, r 28u, 10u bars at 40u/80u,
// inset 24u, true holes), supersampled. Returns size*size alpha values.
std::vector<Uint8> markCoverage(int size) {
  auto inside = [](double x, double y) {
    const double U = 120, R = 28;
    double cx = std::clamp(x, R, U - R), cy = std::clamp(y, R, U - R);
    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > R * R) return false;
    for (double c : {40.0, 80.0}) {
      if (std::fabs(x - c) <= 5 && y >= 24 && y <= 96) return false;
      if (std::fabs(y - c) <= 5 && x >= 24 && x <= 96) return false;
    }
    return true;
  };
  std::vector<Uint8> out(static_cast<size_t>(size) * static_cast<size_t>(size));
  const int ss = size < 64 ? 8 : 4;
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      int hit = 0;
      for (int sy = 0; sy < ss; ++sy)
        for (int sx = 0; sx < ss; ++sx) hit += inside((x + (sx + 0.5) / ss) * 120.0 / size, (y + (sy + 0.5) / ss) * 120.0 / size);
      out[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] =
          static_cast<Uint8>((255 * hit + ss * ss / 2) / (ss * ss));
    }
  return out;
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

// Pixel-exact mark textures (white + coverage alpha), cached per size; tinted
// and faded by the draw list (theme::drawMark), so the holes are true holes.
ImTextureID markTexture(int px) {
  static std::map<int, GLuint> cache;
  if (px < 8 || px > 1024) return ImTextureID_Invalid;
  if (auto it = cache.find(px); it != cache.end()) return static_cast<ImTextureID>(it->second);
  auto cov = markCoverage(px);
  std::vector<Uint8> rgba(cov.size() * 4);
  for (size_t i = 0; i < cov.size(); ++i) {
    rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
    rgba[i * 4 + 3] = cov[i];
  }
  GLint last = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, px, px, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
  cache[px] = tex;
  return static_cast<ImTextureID>(tex);
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

// FL-style default workspace (J-28, see core/Workspace.h): browser docked
// left, the mixer as a thin column docked right, the playlist in the centre
// (piano roll as a tab behind it) and the channel rack floating over the
// centre-left of the playlist. Sizes are picked in pixels for the window the
// layout is built in; the playlist node is the central node and absorbs later
// resizes. Returns the rack's floating rectangle (dock space coordinates).
workspace::Rect buildDefaultLayout(ImGuiID dockspace, ImVec2 size, int channels) {
  ImGui::DockBuilderRemoveNode(dockspace);
  ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace, size);
  const workspace::Layout L = workspace::defaultLayout(size.x, size.y, channels);
  ImGuiID left, rest, mixerNode, mainNode;
  ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, L.browserFrac, &left, &rest);
  ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, L.mixerFrac, &mixerNode, &mainNode);
  ImGui::DockBuilderDockWindow("Browser", left);
  ImGui::DockBuilderDockWindow("Undo history", left);
  ImGui::DockBuilderDockWindow("Playlist", mainNode);
  ImGui::DockBuilderDockWindow("Piano roll", mainNode);
  ImGui::DockBuilderDockWindow("Mixer", mixerNode);
  ImGui::DockBuilderFinish(dockspace);
  return L.rack;
}

// Floating windows (the rack) stay reachable when the main window shrinks:
// their title bar and some of their width remain inside the dock space.
void keepFloatingInView(const char* name, ImVec2 pos, ImVec2 size) {
  ImGuiWindow* w = ImGui::FindWindowByName(name);
  if (!w || w->DockIsActive || !w->WasActive || w->Hidden) return;
  ImVec2 p = w->Pos;
  const float keep = std::min(w->Size.x, 160.0f);
  p.x = std::clamp(p.x, pos.x - (w->Size.x - keep), std::max(pos.x, pos.x + size.x - keep));
  p.y = std::clamp(p.y, pos.y, std::max(pos.y, pos.y + size.y - 40.0f));
  if (p.x != w->Pos.x || p.y != w->Pos.y) ImGui::SetWindowPos(w, p, ImGuiCond_Always);
}

// Keeps the docked workspace usable while the window is resized. ImGui keeps
// side panels at a fixed pixel size and gives all the change to the central
// node (playlist), so shrinking a large window could squeeze the playlist to
// nothing. Here side panels scale with the dock space and are clamped so the
// central area keeps at least half of every split.
bool hostsWindow(const ImGuiDockNode* node, const char* name) {
  if (!node) return false;
  for (ImGuiWindow* w : node->Windows)
    if (w && std::strcmp(w->Name, name) == 0) return true;
  return hostsWindow(node->ChildNodes[0], name) || hostsWindow(node->ChildNodes[1], name);
}

void fitDockTree(ImGuiDockNode* node, ImVec2 scale, ImVec2 avail) {
  if (!node || node->IsLeafNode()) return;
  ImGuiDockNode* c0 = node->ChildNodes[0];
  ImGuiDockNode* c1 = node->ChildNodes[1];
  if (!c0 || !c1) return;
  const int axis = node->SplitAxis == ImGuiAxis_X ? 0 : 1;
  const float parent = axis == 0 ? avail.x : avail.y;
  ImGuiDockNode* side = c1->HasCentralNodeChild ? c0 : c0->HasCentralNodeChild ? c1 : nullptr;
  if (side && side->SizeRef[axis] > 0.0f) {
    float v = side->SizeRef[axis] * (axis == 0 ? scale.x : scale.y);
    const float minPx = axis == 0 ? 180.0f : 150.0f;
    float maxPx = std::max(parent * 0.5f, 1.0f);
    if (axis == 0 && hostsWindow(side, "Browser")) maxPx = std::min(maxPx, 340.0f);  // sidebar stays a sidebar
    v = std::clamp(v, std::min(minPx, parent * 0.5f), std::max(maxPx, std::min(minPx, parent * 0.5f)));
    side->SizeRef[axis] = std::floor(v);
  }
  ImVec2 a0 = avail, a1 = avail;
  if (side) {
    // The central branch gets what the side panel leaves.
    ImGuiDockNode* centre = side == c0 ? c1 : c0;
    ImVec2& ac = side == c0 ? a1 : a0;
    (axis == 0 ? ac.x : ac.y) = std::max(1.0f, parent - side->SizeRef[axis]);
    fitDockTree(centre, scale, ac);
    ImVec2& as = side == c0 ? a0 : a1;
    (axis == 0 ? as.x : as.y) = side->SizeRef[axis];
    fitDockTree(side, scale, as);
  } else {
    fitDockTree(c0, scale, a0);
    fitDockTree(c1, scale, a1);
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

// J-28 dev check: scripted input for interaction checks (--dev-input file).
// One command per line, "<frame> <command> <args>", in logical pixels:
//   move x y · down b · up b · mod ctrl|shift|alt 0|1 · wheel dy · key <letter>
//   audio <wav> <track> <bar> (browser drop path) · dump (playlist clips to stdout)
//   shot <out.ppm> (J-37: this frame is saved once rendered — frame sequences)
//   key space (J-49) · transport (J-49: song position / playing / seek position to stdout)
std::string devShotPending;
struct DevInputLine {
  int frame = 0;
  std::string cmd, a0, a1, a2;
};

std::vector<DevInputLine> loadDevInput(const std::string& path) {
  std::vector<DevInputLine> out;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) return out;
  char line[1024];
  while (std::fgets(line, sizeof line, f)) {
    char c[64] = "", x[512] = "", y[64] = "", z[64] = "";
    int fr = 0;
    if (line[0] == '#' || std::sscanf(line, "%d %63s %511s %63s %63s", &fr, c, x, y, z) < 2) continue;
    out.push_back({fr, c, x, y, z});
  }
  std::fclose(f);
  return out;
}

void runDevInput(App& app, const std::vector<DevInputLine>& script, int frame) {
  ImGuiIO& io = ImGui::GetIO();
  // The scripted pointer stays put between commands (the backend would
  // otherwise report the real pointer whenever the window has focus).
  static ImVec2 pointer(-FLT_MAX, -FLT_MAX);
  if (pointer.x > -FLT_MAX) io.AddMousePosEvent(pointer.x, pointer.y);
  for (const auto& l : script) {
    if (l.frame != frame) continue;
    if (l.cmd == "move") {
      pointer = ImVec2(std::strtof(l.a0.c_str(), nullptr), std::strtof(l.a1.c_str(), nullptr));
      io.AddMousePosEvent(pointer.x, pointer.y);
    }
    else if (l.cmd == "down" || l.cmd == "up") io.AddMouseButtonEvent(std::atoi(l.a0.c_str()), l.cmd == "down");
    else if (l.cmd == "wheel") io.AddMouseWheelEvent(0, std::strtof(l.a0.c_str(), nullptr));
    else if (l.cmd == "mod") {
      const bool on = std::atoi(l.a1.c_str()) != 0;
      ImGuiKey k = l.a0 == "ctrl" ? ImGuiMod_Ctrl : l.a0 == "shift" ? ImGuiMod_Shift : ImGuiMod_Alt;
      io.AddKeyEvent(k, on);
    } else if (l.cmd == "key" && l.a0.size() == 1 && l.a0[0] >= 'a' && l.a0[0] <= 'z') {
      ImGuiKey k = static_cast<ImGuiKey>(ImGuiKey_A + (l.a0[0] - 'a'));
      io.AddKeyEvent(k, true);
      io.AddKeyEvent(k, false);
    } else if (l.cmd == "key" && l.a0 == "space") {
      io.AddKeyEvent(ImGuiKey_Space, true);
      io.AddKeyEvent(ImGuiKey_Space, false);
    } else if (l.cmd == "transport") {
      std::printf("transport frame %d: pos %.0f playing %d seek %.0f mode %d\n", frame, app.engine.position(), app.engine.playing() ? 1 : 0,
                  app.engine.seekPosition(), static_cast<int>(app.engine.mode()));
      std::fflush(stdout);
    } else if (l.cmd == "audio") {
      browserui::DropPlace place;
      place.track = std::atoi(l.a1.c_str());
      place.tick = static_cast<Tick>(std::atoi(l.a2.c_str())) * app.doc.song().barTicks();
      browserui::performDrop(app, l.a0, browse::DropTarget::Playlist, place);
    } else if (l.cmd == "shot") {
      devShotPending = l.a0;
    } else if (l.cmd == "dump") {
      const Song& s = app.doc.song();
      std::printf("dump frame %d: %zu clips, %zu selected\n", frame, s.playlist.size(), app.playlist.selected.size());
      for (const auto& c : s.playlist)
        std::printf("  clip %u kind %d track %d start %lld len %lld offset %lld audioStart %.6f muted %d\n", static_cast<unsigned>(c.id),
                    static_cast<int>(c.kind), c.track, static_cast<long long>(c.start), static_cast<long long>(s.clipLength(c)),
                    static_cast<long long>(c.offset), c.audioStart, c.muted ? 1 : 0);
      std::fflush(stdout);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Child processes (see host/PluginScan.h): plugin scanner and plugin
  // sandbox; no window, no audio.
  if (int rc = host::childMain(argc, argv); rc >= 0) return rc;
  if (argc >= 2 && std::string(argv[1]) == "--version") {
    std::printf("Gridline 0.4.0\n");
    return 0;
  }
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
  SDL_Window* window = SDL_CreateWindow("Gridline", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 380,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  SDL_GL_MakeCurrent(window, gl);
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
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
  theme::apply(app.settings.getBool("ui.darkTheme"));
  std::string iniPath = (fs::path(app.configDir()) / "layout.ini").string();
  {
    std::error_code ec;
    fs::create_directories(app.configDir(), ec);
  }
  io.IniFilename = iniPath.c_str();
  bool firstRun = !fs::exists(iniPath);

  splash(0.42f, "Starting audio engine");
  app.audioSampleRate = static_cast<int>(app.settings.getNumber("audio.sampleRate"));
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
  bool devChords = false;                // --show chords: chord progression editor (frame 3)
  std::string devScript;                  // --script id: J-24 piano roll script window (frame 3) on the --edit target
  bool devPlaylistOnly = false;           // --show playlist: other panels closed
  bool devMixerOnly = false;              // --show mixer: the mixer fills the window
  float shotScale = 1.0f;                 // --scale F: framebuffer scale of an off-screen shot (HiDPI check)
  std::string devSaveAtExit;  // --save-at-exit path: project saved when the run ends (plugin state round trips)
  int devWindowCycle = 0;  // --plugin-window-cycle N: close / reopen the plugin window N times
  bool devHostedEditor = false;           // --hosted-editor: open the native editor of the --plugin target
  std::vector<DevInputLine> devInput;     // --dev-input file: scripted mouse / key input (J-28)
  std::string profilePath;                // --profile-frames out.json: per-window UI CPU time per frame (J-25)
  int profileWarmup = 30;                 // --profile-warmup N: frames not recorded (layout, caches)
  double fixedDt = 0;                     // --fixed-dt ms: fixed UI frame time (J-37 animation frame sequences)
  std::vector<std::string> devHide;       // --hide rack|browser|mixer|playlist|piano: closed once the layout exists
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
      else if (w == "open") app.openDialog();
      else if (w == "unsaved") app.confirmDiscard = true;
      else if (w == "livepads") app.showLivePads = true;
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
      else if (w == "newproject") app.showNewProject = true;
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
        app.importReport.unmapped = {{"Third-party plugin (no installed match)", 2}};
        app.importReport.warnings = {"Two automation clips target parameters that have no Gridline equivalent; they were kept but do nothing.",
                                     "Time stretching of one audio clip is approximated."};
      } else if (w == "steprec") {  // layout check: step record cursor in the piano roll
        app.stepRecord = true;
        app.stepRecorder.cursor = app.doc.song().ppq * 2;
        devFocus = "Piano roll";
      } else if (w == "chanmisc") {  // layout check: channel settings, misc tab
        if (!app.doc.song().channels.empty()) app.settingsChannel = app.doc.song().channels[0].id;
        app.channelTab = 1;
      } else if (w == "chords") {  // J-36 QA: chord progression editor on the selected pattern / channel (frame 3)
        devChords = true;
      } else if (w == "toast") {  // layout check: notification toasts
        app.notify("Saved the project");
        app.notify("Cannot decode sample: missing.wav", true);
      }
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
    } else if (arg == "--make-thumbs") {  // render every built-in editor into <config>/thumbs (plugin picker)
      browserui::beginThumbMode(app);
    } else if (arg == "--make-thumb" && i + 1 < argc) {
      browserui::beginThumbMode(app, argv[++i]);
    } else if (arg == "--picker") {  // dev check: plugin picker open
      browserui::togglePluginPicker();
    } else if (arg == "--browser-search" && i + 1 < argc) {
      app.browserSearch = argv[++i];
    } else if (arg == "--hosted-editor") {
      devHostedEditor = true;
      setPluginWindowLog(true);
    } else if (arg == "--plugin-panel" && i + 1 < argc) {  // dev check: plugin window settings unfolded on page N
      setPluginWindowPanel(std::atoi(argv[++i]));
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
    } else if (arg == "--light") {
      theme::apply(false);
    } else if (arg == "--scale" && i + 1 < argc) {
      shotScale = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.5f, 4.0f);
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
#if !defined(__APPLE__)
  // Displays without OS scaling (e.g. X11 at high DPI): derive a default scale.
  float autoDpi = 1.0f;
  {
    float ddpi = 0;
    int dw, dh, ww, wh;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    SDL_GetWindowSize(window, &ww, &wh);
    if (dw == ww && SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), &ddpi, nullptr, nullptr) == 0 && ddpi > 120)
      autoDpi = std::round(ddpi / 96.0f * 4.0f) / 4.0f;
  }
#else
  const float autoDpi = 1.0f;  // macOS scales points to pixels itself
#endif
  ImU32 lastScaledEvent = 0;

  std::map<SDL_Keycode, HeldNote> held;
  std::string lastTitle;
  bool running = true;
  Uint64 t0 = SDL_GetPerformanceCounter();
  while (running) {
    prof::beginFrame();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      ImGui_ImplSDL2_ProcessEvent(&ev);
      if (hostedEditorEvent(app, ev)) continue;  // plugin editor windows
      if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                                  ev.window.windowID == SDL_GetWindowID(window)))
        app.quitRequested = true;
      if (ev.type == SDL_DROPFILE) {
        std::string path = ev.drop.file;
        SDL_free(ev.drop.file);
        browserui::openFile(app, path);  // samples, MIDI, presets, projects, FL files (J-16)
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
    if (app.quitRequested) {
      app.quitRequested = false;
      app.guardUnsaved([&running]() { running = false; });
    }
    if (app.audioReopen) {
      app.audioReopen = false;
      openAudio(app);
    }
    if (!app.typingKeyboard && app.typing.anyHeld()) {
      for (int p : app.typing.releaseAll()) {
        app.engine.noteOff(app.selChannel, p);
        app.playedNoteOff(app.selChannel, p);
      }
      held.clear();
    }

    double now = static_cast<double>(SDL_GetPerformanceCounter() - t0) / static_cast<double>(SDL_GetPerformanceFrequency());
    {
      prof::Scope ps("tick");
      app.tick(now);
      pumpHostedPlugins(app);
      app.processMidi();
      app.processAudioRecording();  // J-8
    }
    app.engine.setMetronome(app.metronome);
    app.engine.setReturnToSeek(app.settings.getBool("transport.rememberSeek"));  // J-49
    app.hint.clear();
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
    float uiScale = static_cast<float>(std::clamp(app.settings.getNumber("ui.scale"), 0.75, 2.0)) * autoDpi;
    if (uiScale > 1.0f)  // never shrink the logical workspace below the minimum layout size
      uiScale = std::max(1.0f, std::min({uiScale, static_cast<float>(winW) / 960.0f, static_cast<float>(winH) / 600.0f}));

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
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
        runDevInput(app, devInput, frameNo);
        lastScaledEvent = g.InputEventsNextEventId - 1;  // already in logical pixels
      }
    }
    ImGui::NewFrame();
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
    if (firstRun || migrate || app.resetLayout || !ImGui::DockBuilderGetNode(dockId)) {
      const ImVec2 inner(dsize.x - 8, dsize.y - 8);  // dock host padding
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
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
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
    {
      prof::Scope ps("pianoroll");
      drawPianoRoll(app);
    }
    {
      prof::Scope ps("rack");
      if (placeRack && app.showRack) ImGui::SetNextWindowDockID(0, ImGuiCond_Always);  // default layout: the rack floats
      drawChannelRack(app);
      if (placeRack && app.showRack) {  // ... over the playlist's left part (once the window exists)
        if (ImGuiWindow* rw = ImGui::FindWindowByName("Channel rack")) {
          ImGui::SetWindowPos(rw, ImVec2(rackPlacement.x, rackPlacement.y), ImGuiCond_Always);
          ImGui::SetWindowSize(rw, ImVec2(rackPlacement.w, rackPlacement.h), ImGuiCond_Always);
          placeRack = false;
        }
      }
      keepFloatingInView("Channel rack", dpos, dsize);
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
    if (devChords && frameNo == 3) j31::openChordEditor(app);
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

    {
    prof::Scope psRender("render");
    ImGui::Render();
    if (useOffscreen) offscreen.use(true);
    glViewport(0, 0, fbW, fbH);
    ImVec4 bg = theme::vec(theme::pal().chrome);
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (browserui::thumbMode() && !browserui::thumbCapture(app, fbW, fbH)) running = false;
    if (!profilePath.empty() && shotPath.empty() && ++frameNo >= shotFrames + profileWarmup) running = false;
    if (!devShotPending.empty()) {  // J-37: dev-input "shot" (frame sequences)
      writePpm(devShotPending.c_str(), fbW, fbH);
      devShotPending.clear();
    }
    if (!shotPath.empty() && ++frameNo >= shotFrames) {
      // Save the rendered frame (binary PPM) and quit — used for visual checks.
      writePpm(shotPath.c_str(), fbW, fbH);
      if (devHostedEditor && !screenshotHostedEditor(devEditorTarget, shotPath + ".editor.png"))
        std::fprintf(stderr, "no editor screenshot\n");
      running = false;
    }
    if (useOffscreen) offscreen.use(false);
    SDL_GL_SwapWindow(window);
    renderHostedEditors(app);  // control bars of open plugin windows
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
    prof::endFrame();
  }
  if (!profilePath.empty()) {
    std::string label = std::to_string(forcedSize.first) + "x" + std::to_string(forcedSize.second);
    if (prof::writeJson(profilePath, label))
      std::fprintf(stderr, "profile: %d frames -> %s\n", prof::recordedFrames(), profilePath.c_str());
  }

  if (!devSaveAtExit.empty()) {
    captureHostedStates(app);
    std::fprintf(stderr, "%s %s\n", app.saveProject(devSaveAtExit) ? "saved" : "could not save", devSaveAtExit.c_str());
  }
  browserui::shutdownIndex();  // J-46: no worker left running into static teardown
  closeAllHostedEditors(app);
  if (gDevice) SDL_CloseAudioDevice(gDevice);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
