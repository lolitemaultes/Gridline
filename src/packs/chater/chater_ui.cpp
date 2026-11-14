// Gridline - "chater" pack: the Chater editor. One page: the glitch tape (the real captured
// audio with every break marked where it fired, and a read-only lane of where breaks fire),
// eight bold macro tiles, the break odds and quick switches. A tile's chevron folds out its
// page in a drawer under compact tiles. Presets, settings, the dice and the knob menu sit on
// top. Drawn at 1100 x 680 form units, scaled to the window.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include "../../app/App.h"
#include "../../app/AutomationEditor.h"
#include "../../app/EditorZoomUi.h"
#include "../../app/MidiUi.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/Json.h"
#include "../../core/Params.h"
#include "../../core/Presets.h"
#include "../../engine/Program.h"
#include "../PacksUi.h"
#include "chater_widgets.h"

namespace gl::packs::chater {

namespace ui {

using i18n::tr;
constexpr float kPi = 3.14159265f;

// ------------------------------------------------------------------ look

const Look& look() {
  static Look L;
  const theme::Palette& P = theme::pal();
  L.dark = P.dark;
  const theme::Palette& D = theme::darkFaces();
  if (P.dark) {
    L.win = theme::rgb(0x22272A), L.top = theme::rgb(0x1B1F22), L.tape = theme::rgb(0x15191B), L.panel = theme::rgb(0x2C3337);
    L.panel2 = theme::rgb(0x323A3E), L.line = theme::rgb(0x191D20), L.cell = theme::rgb(0x1F2427), L.cellLine = theme::rgb(0x262C30);
    L.card = theme::rgb(0x22282B), L.menu = theme::rgb(0x2A3135), L.side = theme::rgb(0x262C30);
  } else {
    L.win = P.surface900, L.top = P.chrome, L.tape = P.well, L.panel = P.surface800, L.panel2 = P.surface700, L.line = P.line;
    L.cell = P.field, L.cellLine = P.line, L.card = P.surface700, L.menu = theme::popupBg(P), L.side = P.surface800;
  }
  L.knob = theme::rgb(0x171B1D);
  L.ink = theme::rgb(0x121619);
  L.text = P.text, L.text2 = P.textDim, L.text3 = P.textFaint;
  L.wave = theme::withAlpha(P.textDim, P.dark ? 0.8f : 0.7f);
  L.accent = theme::rgb(0xFF5FA2), L.accentInk = P.dark ? theme::rgb(0xFF8DBF) : theme::rgb(0xD63A80), L.accentBg = theme::rgb(0xFF5FA2, 0.13f);
  L.led = theme::lit(theme::rgb(0xA8E44A));
  L.white = D.paper;
  const uint32_t tiles[8] = {0x5FD8FF, 0xFF5FA2, 0xA98BFF, 0xFFB03A, 0xF4EC6A, 0xFF7A4D, 0x6C95FF, 0xC9D3D8};
  for (int i = 0; i < 8; ++i) L.tile[i] = theme::rgb(tiles[i]);
  const uint32_t ev[6] = {0xFF6FB1, 0xB79BFF, 0xFFB45E, 0x7FD8C0, 0x6FA8FF, 0x8E9AA1};
  for (int i = 0; i < 6; ++i) L.ev[i] = theme::lit(theme::rgb(ev[i]));
  const uint32_t g[5] = {0x8DA6B4, 0x7FE3FF, 0x9FE9FF, 0xE9B8FF, 0xFFF0A0};
  for (int i = 0; i < 5; ++i) L.g[i] = theme::lit(theme::rgb(g[i]));
  const uint32_t b[10] = {0x8597A0, 0xFF6FB1, 0xFF9E3D, 0x9EE6FF, 0xF7F08A, 0xFF8A5C, 0xB79BFF, 0x6FA8FF, 0x7FD8C0, 0xB9C5CB};
  for (int i = 0; i < 10; ++i) L.b[i] = theme::lit(theme::rgb(b[i]));
  const uint32_t l[5] = {0xD673EA, 0x62AEF5, 0xA8E44A, 0xFF9E3D, 0x7FD8C0};
  for (int i = 0; i < 5; ++i) L.l[i] = theme::lit(theme::rgb(l[i]));
  return L;
}

const char* breakName(int type) {
  static const char* n[BrCount] = {"", i18n::N_("Repeat"), i18n::N_("Reverse"), i18n::N_("Tape"), i18n::N_("Scrub"), i18n::N_("Jump"), i18n::N_("Skip")};
  return type > 0 && type < BrCount ? tr(n[type]) : "";
}
ImU32 breakColour(const Look& L, int type) { return type > 0 && type < BrCount ? L.ev[type - 1] : L.wave; }

int pageTile(int page) {
  switch (page) {
    case PgGlitter: return 0;
    case PgHaze: return 1;
    case PgFracture: return 5;
    case PgColour: return 4;
    case PgSpace: return 6;
    default: return -1;
  }
}
int tilePage(int tile) {
  static const int p[8] = {PgGlitter, PgHaze, PgHaze, PgFracture, PgColour, PgFracture, PgSpace, PgSpace};
  return p[std::clamp(tile, 0, 7)];
}
int shownTile(const Ui& u) { return u.openTile >= 0 && u.openTile < 8 && tilePage(u.openTile) == u.page ? u.openTile : pageTile(u.page); }

ImU32 groupColour(const Look& L, int k) {
  if (k <= PMix) return L.tile[std::clamp(k, 0, 7)];
  if (k >= PDamp && k <= PWash) return L.tile[PHaze];
  if ((k >= PWow && k <= PEnsemble) || (k >= PHiss && k <= PWowRate)) return L.tile[PMemory];
  if (k >= PRmShimmer && k <= PRmMod) return L.b[7];
  if (k == PEcDiffuse) return L.b[8];
  if (k >= PLength && k <= PHold) return L.b[0];
  if ((k >= PTrigMode && k <= PSeed)) return L.b[2];
  if (k >= POddRepeat && k <= POddSkip) return L.ev[k - POddRepeat];
  if (k >= PDivision && k <= PGate) return L.ev[0];
  if (k >= PTapeTime && k <= PScrubRate) return L.ev[2];
  if (k >= PGrDensity && k <= PGrWindow) return L.g[2];
  if (k >= PIntervals && k <= PDetune) return L.g[3];
  if (k >= PScale && k <= PSnap) return L.g[1];
  if (k >= PShFeedback && k <= PShSpread) return L.g[4];
  if (k >= PCrChord && k <= PCrMix) return L.b[3];
  if (k >= PHaLift && k <= PHaMix) return L.b[4];
  if (k >= PGtBits && k <= PGtMix) return L.b[5];
  if (k >= PWidth && k <= POrbit) return L.b[6];
  if (k >= PRmSize && k <= PRmMix) return L.b[7];
  if (k >= PEcTime && k <= PEcPingPong) return L.b[8];
  if (k >= PLowCut && k <= PSafety) return L.b[9];
  if (k >= PSwA && k < PSwB) return L.l[0];
  if (k >= PSwB && k < PSwB + SwCount) return L.l[1];
  if (k >= PFoAttack && k <= PFoFloor) return L.l[2];
  if (k >= PLink0 && k < PLink0 + kLinks * LkCount) return L.l[3];
  return L.accent;
}

// ------------------------------------------------------------------ prefs

namespace {
std::string prefsPath(App& a) { return a.configDir() + "/chater.json"; }
}  // namespace

Prefs& prefs(App& a) {
  static Prefs p;
  if (!p.loaded) {
    p.loaded = true;
    std::ifstream f(prefsPath(a));
    if (f) {
      std::stringstream ss;
      ss << f.rdbuf();
      Json j;
      if (jsonParse(ss.str(), j)) {
        p.visuals = static_cast<int>(std::clamp(j.num("visuals", 0), 0.0, 2.0));
        p.knobDrag = static_cast<int>(std::clamp(j.num("knobDrag", 1), 0.0, 1.0));
        if (const Json* fv = j.find("favourites"); fv && fv->type == Json::Type::Array)
          for (const Json& e : fv->array)
            if (e.type == Json::Type::String) p.favourites.insert(e.str);
      }
    }
  }
  return p;
}

void savePrefs(App& a) {
  const Prefs& p = prefs(a);
  Json j = Json::makeObject();
  j.object["visuals"] = Json::makeNumber(p.visuals);
  j.object["knobDrag"] = Json::makeNumber(p.knobDrag);
  Json fv = Json::makeArray();
  for (const auto& n : p.favourites) fv.array.push_back(Json::makeString(n));
  j.object["favourites"] = fv;
  std::ofstream f(prefsPath(a));
  if (f) f << jsonStringify(j);
}

// ------------------------------------------------------------------ text and shapes

ImFont* fBold() { return app().fontBold ? app().fontBold : ImGui::GetFont(); }
ImFont* fReg() { return app().fontRegular ? app().fontRegular : ImGui::GetFont(); }
ImFont* fMono() { return app().fontMono ? app().fontMono : ImGui::GetFont(); }
ImFont* fMonoBold() { return app().fontMonoBold ? app().fontMonoBold : fMono(); }

ImVec2 textSize(Ctx& c, ImFont* f, float size, const char* s) {
  const ImVec2 t = f->CalcTextSizeA(c.s(size * kTextScale), 1e9f, 0, s);
  return ImVec2(t.x / c.F.s, t.y / c.F.s);
}
void text(Ctx& c, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* s) {
  const ImVec2 p = c.p(pos.x, pos.y);
  c.dl->AddText(f, c.s(size * kTextScale), ImVec2(std::round(p.x), std::round(p.y)), col, s);
}
void textR(Ctx& c, ImFont* f, float size, float right, float y, ImU32 col, const char* s) {
  text(c, f, size, ImVec2(right - textSize(c, f, size, s).x, y), col, s);
}
void textC(Ctx& c, ImFont* f, float size, float cx, float y, ImU32 col, const char* s) {
  text(c, f, size, ImVec2(cx - textSize(c, f, size, s).x * 0.5f, y), col, s);
}
void rect(Ctx& c, float x, float y, float w, float h, ImU32 col, float r) { c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), col, c.s(r)); }
void rectLine(Ctx& c, float x, float y, float w, float h, ImU32 col, float r, float th) {
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), col, c.s(r), 0, c.s(th));
}

