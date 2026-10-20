// Gridline — keyboard / MIDI workflow UI (J-17): remote control settings
// dialog, link menu items, Settings → MIDI page, recording options and typing
// keyboard menus. Drawn with Gridline's theme.
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../core/I18n.h"
#include "Keys.h"
#include "../../third_party/imgui/imgui.h"
#include "App.h"
#include "MidiUi.h"
#include "ScriptUi.h"
#include "AudioRecordUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

namespace fs = std::filesystem;
using theme::pal;

namespace {

void label(const char* text, float x = 150.0f) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "%s", text);
  ImGui::SameLine(x);
}

std::string targetName(App& a, const ControllerLink& l) {
  if (l.action != RemoteAction::None) return std::string("Action · ") + remoteActionName(l.action);
  return paramDisplayName(a.doc.song(), {l.owner, l.index});
}

template <class E>
bool enumCombo(const char* id, E* v, int count, const char* (*name)(E), float w) {
  bool changed = false;
  if (ui::BeginCombo(id, name(*v), w)) {
    for (int i = 0; i < count; ++i)
      if (ImGui::Selectable(name(static_cast<E>(i)), static_cast<int>(*v) == i)) {
        *v = static_cast<E>(i);
        changed = true;
      }
    ImGui::EndCombo();
  }
  return changed;
}

const char* channelName(int c) {
  static char buf[16];
  if (c < 0) return "Any";
  std::snprintf(buf, sizeof buf, "%d", c + 1);
  return buf;
}

bool midiChannelCombo(const char* id, int* ch, float w, const char* anyLabel = "Any") {
  bool changed = false;
  if (ui::BeginCombo(id, *ch < 0 ? anyLabel : channelName(*ch), w)) {
    if (ImGui::Selectable(anyLabel, *ch < 0)) *ch = -1, changed = true;
    for (int c = 0; c < 16; ++c)
      if (ImGui::Selectable(channelName(c), *ch == c)) *ch = c, changed = true;
    ImGui::EndCombo();
  }
  return changed;
}

// Input → output graph of a link's mapping (curve, formula, range, invert).
void mappingGraph(const ControllerLink& l, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), P.field, theme::kRadiusSm);
  for (int i = 1; i < 4; ++i) {
    float gx = std::round(p.x + size.x * i / 4.0f), gy = std::round(p.y + size.y * i / 4.0f);
    dl->AddLine(ImVec2(gx, p.y), ImVec2(gx, p.y + size.y), i == 2 ? P.gridBar : P.gridBeat);
    dl->AddLine(ImVec2(p.x, gy), ImVec2(p.x + size.x, gy), i == 2 ? P.gridBar : P.gridBeat);
  }
  Expr f;
  if (l.curve == ModCurve::Formula) compileRemoteFormula(l.formula, f);
  const int n = 96;
  ImVec2 prev;
  for (int i = 0; i <= n; ++i) {
    double u = static_cast<double>(i) / n;
    double v = l.encoder != RemoteEncoder::Absolute ? remoteRange(l, u) : remoteRange(l, remoteCurve(l, u, &f));
    ImVec2 q(p.x + 3 + (size.x - 6) * static_cast<float>(u), p.y + size.y - 3 - (size.y - 6) * static_cast<float>(std::clamp(v, 0.0, 1.0)));
    if (i > 0) dl->AddLine(prev, q, P.hot, 2.0f);
    prev = q;
  }
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), P.dark ? theme::withAlpha(P.ink, 0.5f) : P.line, theme::kRadiusSm);
  App& a = app();
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + 6, p.y + size.y - 16), P.textFaint, "INPUT");
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + 6, p.y + 4), P.textFaint, "VALUE");
}

void saveLink(App& a) {
  ControllerLink l = a.remoteEdit;
  const int idx = a.remoteEditIndex;
  a.doc.editSong(idx >= 0 ? "edit controller link" : "link controller", [&](Song& s) {
    if (idx >= 0 && idx < static_cast<int>(s.links.size())) {
      s.links[static_cast<size_t>(idx)] = l;
      return;
    }
    // One link per target.
    s.links.erase(std::remove_if(s.links.begin(), s.links.end(),
                                 [&](const ControllerLink& x) {
                                   return l.action != RemoteAction::None ? x.action == l.action
                                                                         : x.action == RemoteAction::None && x.owner == l.owner && x.index == l.index;
                                 }),
                  s.links.end());
    s.links.push_back(l);
  });
  a.remote.forget(l);
  a.notify("Linked " + remoteSourceLabel(l) + " → " + targetName(a, l));
}

