// Gridline — "plasma" pack: the FX page (y 68..388). Left: rack tabs (MAIN /
// BUS 1 / BUS 2), "+ FX", the module list (drag to reorder). Right: the
// module stack of the rack; each module is a strip with its name, power,
// a display where it helps (distortion transfer, filter response) and its
// controls. Splitters host modules per band.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "../../core/I18n.h"
#include "plasma_dsp.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {

constexpr float kListX = 0, kListW = 265, kStackX = 270, kTop = 69;
constexpr float kStackW = kW - kStackX - 6;
constexpr float kMixW = 66;  // the MIX + meter column at the right of a unit
// The parameter shown in a unit's MIX column (-1: none).
int mixIndex(int type) {
  const auto& defs = fxParams(type);
  for (int q = 0; q < static_cast<int>(defs.size()); ++q)
    if (defs[static_cast<size_t>(q)].kind == 0 && std::strcmp(defs[static_cast<size_t>(q)].key, "kParamWet") == 0) return q;
  return -1;
}

int bandsOf(int type) { return type == FxSplitLMH ? 3 : (type == FxSplitLH || type == FxSplitMS) ? 2 : 0; }
const char* bandName(int type, int b) {
  if (type == FxSplitMS) return b == 0 ? "MID" : "SIDE";
  if (type == FxSplitLH) return b == 0 ? "LOW" : "HIGH";
  return b == 0 ? "LOW" : b == 1 ? "MID" : "HIGH";
}
std::string upper(std::string s) {
  for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

// ------------------------------------------------------------------ rack edits
int freeSlot(const Ctx& c) {
  for (int s = 0; s < kFxSlots; ++s)
    if (vali(c, fxP(s, X_Type)) == FxNone) return s;
  return -1;
}
void addFx(Ctx& c, int type, int rack, int parent, int band, int beforeIndex = -1) {
  const int s = freeSlot(c);
  if (s < 0) return;
  // the current layout first (slots without a node are appended by resolvedFx)
  c.m.fx = resolvedFx(c.m, c.st.params);
  FxNode n{s, rack, parent, band};
  if (beforeIndex >= 0 && beforeIndex <= static_cast<int>(c.m.fx.size())) c.m.fx.insert(c.m.fx.begin() + beforeIndex, n);
  else c.m.fx.push_back(n);
  c.m.fxExtra[static_cast<size_t>(s)] = FxExtra{};
  commit(c, "Add effect");
  setv(c, fxP(s, X_Type), type);
  setv(c, fxP(s, X_On), 1);
  const auto& defs = fxParams(type);
  for (int q = 0; q < kFxParams; ++q) setv(c, fxP(s, X_P0 + q), q < static_cast<int>(defs.size()) ? fxDefaultNorm(type, q) : 0.0);
  c.u.fxSel = s;
}
void removeFx(Ctx& c, int slot) {
  auto nodes = resolvedFx(c.m, c.st.params);
  std::vector<int> gone = {slot};
  for (const auto& n : nodes)
    if (n.parent == slot) gone.push_back(n.slot);
  nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const FxNode& n) { return std::find(gone.begin(), gone.end(), n.slot) != gone.end(); }),
              nodes.end());
  c.m.fx = nodes;
  for (int s : gone) c.m.fxExtra[static_cast<size_t>(s)] = FxExtra{};
  commit(c, "Remove effect");
  for (int s : gone) setv(c, fxP(s, X_Type), FxNone);
  if (c.u.fxSel == slot) c.u.fxSel = -1;
}
// Moves node `slot` to sit before node `before` (-1: to the end of its group).
void moveFx(Ctx& c, int slot, int before) {
  auto nodes = resolvedFx(c.m, c.st.params);
  auto it = std::find_if(nodes.begin(), nodes.end(), [&](const FxNode& n) { return n.slot == slot; });
  if (it == nodes.end() || slot == before) return;
  FxNode n = *it;
  // a splitter moves with its band modules
  std::vector<FxNode> moving = {n};
  nodes.erase(it);
  if (n.parent < 0) {
    for (auto j = nodes.begin(); j != nodes.end();)
      if (j->parent == slot) moving.push_back(*j), j = nodes.erase(j);
      else ++j;
  }
  auto pos = before < 0 ? nodes.end() : std::find_if(nodes.begin(), nodes.end(), [&](const FxNode& q) { return q.slot == before; });
  if (pos != nodes.end()) {
    // take the target's place (rack / parent / band follow the target)
    moving[0].rack = pos->rack, moving[0].parent = pos->parent, moving[0].band = pos->band;
    for (size_t i = 1; i < moving.size(); ++i) moving[i].rack = pos->rack;
  }
  nodes.insert(pos, moving.begin(), moving.end());
  c.m.fx = nodes;
  commit(c, "Move effect");
}

