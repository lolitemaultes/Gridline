// Gridline — AI assistants in the app (SC-07, see McpUi.h).
#include "McpUi.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Mcp.h"
#include "../core/McpTransport.h"
#include "../core/Presets.h"
#include "../core/ProjectFiles.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "ScriptUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::mcpui {

using theme::pal;
using i18n::tr;
namespace fs = std::filesystem;

namespace {

struct State {
  std::unique_ptr<script::ProjectHost> host;
  std::unique_ptr<mcp::Environment> env;
  std::unique_ptr<mcp::HttpEndpoint> ep;
  std::string error;  // last start error
  int port = -1;      // port the server was started with (setting value)
  double retryAt = 0;
  bool showLog = false;
  bool revealToken = false;
  std::string endpointFile;
};
State M;

void label(const char* text, float x = 168.0f) { ui::FormLabelFit(tr(text), x, pal().textDim); }

void removeEndpointFile() {
  if (M.endpointFile.empty()) return;
  std::error_code ec;
  fs::remove(M.endpointFile, ec);
}

void stopServer() {
  if (M.ep && M.ep->running()) {
    M.ep->stop();
    logging::info("mcp", "AI assistant server stopped");
  }
  removeEndpointFile();
  M.port = -1;
}

void ensureEnv(App& a) {
  if (M.env) return;
  M.host = makeProjectHost(a);
  M.env = std::make_unique<mcp::Environment>(a.doc, *M.host);
  mcp::Environment& e = *M.env;
  e.presetRoot = presetRoot(a.configDir());
  e.save = [&a](const std::string& path, std::string& err) {
    if (a.saveProjectAs(path, SaveFormat::Gridline)) return true;
    err = "save failed (see Gridline's notifications)";
    return false;
  };
  e.onActivity = [&a](const mcp::Activity& act) {
    if (act.tool == "connect") a.notify(std::string(tr("AI assistant connected")) + ": " + act.client);
  };
  e.editsOffMessage =
      "Changes are switched off: the user can enable \"Allow assistants to change the project and write files\" in Gridline's "
      "Settings > Integrations.";
  M.ep = std::make_unique<mcp::HttpEndpoint>(e, "");
  M.endpointFile = mcp::defaultEndpointFile();
}

std::string token(App& a) {
  std::string t = a.settings.getString("mcp.token");
  if (t.size() < 16) {
    t = mcp::randomToken();
    a.settings.set("mcp.token", t);
    a.settings.save(Settings::defaultPath());
  }
  return t;
}

std::string timeText(double t) {
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
  localtime_r(&tt, &tm);
  char b[16];
  std::strftime(b, sizeof b, "%H:%M:%S", &tm);
  return b;
}

std::string cliPath() {
  fs::path self = host::selfExecutablePath();
  fs::path cli = self.parent_path() / "gridline";
  return cli.string();
}

std::string claudeCodeCommand(App& a) {
  const int port = M.ep && M.ep->running() ? M.ep->port() : static_cast<int>(a.settings.getNumber("mcp.port"));
  return "claude mcp add --transport http gridline http://127.0.0.1:" + std::to_string(port) + "/mcp --header \"Authorization: Bearer " +
         token(a) + "\"";
}

std::string desktopConfig() {
  return "{\n  \"mcpServers\": {\n    \"gridline\": {\n      \"command\": \"" + cliPath() + "\",\n      \"args\": [\"mcp\", \"--attach\"]\n    }\n  }\n}";
}

}  // namespace

