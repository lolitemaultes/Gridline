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
#include "../app/DialogLayout.h"
#include "../app/Shell.h"
#include "../app/Theme.h"
#include "../app/ThemeSettings.h"
#include "../app/Widgets.h"
#include "../app/PluginUI.h"
#include "../app/Windows.h"
#include "../core/I18n.h"
#include "../core/Platform.h"
#include "../engine/ChannelIns.h"
#include "../engine/Sample.h"
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

// J-130 hook: the suite's logo in a Gridline Suite editor (bottom right,
// `height` logical pixels). Returns false while the suite has no logo; the
// editor then shows nothing there.
bool drawSuiteLogo(App& a, const char* suiteName, float height) {
  (void)a;
  (void)suiteName;
  (void)height;
  return false;
}

// Gridline Suite: the sampler's sample (load button, file name), as in the
// channel window.
void drawSampleRow(App& a, ObjectId id) {
  const Channel* c = a.doc.song().channel(id);
  if (!c) return;
  const bool pick = ui::TextButton("Load sample...", false, 0, "Choose an audio file for this channel");
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_FILE")) {
      std::string path(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      if (isAudioFile(path)) a.doc.editChannel(id, "load sample", [&](Channel& x) { x.plugin.data = path; });
    }
    ImGui::EndDragDropTarget();
  }
  if (pick) {
    a.fileDialog = {};
    a.fileDialog.open = true;
    a.fileDialog.title = "Load sample";
    a.fileDialog.dir = c->plugin.data.empty() ? (gl::platform::homeEnv() ? gl::platform::homeEnv() : ".") : fs::u8path(c->plugin.data).parent_path().string();
    a.fileDialog.exts = {".wav", ".wave", ".flac", ".mp3"};
    a.fileDialog.onOk = [&a, id](const std::string& p) {
      a.doc.editChannel(id, "load sample", [&](Channel& x) { x.plugin.data = p; });
      if (!sampleCache().get(p)) a.notify("Cannot decode " + p, true);
    };
  }
  if (!c->plugin.data.empty()) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(theme::pal().textDim), "%s", fs::u8path(c->plugin.data).filename().string().c_str());
    if (!sampleCache().request(c->plugin.data)) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(theme::pal().danger), "%s", i18n::tr("missing"));
    }
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
}

}  // namespace

Frame::Frame(Instance& inst) : inst_(inst) {}

