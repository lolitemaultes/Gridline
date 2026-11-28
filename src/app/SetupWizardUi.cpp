// Gridline - K-105: First-run setup wizard (see SetupWizardUi.h).
#include "SetupWizardUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutPreset.h"
#include "../core/Settings.h"
#include "../core/SetupTour.h"
#include "../engine/UiSounds.h"
#include "../host/HostedPlugins.h"
#include "../host/ui/QuietWindows.h"
#include "App.h"
#include "GuidedTourUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "SmoothScrollUi.h"
#include "Theme.h"
#include "ThemeSettings.h"
#include "Widgets.h"

namespace gl::setupwizard {

namespace {

using i18n::tr;
using setuptour::LookPreset;
using setuptour::LookSpec;
using setuptour::WizardStep;

struct WizardState {
  bool open = false;
  bool firstRun = false;
  int step = 0;
  bool scanPluginsNow = false;
  bool scanStartedHere = false;
  double testToneUntil = -1.0;
  ImVec2 lastPos{0, 0};
  ImVec2 lastSize{0, 0};
};

WizardState& st() {
  static WizardState s;
  return s;
}

bool quietOrNoScan() {
  if (quiet::on()) return true;
  if (const char* e = std::getenv("GRIDLINE_NO_PLUGIN_SCAN"); e && *e && *e != '0') return true;
  return false;
}

void saveSettingsNow(App& a) {
  a.settings.save(Settings::defaultPath());
}

void triggerTestTone(App& a) {
  // Synthesize a clean stereo two-note chime (A4 440 Hz + E5 659.25 Hz) with smooth envelope.
  const double rate = std::max(8000.0, static_cast<double>(a.audioSampleRate > 0 ? a.audioSampleRate : 44100));
  const double durSec = 0.48;
  const size_t nFrames = static_cast<size_t>(std::round(rate * durSec));
  uisound::Clip clip;
  clip.lr.resize(nFrames * 2, 0.0f);
  constexpr double kTwoPi = 6.28318530717958647692;
  const double f1 = 440.0;
  const double f2 = 659.255;
  for (size_t i = 0; i < nFrames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double attack = std::min(1.0, t / 0.012);
    const double decay = std::exp(-t * 6.5);
    const double env = attack * decay * 0.28;
    const double s1 = std::sin(kTwoPi * f1 * t);
    const double s2 = t > 0.06 ? std::sin(kTwoPi * f2 * (t - 0.06)) * std::min(1.0, (t - 0.06) / 0.01) * 0.7 : 0.0;
    const float sample = static_cast<float>((s1 + s2) * env);
    clip.lr[i * 2] = sample;
    clip.lr[i * 2 + 1] = sample;
  }
  a.uiSounds.setVolume(std::max(0.55f, static_cast<float>(a.settings.getNumber("sounds.volume"))));
  a.uiSounds.preview(std::move(clip), 0.85f);
  st().testToneUntil = a.now + 0.65;
}

void maybeStartPluginScan(App& a) {
  WizardState& s = st();
  if (!s.scanPluginsNow || s.scanStartedHere || quietOrNoScan() || a.embedded) return;
  if (!host::scanStatus().running) {
    host::startScan(host::selfExecutablePath());
    a.scanByUser = true;
    s.scanStartedHere = true;
  }
}

void finishWizard(App& a, bool markDone) {
  WizardState& s = st();
  maybeStartPluginScan(a);
  if (markDone) {
    setuptour::markSetupDone(a.settings);
    saveSettingsNow(a);
  }
  s.open = false;
}

// Crisp miniature preview of a theme LookPreset built purely from theme::makePalette.
void drawLookMiniPreview(ImDrawList* dl, ImVec2 p0, ImVec2 p1, LookPreset preset) {
  const LookSpec& sp = setuptour::lookSpec(preset);
  theme::Tweaks tw;
  tw.brightness = sp.brightness;
  tw.contrast = sp.contrast;
  tw.hue = sp.hue;
  tw.saturation = sp.saturation;
  tw.whiteText = sp.whiteText;
  const theme::Palette mp = theme::makePalette(sp.dark, tw);

  const float w = p1.x - p0.x;
  const float h = p1.y - p0.y;
  const float rad = theme::kRadiusLg;
  dl->AddRectFilled(p0, p1, mp.surface950, rad);

  // Top chrome bar
  const float topH = std::max(14.0f, std::round(h * 0.20f));
  dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + topH), mp.chrome, rad, ImDrawFlags_RoundCornersTop);
  dl->AddLine(ImVec2(p0.x, p0.y + topH), ImVec2(p1.x, p0.y + topH), mp.line);

