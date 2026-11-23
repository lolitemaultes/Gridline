// Gridline — AI assistants in the app (SC-07, see McpUi.h).
#include "McpUi.h"
#include "LayoutShellUi.h"
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
#include "../core/PluginInfo.h"
#include "../core/Presets.h"
#include "../core/ProjectFiles.h"
#include "../core/Secrets.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "CommandPaletteUi.h"
#include "ExportUi.h"
#include "ProgressUi.h"
#include "ScriptUi.h"
#include "Theme.h"
#include "UiWake.h"
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
  // J-61: the token lives in the Keychain / a private file, read once.
  std::string token;
  bool tokenStored = false;
  // K-264: where the running server listens, this computer's addresses (read when it starts),
  // and why a chosen address could not be used.
  mcp::listen::Policy policy;
  std::vector<uint32_t> own;
  std::string listenProblem;
  int confirmMode = -1;  // a mode beyond loopback waiting for the one-time confirmation
  bool watching = false;  // K-142 (follow-up): a new client wakes the interface (HttpServer::watch)
};
State M;

// K-264: the listen policy the settings ask for. Beyond loopback only once confirmed; a specific
// address must be one of this computer's own (otherwise this computer only, with a reason).
mcp::listen::Policy wantedPolicy(const Settings& st, const std::vector<uint32_t>& own, std::string& problem) {
  namespace L = mcp::listen;
  problem.clear();
  L::Policy p;
  p.mode = L::modeFromId(st.getString("mcp.listen"));
  if (p.mode == L::Mode::Loopback) return p;
  if (!st.getBool("mcp.listenConfirmed")) return L::Policy{};
  if (p.mode == L::Mode::Address) {
    uint32_t ip = 0;
    if (!L::parseIpv4(st.getString("mcp.listenAddress"), ip)) {
      problem = tr("Not an IPv4 address: listening on this computer only");
      return L::Policy{};
    }
    if (std::find(own.begin(), own.end(), ip) == own.end()) {
      problem = tr("Not an address of this computer: listening on this computer only");
      return L::Policy{};
    }
    p.address = ip;
  }
  return p;
}

bool samePolicy(const mcp::listen::Policy& x, const mcp::listen::Policy& y) { return x.mode == y.mode && x.address == y.address; }

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
  // J-61: renders, loudness and exports run on worker threads; projects with
  // hosted plugins render on this thread (their offline instances need it).
  e.jobs.background = true;
  e.jobs.runInline = [&a] {
    const Song& s = a.doc.song();
    for (const auto& c : s.channels)
      if (isHostedType(c.plugin.type)) return true;
    for (const auto& m : s.mixer)
      for (const auto& sl : m.slots)
        if (isHostedType(sl.plugin.type)) return true;
    return false;
  };
  // K-281: what only the app can do: the command palette's rows, the windows, the settings.
  e.app.commands = [&a] {
    std::vector<mcp::AppBridge::Command> v;
    for (const auto& c : cmdpalette::commandList(a)) {
      mcp::AppBridge::Command r;
      r.id = c.id;
      r.kind = c.kind == cmdpal::Kind::Window ? "window" : c.kind == cmdpal::Kind::Setting ? "setting" : c.kind == cmdpal::Kind::Plugin ? "plugin" : "command";
      r.label = c.label;
      r.path = c.path;
      r.shortcut = c.shortcut;
      r.hint = c.hint;
      r.enabled = c.enabled;
      r.checkable = c.checkable;
      r.checked = c.checked;
      v.push_back(std::move(r));
    }
    return v;
  };
  e.app.runCommand = [&a](const std::string& id, bool& queued, std::string& err) { return cmdpalette::runNow(a, id, queued, err); };
  e.app.windows = [&a] {
    std::vector<mcp::AppBridge::Window> v;
    for (const auto& w : cmdpalette::windowList(a)) v.push_back({w.name, w.open});
    return v;
  };
  e.app.showWindow = [&a](const std::string& name, bool open, std::string& err) { return cmdpalette::setWindowOpen(a, name, open, err); };
  e.app.settings = &a.settings;
  e.app.settingsChanged = [&a] { a.settings.save(Settings::defaultPath()); };
  M.ep = std::make_unique<mcp::HttpEndpoint>(e, "");
  M.endpointFile = mcp::defaultEndpointFile();
}

