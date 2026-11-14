// Gridline — "plasma" pack: the editor's frame (see plasma_ui.h): the top bar,
// the panels, the motion dock, the play bar, the keyboard drawer, and the
// view of the stage.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include "../../app/EditorZoomUi.h"
#include "../../app/ThemeSettings.h"
#include "../../app/Keys.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

void openView(Ui& u, int v) {
  if (isOverlay(v) && !isOverlay(u.view)) u.back = u.view;
  u.view = v;
}
void closeOverlay(Ui& u) {
  if (isOverlay(u.view)) u.view = isOverlay(u.back) ? VLayer : u.back;
}
void finishClose(Ui& u) {
  if (isOverlay(u.view)) closeOverlay(u);
  else if (u.large && u.view == VEffects) u.large = false;
  else u.view = VLayer, u.large = false;
  u.closing = false, u.expandT = 1, u.expandFrom = ImVec4(0, 0, 0, 0);
}
void beginClose(Ui& u) {
  // shrinks back into the block it came from (when it came from one and animations are on)
  if (u.expandFrom.z > u.expandFrom.x && !std::getenv("GL_PLASMA_VIEW") && !std::getenv("GL_PLASMA_VIEWS")) u.closing = true;
  else finishClose(u);
}

// Library helpers (plasma_browser_ui.cpp)
bool libraryStep(Ctx& c, int dir);
void saveAsButton(Ctx& c, const char* id, float x, float y, float w, float h, bool quiet = false);

namespace {

const PluginInfo* info() { return findPlugin(kType); }

ImU32 famCol(const Ctx& c, int src) { return c.P.fam[familyOf(std::max(src, 0))]; }

}  // namespace
int linksOf(const Ctx& c, int src) {
  int n = 0;
  for (int s = 0; s < kModSlots; ++s)
    if (vali(c, modP(s, M_Source)) == src && vali(c, modP(s, M_Dest)) > 0) ++n;
  return n;
}
int linksUsed(const Ctx& c) {
  int n = 0;
  for (int s = 0; s < kModSlots; ++s)
    if (vali(c, modP(s, M_Source)) > SrcNone && vali(c, modP(s, M_Dest)) > 0) ++n;
  return n;
}
namespace {

// ================================================================== top bar
void soundMenu(Ctx& c) {
  const PluginInfo* in = info();
  if (ImGui::MenuItem(i18n::tr("Init")))
    later(c, [](App& a, const PluginTarget& t) {
      PluginState ns = defaultPluginState(kType);
      Model m = defaultModel();
      m.presetName = "Init";
      ns.data = modelToData(m);
      setPluginState(a, t, ns, "Load preset");
    });
  if (in && ImGui::BeginMenu(i18n::tr("Factory sounds"))) {
    for (size_t i = 0; i < in->presets.size(); ++i)
      if (in->presets[i].name != "Init" && ImGui::MenuItem(in->presets[i].name.c_str())) {
        const auto v = in->presets[i].values;
        const std::string n = in->presets[i].name;
        Ctx* cp = &c;
        (void)cp;
        applyPreset(c, v, "", n);
      }
    ImGui::Separator();
    for (const auto& p : factoryDataPresets())
      if (ImGui::MenuItem(p.name.c_str())) applyPreset(c, p.values, p.data, p.name);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Open file..."))) openPresetDialog(c);
  if (ImGui::MenuItem(i18n::tr("Library"))) openView(c.u, VLibrary);
}

}  // namespace

namespace {
// The kind of sound, from the last word of its name ("Wobble Bass": Bass), else empty.
std::string soundKind(const std::string& name) {
  static const char* const kinds[] = {"Bass", "Lead", "Pad", "Pluck", "Keys", "Key", "Arp", "Chord", "Chords", "Bell", "Brass", "Strings", "Drum",
                                      "Kick", "Snare", "Hat", "Perc", "Fx", "FX", "Sub", "Synth", "Organ", "Vox", "Choir", "Drone", "Riser", "Texture"};
  const size_t sp = name.find_last_of(' ');
  const std::string last = sp == std::string::npos ? std::string() : name.substr(sp + 1);
  for (const char* k : kinds)
    if (last == k) return last;
  return {};
}

// The brand mark: a rounded square and a wave, in the accent.
void brandMark(const Ctx& c, float x, float y, float s) {
  const ImU32 a = c.P.accent;
  c.dl->AddRect(c.p(x + M(0.5f), y + M(0.5f)), c.p(x + s - M(0.5f), y + s - M(0.5f)), theme::withAlpha(a, 0.55f), c.s(M(6)), 0, std::max(1.f, c.s(M(1.2f))));
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 32; ++i) {
    const float u = 4 + 16 * i / 32.f;  // the mark's own 24-unit grid
    const float v = u <= 16 ? 12 - 4.5f * std::sin(3.14159f * (u - 4) / 6) : 12 - 2.f * std::sin(1.5708f * (u - 16) / 4);
    pts.push_back(c.p(x + s * u / 24, y + s * v / 24));
  }
  c.dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), a, 0, std::max(1.5f, c.s(M(2))));
}

// The theme's two halves (dark or light).
void themeGlyph(const Ctx& c, ImVec2 m, float r, ImU32 col) {
  c.dl->AddCircle(m, r, col, 20, std::max(1.f, c.s(M(1.3f))));
  c.dl->PathArcTo(m, r, -1.5708f, 1.5708f, 12);
  c.dl->PathFillConvex(col);
}

