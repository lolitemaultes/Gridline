// Gridline - K-107: the Speech dialog. See SpeechUi.h.
//
// Sections: Text (what to say, and the channel's name), Voice (the voices as chips), Shape
// (Pitch, Speed, Intonation, Voice size) and Character (Breath, Vibrato, Robot, Ring) as
// Gridline knobs with their units (right-click: reset, type a value), and the last preview's
// waveform. Enter previews, Cmd/Ctrl+Enter adds the channel, Escape closes. The last voice and
// its settings are remembered (setting speech.voice).
#include "SpeechUi.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/AudioBuffer.h"
#include "../core/I18n.h"
#include "../core/PluginInfo.h"
#include "../core/Speech.h"
#include "../core/WavWriter.h"
#include "../engine/Sample.h"
#include "App.h"
#include "BrowserUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::speechui {

namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100;

bool wantOpen = false, loaded = false;
char text[2048] = "Hello, I am Gridline.";
char channelName[128] = "";
int voiceIx = 0;
speech::Voice voice = speech::voices()[0];
// The last preview: its samples (the waveform), when it started and whether the settings changed since.
std::vector<float> lastRender;
std::vector<int64_t> lastStarts;  // K-313: where each word starts in it
speech::Style style = speech::Style::Natural;  // K-313: the pitch line (kept apart from the voice chips)
double previewStart = -1, previewSeconds = 0;
bool renderStale = true;

// The text as a name: its first words.
std::string nameOf(const std::string& text) {
  std::string t;  // K-313: the words alone (no pitch marks; joined words apart)
  for (const speech::Word& w : speech::parse(text).words) t += w.text + " ";
  std::string n;
  for (char c : t) {
    if (n.size() >= 32) break;
    if (std::isalnum(static_cast<unsigned char>(c))) n += c;
    else if (!n.empty() && n.back() != ' ') n += ' ';
  }
  while (!n.empty() && n.back() == ' ') n.pop_back();
  return n.empty() ? std::string("Speech") : n;
}

std::string channelNameNow() {
  std::string n = channelName;
  while (!n.empty() && std::isspace(static_cast<unsigned char>(n.back()))) n.pop_back();
  return n.empty() ? nameOf(text) : n;
}

// speech.voice: "name;pitch;intonation;speed;formant;breath;vibrato;robot;ring;style" (K-313: the
// style last; older values without it read as Natural).
std::string voiceText(const speech::Voice& v) {
  char b[256];
  std::snprintf(b, sizeof b, "%s;%g;%g;%g;%g;%g;%g;%g;%g;%s", v.name.c_str(), static_cast<double>(v.pitch), static_cast<double>(v.intonation),
                static_cast<double>(v.speed), static_cast<double>(v.formant), static_cast<double>(v.breath), static_cast<double>(v.vibrato),
                static_cast<double>(v.robot), static_cast<double>(v.ring), speech::styleId(style));
  return b;
}
void loadVoice(App& a) {
  if (loaded) return;
  loaded = true;
  const std::string s = a.settings.getString("speech.voice");
  if (s.empty()) return;
  std::stringstream ss(s);
  std::string name;
  if (!std::getline(ss, name, ';')) return;
  const auto& vs = speech::voices();
  for (size_t k = 0; k < vs.size(); ++k)
    if (vs[k].name == name) voiceIx = static_cast<int>(k), voice = vs[k];
  float* f[] = {&voice.pitch, &voice.intonation, &voice.speed, &voice.formant, &voice.breath, &voice.vibrato, &voice.robot, &voice.ring};
  for (float* p : f) {
    std::string part;
    if (!std::getline(ss, part, ';')) break;
    char* end = nullptr;
    const double v = std::strtod(part.c_str(), &end);
    if (end != part.c_str() && std::isfinite(v)) *p = static_cast<float>(v);
  }
  std::string st;
  if (std::getline(ss, st, ';')) style = speech::styleFromId(st);
}
void saveVoice(App& a) { a.settings.set("speech.voice", voiceText(voice)); }

speech::Voice voiceNow() {
  speech::Voice v = voice;
  v.style = style;
  return v;
}
speech::Rendered renderNow() { return speech::renderWords(text, voiceNow(), kRate); }

