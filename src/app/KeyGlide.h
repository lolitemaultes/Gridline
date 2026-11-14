// Gridline: an audition keyboard that follows the mouse. While the button is held, the key under the
// pointer sounds; sliding onto another key stops the one left behind and starts the new one, so a drag
// plays a glissando one key at a time, as the piano roll's keyboard does. Releasing stops it. Every
// on-screen keyboard of the built-in instruments' editors and of the app's windows plays this way.
//
// No key is left sounding: Escape silences the drag until the button is let go, the pointer leaving
// the window silences it until it comes back over the keys, and a sweep at the start of every frame
// ends any key whose keyboard was not drawn on the frame before (its window closed or hidden), any
// key held while the mouse button is up and every key when the app loses the focus.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include "App.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"

namespace gl::keyglide {

namespace detail {

// A key some keyboard sounds, by the address of the keyboard's held slot (only compared, never read).
struct Live {
  ObjectId ch = kNoId;
  int note = -1;
  int seen = 0;          // the last frame its keyboard was drawn
  bool blocked = false;  // Escape: silent until the button is let go
};
struct Guard {
  std::map<const void*, Live> live;
  App* app = nullptr;
  ImGuiContext* hooked = nullptr;
  std::deque<int> log;  // dev: the notes started (note + 1) and stopped (-(note + 1)), oldest first
};
inline Guard& guard() {
  static Guard g;
  return g;
}
inline void logEvent(int note, bool on) {
  std::deque<int>& l = guard().log;
  l.push_back(on ? note + 1 : -(note + 1));
  while (l.size() > 256) l.pop_front();
}
inline void stop(App& a, Live& l) {
  if (l.note >= 0 && l.ch != kNoId) {
    a.engine.noteOff(l.ch, l.note);
    logEvent(l.note, false);
  }
  l.note = -1;
}
// The start of every frame: ends what the keyboards can no longer end themselves.
inline void sweep(ImGuiContext* ctx, ImGuiContextHook*) {
  Guard& g = guard();
  if (!g.app || !ctx) return;
  const ImGuiIO& io = ctx->IO;
  const int frame = ctx->FrameCount;
  for (auto it = g.live.begin(); it != g.live.end();) {
    Live& l = it->second;
    if (l.seen < frame - 1 || !io.MouseDown[0] || io.AppFocusLost) {
      stop(*g.app, l);
      it = g.live.erase(it);
    } else {
      ++it;
    }
  }
}
inline void install(App& a) {
  Guard& g = guard();
  g.app = &a;
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx || ctx == g.hooked) return;
  ImGuiContextHook hk;
  hk.Type = ImGuiContextHookType_NewFramePost;
  hk.Callback = sweep;
  ImGui::AddContextHook(ctx, &hk);
  g.hooked = ctx;
}
// The pointer is over the window the keyboard is drawn in (a held button keeps the pointer captured
// when it leaves the window, so its position runs on outside).
inline bool pointerInWindow() {
  if (!ImGui::IsMousePosValid()) return false;
  const ImGuiViewport* vp = ImGui::GetWindowViewport();
  if (!vp) return true;
  const ImVec2 m = ImGui::GetIO().MousePos;
  return m.x >= vp->Pos.x && m.y >= vp->Pos.y && m.x < vp->Pos.x + vp->Size.x && m.y < vp->Pos.y + vp->Size.y;
}

}  // namespace detail

// Call every frame after the keyboard's button. `active`: the button is held; `under`: the key under the
// pointer (-1 when it is off the keys: the last key keeps sounding); `held`: the widget's sounding key
// (-1 for none), kept by the caller. Plays only on a channel (`ch` != kNoId). True when a key started.
inline bool follow(App& a, ObjectId ch, bool active, int under, int& held, float velocity) {
  detail::Guard& g = detail::guard();
  detail::install(a);
  auto it = g.live.find(&held);
  detail::Live* l = it == g.live.end() ? nullptr : &it->second;
  if (!l || l->note < 0) held = -1;            // ended by the sweep: already released
  else if (held != l->note) l->note = -1;       // the caller ended it itself (its own Escape or close)
  if (l && !active) l->blocked = false;
  if (active && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (!l) l = &g.live[&held];
    l->blocked = true;
  }
  int want = -1;
  if (active && ch != kNoId && !(l && l->blocked) && detail::pointerInWindow()) want = under >= 0 && under <= 127 ? under : held;
  bool started = false;
  if (l && l->note >= 0 && (l->note != want || l->ch != ch)) detail::stop(a, *l);
  if (want >= 0 && !(l && l->note == want)) {
    a.engine.noteOn(ch, want, velocity);
    detail::logEvent(want, true);
    if (!l) l = &g.live[&held];
    l->ch = ch;
    l->note = want;
    started = true;
  }
  held = want;
  if (l) {
    l->seen = ImGui::GetFrameCount();
    if (l->note < 0 && !l->blocked) g.live.erase(&held);
  }
  return started;
}