// A small vertical meter (the louder side).
void meterV(Ctx& c, float x, float y, float w, float h) {
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), c.P.field, c.s(M(2)));
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.line, c.s(M(2)));
  const float pk = c.hasLive ? std::max(c.live.outL, c.live.outR) : 0.f;
  const float db = pk > 1e-5f ? 20.f * std::log10(std::min(pk, 1.5f)) : -90.f;
  const float n = std::clamp((db + 48.f) / 51.f, 0.f, 1.f);
  if (n <= 0) return;
  const float top = y + h - (h - M(2)) * n;
  const ImU32 lo = c.P.good, hi = db > -0.5f ? c.P.danger : c.P.warn;
  c.dl->AddRectFilledMultiColor(c.p(x + M(1), top), c.p(x + w - M(1), y + h - M(1)), hi, hi, lo, lo);
}
}  // namespace

void topBar(Ctx& c) {
  // Simple / Full is the user's choice for every Plasma editor (a setting, not stored in the sound)
  if (!c.u.fullSet) c.u.full = c.a.settings.getBool("ui.plasmaFull"), c.u.fullSet = true;
  const Pal& P = c.P;
  panelRect(c, 0, 0, kW, kTopH, barBg(c));
  hline(c, 0, kW, kTopH - 0.5f, P.line);
  const std::string name = c.m.presetName.empty() ? std::string("Init") : c.m.presetName;
  if (pui::devHeaderPreset(c.t, nullptr, nullptr) != name) pui::presetLoaded(c.t, kType, name);
  const float cy = kTopH * 0.5f, bh = M(30), by = cy - bh * 0.5f;
  // the brand
  // the brand: Plasma's own wordmark (its icon and name), the vector drawing first, then the
  // in-house mark if the wordmark is missing
  if (pui::wordmarkAt(c.dl, kType, c.p(M(18), cy - M(14)), c.s(M(28)), P.text) <= 0) {
    brandMark(c, M(20), cy - M(12), M(24));
    text(c, M(20 + 24 + 9), cy - T(16) * 0.68f, "Plasma", P.text, T(16), true);
  }
  // the sound: arrows step through the library, the name opens the sound menu
  {
    const float x = M(152), w = M(268), aw = M(26);
    layoutBox(c, x, by, w, bh, "preset");
    c.dl->AddRectFilled(c.p(x, by), c.p(x + w, by + bh), P.field, c.s(M(4)));
    c.dl->AddRect(c.p(x, by), c.p(x + w, by + bh), P.line, c.s(M(4)));
    vline(c, x + aw, by, by + bh, P.line);
    vline(c, x + w - aw, by, by + bh, P.line);
    for (int s = 0; s < 2; ++s) {
      const float ax = s == 0 ? x : x + w - aw;
      ImGui::PushID(s + 4390);
      ImGui::SetCursorScreenPos(c.p(ax, by));
      if (ImGui::InvisibleButton("##step", c.F.sz(aw, bh))) libraryStep(c, s == 0 ? -1 : 1);
      const bool hv = ImGui::IsItemHovered();
      if (hv) c.a.setHint(s == 0 ? "The sound before this one in the library" : "The next sound in the library");
      const ImVec2 m = c.p(ax + aw * 0.5f, cy);
      const float d = c.s(M(3)), sg = s == 0 ? 1.f : -1.f;
      const ImU32 col = hv ? P.text : P.textFaint;
      c.dl->AddLine(ImVec2(m.x + d * 0.5f * sg, m.y - d), ImVec2(m.x - d * 0.5f * sg, m.y), col, std::max(1.f, c.s(M(1.3f))));
      c.dl->AddLine(ImVec2(m.x - d * 0.5f * sg, m.y), ImVec2(m.x + d * 0.5f * sg, m.y + d), col, std::max(1.f, c.s(M(1.3f))));
      ImGui::PopID();
    }
    const float nx = x + aw + M(8), nw = w - aw * 2 - M(16);
    ImGui::SetCursorScreenPos(c.p(x + aw, by));
    if (ImGui::InvisibleButton("##soundname", c.F.sz(w - aw * 2, bh))) ImGui::OpenPopup("##sound");
    if (ImGui::IsItemHovered()) c.a.setHint(name + ": click for Init, the factory sounds, Open file and the Library");
    if (ImGui::BeginPopup("##sound")) {
      soundMenu(c);
      ImGui::EndPopup();
    }
    const std::string kind = soundKind(name);
    float kw = 0;
    if (!kind.empty()) kw = capsW(c, kind.c_str(), T(10.5f), M(0.6f)) + M(8);
    std::string shown = name;
    while (shown.size() > 2 && textW(c, shown.c_str(), T(12), false) > nw - kw) shown.pop_back();
    text(c, nx, cy - T(12) * 0.68f, shown.c_str(), P.text, T(12), false);
    if (!kind.empty()) {
      MonoText mono;
      capsText(c, nx + textW(c, shown.c_str(), T(12), false) + M(8), cy - T(10.5f) * 0.62f, kind.c_str(), P.textFaint, T(10.5f), M(0.6f));
    }
  }
  float x = M(434);
  {
    const float w = textW(c, "Library", T(12), false) + M(24);
    if (btnLook(c, "lib", x, by, w, bh, "Library", false, c.u.view == VLibrary, T(12))) openView(c.u, c.u.view == VLibrary ? c.u.back : VLibrary);
    if (ImGui::IsItemHovered()) c.a.setHint("Library: Plasma sounds, your preset folders, every table and shape (right-click: Init, factory sounds, Open file)");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##sound2");
    if (ImGui::BeginPopup("##sound2")) {
      soundMenu(c);
      ImGui::EndPopup();
    }
    x += w + M(14);
  }
  {
    const float w = textW(c, "Save as", T(12), false) + M(24);
    saveAsButton(c, "save", x, by, w, bh, true);
    x += w + M(14);
  }
  if (!c.m.openFormat.empty()) {  // opened from another format: its report on hover
    const bool approx = !c.m.openNotes.empty() || !c.m.openMissing.empty();
    const float iw = bh;
    ImGui::SetCursorScreenPos(c.p(x, by));
    ImGui::InvisibleButton("##openinfo", c.F.sz(iw, bh));
    const bool ih = ImGui::IsItemHovered();
    const ImU32 col = ih ? P.text : (approx ? P.hot : P.textDim);
    const ImVec2 m = c.p(x + iw * 0.5f, cy);
    c.dl->AddCircle(m, c.s(M(6)), col, 16, std::max(1.0f, c.s(1.1f)));
    c.dl->AddLine(ImVec2(m.x, m.y - c.s(1)), ImVec2(m.x, m.y + c.s(3.5f)), col, std::max(1.0f, c.s(1.3f)));
    c.dl->AddCircleFilled(ImVec2(m.x, m.y - c.s(3.2f)), c.s(0.9f), col);
    if (ih) {
      ImGui::BeginTooltip();
      ImGui::PushTextWrapPos(c.s(420));
      ImGui::TextUnformatted(openReportText(c.m).c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    x += iw + M(14);
  }
  const float leftEnd = x;
  // the right side, from the right edge: theme, output, Simple / Full, Panels / Patch
  float rx = kW - M(20);
  {
    const float w = M(30);
    rx -= w;
    ImGui::SetCursorScreenPos(c.p(rx, by));
    const bool click = ImGui::InvisibleButton("##theme", c.F.sz(w, bh));
    const bool hv = ImGui::IsItemHovered();
    c.dl->AddRectFilled(c.p(rx, by), c.p(rx + w, by + bh), hv ? P.headOn : P.panelHi, c.s(M(4)));
    c.dl->AddRect(c.p(rx, by), c.p(rx + w, by + bh), hv ? P.lineHi : P.line, c.s(M(4)));
    themeGlyph(c, c.p(rx + w * 0.5f, cy), c.s(M(5.5f)), hv ? P.text : P.label);
    if (hv) c.a.setHint(i18n::tr(theme::isDark() ? "Dark or light: switch Gridline to the light theme" : "Dark or light: switch Gridline to the dark theme"));
    // the switch waits for the start of Plasma's next frame (requestThemeSwitch): here, inside the
    // editor's skin scope, the scope's end would put the old palette and colours back
    if (click || devThemePress()) requestThemeSwitch(theme::isDark());  // dark now: go light
    rx -= M(14);
  }
  {  // the output: a small dial, its level, a meter
    rx -= M(6);
    meterV(c, rx, cy - M(14), M(6), M(28));
    rx -= M(8);
    MonoText mono;
    const std::string vs = ptext(c, G_MasterVol);
    const float vw = std::max(textW(c, vs.c_str(), T(10.5f), false), textW(c, "-60.0 dB", T(10.5f), false));
    rx -= vw;
    text(c, rx, cy - T(10.5f) * 0.68f, vs.c_str(), P.textFaint, T(10.5f), false);
    rx -= M(8) + M(28);
    KnobColour kc(P.accent);
    knob(c, G_MasterVol, rx + M(14), cy, M(11), "Output", KnobNoLabel);
    if (ImGui::IsItemHovered()) c.a.setHint("Output: the level of the whole sound");
    rx -= M(14);
  }
  {
    const char* a = "Simple";
    const char* b = "Full";
    const float w = textW(c, a, T(11.5f), false) + textW(c, b, T(11.5f), false) + M(20 * 2 + 2 + 4 + 2);
    rx -= w;
    const int hit = segPick(c, "sf", rx, cy - M(14), w, M(28), {a, b}, c.u.full ? 1 : 0, false, T(11.5f));
    if (ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(c.p(rx, cy - M(14)), c.p(rx + w, cy + M(14))))
      c.a.setHint("Simple: the main controls, details one click away. Full: every control in place");
    if (hit >= 0 && (hit == 1) != c.u.full) c.u.full = hit == 1, c.a.settings.set("ui.plasmaFull", c.u.full);
    rx -= M(14);
  }
  {
    const char* a = "Panels";
    const char* b = "Patch";
    const float w = textW(c, a, T(11.5f), false) + textW(c, b, T(11.5f), false) + M(20 * 2 + 2 + 4 + 2);
    rx -= w;
    const int hit = segPick(c, "pp", rx, cy - M(14), w, M(28), {a, b}, c.u.patch ? 1 : 0, false, T(11.5f));
    if (ImGui::IsMouseHoveringRect(c.p(rx, cy - M(14)), c.p(rx + w, cy + M(14))))
      c.a.setHint("Panels: the sound as blocks. Patch: the same sound as boxes and wires (Tab)");
    if (hit >= 0 && (hit == 1) != c.u.patch) {
      c.u.patch = hit == 1;
      if (c.u.patch && tabOf(c.u.view) != TabSynth && tabOf(c.u.view) != TabEffects) c.u.view = VLayer;
    }
    rx -= M(14);
  }
  // the tabs, centred in the room between: every parameter has its home in one of them
  {
    static const char* const names[TabCount] = {"Synth", "Effects", "Links", "Play", "Setup"};
    static const char* const tips[TabCount] = {"Synth: the sound as blocks (layers, filters, amp) and the modulation under them",
                                               "Effects: the main chain and the two sends", "Links: every link from a source to a control",
                                               "Play: voices, glide, bend, key and touch, arpeggiator, phrases, key zones",
                                               "Setup: sound quality, tuning, time scaling, editor, sound info, compatibility"};
    static const int views[TabCount] = {VLayer, VEffects, VLinks, VVoices, VSetup};
    const bool lib = c.u.view == VLibrary;  // the Library is a full panel: no tab is lit under it
    const int cur = lib ? -1 : tabOf(c.u.view);
    const int nl = linksUsed(c);
    char badge[16];
    std::snprintf(badge, sizeof badge, "%d", nl);
    float ws[TabCount], tot = 0;
    for (int i = 0; i < TabCount; ++i) {
      ws[i] = textW(c, names[i], T(13), false) + M(32);
      if (i == TabLinks && nl > 0) {
        MonoText mono;
        ws[i] += textW(c, badge, T(10), true) + M(10 + 6);
      }
      tot += ws[i] + (i ? M(2) : 0);
    }
    float tx = std::max(leftEnd, leftEnd + (rx - leftEnd - tot) * 0.5f);
    for (int i = 0; i < TabCount; ++i) {
      ImGui::PushID(i + 4300);
      const float tw = ws[i];
      ImGui::SetCursorScreenPos(c.p(tx, 0));
      if (ImGui::InvisibleButton("##tab", c.F.sz(tw, kTopH - 1))) {
        if (!(i == TabPlay && cur == TabPlay)) {
          if (isOverlay(c.u.view)) closeOverlay(c.u);
          c.u.view = views[i];
          if (i != TabSynth && i != TabEffects) c.u.patch = false;
        }
      }
      const bool hv = ImGui::IsItemHovered(), on = cur == i;
      if (hv) c.a.setHint(tips[i]);
      const ImU32 tc = on ? P.text : (hv ? P.text : (lib ? P.textFaint : P.label));
      const float lw = textW(c, names[i], T(13), false);
      text(c, tx + M(16), cy - T(13) * 0.68f, names[i], tc, T(13), false);
      if (on) c.dl->AddRectFilled(c.p(tx + M(14), kTopH - M(2) - 0.5f), c.p(tx + tw - M(14), kTopH - 0.5f), P.accent, c.s(M(2)));
      if (i == TabLinks && nl > 0) {
        MonoText mono;
        const float bx = tx + M(16) + lw + M(6), bw = textW(c, badge, T(10), true) + M(10), bh2 = M(15);
        c.dl->AddRectFilled(c.p(bx, cy - bh2 * 0.5f), c.p(bx + bw, cy + bh2 * 0.5f), P.panelHi, c.s(M(8)));
        c.dl->AddRect(c.p(bx, cy - bh2 * 0.5f), c.p(bx + bw, cy + bh2 * 0.5f), P.line, c.s(M(8)));
        textMid(c, bx, bx + bw, cy, badge, P.label, T(10), true);
      }
      ImGui::PopID();
      tx += tw + M(2);
    }
  }
}

// ================================================================== motion sources
std::string dockName(int src) {
  if (src >= SrcEnv1 && src <= SrcEnv4) return "Envelope " + std::to_string(src - SrcEnv1 + 1);
  if (src >= SrcLfo1 && src <= SrcLfo10) return "LFO " + std::to_string(src - SrcLfo1 + 1);
  if (src >= SrcLfo1Y && src <= SrcLfo10Y) return "LFO " + std::to_string(src - SrcLfo1Y + 1) + " Y";
  return sourceLabel(src);
}
// A source row: grip, name, link count; click opens `view` (if >= 0). Drags from the grip.
void sourceRow(Ctx& c, int src, float x, float y, float w, bool sel, int view, const char* name, float h) {
  ImGui::PushID(src);
  ImGui::SetCursorScreenPos(c.p(x + 20, y));
  const bool click = ImGui::InvisibleButton("##row", c.F.sz(w - 20, h));
  const bool hv = ImGui::IsItemHovered();
  if (hv) c.u.hoverSrcNext = src;
  const bool armed = c.u.linkSrc == src;
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), sel ? c.P.headOn : (hv || armed ? c.P.panelHi : c.P.field), c.s(4));
  if (armed || sel) c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), armed ? famCol(c, src) : c.P.lineHi, c.s(4), 0, std::max(1.0f, c.s(1.3f)));
  const int n = linksOf(c, src);
  char cnt[16] = "";
  if (n > 0) std::snprintf(cnt, sizeof cnt, "%d", n);
  const float cw = cnt[0] ? textW(c, cnt, 11.0f, false) + 8 : 0;
  c.dl->PushClipRect(c.p(x + 20, y), c.p(x + w - 4 - cw, y + h), true);
  labelBeside(c, x + 22, y, h, name, sel ? c.P.text : c.P.label, 11.5f, false);
  c.dl->PopClipRect();
  if (cnt[0]) {
    MonoText mono;
    labelBeside(c, x + w - 4 - cw + 3, y, h, cnt, famCol(c, src), 11.0f, false);
  }
  if (click && view >= 0) {
    if (view == VEnv) c.u.envTab = src - SrcEnv1;
    if (view == VLfo) c.u.lfoTab = (src - SrcLfo1) % kLfos;
    c.u.view = view;
  }
  if (hv) c.a.setHint(dockName(src) + (view >= 0 ? ": click to open \xC2\xB7 drag the dot onto a control to link it" : ": drag the dot onto a control to link it"));
  sourceGrip(c, src, x + 11, y + h * 0.5f, 6.0f);
  ImGui::PopID();
}