// K-313: the file carries a cue point (a slice) at every word and, in its comment, the voice and
// the text, so the channel's sample keeps what it was made from.
std::string writeFile(App& a, const speech::Rendered& r) {
  if (r.samples.empty()) return "";
  const fs::path dir = browserui::contentDir(a, "Speech");
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::string base = nameOf(text) + " (" + voice.name + ")";
  fs::path p = dir / (base + ".wav");
  for (int k = 2; fs::exists(p, ec) && k < 1000; ++k) p = dir / (base + " " + std::to_string(k) + ".wav");
  const std::vector<uint8_t> bytes = speech::wavWithWords(r, speech::parse(text).words, kRate, "speech: " + voiceText(voice) + "\n" + text);
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return out.good() ? p.string() : std::string();
}

bool previewPlaying(const App& a) { return previewStart >= 0 && a.now - previewStart < previewSeconds; }

void stopPreview(App& a) {
  if (previewPlaying(a)) a.stopPreview();
  previewStart = -1;
}

void preview(App& a) {
  speech::Rendered r = renderNow();
  lastRender = std::move(r.samples);
  lastStarts = std::move(r.wordStarts);
  renderStale = false;
  if (lastRender.empty()) return;
  auto s = std::make_shared<Sample>();
  s->path = "speech preview";
  s->sampleRate = kRate;
  s->left = lastRender;
  s->setMono();
  a.engine.preview(s);
  previewStart = a.now;
  previewSeconds = s->seconds();
}

void addChannel(App& a) {
  stopPreview(a);
  const std::string f = writeFile(a, renderNow());
  if (f.empty()) {
    a.notify(i18n::tr("Could not write the speech file"), true);
    return;
  }
  const ObjectId id = a.addInstrument("gl.sampler", f);
  a.doc.editChannel(id, "name speech channel", [n = channelNameNow()](Channel& c) { c.name = n; });
  a.showNewInstrument(id);
  ImGui::CloseCurrentPopup();
}

// A section caption: the name in the label font, a hairline to the right.
void caption(App& a, const char* name, float width) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const char* t = i18n::tr(name);
  const float fs = theme::kFontLabel;
  const float tw = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, t).x;
  dl->AddText(a.fontBold, fs, p, P.textDim, t);
  const float y = std::round(p.y + fs * 0.5f) + 0.5f;
  dl->AddLine(ImVec2(p.x + tw + theme::kS2, y), ImVec2(p.x + width, y), P.line);
  ImGui::Dummy(ImVec2(width, fs + theme::kS1));
}

// One knob of a section: the knob, its name and its value under it, in a cell of `cell` px.
bool knobCell(const char* id, float* v, const ParamDesc& d, float cell, double shownScale = 1.0) {
  const auto& P = theme::pal();
  ImGui::BeginGroup();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float r = 17.0f;
  double dv = static_cast<double>(*v) * shownScale;
  ImGui::SetCursorScreenPos(ImVec2(p.x + (cell - r * 2) * 0.5f, p.y));
  const bool ch = ui::Knob(id, &dv, d, r, nullptr, false);
  if (ch) *v = static_cast<float>(dv / shownScale);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const char* name = i18n::tr(d.name.c_str());
  const std::string val = d.format(dv);
  ImFont* f = ImGui::GetFont();
  const float y1 = p.y + r * 2 + 4;
  const ImVec2 ns = f->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, name);
  dl->AddText(f, theme::kFontLabel, ImVec2(std::round(p.x + (cell - ns.x) * 0.5f), y1), P.textDim, name);
  const ImVec2 vs = f->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, val.c_str());
  dl->AddText(f, theme::kFontLabel, ImVec2(std::round(p.x + (cell - vs.x) * 0.5f), y1 + theme::kFontLabel + 2), P.text, val.c_str());
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(cell, r * 2 + 6 + theme::kFontLabel * 2 + 2));
  ImGui::EndGroup();
  return ch;
}

// The four knobs of a section in a framed panel.
bool knobPanel(App& a, const char* title, float width, const char* const* ids, float* const* vals, const ParamDesc* const* ds, const double* scales) {
  const auto& P = theme::pal();
  caption(a, title, width);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float h = 34 + 6 + theme::kFontLabel * 2 + 2 + theme::kS2 * 2;
  ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + h), P.surface800, theme::kRadiusSm);
  const float cell = (width - theme::kS2 * 2) / 4.0f;
  bool ch = false;
  for (int k = 0; k < 4; ++k) {
    ImGui::SetCursorScreenPos(ImVec2(p0.x + theme::kS2 + cell * static_cast<float>(k), p0.y + theme::kS2));
    ch |= knobCell(ids[k], vals[k], *ds[k], cell, scales[k]);
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(width, h));
  return ch;
}