// J-131: one built-in plugin (Gridline Suite): its editor alone, as Gridline's
// plugin window shows it (menu, preset header, editor; instruments with
// channel envelopes also get the Envelope page), scaled to the host's window.
void Frame::drawSingle(int winW, int winH, int fbW, int fbH) {
  App& app = inst_.app();
  const PluginInfo& info = *inst_.pluginInfo();
  ImGuiIO& io = ImGui::GetIO();
  const double now = nowSeconds();
  io.DeltaTime = last_ > 0 && now > last_ ? static_cast<float>(std::min(0.25, now - last_)) : 1.0f / 60.0f;
  last_ = now;
  app.hint.clear();
  int cw = 0, ch = 0;
  inst_.editorContent(cw, ch);
  float zoom = inst_.editorZoom();
  if (pendingResize_ > 0) --pendingResize_;
  if (cw > 0 && ch > 0) {
    const bool fits = std::fabs(cw * zoom - static_cast<float>(winW)) <= 2.f && std::fabs(ch * zoom - static_cast<float>(winH)) <= 2.f;
    if (fits) {
      pendingResize_ = 0;
    } else if (pendingResize_ == 0) {  // the host or the user resized the window: the editor zooms to fit
      inst_.setEditorZoom(std::min(static_cast<float>(winW) / static_cast<float>(cw), static_cast<float>(winH) / static_cast<float>(ch)));
      zoom = inst_.editorZoom();
    }
  }
  uiScale_ = zoom;
  io.DisplaySize = ImVec2(static_cast<float>(winW) / zoom, static_cast<float>(winH) / zoom);
  io.DisplayFramebufferScale = ImVec2(static_cast<float>(fbW) / io.DisplaySize.x, static_cast<float>(fbH) / io.DisplaySize.y);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  pointerGestureFrame(app);  // one undo step per pointer gesture
  const ObjectId owner = inst_.pluginOwner();
  const PluginTarget target{owner, inst_.pluginSlot()};
  app.focusPluginOwner = target.owner;
  app.focusPluginSlot = target.slot;
  const ImVec2 at(cw > 0 ? std::max(0.f, (io.DisplaySize.x - static_cast<float>(cw)) * 0.5f) : 0.f,
                  ch > 0 ? std::max(0.f, (io.DisplaySize.y - static_cast<float>(ch)) * 0.5f) : 0.f);
  ImGui::SetNextWindowPos(at);
  ImVec2 size(0, 0);
  if (ImGui::Begin("##suite", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
    const float width = info.editorWidth > 0 ? info.editorWidth : (info.type == "gl.eq" ? 780.0f : 600.0f);
    int tab = inst_.editorTab();
    const bool envPage = target.isChannel() && insSupported(info.type);
    if (envPage) {
      const char* tabs[] = {"Plugin", "Envelope"};
      const char* hints[] = {"The instrument's editor", "Envelopes, LFOs and the filter this channel applies to every note"};
      tab = std::clamp(tab, 0, 1);
      if (ui::TabStrip("##suitetabs", tabs, 2, &tab, hints)) inst_.setEditorTab(tab);
    }
    if (envPage && tab == 1) {
      drawChannelEnvelope(app, owner);
    } else {
      if (target.isChannel() && info.type == "gl.sampler") drawSampleRow(app, owner);
      menuButton("suitemenu", target.isChannel() ? "Instrument menu: presets, state" : "Effect menu: presets, state",
                 [&app, target]() { pluginWindowMenu(app, target.owner, target.slot); });
      ImGui::SameLine(0, 8);
      drawPluginHeader(app, target);
      drawPluginBody(app, target, width);
    }
    drawSuiteLogo(app, "Gridline Suite", 14.0f);
    // The editor's natural size: auto-resize clamps the window to the display,
    // which is the view at the current zoom, so a larger editor would never be
    // measured (and would be cut off instead of zoomed to fit).
    const ImGuiWindow* win = ImGui::GetCurrentWindow();
    size = ImVec2(win->ContentSizeIdeal.x + win->WindowPadding.x * 2, win->ContentSizeIdeal.y + win->WindowPadding.y * 2);
  }
  ImGui::End();
  drawDialogs(app);
  if (size.x > 1 && size.y > 1 && inst_.setEditorContent(static_cast<int>(std::ceil(size.x)), static_cast<int>(std::ceil(size.y)))) {
    int w = 0, h = 0;
    inst_.preferredViewSize(w, h);
    if (View* v = inst_.editorView(); v && v->requestResize && (w != winW || h != winH)) {
      pendingResize_ = 30;  // the host answers within a few frames (or not: the editor then fits the window)
      v->requestResize(w, h);
    }
  }
  app.quitRequested = false;

  ImGui::Render();
  glViewport(0, 0, fbW, fbH);
  const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
  glClearColor(bg.x, bg.y, bg.z, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  inst_.syncParams();
  app.syncEngine();
}
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
  io.IniFilename = inst_.single() ? nullptr : iniPath_.c_str();  // a suite editor has no layout to keep
  rendererReady_ = ImGui_ImplOpenGL3_Init(glslVersion);
  if (!inst_.single()) app.extraParamMenu = [this](ParamId pid) { macroMenu(inst_, pid); };
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
  if (inst_.single()) {
    drawSingle(winW, winH, fbW, fbH);
    return;
  }
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
  pointerGestureFrame(app);  // one undo step per pointer gesture, in every editor
  ui::ClampTrackedWindows();  // J-91: floating windows kept in the work area before they are laid out
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
