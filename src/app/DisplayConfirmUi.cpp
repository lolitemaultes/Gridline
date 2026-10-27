// Gridline - the "Keep this change?" dialog (J-77): styled like the
// unsaved-changes prompt (title, dim subtitle, right-aligned buttons), with a
// before / after row, a preview of both sides and the auto-revert countdown.
#include "DisplayConfirmUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Settings.h"
#include "App.h"
#include "Theme.h"
#include "ThemeSettings.h"
#include "Widgets.h"

namespace gl::ui {

using theme::pal;
using i18n::tr;

namespace {

constexpr float kDialogW = 460.0f;
constexpr float kAppear = 0.18f;  // s: fade-in of the dialog

// ---- previews

// A miniature window (title strip, a line of text, a button, a knob, meters)
// at `r` times the current interface size, top-left at `p`. J-137: with
// `style` >= 0 the knob and the meter are the real controls drawn in that
// control style (the palette in use must be P), so a control style change
// shows its two styles side by side.
void mockPanel(App& a, ImDrawList* dl, ImVec2 p, float r, const theme::Palette& P, int style = -1) {
  const float w = std::round(108 * r), h = std::round(70 * r);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), P.surface800, theme::kRadius * r);
  dl->AddRect(p, ImVec2(p.x + w, p.y + h), P.lineStrong, theme::kRadius * r);
  dl->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(p.x + w - 1, p.y + std::round(16 * r)), P.surface900, theme::kRadius * r,
                    ImDrawFlags_RoundCornersTop);
  dl->AddText(a.fontBold, theme::kFontCaption * r, ImVec2(p.x + 5 * r, p.y + 2 * r), P.text, tr("Mixer"));
  dl->AddText(a.fontRegular, theme::kFontBody * r, ImVec2(p.x + 6 * r, p.y + 20 * r), P.textDim, "Aa 120");
  const ImVec2 b0(p.x + 6 * r, p.y + 44 * r), b1(p.x + 52 * r, p.y + 62 * r);
  dl->AddRectFilled(b0, b1, P.surface700, theme::kRadius * r);
  dl->AddText(a.fontRegular, theme::kFontCaption * r, ImVec2(b0.x + 7 * r, b0.y + 2 * r), P.text, tr("Play"));
  const ImVec2 kc(p.x + 70 * r, p.y + 53 * r);
  if (style >= 0) {
    const ControlStyleScope scope(static_cast<ControlStyle>(style));
    DrawKnob(dl, kc, 9 * r, 0.62f, false, false, false);
    DrawMeter(dl, ImVec2(p.x + 86 * r, p.y + 24 * r), ImVec2(11 * r, 38 * r), 0.42f, 0.36f, 0.71f, 0.63f);
    return;
  }
  dl->AddCircleFilled(kc, 8 * r, P.surface950);
  dl->PathArcTo(kc, 8 * r, 2.4f, 5.4f, 16);
  dl->PathStroke(P.accent, 0, 2 * r);
  for (int i = 0; i < 3; ++i) {
    const float x = p.x + (86 + i * 6) * r, top = p.y + (26 + i * 9) * r;
    dl->AddRectFilled(ImVec2(x, top), ImVec2(x + 3 * r, p.y + 62 * r), P.meterLow, 1);
  }
}

// The control style of a settings snapshot.
ControlStyle styleOf(const dispconf::Snapshot& s) {
  Settings tmp;
  dispconf::restore(tmp, s);
  return theme::controlStyleFromSettings(tmp);
}

void scalePreview(App& a, const dispconf::Change& c) {
  const auto& P = pal();
  const float before = static_cast<float>(std::max(0.5, c.beforeValue)), after = static_cast<float>(c.afterValue);
  float rA = 1.0f, rB = after / before;  // drawn at the current size: "now" is 1:1
  const float colW = (kDialogW - theme::kS4) * 0.5f;
  const float fit = std::min(1.0f, (colW - 8) / (108.0f * std::max(rA, rB)));  // both shrink together when the larger does not fit
  rA *= fit, rB *= fit;
  const float boxH = std::round(70.0f * std::max(rA, rB)) + 4;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const char* caps[2] = {tr("Now"), tr("New")};
  const std::string vals[2] = {c.before, c.after};
  for (int k = 0; k < 2; ++k) {
    const float x = o.x + k * (colW + theme::kS4);
    dl->AddRectFilled(ImVec2(x, o.y), ImVec2(x + colW, o.y + boxH + 8), P.surface950, theme::kRadiusLg);
    const float r = k == 0 ? rA : rB;
    mockPanel(a, dl, ImVec2(std::round(x + (colW - 108 * r) * 0.5f), std::round(o.y + 4 + (boxH - 70 * r) * 0.5f)), r, P);
    char lbl[64];
    std::snprintf(lbl, sizeof lbl, "%s  %s", caps[k], vals[k].c_str());
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 2, o.y + boxH + 12), k == 1 ? P.text : P.textDim, lbl);
  }
  ImGui::Dummy(ImVec2(kDialogW, boxH + 12 + theme::kFontLabel + 4));
  if (fit < 0.999f) ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Previews shown smaller than life, at the same proportion."));
}