// The sounding key of a keyboard kind that several editors share (one static per kind): it belongs to
// the keyboard item that was pressed last, on that item's channel.
struct Held {
  int note = -1;
  ImGuiID item = 0;
  ObjectId ch = kNoId;
};

// follow() for the last ImGui item (the keyboard's button) and `ch` (kNoId: not a channel, silent).
// Pressing it takes `h` over (a key another keyboard still holds stops); the other keyboards sharing
// `h` leave it alone. Returns the key it sounds (-1 for none).
inline int followItem(App& a, ObjectId ch, int under, Held& h, float velocity) {
  const ImGuiID item = ImGui::GetItemID();
  const bool active = ImGui::IsItemActive();
  if (active && h.item != item) {
    follow(a, h.ch, false, -1, h.note, velocity);  // the other keyboard's key stops
    h = Held{-1, item, ch};
  }
  if (h.item != item) return -1;
  follow(a, h.ch, active, under, h.note, velocity);
  return h.note;
}

// dev (`keybeds` in --dev-input): each keyboard drawn in the last frames, by name: its rectangle, lowest
// key, white key count, the key it sounds and the points a test drags through (eight keys in a row:
// white keys of a piano, neighbouring keys of a strip of equal keys).
struct DevBed {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int lo = 0, whites = 0, held = -1, frame = 0;
  float keyX = 0, keyW = 0;  // a strip of equal keys (whites 0): the left edge of key `lo` and the key width
};
inline std::map<std::string, DevBed>& devBeds() {
  static std::map<std::string, DevBed> m;
  return m;
}
// A piano: `whites` white keys from `lo` (a C) across the rectangle.
inline void devNote(const std::string& name, ImVec2 p0, ImVec2 p1, int lo, int whites, int held) {
  devBeds()[name] = DevBed{p0.x, p0.y, p1.x, p1.y, lo, whites, held, ImGui::GetFrameCount(), 0, 0};
}
// A strip of equal keys: key `lo` starts at `keyX`, each key `keyW` wide.
inline void devNoteStrip(const std::string& name, ImVec2 p0, ImVec2 p1, int lo, float keyX, float keyW, int held) {
  devBeds()[name] = DevBed{p0.x, p0.y, p1.x, p1.y, lo, 0, held, ImGui::GetFrameCount(), keyX, keyW};
}
inline std::string devReport() {
  static const int wOff[7] = {0, 2, 4, 5, 7, 9, 11};
  std::string out;
  const int now = ImGui::GetFrameCount();
  for (const auto& [name, b] : devBeds()) {
    if (now - b.frame > 2) continue;  // not drawn lately
    char line[256];
    std::snprintf(line, sizeof line, "keybed %s x0 %.1f y0 %.1f x1 %.1f y1 %.1f lo %d whites %d held %d probes", name.c_str(), static_cast<double>(b.x0),
                  static_cast<double>(b.y0), static_cast<double>(b.x1), static_cast<double>(b.y1), b.lo, b.whites, b.held);
    out += line;
    for (int i = 0; i < 8; ++i) {
      int note = 0;
      float x = 0, y = 0;
      if (b.whites > 0) {  // white keys 2 .. 9, low on the keys where only white keys are
        const int wi = 2 + i;
        note = b.lo + 12 * (wi / 7) + wOff[wi % 7];
        x = b.x0 + (b.x1 - b.x0) * (2.0f * static_cast<float>(wi) + 1.0f) / (2.0f * static_cast<float>(b.whites));
        y = b.y0 + (b.y1 - b.y0) * 0.85f;
      } else {  // the keys after the first one in view, through their middle
        const float first = b.keyW > 0 ? std::max(0.0f, std::ceil((b.x0 - b.keyX) / b.keyW)) : 0.0f;
        const int k = static_cast<int>(first) + 1 + i;
        note = b.lo + k;
        x = b.keyX + b.keyW * (static_cast<float>(k) + 0.5f);
        y = b.y0 + (b.y1 - b.y0) * 0.75f;
      }
      std::snprintf(line, sizeof line, " %d:%d:%d", note, static_cast<int>(x), static_cast<int>(y));
      out += line;
    }
    out += "\n";
  }
  out += "glidelog";
  for (int e : detail::guard().log) {
    char ev[24];
    std::snprintf(ev, sizeof ev, " %s:%d", e > 0 ? "on" : "off", (e > 0 ? e : -e) - 1);
    out += ev;
  }
  out += "\n";
  return out;
}

}  // namespace gl::keyglide
