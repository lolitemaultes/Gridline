// Gridline: Settings > Audio for the ASIO backend (K-6). See AsioUi.h.
#include "AsioUi.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../host/audio/AsioCore.h"
#include "App.h"
#include "DisplayConfirmUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::asioui {

using theme::pal;

namespace {

void label(const char* t) { ui::FormLabelFit(t[0] ? i18n::tr(t) : "", 168.0f, pal().textDim); }

// Driver, rate and buffer changes can cut the sound: they apply at once and ask Keep / Revert.
const std::vector<dispconf::Key>& asioKeys() {
  static const std::vector<dispconf::Key> k = {
      {"audio.backend", 's'}, {"audio.asio.driver", 's'}, {"audio.asio.buffer", 'n'}, {"audio.asio.sampleRate", 'n'}};
  return k;
}

std::string snapshotText(const dispconf::Snapshot& s) {
  Settings tmp;
  dispconf::restore(tmp, s);
  if (tmp.getString("audio.backend") != "asio") return i18n::tr("System audio");
  std::string d = tmp.getString("audio.asio.driver");
  if (d.empty()) d = i18n::tr("no driver");
  char b[96];
  const int r = static_cast<int>(tmp.getNumber("audio.asio.sampleRate")), f = static_cast<int>(tmp.getNumber("audio.asio.buffer"));
  std::snprintf(b, sizeof b, " \xC2\xB7 %s \xC2\xB7 %s", r > 0 ? (std::to_string(r) + " Hz").c_str() : i18n::tr("device rate"),
                f > 0 ? std::to_string(f).c_str() : i18n::tr("preferred buffer"));
  return "ASIO \xC2\xB7 " + d + b;
}

// Writes a confirmed setting (the dialog reopens the audio device).
template <typename T>
void confirmSet(App& a, const char* key, T value) {
  Settings& st = a.settings;
  const dispconf::Snapshot before = dispconf::take(st, asioKeys());
  st.set(key, value);
  const dispconf::Snapshot after = dispconf::take(st, asioKeys());
  if (after == before) return;
  App* ap = &a;
  ui::confirmDisplayChange(a, dispconf::snapshotChange(st, "audio.asio", i18n::N_("Keep these audio settings?"), i18n::N_("Audio driver"), snapshotText(before),
                                                       snapshotText(after), before, after, dispconf::Preview::Audio, [ap]() { ap->audioReopen = true; }));
}

// A routing setting (reopens the driver, no confirmation: the sound keeps going).
void setRouting(App& a, const char* key, double v) {
  a.settings.set(key, v);
  a.audioReopen = true;
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

}  // namespace

bool inputsFromDriver(const App& a) { return a.asio.active && a.asio.inputs > 0; }

bool backendRow(App& a, float) {
  if (!a.asio.available) return false;
  const bool asioOn = a.settings.getString("audio.backend") == "asio";
  label("Driver type");
  if (ui::TextButton("System audio", !asioOn, 0.0f, "The operating system's audio (WASAPI on Windows): works with every device") && asioOn)
    confirmSet(a, "audio.backend", std::string("system"));
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("ASIO", asioOn, 0.0f, "A low-latency ASIO driver from the audio interface's maker") && !asioOn) {
    // First time: the only installed driver is chosen for you.
    if (a.settings.getString("audio.asio.driver").empty() && a.asio.drivers.size() == 1) a.settings.set("audio.asio.driver", a.asio.drivers.front());
    confirmSet(a, "audio.backend", std::string("asio"));
  }
  return asioOn;
}