// ================================================================== play bar and drawer
namespace {
// A switch chip: its dot turns `onK` on and off, its name opens `view`.
void playChip(Ctx& c, const char* id, float x, float y, float w, float h, int onK, const char* name, int view, const char* hint) {
  ImGui::PushID(id);
  reach(onK);
  const bool on = val(c, onK) >= 0.5, sel = c.u.view == view;
  layoutBox(c, x, y, w, h, name);
  chipLook(c, x, y, w, h, name, on);
  if (sel) c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.accent, c.s(M(4)));
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton("##on", c.F.sz(M(22), h))) setv(c, onK, on ? 0 : 1);
  if (ImGui::IsItemHovered()) c.a.setHint(paramLongName(onK) + (on ? ": on (click: off)" : ": off (click: on)"));
  ImGui::SetCursorScreenPos(c.p(x + M(22), y));
  if (ImGui::InvisibleButton("##open", c.F.sz(w - M(22), h))) openView(c.u, sel ? VLayer : view);
  if (ImGui::IsItemHovered()) {
    c.a.setHint(hint);
    c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), c.P.lineHi, c.s(M(4)));
  }
  ImGui::PopID();
}
}  // namespace

void playBar(Ctx& c) {
  const Pal& P = c.P;
  const float y = kPlayY, h = kH - y, cy = y + h * 0.5f, fh = M(28), fy = cy - fh * 0.5f;
  panelRect(c, 0, y, kW, h, barBg(c));
  hline(c, 0, kW, y + 0.5f, P.line);
  float x = M(20);
  {
    MonoText mono;
    capsText(c, x, cy - T(10) * 0.62f, "Play", P.textFaint, T(10), M(0.8f));
    x += capsW(c, "Play", T(10), M(0.8f)) + M(10);
  }
  {  // voices: Poly / Mono / Legato (G_Mono, G_Legato)
    reach(G_Mono);
    reach(G_Legato);
    const int mode = val(c, G_Mono) < 0.5 ? 0 : (val(c, G_Legato) >= 0.5 ? 2 : 1);
    const char* lab[3] = {"Poly", "Mono", "Legato"};
    float w = M(2 + 2 + 2 * 2 + 2);
    for (const char* l : lab) w += textW(c, l, T(11.5f), false) + M(20);
    const int hit = segPick(c, "vm", x, fy, w, fh, {lab[0], lab[1], lab[2]}, mode, false, T(11.5f));
    if (ImGui::IsMouseHoveringRect(c.p(x, fy), c.p(x + w, fy + fh)))
      c.a.setHint("Poly: every note its own voice. Mono: one voice, each note restarts it. Legato: one voice that glides between held notes");
    if (hit >= 0 && hit != mode) {
      setv(c, G_Mono, hit == 0 ? 0 : 1);
      setv(c, G_Legato, hit == 2 ? 1 : 0);
    }
    x += w + M(10);
  }
  auto fieldFor = [&](int k, const char* label, const char* widest) {
    float vw;
    {
      MonoText mono;
      vw = std::max(textW(c, ptext(c, k).c_str(), T(11.5f), false), textW(c, widest, T(11.5f), false));
    }
    const float w = textW(c, label, T(11.5f), false) + vw + M(10 + 10 + 12);
    bar(c, k, x, fy, w, fh, label, BarField);
    x += w + M(10);
  };
  fieldFor(G_Poly, "Voices", "16");
  fieldFor(G_PortaTime, "Glide", "400 ms");
  fieldFor(G_Transpose, "Transpose", "-24 st");
  vline(c, x, cy - M(11), cy + M(11), P.line);
  x += M(10);
  {
    const float w = textW(c, "Arpeggiator", T(11.5f), false) + M(11 + 7 + 7 + 11);
    playChip(c, "arp", x, fy, w, fh, G_ArpOn, "Arpeggiator", VArp, "Arpeggiator: the dot switches it on, the name opens it");
    x += w + M(10);
  }
  {
    const float w = textW(c, "Phrases", T(11.5f), false) + M(11 + 7 + 7 + 11);
    playChip(c, "phr", x, fy, w, fh, G_ClipOn, "Phrases", VPhrases, "Phrases: the dot switches the phrase player on, the name opens it");
    x += w + M(10);
  }
  // the rest of the playing settings
  {
    const float w = textW(c, "More", T(11.5f), false) + M(24);
    moreLink(c, "playmore", x, fy, w, fh, "More",
             {G_PortaCurve, G_PortaAlways, G_PortaScaled, G_BendUp, G_BendDown, G_Key, G_Scale, G_Swing, G_SwingDiv},
             [&] {
               ImGui::Separator();
               if (ImGui::MenuItem(i18n::tr("Key zones..."))) c.u.view = VZones;
             },
             "Glide curve and options, bend range, key and scale, groove, key zones", T(11.5f));  // the bar's text size
  }
  {  // the keyboard drawer
    const float w = textW(c, "Keyboard", T(11.5f), false) + M(11 + 16 + 7 + 7 + 10 + 11);
    const float kx = kW - M(20) - w;
    ImGui::SetCursorScreenPos(c.p(kx, fy));
    const bool click = ImGui::InvisibleButton("##kbd", c.F.sz(w, fh));
    const bool hv = ImGui::IsItemHovered();
    layoutBox(c, kx, fy, w, fh, "Keyboard");
    c.dl->AddRectFilled(c.p(kx, fy), c.p(kx + w, fy + fh), c.u.drawer ? P.accentSoft : (hv ? P.headOn : P.panelHi), c.s(M(4)));
    c.dl->AddRect(c.p(kx, fy), c.p(kx + w, fy + fh), c.u.drawer ? P.accent : (hv ? P.lineHi : P.line), c.s(M(4)));
    const ImU32 ic = c.u.drawer ? P.accentText : P.label;
    // the glyph, the name and the arrow on one line: the middle of the name's capitals
    const float my = capMid(c, fy, fh, T(11.5f), false);
    pianoGlyph(c, ImVec2(c.p(kx + M(11 + 8), 0).x, my), ic);
    labelBeside(c, kx + M(11 + 16 + 7), fy, fh, "Keyboard", ic, T(11.5f), false);  // on the row's baseline
    iconArrow(c.dl, ImVec2(std::round(c.p(kx + w - M(11) - M(4), 0).x), my), std::round(c.s(M(9))), c.u.drawer ? 3 : 2, P.textDim);  // which way it slides
    if (click) c.u.drawer = !c.u.drawer;
    if (hv) c.a.setHint("Keyboard drawer (Alt+K): the wheels, the bend range and a keyboard to play the sound");
    // dev (tests): where the Keyboard button is on screen, whether the drawer is open and how much of it was drawn
    if (static const bool rep = std::getenv("GL_PLASMA_DRAWER_REPORT") != nullptr; rep) {
      const ImVec2 a = edzoomui::toScreenHere(c.p(kx, fy)), b = edzoomui::toScreenHere(c.p(kx + w, fy + fh));
      static std::string last;
      char line[160];
      std::snprintf(line, sizeof line, "PLASMA_DRAWER open %d drawn %.0f button %.0f %.0f %.0f %.0f", c.u.drawer ? 1 : 0,
                    static_cast<double>(c.u.devDrawerDrawn), static_cast<double>((a.x + b.x) * 0.5f), static_cast<double>((a.y + b.y) * 0.5f),
                    static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y));
      if (last != line) last = line, std::fprintf(stderr, "%s\n", line);
      // the drawer's parts on screen, a dial under it, and the values (the rest of the sound as one checksum)
      const DrawerDev& dd = drawerDev();
      double sum = 0;
      for (size_t i = 0; i < c.st.params.size(); ++i) {
        const int k = static_cast<int>(i);
        if (k == G_PitchWheel || k == G_ModWheel || k == G_BendUp || k == G_BendDown || k == G_TrigOctave) continue;
        sum += static_cast<double>(c.st.params[i]) * static_cast<double>(i % 97 + 1);
      }
      static std::string lastDev;
      char dev[640];
      auto mid = [](const ImVec4& r) { return ImVec2((r.x + r.z) * 0.5f, (r.y + r.w) * 0.5f); };
      const ImVec2 pw = mid(dd.pitch), mw = mid(dd.mod);
      std::snprintf(dev, sizeof dev,
                    "PLASMA_DRAWER_DEV under %d %.0f %.0f gap %.0f %.0f pitch %.0f %.0f %.0f %.0f mod %.0f %.0f %.0f %.0f "
                    "up %.0f %.0f %.0f %.0f down %.0f %.0f %.0f %.0f oct %.0f %.0f %.0f %.0f keys %.0f %.0f %.0f %.0f "
                    "vals sum %.6f pw %.4f mw %.4f bu %d bd %d to %d key %d vel %.3f hot %d pop %d at %.0f %.0f rect %.0f %.0f %.0f %.0f",
                    dd.underK, static_cast<double>(dd.under.x), static_cast<double>(dd.under.y), static_cast<double>(dd.gap.x), static_cast<double>(dd.gap.y),
                    static_cast<double>(pw.x), static_cast<double>(pw.y), static_cast<double>(dd.pitch.z - dd.pitch.x), static_cast<double>(dd.pitch.w - dd.pitch.y),
                    static_cast<double>(mw.x), static_cast<double>(mw.y), static_cast<double>(dd.mod.z - dd.mod.x), static_cast<double>(dd.mod.w - dd.mod.y),
                    static_cast<double>(dd.up.x), static_cast<double>(dd.up.y), static_cast<double>(dd.up.z), static_cast<double>(dd.up.w),
                    static_cast<double>(dd.down.x), static_cast<double>(dd.down.y), static_cast<double>(dd.down.z), static_cast<double>(dd.down.w),
                    static_cast<double>(dd.oct.x), static_cast<double>(dd.oct.y), static_cast<double>(dd.oct.z), static_cast<double>(dd.oct.w),
                    static_cast<double>(dd.keys.x), static_cast<double>(dd.keys.y), static_cast<double>(dd.keys.z), static_cast<double>(dd.keys.w), sum,
                    val(c, G_PitchWheel), val(c, G_ModWheel), vali(c, G_BendUp), vali(c, G_BendDown), vali(c, G_TrigOctave), dd.lastKey,
                    static_cast<double>(dd.lastVel), dd.hot ? 1 : 0, dd.pop ? 1 : 0, static_cast<double>(dd.at.x), static_cast<double>(dd.at.y),
                    static_cast<double>(dd.screen.x), static_cast<double>(dd.screen.y), static_cast<double>(dd.screen.z), static_cast<double>(dd.screen.w));
      if (c.u.drawer && lastDev != dev) lastDev = dev, std::fprintf(stderr, "%s\n", dev);
    }
  }
}