  // Mini brand dot + transport pills on chrome bar
  const float cy = p0.y + topH * 0.5f;
  dl->AddRectFilled(ImVec2(p0.x + 8, cy - 4), ImVec2(p0.x + 16, cy + 4), mp.logo, 2.0f);
  const float tx = p0.x + w * 0.34f;
  dl->AddTriangleFilled(ImVec2(tx, cy - 4), ImVec2(tx, cy + 4), ImVec2(tx + 6, cy), theme::lit(mp.hot));
  dl->AddRectFilled(ImVec2(tx + 12, cy - 3), ImVec2(tx + 18, cy + 3), mp.textDim, 1.0f);
  dl->AddRectFilled(ImVec2(tx + 24, cy - 4), ImVec2(tx + 54, cy + 4), mp.field, 2.0f);
  dl->AddRectFilled(ImVec2(tx + 28, cy - 1.5f), ImVec2(tx + 46, cy + 1.5f), theme::lit(mp.hot), 1.0f);

  // Left browser column + right timeline well + right mixer column
  const float bodyY0 = p0.y + topH + 3.0f;
  const float bodyY1 = p1.y - 4.0f;
  const float bw = std::round(w * 0.24f);
  const float mw = std::round(w * 0.22f);
  const ImVec2 b0(p0.x + 4.0f, bodyY0), b1(p0.x + 4.0f + bw, bodyY1);
  const ImVec2 m0(p1.x - 4.0f - mw, bodyY0), m1(p1.x - 4.0f, bodyY1);
  const ImVec2 c0(b1.x + 3.0f, bodyY0), c1(m0.x - 3.0f, bodyY1);

  dl->AddRectFilled(b0, b1, mp.surface900, 2.0f);
  dl->AddRectFilled(c0, c1, mp.well, 2.0f);
  dl->AddRectFilled(m0, m1, mp.surface900, 2.0f);

  // Browser rows
  for (int r = 0; r < 5; ++r) {
    const float ry = b0.y + 6.0f + static_cast<float>(r) * 10.0f;
    if (ry + 5.0f > b1.y - 3.0f) break;
    if (r == 1) dl->AddRectFilled(ImVec2(b0.x + 2, ry - 2), ImVec2(b1.x - 2, ry + 6), mp.selection, 2.0f);
    dl->AddRectFilled(ImVec2(b0.x + 6, ry), ImVec2(b0.x + 6 + (bw - 14) * (r % 2 == 0 ? 0.72f : 0.52f), ry + 3.0f),
                      r == 1 ? mp.text : mp.textDim, 1.5f);
  }

  // Timeline clips & playhead
  const float cw = c1.x - c0.x;
  const float ch = c1.y - c0.y;
  for (int g = 1; g < 4; ++g) {
    const float gx = std::round(c0.x + cw * (static_cast<float>(g) / 4.0f));
    dl->AddLine(ImVec2(gx, c0.y), ImVec2(gx, c1.y), mp.gridBar);
  }
  const ImU32 clipCols[3] = {mp.accent, mp.led, mp.hot};
  for (int r = 0; r < 3; ++r) {
    const float ry = c0.y + 7.0f + static_cast<float>(r) * std::max(12.0f, ch * 0.28f);
    if (ry + 9.0f > c1.y - 3.0f) break;
    const float rx0 = c0.x + cw * (r == 1 ? 0.16f : 0.06f);
    const float rx1 = c0.x + cw * (r == 2 ? 0.88f : 0.64f);
    dl->AddRectFilled(ImVec2(rx0, ry), ImVec2(rx1, ry + 9.0f), theme::withAlpha(clipCols[r], 0.45f), 2.0f);
    dl->AddRectFilled(ImVec2(rx0, ry), ImVec2(rx1, ry + 3.0f), clipCols[r], 2.0f, ImDrawFlags_RoundCornersTop);
  }
  const float ph = std::round(c0.x + cw * 0.42f);
  dl->AddLine(ImVec2(ph, c0.y), ImVec2(ph, c1.y), mp.playhead, 1.5f);

  // Mini mixer meters
  const float stripW = (m1.x - m0.x - 10.0f) / 3.0f;
  const float lvls[3] = {0.72f, 0.50f, 0.62f};
  for (int s = 0; s < 3; ++s) {
    const float sx = m0.x + 4.0f + static_cast<float>(s) * (stripW + 1.0f);
    const float sh = std::max(10.0f, (m1.y - m0.y) - 12.0f);
    dl->AddRectFilled(ImVec2(sx, m0.y + 6.0f), ImVec2(sx + stripW - 2.0f, m0.y + 6.0f + sh), mp.field, 1.5f);
    const float fh = sh * lvls[s];
    dl->AddRectFilled(ImVec2(sx, m0.y + 6.0f + (sh - fh)), ImVec2(sx + stripW - 2.0f, m0.y + 6.0f + sh),
                      s == 0 ? mp.meterLow : mp.led, 1.5f);
  }