theme::Palette paletteOf(const dispconf::Snapshot& s) {
  Settings tmp;
  dispconf::restore(tmp, s);
  const theme::Mode m = theme::modeFromSettings(tmp);
  const bool dark = m == theme::Mode::System ? theme::isDark() : m == theme::Mode::Dark;
  return theme::makePalette(dark, theme::tweaksFromSettings(tmp));
}

void themePreview(App& a, const dispconf::Change& c) {
  const float colW = (kDialogW - theme::kS4) * 0.5f, boxH = 82;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const dispconf::Snapshot* snaps[2] = {&c.fromSnap, &c.toSnap};
  const char* caps[2] = {tr("Before"), tr("After")};
  for (int k = 0; k < 2; ++k) {
    const theme::Palette P = paletteOf(*snaps[k]);
    const float x = o.x + k * (colW + theme::kS4);
    dl->AddRectFilled(ImVec2(x, o.y), ImVec2(x + colW, o.y + boxH), P.surface950, theme::kRadiusLg);
    // J-137: the real controls in each side's control style when that side's colours are the ones
    // in use (a control style change); a colour change keeps the drawn miniature in its own colours.
    const theme::Palette& now = pal();
    const bool livePal = P.surface800 == now.surface800 && P.accent == now.accent && P.text == now.text && P.knobFill == now.knobFill &&
                         P.hot == now.hot && P.meterLow == now.meterLow && P.dark == now.dark;
    mockPanel(a, dl, ImVec2(std::round(x + (colW - 108) * 0.5f), o.y + 6), 1.0f, P, livePal ? static_cast<int>(styleOf(*snaps[k])) : -1);
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(x + 2, o.y + boxH + 4), pal().textDim, caps[k]);
  }
  ImGui::Dummy(ImVec2(kDialogW, boxH + theme::kFontLabel + 8));
}

void layoutPreview(const dispconf::Change&) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float w = 200, h = 96, x = o.x + (kDialogW - w) * 0.5f;
  auto box = [&](float x0, float y0, float x1, float y1, ImU32 c, const char* t) {
    dl->AddRectFilled(ImVec2(x + x0 * w, o.y + y0 * h), ImVec2(x + x1 * w - 2, o.y + y1 * h - 2), c, 2);
    dl->AddText(ImGui::GetFont(), theme::kFontLabel - 1, ImVec2(x + x0 * w + 4, o.y + y0 * h + 3), P.textDim, t);
  };
  dl->AddRectFilled(ImVec2(x - 4, o.y - 4), ImVec2(x + w + 4, o.y + h + 4), P.surface950, theme::kRadiusLg);
  box(0, 0, 0.18f, 1, P.surface800, tr("Browser"));
  box(0.18f, 0, 0.74f, 1, P.surface800, tr("Playlist"));
  box(0.74f, 0, 1, 1, P.surface800, tr("Mixer"));
  box(0.30f, 0.22f, 0.66f, 0.64f, P.surface700, tr("Channel rack"));
  ImGui::Dummy(ImVec2(kDialogW, h + 8));
}

std::string bilingual(const char* now, const std::string& before) {
  if (before.empty() || before == now) return now;
  return std::string(now) + "  \xC2\xB7  " + before;
}

}  // namespace

void confirmDisplayChange(App& a, dispconf::Change c) {
  dispconf::Confirm& cf = dispconf::confirm();
  cf.autoRevert = a.settings.getBool("ui.confirmAutoRevert");
  cf.request(std::move(c), ImGui::GetTime());
}