double Ctx::v(int k) const {
  const PluginInfo* in = findPlugin(kType);
  const double raw = k >= 0 && static_cast<size_t>(k) < st.params.size() ? st.params[static_cast<size_t>(k)] : (in ? in->params[static_cast<size_t>(k)].def : 0.0);
  return in ? in->params[static_cast<size_t>(k)].clamp(raw) : raw;
}
double Ctx::eff(int k) const {
  double m[8];
  for (int i = 0; i < 8; ++i) m[i] = hasLive ? live.macros[i] : v(i);
  return applyMacros(k, v(k), m);
}

// ------------------------------------------------------------------ edits

void setParam(Ctx& c, int k, double v, bool merge) { pui::setParamValue(c.a, c.t, k, v, merge); }

void setState(Ctx& c, const std::vector<float>& params, const char* label) {
  PluginState s = c.st;
  s.params = params;
  setPluginState(c.a, c.t, s, label);
}

void loadFactory(Ctx& c, int index) {
  const PluginInfo* in = findPlugin(kType);
  if (!in || index < 0 || static_cast<size_t>(index) >= in->presets.size()) return;
  PluginState s = presetState(*in, static_cast<size_t>(index));
  // keep the dice locks and the per-instance settings
  for (int k = PQuality; k < PVersion; ++k)
    if (static_cast<size_t>(k) < c.st.params.size()) s.params[static_cast<size_t>(k)] = c.st.params[static_cast<size_t>(k)];
  s.original = c.st.original;
  setPluginState(c.a, c.t, s, std::string(tr("Load preset")) + " " + in->presets[static_cast<size_t>(index)].name);
  c.u.lastPreset = index;
  c.u.presetName = in->presets[static_cast<size_t>(index)].name;
  pui::presetLoaded(c.t, kType, c.u.presetName);
}

// ------------------------------------------------------------------ controls

namespace {
bool blocked(Ctx& c);
}

bool hit(Ctx& c, const char* id, float x, float y, float w, float h, bool* hov) {
  if (hov) *hov = false;
  if (blocked(c)) return false;
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, c.s(w)), std::max(1.0f, c.s(h))));
  if (hov) *hov = ImGui::IsItemHovered();
  return ImGui::IsItemClicked(ImGuiMouseButton_Left);
}

bool button(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool on, float fontSize) {
  bool hov = false;
  const bool clicked = hit(c, id, x, y, w, h, &hov);
  const Look& L = c.L;
  rect(c, x, y, w, h, on ? L.text : (hov ? theme::mix(L.panel2, L.text, 0.08f) : L.panel2), 6);
  textC(c, fBold(), fontSize, x + w * 0.5f, y + (h - 16) * 0.5f, on ? L.ink : L.text, label);
  return clicked;
}

namespace {

// Tile knob: a black disc with ticks around it, a light value arc and pointer.
void drawTileKnob(Ctx& c, ImVec2 C, float r, float n, ImU32 tick, bool hot) {
  const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  for (int i = 0; i <= 10; ++i) {
    const float a = a0 + (a1 - a0) * i / 10.0f;
    const ImVec2 u(std::cos(a), std::sin(a));
    c.dl->AddLine(ImVec2(C.x + u.x * (r + c.s(2)), C.y + u.y * (r + c.s(2))), ImVec2(C.x + u.x * (r + c.s(5)), C.y + u.y * (r + c.s(5))), tick, c.s(1.6f));
  }
  const float dr = r - c.s(2);
  c.dl->AddCircleFilled(ImVec2(C.x, C.y + c.s(1.5f)), dr + c.s(1), theme::withAlpha(c.L.ink, 0.25f), 48);
  c.dl->AddCircleFilled(C, dr, hot ? theme::mix(c.L.knob, c.L.white, 0.05f) : c.L.knob, 48);
  const float an = a0 + (a1 - a0) * std::clamp(n, 0.0f, 1.0f);
  if (n > 0.002f) {
    c.dl->PathArcTo(C, dr - c.s(5), a0, an, 40);
    c.dl->PathStroke(c.L.white, 0, c.s(4));
  }
  const ImVec2 u(std::cos(an), std::sin(an));
  c.dl->AddLine(ImVec2(C.x + u.x * (dr - c.s(14) * r / c.s(37)), C.y + u.y * (dr - c.s(14) * r / c.s(37))), ImVec2(C.x + u.x * (dr - c.s(4)), C.y + u.y * (dr - c.s(4))),
                c.L.white, c.s(3.2f) * std::max(0.6f, r / c.s(37)));
}

// Page knob: a rail ring with the value arc in the group colour, a black disc and pointer;
// the macros' share shows as a thin second arc outside.
void drawArcKnob(Ctx& c, ImVec2 C, float r, float n, float nEff, bool bip, ImU32 col, ImU32 macroCol, bool hot) {
  const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  c.dl->PathArcTo(C, r, a0, a1, 40);
  c.dl->PathStroke(c.L.panel2, 0, c.s(3));
  const float from = bip ? (a0 + a1) * 0.5f : a0;
  const float an = a0 + (a1 - a0) * std::clamp(n, 0.0f, 1.0f);
  if (std::fabs(an - from) > 0.01f) {
    c.dl->PathArcTo(C, r, std::min(from, an), std::max(from, an), 32);
    c.dl->PathStroke(col, 0, c.s(3));
  }
  if (std::fabs(nEff - n) > 0.004f) {
    const float ae = a0 + (a1 - a0) * std::clamp(nEff, 0.0f, 1.0f);
    c.dl->PathArcTo(C, r + c.s(3.5f), std::min(an, ae), std::max(an, ae), 24);
    c.dl->PathStroke(macroCol, 0, c.s(1.6f));
  }
  const float dr = r - c.s(6);
  c.dl->AddCircleFilled(C, dr, hot ? theme::mix(c.L.knob, c.L.white, 0.06f) : c.L.knob, 32);
  const ImVec2 u(std::cos(an), std::sin(an));
  c.dl->AddLine(ImVec2(C.x + u.x * dr * 0.3f, C.y + u.y * dr * 0.3f), ImVec2(C.x + u.x * (dr - c.s(2.5f)), C.y + u.y * (dr - c.s(2.5f))), c.L.white, c.s(2.2f));
}

void padlock(Ctx& c, ImVec2 C, float sz, ImU32 col, bool closed) {
  const float w = sz, h = sz * 0.62f;
  c.dl->AddRectFilled(ImVec2(C.x - w / 2, C.y - h / 2 + sz * 0.18f), ImVec2(C.x + w / 2, C.y + h / 2 + sz * 0.18f), col, sz * 0.15f);
  const float sr = sz * 0.28f;
  const ImVec2 sc(C.x, C.y - h / 2 + sz * 0.18f);
  c.dl->PathArcTo(sc, sr, kPi, kPi * 2, 12);
  if (closed) c.dl->PathLineTo(ImVec2(sc.x + sr, sc.y + 1));
  c.dl->PathStroke(col, 0, sz * 0.14f);
  c.dl->AddLine(ImVec2(sc.x - sr, sc.y), ImVec2(sc.x - sr, sc.y + 1), col, sz * 0.14f);
}

std::map<uint64_t, Ui>& uis() {
  static std::map<uint64_t, Ui> m;
  return m;
}

bool gBlock = false;
bool blocked(Ctx& c) {
  (void)c;
  return gBlock;
}

double gClipboard = 0;
bool gHasClip = false;

}  // namespace

