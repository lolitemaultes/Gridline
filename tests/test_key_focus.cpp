// Gridline, GitHub issue #52: keys and typed text that SDL sends naming no window (it was never told
// which window has the keyboard focus) reach the interface.
//
// 1. keyfocus::adopt (src/app/KeyFocus.h) gives key, text and text-editing events with window id 0
//    to the main window and leaves everything else alone.
// 2. With a real (hidden) SDL window and the interface's own SDL input layer: such an event used to
//    be dropped (the cause of the dead keyboard), and the same event after adopt is taken.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "Check.h"
#include "app/KeyFocus.h"
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_internal.h"

using namespace gl;

namespace {

SDL_Event key(Uint32 type, Uint32 windowId) {
  SDL_Event e{};
  e.type = type;
  e.key.windowID = windowId;
  e.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
  e.key.keysym.sym = SDLK_SPACE;
  e.key.keysym.scancode = SDL_SCANCODE_SPACE;
  return e;
}

SDL_Event text(const char* s, Uint32 windowId) {
  SDL_Event e{};
  e.type = SDL_TEXTINPUT;
  e.text.windowID = windowId;
  std::snprintf(e.text.text, sizeof e.text.text, "%s", s);
  return e;
}

}  // namespace

int main() {
  // ---- 1. the rule on its own
  {
    SDL_Event e = key(SDL_KEYDOWN, 0);
    expect(keyfocus::adopt(e, 7) && e.key.windowID == 7, "key down naming no window goes to the main window");
    e = key(SDL_KEYUP, 0);
    expect(keyfocus::adopt(e, 7) && e.key.windowID == 7, "key up naming no window goes to the main window");
    e = text("a", 0);
    expect(keyfocus::adopt(e, 7) && e.text.windowID == 7 && std::strcmp(e.text.text, "a") == 0, "typed text naming no window goes to the main window");
    e = SDL_Event{};
    e.type = SDL_TEXTEDITING;
    expect(keyfocus::adopt(e, 7) && e.edit.windowID == 7, "composed text naming no window goes to the main window");
    e = key(SDL_KEYDOWN, 9);
    expect(!keyfocus::adopt(e, 7) && e.key.windowID == 9, "a key for another window (a plugin window) stays there");
    e = text("b", 9);
    expect(!keyfocus::adopt(e, 7) && e.text.windowID == 9, "text for another window stays there");
    e = SDL_Event{};
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.windowID = 0;
    expect(!keyfocus::adopt(e, 7) && e.button.windowID == 0, "other events are left alone");
    e = key(SDL_KEYDOWN, 0);
    expect(!keyfocus::adopt(e, 0) && e.key.windowID == 0, "no main window: nothing changes");
  }

  // ---- 2. the interface's SDL input layer, on a hidden window
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::printf("SKIP: no video (%s)\n", SDL_GetError());
    return finish() == 0 ? 77 : 1;
  }
  SDL_Window* w = SDL_CreateWindow("key focus test", 0, 0, 320, 200, SDL_WINDOW_HIDDEN);
  expect(w != nullptr, "hidden window created");
  if (w) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL2_InitForOther(w);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const Uint32 id = SDL_GetWindowID(w);

    SDL_Event e = key(SDL_KEYDOWN, 0);
    int before = g.InputEventsQueue.Size;
    const bool tookOrphan = ImGui_ImplSDL2_ProcessEvent(&e);
    expect(!tookOrphan && g.InputEventsQueue.Size == before, "before the fix: a key naming no window is dropped (the reported fault)");
    e = text("x", 0);
    before = g.InputEventsQueue.Size;
    expect(!ImGui_ImplSDL2_ProcessEvent(&e) && g.InputEventsQueue.Size == before, "before the fix: typed text naming no window is dropped");

    e = key(SDL_KEYDOWN, 0);
    keyfocus::adopt(e, id);
    before = g.InputEventsQueue.Size;
    expect(ImGui_ImplSDL2_ProcessEvent(&e) && g.InputEventsQueue.Size > before, "after the fix: the key reaches the interface");
    bool spaceDown = false;
    for (const ImGuiInputEvent& q : g.InputEventsQueue)
      spaceDown = spaceDown || (q.Type == ImGuiInputEventType_Key && q.Key.Key == ImGuiKey_Space && q.Key.Down);
    expect(spaceDown, "after the fix: Space is down for the interface");
    e = text("x", 0);
    keyfocus::adopt(e, id);
    before = g.InputEventsQueue.Size;
    expect(ImGui_ImplSDL2_ProcessEvent(&e) && g.InputEventsQueue.Size > before, "after the fix: typed text reaches the interface");

    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(w);
  }
  SDL_Quit();
  return finish();
}