// J-61: the token is kept in the Keychain (macOS) or a 0600 file in the
// config folder; a token still in settings.json moves there on first use.
// K-327: read only when it is needed: the server starting (the user switched it on) or a press on
// the settings page (Show, Copy). Drawing the page never reads it, and a walk (secrets::NoAccess)
// gets "" and caches nothing.
std::string token(App& a) {
  if (M.token.size() < 16) {
    if (secrets::accessBlocked()) return {};
    M.token = secrets::migrate(a.settings, "mcp.token", Settings::defaultPath(), secrets::Store{}, "mcp-token", [] { return mcp::randomToken(); },
                               &M.tokenStored);
    if (M.token.size() < 16) M.token = mcp::randomToken();
  }
  return M.token;
}

bool tokenKnown() { return M.token.size() >= 16; }

void newToken(App& a) {
  const std::string t = mcp::randomToken();
  std::string err;
  M.tokenStored = secrets::Store{}.set("mcp-token", t, &err);
  if (!M.tokenStored) {  // no store: keep it in the settings as before
    logging::warning("mcp", "token kept in settings.json: " + err);
    a.settings.set("mcp.token", t);
    a.settings.save(Settings::defaultPath());
  }
  M.token = t;
}

// Assistant renders / exports in progress (J-61): the render progress window's
// layout, one window for all running jobs.
void drawJobs(App& a) {
  if (!M.env) return;
  auto jobs = M.env->jobs.running();
  if (jobs.empty()) return;
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin(tr("Assistant job###mcpjobs"), nullptr,
                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    ui::KeepWindowInViewport();
    progui::note("mcpjob");
    for (size_t i = 0; i < jobs.size(); ++i) {
      mcp::Job& j = *jobs[i];
      ImGui::PushID(static_cast<int>(j.id));
      if (i > 0) ImGui::Separator();
      const std::string who = j.client + ": " + j.title;  // 0.7.8: cut to the window's fixed width
      progui::Line(who.c_str(), exportui::kProgressW, pal().textDim);
      std::string st = j.status();
      if (st.empty()) st = "Preparing";
      if (j.cancel.load()) st = "Cancelling";
      if (exportui::progressBody(st, j.progress.load(), false)) j.cancel.store(true);
      ImGui::PopID();
    }
  }
  ImGui::End();
  (void)a;
}

std::string timeText(double t) {
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char b[16];
  std::strftime(b, sizeof b, "%H:%M:%S", &tm);
  return b;
}

std::string cliPath() {
  fs::path self = host::selfExecutablePath();
  fs::path cli = self.parent_path() / ("gridline" + self.extension().string());  // .exe on Windows
  return cli.string();
}

// A string as JSON needs it: a Windows path holds backslashes.
std::string jsonEscaped(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

// K-327: `withToken` false: "<token>" in its place (the page shows the command without reading the
// secret; its Copy button reads it).
std::string claudeCodeCommand(App& a, bool withToken) {
  const int port = M.ep && M.ep->running() ? M.ep->port() : static_cast<int>(a.settings.getNumber("mcp.port"));
  // K-264: the address a client uses (this computer's address beyond loopback: WSL, a VM)
  const std::string host = mcp::listen::clientHost(M.policy, M.own);
  return "claude mcp add --transport http gridline http://" + host + ":" + std::to_string(port) + "/mcp --header \"Authorization: Bearer " +
         (withToken ? token(a) : tokenKnown() ? M.token : std::string("<token>")) + "\"";
}

std::string desktopConfig() {
  return "{\n  \"mcpServers\": {\n    \"gridline\": {\n      \"command\": \"" + jsonEscaped(cliPath()) + "\",\n      \"args\": [\"mcp\", \"--attach\"]\n    }\n  }\n}";
}

}  // namespace