bool knob(Ctx& c, int k, float cx, float cy, float r, ImU32 col, int kind, const char* label, bool valueText) {
  const PluginInfo* in = findPlugin(kType);
  if (!in) return false;
  const ParamDesc& d = in->params[static_cast<size_t>(k)];
  double val = c.v(k);
  bool changed = false, hov = false, act = false;
  const ImVec2 C = c.p(cx, cy);
  const float R = c.s(r);
  ImGui::PushID(k);
  if (!blocked(c)) {
    ImGui::SetCursorScreenPos(ImVec2(C.x - R, C.y - R));
    ImGui::InvisibleButton("##k", ImVec2(R * 2, R * 2));
    hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    const bool right = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool savedClick = io.MouseClicked[ImGuiMouseButton_Right];
    if (right) io.MouseClicked[ImGuiMouseButton_Right] = false;  // Chater's own knob menu instead of the stock one
    if (c.u.typeParam == k) {
      ImGui::OpenPopup("##type");
      c.u.typeParam = -1;
    }
    if (prefs(c.a).knobDrag == 0 && act && d.kind != ParamKind::Choice) {
      // circular drag: the pointer's angle around the knob sets the value
      const ImVec2 m = io.MousePos;
      float ang = std::atan2(m.y - C.y, m.x - C.x);
      if (ang < kPi * 0.5f) ang += 2 * kPi;
      const double n = std::clamp((ang - kPi * 0.75f) / (kPi * 1.5f), 0.0f, 1.0f);
      const double nv = d.fromNormalized(n);
      if (nv != val) val = nv, setParam(c, k, val, true), changed = true;
      if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
      if (hov || act) c.a.setHint(d.name + ": " + display(k, val));
    } else {
      changed = pui::dragParamItem(c.a, c.t.pid(k), d, val, 160.0f, false);
    }
    io.MouseClicked[ImGuiMouseButton_Right] = savedClick;
    if (right) openKnobMenu(c, k), c.u.menuPos = io.MousePos;
    if (hov && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
      std::vector<float> p = c.st.params;
      setLocked(p, k, !locked(p, k));
      setState(c, p, locked(p, k) ? tr("Lock from dice") : tr("Unlock from dice"));
    }
  }
  ImGui::PopID();
  const float n = static_cast<float>(d.toNormalized(val));
  if (kind == KnobTile) {
    drawTileKnob(c, C, R, n, theme::withAlpha(c.L.ink, 0.45f), hov || act);
  } else {
    const float ne = static_cast<float>(d.toNormalized(c.eff(k)));
    int mac = -1;
    for (const auto& row : macroRows())
      if (row.param == k && row.kind != MkInner && mac < 0) mac = row.macro;
    drawArcKnob(c, C, R, n, ne, d.min < 0 && d.max > 0, col, mac >= 0 ? c.L.tile[mac] : col, hov || act);
    if (locked(c.st.params, k)) padlock(c, ImVec2(C.x + R * 0.95f, C.y - R * 0.95f), c.s(7), c.L.text2, true);
    if (label) {
      textC(c, fReg(), 11, cx, cy + r + 6, c.L.text2, label);
      if (valueText) textC(c, fMono(), 10.5f, cx, cy + r + 22, c.L.text, display(k, val).c_str());
    }
  }
  return changed;
}

void openKnobMenu(Ctx& c, int param) {
  c.u.menuParam = param;
  c.u.wantMenu = true;
}

void knobMenu(Ctx& c) {
  ImGui::PushID("chmenu");
  if (c.u.wantMenu) {
    ImGui::OpenPopup("##chknob");
    c.u.wantMenu = false;
  }
  const Look& L = c.L;
  ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::vec(L.menu));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::vec(theme::mix(L.menu, L.text, 0.1f)));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 2));
  ImGui::SetNextWindowSizeConstraints(ImVec2(214, 0), ImVec2(300, 2000));
  if (c.u.menuParam >= 0) ImGui::SetNextWindowPos(c.u.menuPos, ImGuiCond_Appearing);
  if (ImGui::BeginPopup("##chknob")) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    const int k = c.u.menuParam;
    const PluginInfo* in = findPlugin(kType);
    if (in && k >= 0 && k < PCount) {
      const ParamDesc& d = in->params[static_cast<size_t>(k)];
      const double val = c.v(k);
      const ImU32 hc = groupColour(L, k);
      // header: the knob's name and value on its own colour
      {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = std::max(204.0f, ImGui::GetContentRegionAvail().x);
        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + 26), hc, 5);
        dl->AddText(fBold(), 12.5f, ImVec2(p0.x + 9, p0.y + 6), L.ink, d.name.c_str());
        const std::string vs = display(k, val);
        const float tw = fMono()->CalcTextSizeA(11, 1e9f, 0, vs.c_str()).x;
        dl->AddText(fMono(), 11, ImVec2(p0.x + w - 9 - tw, p0.y + 7), L.ink, vs.c_str());
        ImGui::Dummy(ImVec2(w, 30));
      }
      if (ImGui::MenuItem(tr("Type in value"), "Enter")) c.u.typeParam = k;
      if (ImGui::MenuItem(tr("Reset to default"), display(k, d.def).c_str(), false, !d.isDefault(val))) setParam(c, k, d.def);
      if (ImGui::MenuItem(tr("Copy value"))) gClipboard = d.toNormalized(val), gHasClip = true;
      if (ImGui::MenuItem(tr("Paste value"), nullptr, false, gHasClip)) setParam(c, k, d.fromNormalized(gClipboard));
      ImGui::Separator();
      const bool lk = locked(c.st.params, k);
      const bool rolls = diceScope(k) != 0;
      if (ImGui::MenuItem(tr("Lock from dice"), "L", lk, rolls)) {
        std::vector<float> p = c.st.params;
        setLocked(p, k, !lk);
        setState(c, p, lk ? tr("Unlock from dice") : tr("Lock from dice"));
      }
      if (ImGui::MenuItem(tr("Roll this knob only"), nullptr, false, rolls && !lk)) {
        const auto p = roll(c.st.params, 0, DiceShake, c.u.rollSeed++ * 2654435761u + static_cast<uint32_t>(k), -1, k);
        setState(c, p, tr("Roll this knob"));
      }
      ImGui::Separator();
      const bool linkable = d.kind == ParamKind::Continuous && std::find(linkTargets().begin(), linkTargets().end(), k) != linkTargets().end();
      if (ImGui::BeginMenu(tr("Link to"), linkable)) {
        static const char* srcN[] = {"", i18n::N_("Sway A"), i18n::N_("Sway B"), i18n::N_("Follower"), i18n::N_("Breaks")};
        for (int s = SrcSwayA; s < SrcCount; ++s) {
          int slot = -1, free = -1;
          for (int l = 0; l < kLinks; ++l) {
            const int src = static_cast<int>(std::lround(c.v(linkParam(l, LkSource))));
            if (src == s && static_cast<int>(std::lround(c.v(linkParam(l, LkTarget)))) == k) slot = l;
            if (src == SrcOff && free < 0) free = l;
          }
          if (ImGui::MenuItem(tr(srcN[s]), nullptr, slot >= 0, slot >= 0 || free >= 0)) {
            std::vector<float> p = c.st.params;
            if (slot >= 0) {
              p[static_cast<size_t>(linkParam(slot, LkSource))] = SrcOff;
              p[static_cast<size_t>(linkParam(slot, LkDepth))] = 0;
              setState(c, p, tr("Remove link"));
            } else {
              p[static_cast<size_t>(linkParam(free, LkSource))] = static_cast<float>(s);
              p[static_cast<size_t>(linkParam(free, LkTarget))] = static_cast<float>(k);
              p[static_cast<size_t>(linkParam(free, LkDepth))] = 0.5f;
              setState(c, p, tr("Link"));
            }
          }
        }
        ImGui::EndMenu();
      }
      if (k <= PMix) {
        ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::vec(L.menu));
        if (c.u.devDrives) ImGui::OpenPopup(tr("What it drives")), c.u.devDrives = false;  // screenshots
        if (ImGui::BeginMenu(tr("What it drives"))) {
          ImDrawList* dl = ImGui::GetWindowDrawList();
          const ImVec2 p0 = ImGui::GetCursorScreenPos();
          const float w = 236;
          dl->AddRectFilled(p0, ImVec2(p0.x + w - 10, p0.y + 26), L.tile[k], 5);
          const std::string head = std::string(in->params[static_cast<size_t>(k)].name) + " " + tr("drives");
          dl->AddText(fBold(), 12.5f, ImVec2(p0.x + 9, p0.y + 6), L.ink, head.c_str());
          ImGui::Dummy(ImVec2(w - 10, 30));
          double m0[8], m1[8];
          for (int i = 0; i < 8; ++i) m0[i] = m1[i] = c.v(i);
          m0[k] = 0, m1[k] = 1;
          for (const auto& row : macroRows()) {
            if (row.macro != k) continue;
            std::string range;
            if (row.kind == MkInner && row.param == PMix) {
              range = "0 to 100%";
            } else if (row.kind == MkInner) {
              char b[32];
              std::snprintf(b, sizeof b, "0 to %.0f ct", row.span);
              range = b;
            } else {
              range = display(row.param, applyMacros(row.param, c.v(row.param), m0)) + " " + tr("to") + " " +
                      display(row.param, applyMacros(row.param, c.v(row.param), m1));
            }
            const ImVec2 q = ImGui::GetCursorScreenPos();
            dl->AddText(fReg(), 11.5f, ImVec2(q.x + 8, q.y + 5), L.text2, tr(row.label));
            const float tw = fMono()->CalcTextSizeA(10.5f, 1e9f, 0, range.c_str()).x;
            dl->AddText(fMono(), 10.5f, ImVec2(q.x + w - 18 - tw, q.y + 6), L.text, range.c_str());
            ImGui::Dummy(ImVec2(w - 10, 24));
          }
          ImGui::EndMenu();
        }
        ImGui::PopStyleColor();
      }
      automationMenuItems(c.a, c.t.pid(k));
      ImGui::Separator();
      remoteLinkMenuItems(c.a, c.t.pid(k));
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);
  ImGui::PopID();
}

float segmented(Ctx& c, int k, float x, float y, float h, const char* const* labels, int count, float w) {
  const Look& L = c.L;
  const int cur = static_cast<int>(std::lround(c.v(k)));
  float widths[16] = {};
  float total = 4;
  for (int i = 0; i < count && i < 16; ++i) widths[i] = textSize(c, fMono(), 10.5f, labels[i]).x + 14, total += widths[i] + 2;
  if (w > total) {
    const float extra = (w - total) / static_cast<float>(count);
    for (int i = 0; i < count; ++i) widths[i] += extra;
    total = w;
  }
  rect(c, x, y, total, h, L.cell, 4);
  rectLine(c, x, y, total, h, L.line, 4);
  float cx = x + 2;
  for (int i = 0; i < count && i < 16; ++i) {
    char id[16];
    std::snprintf(id, sizeof id, "##sg%d", i);
    bool hov = false;
    ImGui::PushID(k);
    if (hit(c, id, cx, y + 2, widths[i], h - 4, &hov) && i != cur) setParam(c, k, i);
    ImGui::PopID();
    if (i == cur) {
      rect(c, cx, y + 2, widths[i], h - 4, L.accentBg, 3);
      rectLine(c, cx, y + 2, widths[i], h - 4, L.accent, 3);
    } else if (hov) {
      rect(c, cx, y + 2, widths[i], h - 4, theme::withAlpha(L.text, 0.06f), 3);
    }
    textC(c, fMono(), 10.5f, cx + widths[i] * 0.5f, y + (h - 13) * 0.5f, i == cur ? L.accentInk : L.text2, labels[i]);
    cx += widths[i] + 2;
  }
  return total;
}

