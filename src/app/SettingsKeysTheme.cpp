// Gridline - J-55 settings pages: the keyboard shortcut editor (every command
// of the central keymap, searchable, rebind with conflict check, reset) and
// the theme page (dark / light / follow system, accent and role colours,
// contrast, brightness, live preview). Also the read-only shortcut reference
// shown by Help > Keyboard shortcuts (F1).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "DisplayConfirmUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Keymap.h"
#include "App.h"
#include "Keys.h"
#include "SettingsPages.h"
#include "Theme.h"
#include "ThemeSettings.h"
#include "Widgets.h"

namespace gl {

using i18n::tr;
using theme::pal;

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void label(const char* text, float x = 168.0f) { ui::FormLabelFit(tr(text), x, pal().textDim); }

// A key cap: mono text on a raised chip. Returns its width.
float keyChip(App& a, ImDrawList* dl, ImVec2 q, const char* text, ImU32 edge) {
  const auto& P = pal();
  ImVec2 ts = a.fontMono->CalcTextSizeA(theme::kFontNumber, 1e9f, 0, text);
  const float w = ts.x + 12;
  dl->AddRectFilled(q, ImVec2(q.x + w, q.y + 20), P.surface800, theme::kRadiusSm);
  dl->AddRect(q, ImVec2(q.x + w, q.y + 20), edge, theme::kRadiusSm);
  dl->AddText(a.fontMono, theme::kFontNumber, ImVec2(q.x + 6, q.y + 3), P.text, text);
  return w;
}

void saveKeymap(App& a) { a.settings.set("keys.bindings", keys::keymap().serialize()); }

std::string commandName(const keys::Command& c) {
  return std::string(tr(c.label)) + " (" + tr(keys::contextName(c.context)) + ")";
}

struct KeyEditor {
  char search[96] = "";
  int context = -1;          // -1 = all
  std::string captureId;     // command waiting for a key
  bool captureAdd = false;   // add as an alternative instead of replacing
  std::string pendingId;     // conflict question open for this command
  keys::Chord pendingChord;
  bool pendingAdd = false;
  std::string message;       // last result (shadow notes, what was unbound)
};
KeyEditor gKE;

void commitChord(App& a, const std::string& id, keys::Chord c, bool add) {
  auto& km = keys::keymap();
  std::vector<std::string> lost = km.assign(id, c, !add);
  saveKeymap(a);
  const keys::Command* cmd = keys::findCommand(id);
  std::string msg = keys::format(c) + " " + tr("now runs") + " " + (cmd ? commandName(*cmd) : id) + ".";
  for (const auto& l : lost)
    if (const keys::Command* o = keys::findCommand(l)) msg += " " + std::string(tr("Removed from")) + " " + tr(o->label) + ".";
  for (const keys::Command* s : km.shadows(id, c))
    msg += " " + std::string(tr("Also used by")) + " " + commandName(*s) + ": " + tr("the editor's own shortcut wins while it has focus.");
  gKE.message = msg;
}

}  // namespace

