// Gridline — K-169: the layer editor (see LayerEditor.h).
#include "LayerEditor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/LayerRack.h"
#include "../core/NoteTools.h"
#include "../core/Params.h"
#include "../core/Presets.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"

namespace fs = std::filesystem;

namespace gl::layerui {

using theme::pal;

namespace {

DevState gDev, gBuild;

constexpr float kRowH = 34.0f;
constexpr float kLeftW = 352.0f;  // grip, colour, name, mute, solo, level, pan, transpose
constexpr float kEditorW = 960.0f;

// Text drawn by the editor itself, recorded for the overlap check.
struct TextBox {
  ImVec2 a, b;
  std::string s;
};
std::vector<TextBox> gTexts;

void text(ImDrawList* dl, ImVec2 p, ImU32 col, const std::string& s, float size = 0, ImFont* font = nullptr) {
  if (s.empty()) return;
  if (!font) font = ImGui::GetFont();
  if (size <= 0) size = ImGui::GetFontSize();
  const ImVec2 sz = font->CalcTextSizeA(size, 1e9f, 0, s.c_str());
  dl->AddText(font, size, p, col, s.c_str());
  gTexts.push_back({p, ImVec2(p.x + sz.x, p.y + sz.y), s});
}
// The last ImGui item's label as a text box (buttons, segmented controls).
void noteItem(const char* what) {
  gTexts.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), what});
}

void finishAudit() {
  gBuild.texts = static_cast<int>(gTexts.size());
  gBuild.overlaps = 0;
  gBuild.overlapping.clear();
  for (size_t i = 0; i < gTexts.size(); ++i)
    for (size_t j = i + 1; j < gTexts.size(); ++j) {
      const TextBox &x = gTexts[i], &y = gTexts[j];
      const float ox = std::min(x.b.x, y.b.x) - std::max(x.a.x, y.a.x), oy = std::min(x.b.y, y.b.y) - std::max(x.a.y, y.a.y);
      if (ox > 0.5f && oy > 0.5f) {
        ++gBuild.overlaps;
        if (gBuild.overlapping.size() < 8) gBuild.overlapping.push_back(x.s + " | " + y.s);
      }
    }
  gBuild.frame = ImGui::GetFrameCount();
  gDev = gBuild;
}

// Per-editor interaction state (one layer editor is open at a time).
struct Ui {
  ObjectId layer = kNoId;
  bool velocityView = false;
  int selMacro = 0;
  int learning = -1;          // macro waiting for a control to be moved
  uint64_t learnSerial = 0;
  // zone drag
  ObjectId dragChild = kNoId;
  int dragPart = 0;           // 1 body, 2 low edge, 3 high edge, 4 low fade, 5 high fade
  LayerZone dragFrom;
  float dragX0 = 0;
  char saveName[96] = "";
  bool saveOpen = false;
  char macroName[32] = "";
  int renaming = -1;
};
Ui gUi;

std::string macroLabel(const Channel& l, int k) {
  const LayerMacro& m = l.macros[static_cast<size_t>(k)];
  return m.name.empty() ? std::string(i18n::tr("Macro")) + " " + std::to_string(k + 1) : m.name;
}

std::string presetRootOf(App& a) { return presetRoot(a.configDir()); }

void loadPresetJson(App& a, const Json& j, const std::string& name) {
  std::string err;
  const ObjectId l = layer::loadPreset(a.doc, j, -1, &err);
  if (l == kNoId) {
    a.notify(err.empty() ? std::string(i18n::tr("Cannot load this layer preset")) : err, true);
    return;
  }
  a.selectChannel(l);
  a.settingsChannel = l;
  a.notify(std::string(i18n::tr("Added the layer ")) + name);
}

// ------------------------------------------------------------------ toolbar

