// Gridline — J-67: smooth scrolling and elastic edges in the interface.
#include "SmoothScrollUi.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/Settings.h"
#include "../core/SmoothSnap.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gl::scrollui {

namespace {

smoothscroll::Params gParams;
bool gSmoothSnap = true;
smooth::Glides gGlides;

#if !defined(_WIN32)
std::string query(const char* cmd) {
  std::string out;
  if (FILE* f = popen(cmd, "r")) {
    char buf[128];
    while (std::fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
  }
  return out;
}
#endif

// ImGui scroll regions: one entry per window that is gliding or stretched.
struct WinState {
  smoothscroll::Axis ax[2];
  bool active[2] = {false, false};
  float expect[2] = {0, 0};  // the Scroll ImGui will compute from what we set
  float setTarget[2] = {FLT_MAX, FLT_MAX};
};
std::unordered_map<ImGuiID, WinState> gWins;
struct Pre {
  ImGuiWindow* w;
  ImVec2 target;
};
std::vector<Pre> gPre;

void stopAxis(ImGuiWindow* w, WinState& st, int a) {
  st.active[a] = false;
  st.setTarget[a] = FLT_MAX;
  if (w) w->ScrollOverscroll[a] = 0;
}

}  // namespace

bool systemReducedMotion() {
  static int cached = -1;
  if (cached >= 0) return cached != 0;
  if (const char* e = std::getenv("GRIDLINE_REDUCED_MOTION"); e && *e) {
    cached = (e[0] == '1' || e[0] == 'y' || e[0] == 't') ? 1 : 0;
    return cached != 0;
  }
#if defined(_WIN32)
  // Settings > Accessibility > Visual effects > Animation effects.
  BOOL animate = TRUE;
  cached = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && !animate ? 1 : 0;
#elif defined(__APPLE__)
  cached = query("defaults read com.apple.universalaccess reduceMotion 2>/dev/null").find('1') != std::string::npos ? 1 : 0;
#else
  cached = query("gsettings get org.gnome.desktop.interface enable-animations 2>/dev/null").find("false") != std::string::npos ? 1 : 0;
#endif
  return cached != 0;
}

void configure(const Settings& s) {
  const bool reduced = systemReducedMotion();
  gParams.smooth = s.getBool("ui.smoothScroll") && !reduced;
  gParams.elastic = s.getBool("ui.elasticEdges") && !reduced;
  gSmoothSnap = s.getBool("ui.smoothSnap") && !reduced;
}

bool smoothSnap() { return gSmoothSnap; }

double glide(uint64_t key, double target, bool dragging, double eps) {
  return gGlides.follow(key, target, ImGui::GetTime(), dragging && gSmoothSnap, eps);
}

double glideBar(uint64_t key, double target) {
  return gGlides.follow(key ^ 0x51310000000000ull, target, ImGui::GetTime(), gSmoothSnap, 2e-3, smooth::kBarTau);
}

const smoothscroll::Params& params() { return gParams; }

void beforeNewFrame() {
  gPre.clear();
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx) return;
  for (ImGuiWindow* w : ctx->Windows) gPre.push_back({w, w->ScrollTarget});
}