  dl->AddRect(p0, p1, mp.lineStrong, rad);
}

void drawStepPills(App& a, ImDrawList* dl, ImVec2 origin, float width) {
  const auto& P = theme::pal();
  const WizardState& s = st();
  static const char* const kLabels[setuptour::kWizardStepCount] = {
      i18n::N_("Workspace"), i18n::N_("Look"), i18n::N_("Audio"), i18n::N_("Start")};

  const int n = setuptour::kWizardStepCount;
  const float pillH = 28.0f;
  const float gap = 22.0f;
  const float pillW = std::floor((width - gap * static_cast<float>(n - 1)) / static_cast<float>(n));

  for (int i = 0; i < n; ++i) {
    const float x0 = origin.x + static_cast<float>(i) * (pillW + gap);
    const float y0 = origin.y;
    const ImVec2 p0(x0, y0), p1(x0 + pillW, y0 + pillH);

    // Connector line to next pill
    if (i + 1 < n) {
      const float ly = std::round(y0 + pillH * 0.5f);
      const ImU32 connCol = i < s.step ? P.accent : P.line;
      dl->AddLine(ImVec2(p1.x + 3.0f, ly), ImVec2(p1.x + gap - 3.0f, ly), connCol, i < s.step ? 2.0f : 1.0f);
    }

    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID(i);
    const bool clicked = ImGui::InvisibleButton("##step_pill", ImVec2(pillW, pillH));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (clicked) st().step = i;

    const bool active = (i == s.step);
    const bool done = (i < s.step);
    const ImU32 bg = active ? P.accent
                     : done ? theme::mix(P.surface800, P.accent, hov ? 0.28f : 0.16f)
                            : (hov ? P.surface700 : P.surface800);
    const ImU32 border = active ? P.accent : (done ? theme::withAlpha(P.accent, 0.55f) : (hov ? P.lineStrong : P.line));
    const ImU32 fg = active ? theme::contrastText(P.accent) : (done ? P.text : (hov ? P.text : P.textDim));

    dl->AddRectFilled(p0, p1, bg, pillH * 0.5f);
    dl->AddRect(p0, p1, border, pillH * 0.5f);

    // Step number / checkmark badge inside left of pill
    const float badgeR = 8.5f;
    const ImVec2 bc(p0.x + 15.0f, std::round(y0 + pillH * 0.5f));
    if (done) {
      dl->AddCircleFilled(bc, badgeR, P.led);
      ui::drawIcon(dl, ui::Icon::Check, bc, 10.0f, theme::contrastText(P.led));
    } else {
      const ImU32 badgeBg = active ? theme::withAlpha(theme::contrastText(P.accent), 0.22f) : P.surface700;
      dl->AddCircleFilled(bc, badgeR, badgeBg);
      char num[8];
      std::snprintf(num, sizeof num, "%d", i + 1);
      theme::centreText(dl, a.fontMono, theme::kFontLabel, ImVec2(bc.x - badgeR, bc.y - badgeR),
                        ImVec2(bc.x + badgeR, bc.y + badgeR), fg, num);
    }

    const char* lbl = tr(kLabels[i]);
    const float tx = p0.x + 30.0f;
    const float ty = theme::textTop(a.fontBold, theme::kFontCaption, y0, pillH);
    theme::textEllipsis(dl, active ? a.fontBold : a.fontRegular, theme::kFontCaption, ImVec2(tx, ty),
                        std::max(16.0f, pillW - 36.0f), fg, lbl);
  }
}