void toolbar(App& a, const Channel& l, float width) {
  const ObjectId id = l.id;
  const float x0 = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  ui::Label("MODE");
  noteItem("MODE");
  ImGui::SameLine(0, theme::kS2);
  {
    const char* modes[] = {"All", "Random", "Cycle", "No repeat"};
    const char* hints[] = {"Every child that holds the note plays it", "One child, picked at random, plays each note",
                           "The children take turns, one note each", "A random child each note, never the same one twice in a row"};
    int m = l.layerMode;
    if (ui::Segmented("##lmode", modes, 4, &m, 82, 0, hints)) layer::setMode(a.doc, id, m);
    noteItem("modes");
  }
  ImGui::SameLine(0, theme::kS4);
  ImGui::AlignTextToFramePadding();
  ui::Label("ZONES");
  noteItem("ZONES");
  ImGui::SameLine(0, theme::kS2);
  {
    const char* views[] = {"Keys", "Velocity"};
    const char* hints[] = {"Show and drag the key zones", "Show and drag the velocity zones"};
    int v = gUi.velocityView ? 1 : 0;
    if (ui::Segmented("##lview", views, 2, &v, 76, 0, hints)) gUi.velocityView = v == 1;
    noteItem("views");
  }
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Split", false, 0, gUi.velocityView ? "Give each child its own velocity band, soft to hard" : "Give each child its own part of the keyboard, low to high")) {
    if (gUi.velocityView) layer::splitVelocity(a.doc, id, 8);
    else layer::splitKeys(a.doc, id, 2);
  }
  noteItem("Split");
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Reset", false, 0, "Every child plays the whole keyboard at every velocity again")) layer::resetZones(a.doc, id);
  noteItem("Reset");
  // Right side: presets.
  const float loadW = ui::TextButtonWidth("Load layer"), saveW = ui::TextButtonWidth("Save layer...");
  ImGui::SameLine(0, 0);
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + theme::kS2, x0 + width - loadW - saveW - theme::kS2));
  if (ui::TextButton("Load layer", false, 0, "Add a saved or built-in layer with its channels")) ImGui::OpenPopup("##layerload");
  noteItem("Load");
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Save layer...", false, 0, "Save this layer with its children, their instruments, effects, zones and macros")) {
    std::snprintf(gUi.saveName, sizeof gUi.saveName, "%s", l.name.c_str());
    gUi.saveOpen = true;
    ImGui::OpenPopup("##layersave");
  }
  noteItem("Save");
  if (ImGui::BeginPopup("##layerload")) {
    ui::Label("BUILT IN");
    for (const auto& [name, j] : layer::factoryPresets())
      if (ImGui::MenuItem(i18n::tr(name.c_str()))) loadPresetJson(a, j, name);
    const auto user = layer::listPresets(presetRootOf(a));
    ui::Label("SAVED");
    if (user.empty()) ImGui::TextDisabled("%s", i18n::tr("No saved layers yet"));
    for (const auto& e : user) {
      const std::string label = (e.folder.empty() ? e.name : e.folder + "/" + e.name) + "##" + e.file;
      if (ImGui::MenuItem(label.c_str())) {
        Json j;
        std::string err;
        if (layer::readPreset(e.file, j, &err)) loadPresetJson(a, j, e.name);
        else a.notify(err, true);
      }
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("##layersave")) {
    ui::Label("LAYER PRESET NAME");
    ImGui::SetNextItemWidth(260);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##lsname", gUi.saveName, sizeof gUi.saveName, ImGuiInputTextFlags_EnterReturnsTrue);
    std::vector<std::string> leftOut;
    layer::presetToJson(a.doc.song(), id, &leftOut);
    for (const auto& n : leftOut) ImGui::TextDisabled(i18n::tr("%s shares its mixer track: its effects are not saved"), n.c_str());
    if (ui::PrimaryButton("Save", 80) || enter) {
      const std::string name = safePresetName(gUi.saveName);
      std::string err;
      if (name.empty()) a.notify(i18n::tr("Type a name first"), true);
      else if (layer::savePreset(presetRootOf(a), a.doc.song(), id, name, &err)) a.notify(std::string(i18n::tr("Saved the layer ")) + name);
      else a.notify(std::string(i18n::tr("Cannot save the layer: ")) + err, true);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::TextButton("Cancel", false, 80)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

// ---------------------------------------------------------------- the lanes

// Ruler over the zone area: octaves (keys) or velocity marks.
void ruler(ImDrawList* dl, ImVec2 p, float w, float h) {
  const auto& P = pal();
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), P.surface800, 3);
  ImFont* mono = app().fontMono;
  const float fs = theme::kFontLabel;
  if (!gUi.velocityView) {
    const float kw = w / 128.0f;
    for (int k = 0; k < 128; ++k) {
      const int pc = k % 12;
      const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
      if (black) dl->AddRectFilled(ImVec2(p.x + k * kw, p.y + h - 6), ImVec2(p.x + (k + 1) * kw, p.y + h), theme::withAlpha(P.text, 0.18f));
      if (pc == 0) {
        const float x = std::round(p.x + k * kw);
        dl->AddLine(ImVec2(x, p.y + 2), ImVec2(x, p.y + h), P.lineStrong);
        if (k % 24 == 0 || w > 900) text(dl, ImVec2(x + 3, p.y + 1), P.textDim, tools::pitchName(k), fs, mono);
      }
    }
  } else {
    for (int v : {0, 32, 64, 96, 127}) {
      const float x = std::round(p.x + (static_cast<float>(v) + 0.5f) / 128.0f * w);
      dl->AddLine(ImVec2(x, p.y + 2), ImVec2(x, p.y + h), P.lineStrong);
      const std::string s = std::to_string(v);
      const float tw = mono->CalcTextSizeA(fs, 1e9f, 0, s.c_str()).x;
      text(dl, ImVec2(v == 127 ? x - tw - 3 : x + 3, p.y + 1), P.textDim, s, fs, mono);
    }
  }
}

void zoneBar(App& a, const Channel& l, const Channel& c, ImVec2 p, float w, float h, DevState::Lane& dev) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const LayerZone z = layer::zoneOf(l, c.id);
  const bool vel = gUi.velocityView;
  const int lo = vel ? z.velLo : z.keyLo, hi = vel ? z.velHi : z.keyHi;
  const int fLo = vel ? z.velFadeLo : z.keyFadeLo, fHi = vel ? z.velFadeHi : z.keyFadeHi;
  const float u = w / 128.0f;
  const float x0 = p.x + lo * u, x1 = p.x + (hi + 1) * u;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), P.well, 3);
  // The child's own key range (keys view): a dim band behind the zone.
  if (!vel && !c.fullKeyRange())
    dl->AddRectFilled(ImVec2(p.x + c.keyLo * u, p.y + h - 4), ImVec2(p.x + (c.keyHi + 1) * u, p.y + h - 1), theme::withAlpha(P.textFaint, 0.6f));
  const ImU32 col = theme::rgb(c.color, 0.85f), edge = theme::rgb(c.color);
  // The bar with its fades as slopes.
  const float top = p.y + 4, bot = p.y + h - 6;
  const float fl = x0 + fLo * u, fh = x1 - fHi * u;
  dl->PathClear();
  dl->PathLineTo(ImVec2(x0, bot));
  dl->PathLineTo(ImVec2(fl, top));
  dl->PathLineTo(ImVec2(fh, top));
  dl->PathLineTo(ImVec2(x1, bot));
  dl->PathFillConvex(theme::withAlpha(col, c.muted ? 0.3f : 0.75f));
  dl->AddLine(ImVec2(x0, bot), ImVec2(fl, top), edge, 1.5f);
  dl->AddLine(ImVec2(fl, top), ImVec2(fh, top), edge, 1.5f);
  dl->AddLine(ImVec2(fh, top), ImVec2(x1, bot), edge, 1.5f);
  // Fade handles: small squares on the top corners.
  dl->AddRectFilled(ImVec2(fl - 3, top - 3), ImVec2(fl + 3, top + 3), P.paper);
  dl->AddRectFilled(ImVec2(fh - 3, top - 3), ImVec2(fh + 3, top + 3), P.paper);
  dev = {p.x, p.y, p.x + w, p.y + h, x0, x1};
  // Range text inside the bar when it fits.
  {
    const std::string r = vel ? std::to_string(lo) + " - " + std::to_string(hi) : tools::pitchName(lo) + " - " + tools::pitchName(hi);
    ImFont* mono = app().fontMono;
    const float tw = mono->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, r.c_str()).x;
    if (fh - fl > tw + 10) text(dl, ImVec2(std::round((fl + fh - tw) * 0.5f), std::round(top + (bot - top - theme::kFontLabel) * 0.5f)), P.ink, r, theme::kFontLabel, mono);
  }
  // Interaction.
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton(("##zone" + std::to_string(c.id)).c_str(), ImVec2(w, h));
  const ImVec2 m = ImGui::GetIO().MousePos;
  auto partAt = [&]() {
    if (std::fabs(m.x - fl) < 6 && m.y < top + 6) return 4;
    if (std::fabs(m.x - fh) < 6 && m.y < top + 6) return 5;
    if (std::fabs(m.x - x0) < 5) return 2;
    if (std::fabs(m.x - x1) < 5) return 3;
    if (m.x > x0 && m.x < x1) return 1;
    return 0;
  };
  if (ImGui::IsItemHovered()) {
    const int part = gUi.dragChild == c.id ? gUi.dragPart : partAt();
    if (part >= 2) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (part == 1) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ui::Tooltip(i18n::tr("Drag the bar to move the zone, its ends to resize it, the corner squares to set the fades. Double-click: the whole range"));
  }
  if (ImGui::IsItemActivated()) {
    gUi.dragChild = c.id;
    gUi.dragPart = partAt();
    gUi.dragFrom = z;
    gUi.dragX0 = m.x;
    if (gUi.dragPart == 0) {  // a click outside the zone: the nearer end jumps there
      const int at = std::clamp(static_cast<int>((m.x - p.x) / u), 0, 127);
      gUi.dragPart = at < lo ? 2 : 3;
      LayerZone nz = z;
      (vel ? (gUi.dragPart == 2 ? nz.velLo : nz.velHi) : (gUi.dragPart == 2 ? nz.keyLo : nz.keyHi)) = at;
      layer::setZone(a.doc, l.id, nz, 0x4C5A000000000000ull ^ c.id);
      gUi.dragFrom = layer::zoneOf(*a.doc.song().channel(l.id), c.id);
    }
  }
  if (ImGui::IsItemActive() && gUi.dragChild == c.id) {
    const int d = static_cast<int>(std::lround((m.x - gUi.dragX0) / u));
    LayerZone nz = gUi.dragFrom;
    int &zlo = vel ? nz.velLo : nz.keyLo, &zhi = vel ? nz.velHi : nz.keyHi;
    int &zfl = vel ? nz.velFadeLo : nz.keyFadeLo, &zfh = vel ? nz.velFadeHi : nz.keyFadeHi;
    const int width = zhi - zlo;
    switch (gUi.dragPart) {
      case 1: {
        const int nlo = std::clamp(zlo + d, 0, 127 - width);
        zlo = nlo, zhi = nlo + width;
        break;
      }
      case 2: zlo = std::clamp(zlo + d, 0, zhi); break;
      case 3: zhi = std::clamp(zhi + d, zlo, 127); break;
      case 4: zfl = std::clamp(zfl + d, 0, width + 1); break;
      case 5: zfh = std::clamp(zfh - d, 0, width + 1); break;
      default: break;
    }
    layer::setZone(a.doc, l.id, nz, 0x4C5A000000000000ull ^ c.id);
  }
  if (ImGui::IsItemDeactivated()) {
    a.doc.endGesture();
    gUi.dragChild = kNoId;
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    LayerZone nz = z;
    if (vel) nz.velLo = 0, nz.velHi = 127, nz.velFadeLo = nz.velFadeHi = 0;
    else nz.keyLo = 0, nz.keyHi = 127, nz.keyFadeLo = nz.keyFadeHi = 0;
    layer::setZone(a.doc, l.id, nz);
  }
}

