// Gridline — the plugin's editor frame (PH-09): Gridline's workspace (top
// bar, browser, channel rack, piano roll, playlist, mixer, plugin windows,
// dialogs, status bar) drawn into the host's plugin window, plus the "Host
// link" panel: transport sync, MIDI input, outputs, latency and the macros.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#if defined(_WIN32)
#include <windows.h>  // GL/gl.h needs its APIENTRY / WINGDIAPI
#endif
#include <GL/gl.h>
#endif
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_impl_opengl3.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../app/App.h"
#include "../app/AutomationEditor.h"
#include "../app/BrowserUi.h"
#include "../app/Shell.h"
#include "../app/Theme.h"
#include "../app/ThemeSettings.h"
#include "../app/Widgets.h"
#include "../app/Windows.h"
#include "../core/I18n.h"
#include "../packs/PacksUi.h"
#include "PluginCore.h"
#include "PluginView.h"

namespace fs = std::filesystem;

namespace gl::plugin {

namespace {

constexpr const char* kHostLinkId = "###HostLink";

ImGuiContext* imgui(void* p) { return static_cast<ImGuiContext*>(p); }

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Parameter context menus: "Link to host macro" (App::extraParamMenu).
void macroMenu(Instance& inst, ParamId pid) {
  if (!ImGui::BeginMenu(i18n::tr("Link to host macro"))) return;
  for (int k = 0; k < kMacros; ++k) {
    const ParamId cur = inst.macroLink(k);
    const bool here = cur == pid;
    std::string label = inst.macroName(k);
    if (cur.owner != kNoId && !here) label += "  (" + inst.macroTarget(k) + ")";
    if (ImGui::MenuItem(label.c_str(), nullptr, here)) inst.linkMacro(k, here ? ParamId{} : pid);
  }
  ImGui::EndMenu();
}

void drawHostLink(Instance& inst) {
  App& a = inst.app();
  const theme::Palette& P = theme::pal();
  const std::string title = std::string(i18n::tr("Host link")) + kHostLinkId;
  if (!ImGui::Begin(title.c_str())) {
    ImGui::End();
    return;
  }
  ImGui::TextColored(theme::vec(P.textFaint), "%s", inst.variant() == Variant::Effect ? i18n::tr("Gridline FX (effect)") : i18n::tr("Gridline (instrument)"));
  ImGui::Separator();
  // Transport.
  bool follow = inst.followHost();
  if (ImGui::Checkbox(i18n::tr("Follow the host transport"), &follow)) inst.setFollowHost(follow);
  if (inst.followingHost())
    ImGui::TextWrapped("%s", i18n::tr("Following the host: play, stop, position and tempo come from the host."));
  else
    ImGui::TextWrapped("%s", i18n::tr("Gridline's own transport plays the pattern or the song."));
  ImGui::Spacing();
  // MIDI input.
  ImGui::TextUnformatted(i18n::tr("MIDI input"));
  const int mode = static_cast<int>(inst.midiMode());
  if (ImGui::RadioButton(i18n::tr("Selected channel"), mode == 0)) inst.setMidiMode(MidiMode::Selected);
  if (ImGui::RadioButton(i18n::tr("By MIDI channel"), mode == 1)) inst.setMidiMode(MidiMode::PerChannel);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
  ImGui::TextWrapped("%s", mode == 1 ? i18n::tr("MIDI channel 1-16 plays rack channel 1-16.") : i18n::tr("Every MIDI channel plays the selected channel."));
  ImGui::PopStyleColor();
  ImGui::Spacing();
  // Outputs and latency.
  if (inst.variant() == Variant::Instrument)
    ImGui::TextWrapped("%s", i18n::tr("Outputs 2-16 carry mixer inserts 1-15 after their faders; the inserts still feed the master."));
  else
    ImGui::TextWrapped("%s", i18n::tr("The host's audio enters mixer insert 1; the sidechain arrives as inputs 3-4."));
  char lat[128];
  std::snprintf(lat, sizeof lat, i18n::tr("Latency reported to the host: %d samples"), inst.latency());
  ImGui::TextWrapped("%s", lat);
  ImGui::Separator();
  // Macros: name and buttons on one line, the linked parameter and its value under it.
  ImGui::TextUnformatted(i18n::tr("Host macros"));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
  ImGui::TextWrapped("%s", i18n::tr("Right-click a control and choose Link to host macro, or link the control you touched last."));
  ImGui::PopStyleColor();
  const ParamId last = a.doc.lastTouchedParam();
  const bool canLink = last.owner != kNoId && describeParam(a.doc.song(), last).has_value();
  const ImGuiStyle& st = ImGui::GetStyle();
  const float linkW = ImGui::CalcTextSize(i18n::tr("Link")).x + st.FramePadding.x * 2;
  const float unlinkW = ImGui::CalcTextSize(i18n::tr("Unlink")).x + st.FramePadding.x * 2;
  for (int k = 0; k < kMacros; ++k) {
    ImGui::PushID(k);
    ImGui::Spacing();
    const std::string target = inst.macroTarget(k);
    ImGui::TextUnformatted(inst.macroName(k).c_str());
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - linkW - (target.empty() ? 0.f : unlinkW + st.ItemSpacing.x)));
    ImGui::BeginDisabled(!canLink);
    if (ImGui::SmallButton(i18n::tr("Link"))) inst.linkMacro(k, last);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", i18n::tr("Link the control you touched last"));
    if (!target.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(i18n::tr("Unlink"))) inst.linkMacro(k, ParamId{});
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(target.empty() ? P.textFaint : P.textDim));
    if (target.empty())
      ImGui::TextWrapped("%s", i18n::tr("Not linked"));
    else
      ImGui::TextWrapped("%s: %s", target.c_str(), inst.macroText(k, inst.macroValue(k)).c_str());
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::End();
}

}  // namespace