void drawStepWorkspace(App& a, float width) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextColored(theme::vec(P.text), "%s", tr("Choose your workspace layout"));
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     tr("Pick the arrangement that feels most natural. Your projects, sounds, and theme stay the same, and you can switch later from the View menu."));
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS3));

  // 2x2 grid of crisp visual mini-preview cards using layoutui::drawPreview (fits cleanly with no scrollbar).
  const float gap = theme::kS3;
  const float cardW = std::floor((width - gap) * 0.5f);
  const float thumbH = std::floor(cardW * 0.30f);
  const float cardH = thumbH + 54.0f;
  const layout::Preset cur = layoutui::active();
  const ImVec2 base = ImGui::GetCursorScreenPos();

  for (int i = 0; i < layout::kPresetCount; ++i) {
    const auto preset = static_cast<layout::Preset>(i);
    const int col = i % 2;
    const int row = i / 2;
    const ImVec2 p0(base.x + static_cast<float>(col) * (cardW + gap), base.y + static_cast<float>(row) * (cardH + gap));
    const ImVec2 p1(p0.x + cardW, p0.y + cardH);

    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID(i);
    const bool clicked = ImGui::InvisibleButton("##ws_card", ImVec2(cardW, cardH));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();

    if (clicked) layoutui::choose(a, preset, layout::linkedFromSettings(a.settings));

    const bool sel = (preset == cur);
    const ImU32 cardBg = sel ? theme::mix(P.surface800, P.accent, 0.10f) : (hov ? P.surface700 : P.surface800);
    const ImU32 border = sel ? P.accent : (hov ? P.lineStrong : P.line);
    dl->AddRectFilled(p0, p1, cardBg, theme::kRadiusLg + 2.0f);

    // Mini layout preview
    const ImVec2 t0(p0.x + 8.0f, p0.y + 8.0f);
    const ImVec2 t1(p1.x - 8.0f, p0.y + 8.0f + thumbH);
    layoutui::drawPreview(dl, t0, t1, preset);
    dl->AddRect(t0, t1, P.line, theme::kRadius);

    // Title + selected check pill
    const float textY = t1.y + 8.0f;
    const char* name = tr(layout::displayName(preset));
    dl->AddText(a.fontBold, theme::kFontBody, ImVec2(std::round(p0.x + 12.0f), std::round(textY)), P.text, name);
    if (sel) {
      const float br = 9.0f;
      const ImVec2 bc(p1.x - 18.0f, std::round(textY + 8.0f));
      dl->AddCircleFilled(bc, br, P.accent);
      ui::drawIcon(dl, ui::Icon::Check, bc, 11.0f, theme::contrastText(P.accent));
    }

    // Plain-English blurb (up to 2 crisp lines)
    const char* blurb = tr(layout::blurb(preset));
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(p0.x + 12.0f), std::round(textY + 19.0f)),
                P.textDim, blurb, nullptr, cardW - 24.0f);

    dl->AddRect(p0, p1, border, theme::kRadiusLg + 2.0f, 0, sel ? 2.0f : 1.0f);
  }
  ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + 2.0f * cardH + gap + theme::kS2));
  ImGui::Dummy(ImVec2(1, 1));
}

void drawStepLook(App& a, float width) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextColored(theme::vec(P.text), "%s", tr("Pick your look and theme"));
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     tr("Click any look below to preview it live across the entire studio window behind this dialog."));
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS3));

  const LookPreset activeLook = setuptour::detectLookPreset(a.settings);
  const float gap = theme::kS3;
  const float cardW = std::floor((width - gap) * 0.5f);
  const float thumbH = std::floor(cardW * 0.30f);
  const float cardH = thumbH + 52.0f;
  const ImVec2 base = ImGui::GetCursorScreenPos();

  for (int i = 0; i < setuptour::kLookPresetCount; ++i) {
    const auto preset = static_cast<LookPreset>(i);
    const LookSpec& sp = setuptour::lookSpec(preset);
    const int col = i % 2;
    const int row = i / 2;
    const ImVec2 p0(base.x + static_cast<float>(col) * (cardW + gap), base.y + static_cast<float>(row) * (cardH + gap));
    const ImVec2 p1(p0.x + cardW, p0.y + cardH);

    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID(100 + i);
    const bool clicked = ImGui::InvisibleButton("##look_card", ImVec2(cardW, cardH));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();

    if (clicked) {
      setuptour::applyLookToSettings(preset, a.settings);
      theme::applyFromSettings(a.settings, false);
    }

    const bool sel = (preset == activeLook);
    const ImU32 cardBg = sel ? theme::mix(P.surface800, P.accent, 0.10f) : (hov ? P.surface700 : P.surface800);
    const ImU32 border = sel ? P.accent : (hov ? P.lineStrong : P.line);
    dl->AddRectFilled(p0, p1, cardBg, theme::kRadiusLg + 2.0f);

    const ImVec2 t0(p0.x + 8.0f, p0.y + 8.0f);
    const ImVec2 t1(p1.x - 8.0f, p0.y + 8.0f + thumbH);
    drawLookMiniPreview(dl, t0, t1, preset);

    const float textY = t1.y + 8.0f;
    dl->AddText(a.fontBold, theme::kFontBody, ImVec2(std::round(p0.x + 12.0f), std::round(textY)), P.text, tr(sp.name));
    if (sel) {
      const float br = 9.0f;
      const ImVec2 bc(p1.x - 18.0f, std::round(textY + 8.0f));
      dl->AddCircleFilled(bc, br, P.accent);
      ui::drawIcon(dl, ui::Icon::Check, bc, 11.0f, theme::contrastText(P.accent));
    }
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round(p0.x + 12.0f), std::round(textY + 19.0f)),
                P.textDim, tr(sp.blurb), nullptr, cardW - 24.0f);

    dl->AddRect(p0, p1, border, theme::kRadiusLg + 2.0f, 0, sel ? 2.0f : 1.0f);
  }
  ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + 2.0f * cardH + gap + theme::kS2));

  // Quick white-text accessibility toggle on dark themes.
  if (P.dark) {
    bool whiteText = a.settings.getBool("theme.whiteText");
    if (ImGui::Checkbox(tr("High-contrast white text and icons"), &whiteText)) {
      a.settings.set("theme.whiteText", whiteText);
      theme::applyFromSettings(a.settings, false);
    }
  } else {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Accent colour, contrast and hue can be changed later in the theme settings."));
  }
}