// A new instrument channel as a child of the layer `lid`, one undo step. Run through App::later:
// adding a channel may move the rack's channels, and the editor holds the layer's.
void addNewChild(App& a, ObjectId lid, const std::string& name, const std::string& type) {
  a.doc.beginGroup("add layer child");
  const ObjectId nc = a.doc.addChannel(name, type, a.doc.song().channelIndex(lid));
  if (nc != kNoId) layer::addChild(a.doc, lid, nc);
  a.doc.endGroup();
}

void lanes(App& a, const Channel& l, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float zoneX = start.x + kLeftW, zoneW = width - kLeftW;
  // Header: column captions + the ruler.
  const float hh = 18;
  ImFont* mono = app().fontMono;
  const float fs = theme::kFontLabel;
  auto cap = [&](float x, const char* t) { text(dl, ImVec2(start.x + x, start.y + 3), P.textFaint, i18n::tr(t), fs, mono); };
  cap(28, "CHILD");
  cap(178, "M");
  cap(204, "S");
  cap(226, "LEVEL");
  cap(268, "PAN");
  cap(300, "TRANSP");
  ruler(dl, ImVec2(zoneX, start.y), zoneW, hh);
  ImGui::Dummy(ImVec2(width, hh + 2));
  gBuild.lanes.clear();
  const std::vector<ObjectId> kids = l.layer;
  int dropAt = -1, dropFrom = -1;
  for (size_t i = 0; i < kids.size(); ++i) {
    const Channel* c = s.channel(kids[i]);
    if (!c) continue;
    const ObjectId kid = c->id;
    ImGui::PushID(static_cast<int>(kid));
    const ImVec2 r0 = ImGui::GetCursorScreenPos();
    const float cy = r0.y + kRowH * 0.5f;
    dl->AddRectFilled(r0, ImVec2(r0.x + kLeftW - 6, r0.y + kRowH - 2), i % 2 ? P.surface800 : P.surface700, 3);
    // Grip: drag to reorder.
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 2, r0.y + 6));
    ImGui::InvisibleButton("##grip", ImVec2(16, kRowH - 14));
    ui::drawIcon(dl, ui::Icon::GripDots, ImVec2(r0.x + 10, cy - 1), 14, ImGui::IsItemHovered() ? P.text : P.textFaint);
    ui::Hint("Drag to reorder the children (the order the Cycle mode follows)");
    if (ImGui::BeginDragDropSource()) {
      const int idx = static_cast<int>(i);
      ImGui::SetDragDropPayload("GL_LAYERKID", &idx, sizeof idx);
      ui::DragPreview(ui::Icon::GripDots, c->name.c_str());
      ImGui::EndDragDropSource();
    }
    // Colour chip and name (click opens the child's window).
    dl->AddRectFilled(ImVec2(r0.x + 22, r0.y + 8), ImVec2(r0.x + 28, r0.y + kRowH - 10), theme::rgb(c->color), 2);
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 32, r0.y + 4));
    ImGui::InvisibleButton("##name", ImVec2(138, kRowH - 10));
    if (ImGui::IsItemClicked()) a.settingsChannel = kid;
    ui::Hint("Open this child's window");
    {
      const PluginInfo* pi = findPlugin(c->plugin.type);
      dl->PushClipRect(ImVec2(r0.x + 32, r0.y), ImVec2(r0.x + 170, r0.y + kRowH), true);
      text(dl, ImVec2(r0.x + 32, r0.y + 3), ImGui::IsItemHovered() ? P.text : theme::mix(P.text, P.textDim, 0.3f), c->name);
      text(dl, ImVec2(r0.x + 32, r0.y + 18), P.textFaint, pi ? pi->name : std::string(i18n::tr("No instrument")), fs, mono);
      dl->PopClipRect();
      // The overlap check counts the clipped boxes only.
      for (size_t t = gTexts.size() - 2; t < gTexts.size(); ++t) gTexts[t].b.x = std::min(gTexts[t].b.x, r0.x + 170);
    }
    // Activity lamp.
    float peak = 0;
    if (auto rt = a.builder.channelRuntime(kid)) peak = rt->peak.load(std::memory_order_relaxed);
    ui::Led(dl, ImVec2(r0.x + 166, cy), 3, peak > 1e-4f, std::min(1.0f, peak * 2));
    // Mute / solo.
    const LayerZone z = layer::zoneOf(l, kid);
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 174, cy - 10));
    if (ui::TextButton(c->muted ? "M##m" : "M##u", c->muted, 22, "Mute this child (its channel)"))
      a.doc.editChannel(kid, "mute channel", [](Channel& x) { x.muted = !x.muted; });
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 200, cy - 10));
    if (ui::TextButton(z.solo ? "S##s" : "S##n", z.solo, 22, "Solo this child inside the layer (the other children go quiet)")) {
      LayerZone nz = z;
      nz.solo = !nz.solo;
      layer::setZone(a.doc, l.id, nz);
    }
    // Level / pan knobs (the child's channel strip).
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 232, cy - 12));
    ui::ParamKnob("##lvl", channelParam(kid, ChannelParam::Volume), 12);
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 268, cy - 12));
    ui::ParamKnob("##pan", channelParam(kid, ChannelParam::Pan), 12, nullptr, true);
    // Transpose (the layer's notes only).
    ImGui::SetCursorScreenPos(ImVec2(r0.x + 300, cy - ImGui::GetFrameHeight() * 0.5f));
    int tr = z.transpose;
    ImGui::SetNextItemWidth(44);
    if (ImGui::DragInt("##tr", &tr, 0.15f, -48, 48, tr > 0 ? "+%d" : "%d")) {
      LayerZone nz = z;
      nz.transpose = std::clamp(tr, -48, 48);
      layer::setZone(a.doc, l.id, nz, 0x4C54000000000000ull ^ kid);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    ui::Hint("Transpose: semitones added to the notes this child plays from the layer");
    // Zone bar.
    DevState::Lane dev;
    zoneBar(a, l, *c, ImVec2(zoneX, r0.y + 1), zoneW, kRowH - 4, dev);
    gBuild.lanes.push_back(dev);
    // Row context menu and drop target (reorder).
    ImGui::SetCursorScreenPos(r0);
    ImGui::Dummy(ImVec2(kLeftW - 6, kRowH));
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_LAYERKID")) {
        dropFrom = *static_cast<const int*>(pl->Data);
        dropAt = static_cast<int>(i);
      }
      ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##lanemenu")) {
      if (ImGui::MenuItem(i18n::tr("Open"))) a.settingsChannel = kid;
      if (ImGui::MenuItem(i18n::tr("Remove from the layer"))) layer::removeChild(a.doc, l.id, kid);
      if (ImGui::MenuItem(i18n::tr("Duplicate")))  // next frame: a new channel may move the rack's channels, `l` among them
        a.later([&a, lid = l.id, kid, z] {
          a.doc.beginGroup("duplicate layer child");
          const ObjectId cl = a.doc.cloneChannel(kid);
          if (cl != kNoId) {
            layer::addChild(a.doc, lid, cl);
            LayerZone nz = z;
            nz.child = cl;
            layer::setZone(a.doc, lid, nz);
          }
          a.doc.endGroup();
        });
      ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + kRowH));
    ImGui::PopID();
  }
  if (dropFrom >= 0 && dropAt >= 0) layer::moveChild(a.doc, l.id, dropFrom, dropAt);
  // The add row: drop a rack channel or an instrument here, or pick one.
  const ImVec2 r0 = ImGui::GetCursorScreenPos();
  const float addH = 30;
  const bool full = l.layer.size() >= static_cast<size_t>(kMaxLayerChildren);
  ImGui::InvisibleButton("##adddrop", ImVec2(width, addH));
  const bool hov = ImGui::IsItemHovered() || (ImGui::GetDragDropPayload() && ImGui::IsMouseHoveringRect(r0, ImVec2(r0.x + width, r0.y + addH)));
  dl->AddRect(ImVec2(r0.x + 0.5f, r0.y + 3.5f), ImVec2(r0.x + width - 0.5f, r0.y + addH - 0.5f), hov ? P.accent : P.line, 4.0f, 0, 1.0f);
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_CHANNEL")) {
      const ObjectId cid = *static_cast<const ObjectId*>(pl->Data);
      if (!layer::addChild(a.doc, l.id, cid)) a.notify(i18n::tr("This channel cannot join the layer"), true);
    }
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PLUGIN")) {
      const std::string type(static_cast<const char*>(pl->Data), static_cast<size_t>(pl->DataSize));
      const PluginInfo* pi = findPlugin(type);
      if (pi && pi->kind == PluginKind::Generator && !full)
        a.later([&a, lid = l.id, name = pi->name, type] { addNewChild(a, lid, name, type); });  // next frame, as above
    }
    ImGui::EndDragDropTarget();
  }
  const std::string hint = full ? std::string(i18n::tr("The layer is full (32 children)"))
                                : std::string(i18n::tr("Drop an instrument or a rack channel here to add a child"));
  text(dl, ImVec2(r0.x + 12, r0.y + 3 + (addH - 3 - ImGui::GetFontSize()) * 0.5f), P.textDim, hint);
  ImGui::SetCursorScreenPos(ImVec2(r0.x + width - ui::TextButtonWidth("Add child") - 6, r0.y + 3 + (addH - 3 - ImGui::GetFrameHeight()) * 0.5f));
  if (ui::TextButton("Add child", false, 0, "Add a channel of the rack or a new instrument as a child") && !full) ImGui::OpenPopup("##addkid");
  noteItem("Add child");
  if (ImGui::BeginPopup("##addkid")) {
    ui::Label("RACK CHANNELS");
    for (const auto& o : s.channels) {
      if (o.id == l.id || o.isLayer() || std::find(l.layer.begin(), l.layer.end(), o.id) != l.layer.end()) continue;
      if (ImGui::MenuItem((o.name + "##k" + std::to_string(o.id)).c_str())) layer::addChild(a.doc, l.id, o.id);
    }
    if (ImGui::BeginMenu(i18n::tr("New instrument"))) {
      std::string cat;
      for (const auto& p : pluginCatalog()) {
        if (p.kind != PluginKind::Generator || p.hidden) continue;
        if (p.category != cat) {
          cat = p.category;
          ui::Label(cat.c_str());
        }
        if (ImGui::MenuItem((p.name + "##n" + p.type).c_str()))
          a.later([&a, lid = l.id, name = p.name, type = p.type] { addNewChild(a, lid, name, type); });  // next frame, as above
      }
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }
  ImGui::SetCursorScreenPos(ImVec2(r0.x, r0.y + addH + theme::kS2));
}

