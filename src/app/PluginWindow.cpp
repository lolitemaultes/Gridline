// Gridline — plugin windows of installed (VST3 / CLAP / VST2 / AU) plugins.
//
// An installed plugin opens as itself: a Gridline-owned native window whose
// top is the plugin's own interface (attached to a container view), with a
// compact control bar under it and the settings pages unfolding under the
// bar (geometry: host/EditorLayout.h). The bar and the panel are drawn with
// Dear ImGui in the window's own GL + ImGui context, sharing the main
// context's GL objects and font atlas, so they look like the rest of
// Gridline. The plugin view sits above the GL surface and is never covered.
//
// Sandboxed plugins show their interface in a window of the sandbox process;
// Gridline docks a bar-only window right under it and follows it.
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_syswm.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include "../core/I18n.h"
#include "../host/EditorLayout.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/ui/NativeView.h"
#include "App.h"
#include "HostedUi.h"
#include "HostedUiPages.h"
#include "Theme.h"
#include "Widgets.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_impl_sdl2.h"

namespace gl {

namespace lay = host::layout;
namespace native = host::native;

namespace {

bool gLog = false;
int gStartPanel = -1;  // dev: settings panel page new windows start with
int gDevResizeW = 0, gDevResizeH = 0;  // dev: first window dragged to this size after a few frames
bool gDevResized = false;
std::string gDevAction;  // dev: bar button pressed on the first window ("generic", "sandbox", "settings")
SDL_Window* gMainWin = nullptr;
SDL_GLContext gMainGl = nullptr;
std::thread::id gUiThread;

#if defined(__APPLE__)
const char* kGlsl = "#version 150";
#else
const char* kGlsl = "#version 130";
#endif

// Control bar look (slate wrapper roles, drawn by Gridline).
struct BarLook {
  ImU32 top = theme::rgb(0x434C52), bottom = theme::rgb(0x323A3F), edge = theme::rgb(0x23292D);
  ImU32 text = theme::rgb(0xE4ECEE), dim = theme::rgb(0x9FA8AD), faint = theme::rgb(0x6C767C);
  ImU32 btn = theme::rgb(0x2A3237), btnHov = theme::rgb(0x36404A), btnOn = theme::rgb(0x3D63DD);
  ImU32 lcd = theme::rgb(0x161A1D), lcdText = theme::rgb(0x9FE0A8);
  ImU32 led = theme::rgb(0x4CD964), ledOff = theme::rgb(0x55402F), warn = theme::rgb(0xE5484D);
  ImU32 panel = theme::rgb(0x262C30);
};

native::Window nativeOf(SDL_Window* w) {
  native::Window n;
  if (!w) return n;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(w, &info)) return n;
#if defined(__APPLE__)
  if (info.subsystem == SDL_SYSWM_COCOA) n.handle = static_cast<void*>(info.info.cocoa.window);
#elif defined(SDL_VIDEO_DRIVER_X11)
  if (info.subsystem == SDL_SYSWM_X11) {
    n.display = static_cast<void*>(info.info.x11.display);
    n.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(info.info.x11.window));
  }
#endif
  return n;
}

lay::Rect workArea() {
  int d = gMainWin ? SDL_GetWindowDisplayIndex(gMainWin) : 0;
  SDL_Rect r;
  if (SDL_GetDisplayUsableBounds(d < 0 ? 0 : d, &r) != 0) return lay::Rect{0, 0, 1440, 900};
  return lay::Rect{r.x, r.y, r.w, r.h};
}

std::map<uint64_t, std::pair<int, int>>& places() {  // remembered window positions per target
  static std::map<uint64_t, std::pair<int, int>> m;
  return m;
}
std::map<uint64_t, std::pair<int, int>>& sizes() {  // last size of resizable interfaces per target
  static std::map<uint64_t, std::pair<int, int>> m;
  return m;
}
std::set<uint64_t>& panelRequested() {  // targets whose Gridline panel was asked for explicitly
  static std::set<uint64_t> s;
  return s;
}
std::map<uint64_t, double>& reopen() {  // targets to show again once their new instance runs
  static std::map<uint64_t, double> m;
  return m;
}
std::map<uint64_t, bool>& channelBypass() {  // instruments bypassed from the bar (plugin's own bypass)
  static std::map<uint64_t, bool> m;
  return m;
}

// Makes a GL context current for real. SDL skips the call when it believes
// the context is current already, but plugin interfaces that draw with
// OpenGL (Kickstart, JUCE GL editors…) switch the thread's context behind
// SDL's back: clear first, then bind.
void makeCurrent(SDL_Window* win, SDL_GLContext gl) {
  if (!win || !gl) return;
  SDL_GL_MakeCurrent(win, nullptr);
  SDL_GL_MakeCurrent(win, gl);
}

// Restores the main window's GL and ImGui contexts.
struct ContextGuard {
  ImGuiContext* imgui = ImGui::GetCurrentContext();
  SDL_Window* win = SDL_GL_GetCurrentWindow();
  SDL_GLContext gl = SDL_GL_GetCurrentContext();
  ~ContextGuard() {
    if (win && gl) makeCurrent(win, gl);
    ImGui::SetCurrentContext(imgui);
  }
};