void removeLinkAt(App& a, int idx) {
  a.doc.editSong("remove controller link", [idx](Song& s) {
    if (idx >= 0 && idx < static_cast<int>(s.links.size())) s.links.erase(s.links.begin() + idx);
  });
}

}  // namespace

// ------------------------------------------------------------ remote dialog ----

void drawRemoteDialog(App& a) {
  if (!a.showRemoteDialog) return;
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool open = true;
  bool vis = ImGui::Begin(ui::windowTitle("Remote control settings").c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    if (!open) a.showRemoteDialog = a.remoteListening = a.midiLearning = false;
    return;
  }
  ControllerLink& l = a.remoteEdit;
  const float w = 200.0f;
  // Body scrolls when the window is short (small screens / large UI scale);
  // the footer buttons always stay visible.
  {
    const float maxBody = std::max(160.0f, vp->WorkSize.y - theme::kTopBarH - theme::kStatusBarH - ImGui::GetFrameHeight() -
                                               2 * theme::kS3 - ui::DialogFooterHeight() - theme::kS4);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxBody));
  }
  ImGui::BeginChild("##remotebody", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);

  // Target.
  ImGui::PushFont(a.fontBold, theme::kFontTitle);
  ImGui::TextUnformatted(targetName(a, l).c_str());
  ImGui::PopFont();
  if (l.action != RemoteAction::None) {
    label("Action");
    RemoteAction act = l.action;
    if (ui::BeginCombo("##act", remoteActionName(act), w)) {
      for (int i = 1; i < static_cast<int>(RemoteAction::Count); ++i)
        if (ImGui::Selectable(remoteActionName(static_cast<RemoteAction>(i)), static_cast<int>(act) == i)) l.action = static_cast<RemoteAction>(i);
      ImGui::EndCombo();
    }
  } else if (auto d = describeParam(a.doc.song(), {l.owner, l.index})) {
    if (auto v = getParam(a.doc.song(), {l.owner, l.index}))
      ImGui::TextColored(theme::vec(P.textDim), i18n::tr("Current value %s"), d->format(*v).c_str());
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));

  // Controller.
  ui::BeginSection("MIDI CONTROLLER");
  {
    ImVec2 lp = ImGui::GetCursorScreenPos();
    bool blink = a.remoteListening && std::fmod(a.now, 0.8) < 0.4;
    ui::Led(ImGui::GetWindowDrawList(), ImVec2(lp.x + 5, lp.y + ImGui::GetFrameHeight() * 0.5f), 4.0f, blink, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(lp.x + 16, lp.y));
    if (ui::TextButton(a.remoteListening ? "Listening: move a control…" : "Learn from controller", a.remoteListening, 220))
      a.remoteListening = !a.remoteListening;
    ImGui::SameLine(0, theme::kS3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.text), "%s", remoteSourceLabel(l).c_str());
  }
  label("Source");
  enumCombo("##src", &l.source, static_cast<int>(RemoteSource::Count), remoteSourceName, w);
  if (l.source == RemoteSource::CC || l.source == RemoteSource::Note || l.source == RemoteSource::KeyPressure ||
      l.source == RemoteSource::Program) {
    label(l.source == RemoteSource::CC ? "Controller number" : l.source == RemoteSource::Program ? "Program number" : "Note");
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputInt("##num", &l.cc)) l.cc = std::clamp(l.cc, 0, 127);
    if (l.source == RemoteSource::Note || l.source == RemoteSource::KeyPressure) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textDim), "%s", midiNoteName(l.cc).c_str());
    }
  }
  label("MIDI channel");
  midiChannelCombo("##ch", &l.midiChannel, w);
  label("Input port");
  {
    std::string prev = l.port < 0 ? "Any device" : "Port " + std::to_string(l.port);
    if (ui::BeginCombo("##port", prev.c_str(), w)) {
      if (ImGui::Selectable(i18n::tr("Any device"), l.port < 0)) l.port = -1;
      if (a.midi)
        for (const auto& c : a.midi->inputCfg)
          if (ImGui::Selectable(("Port " + std::to_string(c.port) + "  " + c.name).c_str(), l.port == c.port)) l.port = c.port;
      ImGui::EndCombo();
    }
  }

  ui::EndSection();

  // Mapping.
  ui::BeginSection("MAPPING");
  const ImVec2 graphPos(ImGui::GetCursorScreenPos().x + 150 + w + theme::kS6, ImGui::GetCursorScreenPos().y);
  label("Presets");
  if (ui::TextButton("Choose…", false, w)) ImGui::OpenPopup("##presets");
  if (ImGui::BeginPopup("##presets")) {
    const char* group = "";
    for (const auto& p : remotePresets()) {
      if (std::string(group) != p.group) {
        group = p.group;
        ImGui::TextDisabled("%s", group);
      }
      if (ImGui::MenuItem(p.name)) applyRemotePreset(l, p);
    }
    ImGui::EndPopup();
  }
  label("Curve");
  enumCombo("##curve", &l.curve, static_cast<int>(ModCurve::Count), modCurveName, w);
  if (l.curve == ModCurve::Formula) {
    static char buf[256];
    static const ControllerLink* last = nullptr;
    if (last != &l || ImGui::IsWindowAppearing()) std::snprintf(buf, sizeof buf, "%s", l.formula.c_str());
    last = &l;
    label("Formula");
    ImGui::SetNextItemWidth(w);
    if (ImGui::InputTextWithHint("##formula", i18n::tr("input"), buf, sizeof buf)) l.formula = buf;
    Expr e;
    std::string err;
    if (!compileRemoteFormula(l.formula, e, &err)) {
      ImGui::Dummy(ImVec2(150, 0));
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.danger), "%s", err.c_str());
    }
  }
  label("Invert");
  ImGui::Checkbox("##inv", &l.invert);
  label("Range");
  {
    float lo = l.min * 100.f, hi = l.max * 100.f;
    ImGui::SetNextItemWidth(w * 0.5f - 4);
    if (ImGui::DragFloat("##min", &lo, 0.5f, 0, 100, "min %.0f%%")) l.min = std::clamp(lo, 0.f, 100.f) / 100.f;
    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(w * 0.5f - 4);
    if (ImGui::DragFloat("##max", &hi, 0.5f, 0, 100, "max %.0f%%")) l.max = std::clamp(hi, 0.f, 100.f) / 100.f;
  }
  {
    ImVec2 back = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(graphPos);
    mappingGraph(l, ImVec2(std::max(120.0f, ImGui::GetContentRegionAvail().x - theme::kS2), 132));
    ImGui::SetCursorScreenPos(back);
    ImGui::Dummy(ImVec2(0, std::max(0.0f, graphPos.y + 132 - back.y)));
  }

  ui::EndSection();

  // Behaviour.
  ui::BeginSection("BEHAVIOUR");
  label("Takeover");
  {
    const char* names[] = {"Jump", "Pick up", "Scale"};
    const char* hints[] = {"The value jumps to the control's position", "Nothing happens until the control passes the value",
                           "Moves proportionally until the control and the value meet"};
    int t = static_cast<int>(l.takeover);
    if (ui::Segmented("##take", names, 3, &t, 80, 0, hints)) l.takeover = static_cast<RemoteTakeover>(t);
  }
  label("Encoder");
  enumCombo("##enc", &l.encoder, static_cast<int>(RemoteEncoder::Count), remoteEncoderName, w);
  if (l.encoder != RemoteEncoder::Absolute) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("##sens", &l.sensitivity, 0.05f, 8.0f, "step x%.2f", ImGuiSliderFlags_Logarithmic);
  }
  label("Note / button");
  enumCombo("##btn", &l.button, static_cast<int>(RemoteButton::Count), remoteButtonName, w);
  if (l.takeover == RemoteTakeover::Pickup && a.remote.waiting(l)) {
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Waiting for the control to reach the current value…"));
  }
  ui::EndSection();
  ImGui::EndChild();

  // Buttons.
  ui::DialogFooter(0);
  bool close = false;
  if (a.remoteEditIndex >= 0) {
    if (ui::TextButton("Remove link", false, 110)) {
      removeLinkAt(a, a.remoteEditIndex);
      close = true;
    }
    ImGui::SameLine();
  }
  if (ui::TextButton("Reset", false, 80, "Default settings (keeps the source and target)")) {
    ControllerLink d;
    d.cc = l.cc;
    d.source = l.source;
    d.midiChannel = l.midiChannel;
    d.port = l.port;
    d.owner = l.owner;
    d.index = l.index;
    d.action = l.action;
    l = d;
  }
  float right = ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(ImGui::GetCursorPosX() + right - 96 * 2 - theme::kS2);
  if (ui::TextButton("Cancel", false, 96) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Accept", true, 96)) {
    saveLink(a);
    close = true;
  }
  ImGui::End();
  if (close || !open) a.showRemoteDialog = a.remoteListening = a.midiLearning = false;
}