void tick(App& a) {
  if (!a.settings.getBool("mcp.enabled")) {
    if (M.ep && M.ep->running()) stopServer();
    if (M.env) mcp::pollJobs(*M.env);  // jobs still finishing after the switch went off
    M.error.clear();
    return;
  }
  ensureEnv(a);
  mcp::Environment& e = *M.env;
  e.allowEdits = a.settings.getBool("mcp.allowEdits");
  e.propose = a.settings.getBool("mcp.propose");  // K-92
  e.allowScripts = a.settings.getBool("scripts.enabled");
  e.projectPath = a.projectFile();
  e.exportDir = a.settings.getString("mcp.exportFolder");
  e.factoryData = a.settings.getString("import.flData");
  e.scriptLimits = projectScriptLimits(a);
  const std::string tok = token(a);
  const int port = std::clamp(static_cast<int>(a.settings.getNumber("mcp.port")), 0, 65535);
  if (M.ep->running() && port != M.port) stopServer();
  if (!M.ep->running() && a.now >= M.retryAt) M.own = mcp::listen::localAddresses();  // K-264: read once per start
  const mcp::listen::Policy pol = wantedPolicy(a.settings, M.own, M.listenProblem);
  if (M.ep->running() && !samePolicy(pol, M.policy)) stopServer();  // K-264: a new listen address
  if (M.ep->running() && M.ep->token() != tok) {  // new token: old clients must reconnect
    M.ep->stop();
    M.port = -1;
  }
  if (!M.ep->running() && a.now >= M.retryAt) {
    M.ep->setToken(tok);
    M.ep->setListen(pol, M.own);
    M.policy = pol;
    std::string err;
    if (M.ep->start(port, &err)) {
      M.port = port;
      M.error.clear();
      M.watching = M.ep->watch([] { uiwake::poke(); });  // K-142 (follow-up): a client wakes the interface
      std::string ferr;
      // the bridge runs on this computer: loopback reaches every mode but a specific address
      const std::string host = pol.mode == mcp::listen::Mode::Address ? mcp::listen::ipv4Text(pol.address) : std::string("127.0.0.1");
      if (!mcp::writeEndpointFile(M.endpointFile, M.ep->port(), tok, &ferr, host)) logging::warning("mcp", ferr);
      logging::info("mcp", "AI assistant server on " + mcp::listen::ipv4Text(mcp::listen::bindAddress(pol)) + ":" + std::to_string(M.ep->port()) +
                               (pol.mode == mcp::listen::Mode::Loopback ? "" : std::string(" (") + mcp::listen::modeId(pol.mode) + ")"));
    } else {
      M.error = err;
      M.retryAt = a.now + 5.0;
      logging::error("mcp", err);
    }
  }
  // An editor holding an undo group open (live script preview, a drag) keeps
  // requests waiting: an assistant step must never land inside the user's.
  if (M.ep->running() && a.doc.groupDepth() == 0) M.ep->poll(0);
  else mcp::pollJobs(e);
}

bool needsTurns(const App& a) {
  const bool jobs = M.env && M.env->jobs.busy();
  if (!a.settings.getBool("mcp.enabled")) return jobs;  // (jobs still finishing after the switch went off)
  if (!M.ep || !M.ep->running() || !M.watching) return true;
  return jobs;  // (a request, its reply going out: the server wakes the loop itself)
}

void shutdown() {
  if (M.env) M.env->jobs.cancelAll();  // assistant renders stop with the app
  stopServer();
  M.ep.reset();
  M.env.reset();
  M.host.reset();
}