// ----------------------------------------------------------- shortcuts ----
void shortcutSettingsPage(App& a, float fieldW) {
  const auto& P = pal();
  auto& km = keys::keymap();
  KeyEditor& E = gKE;

  // Capture: the next key (with its modifiers) becomes the binding; Esc cancels.
  if (!E.captureId.empty()) {
    keys::setCapturing(true);
    keys::Chord c;
    if (keys::captureChord(&c)) {
      const std::string id = E.captureId;
      E.captureId.clear();
      keys::setCapturing(false);
      if (c.key == keys::Key::Escape && c.mods == 0) {
        E.message = tr("No change.");
      } else if (km.conflicts(id, c).empty()) {
        commitChord(a, id, c, E.captureAdd);
      } else {
        E.pendingId = id, E.pendingChord = c, E.pendingAdd = E.captureAdd;
      }
    }
  }

  // Toolbar: search, context filter, reset all.
  ImGui::SetNextItemWidth(std::min(fieldW, 260.0f));
  ImGui::InputTextWithHint("##keysearch", tr("Search commands or keys"), E.search, sizeof E.search);
  ui::FocusRing();
  ImGui::SameLine(0, theme::kS2);
  const char* ctxPreview = E.context < 0 ? tr("All windows") : tr(keys::contextName(static_cast<keys::Context>(E.context)));
  if (ui::BeginCombo("##keyctx", ctxPreview, 180)) {
    if (ImGui::Selectable(tr("All windows"), E.context < 0)) E.context = -1;
    for (int i = 0; i < static_cast<int>(keys::Context::Count); ++i)
      if (ImGui::Selectable(tr(keys::contextName(static_cast<keys::Context>(i))), E.context == i)) E.context = i;
    ImGui::EndCombo();
  }
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(!km.anyCustomized());
  if (ui::TextButton(tr("Reset all"), false, 0, tr("Every shortcut back to the default map"))) {
    km.resetAll();
    saveKeymap(a);
    E.message = tr("All shortcuts are back to their defaults.");
  }
  ImGui::EndDisabled();

  // Conflict question / last message.
  if (!E.pendingId.empty()) {
    const keys::Command* cmd = keys::findCommand(E.pendingId);
    std::string who;
    for (const keys::Command* o : km.conflicts(E.pendingId, E.pendingChord)) who += (who.empty() ? "" : ", ") + std::string(tr(o->label));
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.danger), "%s %s %s. %s %s?", keys::format(E.pendingChord).c_str(), tr("is already used by"), who.c_str(),
                       tr("Use it for"), cmd ? tr(cmd->label) : E.pendingId.c_str());
    ImGui::PopTextWrapPos();
    if (ui::TextButton(tr("Reassign"), true, 0, tr("Takes the shortcut away from the other command"))) {
      commitChord(a, E.pendingId, E.pendingChord, E.pendingAdd);
      E.pendingId.clear();
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton(tr("Cancel"), false, 0)) {
      E.pendingId.clear();
      E.message = tr("No change.");
    }
  } else if (!E.captureId.empty()) {
    const keys::Command* cmd = keys::findCommand(E.captureId);
    ImGui::TextColored(theme::vec(P.hot), "%s %s  (%s)", tr("Press the new shortcut for"), cmd ? tr(cmd->label) : "", tr("Esc cancels"));
  } else {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textDim), "%s",
                       E.message.empty() ? tr("Shortcuts act in the window shown in brackets. A window's own shortcut wins over a global one while that window has focus.")
                                         : E.message.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));

  // The command table.
  const std::string q = lower(E.search);
  // Action buttons as wide as their longest (translated) label.
  float bw = 58.0f;
  for (const char* l : {"Change", "Add", "Clear", "Default"})
    bw = std::max(bw, std::ceil(ImGui::CalcTextSize(tr(l)).x + 2 * ImGui::GetStyle().FramePadding.x));
  const float actionsW = 4 * bw + 3 * theme::kS1;
  ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("##keymap", 3, tf, ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y - 4)))) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn(tr("Command"), ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn(tr("Shortcut"), ImGuiTableColumnFlags_WidthFixed, 190.0f);
  ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, actionsW);
  ImGui::TableHeadersRow();
  int lastCtx = -1;
  for (const auto& c : keys::commands()) {
    const int ctx = static_cast<int>(c.context);
    if (E.context >= 0 && ctx != E.context) continue;
    const std::string chords = keys::labels(c.id);
    if (!q.empty()) {
      const std::string hay = lower(std::string(tr(c.label)) + " " + c.label + " " + c.id + " " + chords + " " + tr(keys::contextName(c.context)));
      if (hay.find(q) == std::string::npos) continue;
    }
    if (ctx != lastCtx) {
      lastCtx = ctx;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      std::string up = tr(keys::contextName(c.context));
      for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      ImGui::Dummy(ImVec2(0, 2));
      ui::Label(up.c_str());
    }
    ImGui::PushID(c.id);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, 24.0f);
    ImGui::TableNextColumn();
    const bool custom = km.customized(c.id);
    ImGui::AlignTextToFramePadding();
    ui::TextFit(tr(c.label), ImGui::GetContentRegionAvail().x, custom ? P.hot : P.text, false);
    ImGui::TableNextColumn();
    {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImVec2 q0 = ImGui::GetCursorScreenPos();
      q0.y += 2;
      const float maxX = q0.x + ImGui::GetContentRegionAvail().x;
      const bool capturingThis = E.captureId == c.id;
      if (capturingThis) {
        keyChip(a, dl, q0, E.captureAdd ? "+ ..." : "...", P.hot);
      } else if (km.bindings(c.id).empty()) {
        dl->AddText(ImVec2(q0.x, q0.y + 3), P.textFaint, tr("none"));
      } else {
        float x = q0.x;
        for (const auto& ch : km.bindings(c.id)) {
          const std::string t = keys::format(ch);
          if (x + 40 > maxX) break;
          x += keyChip(a, dl, ImVec2(x, q0.y), t.c_str(), custom ? P.hot : P.line) + theme::kS1;
        }
      }
      ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 22));
      if (ImGui::IsItemHovered() && !chords.empty()) {
        std::string tip = chords;
        for (const auto& ch : km.bindings(c.id))
          for (const keys::Command* s : km.shadows(c.id, ch)) tip += "\n" + keys::format(ch) + ": " + std::string(tr("also")) + " " + commandName(*s);
        ui::Tooltip(tip.c_str());
      }
    }
    ImGui::TableNextColumn();
    if (ui::TextButton("Change", E.captureId == c.id && !E.captureAdd, bw, tr("Press the new key combination next"))) {
      E.captureId = c.id, E.captureAdd = false, E.pendingId.clear();
    }
    ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton("Add", E.captureId == c.id && E.captureAdd, bw, tr("Adds a second shortcut for this command"))) {
      E.captureId = c.id, E.captureAdd = true, E.pendingId.clear();
    }
    ImGui::SameLine(0, theme::kS1);
    ImGui::BeginDisabled(km.bindings(c.id).empty());
    if (ui::TextButton("Clear", false, bw, tr("No shortcut for this command"))) {
      km.set(c.id, {});
      saveKeymap(a);
      E.message = std::string(tr(c.label)) + ": " + tr("no shortcut.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, theme::kS1);
    ImGui::BeginDisabled(!custom);
    if (ui::TextButton("Default", false, bw, tr("Back to the default shortcut"))) {
      // The default chord may be in use elsewhere by now: take it back from there.
      km.reset(c.id);
      for (const auto& ch : c.defaults)
        for (const keys::Command* o : km.conflicts(c.id, ch)) {
          std::vector<keys::Chord> b = km.bindings(o->id);
          b.erase(std::remove(b.begin(), b.end(), ch), b.end());
          km.set(o->id, b);
        }
      saveKeymap(a);
      E.message = std::string(tr(c.label)) + ": " + tr("default shortcut restored.");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
  ImGui::EndTable();
}

// Help > Keyboard shortcuts: the live map (read-only) plus mouse gestures.
void shortcutReference(App& a) {
  const auto& P = pal();
  if (ui::TextButton(tr("Change shortcuts..."), false, 0, tr("Settings > Shortcuts"))) {
    a.showSettings = true;
    a.settingsTab = 8;
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const float keyCol = std::min(200.0f, ImGui::GetContentRegionAvail().x * 0.42f);
  auto row = [&](const char* keysText, const char* what) {
    ImVec2 q = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* fitted = theme::fitText(keysText, keyCol - 16);
    keyChip(a, dl, q, fitted, P.line);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption + 1, ImVec2(q.x + keyCol, q.y + 3), ImGui::GetContentRegionAvail().x - keyCol, P.textDim,
                        what);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 22));
  };
  auto heading = [&](const char* text) {
    std::string up = text;
    for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    ImGui::Dummy(ImVec2(0, theme::kS2));
    ui::Label(up.c_str());
  };
  int lastCtx = -1;
  for (const auto& c : keys::commands()) {
    const std::string k = keys::labels(c.id);
    if (k.empty()) continue;
    if (static_cast<int>(c.context) != lastCtx) {
      lastCtx = static_cast<int>(c.context);
      heading(tr(keys::contextName(c.context)));
    }
    row(k.c_str(), tr(c.label));
  }
  heading(tr("Mouse"));
  row(tr("Ctrl+wheel / Alt+wheel"), tr("Zoom / row height"));
  row(tr("Alt+drag"), tr("Bypass snap"));
  row(tr("Right-click"), tr("Delete note"));
  row(tr("Double-click clip"), tr("Edit its pattern"));
  heading(tr("Typing keyboard"));
  row("Z S X D C V G B H N J M", tr("Lower octave (C to B)"));
  row("Q 2 W 3 E R 5 T 6 Y 7 U", tr("Upper octave"));
  heading(tr("Knobs"));
  row(tr("Drag / Shift+drag"), tr("Change / fine"));
  row(tr("Double-click"), tr("Reset"));
  row(tr("Right-click"), tr("Type a value"));
}