float ledToggle(Ctx& c, int k, float x, float y, const char* label) {
  const Look& L = c.L;
  const bool on = c.v(k) >= 0.5;
  const float w = textSize(c, fReg(), 11, label).x + 30, h = 22;
  bool hov = false;
  ImGui::PushID(k);
  if (hit(c, "##led", x, y, w, h, &hov)) setParam(c, k, on ? 0.0 : 1.0);
  ImGui::PopID();
  rect(c, x, y, w, h, hov ? theme::mix(L.cell, L.text, 0.05f) : L.cell, 4);
  rectLine(c, x, y, w, h, L.line, 4);
  const ImVec2 d = c.p(x + 11, y + h * 0.5f);
  if (on) c.dl->AddCircleFilled(d, c.s(5), theme::withAlpha(L.led, 0.3f), 16);
  c.dl->AddCircleFilled(d, c.s(3.5f), on ? L.led : L.text3, 16);
  text(c, fReg(), 11, ImVec2(x + 20, y + 4), on ? L.text : L.text2, label);
  return w;
}

void dropdown(Ctx& c, int k, float x, float y, float w, float h, const char* label) {
  const Look& L = c.L;
  const PluginInfo* in = findPlugin(kType);
  if (!in) return;
  const ParamDesc& d = in->params[static_cast<size_t>(k)];
  float bx = x;
  if (label) {
    text(c, fReg(), 11, ImVec2(x, y + (h - 14) * 0.5f), L.text3, label);
    bx = x + textSize(c, fReg(), 11, label).x + 8;
  }
  const float bw = w - (bx - x);
  bool hov = false;
  ImGui::PushID(k);
  ImGui::PushID("dd");
  if (hit(c, "##dd", bx, y, bw, h, &hov)) ImGui::OpenPopup("##ddm");
  rect(c, bx, y, bw, h, hov ? theme::mix(L.cell, L.text, 0.05f) : L.cell, 4);
  rectLine(c, bx, y, bw, h, L.line, 4);
  const int cur = static_cast<int>(std::lround(c.v(k)));
  const std::string cv = cur >= 0 && static_cast<size_t>(cur) < d.choices.size() ? d.choices[static_cast<size_t>(cur)] : "";
  text(c, fReg(), 11.5f, ImVec2(bx + 8, y + (h - 15) * 0.5f), L.text, cv.c_str());
  const ImVec2 a = c.p(bx + bw - 12, y + h * 0.5f - 2);
  c.dl->AddLine(ImVec2(a.x - c.s(3.5f), a.y - c.s(1.5f)), ImVec2(a.x, a.y + c.s(2)), L.text3, c.s(1.5f));
  c.dl->AddLine(ImVec2(a.x, a.y + c.s(2)), ImVec2(a.x + c.s(3.5f), a.y - c.s(1.5f)), L.text3, c.s(1.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::vec(L.menu));
  if (ImGui::BeginPopup("##ddm")) {
    for (size_t i = 0; i < d.choices.size(); ++i)
      if (ImGui::MenuItem(d.choices[i].c_str(), nullptr, static_cast<int>(i) == cur)) setParam(c, k, static_cast<double>(i));
    ImGui::EndPopup();
  }
  ImGui::PopStyleColor();
  ImGui::PopID();
  ImGui::PopID();
}

// ------------------------------------------------------------------ the top bar

namespace {

void diceGlyph(Ctx& c, float x, float y, float sz, ImU32 col) {
  rectLine(c, x, y, sz, sz, col, 3, 1.6f);
  for (auto [dx, dy] : {std::pair<float, float>{0.32f, 0.32f}, {0.5f, 0.5f}, {0.68f, 0.68f}}) c.dl->AddCircleFilled(c.p(x + sz * dx, y + sz * dy), c.s(1.3f), col, 8);
}

void topBar(Ctx& c) {
  const Look& L = c.L;
  rect(c, 0, 0, kW, 46, L.top, 0);
  // wordmark (the suite's Chater logo)
  pui::wordmarkAt(c.dl, kType, c.p(14, 10), c.s(26), L.text);
  // preset box
  const float px = 124, pw = 250, py = 8, ph = 30;
  rect(c, px, py, pw, ph, L.panel2, 6);
  const PluginInfo* in = findPlugin(kType);
  const int np = in ? static_cast<int>(in->presets.size()) : 1;
  bool hl = false, hr = false, hm = false;
  if (hit(c, "##pprev", px, py, 26, ph, &hl)) loadFactory(c, (c.u.lastPreset - 1 + np) % np);
  if (hit(c, "##pnext", px + pw - 26, py, 26, ph, &hr)) loadFactory(c, (c.u.lastPreset + 1) % np);
  if (hit(c, "##pname", px + 26, py, pw - 52, ph, &hm)) c.u.presets = !c.u.presets, c.u.settings = c.u.dice = false;
  textC(c, fBold(), 15, px + 13, py + 5, hl ? L.text : L.text2, "\xE2\x80\xB9");
  textC(c, fBold(), 15, px + pw - 13, py + 5, hr ? L.text : L.text2, "\xE2\x80\xBA");
  text(c, fBold(), 12, ImVec2(px + 30, py + 7), L.text, c.u.presetName.c_str());
  int bank = -1;
  if (c.u.lastPreset >= 0 && static_cast<size_t>(c.u.lastPreset) < presetMeta().size()) bank = presetMeta()[static_cast<size_t>(c.u.lastPreset)].bank;
  if (bank >= 0) {
    static const int bankTile[kBanks] = {6, 1, 4, 0, 3, 7, 2, 5};
    std::string bn = tr(bankName(bank));
    for (auto& ch : bn) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const float bw = textSize(c, fBold(), 9.5f, bn.c_str()).x + 14;
    rect(c, px + pw - 30 - bw, py + 6, bw, 18, L.tile[bankTile[bank]], 3);
    textC(c, fBold(), 9.5f, px + pw - 30 - bw * 0.5f, py + 9, L.ink, bn.c_str());
  }
  // Presets, Roll
  if (button(c, "##presets", 384, 8, 68, 30, tr("Presets"), c.u.presets)) c.u.presets = !c.u.presets, c.u.settings = c.u.dice = false;
  {
    bool hov = false;
    const bool on = c.u.dice;
    if (hit(c, "##roll", 462, 8, 67, 30, &hov)) c.u.dice = !c.u.dice, c.u.settings = c.u.presets = false;
    rect(c, 462, 8, 67, 30, on ? L.text : (hov ? theme::mix(L.panel2, L.text, 0.08f) : L.panel2), 6);
    diceGlyph(c, 474, 15.5f, 15, on ? L.ink : L.text);
    text(c, fBold(), 12, ImVec2(496, 15), on ? L.ink : L.text, tr("Roll"));
  }
  // Motion (with the count of links in use), Settings
  int links = 0;
  for (int l = 0; l < kLinks; ++l) links += static_cast<int>(std::lround(c.v(linkParam(l, LkSource)))) != SrcOff;
  {
    bool hov = false;
    const bool on = c.u.page == PgMotion && !c.u.presets;
    if (hit(c, "##motion", 795, 8, 94, 30, &hov)) c.u.page = on ? PgMain : PgMotion, c.u.presets = c.u.settings = c.u.dice = false;
    rect(c, 795, 8, 94, 30, on ? L.text : (hov ? theme::mix(L.panel2, L.text, 0.08f) : L.panel2), 6);
    text(c, fBold(), 12, ImVec2(807, 15), on ? L.ink : L.text, tr("Motion"));
    char n[12];
    std::snprintf(n, sizeof n, "%d", links);
    rect(c, 855, 14, 22, 18, theme::withAlpha(L.ink, on ? 0.25f : 0.2f), 4);
    textC(c, fMono(), 10, 866, 16, on ? L.ink : L.text2, n);
  }
  if (button(c, "##settings", 899, 8, 71, 30, tr("Settings"), c.u.settings)) c.u.settings = !c.u.settings, c.u.dice = c.u.presets = false;
  // Output knob, level and meter
  knob(c, POutput, 991, 23, 11, L.tile[7], KnobArc);
  const std::string ov = display(POutput, c.v(POutput));
  text(c, fMono(), 11, ImVec2(1008, 16), L.text2, ov.c_str());
  {
    const float lv = c.hasLive ? std::max(c.live.outL, c.live.outR) : 0.0f;
    const float db = 20 * std::log10(std::max(1e-5f, lv));
    for (int i = 0; i < 5; ++i) {
      const float h = 6 + i * 4.5f;
      const float on = std::clamp((db + 48 - i * 9) / 9.0f, 0.0f, 1.0f);
      const ImU32 col = i == 4 && db > -3 ? theme::rgb(0xFF7A4D) : L.led;
      rect(c, 1060 + i * 6, 36 - h, 4, h, theme::mix(theme::withAlpha(L.led, 0.18f), col, on), 1);
    }
  }
}

// ------------------------------------------------------------------ the glitch tape

double laneChance(Ctx& c, int beatIdx) {
  const int mode = static_cast<int>(std::lround(c.v(PTrigMode)));
  if (mode == TrigHits) return 0;
  const double dens = std::clamp(c.eff(PDensity), 0.0, 1.0);
  if (dens <= 0) return 0;
  const double grid = gridBeats(static_cast<int>(std::lround(c.v(PGrid))));
  const double barBeats = c.hasLive ? c.live.barBeats : 4.0;
  double none = 1;
  const int lean = static_cast<int>(std::lround(c.v(PLean)));
  for (double b = beatIdx; b < beatIdx + 1 - 1e-9; b += grid) {
    const double inBar = std::fmod(b, barBeats);
    const bool onBeat = std::fabs(inBar - std::round(inBar)) < 1e-6;
    double ch = dens;
    if (lean == LeanDown) ch *= onBeat ? 1.6 : 0.5;
    else if (lean == LeanOff) ch *= onBeat ? 0.25 : 1.6;
    else if (lean == LeanBarEnd) ch *= 0.25 + 2.0 * (inBar / barBeats) * (inBar / barBeats);
    none *= 1 - std::min(1.0, ch);
  }
  if (grid > 1) none = 1 - (1 - none) / grid;  // a step longer than a beat
  return 1 - none;
}

void tape(Ctx& c, bool compact) {
  const Look& L = c.L;
  const float x0 = 14, y0 = 58, w = 1072, h = compact ? 120 : 178;
  rect(c, x0, y0, w, h, L.tape, 10);
  const float wx = x0 + 12, ww = w - 24, wy = y0 + 6, wh = compact ? 64 : 104;
  const double barBeats = c.hasLive ? std::max(0.25, c.live.barBeats) : 4.0;
  const double win = 4 * barBeats;
  const double rel = c.hasLive ? c.live.beat - c.live.barOrigin : 0.0;
  double inWin = std::fmod(rel, win);
  if (inWin < 0) inWin += win;
  // bar lines and numbers
  for (int b = 0; b < 4; ++b) {
    const float bx = wx + ww * static_cast<float>(b / 4.0);
    if (b > 0) c.dl->AddLine(c.p(bx, wy + 10), c.p(bx, wy + wh), theme::withAlpha(L.text, 0.05f), c.s(1));
    if (!compact) {
      char n[8];
      std::snprintf(n, sizeof n, "%d", b + 1);
      text(c, fMono(), 9, ImVec2(bx + 4, wy), L.text3, n);
    }
  }
  // break regions (the live events of this pass)
  const float top = wy + (compact ? 4 : 12), bot = wy + wh - 4;
  const double now = c.hasLive ? c.live.beat : 0.0;
  if (c.hasLive)
    for (const FiredBreak& e : c.live.events) {
      if (!e.seq || e.type <= BrNone || e.beat > now + 1e-6 || now - e.beat >= win - 1e-6) continue;
      double s = std::fmod(e.beat - c.live.barOrigin, win);
      if (s < 0) s += win;
      const float rx0 = wx + ww * static_cast<float>(s / win);
      const float rx1 = std::min(wx + ww, wx + ww * static_cast<float>((s + std::min(e.beats, now - e.beat + 1e-9)) / win));
      if (rx1 <= rx0 + 0.5f) continue;
      const ImU32 col = breakColour(L, e.type);
      // soft edges fade the region in and out; a washed break leaves a tail into the space
      const float soft = static_cast<float>(c.eff(PSoftness)), wash = static_cast<float>(c.eff(PWash));
      const float ew = std::min((rx1 - rx0) * 0.45f, (rx1 - rx0) * 0.3f * soft);
      const ImU32 fill = theme::withAlpha(col, 0.16f), clear = theme::withAlpha(col, 0.0f);
      if (ew > 1) {
        c.dl->AddRectFilledMultiColor(c.p(rx0, top), c.p(rx0 + ew, bot), clear, fill, fill, clear);
        c.dl->AddRectFilled(c.p(rx0 + ew, top), c.p(rx1 - ew, bot), fill);
        c.dl->AddRectFilledMultiColor(c.p(rx1 - ew, top), c.p(rx1, bot), fill, clear, clear, fill);
      } else {
        c.dl->AddRectFilled(c.p(rx0, top), c.p(rx1, bot), fill, c.s(2));
      }
      const double ended = e.beat + e.beats;
      if (wash > 0.02f && ended < now) {
        const float tx1 = std::min({wx + ww, rx1 + (rx1 - rx0) * 1.2f * wash, wx + ww * static_cast<float>(inWin / win)});
        if (tx1 > rx1 + 1)
          c.dl->AddRectFilledMultiColor(c.p(rx1, top + (bot - top) * 0.35f), c.p(tx1, bot), theme::withAlpha(L.b[7], 0.14f * wash + 0.04f), clear, clear,
                                        theme::withAlpha(L.b[7], 0.14f * wash + 0.04f));
      }
      c.dl->AddRectFilled(c.p(rx0 + ew * 0.5f, top), c.p(rx1 - ew * 0.5f, top + 2), col);
      if (!compact && rx1 - rx0 > 26) {
        char lab[40];
        if (e.type == BrRepeat && e.repeats > 1) std::snprintf(lab, sizeof lab, "%s x%d", breakName(e.type), e.repeats);
        else std::snprintf(lab, sizeof lab, "%s", breakName(e.type));
        c.dl->PushClipRect(c.p(rx0, top), c.p(rx1, bot), true);
        text(c, fBold(), 9.5f, ImVec2(rx0 + 4, top + 4), col, lab);
        c.dl->PopClipRect();
      }
    }
  // the waveform: one column per slice of the four bars
  const float cy = (top + 2 + bot) * 0.5f, half = (bot - top - 6) * 0.5f;
  const float colW = ww / kTapeCols;
  for (int i = 0; i < kTapeCols; ++i) {
    const float pk = c.hasLive ? std::min(1.0f, std::sqrt(c.live.peak[i]) * 1.1f) : 0.0f;
    const float hh = std::max(0.6f, pk * half);
    const int ty = c.hasLive ? c.live.type[i] : 0;
    const float xx = wx + i * colW + colW * 0.5f;
    c.dl->AddLine(c.p(xx, cy - hh), c.p(xx, cy + hh), ty ? breakColour(L, ty) : L.wave, std::max(1.0f, c.s(colW * 0.62f)));
  }
  // playhead
  if (c.hasLive) {
    const float px = wx + ww * static_cast<float>(inWin / win);
    c.dl->AddLine(c.p(px, top - 4), c.p(px, bot), L.accent, c.s(1.5f));
  }
  // chips: tempo and bar
  if (!compact) {
    char t1[24], t2[24];
    std::snprintf(t1, sizeof t1, "%.0f BPM", c.hasLive ? c.live.tempo : 120.0);
    std::snprintf(t2, sizeof t2, "%s %d", tr("Bar"), c.hasLive ? static_cast<int>(std::floor(inWin / barBeats)) + 1 : 1);
    const float w2 = textSize(c, fMono(), 10, t2).x + 16, w1 = textSize(c, fMono(), 10, t1).x + 16;
    const float r = x0 + w - 14;
    rect(c, r - w2, y0 + 8, w2, 20, L.cellLine, 4);
    text(c, fMono(), 10, ImVec2(r - w2 + 8, y0 + 11), L.text2, t2);
    rect(c, r - w2 - 6 - w1, y0 + 8, w1, 20, L.cellLine, 4);
    text(c, fMono(), 10, ImVec2(r - w2 - 6 - w1 + 8, y0 + 11), L.text2, t1);
  }
  // the lane: one cell per beat, read-only: the grid's chance and the breaks that fired
  const float lx = x0 + 8, lw = w - 16, lh = compact ? 40 : 62, ly = y0 + h - 6 - lh;
  const float cw = (lw - 15 * 4) / 16.0f;
  const int nowBeat = static_cast<int>(std::floor(inWin));
  for (int b = 0; b < 16; ++b) {
    const float cx = lx + b * (cw + 4);
    rect(c, cx, ly, cw, lh, L.cell, 3);
    rectLine(c, cx, ly, cw, lh, b % 4 == 0 ? theme::mix(L.cellLine, L.text, 0.08f) : L.cellLine, 3);
    // what fired in this beat during this pass
    int fired = BrNone;
    if (c.hasLive)
      for (const FiredBreak& e : c.live.events) {
        if (!e.seq || e.type <= BrNone || e.beat > now + 1e-6 || now - e.beat >= win - 1e-6) continue;
        double s = std::fmod(e.beat - c.live.barOrigin, win);
        if (s < 0) s += win;
        if (static_cast<int>(std::floor(s + 1e-6)) == b) fired = e.type;
      }
    const double chance = laneChance(c, b);
    const float fillH = static_cast<float>(fired ? std::max(chance, 0.35) : chance) * (lh - 2);
    if (fillH > 0.5f) {
      const ImU32 col = fired ? breakColour(L, fired) : L.accent;
      c.dl->AddRectFilledMultiColor(c.p(cx + 1, ly + lh - 1 - fillH), c.p(cx + cw - 1, ly + lh - 1), theme::withAlpha(col, fired ? 0.3f : 0.12f),
                                    theme::withAlpha(col, fired ? 0.3f : 0.12f), theme::withAlpha(col, fired ? 0.7f : 0.3f), theme::withAlpha(col, fired ? 0.7f : 0.3f));
    }
    if (!compact && chance > 0.005) {
      char n[8];
      std::snprintf(n, sizeof n, "%d", static_cast<int>(std::lround(chance * 100)));
      textC(c, fMono(), 9.5f, cx + cw * 0.5f, ly + 3, L.text, n);
    }
    // the break that fired here (its initial), or "?" where one may fire
    char mark[8] = {};
    if (fired) {
      const char* nm = breakName(fired);
      int n = 1;
      while (nm[n] && (static_cast<unsigned char>(nm[n]) & 0xC0) == 0x80) ++n;  // one UTF-8 letter
      std::memcpy(mark, nm, static_cast<size_t>(std::min(n, 4)));
    } else if (chance > 0.005) {
      mark[0] = '?';
    }
    if (mark[0]) textC(c, fBold(), 10, cx + cw * 0.5f, ly + lh - (compact ? 14 : 16), fired ? L.text : L.text2, mark);
    if (c.hasLive && b == nowBeat) rectLine(c, cx - 1, ly - 1, cw + 2, lh + 2, L.accent, 3, 1.5f);
  }
}

// ------------------------------------------------------------------ the macro tiles

ImU32 dimmed(ImU32 col) {
  const ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
  const float lum = 0.3f * v.x + 0.59f * v.y + 0.11f * v.z;
  auto f = [&](float ch) { return (lum + (ch - lum) * 0.25f) * 0.65f; };
  return ImGui::ColorConvertFloat4ToU32(ImVec4(f(v.x), f(v.y), f(v.z), 1));
}

const char* tileText(int i) {
  static const char* d[8] = {i18n::N_("Soft grains, shimmer, fairy twinkle"), i18n::N_("Damp, warmth, softer breaks"),
                             i18n::N_("Wow, flutter, worn tape, ensemble"),    i18n::N_("Slow reverses, tape sighs, pitch dips"),
                             i18n::N_("Crystal ring, halo lift, twinkle"),     i18n::N_("Sharp cuts, jumps, bits and rate"),
                             i18n::N_("Long hall, shimmer, echoes"),           i18n::N_("Dry, wet and duck")};
  return tr(d[std::clamp(i, 0, 7)]);
}

void stripes(Ctx& c, float x, float y, float w, float h, ImU32 col) {
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  for (float s = -h; s < w + h; s += 20) {
    const ImVec2 a = c.p(x + s, y), b = c.p(x + s + 10, y), d = c.p(x + s - h, y + h), e = c.p(x + s + 10 - h, y + h);
    c.dl->AddQuadFilled(a, b, e, d, col);
  }
  c.dl->PopClipRect();
}

void tiles(Ctx& c, bool compact) {
  const Look& L = c.L;
  const float y = compact ? 188 : 248, h = compact ? 112 : 262, w = 127;
  const int open = shownTile(c.u);
  for (int i = 0; i < 8; ++i) {
    const float x = 14 + i * (w + 8);
    const bool pick = compact && open == i;
    const bool dim = compact && open != i;
    const ImU32 base = dim ? dimmed(L.tile[i]) : L.tile[i];
    const bool rolled = c.u.dice && (c.u.rolledMask >> i & 1u);
    rect(c, x, y, w, h, base, 10);
    if (rolled) stripes(c, x, y, w, h, theme::withAlpha(L.white, 0.12f));
    // soft light from the top
    for (int s = 0; s < 8; ++s) {
      const float sy = y + h * 0.4f * s / 8.0f;
      c.dl->AddRectFilled(c.p(x, sy), c.p(x + w, sy + h * 0.05f), theme::withAlpha(L.white, 0.13f * (1 - s / 8.0f)), s == 0 ? c.s(10) : 0,
                          s == 0 ? ImDrawFlags_RoundCornersTop : ImDrawFlags_None);
    }
    c.dl->AddRectFilled(c.p(x, y + h * 0.75f), c.p(x + w, y + h), theme::withAlpha(L.ink, 0.06f), c.s(10), ImDrawFlags_RoundCornersBottom);
    if (pick) rectLine(c, x - 1.5f, y - 1.5f, w + 3, h + 3, L.white, 11, 3);
    // click on the tile body: open or close its page
    ImGui::PushID(i);
    bool hov = false;
    const PluginInfo* in = findPlugin(kType);
    const char* nm = in ? in->params[static_cast<size_t>(i)].name.c_str() : "";
    char val[8];
    const double v = c.v(i);
    std::snprintf(val, sizeof val, "%d", static_cast<int>(std::lround(v * 100)));
    const ImU32 ink = L.ink;
    if (compact) {
      if (hit(c, "##tile", x, y, 60, h, &hov)) c.u.page = pick ? PgMain : tilePage(i), c.u.openTile = pick ? -1 : i;
      text(c, fBold(), 15, ImVec2(x + 8, y + 10), ink, tr(nm));
      text(c, fMonoBold(), 16, ImVec2(x + 8, y + 29), ink, val);
      knob(c, i, x + w - 36, y + h * 0.5f + 4, 22, L.white, KnobTile);
    } else {
      text(c, fBold(), 16, ImVec2(x + 8, y + 14), ink, tr(nm));
      text(c, fMonoBold(), 28, ImVec2(x + 8, y + 31), ink, val);
      knob(c, i, x + w * 0.5f, y + 114, 37, L.white, KnobTile);
      // description, wrapped above the bottom edge
      const char* d = tileText(i);
      const float tw = w - 16 - 24;
      ImFont* f = fReg();
      const float fs = c.s(11.0f * kTextScale);
      const ImVec2 sz = f->CalcTextSizeA(fs, 1e9f, c.s(tw), d);
      const ImVec2 dp = c.p(x + 8, y + h - 10);
      c.dl->AddText(f, fs, ImVec2(std::round(dp.x), std::round(dp.y - sz.y)), theme::withAlpha(ink, 0.75f), d, nullptr, c.s(tw));
      // lock
      {
        const bool lk = locked(c.st.params, i);
        bool lh = false;
        if (hit(c, "##lock", x + w - 30, y + 9, 22, 22, &lh)) {
          std::vector<float> p = c.st.params;
          setLocked(p, i, !lk);
          setState(c, p, lk ? tr("Unlock from dice") : tr("Lock from dice"));
        }
        rect(c, x + w - 30, y + 9, 22, 22, lk ? ink : theme::withAlpha(ink, lh ? 0.2f : 0.12f), 5);
        padlock(c, c.p(x + w - 19, y + 20), c.s(10), lk ? base : theme::withAlpha(ink, 0.45f), lk);
        if (lh) c.a.setHint(lk ? tr("Locked: the dice leave this macro alone (click to unlock)") : tr("Lock this macro from the dice"));
      }
      // open the page
      {
        bool oh = false;
        if (hit(c, "##open", x + w - 30, y + h - 30, 22, 22, &oh)) c.u.page = tilePage(i), c.u.openTile = i;
        rect(c, x + w - 30, y + h - 30, 22, 22, theme::withAlpha(ink, oh ? 0.24f : 0.14f), 5);
        const ImVec2 a = c.p(x + w - 19, y + h - 19);
        c.dl->AddLine(ImVec2(a.x - c.s(3), a.y - c.s(1.5f)), ImVec2(a.x, a.y + c.s(1.5f)), ink, c.s(1.6f));
        c.dl->AddLine(ImVec2(a.x, a.y + c.s(1.5f)), ImVec2(a.x + c.s(3), a.y - c.s(1.5f)), ink, c.s(1.6f));
        if (oh) c.a.setHint(tr("Open this macro's page"));
      }
    }
    ImGui::PopID();
  }
}

// ------------------------------------------------------------------ main page: break odds and quick

void quick(Ctx& c) {
  const Look& L = c.L;
  const float x0 = 14, y0 = 522, w = 1072, h = 144;
  rect(c, x0, y0, w, h, L.panel, 10);
  const float split = x0 + w * 1.6f / 2.6f;
  c.dl->AddLine(c.p(split, y0), c.p(split, y0 + h), L.line, c.s(1));
  // break odds
  text(c, fBold(), 12, ImVec2(x0 + 16, y0 + 16), L.text2, tr("BREAK ODDS"));
  text(c, fReg(), 11, ImVec2(x0 + 16 + textSize(c, fBold(), 12, tr("BREAK ODDS")).x + 8, y0 + 17), L.text3, tr("which break fires when one does"));
  const float fx = x0 + 16, fw = split - x0 - 32, colW = (fw - 50) / 6;
  const PluginInfo* in = findPlugin(kType);
  for (int i = 0; i < 6; ++i) {
    const int k = POddRepeat + i;
    const float cx = fx + i * (colW + 10), by = y0 + 42, bh = 52;
    rect(c, cx, by, colW, bh, L.cell, 6);
    double val = c.v(k);
    const float fill = static_cast<float>(val / 100.0) * bh;
    if (fill > 0.5f) c.dl->AddRectFilled(c.p(cx, by + bh - fill), c.p(cx + colW, by + bh), L.ev[i], c.s(6), fill > bh - 6 ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersBottom);
    if (!gBlock && in) {
      ImGui::PushID(k);
      ImGui::SetCursorScreenPos(c.p(cx, by));
      ImGui::InvisibleButton("##odds", ImVec2(c.s(colW), c.s(bh)));
      pui::dragParamTrack(c.a, c.t.pid(k), in->params[static_cast<size_t>(k)], val, c.p(cx, by).y, c.s(bh), false);
      ImGui::PopID();
    }
    text(c, fBold(), 11, ImVec2(cx, by + bh + 6), L.text, breakName(i + 1));
    char n[12];
    std::snprintf(n, sizeof n, "%d", static_cast<int>(std::lround(val)));
    textR(c, fMono(), 10.5f, cx + colW, by + bh + 7, L.text2, n);
  }
  // quick
  const float qx = split + 16;
  text(c, fBold(), 12, ImVec2(qx, y0 + 16), L.text2, tr("QUICK"));
  {
    const int mode = static_cast<int>(std::lround(c.v(PTrigMode)));
    char sub[96];
    if (mode == TrigHits) std::snprintf(sub, sizeof sub, "%s", tr("breaks fire on the input's hits"));
    else std::snprintf(sub, sizeof sub, "%s %s, %s %s", tr("grid"), display(PGrid, c.v(PGrid)).c_str(), tr("density"), display(PDensity, c.eff(PDensity)).c_str());
    text(c, fReg(), 11, ImVec2(qx + textSize(c, fBold(), 12, tr("QUICK")).x + 8, y0 + 17), L.text3, sub);
  }
  auto pill = [&](const char* id, float x, float y, const char* label, ImU32 dot, bool on) {
    const float pw = textSize(c, fBold(), 11.5f, label).x + (dot ? 31 : 20);
    bool hov = false;
    const bool clicked = hit(c, id, x, y, pw, 28, &hov);
    rect(c, x, y, pw, 28, on ? L.text : hov ? theme::mix(L.cell, L.text, 0.06f) : L.cell, 6);
    if (dot) rect(c, x + 10, y + 10, 8, 8, dot, 2);
    text(c, fBold(), 11.5f, ImVec2(x + (dot ? 25 : 10), y + 7), on ? L.ink : L.text2, label);
    return std::pair<bool, float>(clicked, pw);
  };
  float px = qx;
  const float r1 = y0 + 46, r2 = y0 + 82;
  {
    auto [cl, pw] = pill("##hold", px, r1, tr("Hold capture"), L.b[0], c.v(PHold) >= 0.5);
    if (cl) setParam(c, PHold, c.v(PHold) >= 0.5 ? 0.0 : 1.0);
    px += pw + 6;
  }
  {
    auto [cl, pw] = pill("##freeze", px, r1, tr("Freeze space"), L.b[7], c.v(PRmFreeze) >= 0.5);
    if (cl) setParam(c, PRmFreeze, c.v(PRmFreeze) >= 0.5 ? 0.0 : 1.0);
    px += pw + 6;
  }
  {
    const int mode = static_cast<int>(std::lround(c.v(PTrigMode)));
    const bool hits = mode != TrigGrid;
    auto [cl, pw] = pill("##hits", px, r1, tr("Breaks on hits"), 0, hits);
    if (cl) setParam(c, PTrigMode, hits ? TrigGrid : TrigBoth);
    if (mode == TrigHits && cl) setParam(c, PTrigMode, TrigGrid);
  }
  px = qx;
  {
    const int scale = static_cast<int>(std::lround(c.v(PScale)));
    std::string lab;
    if (scale == 0) lab = tr("No scale");
    else lab = display(PRoot, c.v(PRoot)) + " " + display(PScale, scale);
    for (size_t i = 1; i < lab.size(); ++i) lab[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lab[i])));
    auto [cl, pw] = pill("##scale", px, r2, lab.c_str(), L.g[1], false);
    if (cl) ImGui::OpenPopup("##scalemenu");
    px += pw + 6;
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::vec(L.menu));
    if (ImGui::BeginPopup("##scalemenu")) {
      const PluginInfo* info = findPlugin(kType);
      if (info) {
        for (size_t i = 0; i < info->params[PScale].choices.size(); ++i)
          if (ImGui::MenuItem(info->params[PScale].choices[i].c_str(), nullptr, static_cast<int>(i) == scale)) setParam(c, PScale, static_cast<double>(i));
        ImGui::Separator();
        if (ImGui::BeginMenu(tr("Root"))) {
          const int root = static_cast<int>(std::lround(c.v(PRoot)));
          for (size_t i = 0; i < info->params[PRoot].choices.size(); ++i)
            if (ImGui::MenuItem(info->params[PRoot].choices[i].c_str(), nullptr, static_cast<int>(i) == root)) setParam(c, PRoot, static_cast<double>(i));
          ImGui::EndMenu();
        }
      }
      ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
  }
  {
    const std::string lab = std::string(tr("Lean")) + ": " + display(PLean, c.v(PLean));
    auto [cl, pw] = pill("##lean", px, r2, lab.c_str(), L.b[2], false);
    (void)pw;
    if (cl) setParam(c, PLean, (static_cast<int>(std::lround(c.v(PLean))) + 1) % 4);
  }
}