enum class Action { None, Generic, Sandbox, Close, ChannelPanel };

struct PluginWindow {
  App* app = nullptr;
  PluginTarget target;
  std::string type, name, format;
  host::Backend* backend = nullptr;
  std::unique_ptr<host::Editor> editor;
  SDL_Window* win = nullptr;
  SDL_GLContext gl = nullptr;
  ImGuiContext* ctx = nullptr;
  native::Window nw;
  void* container = nullptr;
  bool external = false;  // sandboxed: the interface is in the sandbox process' window
  bool resizable = false;
  bool dead = false;
  bool shown = false;
  int pw = 0, ph = 0;  // plugin view size
  int askedW = 0, askedH = 0;  // size the plugin asked for first (report)
  int panelPage = -1;  // -1: panel folded
  lay::WindowLayout L;
  int applying = 0;  // inside our own SDL_SetWindowSize
  // Resize asked for off the UI thread (CLAP request_resize is thread-safe).
  std::atomic<int> pendingW{0}, pendingH{0};
  bool notifySize = false;  // tell the plugin its new size on the next idle
  int extFrame[4] = {0, 0, 0, 0};
  Action action = Action::None;
  std::string capturePath;  // dev check: capture on the next draw
  int frames = 0;           // frames drawn since opening

  int panelH() const { return panelPage >= 0 ? lay::kPanelH : 0; }
  void computeLayout() { L = external ? lay::layoutBarWindow(extFrame[2] > 0 ? extFrame[2] : 420, panelH()) : lay::layoutWindow(pw, ph, panelH()); }

  void log(const char* what) {
    if (!gLog) return;
    const lay::BarLayout B = lay::layoutBar(L.w);
    std::fprintf(stderr, "[plugin-window] %s | %s (%s)%s: plugin %dx%d (asked %dx%d)%s -> window %dx%d, bar %d (%d row%s), panel %d, scale %.1f\n", what,
                 name.c_str(), format.c_str(), external ? " sandbox" : "", pw, ph, askedW, askedH, resizable ? " resizable" : "", L.w, L.h, L.bar.h,
                 B.rows, B.rows == 1 ? "" : "s", L.panel.h, native::backingScale(nw));
  }

  // Window and container follow the current layout.
  void apply() {
    computeLayout();
    if (!win) return;
    int cw = 0, ch = 0;
    SDL_GetWindowSize(win, &cw, &ch);
    if (cw != L.w || ch != L.h) {
      ++applying;
      SDL_SetWindowSize(win, L.w, L.h);
      --applying;
    }
    if (container) native::setContainerFrame(nw, container, L.plugin);
  }

  // The plugin asks for a new size (UI thread).
  bool pluginResize(int w, int h) {
    if (dead) return false;
    const int ow = pw, oh = ph;
    pw = std::max(w, lay::kMinPluginW);
    ph = std::max(h, lay::kMinPluginH);
    apply();
    notifySize = true;
    if (pw != ow || ph != oh) log("plugin resized");
    return true;
  }

  // The user dragged the window to w x h.
  void userResize(int w, int h) {
    if (external || !editor || (w == L.w && h == L.h)) return;
    if (!resizable) {  // fixed-size interface: the window keeps fitting it
      apply();
      return;
    }
    int nw2 = 0, nh2 = 0;
    lay::pluginSizeForWindow(w, h, panelH(), nw2, nh2);
    editor->constrain(nw2, nh2);
    pw = std::max(nw2, lay::kMinPluginW);
    ph = std::max(nh2, lay::kMinPluginH);
    apply();
    editor->setSize(pw, ph);
    log("window resized");
  }

  void setPanel(int page) {
    if (page == panelPage) return;
    panelPage = page;
    apply();
    log(page >= 0 ? "settings unfolded" : "settings folded");
  }

  void close() {
    if (dead) return;
    if (gLog) std::fprintf(stderr, "[plugin-window] closed | %s\n", name.c_str());
    dead = true;
    if (win) {
      int x = 0, y = 0;
      SDL_GetWindowPosition(win, &x, &y);
      if (!external) places()[target.key()] = {x, y};
      if (!external && resizable) sizes()[target.key()] = {pw, ph};
    }
    if (external && extFrame[2] > 0) places()[target.key()] = {extFrame[0], extFrame[1]};
    editor.reset();  // detaches the plugin's view before its container and window go
    if (container) native::destroyContainer(nw, container);
    container = nullptr;
    if (ctx) {
      ContextGuard g;
      ImGuiContext* dying = ctx;
      if (gl) makeCurrent(win, gl);
      ImGui::SetCurrentContext(ctx);
      ImGui_ImplOpenGL3_Shutdown();
      ImGui_ImplSDL2_Shutdown();
      ImGui::DestroyContext(ctx);
      ctx = nullptr;
      if (g.imgui == dying) g.imgui = nullptr;
      if (g.win == win) g.win = nullptr;
    }
    if (gl) SDL_GL_DeleteContext(gl);
    gl = nullptr;
    if (win) {
      if (gMainWin) native::keepAbove(nw, nativeOf(gMainWin), false);
      SDL_DestroyWindow(win);
    }
    win = nullptr;
    if (backend) backend->onGoingAway = nullptr;
    backend = nullptr;
  }
  ~PluginWindow() { close(); }
};

