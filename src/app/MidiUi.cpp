// Gridline — keyboard / MIDI workflow UI (J-17): remote control settings
// dialog, link menu items, Settings → MIDI page, recording options and typing
// keyboard menus. Drawn with Gridline's theme.
#include "../core/ChromeOs.h"
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../core/I18n.h"
#include "../core/KeyCaps.h"
#include "../core/Params.h"
#include "Keys.h"
#include "../../third_party/imgui/imgui.h"
#include "App.h"
#include "MidiUi.h"
#include "../core/Osc.h"
#include "OscUi.h"
#include "FlkeyUi.h"
#include "MackieUi.h"
#include "Surfaces.h"
#include "ScriptUi.h"
#include "AudioRecordUi.h"
#include "J31Ui.h"
#include "RecordCard.h"
#include "RecordRowsUi.h"
#include "RecordMoreUi.h"
#include "LiveLook.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

namespace fs = std::filesystem;
using theme::pal;

namespace {

void label(const char* text, float x = 150.0f) {
  if (ui::FormColumnIndent() > 0) {  // K-112b: the Logic layout's form (label ends at the controls)
    ui::FormLabelFit(text, x, pal().textDim);
    return;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textDim), "%s", text);
  ImGui::SameLine(ui::SettingsColumn(x));  // K-112c: Live's settings column
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
    const bool relInv = l.invert != (l.curve == ModCurve::Inverted);
    double v = l.encoder != RemoteEncoder::Absolute ? remoteRange(l, relInv ? 1.0 - u : u) : remoteRange(l, remoteCurve(l, u, &f));
    ImVec2 q(p.x + 3 + (size.x - 6) * static_cast<float>(u), p.y + size.y - 3 - (size.y - 6) * static_cast<float>(std::clamp(v, 0.0, 1.0)));
    if (i > 0) dl->AddLine(prev, q, P.hot, 2.0f);
    prev = q;
  }
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), P.dark ? theme::withAlpha(P.ink, 0.5f) : P.line, theme::kRadiusSm);
  App& a = app();
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + 6, p.y + size.y - 16), P.textFaint, "INPUT");
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + 6, p.y + 4), P.textFaint, "VALUE");
}

// The device an input port number belongs to ("" when none is known).
std::string deviceOfPort(App& a, int port) {
  if (!a.midi || port < 0) return "";
  for (const auto& c : a.midi->inputCfg)
    if (c.port == port) return c.name;
  return "";
}

std::string sourceNumberText(RemoteSource src, int number, bool hiRes) {
  switch (src) {
    case RemoteSource::CC: return std::to_string(number) + (hiRes ? " (14-bit)" : "");
    case RemoteSource::Note:
    case RemoteSource::KeyPressure: return midiNoteName(number) + " (" + std::to_string(number) + ")";
    case RemoteSource::Program: return std::to_string(number + 1);
    case RemoteSource::Nrpn:
    case RemoteSource::Rpn: return std::to_string(number) + " (" + std::to_string(number / 128) + ", " + std::to_string(number % 128) + ")";
    default: return "-";
  }
}