void drawStepAudio(App& a, float width) {
  const auto& P = theme::pal();
  Settings& cfg = a.settings;

  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextColored(theme::vec(P.text), "%s", tr("Audio output and plugins"));
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     tr("Verify your audio device and latency, test sound output, and optionally scan installed third-party plugins."));
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS3));

  // Audio card
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface800));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.line));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusLg + 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  if (ImGui::BeginChild("##wiz_audio_card", ImVec2(width, 206.0f), ImGuiChildFlags_Borders)) {
    const float fieldW = std::min(340.0f, std::max(180.0f, width - 210.0f));
    const float labelX = 148.0f;

    ImGui::PushFont(a.fontMono, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("AUDIO OUTPUT"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, theme::kS1));

    // Output device
    ui::FormLabelFit(tr("Output device"), labelX, P.textDim);
    const std::string devLabel = a.audioDevice.empty() ? std::string(tr("System default")) : a.audioDevice;
    if (ui::BeginCombo("##wiz_dev", devLabel.c_str(), fieldW)) {
      if (ImGui::Selectable(tr("System default"), a.audioDevice.empty())) {
        cfg.set("audio.device", std::string());
        a.audioDevice.clear();
        a.audioReopen = true;
      }
      for (const auto& d : a.audioDevices) {
        if (ImGui::Selectable(d.c_str(), d == a.audioDevice)) {
          cfg.set("audio.device", d);
          a.audioDevice = d;
          a.audioReopen = true;
        }
      }
      ImGui::EndCombo();
    }

    // Sample rate
    static constexpr int kRates[] = {44100, 48000, 88200, 96000};
    ui::FormLabelFit(tr("Sample rate"), labelX, P.textDim);
    char curRate[32];
    std::snprintf(curRate, sizeof curRate, "%d Hz", a.audioSampleRate);
    if (ui::BeginCombo("##wiz_rate", curRate, 160.0f)) {
      for (int r : kRates) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d Hz", r);
        if (ImGui::Selectable(buf, r == a.audioSampleRate)) {
          cfg.set("audio.sampleRate", r);
          a.audioSampleRate = r;
          a.audioReopen = true;
        }
      }
      ImGui::EndCombo();
    }

    // Buffer size + latency readout
    static constexpr int kBufs[] = {64, 128, 256, 512, 1024, 2048};
    ui::FormLabelFit(tr("Buffer size"), labelX, P.textDim);
    char curBuf[32];
    std::snprintf(curBuf, sizeof curBuf, tr("%d samples"), a.audioBuffer);
    if (ui::BeginCombo("##wiz_buf", curBuf, 160.0f)) {
      for (int b : kBufs) {
        char buf[32];
        std::snprintf(buf, sizeof buf, tr("%d samples"), b);
        if (ImGui::Selectable(buf, b == a.audioBuffer)) {
          cfg.set("audio.bufferFrames", b);
          a.audioBuffer = b;
          a.audioReopen = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, theme::kS3);
    const double latMs = 1000.0 * static_cast<double>(a.audioBuffer) / static_cast<double>(std::max(1, a.audioSampleRate));
    ImGui::PushFont(a.fontMono, theme::kFontNumber);
    ImGui::TextColored(theme::vec(P.textDim), "%.1f ms", latMs);
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, theme::kS1));
    ui::FormLabelFit(tr("Sound check"), labelX, P.textDim);
    const bool playingTone = st().testToneUntil > a.now;
    if (ui::TextButton(playingTone ? "Playing test tone..." : "Test tone", playingTone, 0.0f,
                       "Play a short chime to confirm audio output works")) {
      triggerTestTone(a);
    }
    ImGui::SameLine(0, theme::kS3);
    if (!a.audioStatus.empty()) {
      ui::TextFit(a.audioStatus.c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.textFaint, true);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);

  ImGui::Dummy(ImVec2(0, theme::kS3));

  // Installed plugins card
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface800));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.line));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusLg + 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  if (ImGui::BeginChild("##wiz_plug_card", ImVec2(width, 94.0f), ImGuiChildFlags_Borders)) {
    ImGui::PushFont(a.fontMono, theme::kFontLabel);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("INSTALLED PLUGINS"));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, theme::kS1));

    const host::ScanStatus ss = host::scanStatus();
    if (ImGui::Checkbox(tr("Scan installed plugins now"), &st().scanPluginsNow)) {
      if (st().scanPluginsNow) maybeStartPluginScan(a);
    }
    if (ss.running) {
      ImGui::SameLine(0, theme::kS3);
      ui::StatusDot(P.accent, true);
      ImGui::SameLine(0, theme::kS1);
      ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Scanning in background..."));
    } else if (st().scanStartedHere) {
      ImGui::SameLine(0, theme::kS3);
      ui::StatusDot(P.led, false);
      ImGui::SameLine(0, theme::kS1);
      ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Scan complete"));
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(theme::vec(P.textFaint), "%s",
                       tr("It runs in the background. You can scan again later from the plugin manager."));
    ImGui::PopTextWrapPos();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

void drawStepReady(App& a, float width) {
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextColored(theme::vec(P.text), "%s", tr("Ready to make music"));
  ImGui::PopFont();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     tr("Everything is set up. Jump straight into the demo project with a quick interactive tour, or start fresh."));
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0, theme::kS3));

  // Crisp configuration summary strip
  {
    const LookSpec& lk = setuptour::lookSpec(setuptour::detectLookPreset(a.settings));
    char summary[256];
    std::snprintf(summary, sizeof summary, "%s  \xC2\xB7  %s  \xC2\xB7  %.1f kHz / %d",
                  tr(layout::displayName(layoutui::active())), tr(lk.name),
                  static_cast<double>(a.audioSampleRate) / 1000.0, a.audioBuffer);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = 34.0f;
    const ImVec2 p1(p0.x + width, p0.y + h);
    dl->AddRectFilled(p0, p1, P.surface800, theme::kRadiusLg);
    dl->AddRect(p0, p1, P.line, theme::kRadiusLg);
    ui::drawIcon(dl, ui::Icon::Check, ImVec2(p0.x + 18.0f, p0.y + h * 0.5f), 14.0f, P.led);
    const float ty = theme::textTop(a.fontMono, theme::kFontCaption, p0.y, h);
    theme::textEllipsis(dl, a.fontMono, theme::kFontCaption, ImVec2(p0.x + 34.0f, ty), width - 46.0f, P.text, summary);
    ImGui::Dummy(ImVec2(width, h + theme::kS3));
  }

  // Three crisp launch option cards
  struct ActionCard {
    ui::Icon icon;
    const char* title;
    const char* desc;
    bool primary;
  };
  const ActionCard cards[3] = {
      {ui::Icon::Play, i18n::N_("Open the demo and start the guided tour"),
       i18n::N_("Loads the built-in demo song and spotlights each workspace panel step by step."), true},
      {ui::Icon::NewFile, i18n::N_("Start empty project"),
       i18n::N_("Opens a clean blank project ready for your own instruments, beats, and recordings."), false},
      {ui::Icon::Check, i18n::N_("Finish"),
       i18n::N_("Closes this wizard and keeps your workspace exactly as it is right now."), false},
  };

  for (int i = 0; i < 3; ++i) {
    const float h = i == 0 ? 74.0f : 64.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + h);

    ImGui::PushID(200 + i);
    const bool clicked = ImGui::InvisibleButton("##ready_act", ImVec2(width, h));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();

    const ImU32 bg = cards[i].primary
                         ? theme::mix(P.surface800, P.accent, hov ? 0.24f : 0.14f)
                         : (hov ? P.surface700 : P.surface800);
    const ImU32 border = cards[i].primary ? P.accent : (hov ? P.lineStrong : P.line);
    dl->AddRectFilled(p0, p1, bg, theme::kRadiusLg + 2.0f);
    dl->AddRect(p0, p1, border, theme::kRadiusLg + 2.0f, 0, cards[i].primary ? 1.8f : 1.0f);

    // Icon badge
    const float badgeSz = 36.0f;
    const ImVec2 ic(p0.x + 14.0f + badgeSz * 0.5f, p0.y + h * 0.5f);
    const ImU32 badgeBg = cards[i].primary ? P.accent : P.surface700;
    const ImU32 iconCol = cards[i].primary ? theme::contrastText(P.accent) : P.text;
    dl->AddRectFilled(ImVec2(ic.x - badgeSz * 0.5f, ic.y - badgeSz * 0.5f),
                      ImVec2(ic.x + badgeSz * 0.5f, ic.y + badgeSz * 0.5f), badgeBg, theme::kRadiusLg);
    ui::drawIcon(dl, cards[i].icon, ic, 18.0f, iconCol);

    const float tx = p0.x + 64.0f;
    const float maxTw = width - 98.0f;
    const float titleY = p0.y + (h - 36.0f) * 0.5f;
    theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(tx, titleY), maxTw, P.text, tr(cards[i].title));
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(tx, titleY + 20.0f), maxTw, P.textDim, tr(cards[i].desc));
    ui::drawIcon(dl, ui::Icon::Chevron, ImVec2(p1.x - 20.0f, p0.y + h * 0.5f), 14.0f,
                 cards[i].primary ? P.accent : P.textDim);

    if (clicked) {
      finishWizard(a, true);
      // Reopened from Help with unsaved work, both ask first (the same question as File > New).
      if (i == 0) {
        a.guardUnsaved([&a]() {
          a.home.projectOpened(a.now);
          a.newFromTemplate("demo");
          guidedtour::start(a, false);
        });
      } else if (i == 1) {
        a.guardUnsaved([&a]() {
          a.home.projectOpened(a.now);
          a.newFromTemplate("");
        });
      }
      return;
    }
    ImGui::Dummy(ImVec2(0, theme::kS2));
  }
}

}  // namespace