// K-281 (tests): "mcp call <tool> <json arguments>" runs one tool call through an in-app session
// (edits allowed) and prints "MCP RESULT <tool> <reply>" on one line. JSON in single quotes.
bool devCommand(App& a, const std::vector<std::string>& args) {
  if (args.size() < 2 || args[0] != "call") return false;
  ensureEnv(a);
  mcp::Environment& e = *M.env;
  e.allowEdits = true;
  e.projectPath = a.projectFile();
  static std::unique_ptr<mcp::Session> session;
  if (!session) {
    session = std::make_unique<mcp::Session>(e);
    session->handle(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"dev"}}})");
  }
  std::string json;
  for (size_t i = 2; i < args.size(); ++i) json += (i > 2 ? " " : "") + args[i];
  if (json.empty()) json = "{}";
  std::replace(json.begin(), json.end(), '\'', '"');  // the dev input drops double quotes: JSON is written with single ones
  static int id = 1;
  const std::string req = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id++) + ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + args[1] +
                          "\",\"arguments\":" + json + "}}";
  std::printf("MCP RESULT %s %s\n", args[1].c_str(), session->handle(req).c_str());
  std::fflush(stdout);
  e.allowEdits = a.settings.getBool("mcp.allowEdits");
  return true;
}

void showActivity() { M.showLog = true; }
void hideActivity() { M.showLog = false; }

