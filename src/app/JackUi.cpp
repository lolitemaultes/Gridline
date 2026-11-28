// Gridline: Settings > Audio for the JACK / PipeWire backend (K-357). See JackUi.h.
#include "JackUi.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../host/audio/AsioCore.h"
#include "../host/audio/JackCore.h"
#include "App.h"
#include "DisplayConfirmUi.h"
#include "LiveLook.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::jackui {

using theme::pal;

namespace {

void label(const char* t) { ui::FormLabelFit(t[0] ? i18n::tr(t) : "", 168.0f, pal().textDim); }

const std::vector<dispconf::Key>& jackConfirmKeys() {
  static const std::vector<dispconf::Key> k = {{"audio.backend", 's'}, {"audio.jack.bufferRequest", 'n'}};
  return k;
}

std::string snapshotText(const dispconf::Snapshot& s) {
  Settings tmp;
  dispconf::restore(tmp, s);
  if (tmp.getString("audio.backend") != "jack") return i18n::tr("System audio");
  const int req = static_cast<int>(tmp.getNumber("audio.jack.bufferRequest"));
  if (req <= 0) return std::string(i18n::tr("JACK / PipeWire")) + " \xC2\xB7 " + i18n::tr("Server default");
  return std::string(i18n::tr("JACK / PipeWire")) + " \xC2\xB7 " + std::to_string(req);
}

template <typename T>
void confirmSet(App& a, const char* key, T value) {
  Settings& st = a.settings;
  const dispconf::Snapshot before = dispconf::take(st, jackConfirmKeys());
  st.set(key, value);
  const dispconf::Snapshot after = dispconf::take(st, jackConfirmKeys());
  if (after == before) return;
  App* ap = &a;
  ui::confirmDisplayChange(a, dispconf::snapshotChange(st, "audio.jack", i18n::N_("Keep these audio settings?"),
                                                       i18n::N_("Audio driver"), snapshotText(before), snapshotText(after),
                                                       before, after, dispconf::Preview::Audio, [ap]() { ap->audioReopen = true; }));
}

std::string mixerName(const App& a, int i) {
  const auto& m = a.doc.song().mixer;
  if (i > 0 && i < static_cast<int>(m.size()) && !m[static_cast<size_t>(i)].name.empty()) return m[static_cast<size_t>(i)].name;
  char b[32];
  std::snprintf(b, sizeof b, i18n::tr("Insert %d"), i);
  return b;
}

std::string msText(int frames, int rate) {
  char b[48];
  std::snprintf(b, sizeof b, "%.1f ms", 1000.0 * frames / std::max(1, rate));
  return b;
}

void drawPortTable(App& a, const std::vector<App::JackPortView>& ports, const std::vector<std::string>& candidates,
                   const char* tableId) {
  const auto& P = pal();
  if (ports.empty()) return;
  const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;
  if (!ImGui::BeginTable(tableId, 3, flags)) return;
  ImGui::TableSetupColumn(i18n::tr("Port"), ImGuiTableColumnFlags_WidthFixed, 148.0f);
  ImGui::TableSetupColumn(i18n::tr("Connected to"), ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 92.0f);
  for (size_t i = 0; i < ports.size(); ++i) {
    const App::JackPortView& p = ports[i];
    ImGui::TableNextRow();
    ImGui::PushID(static_cast<int>(i));

    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    const std::string title = (!p.alias.empty() && p.alias != p.shortName) ? p.alias : p.shortName;
    ImGui::TextUnformatted(title.c_str());

    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    if (p.connected.empty()) {
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("not connected"));
    } else {
      const std::string joined = jack::formatConnectedPeers(p.connected);
      ImGui::TextColored(theme::vec(P.textDim), "%s", joined.c_str());
    }

    ImGui::TableSetColumnIndex(2);
    const std::string popId = "##jconn_" + p.shortName;
    if (ui::TextButton("Connect...", false, 88.0f, "Connect or disconnect this port with another app")) {
      a.jack.refreshGraphRequest = true;
      ImGui::OpenPopup(popId.c_str());
    }
    if (ImGui::BeginPopup(popId.c_str())) {
      if (candidates.empty()) {
        ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("No external audio ports found"));
      } else {
        for (size_t c = 0; c < candidates.size(); ++c) {
          const std::string& peer = candidates[c];
          const bool linked = std::find(p.connected.begin(), p.connected.end(), peer) != p.connected.end();
          ImGui::PushID(static_cast<int>(c));
          if (ImGui::MenuItem(peer.c_str(), nullptr, linked)) {
            a.jack.patchRequests.push_back({!linked, p.shortName, peer});
          }
          ImGui::PopID();
        }
      }
      if (!p.connected.empty()) {
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Disconnect all"))) {
          for (const std::string& peer : p.connected) a.jack.patchRequests.push_back({false, p.shortName, peer});
        }
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
}

}  // namespace