// The last preview's waveform, with the play position while it plays.
void waveform(App& a, float width, float height) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width, p0.y + height);
  dl->AddRectFilled(p0, p1, P.well, theme::kRadiusSm);
  const float mid = std::round((p0.y + p1.y) * 0.5f);
  if (lastRender.empty()) {
    const char* t = i18n::tr("Preview to see the speech here");
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(std::round(p0.x + (width - ts.x) * 0.5f), std::round(mid - ts.y * 0.5f)), P.textFaint, t);
  } else {
    float peak = 1e-6f;
    for (float s : lastRender) peak = std::max(peak, std::fabs(s));
    const int cols = std::max(1, static_cast<int>(width) - 8);
    const size_t n = lastRender.size();
    const ImU32 col = renderStale ? P.textFaint : P.hot;
    for (int x = 0; x < cols; ++x) {
      const size_t a0 = n * static_cast<size_t>(x) / static_cast<size_t>(cols), a1 = std::max(a0 + 1, n * static_cast<size_t>(x + 1) / static_cast<size_t>(cols));
      float lo = 0, hi = 0;
      for (size_t i = a0; i < a1 && i < n; ++i) lo = std::min(lo, lastRender[i]), hi = std::max(hi, lastRender[i]);
      const float half = (height - 8) * 0.5f;
      const float y0 = mid - hi / peak * half, y1 = mid - lo / peak * half;
      dl->AddLine(ImVec2(p0.x + 4 + static_cast<float>(x) + 0.5f, y0), ImVec2(p0.x + 4 + static_cast<float>(x) + 0.5f, std::max(y1, y0 + 1)), col);
    }
    // K-313: a tick where each word starts (the slices the added sample gets)
    for (size_t k = 1; k < lastStarts.size(); ++k) {
      const float x = std::round(p0.x + 4 + static_cast<float>(lastStarts[k]) / static_cast<float>(std::max<size_t>(1, n)) * static_cast<float>(cols)) + 0.5f;
      dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p0.y + 9), P.textDim, 1.0f);
      dl->AddLine(ImVec2(x, p1.y - 9), ImVec2(x, p1.y - 2), P.textDim, 1.0f);
    }
    if (previewPlaying(a)) {
      const float t = static_cast<float>((a.now - previewStart) / std::max(1e-3, previewSeconds));
      const float x = std::round(p0.x + 4 + t * static_cast<float>(cols)) + 0.5f;
      dl->AddLine(ImVec2(x, p0.y + 2), ImVec2(x, p1.y - 2), P.playhead, 1.0f);
    }
    char len[32];
    std::snprintf(len, sizeof len, "%.2f s", static_cast<double>(n) / kRate);
    const ImVec2 ls = ImGui::CalcTextSize(len);
    dl->AddText(ImVec2(p1.x - ls.x - 6, p0.y + 3), P.textDim, len);
  }
  ImGui::Dummy(ImVec2(width, height));
}

}  // namespace

void open(App&) { wantOpen = true; }

namespace {
bool devPending = false, devPreview = false;
std::string devStyle, devText;
}  // namespace
void devSet(const std::string& st, const std::string& t, bool pv) {
  wantOpen = true;
  devPending = true, devStyle = st, devText = t, devPreview = pv;
}