// The whole editor.
void drawFrame(Ctx& c) {
  Ui& u = c.u;
  clearPictures();  // the pictures a double-click opens from, registered as they are drawn
  const Pal& P = c.P;
  // keys (the editor's window has focus and no text field is active)
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {  // used here: the app does not act on the same Esc
      if (u.linkSrc >= 0) u.linkSrc = -1, keys::claimEscape();
      else if (isOverlay(u.view) || u.large || (tabOf(u.view) == TabSynth && u.view != VLayer)) beginClose(u), keys::claimEscape();
    }
    const bool prev = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false), next = ImGui::IsKeyPressed(ImGuiKey_RightBracket, false);
    if (prev || next) {
      const int d = next ? 1 : -1;
      if (u.view == VLayer) u.layer = (u.layer + d + kOscs) % kOscs;
      if (u.view == VEnv) u.envTab = (u.envTab + d + kEnvs) % kEnvs;
      if (u.view == VLfo) u.lfoTab = (u.lfoTab + d + kLfos) % kLfos;
    }
  }
  // the keyboard drawer: Alt+K or the Keyboard button; it slides (a fifth of a second)
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyAlt &&
      ImGui::IsKeyPressed(ImGuiKey_K, false))
    u.drawer = !u.drawer;
  {
    const float want = u.drawer && !isOverlay(u.view) ? 1.f : 0.f;
    const bool anim = c.a.settings.getBool("ui.animations") && !std::getenv("GL_PLASMA_VIEW") && !std::getenv("GL_PLASMA_VIEWS");
    u.drawerT = anim ? (std::fabs(want - u.drawerT) < 0.01f ? want : u.drawerT + (want - u.drawerT) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f)) : want;
  }
  panelRect(c, 0, 0, kW, kH, P.frame);
  const bool overlay = isOverlay(u.view);
  const bool drawerOn = drawerH(u) >= 1 && !overlay;
  u.devDrawerDrawn = 0;  // (drawer() sets the height it drew)
  drawerDev().rect = ImVec4(0, 0, 0, 0), drawerDev().underK = -1;
  // While the pointer is over the drawer, the editor under it must not see it: no hover, click,
  // drag or wheel reaches the controls it covers. The drawer is drawn first; then the pointer is
  // moved off for the rest of the frame (and put back when drawFrame returns) unless a control
  // outside the drawer is held (a drag that started above it carries on).
  struct PointerOff {
    bool on = false;
    ImVec2 pos{0, 0};
    float wheel = 0, wheelH = 0;
    ~PointerOff() {
      if (!on) return;
      ImGuiIO& io = ImGui::GetIO();
      io.MousePos = pos, io.MouseWheel = wheel, io.MouseWheelH = wheelH;
    }
  } pointerOff;
  if (drawerOn) {
    c.dl->ChannelsSplit(2);
    c.dl->ChannelsSetCurrent(1);
    layoutLayer(3);
    const bool held = drawer(c);
    c.dl->ChannelsSetCurrent(0);
    layoutLayer(0);
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 a = c.p(0, kPlayY - drawerH(u)), b = c.p(drawerW(u), kPlayY);
    const bool over = ImGui::IsMousePosValid() && io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y && io.MousePos.y < b.y;
    const bool otherHeld = ImGui::GetActiveID() != 0 && !held;
    if (over && !otherHeld && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      pointerOff.on = true, pointerOff.pos = io.MousePos, pointerOff.wheel = io.MouseWheel, pointerOff.wheelH = io.MouseWheelH;
      io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX), io.MouseWheel = 0, io.MouseWheelH = 0;
    }
  }
  routesBegin(c);
  topBar(c);
  // Tab flips Panels / Patch while the editor has the keyboard
  // (not while a popup is open or a control is held, and not when the shortcut set gives plain Tab to the app)
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeySuper &&
      !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
    const char* app = keys::label("view.toggleMain");
    if (!app || std::strcmp(app, "Tab") != 0) u.patch = !u.patch;
  }
  // the panel pages open only as a block's advanced panel
  if (!u.large && (u.view == VRootTex || u.view == VFilters || u.view == VAmp)) u.view = VLayer;
  // the advanced panel grows from its block (a fifth of a second), and shrinks back on close
  {
    const bool anim = c.a.settings.getBool("ui.animations") && !std::getenv("GL_PLASMA_VIEW") && !std::getenv("GL_PLASMA_VIEWS");
    const float step = anim ? ImGui::GetIO().DeltaTime / 0.18f : 1.f;
    if (u.closing) {
      u.expandT -= step;
      if (u.expandT <= 0) finishClose(u);
    } else {
      u.expandT = std::min(1.f, u.expandT + step);
    }
  }
  const bool growing = u.expandT < 1 && u.expandFrom.z > u.expandFrom.x && u.view != VLayer && !(u.view == VEffects && !u.large);
  ImRect growClip;
  if (growing) {
    const float t = 1.f - (1.f - u.expandT) * (1.f - u.expandT);  // ease out
    const ImVec2 a0(u.expandFrom.x, u.expandFrom.y), b0(u.expandFrom.z, u.expandFrom.w), a1 = c.p(0, kTopH), b1 = c.p(kW, kPlayY);
    growClip = ImRect(ImVec2(a0.x + (a1.x - a0.x) * t, a0.y + (a1.y - a0.y) * t), ImVec2(b0.x + (b1.x - b0.x) * t, b0.y + (b1.y - b0.y) * t));
  }
  if (u.patch && !overlay && tabOf(u.view) != TabSynth && tabOf(u.view) != TabEffects) u.patch = false;
  if (u.patch && !overlay) {
    kStageW = 1000;
    patchView(c);
    playBar(c);
    drawRoutes(c);
    if (drawerOn) c.dl->ChannelsMerge();
    return;
  }
  kStageW = kW;
  if (growing) {  // the tab under the panel, then the panel inside its growing rectangle
    if (tabOf(u.view) == TabEffects) effectsTab(c);
    else synthTab(c);
    ImGui::PushClipRect(growClip.Min, growClip.Max, true);
  }
  if (!overlay && (u.view == VLayer || (u.view == VEffects && !u.large))) {
    if (u.view == VLayer) synthTab(c);
    else effectsTab(c);
    sidebar(c);
    playBar(c);
    drawRoutes(c);
    if (drawerOn) c.dl->ChannelsMerge();
    return;
  }
  panelRect(c, 0, kStageHeadY, kStageW, kStageB - kStageHeadY, P.frame);
  if (overlay) {
    layoutLayer(2);
    switch (u.view) {
      case VPhrases: clipEditor(c); break;
      case VTable: wtEditor(c); break;
      default: libraryPage(c); break;
    }
    layoutLayer(0);
  } else {
    // with the drawer out the view is shorter: it scrolls (wheel) so nothing hides behind it
    const float dh = drawerH(u);
    u.stageScroll = std::clamp(u.stageScroll, 0.f, dh);
    if (dh > 0 && ImGui::IsMouseHoveringRect(c.p(0, kStageHeadY), c.p(kStageW, kPlayY - dh)) && ImGui::GetIO().MouseWheel != 0 && !ImGui::IsAnyItemActive())
      u.stageScroll = std::clamp(u.stageScroll - ImGui::GetIO().MouseWheel * 48.f, 0.f, dh);
    skin::Form G = c.F;
    G.o.y -= u.stageScroll * G.s;
    Ctx sc{c.a, c.t, G, c.dl, c.st, c.m, c.u, c.P, c.live, c.hasLive, c.graph};
    ImGui::PushClipRect(c.p(0, kStageHeadY), c.p(kStageW, kPlayY - dh), true);
    if (dh > 0) layoutScroll(c);
    stageView(sc);
    ImGui::PopClipRect();
    if (dh > 0) {  // its scroll bar
      const float vh = kPlayY - dh - kStageHeadY, total = vh + dh, th = vh * vh / total, ty = kStageHeadY + (vh - th) * u.stageScroll / dh;
      c.dl->AddRectFilled(c.p(kStageW - 6, ty), c.p(kStageW - 3, ty + th), c.P.lineHi, c.s(2));
    }
  }
  if (growing) {
    ImGui::PopClipRect();
    c.dl->AddRect(growClip.Min, growClip.Max, c.P.lineHi, c.s(8), 0, std::max(1.f, c.s(1.5f)));
  }
  playBar(c);
  if (!overlay) drawRoutes(c);
  if (drawerOn) c.dl->ChannelsMerge();
}