bool inputsFromDriver(const App& a) { return a.jack.active; }

bool backendRow(App& a, float) {
  if (!a.jack.available) return false;
  const bool jackOn = a.settings.getString("audio.backend") == "jack";
  label("Driver type");
  if (ui::TextButton("System audio", !jackOn, 0.0f, "The operating system's audio: works with every device") && jackOn)
    confirmSet(a, "audio.backend", std::string("system"));
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("JACK / PipeWire", jackOn, 0.0f,
                     "Named inputs and outputs that other apps can connect to (JACK, or PipeWire with its JACK support)") &&
      !jackOn) {
    confirmSet(a, "audio.backend", std::string("jack"));
  }
  return jackOn;
}

void deviceRows(App& a, float) {
  const auto& P = pal();
  const App::JackState& s = a.jack;
  Settings& st = a.settings;

  label("Server");
  if (s.active) {
    const char* via = s.fake ? i18n::tr("simulated server") : s.viaPipeWire ? i18n::tr("via PipeWire") : i18n::tr("JACK");
    char summary[256];
    std::snprintf(summary, sizeof summary, i18n::tr("Running as %s (%s)"), s.clientName.c_str(), via);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.text), "%s", summary);
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Reconnect", false, 0.0f, "Reconnect to the JACK or PipeWire server")) a.jack.reconnectRequest = true;
  } else {
    const std::string msg = !s.error.empty()
                                ? s.error
                                : std::string(i18n::tr("No JACK or PipeWire server is running. Using the system audio."));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.danger), "%s", msg.c_str());
    if (s.waitingReturn) {
      ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Using the system audio while waiting for the server to return."));
    } else if (!s.libraryMissing) {
      ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Using the system audio until a server is available."));
    }
    ImGui::PopTextWrapPos();
    if (ui::TextButton("Reconnect", false, 0.0f, "Try connecting to the JACK or PipeWire server again")) a.jack.reconnectRequest = true;
    ImGui::EndGroup();
    return;
  }

  // Sample rate follows the server (read-only).
  label("Sample rate");
  ImGui::PushFont(livelook::numFont(a), theme::kFontNumber + 1);
  ImGui::Text(i18n::tr("%d Hz (set by the server)"), s.rate);
  ImGui::PopFont();
  ui::Hint("The audio engine follows the JACK or PipeWire server's sample rate");

  // Buffer size request (0 = server default, or explicit frames).
  label("Buffer size");
  {
    const int vals[] = {0, 64, 128, 256, 512, 1024, 2048};
    const char* labels[] = {i18n::tr("Server default"), "64", "128", "256", "512", "1024", "2048"};
    const int req = static_cast<int>(st.getNumber("audio.jack.bufferRequest"));
    int idx = 0;
    for (int i = 0; i < 7; ++i)
      if (vals[i] == req) idx = i;
    if (ui::OptionRow("jackbuf", labels, 7, &idx, 160.0f) && vals[idx] != req) {
      confirmSet(a, "audio.jack.bufferRequest", vals[idx]);
    }
    ui::Hint("Server default follows the graph's quantum; picking a size requests it from PipeWire or sets it on JACK");
  }

  label("Latency");
  ImGui::PushFont(livelook::numFont(a), theme::kFontNumber + 1);
  ImGui::Text(i18n::tr("in %d smp (%s)  out %d smp (%s)  %d dropouts"), s.inLatency, msText(s.inLatency, s.rate).c_str(),
              s.outLatency, msText(s.outLatency, s.rate).c_str(), s.xruns);
  ImGui::PopFont();
  ui::Hint("As the server reports it; recordings are moved earlier by input + output latency");
}