// ------------------------------------------------------------------ dev views (screenshots)

// Scripted checks (--dev-input "chater ..."): a click at a point of the 1100 x 680 form, the
// knob menu, and the editor's state to stdout (tests/k189_chater_pages.cmake).
struct DevQueue {
  ImVec2 at{0, 0};
  int tries = 0;
  int stage = 0;  // 1: move, 2: press once the pointer arrived, 3: release
  float fx = 0, fy = 0;
  bool report = false, menu = false;
} gDev;

void devClick(Ctx& c) {
  ImGuiIO& io = ImGui::GetIO();
  // Move, then press, then release on later frames, so hover is settled before the press. The
  // editor is drawn at its design size and zoomed as one picture, and the window maps the
  // pointer back into design space: inject where it shows, then correct by what arrives.
  const ImVec2 want = c.p(gDev.fx, gDev.fy);
  const float z = std::max(0.1f, edzoomui::currentZoom());
  if (gDev.stage == 1) {
    gDev.at = edzoomui::toScreenHere(want), gDev.tries = 0;
    io.AddMousePosEvent(gDev.at.x, gDev.at.y);
    gDev.stage = 2;
  } else if (gDev.stage == 2) {
    const ImVec2 got = io.MousePos;
    if ((std::fabs(got.x - want.x) > 1 || std::fabs(got.y - want.y) > 1) && gDev.tries++ < 4) {
      gDev.at.x += (want.x - got.x) * z, gDev.at.y += (want.y - got.y) * z;
      io.AddMousePosEvent(gDev.at.x, gDev.at.y);
    } else {
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      gDev.stage = 3;
    }
  } else if (gDev.stage == 3) {
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    gDev.stage = 0;
  }
  if (gDev.menu) {
    gDev.menu = false;
    openKnobMenu(c, PSparkle);
    c.u.menuPos = c.p(110, 210);
  }
}