Frame::Frame(Instance& inst) : inst_(inst) {}
Frame::~Frame() { shutdown(); }

void Frame::makeCurrent() {
  if (ctx_) ImGui::SetCurrentContext(imgui(ctx_));
  inst_.makeCurrent();
}

bool Frame::init(const char* glslVersion) {
  inst_.makeCurrent();
  App& app = inst_.app();
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ctx_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui(ctx_));
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  io.ConfigDragClickToInputText = true;
  io.BackendPlatformName = "gridline_plugin";
  io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
  const std::string& assets = app.assetsDir;
  auto font = [&](const char* file, float size) -> ImFont* {
    if (assets.empty()) return nullptr;
    const std::string p = (fs::path(assets) / "fonts" / file).string();
    std::error_code ec;
    if (!fs::exists(p, ec)) return nullptr;
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
  app.fontRegular = fRegular;
  app.fontBold = fBold ? fBold : fRegular;
  app.fontLarge = fLarge ? fLarge : app.fontBold;
  app.fontMono = fMono ? fMono : fRegular;
  app.fontMonoBold = fMonoBold ? fMonoBold : app.fontMono;
  io.FontDefault = fRegular;
  theme::apply(true);
  theme::applyFromSettings(app.settings);
  theme::setMarkTextureProvider(&shell::markTexture);
  std::error_code ec;
  fs::create_directories(app.configDir(), ec);
  iniPath_ = (fs::path(app.configDir()) / "plugin-layout.ini").string();
  io.IniFilename = iniPath_.c_str();
  rendererReady_ = ImGui_ImplOpenGL3_Init(glslVersion);
  app.extraParamMenu = [this](ParamId pid) { macroMenu(inst_, pid); };
  if (prev) ImGui::SetCurrentContext(prev);
  return rendererReady_;
}

void Frame::shutdown() {
  if (!ctx_) return;
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGuiContext* mine = imgui(ctx_);
  ImGui::SetCurrentContext(mine);
  if (rendererReady_) ImGui_ImplOpenGL3_Shutdown();
  rendererReady_ = false;
  ImGui::DestroyContext(mine);
  ctx_ = nullptr;
  inst_.app().extraParamMenu = nullptr;
  ImGui::SetCurrentContext(prev != mine ? prev : nullptr);
}

int Frame::cursor() const {
  if (!ctx_) return ImGuiMouseCursor_Arrow;
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(imgui(ctx_));
  const int c = ImGui::GetMouseCursor();
  ImGui::SetCurrentContext(prev);
  return c;
}