// ------------------------------------------------------------ menu items ----

void remoteLinkMenuItems(App& a, ParamId pid) {
  int idx = -1;
  const auto& links = a.doc.song().links;
  for (size_t i = 0; i < links.size(); ++i)
    if (links[i].action == RemoteAction::None && links[i].owner == pid.owner && links[i].index == pid.index) idx = static_cast<int>(i);
  if (ImGui::MenuItem(idx >= 0 ? i18n::tr("Edit controller link…") : i18n::tr("Link to controller…"))) a.openRemoteDialog(pid);
  if (idx >= 0) {
    ImGui::TextDisabled("  %s", remoteSourceLabel(links[static_cast<size_t>(idx)]).c_str());
    if (ImGui::MenuItem(i18n::tr("Remove controller link"))) removeLinkAt(a, idx);
  }
  if (a.extraParamMenu) a.extraParamMenu(pid);  // PH-09: host macros (plugin build)
  if (ImGui::MenuItem(i18n::tr("Multilink to controllers"), nullptr, a.multilink)) {
    a.multilink = !a.multilink;
    if (a.multilink) {
      a.multilinkQueue = {pid};
      a.notify("Multilink: move controls here, then the hardware controls in the same order");
    }
  }
}

void actionLinkMenuItems(App& a, RemoteAction action) {
  bool linked = false;
  for (const auto& l : a.doc.song().links) linked = linked || l.action == action;
  std::string t = std::string(linked ? "Edit controller link (" : "Link to controller (") + remoteActionName(action) + ")…";
  if (ImGui::MenuItem(t.c_str())) a.openRemoteDialog(action);
}