void afterNewFrame() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  gGlides.prune(ImGui::GetTime());
  const smoothscroll::Params& P = gParams;
  // 1) Outside writes stop a glide: a scroll request made by the app last
  //    frame (SetScrollY, scroll to item), a scrollbar drag, resized content.
  for (const Pre& pr : gPre) {
    auto it = gWins.find(pr.w->ID);
    if (it == gWins.end()) continue;
    WinState& st = it->second;
    for (int a = 0; a < 2; ++a) {
      if (!st.active[a]) continue;
      const bool request = pr.target[a] < FLT_MAX && pr.target[a] != st.setTarget[a];
      if (request || std::fabs(pr.w->Scroll[a] - st.expect[a]) > 1.0f) stopAxis(pr.w, st, a);
    }
  }
  // 2) This frame's wheel: ImGui picked the window (hover, child bubbling,
  //    wheel lock) and jumped its ScrollTarget; take the input over per axis.
  //    Both axes scroll (ImGui keeps only the dominant one, which made a
  //    diagonal trackpad gesture stutter between the axes).
  if (g.WheelingWindow && g.WheelingWindowScrolledFrame == g.FrameCount && (P.smooth || P.elastic)) {
    ImGuiWindow* w = g.WheelingWindow;
    float wheel[2] = {g.IO.MouseWheelH, g.IO.MouseWheel};
    if (g.IO.MouseWheelRequestAxisSwap) wheel[0] = g.IO.MouseWheel, wheel[1] = 0;
    const ImGuiKey keys[2] = {ImGuiKey_MouseWheelX, ImGuiKey_MouseWheelY};
    WinState& st = gWins[w->ID];
    for (int a = 0; a < 2; ++a) {
      if (wheel[a] == 0 || w->ScrollMax[a] == 0 || !ImGui::TestKeyOwner(keys[a], w->ID)) continue;
      if (!st.active[a]) {
        st.ax[a].reset(w->Scroll[a]);
        st.active[a] = true;
      }
      const float size = a == 0 ? w->InnerRect.GetWidth() : w->InnerRect.GetHeight();
      const float stepPx = std::trunc(std::min((a == 0 ? 2.0f : 5.0f) * w->FontRefSize, size * 0.67f));
      st.ax[a].scroll(-static_cast<double>(wheel[a]) * stepPx, smoothscroll::isNotch(wheel[a]), 0, w->ScrollMax[a], P);
      w->ScrollTarget[a] = FLT_MAX;  // not ImGui's jump: the glide below
    }
  }
  // 3) Glide / stretch / spring back every active window.
  for (auto it = gWins.begin(); it != gWins.end();) {
    WinState& st = it->second;
    ImGuiWindow* w = ImGui::FindWindowByID(it->first);
    bool any = false;
    for (int a = 0; a < 2; ++a) {
      if (!st.active[a]) continue;
      if (!w || !w->WasActive) {
        stopAxis(w, st, a);
        continue;
      }
      const double out = st.ax[a].update(g.IO.DeltaTime, 0, w->ScrollMax[a], P);
      const double in = std::clamp(out, 0.0, static_cast<double>(w->ScrollMax[a]));
      const float over = std::round(static_cast<float>(out - in));
      w->ScrollTarget[a] = static_cast<float>(in);
      w->ScrollTargetCenterRatio[a] = 0;
      w->ScrollTargetEdgeSnapDist[a] = 0;
      w->ScrollOverscroll[a] = over;
      st.setTarget[a] = static_cast<float>(in);
      st.expect[a] = std::round(static_cast<float>(in)) + over;
      if (!st.ax[a].animating()) {
        st.active[a] = false;
        st.setTarget[a] = FLT_MAX;
        w->ScrollOverscroll[a] = 0;
      }
      any = any || st.active[a];
    }
    // A window that just settled still applies its last target this frame.
    it = any ? std::next(it) : gWins.erase(it);
  }
}

void step(smoothscroll::Axis& ax, float& v, float delta, bool notch, float lo, float hi) {
  if (hi < lo) hi = lo;
  ax.sync(v);
  if (delta != 0) ax.scroll(delta, notch, lo, hi, gParams);
  v = static_cast<float>(ax.update(ImGui::GetIO().DeltaTime, lo, hi, gParams));
}

void clampOuter(const smoothscroll::Axis& ax, float& v, float lo, float hi) {
  if (hi < lo) hi = lo;
  if (std::fabs(static_cast<double>(v) - ax.output()) <= 1e-3 && ax.animating()) return;  // its own overscroll / glide
  v = std::clamp(v, lo, hi);
}

void stepUnits(smoothscroll::Axis& ax, double& v, double delta, bool notch, double lo, double hi, double unitsPerPx, bool elastic) {
  if (!(unitsPerPx > 0) || !std::isfinite(unitsPerPx)) unitsPerPx = 1;
  if (hi < lo) hi = lo;
  smoothscroll::Params p = gParams;
  p.elastic = p.elastic && elastic;
  // The model works in px: scale in and out so the feel matches the other views.
  ax.sync(v / unitsPerPx, 1e-6);
  if (delta != 0) ax.scroll(delta / unitsPerPx, notch, lo / unitsPerPx, hi / unitsPerPx, p);
  v = ax.update(ImGui::GetIO().DeltaTime, lo / unitsPerPx, hi / unitsPerPx, p) * unitsPerPx;
  if (!p.elastic) v = std::clamp(v, lo, hi);
}

}  // namespace gl::scrollui