std::map<uint64_t, std::unique_ptr<PluginWindow>>& windows() {
  static std::map<uint64_t, std::unique_ptr<PluginWindow>> m;
  return m;
}

PluginWindow* windowFor(const PluginTarget& t) {
  auto it = windows().find(t.key());
  return it != windows().end() && !it->second->dead ? it->second.get() : nullptr;
}

// GL + ImGui context of a plugin window (shares the main context's GL
// objects and font atlas). The main contexts are current again afterwards.
bool createSurface(PluginWindow& w) {
  ContextGuard g;
  if (gMainWin && gMainGl) makeCurrent(gMainWin, gMainGl);
  ImGuiContext* mainCtx = ImGui::GetCurrentContext();
  ImFontAtlas* atlas = mainCtx ? ImGui::GetIO().Fonts : nullptr;
  ImFont* defFont = mainCtx ? ImGui::GetIO().FontDefault : nullptr;
  const ImGuiStyle style = mainCtx ? ImGui::GetStyle() : ImGuiStyle();
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  w.gl = SDL_GL_CreateContext(w.win);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  if (!w.gl) return false;
  makeCurrent(w.win, w.gl);
  SDL_GL_SetSwapInterval(0);  // the main window paces the frames
  w.ctx = ImGui::CreateContext(atlas);
  ImGui::SetCurrentContext(w.ctx);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigDragClickToInputText = true;
  io.FontDefault = defFont;
  ImGui::GetStyle() = style;
  ImGui_ImplSDL2_InitForOpenGL(w.win, w.gl);
  ImGui_ImplOpenGL3_Init(kGlsl);
  return true;
}

// ---------------------------------------------------------------- control bar

bool barButton(const char* id, const lay::Rect& r, float y0) {
  ImGui::SetCursorPos(ImVec2(static_cast<float>(r.x), y0 + static_cast<float>(r.y)));
  return ImGui::InvisibleButton(id, ImVec2(static_cast<float>(r.w), static_cast<float>(r.h)));
}

void boxAt(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 fill, ImU32 edge) {
  dl->AddRectFilled(p0, p1, fill, 3.0f);
  dl->AddRect(p0, p1, edge, 3.0f);
}

void centredText(ImDrawList* dl, ImFont* f, float size, ImVec2 p0, ImVec2 p1, ImU32 col, const char* text) {
  ImVec2 ts = f->CalcTextSizeA(size, 1e9f, 0, text);
  dl->AddText(f, size, ImVec2(std::round((p0.x + p1.x - ts.x) * 0.5f), std::round((p0.y + p1.y - ts.y) * 0.5f)), col, text);
}

void drawGear(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  for (int k = 0; k < 8; ++k) {
    const float a = static_cast<float>(k) * 3.14159265f / 4.0f;
    const ImVec2 d(std::cos(a), std::sin(a));
    dl->AddLine(ImVec2(c.x + d.x * r * 0.55f, c.y + d.y * r * 0.55f), ImVec2(c.x + d.x * r, c.y + d.y * r), col, 2.6f);
  }
  dl->AddCircle(c, r * 0.62f, col, 16, 2.0f);
  dl->AddCircleFilled(c, r * 0.22f, col, 10);
}

void drawPower(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->PathArcTo(c, r, -3.14159265f * 0.5f + 0.75f, 3.14159265f * 1.5f - 0.75f, 18);
  dl->PathStroke(col, 0, 1.8f);
  dl->AddLine(ImVec2(c.x, c.y - r - 1.5f), ImVec2(c.x, c.y - 1.0f), col, 1.8f);
}