void recordOptionsMenu(App& a) {
  RecordOptions& o = a.recOptions;
  audioRecordFilterMenu(a);  // J-8: what to record (audio / audio editor / notes / automation)
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Notes"));
  if (ImGui::MenuItem(i18n::tr("Overdub (keep existing notes)"), nullptr, o.overdub)) o.overdub = true;
  if (ImGui::MenuItem(i18n::tr("Replace (clear the channel's notes)"), nullptr, !o.overdub)) o.overdub = false;
  ImGui::Separator();
  ImGui::MenuItem(i18n::tr("Quantize note starts to the snap"), nullptr, &o.quantize);
  ImGui::MenuItem(i18n::tr("Quantize note lengths"), nullptr, &o.quantizeLength);
  ImGui::MenuItem(i18n::tr("Loop recording: new take per pass"), nullptr, &o.takes);
  if (ImGui::BeginMenu(i18n::tr("Controller moves go to"))) {
    for (int i = 0; i < static_cast<int>(AutoRecordTarget::Count); ++i)
      if (ImGui::MenuItem(autoRecordTargetName(static_cast<AutoRecordTarget>(i)), nullptr, static_cast<int>(o.autoTarget) == i))
        o.autoTarget = static_cast<AutoRecordTarget>(i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Count-in"))) {
    const int bars[] = {0, 1, 2, 4};
    for (int b : bars) {
      std::string n = b == 0 ? "Off" : std::to_string(b) + (b == 1 ? " bar" : " bars");
      if (ImGui::MenuItem(n.c_str(), nullptr, a.countInBars == b)) a.countInBars = b;
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Step record (keyboard enters notes at the step cursor)"), nullptr, a.stepRecord)) a.stepRecord = !a.stepRecord;
  ImGui::Separator();
  actionLinkMenuItems(a, RemoteAction::Record);
}

void typingKeyboardMenu(App& a) {
  TypingKeyboard& k = a.typing;
  ImGui::TextDisabled("%s", i18n::tr("Typing keyboard"));
  if (ImGui::MenuItem(i18n::tr("Play notes from the computer keyboard"), keys::label("options.typingKeyboard"), a.typingKeyboard)) a.typingKeyboard = !a.typingKeyboard;
  if (ImGui::BeginMenu(i18n::tr("Octave"))) {
    for (int o = 1; o <= 8; ++o) {
      std::string n = "Z = " + midiNoteName(12 * o);
      if (ImGui::MenuItem(n.c_str(), nullptr, k.octave == o)) k.octave = o;
    }
    ImGui::EndMenu();
  }
  ImGui::SetNextItemWidth(160);
  ImGui::SliderInt(i18n::tr("Velocity"), &k.velocity, 1, 127);
  ImGui::MenuItem(i18n::tr("Chord mode"), "\\", &k.chordMode);
  if (ImGui::BeginMenu(i18n::tr("Chord"))) {
    for (int i = 0; i < static_cast<int>(ChordType::Count); ++i)
      if (ImGui::MenuItem(chordTypeName(static_cast<ChordType>(i)), nullptr, static_cast<int>(k.chord) == i)) k.chord = static_cast<ChordType>(i);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Z…/ and Q…P play · [ ] octave · - = velocity"));
  ImGui::TextDisabled("%s", i18n::tr("\\ chord mode · ' next chord"));
}

// ------------------------------------------------------------ settings page ----

void midiSettingsPage(App& a, float fieldW) {
  const auto& P = pal();
  ImGui::Dummy(ImVec2(0, theme::kS2));
  if (!a.midi) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("MIDI is not available."));
    return;
  }
  MidiManager& m = *a.midi;
  bool changed = false, reopen = false;
  const float colW = std::max(240.0f, fieldW);

  // Input devices table.
  ui::BeginSection("INPUT DEVICES");
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Notes play the selected channel; controls drive links and the device template."));
  if (m.inputs.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No MIDI inputs found."));
  if (!m.inputs.empty() && ImGui::BeginTable("##ins", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn(i18n::tr("On"), ImGuiTableColumnFlags_WidthFixed, 30);
    ImGui::TableSetupColumn(i18n::tr("Device"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(i18n::tr("Controller type"), ImGuiTableColumnFlags_WidthFixed, 190);
    ImGui::TableSetupColumn(i18n::tr("Port"), ImGuiTableColumnFlags_WidthFixed, 74);
    ImGui::TableSetupColumn(i18n::tr("Notes"), ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableHeadersRow();
    for (const auto& name : m.inputs) {
      MidiInputConfig* c = m.inputConfig(name);
      if (!c) continue;
      ImGui::PushID(name.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Checkbox("##on", &c->enabled)) changed = reopen = true;
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableNextColumn();
      const ControllerTemplate* autoT = matchControllerTemplate(a.controllerTemplates, name);
      std::string preview = c->controllerType.empty() ? std::string("Auto: ") + (autoT ? autoT->name : "none")
                            : c->controllerType == "none" ? std::string("(none)")
                                                          : [&]() {
                                                              if (c->controllerType.rfind("script:", 0) == 0) {  // J-24
                                                                for (const auto& s : controllerScripts(a))
                                                                  if ("script:" + s.id == c->controllerType) return "Script: " + s.name;
                                                                return "Script: " + c->controllerType.substr(7) + " (missing)";
                                                              }
                                                              auto* t = findControllerTemplate(a.controllerTemplates, c->controllerType);
                                                              return t ? t->name : c->controllerType;
                                                            }();
      if (ui::BeginCombo("##type", preview.c_str(), 186)) {
        if (ImGui::Selectable(i18n::tr("Automatic (by device name)"), c->controllerType.empty())) c->controllerType.clear(), changed = true;
        if (ImGui::Selectable(i18n::tr("(none)"), c->controllerType == "none")) c->controllerType = "none", changed = true;
        for (const auto& t : a.controllerTemplates)
          if (ImGui::Selectable(t.name.c_str(), c->controllerType == t.id)) c->controllerType = t.id, changed = true;
        // J-24: Lua controller scripts as an alternative to JSON templates.
        if (!controllerScripts(a).empty()) ImGui::SeparatorText(i18n::tr("Scripts"));
        for (const auto& s : controllerScripts(a)) {
          std::string id = "script:" + s.id;
          if (ImGui::Selectable(("Script: " + s.name + "##" + id).c_str(), c->controllerType == id)) c->controllerType = id, changed = true;
          if (!s.description.empty()) ui::Hint(s.description);
        }
        ImGui::EndCombo();
      }
      if (c->controllerType.rfind("script:", 0) == 0) {
        std::string st = controllerScriptStatus(name);
        if (!st.empty() && st != "running") ui::Hint("Script stopped: " + st);
      }
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(70);
      if (ImGui::InputInt("##port", &c->port, 0)) c->port = std::clamp(c->port, 0, 255), changed = reopen = true;
      ImGui::TableNextColumn();
      if (ImGui::Checkbox("##notes", &c->notes)) changed = reopen = true;
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (ui::TextButton("Rescan devices")) {
    m.scan();
    changed = reopen = true;
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Reload templates", false, 0, "Reads the shipped and your own controller templates and scripts again")) {
    a.loadControllerTemplates();
    controllerScripts(a, true);
    shutdownControllerScripts();  // reloaded on the next frame
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Open template folder", false, 0, "Your own templates (JSON) override the shipped ones by id")) {
    std::error_code ec;
    fs::create_directories(a.templatesUserDir(), ec);
    a.notify("Controller templates folder: " + a.templatesUserDir());
  }
  ImGui::SameLine(0, theme::kS3);
  {
    bool active = m.lastActivity() > 0 &&
                  std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - m.lastActivity() < 0.25;
    ImVec2 lp = ImGui::GetCursorScreenPos();
    ui::Led(ImGui::GetWindowDrawList(), ImVec2(lp.x + 5, lp.y + ImGui::GetFrameHeight() * 0.5f), 4.0f, active, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(lp.x + 16, lp.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(active ? P.text : P.textFaint), active ? "Receiving" : "Idle");
  }
  ImGui::TextColored(theme::vec(P.textFaint), "%s", m.status.c_str());

  ui::EndSection();

  // Input routing.
  ui::BeginSection("INPUT");
  MidiRouting& r = m.routing;
  label("Note-on velocity", 168);
  if (enumCombo("##vel", &r.velocity, static_cast<int>(VelocityCurve::Count), velocityCurveName, colW)) changed = true;
  if (r.velocity == VelocityCurve::Fixed) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::SliderInt("##fixvel", &r.fixedVelocity, 1, 127)) changed = true;
  }
  label("Notes play", 168);
  {
    int mode = r.byChannel ? 1 : 0;
    const char* names[] = {"Selected channel", "By MIDI channel"};
    if (ui::Segmented("##route", names, 2, &mode, colW * 0.5f)) r.byChannel = mode == 1, changed = true;
  }
  label("Omni preview channel", 168);
  if (midiChannelCombo("##omni", &r.omniChannel, colW, "Off")) changed = true;
  label("Note-off", 168);
  {
    int off = r.ignoreNoteOff ? 1 : 0;
    const char* names[] = {"Release notes", "Ignore (one-shot pads)"};
    if (ui::Segmented("##noff", names, 2, &off, colW * 0.5f)) r.ignoreNoteOff = off == 1, changed = true;
  }
  label("Pedals", 168);
  if (ImGui::Checkbox(i18n::tr("Sostenuto (CC 66)"), &r.sostenuto)) changed = true;
  ImGui::SameLine(0, theme::kS3);
  if (ImGui::Checkbox(i18n::tr("Soft (CC 67)"), &r.soft)) changed = true;
  ImGui::SameLine(0, theme::kS2);
  ImGui::SetNextItemWidth(110);
  float softPct = r.softFactor * 100;
  if (ImGui::SliderFloat("##softf", &softPct, 10, 100, "velocity %.0f%%")) r.softFactor = softPct / 100, changed = true;
  ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Sustain (CC 64), aftertouch and pitch bend always reach the instrument."));
  if (ImGui::TreeNodeEx("MIDI channel routing and pitch bend range", r.byChannel ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
    const Song& s = a.doc.song();
    if (ImGui::BeginTable("##chroute", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn(i18n::tr("MIDI ch"), ImGuiTableColumnFlags_WidthFixed, 60);
      ImGui::TableSetupColumn(i18n::tr("Plays"), ImGuiTableColumnFlags_WidthFixed, 220);
      ImGui::TableSetupColumn(i18n::tr("Bend range"), ImGuiTableColumnFlags_WidthFixed, 130);
      ImGui::TableHeadersRow();
      for (int c = 0; c < 16; ++c) {
        ImGui::PushID(c);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d", c + 1);
        ImGui::TableNextColumn();
        int ri = r.route[c];
        std::string prev = c == r.omniChannel ? "Selected (omni)"
                           : ri < 0 || ri >= static_cast<int>(s.channels.size()) ? "Selected channel"
                                                                                   : s.channels[static_cast<size_t>(ri)].name;
        if (ui::BeginCombo("##rc", prev.c_str(), 216)) {
          if (ImGui::Selectable(i18n::tr("Selected channel"), ri < 0)) r.route[c] = -1, changed = true;
          for (size_t i = 0; i < s.channels.size(); ++i)
            if (ImGui::Selectable((std::to_string(i + 1) + "  " + s.channels[i].name).c_str(), ri == static_cast<int>(i)))
              r.route[c] = static_cast<int>(i), changed = true;
          ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(126);
        if (ImGui::SliderFloat("##bend", &r.bendRange[c], 0, 24, "±%.0f st")) changed = true;
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    ImGui::TreePop();
  }

  ui::EndSection();

  // Output & sync.
  ui::BeginSection("OUTPUT AND SYNC");
  label("Output device", 168);
  if (ui::BeginCombo("##midiout", m.output.empty() ? "None" : m.output.c_str(), colW)) {
    if (ImGui::Selectable(i18n::tr("None"), m.output.empty())) m.output.clear(), changed = reopen = true;
    for (const auto& o : m.outputs)
      if (ImGui::Selectable(o.c_str(), o == m.output)) m.output = o, changed = reopen = true;
    ImGui::EndCombo();
  }
  label("Send sync", 168);
  if (enumCombo("##sync", &m.syncOut, static_cast<int>(SyncOut::Count), syncOutName, colW)) changed = true;
  label("Sync offset (ms)", 168);
  ImGui::SetNextItemWidth(colW);
  if (ImGui::SliderInt("##soff", &m.syncOffsetMs, -100, 100)) changed = true;
  // MI-02: the transport follows an external master.
  label(i18n::tr("Sync to"), 168);
  if (enumCombo("##syncsrc", &m.syncSource, static_cast<int>(SyncSource::Count), syncSourceName, colW)) {
    changed = reopen = true;
    std::lock_guard<std::mutex> lk(m.followerMutex());
    m.follower().reset();
    m.mtc().reset();
    m.chase().reset();
  }
  if (m.syncSource != SyncSource::Internal) {
    label(i18n::tr("Sync input"), 168);
    if (ui::BeginCombo("##cindev", m.clockInDevice.empty() ? i18n::tr("Any input") : m.clockInDevice.c_str(), colW)) {
      if (ImGui::Selectable(i18n::tr("Any input"), m.clockInDevice.empty())) m.clockInDevice.clear(), changed = reopen = true;
      for (const auto& n : m.inputs)
        if (ImGui::Selectable(n.c_str(), n == m.clockInDevice)) m.clockInDevice = n, changed = reopen = true;
      ImGui::EndCombo();
    }
    label("Position offset (ms)", 168);
    ImGui::SetNextItemWidth(colW);
    if (ImGui::SliderInt("##cinoff", &m.clockInOffsetMs, -1000, 1000)) changed = true;
    label("Chase tolerance (ms)", 168);
    ImGui::SetNextItemWidth(colW);
    if (ImGui::SliderInt("##chase", &m.chaseToleranceMs, 5, 500)) changed = true;
    // Lock indicator: green when locked to a running master, dim otherwise.
    label("Status", 168);
    const double t = MidiManager::clockNow();
    std::lock_guard<std::mutex> lk(m.followerMutex());
    bool locked = false;
    char line[160];
    if (m.syncSource == SyncSource::Mtc) {
      const MtcReceiver& rx = m.mtc();
      locked = rx.locked(t);
      const Timecode tc = rx.timecode();
      if (rx.hasPosition())
        std::snprintf(line, sizeof line, "%s  %02d:%02d:%02d:%02d  %s  drift %+.1f ms", locked ? "Locked" : "Stopped", tc.h, tc.m, tc.s, tc.f,
                      syncOutName(rx.rate()) + 4, m.chase().drift() * 1000.0);
      else
        std::snprintf(line, sizeof line, "No time code received yet.");
    } else {
      locked = m.follower().hasTempo() && m.follower().running();
      if (m.follower().hasTempo())
        std::snprintf(line, sizeof line, "%s  %.2f BPM", m.follower().running() ? "Locked" : "Stopped", m.follower().bpm());
      else
        std::snprintf(line, sizeof line, "No clock received yet.");
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ui::Led(ImGui::GetWindowDrawList(), ImVec2(p.x + 7, p.y + ImGui::GetFrameHeight() * 0.5f), 4.5f, locked);
    ImGui::Dummy(ImVec2(16, ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(locked ? P.text : P.textDim), "%s", line);
    if (m.chase().relocations() > 0) {
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textFaint), "· %d relocation%s", m.chase().relocations(), m.chase().relocations() == 1 ? "" : "s");
    }
  }

  ui::EndSection();

  // Controller links.
  ui::BeginSection("CONTROLLER LINKS");
  const Song& s = a.doc.song();
  if (s.links.empty())
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Right-click any knob, fader or transport button and choose \"Link to controller…\"."));
  int remove = -1, edit = -1;
  if (!s.links.empty() && ImGui::BeginTable("##links", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn(i18n::tr("Control"), ImGuiTableColumnFlags_WidthFixed, 190);
    ImGui::TableSetupColumn(i18n::tr("Target"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(i18n::tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthFixed, 52);
    for (size_t i = 0; i < s.links.size(); ++i) {
      const auto& l = s.links[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::PushFont(a.fontMono, theme::kFontNumber);
      ImGui::TextUnformatted(remoteSourceLabel(l).c_str());
      ImGui::PopFont();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(targetName(a, l).c_str());
      ImGui::TableNextColumn();
      ImGui::TextColored(theme::vec(P.textDim), "%s",
                         l.encoder != RemoteEncoder::Absolute ? "Relative" : remoteTakeoverName(l.takeover));
      ImGui::TableNextColumn();
      if (ui::IconButton("e", ui::Icon::Settings, false, 20, "Remote control settings")) edit = static_cast<int>(i);
      ImGui::SameLine(0, 4);
      if (ui::IconButton("x", ui::Icon::Close, false, 20, "Remove link")) remove = static_cast<int>(i);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (edit >= 0) a.openRemoteDialogForLink(edit);
  if (remove >= 0) removeLinkAt(a, remove);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (ui::TextButton(a.multilink ? "Multilink: on" : "Multilink to controllers", a.multilink, 0,
                     "Move controls in Gridline, then the hardware controls in the same order"))
    a.multilink = !a.multilink;
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Link transport…")) ImGui::OpenPopup("##tr");
  if (ImGui::BeginPopup("##tr")) {
    for (int i = 1; i < static_cast<int>(RemoteAction::Count); ++i) actionLinkMenuItems(a, static_cast<RemoteAction>(i));
    ImGui::EndPopup();
  }
  ui::EndSection();

  if (reopen) m.apply();
  if (changed) {
    a.publishMidi();
    m.save((fs::path(a.configDir()) / "midi.json").string());
  }
}

}  // namespace gl
