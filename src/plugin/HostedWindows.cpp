// Gridline — plugin build (PH-09): windows of installed plugins hosted by the
// Gridline plugin. Replaces app/PluginWindow.cpp (SDL windows) inside another
// application: an installed plugin opens as itself, its own interface in a
// HostWindow (PluginView.h) owned by the host's window that shows Gridline,
// with Gridline's control bar and settings panel under it (J-29 layout, the
// same bar as the application: app/PluginWindowBar.cpp). The windows show and
// hide with Gridline's editor and close with it.
//
// Dev checks (environment):
//   GRIDLINE_PLUGIN_OPEN_HOSTED=1     opens every hosted plugin's interface once
//                                     Gridline's editor has drawn a few frames
//   GRIDLINE_PLUGIN_WINDOW_LOG=<file> appends one line per window event
//   GRIDLINE_PLUGIN_WINDOW_SHOT=<dir> saves each window's image (<format>.png)
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../app/App.h"
#include "../app/HostedUi.h"
#include "../app/HostedUiPages.h"
#include "../app/PluginWindowBar.h"
#include "../host/HostedPlugins.h"
#include "PluginCore.h"
#include "PluginView.h"

namespace gl {

namespace lay = host::layout;
using pluginbar::Action;

namespace {

bool gLog = false;
int gStartPanel = -1;

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void devLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void devLog(const char* fmt, ...) {
  const char* path = std::getenv("GRIDLINE_PLUGIN_WINDOW_LOG");
  if (!path || !*path) return;
  if (FILE* f = std::fopen(path, "a")) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
  }
}

using Key = std::pair<const App*, uint64_t>;

std::map<Key, std::pair<int, int>>& places() {  // remembered window positions per target
  static std::map<Key, std::pair<int, int>> m;
  return m;
}
std::map<Key, std::pair<int, int>>& sizes() {  // last size of resizable interfaces per target
  static std::map<Key, std::pair<int, int>> m;
  return m;
}

struct NestedWindow;

// The control bar's Dear ImGui context: shares the font atlas and style of
// the Gridline editor's frame.
class BarSurface final : public plugin::Surface {
 public:
  explicit BarSurface(NestedWindow& w) : w_(w) {}
  ~BarSurface() override { shutdown(); }
  bool init(const char* glslVersion) override;
  void shutdown() override;
  void makeCurrent() override;
  void draw(int winW, int winH, int fbW, int fbH) override;
  int cursor() const override;

 private:
  NestedWindow& w_;
  ImGuiContext* ctx_ = nullptr;
  bool ready_ = false;
  double last_ = 0;
};

struct NestedWindow {
  plugin::Instance* inst = nullptr;
  App* app = nullptr;
  PluginTarget target;
  std::string type, name, format;
  host::Backend* backend = nullptr;
  std::unique_ptr<host::Editor> editor;
  std::unique_ptr<plugin::HostWindow> win;
  BarSurface surface{*this};
  bool external = false;  // sandboxed: the interface is in the sandbox process' window
  bool resizable = false;
  bool dead = false;
  bool shown = false;
  bool hidden = false;  // Gridline's editor is hidden
  int pw = 0, ph = 0;
  int panelPage = -1;
  lay::WindowLayout L;
  Action action = Action::None;
  std::atomic<int> pendingW{0}, pendingH{0};  // resize asked for off the main thread
  bool notifySize = false;
  int extFrame[4] = {0, 0, 0, 0};
  bool reported = false;
  bool shot = false;