void drawBar(PluginWindow& w, host::HostedInstance* hi) {
  App& a = *w.app;
  const BarLook K;
  const PluginTarget t = w.target;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  const float by = o.y;  // bar top in screen space (the ImGui window starts at the bar)
  const float W = static_cast<float>(w.L.w), H = static_cast<float>(w.L.bar.h);
  dl->AddRectFilledMultiColor(ImVec2(o.x, by), ImVec2(o.x + W, by + H), K.top, K.top, K.bottom, K.bottom);
  dl->AddLine(ImVec2(o.x, by + 0.5f), ImVec2(o.x + W, by + 0.5f), K.edge, 1.0f);
  const lay::BarLayout B = lay::layoutBar(w.L.w);
  auto R = [&](lay::BarItem it, ImVec2& p0, ImVec2& p1) {
    const lay::Rect& r = B.items[it];
    p0 = ImVec2(o.x + static_cast<float>(r.x), by + static_cast<float>(r.y));
    p1 = ImVec2(p0.x + static_cast<float>(r.w), p0.y + static_cast<float>(r.h));
  };
  ImFont* fr = a.fontRegular ? a.fontRegular : ImGui::GetFont();
  ImFont* fm = a.fontMono ? a.fontMono : fr;
  ImVec2 p0, p1;

  // Presets.
  const int np = hi ? hostedui::presetCount(hi) : 0;
  int& cur = hostedui::presetIndex(t);
  R(lay::kBarPrev, p0, p1);
  if (barButton("##prev", B.items[lay::kBarPrev], 0) && np > 0) hostedui::selectPreset(a, t, hi, cur <= 0 ? np - 1 : cur - 1);
  ui::Hint("Previous preset of the plugin");
  boxAt(dl, p0, p1, ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
  {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x + 3, c.y - 5), ImVec2(c.x + 3, c.y + 5), ImVec2(c.x - 4, c.y), np > 0 ? K.text : K.faint);
  }
  R(lay::kBarPreset, p0, p1);
  if (barButton("##preset", B.items[lay::kBarPreset], 0) && np > 0) ImGui::OpenPopup("##presets");
  ui::Hint("The plugin's own programs and preset library");
  boxAt(dl, p0, p1, K.lcd, ImGui::IsItemHovered() ? K.btnOn : K.edge);
  {
    std::string pname = cur >= 0 && cur < np ? hostedui::presetName(hi, cur) : (np > 0 ? std::string("Presets") : std::string("No presets"));
    const float maxW = p1.x - p0.x - 12;
    const char* fitted = theme::fitText(pname.c_str(), maxW);
    ImVec2 ts = fm->CalcTextSizeA(11.0f, 1e9f, 0, fitted);
    dl->AddText(fm, 11.0f, ImVec2(p0.x + 6, std::round((p0.y + p1.y - ts.y) * 0.5f)), np > 0 ? K.lcdText : K.faint, fitted);
  }
  R(lay::kBarNext, p0, p1);
  if (barButton("##next", B.items[lay::kBarNext], 0) && np > 0) hostedui::selectPreset(a, t, hi, cur < 0 ? 0 : (cur + 1) % np);
  ui::Hint("Next preset of the plugin");
  boxAt(dl, p0, p1, ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
  {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x - 3, c.y - 5), ImVec2(c.x - 3, c.y + 5), ImVec2(c.x + 4, c.y), np > 0 ? K.text : K.faint);
  }
  if (ImGui::BeginPopup("##presets")) {
    if (hi->programCount() > 0) ImGui::TextDisabled("%s", i18n::tr("Programs"));
    for (int k = 0; k < hi->programCount(); ++k)
      if (ImGui::Selectable((hi->programName(k) + "##p" + std::to_string(k)).c_str(), k == cur)) hostedui::selectPreset(a, t, hi, k);
    std::string cat = "\x01";
    const auto& lib = hi->libraryPresets();
    for (size_t k = 0; k < lib.size(); ++k) {
      if (lib[k].category != cat) {
        cat = lib[k].category;
        ImGui::TextDisabled("%s", cat.empty() ? "Library" : cat.c_str());
      }
      const int idx = hi->programCount() + static_cast<int>(k);
      if (ImGui::Selectable((lib[k].name + "##l" + std::to_string(k)).c_str(), idx == cur)) hostedui::selectPreset(a, t, hi, idx);
    }
    ImGui::EndPopup();
  }

  // Bypass: effects switch their mixer slot; instruments use the plugin's own bypass.
  {
    bool avail = true, on = true;
    if (!t.isChannel()) {
      on = getParam(a.doc.song(), slotEnableParam(t.owner, t.slot)).value_or(1.0) >= 0.5;
    } else {
      avail = hi && hi->backend()->hasBypass();
      on = !channelBypass()[t.key()];
    }
    R(lay::kBarBypass, p0, p1);
    if (barButton("##bypass", B.items[lay::kBarBypass], 0) && avail) {
      if (!t.isChannel()) {
        a.doc.setParam(slotEnableParam(t.owner, t.slot), on ? 0.0 : 1.0, false);
      } else if (hi->backend()->setBypass(on)) {
        channelBypass()[t.key()] = on;
      }
    }
    ui::Hint(!avail ? "This instrument has no bypass of its own" : on ? "Plugin on: click to bypass it" : "Plugin bypassed: click to switch it on");
    boxAt(dl, p0, p1, ImGui::IsItemHovered() && avail ? K.btnHov : K.btn, K.edge);
    drawPower(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f + 1), 5.5f, !avail ? K.faint : on ? K.led : K.warn);
  }

  // Sandbox / generic switches.
  auto toggle = [&](lay::BarItem it, const char* id, const char* label, bool lit, const char* hint) {
    R(it, p0, p1);
    const bool clicked = barButton(id, B.items[it], 0);
    ui::Hint(hint);
    boxAt(dl, p0, p1, lit ? K.btnOn : ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
    centredText(dl, fr, 10.5f, p0, p1, lit ? theme::rgb(0xFFFFFF) : K.text, label);
    return clicked;
  };
  const bool sandboxed = hi && hi->sandboxed();
  if (toggle(lay::kBarSandbox, "##sandbox", "SANDBOX", sandboxed,
             sandboxed ? "Runs in its own process (a crash only silences it). Click to run it inside Gridline"
                       : "Run this plugin in its own process: if it crashes, Gridline keeps running"))
    w.action = Action::Sandbox;
  if (toggle(lay::kBarGeneric, "##generic", "GENERIC", false, "Show Gridline's parameter list instead of the plugin's interface"))
    w.action = Action::Generic;

  // CPU / latency readout.
  if (B.visible[lay::kBarCpu] && hi) {
    R(lay::kBarCpu, p0, p1);
    float avg = 0, pk = 0;
    hostedui::cpuLoad(a, t, hi, avg, pk);
    dl->AddRectFilled(p0, p1, K.lcd, 3.0f);
    const float fillW = std::clamp(avg, 0.0f, 1.0f) * (p1.x - p0.x);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - 3), ImVec2(p0.x + fillW, p1.y), avg > 0.5f ? K.warn : K.btnOn, 1.0f);
    char line[64];
    const double sr = hi->backend()->sampleRate() > 0 ? hi->backend()->sampleRate() : 48000.0;
    std::snprintf(line, sizeof line, "CPU %.1f%%  %.1fms", 100.0 * avg, 1000.0 * hi->totalLatency() / sr);
    centredText(dl, fm, 10.0f, p0, ImVec2(p1.x, p1.y - 2), K.dim, line);
    ImGui::SetCursorPos(ImVec2(static_cast<float>(B.items[lay::kBarCpu].x), static_cast<float>(B.items[lay::kBarCpu].y)));
    ImGui::Dummy(ImVec2(static_cast<float>(B.items[lay::kBarCpu].w), static_cast<float>(B.items[lay::kBarCpu].h)));
    char tip[160];
    std::snprintf(tip, sizeof tip, "Processing load %.1f%% (peak %.0f%%), latency %d samples (compensated)", 100.0 * avg, 100.0 * pk, hi->totalLatency());
    ui::Hint(tip);
  }

  // Settings panel.
  R(lay::kBarSettings, p0, p1);
  if (barButton("##settings", B.items[lay::kBarSettings], 0)) w.setPanel(w.panelPage >= 0 ? -1 : 0);
  ui::Hint(w.panelPage >= 0 ? "Fold the settings away" : "Settings: processing, MIDI, sidechain, outputs, parameters");
  boxAt(dl, p0, p1, w.panelPage >= 0 ? K.btnOn : ImGui::IsItemHovered() ? K.btnHov : K.btn, K.edge);
  drawGear(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), 6.5f, K.text);
}