void drawDisplayConfirm(App& a) {
  dispconf::Confirm& cf = dispconf::confirm();
  cf.autoRevert = a.settings.getBool("ui.confirmAutoRevert");
  const double now = ImGui::GetTime();
  if (cf.tick(now) == dispconf::Result::TimedOut) a.notify(tr("Change reverted: it was not confirmed in time"));
  static double shownAt = 0;
  const bool isOpen = ImGui::IsPopupOpen("##dispconfirm");
  if (cf.pending() && !isOpen) {
    ImGui::OpenPopup("##dispconfirm");
    shownAt = now;
  }
  if (!cf.pending() && !isOpen) return;
  // Smooth entrance: the dialog (and the dimmed backdrop) fade in.
  const float appear = std::clamp(static_cast<float>((now - shownAt) / kAppear), 0.0f, 1.0f);
  const float ease = 1.0f - (1.0f - appear) * (1.0f - appear);
  ui::PlaceNextDialog(ImVec2(kDialogW + 2 * theme::kS6, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.15f + 0.85f * ease);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS6 - 4));
  ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg);
  dim.w *= ease;
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dim);
  const bool open = ImGui::BeginPopupModal("##dispconfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!open) return;
  const dispconf::Change* c = cf.current();
  if (!c) {  // resolved elsewhere (time-out, another change)
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const auto& P = pal();
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(tr(c->title.c_str()));
  ImGui::PopFont();
  if (!c->titleBefore.empty() && c->titleBefore != tr(c->title.c_str())) ImGui::TextColored(theme::vec(P.textDim), "%s", c->titleBefore.c_str());
  {  // "Interface size   100%  ->  150%"
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr(c->label.c_str()));
    ImGui::SameLine(0, theme::kS3);
    ImGui::PushFont(a.fontMono, theme::kFontBody);
    ImGui::TextColored(theme::vec(P.textDim), "%s", c->before.c_str());
    ImGui::SameLine(0, theme::kS2);
    ImGui::TextColored(theme::vec(P.textFaint), "\xE2\x86\x92");
    ImGui::SameLine(0, theme::kS2);
    ImGui::TextColored(theme::vec(P.text), "%s", c->after.c_str());
    ImGui::PopFont();
  }
  ImGui::Dummy(ImVec2(kDialogW, theme::kS2));
  switch (c->preview) {
    case dispconf::Preview::Scale: scalePreview(a, *c); break;
    case dispconf::Preview::Theme: themePreview(a, *c); break;
    case dispconf::Preview::Layout: layoutPreview(*c); break;
    default: break;
  }
  // Countdown: the time left, as words and as a thin draining bar.
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kDialogW);
  if (cf.autoRevert) {
    const double left = cf.remaining(now);
    const char* fmt = c->mode == dispconf::Mode::PreviewFirst ? tr("Not applied unless you keep it: closes by itself in %d s.")
                                                              : tr("Goes back to the earlier setting in %d s unless you keep it.");
    ImGui::TextColored(theme::vec(P.textDim), fmt, static_cast<int>(std::ceil(left)));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(q, ImVec2(q.x + kDialogW, q.y + 3), P.surface950, 1.5f);
    dl->AddRectFilled(q, ImVec2(q.x + kDialogW * static_cast<float>(left / std::max(0.1, cf.timeout)), q.y + 3), P.accent, 1.5f);
    ImGui::Dummy(ImVec2(kDialogW, 3));
  } else {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Automatic revert is off (Settings, Interface)."));
  }
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(kDialogW, theme::kS2));
  const std::string revertL = bilingual(tr("Revert"), c->revertBefore), keepL = bilingual(tr("Keep"), c->keepBefore);
  const float rw = std::max(96.0f, ImGui::CalcTextSize(revertL.c_str()).x + 2 * theme::kS4);
  const float kw = std::max(96.0f, ImGui::CalcTextSize(keepL.c_str()).x + 2 * theme::kS4);
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + kDialogW - rw - kw - theme::kS2));
  bool revert = ui::TextButton(revertL.c_str(), false, rw) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SameLine(0, theme::kS2);
  bool keep = ui::TextButton(keepL.c_str(), true, kw) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  if (revert) {
    cf.revert();
    ImGui::CloseCurrentPopup();
  } else if (keep) {
    cf.keep();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// ---- wired settings

void confirmScale(App& a, double to) { confirmDisplayChange(a, dispconf::scaleChange(a.settings, to)); }

void confirmLayoutReset(App& a) {
  dispconf::Change c;
  c.id = "ui.layout";
  c.title = i18n::N_("Reset the window layout?");
  c.label = i18n::N_("Windows");
  c.before = tr("Your arrangement");
  c.after = tr("Default layout");
  c.mode = dispconf::Mode::PreviewFirst;
  c.preview = dispconf::Preview::Layout;
  App* ap = &a;
  c.apply = [ap]() { ap->resetLayout = true; };
  confirmDisplayChange(a, std::move(c));
}

namespace {
std::string themeText(const dispconf::Snapshot& s) {
  Settings tmp;
  dispconf::restore(tmp, s);
  const theme::Mode m = theme::modeFromSettings(tmp);
  std::string t = m == theme::Mode::Dark ? tr("Dark") : m == theme::Mode::Light ? tr("Light") : tr("Follow system");
  const double c = tmp.getNumber("theme.contrast"), b = tmp.getNumber("theme.brightness");
  char buf[48];
  if (std::fabs(c) > 0.005) std::snprintf(buf, sizeof buf, ", %+.0f%% %s", c * 100, tr("contrast")), t += buf;
  if (std::fabs(b) > 0.005) std::snprintf(buf, sizeof buf, ", %+.0f%% %s", b * 100, tr("brightness")), t += buf;
  // J-137: and the control style (a style change read "Dark -> Dark").
  static const char* const kStyles[] = {i18n::N_("Classic"), i18n::N_("Graphite"), i18n::N_("Hairline"), i18n::N_("Lumen"), i18n::N_("Slate")};
  t += std::string(" \xC2\xB7 ") + tr(kStyles[static_cast<int>(theme::controlStyleFromSettings(tmp))]);
  return t;
}
}  // namespace

void confirmTheme(App& a, const dispconf::Snapshot& before, const char* what) {
  const dispconf::Snapshot after = dispconf::take(a.settings, dispconf::themeKeys());
  if (after == before) return;
  Settings* st = &a.settings;
  confirmDisplayChange(a, dispconf::snapshotChange(a.settings, "theme", i18n::N_("Keep this theme?"), what, themeText(before), themeText(after), before,
                                                   after, dispconf::Preview::Theme, [st]() { theme::applyFromSettings(*st); }));
}

void confirmLanguage(App& a, const std::string& code) {
  const std::string cur = a.settings.getString("general.language");
  if (cur == code) return;
  std::string curName = cur, newName = code;
  for (const auto& l : i18n::available(a.languageDir())) {
    if (l.code == cur) curName = l.name;
    if (l.code == code) newName = l.name;
  }
  const dispconf::Snapshot from = dispconf::take(a.settings, {{"general.language", 's'}});
  Settings tmp;
  tmp.set("general.language", code);
  const dispconf::Snapshot to = dispconf::take(tmp, {{"general.language", 's'}});
  App* ap = &a;
  dispconf::Change c = dispconf::snapshotChange(a.settings, "general.language", i18n::N_("Keep this language?"), i18n::N_("Display language"), curName, newName, from,
                                                to, dispconf::Preview::Language, [ap]() { ap->applySettingsHooks(); });
  // The texts in the language in use now: readable whichever language turns out unreadable.
  c.titleBefore = tr("Keep this language?");
  c.keepBefore = tr("Keep");
  c.revertBefore = tr("Revert");
  confirmDisplayChange(a, std::move(c));
}

namespace {
std::string audioText(const dispconf::Snapshot& s) {
  Settings tmp;
  dispconf::restore(tmp, s);
  char b[64];
  std::snprintf(b, sizeof b, "%.0f Hz \xC2\xB7 %.0f", tmp.getNumber("audio.sampleRate"), tmp.getNumber("audio.bufferFrames"));
  return b;
}
}  // namespace

void confirmAudio(App& a, const dispconf::Snapshot& before) {
  const dispconf::Snapshot after = dispconf::take(a.settings, dispconf::audioKeys());
  if (after == before) return;
  App* ap = &a;
  confirmDisplayChange(a, dispconf::snapshotChange(a.settings, "audio", i18n::N_("Keep these audio settings?"), i18n::N_("Audio output"), audioText(before),
                                                   audioText(after), before, after, dispconf::Preview::Audio, [ap]() {
                                                     ap->audioSampleRate = static_cast<int>(ap->settings.getNumber("audio.sampleRate"));
                                                     ap->audioBuffer = static_cast<int>(ap->settings.getNumber("audio.bufferFrames"));
                                                     ap->audioReopen = true;
                                                   }));
}

void confirmAudioDevice(App& a, const std::string& device) {
  if (device == a.audioDevice) return;
  dispconf::Change c;
  c.id = "audio.device";
  c.title = i18n::N_("Keep this audio device?");
  c.label = i18n::N_("Output device");
  c.before = a.audioDevice.empty() ? tr("System default") : a.audioDevice;
  c.after = device.empty() ? tr("System default") : device;
  c.mode = dispconf::Mode::ApplyFirst;
  c.preview = dispconf::Preview::Audio;
  App* ap = &a;
  const std::string old = a.audioDevice;
  c.apply = [ap, device]() { ap->audioDevice = device, ap->audioReopen = true; };
  c.revert = [ap, old]() { ap->audioDevice = old, ap->audioReopen = true; };
  c.revertQuiet = [ap, old]() { ap->audioDevice = old; };
  confirmDisplayChange(a, std::move(c));
}

}  // namespace gl::ui