void addMenu(Ctx& c, int rack, int parent, int band) {
  const auto& names = fxTypeNames();
  const bool full = freeSlot(c) < 0;
  if (full) ImGui::TextDisabled("%s", i18n::tr("All 24 effect slots are in use"));
  for (int t = 1; t < FxTypeCount; ++t) {
    if (parent >= 0 && bandsOf(t)) continue;  // no splitter inside a splitter
    if (t == FxSplitLH) ImGui::Separator();
    if (ImGui::MenuItem(names[static_cast<size_t>(t)].c_str(), nullptr, false, !full)) addFx(c, t, rack, parent, band);
  }
}

// ------------------------------------------------------------------ module displays
void distDisplay(Ctx& c, int s, float x, float y, float w, float h) {
  displayWellPublic(c, x, y, w, h);
  const int mi = fxParamIndex(FxDistortion, "kParamMode"), di = fxParamIndex(FxDistortion, "kParamDrive");
  const int mode = static_cast<int>(std::lround(fxPlain(FxDistortion, mi, val(c, fxP(s, X_P0 + mi)))));
  const float drive = static_cast<float>(val(c, fxP(s, X_P0 + di)));
  FxExtra& ex = c.m.fxExtra[static_cast<size_t>(s)];
  if (mode == 11 || mode == 12) {
    // X-Shaper: editable transfer curve(s)
    Curve& cv = ex.shapeA;
    Curve edit = cv.empty() ? linearCurve() : cv;
    static int sel = -1;
    if (curveEditor(c, "xs", edit, x + 2, y + 2, w - 4, h - 4, 8, 8, sel, false, c.P.warn)) {
      cv = edit;
      commit(c, "Edit X-Shaper", 6000 + s);
    }
    return;
  }
  float ca[257], cb[257];
  for (int i = 0; i < 257; ++i) ca[i] = cb[i] = i / 256.f;
  float st = 0;
  std::vector<ImVec2> pts;
  for (int i = 0; i <= 48; ++i) {
    const float xin = -1.f + 2.f * i / 48.f;
    const float yo = std::clamp(shapeSample(mode, xin, drive, 0.f, ca, cb, st), -1.2f, 1.2f);
    pts.push_back(c.p(x + w * i / 48.f, y + h * 0.5f - yo * h * 0.42f));
  }
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  skin::curveArea(c.dl, pts.data(), static_cast<int>(pts.size()), c.p(0, y + h * 0.5f).y, c.P.warn, std::max(1.5f, c.s(1.6f)), 0.25f);
  c.dl->PopClipRect();
}
void filterDisplay(Ctx& c, int s, float x, float y, float w, float h) {
  displayWellPublic(c, x, y, w, h);
  auto P = [&](const char* key) {
    const int i = fxParamIndex(FxFilter, key);
    return i < 0 ? 0.0 : fxPlain(FxFilter, i, val(c, fxP(s, X_P0 + i)));
  };
  constexpr int N = 64;
  float hz[N], mag[N];
  for (int i = 0; i < N; ++i) hz[i] = 20.f * std::pow(1000.f, static_cast<float>(i) / (N - 1));
  const int ti = fxParamIndex(FxFilter, "kParamType"), fi = fxParamIndex(FxFilter, "kParamFreq");
  Filter::responseCurve(static_cast<int>(std::lround(P("kParamType"))), 48000, cutoffHz(val(c, fxP(s, X_P0 + fi))), P("kParamReso") / 100.0,
                        P("kParamVar") / 100.0, hz, mag, N);
  (void)ti;
  std::vector<ImVec2> pts(N);
  for (int i = 0; i < N; ++i) {
    const float db = mag[i] > 1e-6f ? 20.f * std::log10(mag[i]) : -60.f;
    pts[static_cast<size_t>(i)] = c.p(x + w * i / (N - 1), y + h - std::clamp((db + 36.f) / 60.f, 0.f, 1.f) * h);
  }
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  skin::curveArea(c.dl, pts.data(), N, c.p(0, y + h).y, c.P.warn, std::max(1.5f, c.s(1.6f)), 0.2f);
  c.dl->PopClipRect();
}