// k078-linkctl: the listening line and the card of the captured control (device, channel, type,
// number and a meter that follows it), with the note that a surface's control is taken over.
void captureBox(App& a, ControllerLink& l) {
  const auto& P = pal();
  LinkCapture& cap = linkctl::capture();
  const bool listening = a.remoteListening;
  const bool osc = l.source == RemoteSource::Osc;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  {  // the listening line
    ImVec2 lp = ImGui::GetCursorScreenPos();
    const float rightX = lp.x + ImGui::GetContentRegionAvail().x;
    const bool blink = listening && std::fmod(a.now, 0.8) < 0.4;
    ui::Led(dl, ImVec2(lp.x + 5, lp.y + ImGui::GetFrameHeight() * 0.5f), 4.0f, blink, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(lp.x + 16, lp.y));
    ImGui::AlignTextToFramePadding();
    const char* line = !listening ? i18n::tr("Not listening")
                       : cap.hasCapture() ? i18n::tr("Listening: move another control to pick it instead")
                                          : i18n::tr("Listening: move a control on your controller");
    ImGui::PushFont(a.fontBold, theme::kFontBody);
    ImGui::TextColored(theme::vec(listening ? P.text : P.textDim), "%s", line);
    ImGui::PopFont();
    const float bw = 130;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(rightX - bw, lp.y));
    if (ui::TextButton(listening ? "Stop listening" : "Listen again", false, bw,
                       listening ? "Keep the control captured and stop listening" : "Move a control on your controller to pick it")) {
      a.remoteListening = !listening;
      if (a.remoteListening) linkctl::start(a);
    }
  }
  // The card.
  const float cardH = 78;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float cardW = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy(ImVec2(cardW, cardH));
  dl->AddRectFilled(p0, ImVec2(p0.x + cardW, p0.y + cardH), P.field, theme::kRadius);
  dl->AddRect(p0, ImVec2(p0.x + cardW, p0.y + cardH), listening && !cap.hasCapture() ? theme::withAlpha(P.hot, 0.6f) : P.line, theme::kRadius);
  const bool have = cap.hasCapture() || (!listening && a.remoteEditIndex >= 0);
  if (!have || osc) {
    const char* wait = osc ? i18n::tr("OSC: the address below is what links")
                       : listening ? i18n::tr("Waiting for a knob, fader, wheel or button. Clock, repeated values and the keys are ignored.")
                                   : i18n::tr("Nothing captured yet");
    dl->AddText(ImGui::GetFont(), theme::kFontCaption, ImVec2(p0.x + theme::kS3, p0.y + cardH * 0.5f - theme::kFontCaption * 0.6f), P.textFaint, wait);
  } else {
    const bool fromCap = cap.hasCapture();
    const LinkCapture::Captured& c = cap.captured();
    std::string device = fromCap ? c.device : deviceOfPort(a, l.port);
    if (device.empty()) device = i18n::tr("Any device");
    const std::string ch = l.midiChannel < 0 ? std::string(i18n::tr("Any")) : std::to_string(l.midiChannel + 1);
    const char* cols[] = {"DEVICE", "CHANNEL", "TYPE", "NUMBER"};
    const std::string vals[] = {device, ch, remoteSourceName(l.source), sourceNumberText(l.source, l.cc, l.hiRes)};
    const float colX[] = {0, 190, 270, 410};
    for (int i = 0; i < 4; ++i) {
      const ImVec2 q(p0.x + theme::kS3 + colX[i], p0.y + 9);
      dl->AddText(a.fontMono, theme::kFontLabel, q, P.textFaint, cols[i]);
      const float maxW = (i < 3 ? colX[i + 1] - colX[i] : cardW - 2 * theme::kS3 - colX[i]) - 10;
      std::string v = vals[i];
      while (v.size() > 4 && ImGui::GetFont()->CalcTextSizeA(theme::kFontBody, 1e9f, 0, v.c_str()).x > maxW) v = v.substr(0, v.size() - 4) + "...";
      dl->AddText(ImGui::GetFont(), theme::kFontBody, ImVec2(q.x, q.y + 17), P.text, v.c_str());
    }
    // The meter: where the control is now (it follows the controller while the dialog listens).
    const float mx = p0.x + theme::kS3 + 52, mw = p0.x + cardW - theme::kS3 - 34 - mx;
    if (fromCap && mw > 30) {
      const float my = p0.y + 56, mh = 8;
      const float f = static_cast<float>(std::clamp(c.value, 0.0, 1.0));
      dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + mw, my + mh), theme::withAlpha(P.ink, 0.35f), theme::kRadiusSm);
      dl->AddRectFilled(ImVec2(mx, my), ImVec2(mx + std::max(2.0f, mw * f), my + mh), P.hot, theme::kRadiusSm);
      char num[16];
      std::snprintf(num, sizeof num, "%d", static_cast<int>(std::lround(f * 127.0)));
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p0.x + theme::kS3, my - 2), P.textFaint, "VALUE");
      const float tw = a.fontMono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, num).x;
      dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p0.x + cardW - theme::kS3 - tw, my - 2), P.text, num);
    }
  }
  if (cap.hasCapture() && cap.captured().surface && !osc) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW);
    ImGui::TextColored(theme::vec(P.textDim),
                       i18n::tr("This control belongs to the controller's own Gridline page (%s). Once linked, it moves this control instead; its other controls carry on as before."),
                       cap.captured().device.c_str());
    ImGui::PopTextWrapPos();
  }
  bool notes = linkctl::notes();
  if (ImGui::Checkbox(i18n::tr("Notes and pads too (for a button)"), &notes)) {
    linkctl::notes() = notes;
    cap.setNotes(notes);
  }
  ui::Hint("Off: the keys and pads are ignored while the dialog listens, so playing a note never links it");
  ImGui::Dummy(ImVec2(0, theme::kS1));
}