// The full-width view of u.view (drawn in a context that may be scrolled).
void stageView(Ctx& c) {
  {
    switch (c.u.view) {
      case VLayer: layerView(c); break;
      case VRootTex: rootTextureView(c); break;
      case VFilters: filtersView(c); break;
      case VAmp: ampView(c); break;
      case VRouting: routingView(c); break;
      case VEffects: effectsView(c); break;
      case VEnv: envView(c); break;
      case VLfo: lfoView(c); break;
      case VKeys: keysView(c); break;
      case VLinks: linksView(c); break;
      case VArp: arpView(c); break;
      case VZones: mappingPanel(c); break;
      case VVoices: playPage(c); break;
      default: setupView(c); break;
    }
  }
}


// ================================================================== Panels
void scrollBody(Ctx& c, float x, float y0, float w, float y1, float& sc, float& content,
                const std::function<float(Ctx&, float, float, float)>& fn) {
  const float bh = y1 - y0;
  sc = std::clamp(sc, 0.f, std::max(0.f, content - bh));
  const ImVec2 a = c.p(x + 3, y0), b = c.p(x + w - 3, y1);
  ImGui::PushClipRect(a, b, true);
  layoutScroll(c);
  skin::Form G = c.F;
  G.o.y -= sc * G.s;
  Ctx cc{c.a, c.t, G, c.dl, c.st, c.m, c.u, c.P, c.live, c.hasLive, c.graph};
  // the hidden part of the body must not grow the window's content (the host would shrink the editor)
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  const ImVec2 keepMax = win->DC.CursorMaxPos;
  const float end = fn(cc, x + 12, y0 + 4, w - 24 - (content > bh ? 6 : 0));
  win->DC.CursorMaxPos = keepMax;
  ImGui::PopClipRect();
  content = end - y0 + 8;
  if (content > bh) {
    if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemActive()) {
      const float wh = ImGui::GetIO().MouseWheel;
      if (wh != 0) sc = std::clamp(sc - wh * 48.f, 0.f, content - bh);
    }
    const float th = bh * bh / content, ty = y0 + (bh - th) * (sc / std::max(1.f, content - bh));
    c.dl->AddRectFilled(c.p(x + w - 8, y0), c.p(x + w - 5, y1), theme::withAlpha(c.P.line, 0.5f), c.s(2));
    c.dl->AddRectFilled(c.p(x + w - 8, ty), c.p(x + w - 5, ty + th), c.P.lineHi, c.s(2));
  }
}
}  // namespace gl::packs::plasma::ui