// ------------------------------------------------------------------ one module strip
float moduleHeight(int type) {
  const auto& defs = fxParams(type);
  int knobs = 0, small = 0;
  for (const auto& d : defs) (d.kind == 0 ? knobs : small)++;
  if (mixIndex(type) >= 0) --knobs;
  const float area = kStackW - 150 - (small ? 118 : 0) - ((type == FxDistortion || type == FxFilter) ? 124 : 0) - 40 - kMixW;
  const int perRow = std::max(1, static_cast<int>(area / 54));
  const int knobRows = (knobs + perRow - 1) / perRow;
  const int smallRows = (small + 1) / 1;
  return std::max({76.f, knobRows * 52.f + 16, smallRows * 21.f + 14});
}

void moduleStrip(Ctx& c, const FxNode& n, float x, float y, float w, int bandLabelType) {
  const int s = n.slot, type = vali(c, fxP(s, X_Type));
  const float h = moduleHeight(type);
  const bool on = val(c, fxP(s, X_On)) >= 0.5;
  ImGui::PushID(s + 6100);
  const bool selected = c.u.fxSel == s;
  // a rack unit: name block | display / choices | knobs | MIX + meter
  frameBox(c, x - kGutter, y - kGutter, w + 2 * kGutter, h + 2 * kGutter, c.P.panelHi);
  if (selected) c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), theme::withAlpha(c.P.accent, 0.6f), c.s(3.5f), 0, std::max(1.f, c.s(1.2f)));
  // name block
  {
    const float bw = 140;
    c.dl->AddRectFilled(c.p(x + 1, y + 1), c.p(x + bw, y + h - 1), c.P.panelLo, c.s(3), ImDrawFlags_RoundCornersLeft);
    vline(c, x + bw, y + 1, y + h - 1, c.P.frame);
    led(c, fxP(s, X_On), x + 8, y + 9, 10);
    const std::string nm = upper(fxTypeNames()[static_cast<size_t>(type)]);
    text(c, x + 24, y + 8, nm.c_str(), on ? c.P.warn : c.P.textFaint, 10.0f, true);
    if (bandLabelType >= 0) text(c, x + 24, y + 22, bandName(bandLabelType, n.band), c.P.textDim, 8, true);
    // up / down / remove
    const char* tips[3] = {"Move up", "Move down", "Remove"};
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i + 10);
      const float bx = x + 8 + i * 24.f, by = y + h - 24;
      ImGui::SetCursorScreenPos(c.p(bx, by));
      const bool click = ImGui::InvisibleButton("##mv", c.F.sz(20, 18));
      const bool hv = ImGui::IsItemHovered();
      c.dl->AddRectFilled(c.p(bx, by), c.p(bx + 20, by + 18), hv ? c.P.headOn : c.P.head, c.s(2));
      if (i < 2) iconArrow(c.dl, c.p(bx + 10, by + 9), c.s(8), i == 0 ? 2 : 3, hv ? c.P.text : c.P.textDim);
      else iconClose(c.dl, c.p(bx + 10, by + 9), c.s(10), hv ? c.P.danger : c.P.textDim);
      if (hv) c.a.setHint(tips[i]);
      if (click) {
        auto nodes = resolvedFx(c.m, c.st.params);
        if (i == 2) {
          removeFx(c, s);
        } else {
          // neighbour in the same group
          std::vector<int> group;
          for (const auto& q : nodes)
            if (q.rack == n.rack && q.parent == n.parent && (n.parent < 0 || q.band == n.band)) group.push_back(q.slot);
          const auto it = std::find(group.begin(), group.end(), s);
          const long idx = it - group.begin();
          if (i == 0 && idx > 0) moveFx(c, s, group[static_cast<size_t>(idx - 1)]);
          if (i == 1 && idx + 1 < static_cast<long>(group.size())) moveFx(c, group[static_cast<size_t>(idx + 1)], s);
        }
      }
      ImGui::PopID();
    }
  }
  // MIX column with the output meter
  const int mixQ = mixIndex(type);
  {
    const float mx = x + w - kMixW;
    vline(c, mx, y + 6, y + h - 6, c.P.line);
    if (mixQ >= 0) {
      ImGui::PushID(mixQ + 200);
      knob(c, fxP(s, X_P0 + mixQ), mx + 28, y + 26, 14.0f, "MIX");
      ImGui::PopID();
    }
    const float my0 = y + 8, my1 = y + h - 8, mxx = x + w - 13;
    c.dl->AddRectFilled(c.p(mxx, my0), c.p(mxx + 5, my1), c.P.well, c.s(1));
    c.dl->AddRect(c.p(mxx, my0), c.p(mxx + 5, my1), c.P.line, c.s(1));
    if (c.hasLive) {
      const float lv = std::clamp(c.live.fxMeter[s], 0.f, 1.f);
      c.dl->AddRectFilled(c.p(mxx + 1, my1 - lv * (my1 - my0)), c.p(mxx + 4, my1 - 1), c.P.good, 1);
    }
  }
  float cx = x + 150;
  // display
  if (type == FxDistortion || type == FxFilter) {
    if (type == FxDistortion) distDisplay(c, s, cx, y + 8, 116, h - 16);
    else filterDisplay(c, s, cx, y + 8, 116, h - 16);
    cx += 124;
  }
  // choices / toggles column
  const auto& defs = fxParams(type);
  int smallCount = 0;
  for (const auto& d : defs) smallCount += d.kind != 0;
  if (smallCount) {
    insetFrame(c, cx - 4, y + 4, 118, h - 8, 0, 0, false);  // the unit's display: mode and switches
    float sy = y + 8;
    for (int q = 0; q < static_cast<int>(defs.size()); ++q) {
      const auto& d = defs[static_cast<size_t>(q)];
      if (d.kind == 0) continue;
      const int k = fxP(s, X_P0 + q);
      ImGui::PushID(q + 100);
      if (d.kind == 1) {
        toggleBtn(c, k, cx, sy, 110, 18, d.label, 7.5f);
      } else {
        // choice: the stored value is normalized; show the choice names
        const int cnt = static_cast<int>(d.choices.size());
        const int cur = std::clamp(static_cast<int>(std::lround(fxPlain(type, q, val(c, k)))), 0, std::max(0, cnt - 1));
        int step = 0;
        const std::string shown = cnt ? d.choices[static_cast<size_t>(cur)] : ptext(c, k);
        dropField(c, "ch", cx, sy, 110, 18, shown.c_str(),
                  [&] {
                    if (type == FxConvolve && std::strcmp(d.key, "kParamIr") == 0) {
                      if (ImGui::MenuItem(i18n::tr("Load impulse response..."))) {
                        const int slot = s;
                        openFile(c, i18n::tr("Load impulse response"), {".wav", ".flac", ".aif", ".aiff"},
                                 [slot, type, q](App& a, const PluginTarget& t, const std::string& p) {
                                   editState(a, t, "Load impulse", [&](PluginState& st, Model& m) {
                                     m.fxExtra[static_cast<size_t>(slot)].ir = p;
                                     st.params[static_cast<size_t>(fxP(slot, X_P0 + q))] = static_cast<float>(fxNorm(type, q, 0));
                                   });
                                 });
                      }
                      ImGui::Separator();
                    }
                    for (int i = 0; i < cnt; ++i)
                      if (ImGui::Selectable(d.choices[static_cast<size_t>(i)].c_str(), i == cur)) setv(c, k, fxNorm(type, q, i));
                  },
                  true, &step);
        if (step && cnt) setv(c, k, fxNorm(type, q, std::clamp(cur + step, 0, cnt - 1)));
      }
      ImGui::PopID();
      sy += 21;
    }
    if (type == FxConvolve) {
      const std::string& ir = c.m.fxExtra[static_cast<size_t>(s)].ir;
      if (!ir.empty()) text(c, cx, sy + 2, fs_stem(ir).c_str(), c.P.textDim, 7.5f, false);
    }
    cx += 118;
  }
  // knobs grid
  {
    const float area = x + w - kMixW - cx - 8;
    const int perRow = std::max(1, static_cast<int>(area / 54));
    int i = 0;
    for (int q = 0; q < static_cast<int>(defs.size()); ++q) {
      const auto& d = defs[static_cast<size_t>(q)];
      if (d.kind != 0 || q == mixQ) continue;
      const int row = i / perRow, col = i % perRow;
      ImGui::PushID(q + 200);
      knob(c, fxP(s, X_P0 + q), cx + 27 + col * 54.f, y + 26 + row * 52.f, 14.0f, d.label, (d.min < 0 && d.max > 0) ? KnobBipolar : 0);
      ImGui::PopID();
      ++i;
    }
  }
  if (!on) c.dl->AddRectFilled(c.p(x + 141, y + 1), c.p(x + w - 1, y + h - 1), theme::withAlpha(c.P.frame, 0.4f), c.s(3), ImDrawFlags_RoundCornersRight);
  // click on the strip selects it
  if (ImGui::IsMouseHoveringRect(c.p(x, y), c.p(x + w, y + h)) && ImGui::IsMouseClicked(0)) c.u.fxSel = s;
  ImGui::PopID();
}