void devReport(Ctx& c) {
  if (!gDev.report) return;
  gDev.report = false;
  ImGui::PushID("chmenu");
  const bool menu = ImGui::IsPopupOpen("##chknob");
  ImGui::PopID();
  std::printf("chater state: page=%d settings=%d presets=%d dice=%d menu=%d\n", c.u.page, c.u.settings ? 1 : 0, c.u.presets ? 1 : 0, c.u.dice ? 1 : 0, menu ? 1 : 0);
  std::fflush(stdout);
}

void devView(Ctx& c) {
  static const char* view = std::getenv("GL_CHATER_VIEW");
  static const char* preset = std::getenv("GL_CHATER_PRESET");
  ++c.u.devFrames;
  if (c.u.devFrames == 2 && preset) {
    const PluginInfo* in = findPlugin(kType);
    if (in)
      for (size_t i = 0; i < in->presets.size(); ++i)
        if (in->presets[i].name == preset) loadFactory(c, static_cast<int>(i));
  }
  if (c.u.devFrames != 4 || !view) return;  // after the preset has reached the state
  const std::string v = view;
  if (v == "fracture") c.u.page = PgFracture;
  else if (v == "glitter") c.u.page = PgGlitter;
  else if (v == "colour") c.u.page = PgColour;
  else if (v == "space") c.u.page = PgSpace;
  else if (v == "haze") c.u.page = PgHaze;
  else if (v == "motion") c.u.page = PgMotion;
  else if (v == "presets") c.u.presets = true;
  else if (v == "settings") c.u.settings = true;
  else if (v == "random") {
    c.u.dice = true;
    std::vector<float> p = c.st.params;
    setLocked(p, PSparkle, true), setLocked(p, PShine, true), setLocked(p, PMix, true);
    c.u.history = {p};
    c.u.historyAt = 0;
    const auto r = roll(p, c.u.diceScopes, DiceShake, 4, 0);
    c.u.rolledMask = 0;
    for (int i = 0; i < 8; ++i)
      if (r[static_cast<size_t>(i)] != p[static_cast<size_t>(i)]) c.u.rolledMask |= 1u << i;
    c.u.history.push_back(r);
    c.u.historyAt = 1;
    setState(c, r, tr("Roll the dice"));
  } else if (v == "menu") {
    openKnobMenu(c, PSparkle);
    c.u.menuPos = c.p(110, 210);
    c.u.devDrives = true;
  }
}

}  // namespace