// k078-songlinks: what the INTERNAL CONTROLLER section showed on the last frame: "offered" (the
// controller choice), "note" (a song setting: the note only), "note-linked" (the note and an old
// link with its Remove button) or "none" (an action target has no such section).
std::string& internalShown() {
  static std::string s = "none";
  return s;
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

void remoteDialogApply(App& a) {
  if (!a.showRemoteDialog) return;
  saveLink(a);
  a.showRemoteDialog = a.remoteListening = a.midiLearning = false;
}

void remoteDialogCancel(App& a) { a.showRemoteDialog = a.remoteListening = a.midiLearning = false; }

void remoteDialogRemove(App& a) {
  if (a.showRemoteDialog && a.remoteEditIndex >= 0) removeLinkAt(a, a.remoteEditIndex);
  remoteDialogCancel(a);
}

void remoteDialogRemoveInternal(App& a) {
  const ControllerLink& l = a.remoteEdit;
  if (a.showRemoteDialog && l.action == RemoteAction::None) a.doc.removeModLink({l.owner, l.index});
}

std::string remoteDialogDescribe(App& a) {
  char buf[640];
  const LinkCapture& cap = linkctl::capture();
  std::snprintf(buf, sizeof buf, "open %d listening %d captured %d device \"%s\" source \"%s\" meter %.3f surface %d notes %d target \"%s\" internal %s modlinks %zu undo %d",
                a.showRemoteDialog ? 1 : 0, a.remoteListening ? 1 : 0, cap.captured().captures, cap.captured().device.c_str(),
                remoteSourceLabel(a.remoteEdit).c_str(), cap.captured().value, cap.captured().surface ? 1 : 0, linkctl::notes() ? 1 : 0,
                targetName(a, a.remoteEdit).c_str(), a.showRemoteDialog ? internalShown().c_str() : "none", a.doc.song().modLinks.size(),
                static_cast<int>(a.doc.history().position()));
  return buf;
}

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

  // Controller: what the dialog captured (k078-linkctl), then the source rows to edit by hand.
  ui::BeginSection("CONTROLLER");
  captureBox(a, l);
  label("Source");
  enumCombo("##src", &l.source, static_cast<int>(RemoteSource::Count), remoteSourceName, w);
  if (l.source == RemoteSource::Osc) {  // OSC: an address instead of a MIDI message
    static char buf[256];
    static const ControllerLink* last = nullptr;
    static std::string shown;
    if (last != &l || shown != l.oscAddress || ImGui::IsWindowAppearing()) std::snprintf(buf, sizeof buf, "%s", l.oscAddress.c_str());
    last = &l;
    label("OSC address");
    ImGui::SetNextItemWidth(w + 140);
    if (ImGui::InputTextWithHint("##oscaddr", "/1/fader1", buf, sizeof buf)) l.oscAddress = buf;
    shown = l.oscAddress;
    ui::Hint("The address the surface sends. Patterns such as /1/fader* are allowed. The first number of the message is used");
    if (!l.oscAddress.empty() && !osc::validAddress(l.oscAddress)) {
      ImGui::Dummy(ImVec2(150, 0));
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("An OSC address starts with / and has no spaces."));
    }
    if (!oscui::listening()) {
      ImGui::Dummy(ImVec2(150, 0));
      ImGui::SameLine();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("OSC is off: switch it on in Settings > MIDI."));
    }
  }
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
  if (l.source != RemoteSource::Osc) {
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
  }

  ui::EndSection();

  // GitHub #60: an internal controller (Envelope Mod, Peak Follower, X-Y Pad, a layer macro, ...)
  // can drive the same control. Chosen here, applied on Accept as the control's ModLink.
  struct Internal {
    bool chosen = false;  // the user picked something in the combo since the dialog opened
    bool none = true;
    ObjectId owner = kNoId;
    int slot = -1, output = 0;
    ControllerLink opened;  // the MIDI side as it opened (a new, untouched one is not saved with an internal pick)
  };
  static Internal in;
  const bool paramTarget = l.action == RemoteAction::None;
  const ParamId target{l.owner, l.index};
  if (ImGui::IsWindowAppearing()) {
    in = Internal{};
    // K-412: when opening for an existing internal controller link (with no MIDI link yet),
    // reflect its curve/invert/range/formula in the Mapping section so editing them works.
    if (paramTarget && a.remoteEditIndex < 0)
      if (const ModLink* cur = findModLink(a.doc.song(), l.owner, l.index)) {
        l.curve = cur->curve;
        l.invert = cur->invert;
        l.min = cur->min;
        l.max = cur->max;
        l.formula = cur->formula;
      }
    in.opened = l;
  }
  // Internal controller links apply to song settings too (tempo, swing, master pitch, groove
  // accents): the engine moves them live (AudioEngine::applyLinkValue).
  const bool internalOk = paramTarget && internalLinkApplies(a.doc.song(), target);
  internalShown() = !paramTarget ? "none" : internalOk ? "offered" : "note";
  if (paramTarget && !internalOk) {
    ui::BeginSection("INTERNAL CONTROLLER");
    if (const ModLink* cur = findModLink(a.doc.song(), l.owner, l.index)) {
      internalShown() = "note-linked";
      label("Controller");
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(controllerOutputName(a.doc.song(), cur->sourceOwner, cur->sourceSlot, cur->sourceOutput).c_str());
      ImGui::SameLine(0, theme::kS3);
      if (ui::TextButton("Remove", false, 80, "Removes this link")) remoteDialogRemoveInternal(a);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("This parameter cannot be linked to an internal controller."));
    ImGui::PopTextWrapPos();
    ui::EndSection();
  }
  if (internalOk) {
    ui::BeginSection("INTERNAL CONTROLLER");
    const ModLink* cur = findModLink(a.doc.song(), l.owner, l.index);
    const bool none = in.chosen ? in.none : cur == nullptr;
    const ObjectId so = in.chosen ? in.owner : cur ? cur->sourceOwner : kNoId;
    const int ss = in.chosen ? in.slot : cur ? cur->sourceSlot : -1, sout = in.chosen ? in.output : cur ? cur->sourceOutput : 0;
    const auto outs = listControllerOutputs(a.doc.song());
    if (cur && !none) internalShown() = "offered-linked";
    label("Controller");
    const std::string shown = none ? std::string(i18n::tr("None")) : controllerOutputName(a.doc.song(), so, ss, sout);
    if (ui::BeginCombo("##internal", shown.c_str(), w + (cur && !none ? 60.0f : 140.0f))) {
      if (ImGui::Selectable(i18n::tr("None"), none)) in.chosen = true, in.none = true;
      for (const auto& o : outs)
        if (ImGui::Selectable(o.label.c_str(), !none && o.owner == so && o.slot == ss && o.output == sout))
          in.chosen = true, in.none = false, in.owner = o.owner, in.slot = o.slot, in.output = o.output;
      ImGui::EndCombo();
    }
    ui::Hint("A controller in this project moves this control as it plays");
    if (cur && !none) {
      ImGui::SameLine(0, theme::kS2);
      if (ui::TextButton("Remove", false, 72, "Removes this internal controller link")) {
        remoteDialogRemoveInternal(a);
        in.chosen = true;
        in.none = true;
      }
    }
    if (target.owner == kSongId) {
      ImGui::Dummy(ImVec2(150, 0));
      ImGui::SameLine();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(theme::vec(!none ? P.accent : P.textFaint), "%s",
                         !none ? i18n::tr("Live internal controller link active on this song setting.")
                               : i18n::tr("Live internal controllers can drive this song setting directly."));
      ImGui::PopTextWrapPos();
    }
    if (outs.empty() && none) {
      ImGui::Dummy(ImVec2(150, 0));
      ImGui::SameLine();
      ImGui::PushTextWrapPos(0.0f);  // (wrapped: it is wider than the dialog)
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No controllers yet: add Envelope Mod, Peak Follower, X-Y Pad or Formula Mod first."));
      ImGui::PopTextWrapPos();
    }
    ui::EndSection();
  }

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
    d.oscAddress = l.oscAddress;
    d.owner = l.owner;
    d.index = l.index;
    d.action = l.action;
    l = d;
  }
  float right = ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(ImGui::GetCursorPosX() + right - 96 * 2 - theme::kS2);
  if (ui::TextButton("Cancel", false, 96) || ::gl::keys::escapePressed()) close = true;
  ImGui::SameLine(0, theme::kS2);
  const bool oscOk = l.source != RemoteSource::Osc || osc::validAddress(l.oscAddress);
  if (ui::PrimaryButton("Apply", 96, oscOk ? nullptr : "Enter or learn an OSC address first", oscOk)) {
    // GitHub #60 / K-412: the internal controller first (carrying the Mapping section's curve,
    // invert, min/max and formula onto the ModLink); a new MIDI link nobody set up is not saved with it.
    bool skipMidi = false;
    if (internalOk) {
      const ModLink* cur = findModLink(a.doc.song(), target.owner, target.index);
      const bool mappingChanged = l.curve != in.opened.curve || l.invert != in.opened.invert || l.min != in.opened.min ||
                                  l.max != in.opened.max || l.formula != in.opened.formula;
      if (in.chosen) {
        if (in.none) {
          if (cur) a.doc.removeModLink(target);
        } else {
          ModLink m = cur ? *cur : ModLink{};
          m.sourceOwner = in.owner, m.sourceSlot = in.slot, m.sourceOutput = in.output;
          m.owner = target.owner, m.index = target.index;
          m.curve = l.curve, m.invert = l.invert, m.min = l.min, m.max = l.max, m.formula = l.formula;
          if (!a.doc.setModLink(m)) a.notify("Cannot link this parameter", true);
        }
      } else if (cur && mappingChanged && a.remoteEditIndex < 0) {
        ModLink m = *cur;
        m.curve = l.curve, m.invert = l.invert, m.min = l.min, m.max = l.max, m.formula = l.formula;
        if (!a.doc.setModLink(m)) a.notify("Cannot link this parameter", true);
      }
      const bool midiUnconfigured = a.remoteEditIndex < 0 && !linkctl::capture().hasCapture() && l.source == in.opened.source &&
                                    l.cc == in.opened.cc && l.midiChannel == in.opened.midiChannel && l.port == in.opened.port &&
                                    l.oscAddress == in.opened.oscAddress && l.hiRes == in.opened.hiRes;
      const bool activeInternal = in.chosen ? !in.none : (cur != nullptr);
      skipMidi = activeInternal && midiUnconfigured;
    }
    if (!skipMidi) saveLink(a);
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
  const bool clips = livelook::active(a);  // N49-11: Clips maps a control to MIDI; no Multilink
  if (ImGui::MenuItem(clips ? i18n::tr("Edit MIDI map…") : idx >= 0 ? i18n::tr("Edit controller link…") : i18n::tr("Link to controller…")))
    a.openRemoteDialog(pid);
  if (idx >= 0) {
    ImGui::TextDisabled("  %s", remoteSourceLabel(links[static_cast<size_t>(idx)]).c_str());
    if (ImGui::MenuItem(i18n::tr("Remove controller link"))) removeLinkAt(a, idx);
  }
  if (a.extraParamMenu) a.extraParamMenu(pid);  // PH-09: host macros (plugin build)
  if (!clips && ImGui::MenuItem(i18n::tr("Multilink to controllers"), nullptr, a.multilink)) {
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

// K-123: one menu in quiet groups. K-325: the most used first (what records, punch and count-in,
// takes, notes), the rows set once and left (the file format, the punch crossfade, capture while
// armed, the latency check, monitoring, the record controls) under Advanced, then the controller link.
void recordOptionsMenu(App& a) {
  RecordOptions& o = a.recOptions;
  // N48-15 (checkpoint 49): Clips names the clip, and keeps the Studio-only rows (takes per pass, the
  // controller target, step record) out of its menu.
  const bool clips = livelook::active(a);
  if (ImGui::MenuItem(i18n::tr("Record window..."))) recordcard::openWindow(a);
  audioRecordFilterMenu(a);  // J-8: what to record (audio / audio editor / notes / automation)
  ImGui::SeparatorText(i18n::tr("Punch and count-in"));
  // Checkpoint 56 (N55-04): Clips punches at its loop brace, and keeps its own switch (clips.punch).
  if (ImGui::MenuItem(clips ? i18n::tr("Punch in/out at the loop brace") : i18n::tr("Punch in/out at the time selection"), nullptr, &a.recPunch) && clips) {
    a.settings.set("clips.punch", a.recPunch);
  }
  recmore::punchMenuItems(a);  // K-123 (0.7.7): quick punch
  // N48-15 (checkpoint 49): in Clips the Count-in switch is the row beside this submenu: here only its length.
  if (ImGui::BeginMenu(clips ? i18n::tr("Count-in length") : i18n::tr("Count-in"))) {
    const int bars[] = {0, 1, 2, 4};
    // Checkpoint 60 (N59-10): Clips' length is ticked whether the count-in is on or not (its switch
    // turns it on at this length).
    const int shown = clips && a.countInBars <= 0 ? std::max(1, a.countInLength) : a.countInBars;
    for (int b : bars) {
      if (clips && b == 0) continue;
      std::string n = b == 0 ? "Off" : std::to_string(b) + (b == 1 ? " bar" : " bars");
      if (ImGui::MenuItem(n.c_str(), nullptr, shown == b)) {
        if (b > 0) a.countInLength = b;
        if (!clips || a.countInBars > 0) a.countInBars = b;
      }
    }
    ImGui::EndMenu();
  }
  j31::recordingRollItems(a);
  recmore::startMenuItems(a);  // K-123 (0.7.7): start on the first note
  ImGui::SeparatorText(i18n::tr("Takes"));  // K-77: one record options menu (it was a second one in Options)
  j31::recordingOptionItems(a);
  ImGui::SeparatorText(i18n::tr("Notes"));
  if (ImGui::MenuItem(i18n::tr("Overdub (keep existing notes)"), nullptr, o.overdub)) o.overdub = true;
  if (ImGui::MenuItem(clips ? i18n::tr("Replace (clear the clip's notes)") : i18n::tr("Replace (clear the channel's notes)"), nullptr, !o.overdub)) o.overdub = false;
  ImGui::MenuItem(i18n::tr("Quantize note starts to the snap"), nullptr, &o.quantize);
  ImGui::MenuItem(i18n::tr("Quantize note lengths"), nullptr, &o.quantizeLength);
  // The rows Clips keeps out of its main list (K-314, C-02: still reachable there, under Advanced).
  auto studioRows = [&a, &o] {
    ImGui::MenuItem(i18n::tr("Loop recording: new take per pass"), nullptr, &o.takes);
    if (ImGui::MenuItem(i18n::tr("Step record (keyboard enters notes at the step cursor)"), nullptr, a.stepRecord)) {
      a.stepRecord = !a.stepRecord;
    }
    recordrows::recoverMenuItem(a);  // K-123 (4.2) (Clips: also its Capture button)
    if (ImGui::BeginMenu(i18n::tr("Controller moves go to"))) {
      for (int i = 0; i < static_cast<int>(AutoRecordTarget::Count); ++i) {
        if (ImGui::MenuItem(autoRecordTargetName(static_cast<AutoRecordTarget>(i)), nullptr, static_cast<int>(o.autoTarget) == i)) {
          o.autoTarget = static_cast<AutoRecordTarget>(i);
        }
      }
      ImGui::EndMenu();
    }
  };
  if (!clips) {
    studioRows();
  }
  ImGui::Separator();
  if (ImGui::BeginMenu(i18n::tr("Advanced"))) {  // K-325
    if (clips) {  // K-314 (C-02)
      ImGui::SeparatorText(i18n::tr("Takes and notes"));
      j31::recordingNamingItems(a);
      audioEditorRecordItem(a);
      studioRows();
      actionLinkMenuItems(a, RemoteAction::Record);
      ImGui::Separator();
    }
    audioFormatMenu(a);
    recmore::advancedMenuItems(a);  // K-123 (0.7.7): punch crossfade, capture while armed (and recover), the latency check
    ImGui::SeparatorText(i18n::tr("Monitoring"));
    j31::recordingMonitorItems(a);
    ImGui::Separator();
    j31::recordingControlsMenu(a);
    ImGui::EndMenu();
  }
  ui::Hint("The audio file format, the punch crossfade, capture while armed, the latency check, monitoring and when the record controls open");
  if (!clips) actionLinkMenuItems(a, RemoteAction::Record);  // N49-12: Clips maps controls from its MIDI map
}

TypingLayout typingLayout = TypingLayout::Computer;

void loadTypingLayout(const Settings& s) {
  typingLayout = keycaps::capsFromId(s.getString("keys.myKeys"));
  keycaps::setCaps(typingLayout);
}

bool typingKeyboardAtStart(const Settings& s) {
  const std::string at = s.getString("keys.typingAtStart");
  if (at == "on") return true;
  if (at == "off") return false;
  return s.getBool("keys.typingOn");  // "last", and anything unknown: as it was left
}

void setTypingLayout(App& a, TypingLayout l) {
  typingLayout = l < TypingLayout::Count ? l : TypingLayout::Computer;
  keycaps::setCaps(typingLayout);
  a.settings.set("keys.myKeys", keycaps::capsId(typingLayout));
}

std::string typingKeyName(int key) {
  const int position = TypingKeyboard::positionOfKey(key);
  const int typed = typingLayout == TypingLayout::Computer ? keycaps::systemChar(position) : TypingKeyboard::layoutChar(typingLayout, position);
  return TypingKeyboard::keyLabel(key, typed);
}

void myKeysItems(App& a) {
  for (TypingLayout l : keycaps::kCapsOrder)
    if (ImGui::MenuItem(i18n::tr(keycaps::capsName(l)), nullptr, typingLayout == l)) setTypingLayout(a, l);
}

void typingKeyboardMenu(App& a) {
  TypingKeyboard& k = a.typing;
  ImGui::TextDisabled("%s", i18n::tr("Typing keyboard"));
  // N48-15: Clips shows the switch once, beside this submenu. N48-08: its note keys start at A.
  const bool clips = livelook::active(a);
  if (!clips && ImGui::MenuItem(i18n::tr("Play notes from the computer keyboard"), keys::label("options.typingKeyboard"), a.typingKeyboard))
    a.typingKeyboard = !a.typingKeyboard;
  if (ImGui::BeginMenu(i18n::tr("Octave"))) {
    for (int o = 1; o <= 8; ++o) {
      std::string n = typingKeyName(k.aRow ? 'a' : 'z') + " = " + midiNoteName(k.baseNoteAt(o));
      if (ImGui::MenuItem(n.c_str(), nullptr, k.octave == o)) k.octave = o;
    }
    ImGui::EndMenu();
  }
  ImGui::SetNextItemWidth(160);
  ImGui::SliderInt(i18n::tr("Velocity"), &k.velocity, 1, 127);
  ImGui::MenuItem(i18n::tr("Chord mode"), k.aRow ? nullptr : typingKeyName('\\').c_str(), &k.chordMode);  // (no chord key on the A row)
  if (ImGui::BeginMenu(i18n::tr("Chord"))) {
    for (int i = 0; i < static_cast<int>(ChordType::Count); ++i)
      if (ImGui::MenuItem(chordTypeName(static_cast<ChordType>(i)), nullptr, static_cast<int>(k.chord) == i)) k.chord = static_cast<ChordType>(i);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("My keys say"))) {  // K-113, GitHub #79: shared with the names of shortcuts
    myKeysItems(a);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (k.aRow) {  // N48-08 (Clips), R06-15 (Smooth): the A row
    ImGui::TextDisabled(i18n::tr("%s…%s play · %s %s octave · %s %s velocity"), typingKeyName('a').c_str(), typingKeyName('\'').c_str(),
                        typingKeyName('z').c_str(), typingKeyName('x').c_str(), typingKeyName('c').c_str(), typingKeyName('v').c_str());
    return;
  }
  ImGui::TextDisabled(i18n::tr("%s…%s and %s…%s play · %s %s octave · %s %s velocity"), typingKeyName('z').c_str(), typingKeyName('/').c_str(),
                      typingKeyName('q').c_str(), typingKeyName('p').c_str(), typingKeyName('[').c_str(), typingKeyName(']').c_str(),
                      typingKeyName('-').c_str(), typingKeyName('=').c_str());
  ImGui::TextDisabled(i18n::tr("%s chord mode · %s next chord"), typingKeyName('\\').c_str(), typingKeyName('\'').c_str());
}

// ------------------------------------------------------------ settings page ----

namespace {
// The system MIDI service hung or refused: what happened, that everything else works, and
// when Gridline tries again (on its own, or now with Retry).
void midiServiceNotice(MidiManager& m) {
  const auto& P = pal();
  const MidiOpenState st = m.serviceState();
  if (st == MidiOpenState::Ready || st == MidiOpenState::Idle) return;
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (st == MidiOpenState::Opening) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("Looking for MIDI devices…"));
    ImGui::Dummy(ImVec2(0, theme::kS1));
    return;
  }
  const int wait = static_cast<int>(std::ceil(std::max(0.0, m.serviceRetryIn())));
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("MIDI devices are unavailable."));
  if (st == MidiOpenState::Failed)
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("The system MIDI service refused to open: %s"), m.serviceError().c_str());
  else
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("The system MIDI service is not responding."));
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     i18n::tr("Gridline carries on without MIDI devices: audio, projects and the typing keyboard work as usual."));
  ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("It tries again on its own in %d s. Tries so far: %d."), wait, m.serviceAttempts());
  ImGui::PopTextWrapPos();
  if (ui::TextButton("Retry now", false, 0, "Asks the system MIDI service again")) m.retry();
  ImGui::Dummy(ImVec2(0, theme::kS1));
}
}  // namespace