void open(App& a, bool firstRun) {
  WizardState& s = st();
  s.open = true;
  s.firstRun = firstRun;
  s.step = 0;
  s.scanStartedHere = false;
  s.scanPluginsNow = false;
  s.testToneUntil = -1.0;
  // Close guided tour if running so they never overlap visually.
  if (guidedtour::active()) guidedtour::stop();
  (void)a;
}

void close(App& a, bool markDone) { finishWizard(a, markDone); }

bool visible() { return st().open; }

int currentStep() { return st().step; }

void setStep(App& a, int stepIndex) {
  if (!st().open) open(a, false);
  st().step = std::clamp(stepIndex, 0, setuptour::kWizardStepCount - 1);
}

void draw(App& a) {
  WizardState& s = st();
  if (!s.open) return;

  const auto& P = theme::pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float winW = std::min(756.0f, std::max(520.0f, vp->WorkSize.x - 64.0f));
  const float winH = std::min(626.0f, std::max(460.0f, vp->WorkSize.y - 48.0f));
  const ImVec2 pos(std::round(vp->WorkPos.x + (vp->WorkSize.x - winW) * 0.5f),
                   std::round(vp->WorkPos.y + (vp->WorkSize.y - winH) * 0.5f));
  s.lastPos = pos;
  s.lastSize = ImVec2(winW, winH);

  // Subtle translucent backdrop sitting above workspace windows and below the wizard card
  // (lighter on Step 2 so the live theme preview shines through clearly).
  const float dimAlpha = s.step == static_cast<int>(WizardStep::Look) ? (P.dark ? 0.34f : 0.20f) : (P.dark ? 0.54f : 0.32f);
  ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(theme::withAlpha(P.ink, dimAlpha)));
  if (ImGui::Begin("##setup_wizard_backdrop", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);

  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::kRadiusLg + 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface900));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.lineStrong));

  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("##setup_wizard", nullptr, flags)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float innerW = ImGui::GetContentRegionAvail().x;
    const ImVec2 topPos = ImGui::GetCursorScreenPos();

    // Soft top accent bar
    dl->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y + 1.0f), ImVec2(pos.x + winW - 1.0f, pos.y + 4.0f), P.accent,
                      theme::kRadiusLg + 5.0f, ImDrawFlags_RoundCornersTop);

    // Header: Gridline mark + Title + Skip button
    theme::drawMark(dl, ImVec2(topPos.x, topPos.y + 2.0f), 26.0f, P.logo, P.surface900);
    const char* title = s.firstRun ? tr("Welcome to Gridline") : tr("Gridline setup wizard");
    dl->AddText(a.fontBold, 19.0f, ImVec2(std::round(topPos.x + 36.0f), std::round(topPos.y + 4.0f)), P.text, title);

    const float skipW = ui::TextButtonWidth("Skip");
    ImGui::SetCursorScreenPos(ImVec2(topPos.x + innerW - skipW, topPos.y + 2.0f));
    if (ui::TextButton("Skip", false, skipW, "Close the setup wizard and keep your current settings")) {
      finishWizard(a, true);
      ImGui::End();
      ImGui::PopStyleColor(2);
      ImGui::PopStyleVar(3);
      return;
    }

    // 4-step indicator pills
    const ImVec2 pillsPos(topPos.x, topPos.y + 40.0f);
    drawStepPills(a, dl, pillsPos, innerW);
    const float divY = pillsPos.y + 38.0f;
    dl->AddLine(ImVec2(topPos.x, divY), ImVec2(topPos.x + innerW, divY), P.line);

    // Step content body
    const float footerH = 44.0f;
    const float bodyY = divY + 14.0f;
    const float bodyH = std::max(140.0f, (pos.y + winH - 20.0f - footerH) - bodyY);
    ImGui::SetCursorScreenPos(ImVec2(topPos.x, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(theme::withAlpha(P.surface900, 0.0f)));
    if (ImGui::BeginChild("##wiz_body", ImVec2(innerW, bodyH), ImGuiChildFlags_None)) {
      const float bw = ImGui::GetContentRegionAvail().x;
      switch (static_cast<WizardStep>(s.step)) {
        case WizardStep::Workspace: drawStepWorkspace(a, bw); break;
        case WizardStep::Look: drawStepLook(a, bw); break;
        case WizardStep::Audio: drawStepAudio(a, bw); break;
        case WizardStep::Ready: drawStepReady(a, bw); break;
        default: break;
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Footer bar
    if (s.open) {
      const float footY = pos.y + winH - 20.0f - footerH;
      dl->AddLine(ImVec2(topPos.x, footY), ImVec2(topPos.x + innerW, footY), P.line);

      const float btnY = footY + 12.0f;
      if (s.step > 0) {
        ImGui::SetCursorScreenPos(ImVec2(topPos.x, btnY));
        if (ui::TextButton("Back", false, 84.0f)) s.step = std::max(0, s.step - 1);
      }

      char stepNote[48];
      std::snprintf(stepNote, sizeof stepNote, tr("Step %d of %d"), s.step + 1, setuptour::kWizardStepCount);
      const ImVec2 noteSz = a.fontMono->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, stepNote);
      dl->AddText(a.fontMono, theme::kFontCaption,
                  ImVec2(std::round(topPos.x + (innerW - noteSz.x) * 0.5f), std::round(btnY + 6.0f)), P.textFaint,
                  stepNote);

      const float nextW = 104.0f;
      ImGui::SetCursorScreenPos(ImVec2(topPos.x + innerW - nextW, btnY));
      if (s.step < setuptour::kWizardStepCount - 1) {
        if (ui::PrimaryButton("Next", nextW)) ++s.step;
      } else {
        if (ui::TextButton("Close", false, nextW)) finishWizard(a, true);
      }
    }

    // Esc dismisses cleanly when no popup is open inside the wizard.
    if (s.open && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        keys::escapePressed()) {
      finishWizard(a, true);
    }
  }
  ImGui::End();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
}

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  const std::string sub = args.empty() ? std::string("state") : args[0];
  if (sub == "open") {
    open(a, false);
  } else if (sub == "step" && args.size() >= 2) {
    const int n = std::atoi(args[1].c_str());
    const int idx = n <= 0 ? 0 : n - 1;  // 1-based step number (1..4), 0 -> 1
    setStep(a, idx);
  } else if (sub == "next") {
    if (!st().open) open(a, false);
    else st().step = std::min(setuptour::kWizardStepCount - 1, st().step + 1);
  } else if (sub == "back") {
    if (st().open) st().step = std::max(0, st().step - 1);
  } else if (sub == "close") {
    close(a, true);
  } else if (sub == "look" && args.size() >= 2) {
    const std::string& id = args[1];
    LookPreset lp = LookPreset::Dark;
    if (id == "light") lp = LookPreset::Light;
    else if (id == "night") lp = LookPreset::Night;
    else if (id == "tinted") lp = LookPreset::Tinted;
    setuptour::applyLookToSettings(lp, a.settings);
    theme::applyFromSettings(a.settings, false);
  } else if (sub == "tone") {
    triggerTestTone(a);
  }
  const WizardState& s = st();
  std::printf("wizard frame %d: open %d step %d/%d layout %s look %s setupDone %d rect %.0f,%.0f,%.0f,%.0f\n", frame,
              s.open ? 1 : 0, s.step + 1, setuptour::kWizardStepCount, layout::id(layoutui::active()),
              setuptour::lookSpec(setuptour::detectLookPreset(a.settings)).id,
              setuptour::isSetupDone(a.settings) ? 1 : 0, static_cast<double>(s.lastPos.x),
              static_cast<double>(s.lastPos.y), static_cast<double>(s.lastSize.x), static_cast<double>(s.lastSize.y));
  std::fflush(stdout);
}

}  // namespace gl::setupwizard