// --------------------------------------------------------------- theme ----
namespace {

void applyTheme(App& a) { theme::applyFromSettings(a.settings); }

// One role colour: swatch (picker), the theme default when unset, a Default button.
bool roleColour(const char* id, const char* name, const char* hint, uint32_t* value, uint32_t themeDefault) {
  bool changed = false;
  label(name);
  ImGui::PushID(id);
  const uint32_t shown = *value == theme::Tweaks::kKeep ? themeDefault : *value;
  float f[3] = {((shown >> 16) & 0xFF) / 255.f, ((shown >> 8) & 0xFF) / 255.f, (shown & 0xFF) / 255.f};
  if (ImGui::ColorEdit3("##c", f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha)) {
    *value = (static_cast<uint32_t>(f[0] * 255 + 0.5f) << 16) | (static_cast<uint32_t>(f[1] * 255 + 0.5f) << 8) | static_cast<uint32_t>(f[2] * 255 + 0.5f);
    changed = true;
  }
  ui::Hint(tr(hint));
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(*value == theme::Tweaks::kKeep);
  if (ui::TextButton("Default", false, 0, "The theme's own colour")) {
    *value = theme::Tweaks::kKeep;
    changed = true;
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, theme::kS2);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", *value == theme::Tweaks::kKeep ? tr("theme default") : theme::colourText(*value).c_str());
  ImGui::PopID();
  return changed;
}

uint32_t rgbOf(ImU32 c) { return ((c >> IM_COL32_R_SHIFT) & 0xFF) << 16 | ((c >> IM_COL32_G_SHIFT) & 0xFF) << 8 | ((c >> IM_COL32_B_SHIFT) & 0xFF); }

// A small studio scene drawn with the live palette: toolbar with lit tools,
// step pads, LEDs, an LCD field, a grid with a selection and the song position.
void themePreview(App& a) {
  const auto& P = pal();
  const float w = ImGui::GetContentRegionAvail().x, h = 132.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), P.surface900, theme::kRadiusLg);
  dl->AddRect(o, ImVec2(o.x + w, o.y + h), P.line, theme::kRadiusLg);
  // Toolbar strip.
  dl->AddRectFilled(ImVec2(o.x + 1, o.y + 1), ImVec2(o.x + w - 1, o.y + 30), P.chrome, theme::kRadiusLg, ImDrawFlags_RoundCornersTop);
  const ui::Icon icons[] = {ui::Icon::ToolDraw, ui::Icon::ToolPaint, ui::Icon::ToolDelete, ui::Icon::ToolSelect, ui::Icon::ToolZoom};
  for (int i = 0; i < 5; ++i) {
    const ImVec2 c(o.x + 18 + i * 26.0f, o.y + 15);
    if (i == 0) dl->AddRectFilled(ImVec2(c.x - 11, c.y - 11), ImVec2(c.x + 11, c.y + 11), P.surface700, theme::kRadius);
    ui::drawIcon(dl, icons[i], c, 14, i == 0 ? theme::lit(P.hot) : P.textDim);
  }
  // LCD field.
  const ImVec2 f0(o.x + 156, o.y + 5), f1(o.x + 236, o.y + 25);
  dl->AddRectFilled(f0, f1, P.field, theme::kRadiusSm);
  dl->AddText(a.fontMono, theme::kFontNumber + 1, ImVec2(f0.x + 8, f0.y + 3), P.hot, "140.000");
  // LEDs.
  for (int i = 0; i < 3; ++i) ui::Led(dl, ImVec2(o.x + 256 + i * 16.0f, o.y + 15), 4.5f, i != 1);
  // Text levels.
  {
    float tx = o.x + 310;
    const struct { const char* t; ImU32 c; } levels[] = {{tr("Text"), P.text}, {tr("Caption"), P.textDim}, {tr("Hint"), P.textFaint}};
    for (const auto& l : levels) {
      dl->AddText(ImVec2(tx, o.y + 7), l.c, l.t);
      tx += ImGui::CalcTextSize(l.t).x + theme::kS3;
    }
  }
  // Step pads.
  for (int i = 0; i < 16; ++i) {
    const float x = o.x + 12 + i * 20.0f;
    if (x + 18 > o.x + w * 0.5f) break;
    ui::StepPad(dl, ImVec2(x, o.y + 42), ImVec2(x + 17, o.y + 62), i % 4 == 0 || i == 6, (i / 4) % 2 == 1, false, 0, 1.0f);
  }
  // Grid well with a selection and the song position.
  const ImVec2 g0(o.x + 12, o.y + 72), g1(o.x + w - 12, o.y + h - 10);
  dl->AddRectFilled(g0, g1, P.well, theme::kRadiusSm);
  for (int i = 0; i <= 16; ++i) {
    const float x = g0.x + (g1.x - g0.x) * i / 16.0f;
    dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), i % 4 == 0 ? P.gridBar : P.gridBeat);
  }
  const float sx0 = g0.x + (g1.x - g0.x) * 0.25f, sx1 = g0.x + (g1.x - g0.x) * 0.5f;
  dl->AddRectFilled(ImVec2(sx0, g0.y + 8), ImVec2(sx1, g0.y + 22), theme::withAlpha(P.accent, 0.35f), 2);
  dl->AddRect(ImVec2(sx0, g0.y + 8), ImVec2(sx1, g0.y + 22), P.accent, 2);
  dl->AddRectFilled(ImVec2(g0.x + (g1.x - g0.x) * 0.56f, g0.y + 28), ImVec2(g0.x + (g1.x - g0.x) * 0.72f, g0.y + 40), theme::content(0x5FA8D3), 2);
  const float px = g0.x + (g1.x - g0.x) * 0.62f;
  dl->AddLine(ImVec2(px, g0.y), ImVec2(px, g1.y), P.playhead, 2.0f);
  ImGui::Dummy(ImVec2(w, h));
}

}  // namespace

void themeSettingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection(tr("APPEARANCE"));
  {
    label("Theme");
    int m = static_cast<int>(theme::modeFromSettings(st));
    const char* modes[] = {tr("Dark"), tr("Light"), tr("Follow system")};
    if (ui::Segmented("thememode", modes, 3, &m, 0)) {
      const dispconf::Snapshot before = dispconf::take(st, dispconf::themeKeys());
      theme::modeToSettings(static_cast<theme::Mode>(m), st);
      theme::applyFromSettings(st, true);
      ui::confirmTheme(a, before, "Theme");  // J-77
    }
    if (static_cast<theme::Mode>(m) == theme::Mode::System) {
      ImGui::SameLine(0, theme::kS2);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::isDark() ? tr("(the system is dark now)") : tr("(the system is light now)"));
    }
  }
  ui::EndSection();

  theme::Tweaks t = theme::tweaksFromSettings(st);
  const theme::Palette base = theme::makePalette(theme::isDark(), theme::Tweaks{});
  bool changed = false;
  ui::BeginSection(tr("COLOURS"));
  {
    label("Accent");
    // Theme default + a few accents that keep the studio look readable.
    const uint32_t presets[] = {theme::Tweaks::kKeep, 0x3D63DD, 0x4ECCFF, 0x7AE4A3, 0xA8E44A, 0xFFBC40, 0xFF53A9, 0xD963F1};
    for (int i = 0; i < 8; ++i) {
      if (i) ImGui::SameLine(0, theme::kS1);
      ImGui::PushID(i);
      const uint32_t col = presets[i] == theme::Tweaks::kKeep ? rgbOf(base.accent) : presets[i];
      ImVec2 q = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("acc", ImVec2(22, 22))) {
        t.accent = presets[i];
        changed = true;
      }
      ui::Hint(i == 0 ? tr("The theme's own accent") : tr("Accent colour for selection, focus and value arcs"));
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(q, ImVec2(q.x + 22, q.y + 22), theme::rgb(col), 4);
      if (t.accent == presets[i]) dl->AddRect(ImVec2(q.x - 2, q.y - 2), ImVec2(q.x + 24, q.y + 24), P.text, 5.0f, ImDrawFlags_None, 2.0f);
      if (i == 0) dl->AddLine(ImVec2(q.x + 5, q.y + 17), ImVec2(q.x + 17, q.y + 5), theme::contrastText(theme::rgb(col)), 1.5f);
      ImGui::PopID();
    }
    ImGui::SameLine(0, theme::kS2);
    {
      const uint32_t shown = t.accent == theme::Tweaks::kKeep ? rgbOf(base.accent) : t.accent;
      float f[3] = {((shown >> 16) & 0xFF) / 255.f, ((shown >> 8) & 0xFF) / 255.f, (shown & 0xFF) / 255.f};
      if (ImGui::ColorEdit3("##accentc", f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha)) {
        t.accent = (static_cast<uint32_t>(f[0] * 255 + 0.5f) << 16) | (static_cast<uint32_t>(f[1] * 255 + 0.5f) << 8) | static_cast<uint32_t>(f[2] * 255 + 0.5f);
        changed = true;
      }
      ui::Hint(tr("Any accent colour"));
    }
    changed |= roleColour("hot", "Lit glyphs", "Active tools, toggles, tab marks and LCD digits", &t.hot, rgbOf(base.hot));
    changed |= roleColour("led", "LEDs", "Mute, select and effect lamps, the low meter range", &t.led, rgbOf(base.led));
    changed |= roleColour("playhead", "Song position", "The playback line in the playlist and the editors", &t.playhead, rgbOf(base.playhead));
  }
  ui::EndSection();

  ui::BeginSection(tr("ADJUST"));
  {
    // J-77: contrast / brightness preview live while dragging; on release they ask Keep / Revert
    // (back to the values before the drag).
    static dispconf::Snapshot dragFrom;
    auto dragConfirm = [&](const char* what) {
      if (ImGui::IsItemActivated()) dragFrom = dispconf::take(st, dispconf::themeKeys());
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (changed) theme::tweaksToSettings(t, st), applyTheme(a), changed = false;
        ui::confirmTheme(a, dragFrom, what);
      }
    };
    label("Contrast");
    float c = t.contrast * 100.0f;
    ImGui::SetNextItemWidth(fieldW - 56);
    if (ImGui::SliderFloat("##contrast", &c, -100.0f, 100.0f, " ")) t.contrast = c / 100.0f, changed = true;
    dragConfirm("Contrast");
    ui::Hint(tr("Text and lines against the panels"));
    ImGui::SameLine(0, theme::kS2);
    ImGui::Text("%+.0f%%", static_cast<double>(c));
    label("Brightness");
    float b = t.brightness * 100.0f;
    ImGui::SetNextItemWidth(fieldW - 56);
    if (ImGui::SliderFloat("##brightness", &b, -100.0f, 100.0f, " ")) t.brightness = b / 100.0f, changed = true;
    dragConfirm("Brightness");
    ui::Hint(tr("Panels, fields and grids lighter or darker"));
    ImGui::SameLine(0, theme::kS2);
    ImGui::Text("%+.0f%%", static_cast<double>(b));
  }
  ui::EndSection();

  if (changed) {
    theme::tweaksToSettings(t, st);
    applyTheme(a);
  }

  ui::BeginSection(tr("PREVIEW"));
  themePreview(a);
  ui::EndSection();

  if (ui::TextButton(tr("Reset theme"), false, 0, tr("Theme colours, contrast and brightness back to the defaults (the dark / light choice stays)"))) {
    const dispconf::Snapshot before = dispconf::take(st, dispconf::themeKeys());
    theme::resetThemeSettings(st);
    applyTheme(a);
    ui::confirmTheme(a, before, "Theme reset");  // J-77: colours and adjustments can come back
  }
}

}  // namespace gl