// ------------------------------------------------------------------ list (left)
void listRow(Ctx& c, const FxNode& n, float y, int depth, int bandType) {
  const int s = n.slot, type = vali(c, fxP(s, X_Type));
  ImGui::PushID(s + 6400);
  const float x = kListX + 6 + depth * 14.f, w = kListW - 12 - depth * 14.f;
  ImGui::SetCursorScreenPos(c.p(x, y));
  if (ImGui::InvisibleButton("##row", c.F.sz(w - 48, 20))) c.u.fxSel = s;
  const bool hv = ImGui::IsItemHovered(), sel = c.u.fxSel == s;
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("PLASMA_FX", &s, sizeof(int));
    ImGui::TextUnformatted(fxTypeNames()[static_cast<size_t>(type)].c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_FX")) {
      int from = 0;
      std::memcpy(&from, p->Data, sizeof(int));
      if (from != s) moveFx(c, from, s);
    }
    ImGui::EndDragDropTarget();
  }
  if (sel || hv) panelRect(c, x, y, w, 20, sel ? c.P.accentSoft : c.P.panelHi, 2);
  const bool on = val(c, fxP(s, X_On)) >= 0.5;
  c.dl->AddRectFilled(c.p(x + 4, y + 5), c.p(x + 6, y + 15), on ? c.P.warn : c.P.textFaint);
  std::string nm = upper(fxTypeNames()[static_cast<size_t>(type)]);
  if (bandType >= 0) nm = std::string(bandName(bandType, n.band)) + " \xC2\xB7 " + nm;
  text(c, x + 12, y + 5, nm.c_str(), on ? c.P.text : c.P.textFaint, 8.5f, true);
  if (hv) c.a.setHint("Drag to reorder \xC2\xB7 click to select");
  // bypass + remove
  ImGui::SetCursorScreenPos(c.p(x + w - 44, y + 1));
  if (ImGui::InvisibleButton("##on", c.F.sz(20, 18))) setv(c, fxP(s, X_On), on ? 0 : 1);
  iconPower(c.dl, c.p(x + w - 34, y + 11), c.s(4.5f), ImGui::IsItemHovered() ? c.P.text : (on ? c.P.accent : c.P.textFaint));
  ImGui::SetCursorScreenPos(c.p(x + w - 22, y + 1));
  if (ImGui::InvisibleButton("##rm", c.F.sz(20, 18))) removeFx(c, s);
  iconClose(c.dl, c.p(x + w - 12, y + 10), c.s(9), ImGui::IsItemHovered() ? c.P.danger : c.P.textFaint);
  ImGui::PopID();
}

}  // namespace