void tick(App& a) {
  if (!a.settings.getBool("mcp.enabled")) {
    if (M.ep && M.ep->running()) stopServer();
    M.error.clear();
    return;
  }
  ensureEnv(a);
  mcp::Environment& e = *M.env;
  e.allowEdits = a.settings.getBool("mcp.allowEdits");
  e.allowScripts = a.settings.getBool("scripts.enabled");
  e.projectPath = a.projectFile();
  e.exportDir = a.settings.getString("mcp.exportFolder");
  e.factoryData = a.settings.getString("import.flData");
  e.scriptLimits = projectScriptLimits(a);
  const std::string tok = token(a);
  const int port = std::clamp(static_cast<int>(a.settings.getNumber("mcp.port")), 0, 65535);
  if (M.ep->running() && port != M.port) stopServer();
  if (M.ep->running() && M.ep->token() != tok) {  // new token: old clients must reconnect
    M.ep->stop();
    M.port = -1;
  }
  if (!M.ep->running() && a.now >= M.retryAt) {
    M.ep->setToken(tok);
    std::string err;
    if (M.ep->start(port, &err)) {
      M.port = port;
      M.error.clear();
      std::string ferr;
      if (!mcp::writeEndpointFile(M.endpointFile, M.ep->port(), tok, &ferr)) logging::warning("mcp", ferr);
      logging::info("mcp", "AI assistant server on 127.0.0.1:" + std::to_string(M.ep->port()));
    } else {
      M.error = err;
      M.retryAt = a.now + 5.0;
      logging::error("mcp", err);
    }
  }
  // An editor holding an undo group open (live script preview, a drag) keeps
  // requests waiting: an assistant step must never land inside the user's.
  if (M.ep->running() && a.doc.groupDepth() == 0) M.ep->poll(0);
}

void shutdown() {
  stopServer();
  M.ep.reset();
  M.env.reset();
  M.host.reset();
}

void showActivity() { M.showLog = true; }

void statusLamp(App& a, ImDrawList* dl, float& rx, float ty, float fs) {
  if (!M.ep || !M.ep->running()) return;
  const auto& P = pal();
  const bool connected = M.ep->sessionCount() > 0;
  const bool busy = M.env && M.env->log.lastTime() > 0 && M.env->clock() - M.env->log.lastTime() < 1.5;
  const char* lbl = tr("AI");
  const float lw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl).x;
  rx -= lw + 14 + theme::kS4;
  const ImVec2 c(rx + 4, ty + fs * 0.5f + 1);
  if (busy)
    dl->AddCircleFilled(c, 3.5f, P.hot, 12);
  else
    ui::Led(dl, c, 3.5f, connected);
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(rx + 12, ty + 1), connected ? P.text : P.textFaint, lbl);
}

void settingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection(tr("AI ASSISTANTS"));
  label("Assistants");
  bool on = st.getBool("mcp.enabled");
  if (ImGui::Checkbox(tr("Allow AI assistants to control Gridline"), &on)) st.set("mcp.enabled", on);
  ui::Hint("Starts a server for MCP clients such as Claude Desktop or Claude Code, reachable from this computer only (127.0.0.1)");
  label("Changes");
  bool edits = st.getBool("mcp.allowEdits");
  if (ImGui::Checkbox(tr("Allow assistants to change the project and write files"), &edits)) st.set("mcp.allowEdits", edits);
  ui::Hint("Off: assistants can only look at the project and measure it. Every change is one undo step labelled AI");
  label("Status");
  {
    std::string s;
    ImU32 col = P.textDim;
    if (!on) {
      s = tr("Off");
    } else if (M.ep && M.ep->running()) {
      char b[160];
      std::snprintf(b, sizeof b, tr("Listening on 127.0.0.1:%d, %d assistant session(s)"), M.ep->port(), static_cast<int>(M.ep->sessionCount()));
      s = b;
      col = P.text;
    } else if (!M.error.empty()) {
      s = M.error;
      col = P.danger;
    } else {
      s = tr("Starting...");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(col));
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
  }
  label("Port");
  {
    int port = static_cast<int>(st.getNumber("mcp.port"));
    ImGui::SetNextItemWidth(std::min(fieldW, 140.0f));
    if (ImGui::InputInt("##mcpport", &port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) st.set("mcp.port", std::clamp(port, 0, 65535));
    ui::Hint("Loopback port of the server (0 = any free port; the attach bridge finds it either way)");
  }
  label("Token");
  {
    const std::string tok = token(a);
    std::string shown = M.revealToken ? tok : std::string(tok.size(), '*').substr(0, 24);
    ui::TextFit(shown.c_str(), std::max(80.0f, fieldW - 150), P.text, false);
    ImGui::SameLine();
    if (ui::TextButton(M.revealToken ? tr("Hide") : tr("Show"), false, 50)) M.revealToken = !M.revealToken;
    ImGui::SameLine();
    if (ui::TextButton(tr("Copy"), false, 50, "Copy the token to the clipboard")) ImGui::SetClipboardText(tok.c_str());
    ImGui::SameLine();
    if (ui::TextButton(tr("New"), false, 50, "Make a new token: assistants with the old one are refused")) {
      st.set("mcp.token", mcp::randomToken());
      st.save(Settings::defaultPath());
    }
  }
  label("Export folder");
  {
    static char buf[1024];
    static bool init = false;
    if (!init) std::snprintf(buf, sizeof buf, "%s", st.getString("mcp.exportFolder").c_str()), init = true;
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputTextWithHint("##mcpexport", tr("Only the project folder"), buf, sizeof buf)) st.set("mcp.exportFolder", std::string(buf));
    ui::FocusRing();
    ui::Hint("Assistants may write renders, exports and saves only into the project's folder and this folder");
  }
  label("");
  if (ui::TextButton(tr("Activity log"), M.showLog)) M.showLog = !M.showLog;
  ui::EndSection();

  ui::BeginSection(tr("CONNECT AN ASSISTANT"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Claude Code: run this command once (it includes the token)."));
  ImGui::PopTextWrapPos();
  const std::string cmd = claudeCodeCommand(a);
  ui::TextFit(cmd.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x - 70), P.textFaint, true);
  ImGui::SameLine();
  if (ui::TextButton(tr("Copy##cc"), false, 60)) ImGui::SetClipboardText(cmd.c_str());
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     tr("Claude Desktop: add this server to claude_desktop_config.json. It starts a bridge that connects to the running Gridline."));
  ImGui::PopTextWrapPos();
  const std::string cfg = desktopConfig();
  ui::TextFit(cliPath().c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x - 70), P.textFaint, true);
  ImGui::SameLine();
  if (ui::TextButton(tr("Copy##cd"), false, 60, "Copy the JSON configuration")) ImGui::SetClipboardText(cfg.c_str());
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("See docs/mcp.md for details. Assistants never get shell access, and file writes stay inside the folders above."));
  ImGui::PopTextWrapPos();
  ui::EndSection();
}

void drawActivityWindow(App& a) {
  if (!M.showLog) return;
  const auto& P = pal();
  ImGui::SetNextWindowSize(ImVec2(640, 360), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(tr("AI assistant activity###mcpactivity"), &M.showLog, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  if (!a.settings.getBool("mcp.enabled")) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("AI assistants are off (Settings > Integrations)."));
  } else if (M.ep && M.ep->running()) {
    std::string who;
    for (const auto& c : M.ep->clients()) who += (who.empty() ? "" : ", ") + c;
    ImGui::TextColored(theme::vec(P.text), "%s %d", tr("Listening on 127.0.0.1 port"), M.ep->port());
    if (!who.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textDim), "(%s)", who.c_str());
    }
    if (!a.settings.getBool("mcp.allowEdits")) ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Read only: changes are switched off."));
  }
  const std::string undo = a.doc.history().undoLabel();
  const bool aiUndo = undo.rfind("AI: ", 0) == 0;
  if (!aiUndo) ImGui::BeginDisabled();
  if (ui::TextButton(tr("Undo last assistant change"))) a.doc.undo();
  if (!aiUndo) ImGui::EndDisabled();
  ImGui::SameLine();
  if (ui::TextButton(tr("Clear")) && M.env) M.env->log.clear();
  ImGui::Separator();
  if (ImGui::BeginTable("##mcplog", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time"), ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn(tr("Assistant"), ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableSetupColumn(tr("Result"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    if (M.env) {
      const auto& v = M.env->log.entries();
      for (auto it = v.rbegin(); it != v.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::vec(P.textFaint), "%s", timeText(it->time).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(it->client.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::vec(it->changed ? P.hot : P.text), "%s", it->tool.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::vec(it->ok ? P.textDim : P.danger), "%s", it->summary.c_str());
      }
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

}  // namespace gl::mcpui
