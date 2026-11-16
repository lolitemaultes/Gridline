// Gridline — K-107: the Speech dialog. See SpeechUi.h.
#include "SpeechUi.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/AudioBuffer.h"
#include "../core/I18n.h"
#include "../core/Speech.h"
#include "../core/WavWriter.h"
#include "App.h"
#include "BrowserUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::speechui {

namespace fs = std::filesystem;

namespace {

bool wantOpen = false;
char text[2048] = "Hello, I am Gridline.";
int voiceIx = 0;
speech::Voice voice = speech::voices()[0];

// The text as a file name: its first words.
std::string nameOf(const std::string& t) {
  std::string n;
  for (char c : t) {
    if (n.size() >= 32) break;
    if (std::isalnum(static_cast<unsigned char>(c))) n += c;
    else if (!n.empty() && n.back() != ' ') n += ' ';
  }
  while (!n.empty() && n.back() == ' ') n.pop_back();
  return n.empty() ? std::string("Speech") : n;
}

std::string renderFile(App& a) {
  const std::vector<float> s = speech::render(text, voice, 44100);
  if (s.empty()) return "";
  AudioBuffer b(2, s.size());
  for (size_t i = 0; i < s.size(); ++i) b.at(0, i) = b.at(1, i) = s[i];
  const fs::path dir = browserui::contentDir(a, "Speech");
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::string base = nameOf(text) + " (" + voice.name + ")";
  fs::path p = dir / (base + ".wav");
  for (int k = 2; fs::exists(p, ec) && k < 1000; ++k) p = dir / (base + " " + std::to_string(k) + ".wav");
  return writeWav(p.string(), b, 44100, 16) ? p.string() : std::string();
}

}  // namespace

void open(App&) { wantOpen = true; }

void draw(App& a) {
  if (wantOpen) {
    ImGui::OpenPopup("###speech");
    wantOpen = false;
  }
  ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
  bool keep = true;
  if (!ImGui::BeginPopupModal((std::string(i18n::tr("Speech")) + "###speech").c_str(), &keep, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextColored(theme::vec(theme::pal().textDim), "%s", i18n::tr("Text to speak"));
  ImGui::InputTextMultiline("##speechtext", text, sizeof text, ImVec2(440, 80));
  ImGui::TextColored(theme::vec(theme::pal().textDim), "%s", i18n::tr("Voice"));
  ImGui::SetNextItemWidth(200);
  const auto& vs = speech::voices();
  if (ImGui::BeginCombo("##speechvoice", i18n::tr(vs[static_cast<size_t>(voiceIx)].name.c_str()))) {
    for (int k = 0; k < static_cast<int>(vs.size()); ++k)
      if (ImGui::Selectable(i18n::tr(vs[static_cast<size_t>(k)].name.c_str()), k == voiceIx)) voiceIx = k, voice = vs[static_cast<size_t>(k)];
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Pitch"), &voice.pitch, 50, 400, "%.0f Hz");
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Speed"), &voice.speed, 0.5f, 2.0f, "%.2fx");
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Intonation"), &voice.intonation, 0, 1, "%.2f");
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Voice size"), &voice.formant, 0.75f, 1.35f, "%.2f");
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Breath"), &voice.breath, 0, 1, "%.2f");
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Vibrato"), &voice.vibrato, 0, 1, "%.2f st");
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Robot"), &voice.robot, 0, 1, "%.2f");
  ImGui::SetNextItemWidth(300);
  ImGui::SliderFloat(i18n::tr("Ring"), &voice.ring, 10, 400, "%.0f Hz");
  ImGui::Dummy(ImVec2(0, theme::kS2));
  const bool any = text[0] != 0;
  if (!any) ImGui::BeginDisabled();
  if (ui::TextButton(i18n::tr("Preview"), false, 100)) {
    const std::string f = renderFile(a);
    if (!f.empty()) browserui::previewFile(a, f);
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::PrimaryButton(i18n::tr("Add channel"), 120)) {
    const std::string f = renderFile(a);
    if (f.empty()) {
      a.notify(i18n::tr("Could not write the speech file"), true);
    } else {
      const ObjectId id = a.addInstrument("gl.sampler", f);
      a.doc.editChannel(id, "name speech channel", [n = nameOf(text)](Channel& c) { c.name = n; });
      a.showNewInstrument(id);
      ImGui::CloseCurrentPopup();
    }
  }
  if (!any) ImGui::EndDisabled();
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::tr("Close"), false, 90)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

}  // namespace gl::speechui