  int panelH() const { return panelPage >= 0 ? lay::kPanelH : 0; }
  void computeLayout() { L = external ? lay::layoutBarWindow(extFrame[2] > 0 ? extFrame[2] : 420, panelH()) : lay::layoutWindow(pw, ph, panelH()); }
  void apply() {
    computeLayout();
    if (win) win->setLayout(L);
  }
  bool pluginResize(int w, int h) {
    if (dead) return false;
    pw = std::max(w, lay::kMinPluginW);
    ph = std::max(h, lay::kMinPluginH);
    apply();
    notifySize = true;
    return true;
  }
  void userResize(int w, int h) {
    if (external || !editor || (w == L.w && h == L.h)) return;
    if (!resizable) {
      apply();
      return;
    }
    int nw = 0, nh = 0;
    lay::pluginSizeForWindow(w, h, panelH(), nw, nh);
    editor->constrain(nw, nh);
    pw = std::max(nw, lay::kMinPluginW);
    ph = std::max(nh, lay::kMinPluginH);
    apply();
    editor->setSize(pw, ph);
  }
  void setPanel(int page) {
    if (page == panelPage) return;
    panelPage = page;
    apply();
  }
  void close() {
    if (dead) return;
    dead = true;
    const plugin::GlBinding gl = plugin::currentGl();
    ImGuiContext* imgui = ImGui::GetCurrentContext();
    const Key k{app, target.key()};
    int x = 0, y = 0;
    if (win && win->topLeft(x, y) && !external) places()[k] = {x, y};
    if (external && extFrame[2] > 0) places()[k] = {extFrame[0], extFrame[1]};
    if (!external && resizable) sizes()[k] = {pw, ph};
    devLog("close format=%s name=%s frames=%d", format.c_str(), name.c_str(), win ? win->barFrames() : 0);
    editor.reset();  // detaches the plugin's view before its window goes
    win.reset();     // the bar's surface shuts down with its view
    surface.shutdown();
    if (backend) backend->onGoingAway = nullptr;
    backend = nullptr;
    plugin::restoreGl(gl);
    ImGui::SetCurrentContext(imgui);
  }
  ~NestedWindow() { close(); }
};

std::map<Key, std::unique_ptr<NestedWindow>>& windows() {
  static std::map<Key, std::unique_ptr<NestedWindow>> m;
  return m;
}

NestedWindow* windowFor(const App* a, const PluginTarget& t) {
  auto it = windows().find(Key{a, t.key()});
  return it != windows().end() && !it->second->dead ? it->second.get() : nullptr;
}

bool BarSurface::init(const char* glslVersion) {
  plugin::View* v = w_.inst ? w_.inst->editorView() : nullptr;
  plugin::Frame* f = v ? v->frame() : nullptr;
  auto* frameCtx = f ? static_cast<ImGuiContext*>(f->imguiContext()) : nullptr;
  if (!frameCtx) return false;  // Gridline's editor has not drawn yet: fonts come from it
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(frameCtx);
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  ImFont* defFont = ImGui::GetIO().FontDefault;
  const ImGuiStyle style = ImGui::GetStyle();
  ctx_ = ImGui::CreateContext(atlas);
  ImGui::SetCurrentContext(ctx_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigDragClickToInputText = true;
  io.BackendPlatformName = "gridline_plugin_bar";
  io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
  io.FontDefault = defFont;
  ImGui::GetStyle() = style;
  ready_ = ImGui_ImplOpenGL3_Init(glslVersion);
  ImGui::SetCurrentContext(prev);
  if (!ready_) shutdown();
  return ready_;
}

void BarSurface::shutdown() {
  if (!ctx_) return;
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(ctx_);
  if (ready_) ImGui_ImplOpenGL3_Shutdown();
  ready_ = false;
  ImGui::DestroyContext(ctx_);
  ImGui::SetCurrentContext(prev != ctx_ ? prev : nullptr);
  ctx_ = nullptr;
}

void BarSurface::makeCurrent() {
  if (ctx_) ImGui::SetCurrentContext(ctx_);
  if (w_.inst) w_.inst->makeCurrent();
}

int BarSurface::cursor() const {
  if (!ctx_) return ImGuiMouseCursor_Arrow;
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(ctx_);
  const int c = ImGui::GetMouseCursor();
  ImGui::SetCurrentContext(prev);
  return c;
}

void BarSurface::draw(int winW, int winH, int fbW, int fbH) {
  NestedWindow& w = w_;
  if (!ctx_ || !ready_ || w.dead || winW <= 0 || winH <= 0) return;
  ImGuiContext* prev = ImGui::GetCurrentContext();
  makeCurrent();
  ImGuiIO& io = ImGui::GetIO();
  const double now = nowSeconds();
  io.DeltaTime = last_ > 0 && now > last_ ? static_cast<float>(std::min(0.25, now - last_)) : 1.0f / 60.0f;
  last_ = now;
  io.DisplaySize = ImVec2(static_cast<float>(winW), static_cast<float>(winH));
  io.DisplayFramebufferScale = ImVec2(static_cast<float>(fbW) / static_cast<float>(winW), static_cast<float>(fbH) / static_cast<float>(winH));
  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  pluginbar::Bar bar;
  bar.app = w.app;
  bar.target = w.target;
  bar.type = w.type;
  bar.L = w.L;
  bar.panelPage = w.panelPage;
  pluginbar::draw(bar, hostedInstanceFor(*w.app, w.target), 0);  // the GL view starts at the bar
  ImGui::Render();
  glViewport(0, 0, fbW, fbH);
  float bg[4];
  pluginbar::background(bg);
  glClearColor(bg[0], bg[1], bg[2], bg[3]);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  ImGui::SetCurrentContext(prev);
  // Applied after drawing: actions run on the next idle (they may close this window).
  w.panelPage = bar.panelPage;
  if (bar.action != Action::None) w.action = bar.action;
  if (bar.wantPanel != pluginbar::kNoPanelChange) w.setPanel(bar.wantPanel);
}

// Every installed plugin of the song (dev check: open them all).
std::vector<PluginTarget> hostedTargets(App& a) {
  std::vector<PluginTarget> out;
  const Song& s = a.doc.song();
  for (const auto& c : s.channels)
    if (isHostedType(c.plugin.type)) out.push_back(PluginTarget{c.id, -1});
  for (const auto& m : s.mixer)
    for (int k = 0; k < kEffectSlots && k < static_cast<int>(m.slots.size()); ++k)
      if (isHostedType(m.slots[static_cast<size_t>(k)].plugin.type))out.push_back(PluginTarget{m.id, k});
  return out;
}

void runAction(NestedWindow& w) {
  const Action act = w.action;
  w.action = Action::None;
  if (act == Action::None || w.dead) return;
  App& a = *w.app;
  const PluginTarget t = w.target;
  const std::string type = w.type;
  pluginbar::runAction(a, t, type, act);
}

}  // namespace

// ---------------------------------------------------------------- public

void setPluginWindowLog(bool on) { gLog = on; }
void setPluginWindowPanel(int page) { gStartPanel = std::clamp(page, -1, 2); }
void setPluginWindowDevAction(const std::string&) {}
void setPluginWindowDevResize(int, int) {}

bool hostedEditorOpen(const PluginTarget& t) { return windowFor(&app(), t) != nullptr; }

void closeHostedEditor(App& a, const PluginTarget& t) {
  auto it = windows().find(Key{&a, t.key()});
  if (it == windows().end()) return;
  std::unique_ptr<NestedWindow> w = std::move(it->second);
  windows().erase(it);
  w->close();
}

void closeAllHostedEditors(App& a) {
  for (auto it = windows().begin(); it != windows().end();) {
    if (it->first.first != &a) {
      ++it;
      continue;
    }
    std::unique_ptr<NestedWindow> w = std::move(it->second);
    it = windows().erase(it);
    w->close();
  }
}

bool openHostedEditor(App& a, const PluginTarget& t) {
  if (NestedWindow* ex = windowFor(&a, t)) {
    if (ex->win) ex->win->raise();
    return true;
  }
  plugin::Instance* inst = plugin::Instance::of(&a);
  plugin::View* owner = inst ? inst->editorView() : nullptr;
  host::HostedInstance* hi = hostedInstanceFor(a, t);
  if (!hi || hi->crashed()) {
    a.notify(hi ? "This plugin crashed: reload it first" : "This plugin is not loaded", true);
    return false;
  }
  if (!owner) return false;  // Gridline's editor is closed: nothing to own the window
  host::Backend* b = hi->backend();
  const host::HostedClass& cls = hi->hostedClass();
  if (!b->hasEditor()) {
    a.notify(std::string("No interface of its own, Gridline shows its parameters: ") + cls.name);
    return false;
  }
  // Plugin interfaces may bind their own GL context or ImGui may be mid-frame:
  // both are restored afterwards.
  const plugin::GlBinding gl = plugin::currentGl();
  ImGuiContext* imgui = ImGui::GetCurrentContext();
  struct Restore {
    const plugin::GlBinding& gl;
    ImGuiContext* imgui;
    ~Restore() {
      plugin::restoreGl(gl);
      ImGui::SetCurrentContext(imgui);
    }
  } restore{gl, imgui};

  std::unique_ptr<host::Editor> editor = b->createEditor();
  if (!editor) {
    a.notify(std::string("Its interface cannot be shown on this system: ") + cls.name, true);
    return false;
  }
  auto nwp = std::make_unique<NestedWindow>();
  NestedWindow& w = *nwp;
  w.inst = inst;
  w.app = &a;
  w.target = t;
  w.type = cls.type;
  w.name = cls.name;
  w.format = b->format();
  w.backend = b;
  w.external = editor->external();
  w.panelPage = gStartPanel;
  const Key key{&a, t.key()};
  const auto place = places().find(key);
  int placeXY[2] = {0, 0};
  if (place != places().end()) placeXY[0] = place->second.first, placeXY[1] = place->second.second;
  const std::string title = cls.name + "  \xC2\xB7  " + hostedTargetName(a, t);
  int index = 0;
  for (const auto& [k, other] : windows())
    if (k.first == &a) ++index;

  if (w.external) {  // sandboxed: the interface opens in the sandbox process' window
    if (place != places().end()) {
      editor->placeX = placeXY[0];
      editor->placeY = placeXY[1];
    }
    if (!editor->attach(nullptr)) {
      a.notify(std::string("The sandboxed interface could not be opened: ") + cls.name, true);
      return false;
    }
    w.computeLayout();
    w.win = plugin::createHostWindow(*owner, title, w.L, false, true, w.surface, nullptr, index);
  } else {
    int ew = 640, eh = 420;
    editor->size(ew, eh);
    w.pw = std::max(ew, lay::kMinPluginW);
    w.ph = std::max(eh, lay::kMinPluginH);
    w.resizable = editor->canResize();
    w.computeLayout();
    w.win = plugin::createHostWindow(*owner, title, w.L, w.resizable, false, w.surface, place != places().end() ? placeXY : nullptr, index);
  }
  if (!w.win) {
    a.notify(std::string("No window for its interface on this system: ") + cls.name, true);
    w.editor = std::move(editor);
    w.close();
    return false;
  }
  NestedWindow* raw = &w;
  w.win->onClose = [raw]() { raw->action = Action::Close; };
  w.win->onUserResize = [raw](int nw, int nh) { raw->userResize(nw, nh); };
  if (!w.external) {
    editor->setContentScale(w.win->backingScale());
    editor->requestResize = [raw](int nw, int nh) {
      if (raw->dead) return false;
      raw->pendingW.store(nw);  // applied on the next idle (any thread may ask)
      raw->pendingH.store(nh);
      return true;
    };
    if (!editor->attach(w.win->container())) {
      w.editor = std::move(editor);
      w.close();
      a.notify(std::string("Its interface could not be attached: ") + cls.name, true);
      return false;
    }
    // Some editors (VST2) know their final size only once open.
    int w2 = w.pw, h2 = w.ph;
    if (editor->size(w2, h2) && (w2 != w.pw || h2 != w.ph)) {
      w.pw = w2, w.ph = h2;
      w.apply();
    }
    // A resizable interface reopens at the size the user left it at.
    if (auto sz = sizes().find(key); w.resizable && sz != sizes().end() && (sz->second.first != w.pw || sz->second.second != w.ph)) {
      int rw = sz->second.first, rh = sz->second.second;
      editor->constrain(rw, rh);
      w.pw = std::max(rw, lay::kMinPluginW), w.ph = std::max(rh, lay::kMinPluginH);
      w.apply();
      editor->setSize(w.pw, w.ph);
    }
    w.win->show();
    w.shown = true;
  }
  w.editor = std::move(editor);
  b->onGoingAway = [raw]() { raw->action = Action::Close; };
  devLog("open format=%s name=%s plugin=%dx%d window=%dx%d bar=%d resizable=%d external=%d child=%d", w.format.c_str(), w.name.c_str(), w.pw, w.ph,
         w.L.w, w.L.h, w.L.bar.h, w.resizable ? 1 : 0, w.external ? 1 : 0, w.win->ownedByEditor() ? 1 : 0);
  if (gLog) std::fprintf(stderr, "[plugin-window] opened | %s (%s)\n", w.name.c_str(), w.format.c_str());
  windows()[key] = std::move(nwp);
  return true;
}

bool hostedEditorEvent(App&, const SDL_Event&) { return false; }
void renderHostedEditors(App&) {}

bool screenshotHostedEditor(const PluginTarget& t, const std::string& pngPath) {
  NestedWindow* w = windowFor(&app(), t);
  return w && w->win && w->win->capture(pngPath);
}

void hostedui::pluginWindowsIdle(App& a) {
  plugin::Instance* inst = plugin::Instance::of(&a);
  // Dev check: every hosted plugin opens its own interface.
  const char* openAll = std::getenv("GRIDLINE_PLUGIN_OPEN_HOSTED");
  if (openAll && *openAll && inst && inst->editorView() && inst->editorView()->frames() >= 3) {
    static std::map<const App*, bool> done;
    if (!done[&a]) {
      done[&a] = true;
      for (const PluginTarget& t : hostedTargets(a)) openHostedEditor(a, t);
    }
  }
  const char* shotDir = std::getenv("GRIDLINE_PLUGIN_WINDOW_SHOT");
  for (auto it = windows().begin(); it != windows().end();) {
    if (it->first.first != &a) {
      ++it;
      continue;
    }
    NestedWindow& w = *it->second;
    if (!w.dead && w.editor) {
      const int pwq = w.pendingW.exchange(0), phq = w.pendingH.exchange(0);
      if (pwq > 0 && phq > 0) w.pluginResize(pwq, phq);
      if (w.notifySize) {
        w.notifySize = false;
        w.editor->setSize(w.pw, w.ph);
      }
      {
        const plugin::GlBinding gl = plugin::currentGl();
        w.editor->idle();
        plugin::restoreGl(gl);
      }
      if (w.external) {
        if (!w.editor->isOpen()) {
          w.action = Action::Close;
        } else {
          int f[4] = {0, 0, 0, 0};
          if (w.editor->windowFrame(f[0], f[1], f[2], f[3]) && f[2] > 0) {
            const bool moved = f[0] != w.extFrame[0] || f[1] != w.extFrame[1] || f[2] != w.extFrame[2] || f[3] != w.extFrame[3];
            if (moved || !w.shown) {
              std::copy(f, f + 4, w.extFrame);
              w.apply();
              w.win->moveTopLeft(f[0], f[1] + f[3]);  // docked under the sandbox window
              if (!w.shown) {
                w.win->show();
                w.shown = true;
              }
            }
          }
        }
      }
      if (w.win) w.win->tick();
      if (w.win && !w.reported && w.win->barFrames() >= 10) {
        w.reported = true;
        int vw = 0, vh = 0;
        w.win->pluginViewSize(vw, vh);
        devLog("drawn format=%s name=%s frames=%d visible=%d child=%d view=%dx%d window=%dx%d", w.format.c_str(), w.name.c_str(), w.win->barFrames(),
               w.win->visible() ? 1 : 0, w.win->ownedByEditor() ? 1 : 0, vw, vh, w.L.w, w.L.h);
      }
      if (shotDir && *shotDir && w.win && !w.shot && w.win->barFrames() >= 30) {
        w.shot = true;
        std::string fmt = w.format;
        for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string path = std::string(shotDir) + "/" + fmt + ".png";
        devLog("shot format=%s path=%s ok=%d", w.format.c_str(), path.c_str(), w.win->capture(path) ? 1 : 0);
      }
    }
    if (w.action != Action::None && !w.dead) {
      runAction(w);
      break;  // the map may have changed
    }
    if (w.dead) {
      it = windows().erase(it);
      continue;
    }
    ++it;
  }
  pluginbar::reopenPending(a);
  routePluginWindows(a);
}

namespace plugin {

void setHostedWindowsVisible(Instance& inst, bool on) {
  for (auto& [k, w] : windows())
    if (k.first == &inst.app() && w->win && !w->dead) {
      w->hidden = !on;
      if (w->shown) w->win->setVisible(on);
    }
}

void closeHostedWindows(Instance& inst) { closeAllHostedEditors(inst.app()); }

}  // namespace plugin

}  // namespace gl
