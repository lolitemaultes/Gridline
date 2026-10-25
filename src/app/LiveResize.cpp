// Gridline — J-116: windows redraw while they are being resized (see LiveResize.h).
#include "LiveResize.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <utility>
#include <vector>
#include "../../third_party/imgui/imgui_impl_sdl2.h"
#include "../core/LiveResize.h"
#include "../host/ui/NativeView.h"
#include "Viewports.h"

namespace gl::liveresize {
namespace {

struct LiveFrame {
  Uint32 window;  // the window whose event drew it
  int w, h;       // that window's size the frame was drawn at
  double ms;      // how long the frame took
  double sincePrev;  // ms since the frame before it
};

struct State {
  SDL_Window* main = nullptr;
  std::function<void()> frame;
  SDL_threadID thread = 0;
  bool installed = false, inPump = false, inFrame = false, live = false, simModal = false;
  Uint64 pumpStart = 0, lastFrame = 0;
  std::map<Uint32, std::pair<int, int>> sizes;  // every Gridline window's size at the start of the last frame
  std::vector<std::function<void()>> dev;
  int liveFrames = 0;
  std::deque<LiveFrame> recent;  // dev: the last live frames
} L;

double secondsSince(Uint64 t) {
  return t ? static_cast<double>(SDL_GetPerformanceCounter() - t) / static_cast<double>(SDL_GetPerformanceFrequency()) : 1e9;
}

std::pair<int, int> sizeOf(Uint32 id) {
  int w = 0, h = 0;
  if (SDL_Window* win = SDL_GetWindowFromID(id)) SDL_GetWindowSize(win, &w, &h);
  return {w, h};
}

bool ours(Uint32 id) { return id != 0 && ((L.main && id == SDL_GetWindowID(L.main)) || viewports::ownsWindow(id)); }

// SDL calls this for every event as it is generated, on the thread generating it: inside the event
// pump for window events (also from inside a modal resize loop that blocks it).
int SDLCALL watch(void*, SDL_Event* e) {
  if (!L.installed || e->type != SDL_WINDOWEVENT || SDL_ThreadID() != L.thread) return 0;
  live::Kind kind = live::Kind::Expose;
  switch (e->window.event) {
    case SDL_WINDOWEVENT_SIZE_CHANGED:
    case SDL_WINDOWEVENT_RESIZED: kind = live::Kind::Resize; break;
    case SDL_WINDOWEVENT_EXPOSED: kind = live::Kind::Expose; break;
    case SDL_WINDOWEVENT_MOVED: kind = live::Kind::Move; break;
    default: return 0;
  }
  if (!L.inPump || L.inFrame || !ours(e->window.windowID)) return 0;
  const bool modal = L.simModal || host::native::inModalSizeLoop();
  const std::pair<int, int> now = sizeOf(e->window.windowID);
  const auto was = L.sizes.find(e->window.windowID);
  const bool changed = was == L.sizes.end() || was->second != now;
  if (!live::shouldDraw(kind, L.inPump, L.inFrame, modal, secondsSince(L.pumpStart), changed, secondsSince(L.lastFrame))) return 0;
  // The backend learns about the new size now (it reads the event from the queue again later,
  // which only asks it to read the same size once more). It acts on RESIZED only, and SDL
  // reports SIZE_CHANGED first (from inside its RESIZED handling): that one is passed as RESIZED.
  SDL_Event copy = *e;
  if (copy.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) copy.window.event = SDL_WINDOWEVENT_RESIZED;
  ImGui_ImplSDL2_ProcessEvent(&copy);
  const double sincePrev = L.lastFrame ? secondsSince(L.lastFrame) * 1000.0 : 0.0;
  const Uint64 t = SDL_GetPerformanceCounter();
  L.frame();  // frameBegin(true) / frameEnd() inside
  L.recent.push_back(LiveFrame{e->window.windowID, now.first, now.second, secondsSince(t) * 1000.0, sincePrev});
  while (L.recent.size() > 256) L.recent.pop_front();
  ++L.liveFrames;
  return 0;
}

}  // namespace

void install(SDL_Window* mainWindow, std::function<void()> frame) {
  L.main = mainWindow;
  L.frame = std::move(frame);
  L.thread = SDL_ThreadID();
  if (std::getenv("GRIDLINE_NO_LIVE_RESIZE")) return;  // dev: the behaviour before J-116 (for comparisons)
  if (!L.installed) SDL_AddEventWatch(watch, nullptr);
  L.installed = true;
}

void uninstall() {
  if (!L.installed) return;
  SDL_DelEventWatch(watch, nullptr);
  L.installed = false;
  L.frame = nullptr;
}

void pumpBegin() {
  L.inPump = true;
  L.pumpStart = SDL_GetPerformanceCounter();
}

void pumpEnd() {
  if (!L.dev.empty() && !L.inFrame) {  // dev actions run inside the pump, like the user's resize
    std::vector<std::function<void()>> d;
    d.swap(L.dev);
    for (auto& f : d) f();
  }
  L.inPump = false;
}

void frameBegin(bool live) {
  L.inFrame = true;
  L.live = live;
  L.lastFrame = SDL_GetPerformanceCounter();
  L.sizes.clear();
  if (L.main) L.sizes[SDL_GetWindowID(L.main)] = sizeOf(SDL_GetWindowID(L.main));
  for (Uint32 id : viewports::windowIds()) L.sizes[id] = sizeOf(id);
}

void frameEnd() {
  L.inFrame = false;
  L.live = false;
}

bool inLiveFrame() { return L.live; }
void queueDev(std::function<void()> f) { L.dev.push_back(std::move(f)); }
void setSimulatedModal(bool on) { L.simModal = on; }
int liveFrames() { return L.liveFrames; }

void dump(int frame) {
  double sum = 0, mx = 0, isum = 0;
  int in = 0;
  for (const LiveFrame& f : L.recent) {
    sum += f.ms, mx = std::max(mx, f.ms);
    if (f.sincePrev > 0 && f.sincePrev < 1000) isum += f.sincePrev, ++in;
  }
  const size_t n = L.recent.size();
  std::printf("liveresize frame %d: live-frames %d recent %zu avg-ms %.3f max-ms %.3f interval-ms %.3f\n", frame, L.liveFrames, n,
              n ? sum / static_cast<double>(n) : 0.0, mx, in ? isum / in : 0.0);
  for (const LiveFrame& f : L.recent)
    std::printf("  live window %u size %d %d ms %.3f since %.3f\n", static_cast<unsigned>(f.window), f.w, f.h, f.ms, f.sincePrev);
  std::fflush(stdout);
}

}  // namespace gl::liveresize