void fxPage(Ctx& c) {
  const int rack = std::clamp(c.u.fxRack, 0, 2);
  panelRect(c, 0, kPageY, kW, kLowerY - kPageY, c.P.frame);
  // ---------------- left column (framed) and the rack (framed, with rails)
  frameBox(c, kListX, kPageY, kListW + 3, kLowerY - kPageY);
  frameBox(c, kListW + 3, kPageY, kW - kListW - 3, kLowerY - kPageY, c.P.panelLo);
  hline(c, kListX + 3, kListW, kTop + 58.5f, c.P.line);
  {
    const char* rn[3] = {"MAIN", "BUS 1", "BUS 2"};
    for (int r = 0; r < 3; ++r) {
      ImGui::PushID(r + 6600);
      const float tx = kListX + 6 + r * 86.f;
      if (button(c, "rk", tx, kTop + 4, 83, 22, rn[r], r == rack, 9)) c.u.fxRack = r, c.u.fxSel = -1;
      // drop a module on a rack tab: move it to the end of that rack
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PLASMA_FX")) {
          int from = 0;
          std::memcpy(&from, p->Data, sizeof(int));
          auto nodes = resolvedFx(c.m, c.st.params);
          for (auto& q : nodes)
            if (q.slot == from || q.parent == from) q.rack = r, q.parent = q.slot == from ? -1 : q.parent;
          c.m.fx = nodes;
          commit(c, "Move effect to rack");
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::PopID();
    }
  }
  if (button(c, "add", kListX + 6, kTop + 31, 60, 22, "+ FX", false, 9.5f)) ImGui::OpenPopup("##addfx");
  if (ImGui::BeginPopup("##addfx")) {
    addMenu(c, rack, -1, 0);
    ImGui::EndPopup();
  }
  dropField(c, "rackmenu", kListX + 70, kTop + 31, kListW - 76, 22, c.m.lockFx ? "Effects locked" : "Rack", [&] {
    static std::vector<std::pair<FxNode, std::vector<float>>> clip;
    static std::vector<FxExtra> clipExtra;
    if (ImGui::MenuItem(i18n::tr("Copy this rack"))) {
      clip.clear();
      clipExtra.clear();
      for (const auto& n : resolvedFx(c.m, c.st.params))
        if (n.rack == rack) {
          std::vector<float> p;
          for (int q = 0; q < X_Count; ++q) p.push_back(valf(c, fxP(n.slot, q)));
          clip.push_back({n, p});
          clipExtra.push_back(c.m.fxExtra[static_cast<size_t>(n.slot)]);
        }
    }
    if (ImGui::MenuItem(i18n::tr("Paste (adds to this rack)"), nullptr, false, !clip.empty())) {
      std::map<int, int> remap;
      auto nodes = resolvedFx(c.m, c.st.params);
      std::vector<std::pair<int, std::vector<float>>> writes;
      std::vector<bool> taken(kFxSlots, false);
      for (const auto& n : nodes) taken[static_cast<size_t>(n.slot)] = true;
      for (size_t i = 0; i < clip.size(); ++i) {
        int s = -1;
        for (int q = 0; q < kFxSlots; ++q)
          if (!taken[static_cast<size_t>(q)] && vali(c, fxP(q, X_Type)) == FxNone) {
            s = q;
            break;
          }
        if (s < 0) break;
        taken[static_cast<size_t>(s)] = true;
        remap[clip[i].first.slot] = s;
        FxNode n = clip[i].first;
        n.slot = s, n.rack = rack;
        if (n.parent >= 0) n.parent = remap.count(n.parent) ? remap[n.parent] : -1;
        nodes.push_back(n);
        c.m.fxExtra[static_cast<size_t>(s)] = clipExtra[i];
        writes.push_back({s, clip[i].second});
      }
      c.m.fx = nodes;
      commit(c, "Paste effects");
      for (const auto& [s, p] : writes)
        for (int q = 0; q < X_Count; ++q) setv(c, fxP(s, q), p[static_cast<size_t>(q)]);
    }
    if (ImGui::MenuItem(i18n::tr("Clear this rack"))) {
      std::vector<int> gone;
      for (const auto& n : resolvedFx(c.m, c.st.params))
        if (n.rack == rack) gone.push_back(n.slot);
      for (int s : gone) removeFx(c, s);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Lock effects (keep them when loading presets)"), nullptr, c.m.lockFx)) {
      c.m.lockFx = !c.m.lockFx;
      commit(c, "Lock effects");
    }
  });
  const auto nodes = resolvedFx(c.m, c.st.params);
  {
    float y = kTop + 64;
    const float y1 = kLowerY - 6;
    c.dl->PushClipRect(c.p(kListX, y), c.p(kListW, y1), true);
    for (const auto& n : nodes) {
      if (n.rack != rack || n.parent >= 0) continue;
      if (y > y1) break;
      listRow(c, n, y, 0, -1);
      y += 22;
      const int t = vali(c, fxP(n.slot, X_Type));
      if (bandsOf(t))
        for (const auto& ch : nodes)
          if (ch.parent == n.slot && ch.rack == rack) {
            listRow(c, ch, y, 1, t);
            y += 22;
          }
    }
    c.dl->PopClipRect();
    if (y == kTop + 64) textC(c, kListW * 0.5f, y + 20, "No effects \xE2\x80\x94 click + FX", c.P.textFaint, 8.5f, true);
  }
  // ---------------- module stack
  {
    // rails
    for (int i = 0; i < 2; ++i) {
      const float rx = i == 0 ? kStackX + 1 : kW - 9;
      c.dl->AddRectFilled(c.p(rx, kTop + 3), c.p(rx + 5, kLowerY - 5), c.P.frame, c.s(1));
      for (float y = kTop + 7; y < kLowerY - 8; y += 8) c.dl->AddRectFilled(c.p(rx + 1, y), c.p(rx + 4, y + 4), c.P.panelHi, 1);
    }
    std::vector<std::pair<FxNode, int>> order;  // node + band label type (-1)
    for (const auto& n : nodes) {
      if (n.rack != rack || n.parent >= 0) continue;
      order.push_back({n, -1});
      const int t = vali(c, fxP(n.slot, X_Type));
      if (bandsOf(t))
        for (const auto& ch : nodes)
          if (ch.parent == n.slot && ch.rack == rack) order.push_back({ch, t});
    }
    float total = 0;
    for (const auto& [n, bt] : order) total += moduleHeight(vali(c, fxP(n.slot, X_Type))) + 6 + (bandsOf(vali(c, fxP(n.slot, X_Type))) ? 26 : 0);
    static std::map<uint64_t, float> scroll;
    float& sc = scroll[c.t.key() * 3 + static_cast<uint64_t>(rack)];
    const float top = kTop + 4, h = kLowerY - 4 - top;
    const ImVec2 a = c.p(kStackX + 8, top), b = c.p(kW - 10, top + h);
    if (ImGui::IsMouseHoveringRect(a, b) && ImGui::GetIO().MouseWheel != 0 && !ImGui::IsAnyItemActive()) sc -= ImGui::GetIO().MouseWheel * 40.f;
    // keep the selected module in view when picked from the list
    static int lastSel = -2;
    if (c.u.fxSel != lastSel) {
      float yy = 0;
      for (const auto& [n, bt] : order) {
        const int t = vali(c, fxP(n.slot, X_Type));
        if (n.slot == c.u.fxSel) {
          if (yy < sc || yy > sc + h - 60) sc = yy;
          break;
        }
        yy += moduleHeight(t) + 6 + (bandsOf(t) ? 26 : 0);
      }
      lastSel = c.u.fxSel;
    }
    sc = std::clamp(sc, 0.f, std::max(0.f, total - h));
    c.dl->PushClipRect(a, b, true);
    ImGui::PushClipRect(a, b, true);
    float y = top - sc;
    for (const auto& [n, bt] : order) {
      const int t = vali(c, fxP(n.slot, X_Type));
      const float mh = moduleHeight(t);
      const float indent = bt >= 0 ? 24.f : 0.f;
      if (y + mh > top && y < top + h) moduleStrip(c, n, kStackX + 11 + indent, y + kGutter, kStackW - 22 - indent, bt);
      y += mh + 6;
      if (bandsOf(t)) {
        // "+ FX" per band
        ImGui::PushID(n.slot + 6700);
        for (int bnd = 0; bnd < bandsOf(t); ++bnd) {
          ImGui::PushID(bnd);
          const std::string lab = std::string("+ ") + bandName(t, bnd);
          if (y + 20 > top && y < top + h && button(c, "addb", kStackX + 34 + bnd * 100.f, y, 94, 20, lab.c_str(), false, 8)) ImGui::OpenPopup("##addband");
          if (ImGui::BeginPopup("##addband")) {
            addMenu(c, rack, n.slot, bnd);
            ImGui::EndPopup();
          }
          ImGui::PopID();
        }
        ImGui::PopID();
        y += 26;
      }
    }
    ImGui::PopClipRect();
    c.dl->PopClipRect();
    if (order.empty()) textC(c, (kStackX + kW) * 0.5f, kTop + 140, "Add an effect with + FX", c.P.textFaint, 9.5f, true);
    if (total > h) {
      const float th = h * h / total, ty = top + (h - th) * (sc / std::max(1.f, total - h));
      c.dl->AddRectFilled(c.p(kW - 14, ty), c.p(kW - 11, ty + th), c.P.lineHi, c.s(1));
    }
  }
}

}  // namespace gl::packs::plasma::ui