// ------------------------------------------------------------------ the dice popover

void drawDice(Ctx& c) {
  const Look& L = c.L;
  const float x = 364, y = 52, w = 330, h = 208;
  c.dl->AddRectFilled(c.p(x + 2, y + 8), c.p(x + w + 2, y + h + 12), theme::withAlpha(L.ink, 0.35f), c.s(10));
  rect(c, x, y, w, h, L.menu, 8);
  rectLine(c, x, y, w, h, L.line, 8);
  text(c, fBold(), 13.5f, ImVec2(x + 14, y + 13), L.text, tr("Roll the dice"));
  textR(c, fReg(), 11, x + w - 14, y + 15, L.text3, tr("Locked knobs stay put"));
  text(c, fReg(), 11.5f, ImVec2(x + 14, y + 44), L.text2, tr("Amount"));
  {
    static const char* am[3] = {i18n::N_("Nudge"), i18n::N_("Shake"), i18n::N_("New sound")};
    float cx = x + 94;
    for (int i = 0; i < 3; ++i) {
      const char* lab = tr(am[i]);
      const float bw = textSize(c, fMono(), 10.5f, lab).x + 16;
      char id[12];
      std::snprintf(id, sizeof id, "##am%d", i);
      bool hov = false;
      if (hit(c, id, cx, y + 40, bw, 22, &hov)) c.u.diceAmount = i;
      const bool on = c.u.diceAmount == i;
      rect(c, cx, y + 40, bw, 22, on ? L.accentBg : L.cell, 3);
      rectLine(c, cx, y + 40, bw, 22, on ? L.accent : L.line, 3);
      textC(c, fMono(), 10.5f, cx + bw * 0.5f, y + 44, on ? L.accentInk : L.text2, lab);
      cx += bw + 2;
    }
  }
  text(c, fReg(), 11.5f, ImVec2(x + 14, y + 86), L.text2, tr("What rolls"));
  {
    float cx = x + 94, cy = y + 72;
    for (int i = 0; i < kDiceScopes; ++i) {
      const char* lab = tr(scopeName(i));
      const float bw = textSize(c, fReg(), 11, lab).x + 18;
      if (cx + bw > x + w - 10) cx = x + 94, cy += 26;
      char id[12];
      std::snprintf(id, sizeof id, "##sc%d", i);
      bool hov = false;
      if (hit(c, id, cx, cy, bw, 22, &hov)) c.u.diceScopes ^= 1 << i;
      const bool on = c.u.diceScopes & (1 << i);
      rect(c, cx, cy, bw, 22, on ? L.accentBg : (hov ? theme::withAlpha(L.text, 0.05f) : L.menu), 11);
      rectLine(c, cx, cy, bw, 22, on ? L.accent : L.line, 11);
      textC(c, fReg(), 11, cx + bw * 0.5f, cy + 4, on ? L.accentInk : L.text3, lab);
      cx += bw + 4;
    }
  }
  text(c, fReg(), 11.5f, ImVec2(x + 14, y + 132), L.text2, tr("History"));
  {
    const int n = static_cast<int>(c.u.history.size());
    char hs[48];
    std::snprintf(hs, sizeof hs, "%s %d %s %d", tr("Roll"), std::max(0, c.u.historyAt), tr("of"), std::max(0, n - 1));
    bool hl = false, hr = false;
    if (hit(c, "##hprev", x + 92, y + 128, 16, 20, &hl) && c.u.historyAt > 0) {
      --c.u.historyAt;
      setState(c, c.u.history[static_cast<size_t>(c.u.historyAt)], tr("Earlier roll"));
    }
    textC(c, fBold(), 13, x + 100, y + 129, c.u.historyAt > 0 ? L.text2 : L.text3, "\xE2\x80\xB9");
    text(c, fReg(), 11.5f, ImVec2(x + 112, y + 132), L.text, hs);
    const float tw = textSize(c, fReg(), 11.5f, hs).x;
    if (hit(c, "##hnext", x + 116 + tw, y + 128, 16, 20, &hr) && c.u.historyAt + 1 < n) {
      ++c.u.historyAt;
      setState(c, c.u.history[static_cast<size_t>(c.u.historyAt)], tr("Later roll"));
    }
    textC(c, fBold(), 13, x + 124 + tw, y + 129, c.u.historyAt + 1 < n ? L.text2 : L.text3, "\xE2\x80\xBA");
  }
  c.dl->AddLine(c.p(x + 14, y + 158), c.p(x + w - 14, y + 158), L.line, c.s(1));
  {
    bool hov = false;
    bool all = true;
    for (int i = 0; i < 8; ++i) all = all && locked(c.st.params, i);
    const char* lab = all ? tr("Unlock all macros") : tr("Lock all macros");
    const float bw = textSize(c, fReg(), 11.5f, lab).x + 24;
    if (hit(c, "##lockall", x + 14, y + 168, bw, 28, &hov)) {
      std::vector<float> p = c.st.params;
      for (int i = 0; i < 8; ++i) setLocked(p, i, !all);
      setState(c, p, lab);
    }
    rect(c, x + 14, y + 168, bw, 28, hov ? theme::mix(L.panel2, L.text, 0.06f) : L.panel2, 4);
    rectLine(c, x + 14, y + 168, bw, 28, L.line, 4);
    textC(c, fReg(), 11.5f, x + 14 + bw * 0.5f, y + 174, L.text, lab);
  }
  {
    bool hov = false;
    if (hit(c, "##doroll", x + w - 60, y + 168, 46, 28, &hov)) {
      int bank = c.u.lastPreset >= 0 && static_cast<size_t>(c.u.lastPreset) < presetMeta().size() ? presetMeta()[static_cast<size_t>(c.u.lastPreset)].bank : -1;
      const std::vector<float> before = c.st.params;
      const auto r = roll(before, c.u.diceScopes, c.u.diceAmount, c.u.rollSeed++ * 2654435761u + static_cast<uint32_t>(c.now * 1000), bank);
      if (c.u.history.empty()) c.u.history.push_back(before), c.u.historyAt = 0;
      c.u.history.resize(static_cast<size_t>(c.u.historyAt + 1));
      c.u.history.push_back(r);
      if (c.u.history.size() > 17) c.u.history.erase(c.u.history.begin());
      c.u.historyAt = static_cast<int>(c.u.history.size()) - 1;
      c.u.rolledMask = 0;
      for (int i = 0; i < 8; ++i)
        if (r[static_cast<size_t>(i)] != before[static_cast<size_t>(i)]) c.u.rolledMask |= 1u << i;
      c.u.rolledAt = c.now;
      setState(c, r, tr("Roll the dice"));
    }
    rect(c, x + w - 60, y + 168, 46, 28, hov ? theme::mix(L.accent, L.white, 0.12f) : L.accent, 4);
    textC(c, fBold(), 11.5f, x + w - 37, y + 174, L.ink, tr("Roll"));
  }
}

}  // namespace ui