void deviceRows(App& a, float fieldW) {
  const auto& P = pal();
  const App::AsioState& s = a.asio;
  Settings& st = a.settings;
  const std::string chosen = st.getString("audio.asio.driver");
  label("ASIO driver");
  const char* preview = !chosen.empty() ? chosen.c_str() : s.drivers.empty() ? i18n::tr("No ASIO driver installed") : i18n::tr("Choose a driver");
  if (ui::BeginCombo("##asiodrv", preview, fieldW)) {
    for (const auto& d : s.drivers)
      if (ImGui::Selectable(d.c_str(), d == chosen) && d != chosen) confirmSet(a, "audio.asio.driver", d);
    if (s.drivers.empty()) ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Install the driver of your audio interface, then reopen Settings."));
    ImGui::EndCombo();
  }
  ui::Hint("Drivers listed in the Windows registry (HKEY_LOCAL_MACHINE\\SOFTWARE\\ASIO)");
  label("Driver settings");
  ImGui::BeginDisabled(!s.active);
  if (ui::TextButton("Control panel", false, 0.0f, "The driver's own settings window (buffer, clock, routing)")) a.asio.panelRequest = true;
  ImGui::EndDisabled();
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("Reconnect", false, 0.0f, "Reopen the driver (after plugging the device back in)")) a.audioReopen = true;

  if (!s.active) {
    if (!s.error.empty()) {
      label("Status");
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(P.danger), "%s", s.error.c_str());
      ImGui::PopTextWrapPos();
    }
    return;
  }
  // Sample rates the device supports: the engine follows the device.
  label("Sample rate");
  {
    int shown = 0;
    const float avail = ImGui::GetContentRegionAvail().x;
    float used = 0;
    for (int r : s.rates) {
      const std::string t = std::to_string(r);
      const float w = ImGui::CalcTextSize(t.c_str()).x + 2 * ImGui::GetStyle().FramePadding.x;
      if (shown > 0) {
        if (used + theme::kS1 + w <= avail) ImGui::SameLine(0, theme::kS1), used += theme::kS1;
        else {
          label("");
          used = 0;
        }
      }
      used += w;
      if (ui::TextButton(t.c_str(), s.rate == r) && s.rate != r) confirmSet(a, "audio.asio.sampleRate", r);
      ++shown;
    }
  }
  // Buffer sizes the driver allows.
  label("Buffer size");
  {
    const int want = static_cast<int>(st.getNumber("audio.asio.buffer"));
    char pv[96];
    std::snprintf(pv, sizeof pv, "%d (%s)%s", s.buffer, msText(s.buffer, s.rate).c_str(), want == 0 ? i18n::tr(", driver's choice") : "");
    if (ui::BeginCombo("##asiobuf", pv, fieldW)) {
      char l[96];
      std::snprintf(l, sizeof l, i18n::tr("Driver's preferred size (%d)"), s.preferredBuffer);
      if (ImGui::Selectable(l, want == 0) && want != 0) confirmSet(a, "audio.asio.buffer", 0);
      ImGui::Separator();
      for (int b : s.bufferChoices) {
        std::snprintf(l, sizeof l, "%d  (%s)", b, msText(b, s.rate).c_str());
        if (ImGui::Selectable(l, want != 0 && b == s.buffer) && b != want) confirmSet(a, "audio.asio.buffer", b);
      }
      ImGui::EndCombo();
    }
    char hint[160];
    std::snprintf(hint, sizeof hint, "%s %d to %d, %s", i18n::tr("The driver allows"), s.minBuffer, s.maxBuffer,
                  s.granularity < 0 ? i18n::tr("powers of two") : s.granularity == 0 ? i18n::tr("one size") : (std::string(i18n::tr("in steps of ")) + std::to_string(s.granularity)).c_str());
    ui::Hint(hint);
  }
  label("Latency");
  ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
  ImGui::Text(i18n::tr("in %d smp (%s)  out %d smp (%s)"), s.inLatency, msText(s.inLatency, s.rate).c_str(), s.outLatency, msText(s.outLatency, s.rate).c_str());
  ImGui::PopFont();
  ui::Hint("As the driver reports it; recordings are moved earlier by input + output latency");
  label("Device");
  ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%d in, %d out \xC2\xB7 %s"), s.numIn, s.numOut, s.format.c_str());
}