void midiSettingsPage(App& a, float fieldW) {
  const auto& P = pal();
  ImGui::Dummy(ImVec2(0, theme::kS2));
  {  // K-113: the typing keyboard needs no MIDI device
    ui::BeginSection("TYPING KEYBOARD");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("Keys play by where they sit. %s…%s play, %s %s octave, %s %s velocity."),
                       typingKeyName('z').c_str(), typingKeyName('/').c_str(), typingKeyName('[').c_str(), typingKeyName(']').c_str(),
                       typingKeyName('-').c_str(), typingKeyName('=').c_str());
    // GitHub #79: what the keys are called is "My keys say", beside the other key settings.
    ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("Their names follow Settings > Shortcuts > My keys say (%s)."),
                       i18n::tr(keycaps::capsName(typingLayout)));
    ImGui::PopTextWrapPos();
    label(i18n::tr("When Gridline starts"), 168);
    {
      static const char* const kIds[3] = {"last", "on", "off"};
      static const char* const kNames[3] = {i18n::N_("As it was left"), i18n::N_("On"), i18n::N_("Off")};
      const std::string cur = a.settings.getString("keys.typingAtStart");
      int sel = 0;
      for (int i = 0; i < 3; ++i)
        if (cur == kIds[i]) sel = i;
      if (ui::BeginCombo("##typingatstart", i18n::tr(kNames[sel]), std::max(240.0f, fieldW))) {
        for (int i = 0; i < 3; ++i)
          if (ImGui::Selectable(i18n::tr(kNames[i]), sel == i)) a.settings.set("keys.typingAtStart", std::string(kIds[i]));
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Whether the typing keyboard plays notes when Gridline opens"));
    }
    ui::EndSection();
  }
  if (!a.midi) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("MIDI is not available."));
    oscui::settingsSection(a, fieldW);  // OSC works without MIDI devices
    return;
  }
  MidiManager& m = *a.midi;
  bool changed = false, reopen = false;
  const float colW = std::max(240.0f, fieldW);

  // Input devices table.
  ui::BeginSection("INPUT DEVICES");
  ui::SectionFullWidth();  // K-112b: the device table uses the whole width
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Notes play the selected channel; controls drive links and the device template."));
  midiServiceNotice(m);
  if (m.inputs.empty() && m.serviceAvailable()) {
    ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No MIDI inputs found."));
    if (const char* hint = chromeos::midiHint(chromeos::current()); *hint) {  // K-75
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr(hint));
      ImGui::PopTextWrapPos();
    }
  }
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
      const surfaceui::Type* autoS = c->controllerType.empty() ? surfaceui::typeOf(a, name) : nullptr;  // a pad controller
      const std::string autoFlkey = flkeyAutoName(name);  // k078-flkey: an FLkey's DAW port is found by name
      std::string preview = c->controllerType.empty()
                                ? std::string("Auto: ") + (autoS ? autoS->label : !autoFlkey.empty() ? autoFlkey : autoT ? autoT->name : "none")
                            : c->controllerType == "none" ? std::string("(none)")
                                                          : [&]() {
                                                              if (c->controllerType.rfind("script:", 0) == 0) {  // J-24
                                                                for (const auto& s : controllerScripts(a))
                                                                  if ("script:" + s.id == c->controllerType) return "Script: " + s.name;
                                                                return "Script: " + c->controllerType.substr(7) + " (missing)";
                                                              }
                                                              for (const auto& [id, label] : mackieControllerTypes())  // K-214
                                                                if (id == c->controllerType) return label;
                                                              if (const surfaceui::Type* st = surfaceui::findType(c->controllerType)) return st->label;
                                                              for (const auto& [id, label] : flkeyControllerTypes())  // k078-flkey
                                                                if (id == c->controllerType) return label;
                                                              auto* t = findControllerTemplate(a.controllerTemplates, c->controllerType);
                                                              return t ? t->name : c->controllerType;
                                                            }();
      if (ui::BeginCombo("##type", preview.c_str(), 186)) {
        if (ImGui::Selectable(i18n::tr("Automatic (by device name)"), c->controllerType.empty())) c->controllerType.clear(), changed = true;
        if (ImGui::Selectable(i18n::tr("(none)"), c->controllerType == "none")) c->controllerType = "none", changed = true;
        for (const auto& t : a.controllerTemplates)
          if (ImGui::Selectable(t.name.c_str(), c->controllerType == t.id)) c->controllerType = t.id, changed = true;
        // K-214: Mackie Control / HUI surfaces (the protocol, not a template).
        ImGui::SeparatorText(i18n::tr("Control surfaces"));
        for (const auto& [id, label] : mackieControllerTypes())
          if (ImGui::Selectable((label + "##" + id).c_str(), c->controllerType == id)) c->controllerType = id, changed = true;
        for (const surfaceui::Type& st : surfaceui::types())  // pad controllers with a DAW mode (Surfaces.h)
          if (ImGui::Selectable((st.label + "##" + st.id).c_str(), c->controllerType == st.id)) c->controllerType = st.id, changed = true;
        // k078-flkey: Novation FLkey keyboards (pick it on the keyboard's DAW port; found by name otherwise).
        ImGui::SeparatorText(i18n::tr("Keyboards with a DAW port"));
        for (const auto& [id, label] : flkeyControllerTypes())
          if (ImGui::Selectable((label + "##" + id).c_str(), c->controllerType == id)) c->controllerType = id, changed = true;
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
  if (surfaceui::settings(a)) changed = true;  // each pad controller's options
  if (ui::TextButton("Rescan devices")) {
    m.scan();
    surfaceui::rescanned();
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
  if (m.serviceAvailable()) ImGui::TextColored(theme::vec(P.textFaint), "%s", m.status.c_str());  // else the notice above says it

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
  if (ImGui::Checkbox(i18n::tr("Sustain holds notes (CC 64)"), &r.sustain)) changed = true;
  ui::Hint(i18n::tr("On: Gridline holds the notes while the sustain pedal is down. Off: notes end when their keys lift and the "
                    "instrument decides what the pedal does (piano plugins that model their own pedal)."));
  ImGui::SameLine(0, theme::kS3);
  if (ImGui::Checkbox(i18n::tr("Sostenuto (CC 66)"), &r.sostenuto)) changed = true;
  ImGui::SameLine(0, theme::kS3);
  if (ImGui::Checkbox(i18n::tr("Soft (CC 67)"), &r.soft)) changed = true;
  // The value sits beside the slider: inside it, the handle covered it.
  label(i18n::tr("Soft velocity"), 168);
  ImGui::SetNextItemWidth(110);
  float softPct = r.softFactor * 100;
  if (ImGui::SliderFloat("##softf", &softPct, 10, 100, "")) r.softFactor = softPct / 100, changed = true;
  ImGui::SameLine(0, theme::kS2);
  ImGui::Text("%.0f%%", static_cast<double>(r.softFactor * 100));
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textFaint), "%s",
                     i18n::tr("The sustain pedal, aftertouch and pitch bend always reach the instrument. With Sustain holds notes off, "
                              "each note ends when its key lifts and the instrument decides what the pedal does."));
  ImGui::PopTextWrapPos();
  label("MPE", 168);  // K-144
  if (ImGui::Checkbox(i18n::tr("MPE controller"), &r.mpe)) changed = true;
  if (r.mpe) {
    ImGui::SameLine(0, theme::kS3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr("Note bend range"));
    ImGui::SameLine(0, theme::kS2);
    ImGui::SetNextItemWidth(110);
    if (ImGui::SliderFloat("##mpebend", &r.mpeBendRange, 1, 96, "")) changed = true;
    ImGui::SameLine(0, theme::kS2);
    ImGui::Text(i18n::tr("%.0f semitones"), static_cast<double>(r.mpeBendRange));
  }
  ImGui::TextColored(theme::vec(P.textFaint), "%s",
                     i18n::tr("With MPE, channel 1 acts on the whole instrument and channels 2 to 16 each move their own note: bend, pressure and slide (CC 74)."));
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

  oscui::settingsSection(a, fieldW);

  if (reopen) m.apply();
  if (changed) {
    a.publishMidi();
    m.save((fs::path(a.configDir()) / "midi.json").string());
  }
}

}  // namespace gl