void drawPanel(PluginWindow& w) {
  const BarLook K;
  App& a = *w.app;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetWindowPos();
  const float top = static_cast<float>(w.L.bar.h);
  dl->AddRectFilled(ImVec2(o.x, o.y + top), ImVec2(o.x + static_cast<float>(w.L.w), o.y + top + static_cast<float>(w.L.panel.h)), K.panel);
  ImGui::SetCursorPos(ImVec2(8, top + 6));
  static const char* const kTabs[] = {"Processing", "Settings", "Parameters"};
  static const char* const kHints[] = {"Buffers, rendering mode, sandbox, latency", "MIDI ports, sidechain input, extra outputs, plugin facts",
                                       "Every parameter: automate, link to a controller, type a value"};
  int page = w.panelPage;
  ui::TabStrip("##ptabs", kTabs, 3, &page, kHints);
  if (page != w.panelPage) w.panelPage = page;
  if (w.target.isChannel()) {
    ImGui::SameLine(0, 12);
    if (ui::TextButton("Channel\xE2\x80\xA6", false, 0, "Gridline's channel window: tuning, polyphony, note effects")) w.action = Action::ChannelPanel;
  }
  const float y = ImGui::GetCursorPosY() + 2;
  ImGui::SetCursorPos(ImVec2(8, y));
  const float width = static_cast<float>(w.L.w) - 16;
  ImGui::BeginChild("##page", ImVec2(width, top + static_cast<float>(w.L.panel.h) - y - 6), ImGuiChildFlags_None);
  hostedui::drawPage(a, w.target, w.panelPage + 1, width - 12);  // tabs: processing, settings, parameters (compact)
  ImGui::EndChild();
}

void renderWindow(PluginWindow& w) {
  if (w.dead || !w.win || !w.ctx) return;
  if (!w.shown) return;
  host::HostedInstance* hi = hostedInstanceFor(*w.app, w.target);
  makeCurrent(w.win, w.gl);
  ImGui::SetCurrentContext(w.ctx);
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();
  const float barTop = static_cast<float>(w.L.bar.y);
  ImGui::SetNextWindowPos(ImVec2(0, barTop));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w.L.w), static_cast<float>(w.L.bar.h + w.L.panel.h)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##pluginbar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
  ImGui::PopStyleVar(3);
  if (hi && !hi->crashed()) {
    drawBar(w, hi);
    if (w.panelPage >= 0) drawPanel(w);
  }
  ImGui::End();
  ImGui::Render();
  int fbw = 0, fbh = 0;
  SDL_GL_GetDrawableSize(w.win, &fbw, &fbh);
  glViewport(0, 0, fbw, fbh);
  ImVec4 bg = theme::vec(BarLook().panel);
  glClearColor(bg.x, bg.y, bg.z, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  if (!w.capturePath.empty()) {  // dev check: this window only, GL bar laid over its views
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w.win, &ww, &wh);
    const int rows = wh > 0 ? static_cast<int>(std::lround(static_cast<double>(w.L.bar.h + w.L.panel.h) * fbh / wh)) : 0;
    std::vector<unsigned char> px(static_cast<size_t>(fbw) * static_cast<size_t>(std::max(rows, 1)) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbw, rows, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (!native::captureWindow(w.nw, w.capturePath, px.data(), fbw, rows)) std::fprintf(stderr, "plugin window capture failed\n");
    w.capturePath.clear();
  }
  SDL_GL_SwapWindow(w.win);
}