void Frame::draw(int winW, int winH, int fbW, int fbH) {
  if (!ctx_ || !rendererReady_ || winW <= 0 || winH <= 0) return;
  makeCurrent();
  App& app = inst_.app();
  ImGuiIO& io = ImGui::GetIO();
  const double now = nowSeconds();
  io.DeltaTime = last_ > 0 && now > last_ ? static_cast<float>(std::min(0.25, now - last_)) : 1.0f / 60.0f;
  last_ = now;
  app.engine.setMetronome(app.metronome);
  app.engine.setReturnToSeek(app.settings.getBool("transport.rememberSeek"));
  app.hint.clear();
  float uiScale = static_cast<float>(std::clamp(app.settings.getNumber("ui.scale"), 0.75, 2.0));
  if (uiScale > 1.0f)
    uiScale = std::max(1.0f, std::min({uiScale, static_cast<float>(winW) / 960.0f, static_cast<float>(winH) / 600.0f}));
  uiScale_ = uiScale;
  io.DisplaySize = ImVec2(static_cast<float>(winW) / uiScale, static_cast<float>(winH) / uiScale);
  io.DisplayFramebufferScale = ImVec2(static_cast<float>(fbW) / io.DisplaySize.x, static_cast<float>(fbH) / io.DisplaySize.y);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  const float topH = drawTopBar(app);
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 dpos(vp->WorkPos.x, vp->WorkPos.y + topH), dsize(vp->WorkSize.x, vp->WorkSize.y - topH - theme::kStatusBarH);
  ImGui::SetNextWindowPos(dpos);
  ImGui::SetNextWindowSize(dsize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::pal().surface950));
  ImGui::Begin("##dockhost", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);
  const ImGuiID dockId = ImGui::GetID("GridlinePluginDock");
  if (!layoutDone_ && !ImGui::DockBuilderGetNode(dockId)) {
    const ImVec2 inner(dsize.x - 8, dsize.y - 8);
    const workspace::Rect r = shell::buildDefaultLayout(dockId, inner, static_cast<int>(app.doc.song().channels.size()), kHostLinkId);
    rack_[0] = dpos.x + 4 + r.x;
    rack_[1] = dpos.y + 4 + r.y;
    rack_[2] = r.w;
    rack_[3] = r.h;
    placeRack_ = true;
    app.focusWindow = "Playlist";
  }
  layoutDone_ = true;
  if (++frameNo_ == 3)  // dev checks: a window brought to front once the layout exists (e.g. "###HostLink")
    if (const char* f = std::getenv("GRIDLINE_PLUGIN_FOCUS"); f && *f) app.focusWindow = f;
  {
    const ImVec2 avail(dsize.x - 8, dsize.y - 8);
    if (lastDock_[0] > 0 && (std::fabs(avail.x - lastDock_[0]) > 0.5f || std::fabs(avail.y - lastDock_[1]) > 0.5f))
      shell::fitDockTree(ImGui::DockBuilderGetNode(dockId), ImVec2(avail.x / lastDock_[0], avail.y / lastDock_[1]), avail);
    lastDock_[0] = avail.x;
    lastDock_[1] = avail.y;
  }
  ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_None);
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);

  drawBrowser(app);
  drawHostLink(inst_);
  drawPlaylist(app);
  drawLivePads(app);
  drawMasterPanel(app);
  drawPianoRoll(app);
  if (placeRack_ && app.showRack) ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
  drawChannelRack(app);
  if (placeRack_ && app.showRack)
    if (ImGuiWindow* rw = ImGui::FindWindowByName("Channel rack")) {
      ImGui::SetWindowPos(rw, ImVec2(rack_[0], rack_[1]), ImGuiCond_Always);
      ImGui::SetWindowSize(rw, ImVec2(rack_[2], rack_[3]), ImGuiCond_Always);
      placeRack_ = false;
    }
  shell::keepFloatingInView("Channel rack", dpos, dsize);
  drawMixer(app);
  drawChannelSettings(app);
  drawEffectEditor(app);
  packs::drawWindows(app);
  drawAutomationEditor(app);
  drawDialogs(app);
  browserui::drawPluginPicker(app);
  if (!app.focusWindow.empty()) {
    ImGui::SetWindowFocus(app.focusWindow.c_str());
    app.focusWindow.clear();
  }
  drawStatusBar(app);
  handleGlobalShortcuts(app);
  app.quitRequested = false;  // the host closes the plugin, not Gridline

  ImGui::Render();
  glViewport(0, 0, fbW, fbH);
  const ImVec4 bg = theme::vec(theme::pal().chrome);
  glClearColor(bg.x, bg.y, bg.z, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  app.syncEngine();
}

}  // namespace gl::plugin