// ------------------------------------------------------------------ entry

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args) {
  (void)a;
  if (cmd != "chater") return false;
  const std::string op = args.empty() ? "" : args[0];
  if (op == "click" && args.size() >= 3) ui::gDev.stage = 1, ui::gDev.fx = std::strtof(args[1].c_str(), nullptr), ui::gDev.fy = std::strtof(args[2].c_str(), nullptr);
  else if (op == "menu") ui::gDev.menu = true;
  else ui::gDev.report = true;
  return true;
}

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  PluginState st = *sp;
  // a state saved before version 2: rewrite it once into the new layout (the engine already
  // plays it that way), so the knobs show what plays
  if (needsMigration(st.params.data(), st.params.size())) {
    migrateV1(st.params.data(), st.params.size());
    setPluginState(a, t, st, i18n::tr("Update the saved Chater sound"));
  }
  using namespace ui;
  Ui& u = uis()[t.key()];
  const Look& L = look();
  skin::Form F = skin::Form::fit(width, kW);
  Ctx c{a, t, F, ImGui::GetWindowDrawList(), st, u, L, {}, false, ImGui::GetTime()};
  if (!t.isChannel())
    if (auto rt = a.builder.effectRuntime(t.owner, t.slot); rt && rt->fx) c.hasLive = liveView(rt->fx.get(), c.live);
  // Visuals: Calm keeps the tape and meters but stills the grain sparkle, Still draws nothing live
  if (prefs(a).visuals >= 1 && c.hasLive)
    for (auto& g : c.live.grains) g.alive = false;
  if (prefs(a).visuals == 2) c.hasLive = false;
  // the preset name: follow what the header knows (a browser drop names it there)
  if (u.devFrames == 0) {
    std::string type;
    bool onB = false;
    const std::string shown = pui::devHeaderPreset(t, &type, &onB);
    if (!shown.empty() && type == kType) u.presetName = shown;
  }
  // The window opens on the main page: a page, sheet, popover or menu left open when it closed
  // (or in an earlier session of this instance) never comes back with it.
  {
    const int fr = ImGui::GetFrameCount();
    if (fr - u.lastFrame > 2) {
      u.page = PgMain;
      u.settings = u.presets = u.dice = false;
      u.wantMenu = false, u.menuParam = -1, u.typeParam = -1;
      u.saving = false;
    }
    u.lastFrame = fr;
  }
  ImGui::PushID("gl.chater");
  devView(c);
  devClick(c);
  // Leaving any overlay or page (nothing may trap the user): Escape steps back one level
  // (sheet or popover, then the preset browser, then a page) whether the editor has keyboard
  // focus or just the pointer; the top bar's buttons always work and toggle; a click outside
  // the settings sheet or the dice popover closes it and still reaches what it lands on.
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  if ((ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || hovered) && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    if (u.settings || u.dice) u.settings = u.dice = false;
    else if (u.presets) u.presets = false;
    else u.page = PgMain;
  }
  if ((u.settings || u.dice) && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    auto in = [&](float x0, float y0, float x1, float y1) {
      const ImVec2 a = c.p(x0, y0), b = c.p(x1, y1);
      return m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y;
    };
    if (!in(0, 0, kW, 46)) {  // the top bar handles its own clicks
      if (u.settings && !in(690, 46, kW, kH)) u.settings = false;
      if (u.dice && !in(364, 52, 694, 260)) u.dice = false;
    }
  }
  rect(c, 0, 0, kW, kH, L.win, 0);
  const bool modal = u.settings || u.dice;
  gBlock = false;
  topBar(c);
  gBlock = modal && (u.settings || u.dice);
  if (u.presets) {
    drawPresets(c, 58);
  } else {
    const bool compact = u.page != PgMain;
    tape(c, compact);
    tiles(c, compact);
    if (compact) drawDrawer(c, 314);
    else quick(c);
  }
  gBlock = false;
  if (u.dice) drawDice(c);
  if (u.settings) drawSettings(c);
  devReport(c);
  knobMenu(c);
  F.end(kW, kH);
  ImGui::PopID();
  return true;
}

}  // namespace gl::packs::chater