void channelRows(App& a, float fieldW) {
  const auto& P = pal();
  const App::AsioState& s = a.asio;
  Settings& st = a.settings;
  if (!s.active) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Channels appear once the driver runs."));
    return;
  }
  // Inputs: a run of device inputs becomes the mixer's In 1, In 2, ...
  const int inCount = static_cast<int>(st.getNumber("audio.asio.inCount"));
  label("Inputs from");
  {
    std::string pv = s.inputs > 0 ? (s.inNames.empty() ? std::string("In ") + std::to_string(s.inFirst + 1) : s.inNames[static_cast<size_t>(s.inFirst)])
                                  : std::string(i18n::tr("None (system input device)"));
    if (ui::BeginCombo("##asioin", pv.c_str(), fieldW)) {
      if (ImGui::Selectable(i18n::tr("None (system input device)"), s.inputs == 0) && inCount != 0) setRouting(a, "audio.asio.inCount", 0);
      for (int k = 0; k < s.numIn; ++k) {
        const std::string n = k < static_cast<int>(s.inNames.size()) && !s.inNames[static_cast<size_t>(k)].empty() ? s.inNames[static_cast<size_t>(k)]
                                                                                                                 : "In " + std::to_string(k + 1);
        ImGui::PushID(k);
        if (ImGui::Selectable(n.c_str(), s.inputs > 0 && k == s.inFirst)) {
          st.set("audio.asio.inFirst", k);
          if (inCount == 0) st.set("audio.asio.inCount", std::min(2, s.numIn - k));
          a.audioReopen = true;
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ui::Hint("The first device input that becomes the mixer's In 1");
  }
  if (s.inputs > 0 || inCount > 0) {
    label("Input channels");
    const int room = std::min(asio::kMaxInputs, s.numIn - s.inFirst);
    const int counts[] = {1, 2, 4, 8, 16};
    bool first = true;
    for (int c : counts) {
      if (c > room && c != counts[0]) continue;
      if (!first) ImGui::SameLine(0, theme::kS1);
      first = false;
      if (ui::TextButton(std::to_string(c).c_str(), s.inputs == c) && s.inputs != c) setRouting(a, "audio.asio.inCount", c);
    }
    if (room > 0 && std::find(std::begin(counts), std::end(counts), room) == std::end(counts)) {
      ImGui::SameLine(0, theme::kS1);
      char l[32];
      std::snprintf(l, sizeof l, i18n::tr("All %d"), room);
      if (ui::TextButton(l, s.inputs == room) && s.inputs != room) setRouting(a, "audio.asio.inCount", room);
    }
    if (s.inputs > 0) {
      char m[128];
      std::snprintf(m, sizeof m, i18n::tr("Mixer In 1 to In %d"), s.inputs);
      ImGui::SameLine(0, theme::kS2);
      ImGui::TextColored(theme::vec(P.textDim), "%s", m);
    }
  }
  // Main output pair.
  label("Main output");
  if (s.mono) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", (asio::pairLabel(s.outNames, 0, false) + " " + i18n::tr("(mono: left and right mixed)")).c_str());
  } else if (ui::BeginCombo("##asioout", asio::pairLabel(s.outNames, s.mainOut, false).c_str(), fieldW)) {
    for (int k = 0; k + 1 < s.numOut; k += 2) {
      ImGui::PushID(k);
      if (ImGui::Selectable(asio::pairLabel(s.outNames, k, false).c_str(), k == s.mainOut) && k != s.mainOut) setRouting(a, "audio.asio.outMain", k);
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ui::Hint("The device outputs the master plays on");
  // Extra output pairs: each can carry one mixer track (post fader), for outboard gear or
  // separate monitor mixes.
  if (s.numOut >= 4) {
    std::vector<asio::ExtraOut> extras = asio::parseExtraOuts(st.getString("audio.asio.extraOuts"));
    const int mixers = static_cast<int>(a.doc.song().mixer.size());
    int rows = 0;
    for (int k = 0; k + 1 < s.numOut && rows < asio::kMaxExtraOutputs; k += 2) {
      if (k == s.mainOut) continue;
      ++rows;
      ImGui::PushID(1000 + k);
      label(rows == 1 ? "Extra outputs" : "");
      int cur = -1;
      for (const auto& e : extras)
        if (e.firstOut == k) cur = e.mixer;
      const std::string pairName = asio::pairLabel(s.outNames, k, false);
      const std::string pv = cur > 0 ? mixerName(a, cur) : std::string(i18n::tr("Off"));
      const float comboW = std::max(120.0f, fieldW * 0.5f);
      if (ui::BeginCombo("##extra", pv.c_str(), comboW)) {
        auto pick = [&](int m) {
          extras.erase(std::remove_if(extras.begin(), extras.end(), [&](const asio::ExtraOut& e) { return e.firstOut == k; }), extras.end());
          if (m > 0) extras.push_back({m, k});
          st.set("audio.asio.extraOuts", asio::formatExtraOuts(extras));
          a.audioReopen = true;
        };
        if (ImGui::Selectable(i18n::tr("Off"), cur <= 0) && cur > 0) pick(-1);
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
    if (rows > 0) ui::Hint("A mixer track (after its fader) also plays on this output pair");
  }
}

}  // namespace gl::asioui