void runAction(PluginWindow& w) {
  const Action act = w.action;
  w.action = Action::None;
  if (act == Action::None || w.dead) return;
  // Copies: the actions below close (destroy) this window.
  App& a = *w.app;
  const PluginTarget t = w.target;
  const std::string type = w.type;
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  switch (act) {
    case Action::Generic:
      hostedui::setGenericEditor(a, type, true);
      break;
    case Action::Sandbox:
      if (hi) setHostedSandbox(a, type, !hi->sandboxed());  // reopens on the new instance
      break;
    case Action::ChannelPanel:
      showHostedPanel(a, t);
      break;
    case Action::Close:
      closeHostedEditor(a, t);
      break;
    case Action::None:
      break;
  }
}

}  // namespace

// ---------------------------------------------------------------- public

void setPluginWindowLog(bool on) { gLog = on; }
void setPluginWindowPanel(int page) { gStartPanel = std::clamp(page, -1, 2); }
void setPluginWindowDevAction(const std::string& action) { gDevAction = action; }
void setPluginWindowDevResize(int w, int h) {
  gDevResizeW = w;
  gDevResizeH = h;
}

bool hostedEditorOpen(const PluginTarget& t) { return windowFor(t) != nullptr; }

void closeHostedEditor(App& a, const PluginTarget& t) {
  auto it = windows().find(t.key());
  if (it == windows().end()) return;
  it->second->close();
  windows().erase(it);
  (void)a;
}

void closeAllHostedEditors(App& a) {
  for (auto& [k, w] : windows()) w->close();
  windows().clear();
  (void)a;
}

bool openHostedEditor(App& a, const PluginTarget& t) {
  if (PluginWindow* ex = windowFor(t)) {
    if (ex->win) SDL_RaiseWindow(ex->win);
    return true;
  }
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi || hi->crashed()) {
    a.notify(hi ? "This plugin crashed: reload it first" : "This plugin is not loaded", true);
    return false;
  }
  host::Backend* b = hi->backend();
  const host::HostedClass& cls = hi->hostedClass();
  if (!b->hasEditor()) {
    a.notify(cls.name + " has no interface of its own: Gridline shows its parameters");
    return false;
  }
  std::unique_ptr<host::Editor> editor = b->createEditor();
  if (!editor) {
    a.notify(cls.name + ": its interface cannot be shown on this system", true);
    return false;
  }
  if (gUiThread == std::thread::id()) gUiThread = std::this_thread::get_id();
  auto pw = std::make_unique<PluginWindow>();
  PluginWindow& w = *pw;
  w.app = &a;
  w.target = t;
  w.type = cls.type;
  w.name = cls.name;
  w.format = b->format();
  w.backend = b;
  w.external = editor->external();
  const auto place = places().find(t.key());
  const lay::Rect work = workArea();
  const std::string title = cls.name + "  \xC2\xB7  " + hostedTargetName(a, t);
  Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;

  if (w.external) {  // sandboxed: the interface opens in the sandbox process' window
    if (place != places().end()) {
      editor->placeX = place->second.first;
      editor->placeY = place->second.second;
    }
    if (!editor->attach(nullptr)) {
      a.notify(cls.name + ": the sandboxed interface could not be opened", true);
      return false;
    }
    w.computeLayout();
    w.win = SDL_CreateWindow(title.c_str(), work.x, work.y, w.L.w, w.L.h, flags | SDL_WINDOW_BORDERLESS);
  } else {
    int ew = 640, eh = 420;
    editor->size(ew, eh);
    w.askedW = ew, w.askedH = eh;
    w.pw = std::max(ew, lay::kMinPluginW);
    w.ph = std::max(eh, lay::kMinPluginH);
    w.resizable = editor->canResize();
    w.panelPage = gStartPanel;
    w.computeLayout();
    const lay::Rect r = lay::placeWindow(w.L.w, w.L.h, work, place != places().end(), place != places().end() ? place->second.first : 0,
                                         place != places().end() ? place->second.second : 0, static_cast<int>(windows().size()));
    w.win = SDL_CreateWindow(title.c_str(), r.x, r.y, w.L.w, w.L.h, flags | (w.resizable ? SDL_WINDOW_RESIZABLE : 0));
  }
  if (!w.win) {
    a.notify(std::string("Cannot open a window: ") + SDL_GetError(), true);
    return false;
  }
  w.nw = nativeOf(w.win);
  if (!createSurface(w)) {
    a.notify(std::string("Cannot draw the plugin window: ") + SDL_GetError(), true);
    w.editor = std::move(editor);
    w.close();
    return false;
  }
  PluginWindow* raw = &w;
  if (!w.external) {
    w.container = native::createContainer(w.nw, w.L.plugin);
    editor->setContentScale(native::backingScale(w.nw));
    editor->requestResize = [raw](int nw, int nh) {
      if (raw->dead) return false;
      if (std::this_thread::get_id() != gUiThread) {  // applied on the next idle
        raw->pendingW.store(nw);
        raw->pendingH.store(nh);
        return true;
      }
      return raw->pluginResize(nw, nh);
    };
    if (!w.container || !editor->attach(w.container)) {
      w.editor = std::move(editor);
      w.close();
      a.notify(cls.name + ": its interface could not be attached", true);
      return false;
    }
    // Some editors (VST2) know their final size only once open.
    int w2 = w.pw, h2 = w.ph;
    if (editor->size(w2, h2) && (w2 != w.pw || h2 != w.ph)) {
      w.pw = w2, w.ph = h2;
      w.apply();
    }
    // A resizable interface reopens at the size the user left it at.
    if (auto sz = sizes().find(t.key()); w.resizable && sz != sizes().end() && (sz->second.first != w.pw || sz->second.second != w.ph)) {
      int rw = sz->second.first, rh = sz->second.second;
      editor->constrain(rw, rh);
      w.pw = std::max(rw, lay::kMinPluginW), w.ph = std::max(rh, lay::kMinPluginH);
      w.apply();
      editor->setSize(w.pw, w.ph);
    }
    SDL_SetWindowMinimumSize(w.win, lay::kMinWindowW, w.L.bar.h + lay::kMinPluginH);
    SDL_ShowWindow(w.win);
    w.shown = true;
    if (gMainWin && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN)) native::keepAbove(w.nw, nativeOf(gMainWin), true);
    SDL_RaiseWindow(w.win);
  }
  w.editor = std::move(editor);
  b->onGoingAway = [raw]() { raw->close(); };
  w.log("opened");
  windows()[t.key()] = std::move(pw);
  return true;
}