void statusLamp(App& a, ImDrawList* dl, float& rx, float ty, float fs) {
  if (!M.ep || !M.ep->running()) return;
  const auto& P = pal();
  const bool connected = M.ep->sessionCount() > 0;
  const bool busy = M.env && ((M.env->log.lastTime() > 0 && M.env->clock() - M.env->log.lastTime() < 1.5) || !M.env->jobs.running().empty());
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
  ui::Hint("Starts a server for MCP clients such as Claude Desktop or Claude Code, reachable from this computer only (127.0.0.1) unless Listen on says otherwise");
  label("Changes");
  bool edits = st.getBool("mcp.allowEdits");
  if (ImGui::Checkbox(tr("Allow assistants to change the project and write files"), &edits)) st.set("mcp.allowEdits", edits);
  {  // K-92: the Propose level
    bool prop = st.getBool("mcp.propose");
    if (!edits) ImGui::BeginDisabled();
    if (ImGui::Checkbox(tr("Propose changes first (accept or discard each in the AI assistant panel)"), &prop)) st.set("mcp.propose", prop);
    if (!edits) ImGui::EndDisabled();
  }
  ui::Hint("Off: assistants can only look at the project and measure it. Every change is one undo step labelled AI");
  label("Status");
  {
    std::string s;
    ImU32 col = P.textDim;
    if (!on) {
      s = tr("Off");
    } else if (M.ep && M.ep->running()) {
      char b[160];
      std::snprintf(b, sizeof b, tr("Listening on %s:%d, %d assistant session(s)"), mcp::listen::ipv4Text(mcp::listen::bindAddress(M.policy)).c_str(),
                    M.ep->port(), static_cast<int>(M.ep->sessionCount()));
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
    ui::Hint("Port of the server (0 = any free port; the attach bridge finds it either way)");
  }
  // K-264: where the server listens. Loopback is the default; the others are opt-in, in red, and
  // ask once for a confirmation. The token is required whatever the address.
  label("Listen on");
  {
    namespace L = mcp::listen;
    static const char* names[4] = {"This computer only", "This computer, WSL and virtual machines", "A specific address", "All networks"};
    const L::Mode cur = L::modeFromId(st.getString("mcp.listen"));
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::BeginCombo("##mcplisten", tr(names[static_cast<int>(cur)]))) {
      for (int i = 0; i < 4; ++i) {
        if (i > 0) ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.danger));
        if (ImGui::Selectable(tr(names[i]), i == static_cast<int>(cur))) {
          if (i == 0 || st.getBool("mcp.listenConfirmed")) st.set("mcp.listen", std::string(L::modeId(static_cast<L::Mode>(i))));
          else M.confirmMode = i;
        }
        if (i > 0) ImGui::PopStyleColor();
      }
      ImGui::EndCombo();
    }
    ui::Hint("This computer only: 127.0.0.1. WSL and virtual machines: also private addresses (10.x, 172.16-31.x, 192.168.x). "
             "All networks: any address. The token is always required");
    if (M.confirmMode > 0) ImGui::OpenPopup("##mcplistenconfirm");
    if (ImGui::BeginPopupModal("##mcplistenconfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
      ImGui::TextColored(theme::vec(P.danger), "%s", tr("Let assistants on other machines reach Gridline?"));
      ImGui::PushTextWrapPos(420);
      ImGui::TextUnformatted(tr("The server will accept connections from outside this computer. Anyone who has the token can then read and, if "
                                "changes are allowed, edit your project. Use this only on a network you trust, and make a new token if it "
                                "was shared."));
      ImGui::PopTextWrapPos();
      if (ui::TextButton(tr("Listen beyond this computer"), false, 0)) {
        st.set("mcp.listenConfirmed", true);
        st.set("mcp.listen", std::string(L::modeId(static_cast<L::Mode>(M.confirmMode))));
        M.confirmMode = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ui::TextButton(tr("Cancel"), false, 0) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        M.confirmMode = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    if (cur == L::Mode::Address) {
      label("Address");
      static char buf[64];
      static bool init = false;
      if (!init) std::snprintf(buf, sizeof buf, "%s", st.getString("mcp.listenAddress").c_str()), init = true;
      std::string mine;
      for (uint32_t o : M.own.empty() ? L::localAddresses() : M.own)
        if (!L::isLoopback(o)) mine += (mine.empty() ? "" : ", ") + L::ipv4Text(o);
      ImGui::SetNextItemWidth(std::min(fieldW, 180.0f));
      if (ImGui::InputTextWithHint("##mcpaddr", mine.empty() ? "192.168.1.20" : mine.substr(0, mine.find(',')).c_str(), buf, sizeof buf))
        st.set("mcp.listenAddress", std::string(buf));
      ui::FocusRing();
      ui::Hint((std::string(tr("One of this computer's addresses")) + (mine.empty() ? std::string() : ": " + mine)).c_str());
    }
    if (cur != L::Mode::Loopback) {
      label("");
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(P.danger), "%s",
                         !M.listenProblem.empty() ? M.listenProblem.c_str()
                         : cur == L::Mode::All ? tr("Reachable from any network this computer is on. Keep the token secret.")
                                               : tr("Reachable from other machines on private addresses. Keep the token secret."));
      ImGui::PopTextWrapPos();
    }
  }
  label("Token");
  {
    // K-327: the page shows the token only once it was read (the server started, or Show / Copy was
    // pressed): opening the page, hovering it or the command palette reading it never reads the
    // Keychain, which can wait on a macOS password prompt.
    const bool known = tokenKnown();
    std::string shown = !known ? std::string(tr("(not read yet: Show or Copy reads it)"))
                        : M.revealToken ? M.token
                                        : std::string(M.token.size(), '*').substr(0, 24);
    ui::TextFit(shown.c_str(), std::max(80.0f, fieldW - 150), known ? P.text : P.textFaint, false);
    ImGui::SameLine();
    if (ui::TextButton(M.revealToken ? tr("Hide") : tr("Show"), false, 50)) {
      if (!M.revealToken) token(a);
      M.revealToken = !M.revealToken;
    }
    ImGui::SameLine();
    if (ui::TextButton(tr("Copy"), false, 50, "Copy the token to the clipboard")) ImGui::SetClipboardText(token(a).c_str());
    ImGui::SameLine();
    if (ui::TextButton(tr("New"), false, 50, "Make a new token: assistants with the old one are refused")) newToken(a);
  }
  label("");
  ImGui::TextColored(theme::vec(P.textFaint), "%s %s", tr("Token kept in:"),
                     M.tokenStored || !tokenKnown() ? tr(secrets::backendName(secrets::defaultBackend())) : tr("settings.json (no secure store)"));
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
  const std::string cmd = claudeCodeCommand(a, false);
  ui::TextFit(cmd.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x - 70), P.textFaint, true);
  ImGui::SameLine();
  if (ui::TextButton(tr("Copy##cc"), false, 60)) ImGui::SetClipboardText(claudeCodeCommand(a, true).c_str());
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
  drawJobs(a);
  if (!M.showLog) return;
  const auto& P = pal();
  ImGui::SetNextWindowSize(ImVec2(640, 360), ImGuiCond_FirstUseEver);
  const char* title = tr("AI assistant activity###mcpactivity");
  if (!ImGui::Begin(title, &M.showLog, layoutui::dockFlags(title, ImGuiWindowFlags_NoCollapse))) {  // K-112: its region in a layout
    ImGui::End();
    return;
  }
  if (!a.settings.getBool("mcp.enabled")) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("AI assistants are off (Settings > Integrations)."));
  } else if (M.ep && M.ep->running()) {
    std::string who;
    for (const auto& c : M.ep->clients()) who += (who.empty() ? "" : ", ") + c;
    ImGui::TextColored(theme::vec(P.text), "%s %s %s %d", tr("Listening on"), mcp::listen::ipv4Text(mcp::listen::bindAddress(M.policy)).c_str(), tr("port"),
                       M.ep->port());
    if (!who.empty()) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textDim), "(%s)", who.c_str());
    }
  }
  // K-92: the permission level, the proposals waiting for an answer and the conversation.
  {
    static const char* levels[] = {i18n::N_("Read only"), i18n::N_("Propose"), i18n::N_("Allow changes")};
    const int cur = !a.settings.getBool("mcp.allowEdits") ? 0 : a.settings.getBool("mcp.propose") ? 1 : 2;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Assistants may"));
    for (int k = 0; k < 3; ++k) {
      ImGui::SameLine(0, k ? 2.0f : theme::kS2);
      if (ui::TextButton(tr(levels[k]), k == cur, 0,
                         k == 0   ? tr("Assistants read the project; every change is refused")
                         : k == 1 ? tr("Each change waits here for you to accept or discard it")
                                  : tr("Assistants change the project and write files; each change is one undo step"))) {
        a.settings.set("mcp.allowEdits", k > 0);
        a.settings.set("mcp.propose", k == 1);
      }
    }
  }
  if (M.env && !M.env->proposals.empty()) {
    ImGui::Separator();
    uint64_t accept = 0, discard = 0;
    for (const auto& p : M.env->proposals) {
      ImGui::PushID(static_cast<int>(p.id));
      ImGui::TextColored(theme::vec(P.hot), "%s #%llu  %s (%s)", tr("Proposal"), static_cast<unsigned long long>(p.id), p.title.c_str(), p.client.c_str());
      for (const auto& l : p.diff) ImGui::TextColored(theme::vec(P.text), "  %s", l.c_str());
      if (ui::PrimaryButton(tr("Accept"), 90)) accept = p.id;
      ImGui::SameLine(0, theme::kS2);
      if (ui::TextButton(tr("Discard"), false, 90)) discard = p.id;
      ImGui::PopID();
    }
    if (accept) {
      std::string err;
      if (!mcp::acceptProposal(*M.env, accept, err)) a.notify(std::string(tr("Could not apply the proposal: ")) + err, true);
    }
    if (discard) mcp::discardProposal(*M.env, discard);
  }
  if (M.env) {
    ImGui::Separator();
    const float h = std::min(140.0f, 22.0f * static_cast<float>(M.env->chat.size()) + 4.0f);
    if (!M.env->chat.empty() && ImGui::BeginChild("##mcpchat", ImVec2(0, h), false)) {
      for (const auto& c : M.env->chat) {
        ImGui::TextColored(theme::vec(c.fromUser ? P.textDim : P.hot), "%s:", c.fromUser ? tr("You") : c.who.c_str());
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(c.text.c_str());
        ImGui::PopTextWrapPos();
      }
      if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    }
    if (!M.env->chat.empty()) ImGui::EndChild();
    static char ask[1024] = "";
    ImGui::SetNextItemWidth(-90);
    const bool enter = ImGui::InputTextWithHint("##mcpask", tr("Ask the connected assistant..."), ask, sizeof ask, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ui::TextButton(tr("Send"), false, 80) || enter) && ask[0]) {
      M.env->say(true, "user", ask);
      ask[0] = 0;
    }
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