// ---------------------------------------------------------------- the macros

void macros(App& a, const Channel& l, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* mono = app().fontMono;
  const float fs = theme::kFontLabel;
  // Learn: the next control moved anywhere becomes a target of the macro.
  if (gUi.learning >= 0 && a.doc.touchSerial() != gUi.learnSerial) {
    const ParamId t = a.doc.lastTouchedParam();
    const bool own = t.owner == l.id && t.index >= kLayerMacroBase && t.index < kLayerMacroBase + kLayerMacros;
    if (!own && t.owner != kNoId) {
      if (layer::mapMacro(a.doc, l.id, gUi.learning, t)) a.notify(macroLabel(l, gUi.learning) + " \xE2\x86\x92 " + paramDisplayName(s, t));
      else a.notify(i18n::tr("This control cannot be linked to a macro"), true);
      gUi.learning = -1;
    } else {
      gUi.learnSerial = a.doc.touchSerial();
    }
  }
  const ImVec2 p = ImGui::GetCursorScreenPos();
  text(dl, ImVec2(p.x, p.y), P.textFaint, i18n::tr("MACROS"), fs, mono);
  ImGui::Dummy(ImVec2(width, fs + 4));
  const float cellW = std::floor(width / kLayerMacros);
  const ImVec2 row = ImGui::GetCursorScreenPos();
  const float r = 15;
  for (int k = 0; k < kLayerMacros; ++k) {
    ImGui::PushID(k);
    const float cx = row.x + k * cellW + cellW * 0.5f;
    const bool sel = gUi.selMacro == k;
    if (sel) dl->AddRectFilled(ImVec2(row.x + k * cellW + 2, row.y), ImVec2(row.x + (k + 1) * cellW - 2, row.y + 92), P.surface800, 4);
    ImGui::SetCursorScreenPos(ImVec2(cx - r, row.y + 6));
    ui::ParamKnob("##macro", layerMacroParam(l.id, k), r);
    if (ImGui::IsItemClicked()) gUi.selMacro = k;
    gBuild.macroX[k] = cx, gBuild.macroY[k] = row.y + 6 + r;
    // Name (double-click renames).
    const std::string name = macroLabel(l, k);
    const float maxW = cellW - 10;
    const ImVec2 np(row.x + k * cellW + 5, row.y + 6 + 2 * r + 4);
    if (gUi.renaming == k) {
      ImGui::SetCursorScreenPos(np);
      ImGui::SetNextItemWidth(maxW);
      if (ImGui::IsWindowFocused() && !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##mname", gUi.macroName, sizeof gUi.macroName, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) ||
          ImGui::IsItemDeactivated()) {
        layer::renameMacro(a.doc, l.id, k, gUi.macroName);
        gUi.renaming = -1;
      }
    } else {
      float tw = ImGui::CalcTextSize(name.c_str()).x;
      std::string shown = name;
      while (tw > maxW && shown.size() > 2) {
        shown.pop_back();
        tw = ImGui::CalcTextSize((shown + "...").c_str()).x;
        if (tw <= maxW) shown += "...";
      }
      tw = ImGui::CalcTextSize(shown.c_str()).x;
      text(dl, ImVec2(std::round(cx - tw * 0.5f), np.y + 2), sel ? P.text : P.textDim, shown);
      ImGui::SetCursorScreenPos(np);
      ImGui::InvisibleButton("##mnamebtn", ImVec2(maxW, ImGui::GetFontSize() + 4));
      if (ImGui::IsItemClicked()) gUi.selMacro = k;
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        gUi.renaming = k;
        std::snprintf(gUi.macroName, sizeof gUi.macroName, "%s", l.macros[static_cast<size_t>(k)].name.c_str());
      }
      ui::Hint("Click: show the macro's targets below. Double-click: rename");
    }
    // Map button and the count of targets.
    const int n = static_cast<int>(layer::macroLinks(s, l.id, k).size());
    const bool learning = gUi.learning == k;
    const char* mapLabel = learning ? "Move a control" : "Map";
    const float bw = std::min(maxW, ui::TextButtonWidth(mapLabel));
    ImGui::SetCursorScreenPos(ImVec2(std::round(cx - bw * 0.5f), np.y + ImGui::GetFontSize() + 8));
    if (ui::TextButton(learning ? "Move a control##map" : "Map##map", learning, bw,
                       learning ? "Move any control of the children now; click again to stop" : "Link a parameter: move a control, or pick one from the list")) {
      gUi.selMacro = k;
      if (learning) gUi.learning = -1;
      else ImGui::OpenPopup("##mapmenu");
    }
    noteItem(learning ? "learn" : "Map");
    if (n > 0) {
      const std::string cnt = std::to_string(n);
      dl->AddCircleFilled(ImVec2(cx + r + 4, row.y + 8), 7, P.accent);
      const ImVec2 ts = mono->CalcTextSizeA(fs, 1e9f, 0, cnt.c_str());
      dl->AddText(mono, fs, ImVec2(std::round(cx + r + 4 - ts.x * 0.5f), std::round(row.y + 8 - ts.y * 0.5f)), P.paper, cnt.c_str());
    }
    if (ImGui::BeginPopup("##mapmenu")) {
      if (ImGui::MenuItem(i18n::tr("Learn: move a control now"))) {
        gUi.learning = k;
        gUi.learnSerial = a.doc.touchSerial();
      }
      ImGui::Separator();
      // Targets grouped by child.
      const std::vector<ParamId> targets = layer::macroTargets(s, l.id);
      ObjectId curOwner = kNoId;
      bool open = false;
      for (const ParamId& t : targets) {
        const Channel* oc = s.channel(t.owner);
        const ObjectId group = oc ? t.owner : t.owner;  // mixer slot params follow their child's group
        if (group != curOwner) {
          if (open) ImGui::EndMenu();
          curOwner = group;
          const std::string gname = oc ? oc->name : mixerTrackName(s, static_cast<size_t>(std::max(0, s.mixerIndex(t.owner))));
          open = ImGui::BeginMenu((gname + "##g" + std::to_string(group)).c_str());
        }
        if (!open) continue;
        const std::string pn = paramDisplayName(s, t);
        const size_t cut = pn.find(" \xE2\x80\xBA ");
        const std::string shortName = cut == std::string::npos ? pn : pn.substr(cut + 5);
        const ModLink* cur = findModLink(s, t.owner, t.index);
        const bool on = cur && cur->sourceOwner == l.id && cur->sourceSlot == kLayerMacroSlot && cur->sourceOutput == k;
        if (ImGui::MenuItem((shortName + "##t" + std::to_string(t.packed())).c_str(), nullptr, on)) {
          if (on) a.doc.removeModLink(t);
          else layer::mapMacro(a.doc, l.id, k, t);
        }
      }
      if (open) ImGui::EndMenu();
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 96));
  // The selected macro's targets with their ranges.
  const int k = gUi.selMacro;
  const auto links = layer::macroLinks(s, l.id, k);
  const ImVec2 lp = ImGui::GetCursorScreenPos();
  text(dl, lp, P.textFaint, macroLabel(l, k) + "  \xE2\x86\x92", fs, mono);
  ImGui::Dummy(ImVec2(width, fs + 4));
  if (links.empty()) {
    const ImVec2 q = ImGui::GetCursorScreenPos();
    text(dl, q, P.textDim, i18n::tr("No targets yet: press Map under the knob and move a control, or pick one from its list"));
    ImGui::Dummy(ImVec2(width, ImGui::GetFontSize() + 4));
  }
  std::vector<ParamId> remove;
  for (const ModLink* ml : links) {
    const ParamId t{ml->owner, ml->index};
    ImGui::PushID(static_cast<int>(t.packed() & 0x7FFFFFFF));
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    dl->PushClipRect(q, ImVec2(q.x + width - 300, q.y + h), true);
    text(dl, ImVec2(q.x + 4, q.y + (h - ImGui::GetFontSize()) * 0.5f), P.text, paramDisplayName(s, t));
    dl->PopClipRect();
    gTexts.back().b.x = std::min(gTexts.back().b.x, q.x + width - 300);
    float mn = ml->min * 100, mx = ml->max * 100;
    const uint64_t key = t.packed() ^ 0x4C4D000000000000ull;
    ImGui::SetCursorScreenPos(ImVec2(q.x + width - 292, q.y));
    ImGui::SetNextItemWidth(118);
    if (ImGui::SliderFloat("##min", &mn, 0, 100, i18n::tr("from %.0f%%"))) a.doc.editModLink(t, "change link range", [&](ModLink& x) { x.min = mn / 100; }, key ^ 1);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    ImGui::SameLine(0, theme::kS2);
    ImGui::SetNextItemWidth(118);
    if (ImGui::SliderFloat("##max", &mx, 0, 100, i18n::tr("to %.0f%%"))) a.doc.editModLink(t, "change link range", [&](ModLink& x) { x.max = mx / 100; }, key ^ 2);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    ImGui::SameLine(0, theme::kS2);
    if (ui::IconButton("##rm", ui::Icon::Close, false, h, "Remove this target")) remove.push_back(t);
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + h + 4));
    ImGui::PopID();
  }
  for (const ParamId& t : remove) a.doc.removeModLink(t);
  ImGui::Dummy(ImVec2(width, 2));
}

}  // namespace