bool hostedEditorEvent(App& a, const SDL_Event& ev) {
  Uint32 id = 0;
  switch (ev.type) {
    case SDL_WINDOWEVENT: id = ev.window.windowID; break;
    case SDL_MOUSEMOTION: id = ev.motion.windowID; break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: id = ev.button.windowID; break;
    case SDL_MOUSEWHEEL: id = ev.wheel.windowID; break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: id = ev.key.windowID; break;
    case SDL_TEXTINPUT: id = ev.text.windowID; break;
    case SDL_TEXTEDITING: id = ev.edit.windowID; break;
    default: return false;
  }
  for (auto& [k, w] : windows()) {
    if (w->dead || !w->win || SDL_GetWindowID(w->win) != id) continue;
    if (w->ctx) {
      ImGuiContext* prev = ImGui::GetCurrentContext();
      ImGui::SetCurrentContext(w->ctx);
      ImGui_ImplSDL2_ProcessEvent(&ev);
      ImGui::SetCurrentContext(prev);
    }
    if (ev.type == SDL_WINDOWEVENT) {
      if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
        w->action = Action::Close;
        runAction(*w);
      } else if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !w->applying) {
        w->userResize(ev.window.data1, ev.window.data2);
      }
    }
    return true;
  }
  (void)a;
  return false;
}

void renderHostedEditors(App& a) {
  if (windows().empty()) return;
  {
    ContextGuard g;
    for (auto& [k, w] : windows()) renderWindow(*w);
  }
  for (auto& [k, w] : windows())
    if (w->action != Action::None) {
      runAction(*w);
      break;  // the map may have changed
    }
  (void)a;
}

bool screenshotHostedEditor(const PluginTarget& t, const std::string& pngPath) {
  PluginWindow* w = windowFor(t);
  if (!w) return false;
  w->capturePath = pngPath;  // taken when the window draws next (this frame)
  return true;
}

void hostedui::reopenLater(const PluginTarget& t) { reopen()[t.key()] = 0; }