void portRows(App& a, float fieldW) {
  const auto& P = pal();
  const App::JackState& s = a.jack;
  Settings& st = a.settings;

  // Input port pool count.
  {
    const int counts[] = {0, 2, 4, 8, 16};
    const char* labels[] = {"0", "2", "4", "8", "16"};
    const int cur = static_cast<int>(st.getNumber("audio.jack.inputs"));
    int idx = 1;
    for (int i = 0; i < 5; ++i)
      if (counts[i] == cur) idx = i;
    label("Input ports");
    if (ui::OptionRow("jackin", labels, 5, &idx, 160.0f) && counts[idx] != cur) {
      st.set("audio.jack.inputs", counts[idx]);
      a.audioReopen = true;
    }
    ui::Hint("Named input ports (In 1, In 2, ...) available to audio tracks and external apps");
  }

  // Extra stereo output pairs.
  {
    const int counts[] = {0, 1, 2, 4, 8, 15};
    const char* labels[] = {"0", "1", "2", "4", "8", "15"};
    const int cur = static_cast<int>(st.getNumber("audio.jack.extraPairs"));
    int idx = 0;
    for (int i = 0; i < 6; ++i)
      if (counts[i] == cur) idx = i;
    label("Extra output pairs");
    if (ui::OptionRow("jackoutpairs", labels, 6, &idx, 160.0f) && counts[idx] != cur) {
      st.set("audio.jack.extraPairs", counts[idx]);
      a.audioReopen = true;
    }
    ui::Hint("Additional stereo output ports (Out 3 and up) for Hardware Insert sends, Voltage Out and mixer tracks");
  }

  // Auto-connect, restore, auto-return switches.
  label("Connections");
  {
    ImGui::BeginGroup();
    bool conOut = st.getBool("audio.jack.connectOutputs");
    if (ImGui::Checkbox(i18n::tr("Connect outputs to the speakers##jackconout"), &conOut))
      st.set("audio.jack.connectOutputs", conOut);
    ui::Hint("On a new project without saved connections, wire Out 1 and Out 2 to the first physical playback ports");

    bool conIn = st.getBool("audio.jack.connectInputs");
    if (ImGui::Checkbox(i18n::tr("Connect inputs to the system inputs##jackconin"), &conIn))
      st.set("audio.jack.connectInputs", conIn);
    ui::Hint("Off by default to prevent microphone feedback; turn on to auto-wire system capture ports");

    bool restore = st.getBool("audio.jack.restoreLinks");
    if (ImGui::Checkbox(i18n::tr("Restore connections with the project##jackrestore"), &restore))
      st.set("audio.jack.restoreLinks", restore);
    ui::Hint("Save port connections per computer with the project and reconnect them when the other app starts");

    bool autoRet = st.getBool("audio.jack.autoReturn");
    if (ImGui::Checkbox(i18n::tr("Return when the server is back##jackreturn"), &autoRet))
      st.set("audio.jack.autoReturn", autoRet);
    ui::Hint("Switch back from the system audio automatically once the JACK or PipeWire server restarts");
    ImGui::EndGroup();
  }

  if (!s.active) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Ports appear once the JACK or PipeWire server is running."));
    return;
  }

  // Inline patchbay connections table.
  if (!s.inPorts.empty()) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    label("Inputs");
    ImGui::BeginGroup();
    drawPortTable(a, s.inPorts, s.extOutPorts, "##jackinports");
    ImGui::EndGroup();
  }
  if (!s.outPorts.empty()) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    label("Outputs");
    ImGui::BeginGroup();
    drawPortTable(a, s.outPorts, s.extInPorts, "##jackoutports");
    ImGui::EndGroup();
  }

  // Extra output pair routing (mixer track -> extra stereo pair).
  if (s.extraPairs > 0) {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    std::vector<asio::ExtraOut> extras = asio::parseExtraOuts(st.getString("audio.jack.extraOuts"));
    const int mixers = static_cast<int>(a.doc.song().mixer.size());
    for (int p = 0; p < s.extraPairs && p < jack::kMaxExtraPairs; ++p) {
      ImGui::PushID(2000 + p);
      label(p == 0 ? "Extra outputs" : "");
      int cur = -1;
      for (const auto& e : extras)
        if (e.firstOut == p) cur = e.mixer;
      const std::string pairName = "Out " + std::to_string(3 + 2 * p) + " - Out " + std::to_string(4 + 2 * p);
      const std::string pv = cur > 0 ? mixerName(a, cur) : std::string(i18n::tr("Direct only"));
      const float comboW = std::max(120.0f, fieldW * 0.5f);
      if (ui::BeginCombo("##jextra", pv.c_str(), comboW)) {
        auto pick = [&](int m) {
          extras.erase(std::remove_if(extras.begin(), extras.end(), [&](const asio::ExtraOut& e) { return e.firstOut == p; }),
                       extras.end());
          if (m > 0) extras.push_back({m, p});
          st.set("audio.jack.extraOuts", asio::formatExtraOuts(extras));
          a.audioReopen = true;
        };
        if (ImGui::Selectable(i18n::tr("Direct only"), cur <= 0) && cur > 0) pick(-1);
        for (int m = 1; m < mixers; ++m) {
          ImGui::PushID(m);
          if (ImGui::Selectable(mixerName(a, m).c_str(), m == cur) && m != cur) pick(m);
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      ImGui::SameLine(0, theme::kS2);
      ImGui::TextColored(theme::vec(P.textDim), "\xE2\x86\x92 %s", pairName.c_str());
      ImGui::PopID();
    }
    ui::Hint("Send a mixer track (after its fader) to this extra stereo pair; Direct only leaves it for Hardware Insert and Voltage Out");
  }
}

}  // namespace gl::jackui