float editorWidth() { return kEditorW; }

const DevState& devState() { return gDev; }

void drawEditor(App& a, ObjectId id, float width) {
  const Song& s = a.doc.song();
  const Channel* l = s.channel(id);
  if (!l) return;
  if (gUi.layer != id) {
    const bool vel = gUi.velocityView;
    gUi = Ui{};
    gUi.velocityView = vel;
    gUi.layer = id;
  }
  gTexts.clear();
  gBuild = DevState{};
  width = std::floor(std::max(720.0f, width));
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::PushID("layereditor");
  if (!l->isLayer()) {
    ImGui::TextDisabled("%s", i18n::tr("This channel has no children yet: add channels to make it a layer."));
    lanes(a, *l, width);
    ImGui::PopID();
    finishAudit();
    return;
  }
  toolbar(a, *l, width);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (const Channel* lc = a.doc.song().channel(id)) lanes(a, *lc, width);
  ImGui::Separator();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (const Channel* lc = a.doc.song().channel(id); lc && lc->isLayer()) macros(a, *lc, width);
  ImGui::PopID();
  const ImVec2 p1 = ImGui::GetCursorScreenPos();
  gBuild.minX = p0.x, gBuild.minY = p0.y, gBuild.maxX = p0.x + width, gBuild.maxY = p1.y;
  finishAudit();
}

}  // namespace gl::layerui