void hostedui::pluginWindowsIdle(App& a) {
  if (!gMainWin) {
    gMainWin = SDL_GL_GetCurrentWindow();
    gMainGl = SDL_GL_GetCurrentContext();
  }
  // Plugin interfaces may have bound their own GL context during the event
  // loop: the main window draws next.
  if (!windows().empty()) makeCurrent(gMainWin, gMainGl);
  if (gUiThread == std::thread::id()) gUiThread = std::this_thread::get_id();
  for (auto it = windows().begin(); it != windows().end();) {
    PluginWindow& w = *it->second;
    if (!w.dead && w.editor) {
      if (++w.frames == 5 && gDevResizeW > 0 && !gDevResized && w.win) {  // dev: as if the user dragged the window corner
        if (gLog) std::fprintf(stderr, "[plugin-window] user drags window to %dx%d\n", gDevResizeW, gDevResizeH);
        SDL_SetWindowSize(w.win, gDevResizeW, gDevResizeH);
        gDevResized = true;
      }
      if (w.frames == 8 && !gDevAction.empty()) {  // dev: as if the user pressed a bar button
        if (gLog) std::fprintf(stderr, "[plugin-window] bar button: %s\n", gDevAction.c_str());
        if (gDevAction == "generic") w.action = Action::Generic;
        if (gDevAction == "sandbox") w.action = Action::Sandbox;
        if (gDevAction == "settings") w.setPanel(0);
        gDevAction.clear();
      }
      const int pwq = w.pendingW.exchange(0), phq = w.pendingH.exchange(0);
      if (pwq > 0 && phq > 0) w.pluginResize(pwq, phq);
      if (w.notifySize) {
        w.notifySize = false;
        w.editor->setSize(w.pw, w.ph);
      }
      w.editor->idle();
      if (w.external) {
        if (!w.editor->isOpen()) {
          w.close();
        } else {
          int f[4] = {0, 0, 0, 0};
          if (w.editor->windowFrame(f[0], f[1], f[2], f[3]) && f[2] > 0) {
            const bool moved = f[0] != w.extFrame[0] || f[1] != w.extFrame[1] || f[2] != w.extFrame[2] || f[3] != w.extFrame[3];
            if (moved || !w.shown) {
              std::copy(f, f + 4, w.extFrame);
              w.computeLayout();
              ++w.applying;
              SDL_SetWindowSize(w.win, w.L.w, w.L.h);
              SDL_SetWindowPosition(w.win, f[0], f[1] + f[3]);  // docked under the sandbox window
              --w.applying;
              if (!w.shown) {
                SDL_ShowWindow(w.win);
                w.shown = true;
                if (gMainWin && !(SDL_GetWindowFlags(gMainWin) & SDL_WINDOW_HIDDEN)) native::keepAbove(w.nw, nativeOf(gMainWin), true);
                w.log("sandbox window docked");
              }
            }
          }
        }
      }
    }
    it = w.dead ? windows().erase(it) : std::next(it);
  }
  // Windows to show again on new instances (sandbox switched, options changed).
  for (auto it = reopen().begin(); it != reopen().end();) {
    if (it->second == 0) it->second = a.now + 5.0;  // give up after a few seconds
    bool done = a.now > it->second;
    // Find the target by key among the song's hosted plugins.
    const Song& s = a.doc.song();
    PluginTarget found{kNoId, -1};
    for (const auto& c : s.channels)
      if (PluginTarget{c.id, -1}.key() == it->first) found = {c.id, -1};
    for (const auto& m : s.mixer)
      for (int k = 0; k < kEffectSlots && k < static_cast<int>(m.slots.size()); ++k)
        if (PluginTarget{m.id, k}.key() == it->first) found = {m.id, k};
    if (found.owner == kNoId) done = true;
    if (!done) {
      host::HostedInstance* hi = hostedInstanceFor(a, found);
      if (hi && !hi->crashed()) {
        if (hi->backend()->hasEditor() && !host::hostOptions(hi->hostedClass().type).genericEditor) openHostedEditor(a, found);
        done = true;
      }
    }
    it = done ? reopen().erase(it) : std::next(it);
  }
  routePluginWindows(a);
}

// ---------------------------------------------------------------- routing

namespace {
bool panelShowing(App& a, const PluginTarget& t) {
  return t.isChannel() ? a.settingsChannel == t.owner : a.fxEditor == std::make_pair(t.owner, t.slot);
}
void hidePanel(App& a, const PluginTarget& t) {
  if (t.isChannel()) {
    if (a.settingsChannel == t.owner) a.settingsChannel = kNoId;
  } else if (a.fxEditor == std::make_pair(t.owner, t.slot)) {
    a.fxEditor = {kNoId, -1};
  }
}
// The window to show for an installed plugin: its own interface unless it
// has none, the generic editor is chosen, or it is not running.
bool wantsNative(App& a, const PluginTarget& t) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || !isHostedType(st->type)) return false;
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi || hi->crashed()) return false;  // Gridline's panel explains (missing / crashed)
  return lay::chooseEditorMode(hi->backend()->hasEditor(), host::hostOptions(hi->hostedClass().type).genericEditor) == lay::EditorMode::Native;
}
}  // namespace

void routePluginWindows(App& a) {
  // Explicit panel requests end with the panel.
  for (auto it = panelRequested().begin(); it != panelRequested().end();) {
    const bool still = (a.settingsChannel != kNoId && PluginTarget{a.settingsChannel, -1}.key() == *it) ||
                       (a.fxEditor.first != kNoId && PluginTarget{a.fxEditor.first, a.fxEditor.second}.key() == *it);
    it = still ? std::next(it) : panelRequested().erase(it);
  }
  auto route = [&a](const PluginTarget& t) {
    if (panelRequested().count(t.key()) || !wantsNative(a, t)) return;
    if (openHostedEditor(a, t))
      hidePanel(a, t);
    else
      panelRequested().insert(t.key());  // could not open: Gridline's panel stays
  };
  if (a.settingsChannel != kNoId) route(PluginTarget{a.settingsChannel, -1});
  if (a.fxEditor.first != kNoId) route(PluginTarget{a.fxEditor.first, a.fxEditor.second});
}

bool pluginWindowShown(App& a, const PluginTarget& t) { return hostedEditorOpen(t) || panelShowing(a, t); }

void togglePluginWindow(App& a, const PluginTarget& t) {
  if (hostedEditorOpen(t)) {
    closeHostedEditor(a, t);
    return;
  }
  if (panelShowing(a, t)) {
    hidePanel(a, t);
    return;
  }
  if (t.isChannel())
    a.settingsChannel = t.owner;
  else
    a.fxEditor = {t.owner, t.slot};
  routePluginWindows(a);
}

void showHostedPanel(App& a, const PluginTarget& t) {
  panelRequested().insert(t.key());
  if (t.isChannel())
    a.settingsChannel = t.owner;
  else
    a.fxEditor = {t.owner, t.slot};
}

}  // namespace gl