void draw(App& a) {
  if (wantOpen) {
    loadVoice(a);
    ImGui::OpenPopup("###speech");
    wantOpen = false;
  }
  if (devPending) {
    devPending = false;
    if (!devStyle.empty()) style = speech::styleFromId(devStyle);
    if (!devText.empty()) std::snprintf(text, sizeof text, "%s", devText.c_str());
    renderStale = true;
    if (devPreview) preview(a);
  }
  const float W = 600;
  ImGui::SetNextWindowSize(ImVec2(W + ImGui::GetStyle().WindowPadding.x * 2, 0), ImGuiCond_Appearing);
  bool keep = true;
  if (!ImGui::BeginPopupModal((std::string(i18n::tr("Speech")) + "###speech").c_str(), &keep, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (previewStart >= 0) previewStart = -1;
    return;
  }
  const auto& P = theme::pal();
  const ImGuiIO& io = ImGui::GetIO();
  const bool any = text[0] != 0;
  const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);  // no type-in box on top
  const bool enter = focused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
  const bool modEnter = enter && (io.KeyCtrl || io.KeySuper);

  // Text
  caption(a, "Text", W);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::PushStyleColor(ImGuiCol_FrameBg, P.field);
  // Enter previews and Ctrl/Cmd+Enter adds the channel (handled below for the whole dialog), so
  // neither may leave a new line in the text: the field takes Enter as "done", and the line a
  // modified Enter inserts is taken back.
  static std::string before;
  if (modEnter) before = text;
  ImGui::InputTextMultiline("##speechtext", text, sizeof text, ImVec2(W, 64), ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue);
  if (modEnter) std::snprintf(text, sizeof text, "%s", before.c_str());
  if (ImGui::IsItemEdited()) renderStale = true;
  const speech::Parsed parsed = speech::parse(text);
  {  // K-313: the pitch marks in colour (the accent when they are good, the warning colour when not)
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    const ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID());
    const ImVec2 scroll = st ? st->Scroll : ImVec2(0, 0);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->PushClipRect(r0, r1, true);
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const std::string t = text;
    for (const speech::Mark& m : parsed.marks) {
      const size_t ls = t.rfind('\n', m.begin == 0 ? 0 : m.begin - 1);
      const size_t lineStart = ls == std::string::npos || m.begin == 0 ? 0 : ls + 1;
      int line = 0;
      for (size_t k = 0; k < lineStart; ++k) line += t[k] == '\n';
      const std::string pre = t.substr(lineStart, m.begin - lineStart), body = t.substr(m.begin, m.end - m.begin);
      const float x = r0.x + pad.x + ImGui::CalcTextSize(pre.c_str()).x - scroll.x;
      const float y = r0.y + pad.y + static_cast<float>(line) * ImGui::GetTextLineHeight() - scroll.y;
      const ImVec2 bs = ImGui::CalcTextSize(body.c_str());
      const ImU32 col = m.valid ? P.hot : P.danger;
      fg->AddRectFilled(ImVec2(x - 1, y), ImVec2(x + bs.x + 1, y + bs.y), theme::withAlpha(col, 0.18f), 2.0f);
      fg->AddText(ImVec2(x, y), col, body.c_str());
    }
    fg->PopClipRect();
  }
  ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("A number after a word raises or lowers it in semitones: high(3) low(-2). _ joins words."));
  ui::Hint(i18n::tr("A comma pauses; a full stop or question mark ends a phrase. Enter previews, Ctrl+Enter adds the channel."));
  for (const speech::Mark& m : parsed.marks)
    if (!m.valid) {
      const std::string bad = std::string(text).substr(m.begin, std::min<size_t>(m.end - m.begin, 24));
      ImGui::TextColored(theme::vec(P.danger), i18n::tr("%s is not a pitch mark (a number of semitones, -24 to 24); it is left out"), bad.c_str());
      break;
    }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Channel name"));
  ImGui::SameLine(0, theme::kS2);
  ImGui::SetNextItemWidth(W - ImGui::GetCursorPosX() + ImGui::GetStyle().WindowPadding.x);
  ImGui::InputTextWithHint("##speechname", nameOf(text).c_str(), channelName, sizeof channelName);
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, theme::kS2));

  // Voice: the voices as chips, five to a row
  caption(a, "Voice", W);
  const auto& vs = speech::voices();
  const int perRow = 5;
  const float gap = theme::kS1, chipW = (W - gap * (perRow - 1)) / perRow;
  for (int k = 0; k < static_cast<int>(vs.size()); ++k) {
    if (k % perRow) ImGui::SameLine(0, gap);
    ImGui::PushID(k);
    if (ui::TextButton(i18n::tr(vs[static_cast<size_t>(k)].name.c_str()), k == voiceIx, chipW)) {
      voiceIx = k, voice = vs[static_cast<size_t>(k)], renderStale = true;
      saveVoice(a);
    }
    ImGui::PopID();
  }
  // K-313: the pitch line: the voice's own, every word on one note (melodies with pitch marks), or random
  {
    ImGui::Dummy(ImVec2(0, theme::kS1));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Style"));
    using i18n::N_;
    const char* names[] = {N_("Natural"), N_("Sing (one note)"), N_("Random")};
    const char* tips[] = {N_("The voice's own rise and fall, with the pitch marks added"),
                          N_("Every word on the voice's pitch plus its mark: write a melody with the marks"),
                          N_("Each word a few semitones up or down")};
    const float lw = ImGui::CalcTextSize(i18n::tr("Style")).x + theme::kS2;
    const float sw = (W - lw - gap * 2) / 3;
    for (int k = 0; k < 3; ++k) {
      ImGui::SameLine(0, k ? gap : theme::kS2);
      ImGui::PushID(100 + k);
      if (ui::TextButton(i18n::tr(names[k]), static_cast<int>(style) == k, sw)) {
        style = static_cast<speech::Style>(k), renderStale = true;
        saveVoice(a);
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr(tips[k]));
      ImGui::PopID();
    }
  }
  ImGui::Dummy(ImVec2(0, theme::kS2));

  // Shape and Character: the voice's defaults are the knobs' defaults (double-click, right-click Reset)
  const speech::Voice& base = vs[static_cast<size_t>(voiceIx)];
  const ParamDesc dPitch{"Pitch", "Hz", 50, 400, base.pitch, ParamKind::Continuous, ParamScale::Log, {}};
  const ParamDesc dSpeed{"Speed", "times", 0.5, 2.0, base.speed, ParamKind::Continuous, ParamScale::Linear, {}};
  const ParamDesc dInto{"Intonation", "%", 0, 1, base.intonation, ParamKind::Continuous, ParamScale::Linear, {}};
  const ParamDesc dSize{"Voice size", "times", 0.75, 1.35, base.formant, ParamKind::Continuous, ParamScale::Linear, {}};
  const ParamDesc dBreath{"Breath", "%", 0, 1, base.breath, ParamKind::Continuous, ParamScale::Linear, {}};
  const ParamDesc dVib{"Vibrato", "ct", 0, 100, base.vibrato * 100.0, ParamKind::Continuous, ParamScale::Linear, {}};
  const ParamDesc dRobot{"Robot", "%", 0, 1, base.robot, ParamKind::Continuous, ParamScale::Linear, {}};
  const ParamDesc dRing{"Ring", "Hz", 10, 400, base.ring, ParamKind::Continuous, ParamScale::Log, {}};
  const float half = (W - theme::kS3) * 0.5f;
  bool changed = false;
  {
    const char* ids[] = {"##pitch", "##speed", "##into", "##size"};
    float* vals[] = {&voice.pitch, &voice.speed, &voice.intonation, &voice.formant};
    const ParamDesc* ds[] = {&dPitch, &dSpeed, &dInto, &dSize};
    const double sc[] = {1, 1, 1, 1};
    ImGui::BeginGroup();
    changed |= knobPanel(a, "Shape", half, ids, vals, ds, sc);
    ImGui::EndGroup();
  }
  ImGui::SameLine(0, theme::kS3);
  {
    const char* ids[] = {"##breath", "##vib", "##robot", "##ring"};
    float* vals[] = {&voice.breath, &voice.vibrato, &voice.robot, &voice.ring};
    const ParamDesc* ds[] = {&dBreath, &dVib, &dRobot, &dRing};
    const double sc[] = {1, 100, 1, 1};  // vibrato in cents on the knob, semitones in the voice
    ImGui::BeginGroup();
    changed |= knobPanel(a, "Character", half, ids, vals, ds, sc);
    ImGui::EndGroup();
  }
  if (changed) renderStale = true;
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && renderStale) saveVoice(a);
  ImGui::Dummy(ImVec2(0, theme::kS2));

  // The last preview
  caption(a, "Preview", W);
  waveform(a, W, 56);
  ImGui::Dummy(ImVec2(0, theme::kS2));

  // Actions: play / stop and reset on the left, Close and Add channel on the right
  if (!any) ImGui::BeginDisabled();
  const bool playing = previewPlaying(a);
  if (ui::IconButton("##speechplay", playing ? ui::Icon::Stop : ui::Icon::Play, playing, 28.0f, playing ? "Stop the preview" : "Preview (Enter)")) {
    if (playing) stopPreview(a);
    else preview(a);
  }
  if (!any) ImGui::EndDisabled();
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::tr("Reset to voice defaults"), false, 0, "The chosen voice's own settings")) {
    voice = base, renderStale = true;
    saveVoice(a);
  }
  const float bw = 110, bw2 = 120;
  ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + W - bw - bw2 - theme::kS2);
  if (ui::TextButton(i18n::tr("Close"), false, bw)) {
    stopPreview(a);
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine(0, theme::kS2);
  const bool add = ui::PrimaryButton(i18n::tr("Add channel"), bw2, "Adds a sampler channel playing the speech (Ctrl+Enter)", any);

  if (any && (add || modEnter)) {
    saveVoice(a);
    addChannel(a);
  } else if (any && enter) {
    preview(a);
  } else if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    stopPreview(a);
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace gl::speechui
