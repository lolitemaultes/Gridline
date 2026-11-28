// Gridline - "seltmann" pack: Seltmann's own screens in the Tracker layout (0.7.8, K-347). See
// seltmann_tracker.h. Drawn with the Tracker's pieces (bevel buttons, radio and check rows, steppers,
// the band's spacing scale, the monospaced face); the waveform and the sequence blocks are the
// Ribbon's own (their hit-testing, tools, cuts and flags), so a region edited here is edited as it
// is in the other layouts.
#include "../../app/UrlImportUi.h"
#include "seltmann_tracker.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <SDL.h>
#include "../../../third_party/imgui/imgui_internal.h"
#include "../../app/App.h"
#include "../../app/Keys.h"
#include "../../app/DevSignal.h"
#include "../../app/Theme.h"
#include "../../app/TrackerEditors.h"
#include "../../app/TrackerLook.h"
#include "../../app/TrackerPages.h"
#include "../../app/TrackerView.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/TrackerGrid.h"
#include "seltmann_ui.h"

namespace gl::packs::seltmann::tracker {

using theme::pal;
using namespace trackerview;
namespace fs = std::filesystem;

namespace {

// ---- view state (never saved) ----
enum Modal { ModalNone = 0, ModalCard, ModalValues, ModalSave };
struct View {
  bool stepScope = false;  // the values edit the selected step (else its region)
  int seqTop = 0;          // the sequence rows' first row in view
  int cursor = 0;          // the layout list's cursor (in the group shown)
  int listTop = 0;
  int soundTop = 0;        // the Sound box's first row in view
  int modal = ModalNone;
  int modalCard = 0;       // the card a ModalCard box shows
  bool modalAsk = false;   // open it this frame
  char saveName[64] = {};
  char search[48] = {};    // the layouts' search (names and hints)
  bool paramDrag = false;  // a parameter is being dragged (the edit ends on release)
  int pressKey = -1;       // a piano key held by the pointer
  ImGuiID boxId = 0;       // the open box's popup (closed from a key or when the screen goes)
};
std::map<ObjectId, View>& views() {
  static std::map<ObjectId, View> v;
  return v;
}
View& viewOf(ObjectId ch) { return views()[ch]; }

struct KeyEv {
  int sc = 0, kc = 0, mods = 0;
};
std::vector<KeyEv>& keyQueue() {
  static std::vector<KeyEv> q;
  return q;
}

// Controls drawn last frame (dev: "selt dump" lists them, "selt at" points at one).
struct Ctl {
  std::string name;
  ImVec2 p0, p1;
};
std::vector<Ctl> gCtls, gCtlsDrawn;
int gCtlFrame = -1;
std::string gScreen;  // the screen drawn last ("chop", "instrument", "page")
ObjectId gLastCh = kNoId;
ImVec2 gDevPt;
bool gDevPtSet = false;
std::string gRows;    // (dev) each part's rect, "name x0 y0 x1 y1; ..."

void noteCtl(const char* name) {
  const int f = ImGui::GetFrameCount();
  if (f != gCtlFrame) {
    gCtlsDrawn.swap(gCtls);
    gCtls.clear();
    gCtlFrame = f;
  }
  gCtls.push_back({name[0] == '#' && name[1] == '#' ? name + 2 : name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
}
void notePart(const char* name, ImVec2 p0, ImVec2 p1) {
  char b[96];
  std::snprintf(b, sizeof b, "%s%s %.0f %.0f %.0f %.0f", gRows.empty() ? "" : "; ", name, p0.x, p0.y, p1.x, p1.y);
  gRows += b;
}

ImFont* boldFont(App& a) { return a.fontBold ? a.fontBold : a.fontRegular; }
float textW(App& a, const char* english) { return a.fontRegular->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, i18n::tr(english)).x; }
float monoW(App& a, const char* s) { return monoFont(a)->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, s).x; }

std::string trkNote(App& a, int midi) { return trk::noteText(std::clamp(midi, 0, 127), a.settings.getBool("tracker.flats")); }

PluginTarget targetOf(ObjectId ch) { return PluginTarget{ch, -1}; }

int stepRegion(const Model& m, int i) {
  if (i < 0 || i >= static_cast<int>(m.pattern.steps.size())) return -1;
  const Step& s = m.pattern.steps[static_cast<size_t>(i)];
  return s.src == kRest ? -1 : stepSlice(m.pattern, s, static_cast<int>(m.slices.size()));
}

std::string fmt(const char* f, double v) {
  char b[48];
  std::snprintf(b, sizeof b, f, v);
  return b;
}
std::string pitchValue(float st) {
  if (std::fabs(st) < 0.005f) return "0 st";
  if (std::fabs(st - std::round(st)) < 0.005f) return fmt("%+.0f st", st);
  return fmt("%+.2f st", st);
}

// A gesture key for a value being dragged (one undo step per drag), 0 for a click or a key.
uint64_t dragKey(sui::Ed& e, int field, int item) {
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) return 0;
  return sui::gestureKey(0x5E1A000000000000ull + (static_cast<uint64_t>(field) << 40), e, item);
}

// ---- the model edits the screens make (each one undo step through Seltmann's model) ----
void regionField(App& a, const PluginTarget& t, sui::Ed& e, int field, double v, uint64_t mk) {
  if (e.m.slices.empty()) return;
  const int k = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
  Model nm = e.m;
  Slice& s = nm.slices[static_cast<size_t>(k)];
  switch (field) {
    case 0:
      if (!setSlicePitch(nm, k, static_cast<float>(v))) return;
      break;
    case 1: s.gain = static_cast<float>(std::clamp(v, -24.0, 12.0)); break;
    case 2: s.fadeIn = static_cast<float>(std::clamp(v, 0.0, 200.0)); break;
    case 3: s.fadeOut = static_cast<float>(std::clamp(v, 0.0, 400.0)); break;
    case 4: s.offset = static_cast<float>(std::clamp(v, 0.0, 0.9)); break;
    case 5: s.length = static_cast<float>(std::clamp(v, 0.05, 1.0)); break;
    case 6: s.choke = std::clamp(static_cast<int>(std::lround(v)), 0, 8); break;
    case 7: s.reverse = v > 0.5; break;
    default: return;
  }
  if (nm.slices == e.m.slices) return;
  sui::commit(a, t, e, nm, field == 0 ? i18n::N_("Region pitch") : i18n::N_("Region settings"), mk);
}

int curStep(const sui::Ed& e) {
  const int n = static_cast<int>(e.m.pattern.steps.size());
  if (e.selStep >= 0 && e.selStep < n) return e.selStep;
  return firstStepOf(e.m, e.selSlice);
}

void stepField(App& a, const PluginTarget& t, sui::Ed& e, int field, double v, uint64_t mk) {
  const int i = curStep(e);
  if (i < 0) return;
  Model nm = e.m;
  Step& s = nm.pattern.steps[static_cast<size_t>(i)];
  const int nSl = static_cast<int>(nm.slices.size());
  switch (field) {
    case 0:
      if (!setStepPitch(nm, i, static_cast<float>(v))) return;
      break;
    case 1: s.gain = static_cast<float>(std::clamp(v, -30.0, 6.0)); break;
    case 4: s.offset = static_cast<float>(std::clamp(v, 0.0, 0.9)); break;
    case 5: s.length = static_cast<float>(std::clamp(v, 0.05, 1.0)); break;
    case 8: s.stutter = std::clamp(static_cast<int>(std::lround(v)), 1, 16); break;
    case 7: s.reverse = v > 0.5; break;
    case 9: {  // plays: -1 rest, else a region
      const int r = static_cast<int>(std::lround(v));
      if (r < 0) s.src = kRest, s.slot = false;
      else s.src = std::clamp(r, 0, std::max(0, nSl - 1)), s.slot = false;
      break;
    }
    case 10: {  // its length in units (1 + ties); own lengths step by the unit
      const double unit = std::max(kMinStepBeats, nm.pattern.unitBeats);
      if (s.beats > 0) {
        if (!setStepBeats(nm, i, std::max(unit, v * unit))) return;
      } else {
        s.ties = std::clamp(static_cast<int>(std::lround(v)) - 1, 0, 63);
      }
      break;
    }
    default: return;
  }
  if (nm.pattern == e.m.pattern) return;
  sui::markEdited(nm);
  sui::commit(a, t, e, nm, i18n::N_("Edit a step"), mk);
}

void duplicateStep(App& a, const PluginTarget& t, sui::Ed& e) {
  const int i = curStep(e);
  if (i < 0 || e.m.pattern.steps.size() >= static_cast<size_t>(kMaxSteps)) return;
  Model nm = e.m;
  const Step copy = nm.pattern.steps[static_cast<size_t>(i)];
  nm.pattern.steps.insert(nm.pattern.steps.begin() + i + 1, copy);
  sui::markEdited(nm);
  sui::commit(a, t, e, nm, i18n::N_("Duplicate the step"));
  e.selStep = i + 1;
}
void deleteStep(App& a, const PluginTarget& t, sui::Ed& e) {
  const int i = curStep(e);
  if (i < 0 || e.m.pattern.steps.size() <= 1) return;
  Model nm = e.m;
  nm.pattern.steps.erase(nm.pattern.steps.begin() + i);
  sui::markEdited(nm);
  sui::commit(a, t, e, nm, i18n::N_("Delete the step"));
  e.selStep = std::min(i, static_cast<int>(nm.pattern.steps.size()) - 1);
}
void moveStepBy(App& a, const PluginTarget& t, sui::Ed& e, int d) {
  const int i = curStep(e);
  const int n = static_cast<int>(e.m.pattern.steps.size());
  if (i < 0 || i + d < 0 || i + d >= n) return;
  Model nm = e.m;
  if (!moveStep(nm, i, d > 0 ? i + d + 1 : i + d)) return;
  sui::markEdited(nm);
  sui::commit(a, t, e, nm, i18n::N_("Move a step"));
  e.selStep = i + d;
}
void restStep(App& a, const PluginTarget& t, sui::Ed& e) {
  const int i = curStep(e);
  if (i < 0) return;
  const Step& s = e.m.pattern.steps[static_cast<size_t>(i)];
  stepField(a, t, e, 9, s.src == kRest ? std::max(0, e.selSlice) : -1, 0);
}
void resetOrder(App& a, const PluginTarget& t, sui::Ed& e) {
  if (isNaturalOrder(e.m)) return;
  Model nm = e.m;
  nm.preset.clear();
  naturalPattern(nm);
  sui::commit(a, t, e, nm, i18n::N_("Reset the order"));
  e.selStep = -1;
}
void reverseScope(App& a, const PluginTarget& t, sui::Ed& e, bool step) {
  if (step) {
    const int i = curStep(e);
    if (i >= 0) stepField(a, t, e, 7, e.m.pattern.steps[static_cast<size_t>(i)].reverse ? 0 : 1, 0);
  } else if (!e.m.slices.empty()) {
    const int k = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
    regionField(a, t, e, 7, e.m.slices[static_cast<size_t>(k)].reverse ? 0 : 1, 0);
  }
}
void gridEdit(App& a, const PluginTarget& t, sui::Ed& e, Model nm, const char* label, uint64_t mk = 0) {
  nm.grid.loopFit = false;
  rechopKeepPattern(nm, sui::sampleOf(e).get());
  sui::commit(a, t, e, nm, label, mk);
}
double bar1Frame(const Model& m) {
  return m.grid.beats.empty() ? 0.0 : m.grid.beats[static_cast<size_t>(std::clamp(m.grid.bar1, 0, static_cast<int>(m.grid.beats.size()) - 1))];
}
void setTempo(App& a, const PluginTarget& t, sui::Ed& e, double bpm) {
  if (!(bpm >= 30 && bpm <= 300) || e.m.grid.beats.empty()) return;
  Model nm = e.m;
  gridSetBpm(nm.grid, bpm, bar1Frame(nm));
  gridEdit(a, t, e, nm, i18n::N_("Set the tempo"));
}
void tapTempoNow(App& a, const PluginTarget& t, sui::Ed& e) {
  if (!e.taps.empty() && a.now - e.taps.back() > 2.0) e.taps.clear();
  e.taps.push_back(a.now);
  const double bpm = tapTempo(e.taps);
  if (bpm > 0 && e.taps.size() >= 4 && !e.m.grid.beats.empty()) {
    Model nm = e.m;
    gridSetBpm(nm.grid, bpm, bar1Frame(nm));
    gridEdit(a, t, e, nm, i18n::N_("Tap tempo"), 0x5E17A9ull);
  }
}
void bar1Here(App& a, const PluginTarget& t, sui::Ed& e) {
  if (e.m.slices.empty() || e.m.grid.beats.empty()) return;
  Model nm = e.m;
  gridMoveDownbeat(nm.grid, static_cast<double>(nm.slices[static_cast<size_t>(std::clamp(e.selSlice, 0, static_cast<int>(nm.slices.size()) - 1))].start));
  gridEdit(a, t, e, nm, i18n::N_("Move bar 1"));
}
void analyseAgain(sui::Ed& e) {
  if (e.path.empty()) return;
  forgetAnalysis(e.path);
  e.job = requestAnalysis(e.path);
  e.reanalyse = true;
}
// The key (0..23: C major .. B minor; -1 unknown) one step on.
void stepKey(App& a, const PluginTarget& t, sui::Ed& e, int d) {
  int k = e.m.grid.key < 0 ? -1 : e.m.grid.key + (e.m.grid.minor ? 12 : 0);
  k = k + d;
  if (k < -1) k = 23;
  if (k > 23) k = -1;
  Model nm = e.m;
  nm.grid.key = k < 0 ? -1 : k % 12;
  nm.grid.minor = k >= 12;
  sui::commit(a, t, e, nm, i18n::N_("Set the key"));
}
void setParam(App& a, const PluginTarget& t, View& v, int param, double value) {
  const bool drag = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f);
  pui::setParamValue(a, t, param, value, drag);
  if (drag) v.paramDrag = true;
}
const char* followName(int sync) { return sync == SyncStretch ? "Stretch" : sync == SyncResample ? "Resample" : "Off"; }

void hearRegion(App& a, const PluginTarget& t, sui::Ed& e, int k) {
  if (k < 0 || k >= static_cast<int>(e.m.slices.size())) return;
  sui::auditionStart(a, t, e, k);
}
void selectRegion(App& a, const PluginTarget& t, sui::Ed& e, View& v, int k, bool hear) {
  const int n = static_cast<int>(e.m.slices.size());
  if (n == 0) return;
  e.selSlice = std::clamp(k, 0, n - 1);
  e.selStep = -1;
  v.stepScope = false;
  if (hear) hearRegion(a, t, e, e.selSlice);
}
void selectStep(App& a, const PluginTarget& t, sui::Ed& e, View& v, int i, bool hear) {
  const int n = static_cast<int>(e.m.pattern.steps.size());
  if (n == 0) return;
  e.selStep = std::clamp(i, 0, n - 1);
  const int r = stepRegion(e.m, e.selStep);
  if (r >= 0) e.selSlice = r;
  v.stepScope = true;
  if (hear && r >= 0) hearRegion(a, t, e, r);
}
// A song (or a long file): the whole-song strip has something to pick (as the editor's rule).
bool isSong(const sui::Ed& e) { return e.hasModel && (e.m.grid.kind == KindSong || barCount(e.m) > 16); }

void openModalCard(View& v, int card) {
  v.modal = ModalCard;
  v.modalCard = card;
  v.modalAsk = true;
}

// ---- the button panel: columns of cells, folded by the room ----
enum CellKind { CButton, CRadio, CCheck, CStepper, CDrag, CCaption, CRow };
struct Cell {
  CellKind kind = CButton;
  const char* id = "";
  const char* label = "";  // English (translated where drawn)
  const char* hint = nullptr;
  bool on = false, enabled = true;
  std::string value;       // a stepper's value
  float labelW = -1;
  std::function<void()> act;           // a click
  std::function<void(int)> step;       // a stepper's step
  std::function<void(double)> typed;   // a stepper's typed value
  std::function<void()> drag;          // a drag source on the last item
  std::vector<Cell> parts;             // CRow: cells side by side
};
struct Column {
  int prio = 0;  // 0 never folds away; higher numbers fold first
  std::vector<Cell> cells;
  float least = 0, units = 1, x = 0, w = 0;
};

Cell sButton(const char* id, const char* label, const char* hint, std::function<void()> act, bool on = false, bool enabled = true) {
  Cell c;
  c.kind = CButton, c.id = id, c.label = label, c.hint = hint, c.act = std::move(act), c.on = on, c.enabled = enabled;
  return c;
}
Cell sRadio(const char* id, const char* label, const char* hint, bool on, std::function<void()> act) {
  Cell c = sButton(id, label, hint, std::move(act), on);
  c.kind = CRadio;
  return c;
}
Cell sCheck(const char* id, const char* label, const char* hint, bool on, std::function<void()> act) {
  Cell c = sButton(id, label, hint, std::move(act), on);
  c.kind = CCheck;
  return c;
}
Cell sStepper(const char* id, const char* label, std::string value, const char* hint, std::function<void(int)> step, std::function<void(double)> typed = nullptr,
                 bool enabled = true) {
  Cell c;
  c.kind = CStepper, c.id = id, c.label = label, c.hint = hint, c.value = std::move(value), c.step = std::move(step), c.typed = std::move(typed), c.enabled = enabled;
  return c;
}
Cell sDrag(const char* id, const char* label, const char* hint, std::function<void()> drag, bool enabled = true) {
  Cell c;
  c.kind = CDrag, c.id = id, c.label = label, c.hint = hint, c.drag = std::move(drag), c.enabled = enabled;
  return c;
}
Cell sCaption(const char* label) {
  Cell c;
  c.kind = CCaption, c.label = label;
  return c;
}
Cell row(std::vector<Cell> parts) {
  Cell c;
  c.kind = CRow;
  c.parts = std::move(parts);
  return c;
}

float cellLeast(App& a, const Cell& c, float rowH) {
  const float fs = ImGui::GetFontSize();
  const float radioPad = 2 * std::max(3.0f, std::round(fs * 0.3f)) + 12 + space::kPad;
  switch (c.kind) {
    case CButton:
    case CDrag: return std::ceil(textW(a, c.label) + 2 * space::kPad + 2);
    case CRadio:
    case CCheck: return std::ceil(textW(a, c.label) + radioPad);
    case CCaption: return std::ceil(a.fontRegular->CalcTextSizeA(std::round(fs * 0.85f), FLT_MAX, 0, i18n::tr(c.label)).x + 4);
    case CStepper: {
      const float lw = c.labelW > 0 ? c.labelW : textW(a, c.label) + space::kGroup;
      return std::ceil(lw + std::max(monoW(a, c.value.c_str()), monoW(a, "000")) + 10 + 2 * std::round(rowH * 0.8f) + 2);
    }
    case CRow: {
      float w = static_cast<float>(std::max<size_t>(1, c.parts.size()) - 1) * space::kGap;
      for (const Cell& p : c.parts) w += cellLeast(a, p, rowH);
      return w;
    }
  }
  return 0;
}

void drawCell(App& a, Cell& c, ImVec2 p0, ImVec2 p1, float rowH) {
  const auto& P = pal();
  const float fs = ImGui::GetFontSize();
  if (!c.enabled) ImGui::BeginDisabled();
  switch (c.kind) {
    case CButton:
      if (bevelButton(a, c.id, c.label, p0, p1, c.on, c.hint) && c.enabled && c.act) c.act();
      noteCtl(c.id);
      break;
    case CDrag:
      bevelButton(a, c.id, c.label, p0, p1, false, c.hint);
      noteCtl(c.id);
      if (c.enabled && c.drag) {
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        c.drag();
      }
      break;
    case CRadio:
      if (radioRow(a, c.id, c.label, p0, p1, c.on, c.hint) && c.enabled && c.act) c.act();
      noteCtl(c.id);
      break;
    case CCheck:
      if (checkRow(a, c.id, c.label, p0, p1, c.on, c.hint) && c.enabled && c.act) c.act();
      noteCtl(c.id);
      break;
    case CCaption: {
      const float cs = std::round(fs * 0.85f);
      theme::textEllipsis(ImGui::GetWindowDrawList(), a.fontRegular, cs, ImVec2(p0.x + 2, std::round(p0.y + (p1.y - p0.y - cs) * 0.5f)), p1.x - p0.x - 4, P.textFaint,
                          i18n::tr(c.label));
      break;
    }
    case CStepper: {
      double typed = std::nan("");
      const float lw = c.labelW > 0 ? c.labelW : std::ceil(textW(a, c.label) + space::kGroup);
      const int d = stepper(a, c.id, c.label, c.value, p0, p1.x - p0.x, p1.y - p0.y, c.hint, lw, true, c.typed ? &typed : nullptr);
      noteCtl(c.id);
      if (c.enabled) {
        if (d && c.step) c.step(d);
        if (!std::isnan(typed) && c.typed) c.typed(typed);
      }
      break;
    }
    case CRow: {
      float least = 0;
      for (const Cell& q : c.parts) least += cellLeast(a, q, rowH);
      const float gaps = static_cast<float>(std::max<size_t>(1, c.parts.size()) - 1) * space::kGap;
      const float extra = std::max(0.0f, (p1.x - p0.x) - gaps - least);
      float x = p0.x;
      for (size_t i = 0; i < c.parts.size(); ++i) {
        Cell& q = c.parts[i];
        const float lq = cellLeast(a, q, rowH);
        const float w = i + 1 == c.parts.size() ? p1.x - x : std::floor(lq + extra * (lq / std::max(1.0f, least)));
        drawCell(a, q, ImVec2(x, p0.y), ImVec2(x + w, p1.y), rowH);
        x += w + space::kGap;
      }
      break;
    }
  }
  if (!c.enabled) ImGui::EndDisabled();
}

// A folded cell as a menu item (steppers and drags stay out: the Values box and the Loops box have them).
void menuCell(Cell& c) {
  switch (c.kind) {
    case CButton:
    case CRadio:
    case CCheck:
      if (ImGui::MenuItem(i18n::tr(c.label), nullptr, c.on, c.enabled) && c.act) c.act();
      if (c.hint && ImGui::IsItemHovered()) ui::Tooltip(i18n::tr(c.hint));
      break;
    case CRow:
      for (Cell& q : c.parts) menuCell(q);
      break;
    default: break;
  }
}

// Lays the columns out across [x0, x1] (rows of rowH from y0): the columns that do not fit fold
// into More... (the last column's last cell) by their priority. Draws them; the folded cells go to the menu.
void panel(App& a, std::vector<Column>& cols, float x0, float x1, float y0, int rows, float rowH, const char* menuId, std::vector<Cell> moreCells) {
  const float gapCol = 3;
  for (Column& c : cols) {
    c.least = 0;
    for (const Cell& cl : c.cells) c.least = std::max(c.least, cellLeast(a, cl, rowH));
  }
  std::vector<bool> on(cols.size(), true);
  for (size_t i = 0; i < cols.size(); ++i) on[i] = cols[i].prio < 99;  // (99: only in More...)
  auto total = [&] {
    float t = -gapCol;
    for (size_t i = 0; i < cols.size(); ++i)
      if (on[i]) t += cols[i].least + gapCol;
    return t;
  };
  // fold the highest priority numbers first (the later column first among equals)
  while (total() > x1 - x0) {
    int pick = -1;
    for (size_t i = 0; i < cols.size(); ++i)
      if (on[i] && cols[i].prio > 0 && (pick < 0 || cols[i].prio >= cols[static_cast<size_t>(pick)].prio)) pick = static_cast<int>(i);
    if (pick < 0) break;
    on[static_cast<size_t>(pick)] = false;
  }
  // the room left shared by the columns' units
  float units = 0;
  for (size_t i = 0; i < cols.size(); ++i)
    if (on[i]) units += cols[i].units;
  const float extra = std::max(0.0f, (x1 - x0) - total());
  float x = x0;
  int lastOn = -1;
  for (size_t i = 0; i < cols.size(); ++i)
    if (on[i]) lastOn = static_cast<int>(i);
  for (size_t i = 0; i < cols.size(); ++i) {
    if (!on[i]) continue;
    Column& c = cols[i];
    c.x = x;
    c.w = static_cast<int>(i) == lastOn ? x1 - x : std::floor(c.least + extra * c.units / std::max(0.01f, units));
    x += c.w + gapCol;
  }
  // folded cells (whole columns, and cells past the rows) into the menu
  std::vector<Cell*> menu;
  for (size_t i = 0; i < cols.size(); ++i)
    for (size_t r = 0; r < cols[i].cells.size(); ++r)
      if (!on[i] || static_cast<int>(r) >= rows) menu.push_back(&cols[i].cells[r]);
  for (size_t i = 0; i < cols.size(); ++i) {
    if (!on[i]) continue;
    Column& c = cols[i];
    for (int r = 0; r < rows && r < static_cast<int>(c.cells.size()); ++r) {
      const float y = y0 + static_cast<float>(r) * (rowH + space::kGap);
      Cell& cl = c.cells[static_cast<size_t>(r)];
      const bool more = std::strcmp(cl.id, "##smore") == 0;
      drawCell(a, cl, ImVec2(c.x, y), ImVec2(c.x + c.w, y + rowH), rowH);
      if (more) {
        // (the More... cell's click opens the menu: its act set the request)
      }
    }
  }
  if (ImGui::BeginPopup(menuId)) {
    for (Cell* c : menu) menuCell(*c);
    if (!menu.empty() && !moreCells.empty()) ImGui::Separator();
    for (Cell& c : moreCells) menuCell(c);
    ImGui::EndPopup();
  }
}

// ---- the parts shared by the screens and the page ----

// The empty instance, the analysis, the error, the recorder: in the well's place.
void wellState(App& a, const PluginTarget& t, sui::Ed& e, ImVec2 p0, ImVec2 p1) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  bevelBox(dl, p0, p1, true);
  sui::wellDropTarget(a, t, p0, p1);
  if (sui::recorderOpenFor(t)) {
    sui::drawRecorder(a, t, e, ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x - 2, p1.y - 2));
    return;
  }
  const float mid = std::round((p0.y + p1.y) * 0.5f);
  const bool analysing = !e.path.empty() && e.job && e.job->state == 0;
  const bool failed = e.job && e.job->state == 2;
  if (analysing) {
    const float pr = std::clamp(e.job->progress.load(), 0.0f, 1.0f);
    const char* stage = pr < 0.12f ? i18n::tr("Reading the file") : pr < 0.5f ? i18n::tr("Listening for the hits") : pr < 0.62f ? i18n::tr("Following the beat")
                        : pr < 0.9f ? i18n::tr("Finding the bars and sections") : i18n::tr("Chopping");
    theme::centreText(dl, boldFont(a), fs, ImVec2(p0.x, mid - fs - 6), ImVec2(p1.x, mid - 6), P.text, fs::u8path(e.path).filename().u8string().c_str());
    theme::centreText(dl, a.fontRegular, fs, ImVec2(p0.x, mid), ImVec2(p1.x, mid + fs), P.textDim, stage);
    const float bw = std::min(p1.x - p0.x - 40, fs * 18);
    const ImVec2 b0(std::round((p0.x + p1.x - bw) * 0.5f), mid + fs + 6), b1(b0.x + bw, b0.y + 6);
    bevelBox(dl, b0, b1, true);
    dl->AddRectFilled(ImVec2(b0.x + 1, b0.y + 1), ImVec2(b0.x + 1 + (bw - 2) * pr, b1.y - 1), P.hot);
    return;
  }
  const char* t1 = failed ? i18n::tr("This file cannot be read") : i18n::tr("Drop audio here");
  const char* t2 = failed ? i18n::tr("Try another file: WAV, AIFF, FLAC, MP3 or OGG.")
                          : i18n::tr("Breaks, loops, full songs, anything: Seltmann finds the tempo and the bars and chops it for you");
  theme::centreText(dl, boldFont(a), fs, ImVec2(p0.x, mid - fs * 2 - 4), ImVec2(p1.x, mid - fs - 4), P.text, t1);
  theme::centreText(dl, a.fontRegular, fs, ImVec2(p0.x + 8, mid - fs), ImVec2(p1.x - 8, mid), P.textDim, t2);
  const float bh = std::round(fs + 8),
              bw = std::ceil(std::max({textW(a, "Load audio..."), textW(a, "From URL..."), textW(a, "Record...")}) + 4 * space::kPad);
  const float bx = std::round((p0.x + p1.x) * 0.5f), by = mid + 6;
  if (by + bh < p1.y - 2) {
    const float hw = std::round(bw * 0.5f);
    if (bevelButton(a, "##swload", "Load audio...", ImVec2(bx - hw - 4 - bw, by), ImVec2(bx - hw - 4, by + bh), false, "Pick an audio file to chop"))
      sui::openLoadDialog(a, t, e);
    noteCtl("##swload");
    if (bevelButton(a, "##swurl", "From URL...", ImVec2(bx - hw, by), ImVec2(bx - hw + bw, by + bh), false, "Import from URL: a web link's audio, chopped here"))
      urlimportui::openForPlugin(a, t, 0);
    noteCtl("##swurl");
    if (bevelButton(a, "##swrec", "Record...", ImVec2(bx - hw + bw + 4, by), ImVec2(bx - hw + 2 * bw + 4, by + bh), false,
                    "Record from an audio input (a microphone, an instrument) and chop the recording"))
      sui::openRecorder(a, t);
    noteCtl("##swrec");
  }
}

// The waveform (the Ribbon's: regions, letters, cuts, flags, tools) and each region's key under its letter.
void wave(App& a, const PluginTarget& t, sui::Ed& e, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  sui::wellDropTarget(a, t, p0, p1);
  const int cardBefore = e.card;
  sui::drawWave(a, t, e, p0, p1, lv);
  // (the Tracker shows a region's values on the band: no card opens over it)
  if (e.card != cardBefore && (e.card == sui::CardRegion || e.card == sui::CardStep)) sui::closeCard(e);
  e.cardTouched = false;
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Model& m = e.m;
  const float gx0 = p0.x + 8, gx1 = p1.x - 8, gy1 = p1.y - 8;
  const double ra = static_cast<double>(m.regionStart), rb = static_cast<double>(std::max(m.regionEnd, m.regionStart + 1));
  auto X = [&](double f) { return gx0 + static_cast<float>((f - ra) / (rb - ra)) * (gx1 - gx0); };
  const int base = sui::baseNote(a, t);
  const bool slots = static_cast<int>(std::lround(sui::paramOf(a, t, SmKeyMap))) == KeySlots;
  ImFont* mono = monoFont(a);
  const float ks = std::round(ImGui::GetFontSize() * 0.8f);
  for (int q = 0; q < static_cast<int>(m.slices.size()); ++q) {
    const auto& s = m.slices[static_cast<size_t>(q)];
    const float x0 = X(static_cast<double>(s.start)), x1 = X(static_cast<double>(s.end));
    const std::string L = regionName(q);
    const float lfs = x1 - x0 >= 30 ? 15.0f : 12.0f;
    if (x1 - x0 < (L.size() > 1 ? 24.0f : 16.0f)) continue;
    const int key = slots ? (sui::sliceSlot(m, q) >= 0 ? base + sui::sliceSlot(m, q) : -1) : base + q;
    if (key < 0 || key > 127) continue;
    const std::string kn = trkNote(a, key);
    const float lw = boldFont(a)->CalcTextSizeA(lfs, FLT_MAX, 0, L.c_str()).x;
    const float kw = mono->CalcTextSizeA(ks, FLT_MAX, 0, kn.c_str()).x;
    const float kx = x0 + 5 + lw + 4;
    if (kx + kw > x1 - 3) continue;
    dl->AddText(mono, ks, ImVec2(kx, gy1 - 3 - ks), theme::withAlpha(P.text, 0.62f), kn.c_str());
  }
}

// The sequence as rows: step, region, reverse, pitch, length, options. Click: the step (heard),
// right-click: the region menu, the wheel scrolls.
void seqRows(App& a, const PluginTarget& t, sui::Ed& e, View& v, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  ImFont* mono = monoFont(a);
  bevelBox(dl, p0, p1, true);
  const float rh = std::round(fs + 3);
  const Model& m = e.m;
  const int n = static_cast<int>(m.pattern.steps.size());
  // the head: the columns' names and how many steps
  const float sbW = std::round(fs * 0.9f);
  const float x0 = p0.x + 3, x1 = p1.x - 3 - sbW;
  const float cw = monoW(a, "0");
  const float cIdx = x0 + 2, cReg = cIdx + cw * 3, cRev = cReg + cw * 3.5f, cPit = cRev + cw * 2, cLen = cPit + cw * 5.5f, cOpt = cLen + cw * 4.5f;
  const float hy = p0.y + 2;
  {
    char b[64];
    std::snprintf(b, sizeof b, "%d %s", n, n == 1 ? i18n::tr("step") : i18n::tr("steps"));
    dl->AddText(a.fontRegular, std::round(fs * 0.85f), ImVec2(cReg, hy + 1), P.textFaint, "ST");
    dl->AddText(a.fontRegular, std::round(fs * 0.85f), ImVec2(cRev, hy + 1), P.textFaint, "R");
    dl->AddText(a.fontRegular, std::round(fs * 0.85f), ImVec2(cPit, hy + 1), P.textFaint, "PIT");
    dl->AddText(a.fontRegular, std::round(fs * 0.85f), ImVec2(cLen, hy + 1), P.textFaint, "LEN");
    if (cOpt + cw * 3 < x1 - monoW(a, b) - 8) dl->AddText(a.fontRegular, std::round(fs * 0.85f), ImVec2(cOpt, hy + 1), P.textFaint, "OPT");
    const float tw = a.fontRegular->CalcTextSizeA(std::round(fs * 0.85f), FLT_MAX, 0, b).x;
    dl->AddText(a.fontRegular, std::round(fs * 0.85f), ImVec2(x1 - tw - 2, hy + 1), P.textFaint, b);
  }
  const float ly0 = hy + rh, ly1 = p1.y - 2;
  const int vis = std::max(1, static_cast<int>((ly1 - ly0) / rh));
  // keep the cursor in view
  const int cur = e.selStep;
  if (cur >= 0 && cur < v.seqTop) v.seqTop = cur;
  if (cur >= v.seqTop + vis) v.seqTop = cur - vis + 1;
  v.seqTop = std::clamp(v.seqTop, 0, std::max(0, n - vis));
  dl->PushClipRect(ImVec2(x0, ly0), ImVec2(x1, ly1), true);
  for (int r = 0; r < vis && v.seqTop + r < n; ++r) {
    const int i = v.seqTop + r;
    const Step& s = m.pattern.steps[static_cast<size_t>(i)];
    const float y = ly0 + static_cast<float>(r) * rh;
    const bool sel = i == e.selStep, playing = i == lv.step;
    if (sel) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rh), theme::withAlpha(P.hot, 0.28f));
    else if (playing) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rh), theme::withAlpha(P.led, 0.22f));
    const float ty = y + std::round((rh - fs) * 0.5f);
    const std::string idx = trk::hex2(i);
    const int bpb = std::max(1, m.grid.beatsPerBar);
    const bool barStart = std::fabs(std::fmod(stepStart(m.pattern, i, 0.0) / bpb, 1.0)) < 1e-6;
    dl->AddText(mono, fs, ImVec2(cIdx, ty), barStart ? P.text : P.textDim, idx.c_str());
    const int k = s.src == kRest ? -1 : stepSlice(m.pattern, s, static_cast<int>(m.slices.size()));
    dl->AddText(mono, fs, ImVec2(cReg, ty), k >= 0 ? sui::regionColor(k) : P.textFaint, k >= 0 ? regionName(k).c_str() : "---");
    dl->AddText(mono, fs, ImVec2(cRev, ty), s.reverse ? P.hot : P.textFaint, s.reverse ? "R" : "\xC2\xB7");
    const std::string pit = std::fabs(s.pitch) >= 0.005f ? sui::pitchText(s.pitch) : "\xC2\xB7\xC2\xB7\xC2\xB7";
    dl->AddText(mono, fs, ImVec2(cPit, ty), std::fabs(s.pitch) >= 0.005f ? valueInk() : P.textFaint, pit.c_str());
    std::string len = "\xC2\xB7\xC2\xB7\xC2\xB7";
    if (s.beats > 0) len = fmt("%.3gb", s.beats);
    else if (s.ties > 0) len = "+" + std::to_string(s.ties);
    else if (s.length < 0.995f) len = fmt("%.0f", s.length * 100.0);
    dl->AddText(mono, fs, ImVec2(cLen, ty), len[0] == '\xC2' ? P.textFaint : valueInk(), len.c_str());
    std::string opt;
    if (s.stutter > 1) opt += "X" + std::to_string(s.stutter);
    if (std::fabs(s.gain) >= 0.05f) opt += (opt.empty() ? "" : " ") + fmt("%+.0fdB", s.gain);
    if (s.offset > 0.005f) opt += (opt.empty() ? "" : " ") + fmt("o%.0f", s.offset * 100.0);
    if (opt.empty()) opt = "\xC2\xB7\xC2\xB7\xC2\xB7";
    dl->AddText(mono, fs, ImVec2(cOpt, ty), opt[0] == '\xC2' ? P.textFaint : P.led, opt.c_str());
  }
  dl->PopClipRect();
  // the mouse
  ImGui::SetCursorScreenPos(ImVec2(x0, ly0));
  ImGui::InvisibleButton("##sseqrows", ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, ly1 - ly0)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  noteCtl("##sseqrows");
  const ImVec2 mp = ImGui::GetIO().MousePos;
  const int under = v.seqTop + static_cast<int>((mp.y - ly0) / rh);
  if (ImGui::IsItemHovered()) {
    if (ImGui::GetIO().MouseWheel != 0) v.seqTop = std::clamp(v.seqTop - static_cast<int>(ImGui::GetIO().MouseWheel) * 2, 0, std::max(0, n - vis));
    if (under >= 0 && under < n) ui::Hint(i18n::tr("A step of the sequence: click hears it, Up and Down move the cursor, right-click: the region menu"));
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && under >= 0 && under < n) selectStep(a, t, e, v, under, true);
  if (ImGui::IsItemDeactivated()) sui::auditionStop(a, t, e);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && under >= 0 && under < n) {
    selectStep(a, t, e, v, under, false);
    const int r = stepRegion(e.m, under);
    sui::openRegionMenu(e, r, under);  // (a blank: its own menu)
  }
  double top = v.seqTop;
  if (vScrollBar(a, "##sseqsb", ImVec2(x1, ly0), ImVec2(p1.x - 3, ly1), &top, vis, std::max(n, vis), 1, "Scroll the sequence"))
    v.seqTop = static_cast<int>(std::lround(top));
}

// The key map on a piano: a key per region from the base note (its letter), the pattern octave below
// it (P, +1 .. +11); a click plays the key (lower on the key: louder).
void keyMap(App& a, const PluginTarget& t, sui::Ed& e, View& v, ImVec2 p0, ImVec2 p1, const LiveView& lv) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Aliased hard(dl);
  const float fs = ImGui::GetFontSize();
  bevelBox(dl, p0, p1, true);
  const int base = sui::baseNote(a, t);
  const int n = static_cast<int>(e.m.slices.size());
  const bool slots = static_cast<int>(std::lround(sui::paramOf(a, t, SmKeyMap))) == KeySlots;
  const std::vector<std::string> names = keyNamesFor(base, slots ? 0 : n);
  // octaves: from the pattern octave to past the last region, as many more as the width wants
  const float x0 = p0.x + 2, x1 = p1.x - 2, y0 = p0.y + 2, y1 = p1.y - 2;
  int firstOct = std::max(0, (base - 12) / 12 - (base % 12 ? 0 : 0));
  int lastOct = std::min(10, (base + std::max(1, n) - 1) / 12);
  const float minWhite = std::max(12.0f, std::round(fs * 1.25f));
  int octs = lastOct - firstOct + 1;
  while (octs < 11 && (x1 - x0) / (octs * 7) > minWhite * 1.8f) {
    if (firstOct > 0 && (octs % 2 == 0)) --firstOct;
    else if (lastOct < 10) ++lastOct;
    else if (firstOct > 0) --firstOct;
    else break;
    octs = lastOct - firstOct + 1;
  }
  // a narrow band: as many whole octaves as fit, from the pattern octave
  while (octs > 1 && (x1 - x0) / (octs * 7) < minWhite) --lastOct, octs = lastOct - firstOct + 1;
  const int lo = firstOct * 12, hi = std::min(127, (lastOct + 1) * 12 - 1);
  const int whites = octs * 7;
  const float ww = (x1 - x0) / static_cast<float>(whites);
  static const int wIdx[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
  static const int bAfter[12] = {-1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1};
  auto keyRect = [&](int note, ImVec2& q0, ImVec2& q1) {
    const int o = note / 12 - firstOct, pc = note % 12;
    if (wIdx[pc] >= 0) {
      q0 = ImVec2(std::round(x0 + (o * 7 + wIdx[pc]) * ww), y0);
      q1 = ImVec2(std::round(x0 + (o * 7 + wIdx[pc] + 1) * ww), y1);
      return false;
    }
    const float cx = x0 + (o * 7 + bAfter[pc] + 1) * ww;
    const float bw = std::max(4.0f, std::round(ww * 0.62f));
    q0 = ImVec2(std::round(cx - bw * 0.5f), y0);
    q1 = ImVec2(std::round(cx + bw * 0.5f), std::round(y0 + (y1 - y0) * 0.6f));
    return true;
  };
  auto regionOf = [&](int note) -> int {
    if (slots) {
      const int s = note - base;
      if (s < 0 || s >= kSlotCount) return -1;
      return s < static_cast<int>(e.m.pattern.slots.size()) ? e.m.pattern.slots[static_cast<size_t>(s)] : -1;
    }
    const int r = note - base;
    return r >= 0 && r < n ? r : -1;
  };
  const int lit = lv.slice >= 0 && !slots ? base + lv.slice : -1;
  ImFont* mono = monoFont(a);
  const float ls = std::round(fs * 0.78f);
  for (int pass = 0; pass < 2; ++pass) {
    for (int note = lo; note <= hi; ++note) {
      ImVec2 q0, q1;
      const bool black = keyRect(note, q0, q1);
      if (black != (pass == 1)) continue;
      const int r = regionOf(note);
      const bool pattern = note >= base - 12 && note < base;
      ImU32 face = black ? P.keyBlack : P.keyWhite;
      if (r >= 0) face = black ? theme::mix(P.keyBlack, sui::regionColor(r), 0.35f) : theme::mix(P.keyWhite, sui::regionColor(r), 0.22f);
      if (note == lit || note == v.pressKey || (e.auditionKey == note)) face = P.hot;
      dl->AddRectFilled(q0, q1, face);
      dl->AddRect(q0, ImVec2(q1.x + 1, q1.y + 1), theme::withAlpha(P.surface950, 0.8f));
      if (r >= 0) dl->AddRectFilled(ImVec2(q0.x + 1, q1.y - 4), ImVec2(q1.x - 1, q1.y - 1), sui::regionColor(r));
      // labels: the region's letter, the pattern octave's P and steps, each C's octave
      std::string lab;
      if (note >= 0 && note < static_cast<int>(names.size())) lab = names[static_cast<size_t>(note)];
      if (pattern) lab = note == base - 12 ? "P" : "+" + std::to_string(note - (base - 12));
      else if (r >= 0) lab = regionName(r);
      const ImU32 ink = black ? P.keyWhite : P.keyBlack;  // each key's letters in the other key's colour
      if (!lab.empty() && (q1.x - q0.x) >= mono->CalcTextSizeA(ls, FLT_MAX, 0, lab.c_str()).x + 2) {
        const float tw = mono->CalcTextSizeA(ls, FLT_MAX, 0, lab.c_str()).x;
        dl->AddText(mono, ls, ImVec2(std::round((q0.x + q1.x - tw) * 0.5f), q1.y - ls - 6), r >= 0 && !black ? IM_COL32(20, 20, 20, 255) : ink, lab.c_str());
      }
      if (!black && note % 12 == 0 && y1 - y0 > ls * 3) {
        const std::string c = trkNote(a, note);
        dl->AddText(mono, ls, ImVec2(q0.x + 2, y0 + 1), IM_COL32(70, 70, 70, 255), c.c_str());
      }
    }
  }
  // the mouse: press a key to play it (Seltmann's own key map: the regions, the pattern below)
  ImGui::SetCursorScreenPos(ImVec2(x0, y0));
  ImGui::InvisibleButton("##skeymap", ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, y1 - y0)));
  noteCtl("##skeymap");
  if (ImGui::IsItemHovered()) ui::Hint(i18n::tr("The key map: a key per region from the base note, the pattern on the octave below (P plays it, +1 .. +11 transpose it)"));
  if (ImGui::IsItemActivated()) {
    const ImVec2 mp = ImGui::GetIO().MousePos;
    int hit = -1;
    for (int pass = 1; pass >= 0 && hit < 0; --pass)
      for (int note = lo; note <= hi && hit < 0; ++note) {
        ImVec2 q0, q1;
        if (keyRect(note, q0, q1) != (pass == 1)) continue;
        if (mp.x >= q0.x && mp.x < q1.x && mp.y >= q0.y && mp.y < q1.y) hit = note;
      }
    if (hit >= 0 && t.isChannel()) {
      a.engine.noteOn(t.owner, hit, sui::clickVelocity(y0, y1, mp.y));
      v.pressKey = hit;
    }
  }
  if (ImGui::IsItemDeactivated() && v.pressKey >= 0) {
    a.engine.noteOff(t.owner, v.pressKey);
    v.pressKey = -1;
  }
}

// The facts line of the titles.
std::string facts(App& a, const PluginTarget& t, sui::Ed& e, bool wide) {
  std::string s;
  const Channel* c = a.doc.song().channel(t.owner);
  const int ci = c ? a.doc.song().channelIndex(t.owner) : -1;
  s = std::string(i18n::tr("Instrument")) + " " + trk::hex2(ci + trackBase(a)) + " " + (c ? c->name : std::string());
  if (!e.path.empty()) s += "   " + fs::u8path(e.path).filename().u8string();
  if (!e.hasModel) return s;
  const Model& m = e.m;
  char b[160];
  const double bars = regionBars(m);
  std::snprintf(b, sizeof b, "   %s  \xC2\xB7  %.3g %s  \xC2\xB7  %zu %s  \xC2\xB7  %s  \xC2\xB7  %.2f BPM", i18n::tr(sui::kindLabel(m.grid.kind)), bars,
                std::fabs(bars - 1) < 1e-9 ? i18n::tr("bar") : i18n::tr("bars"), m.slices.size(), i18n::tr("regions"), sui::chopSummaryText(m), m.grid.bpm);
  s += b;
  if (wide && m.grid.key >= 0) s += std::string("  \xC2\xB7  ") + i18n::tr(sui::keyName(m.grid.key, m.grid.minor));
  LinkState ls = LinkNone;
  const ObjectId pid = sui::linkedPatternOf(a, t, &ls);
  if (pid != kNoId) {
    const int pi = a.doc.song().patternIndex(pid);
    std::snprintf(b, sizeof b, "  \xC2\xB7  %s %02d%s", i18n::tr("linked to pattern"), pi + 1, ls == LinkEdited ? i18n::tr(" (edited by hand)") : "");
    s += b;
  }
  return s;
}

void statusLine(App& a, ImVec2 p0, ImVec2 p1, const std::string& text, bool keys) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  bevelBox(dl, p0, p1, true);
  const char* k = keys ? i18n::tr("Keys: the screen") : i18n::tr("Keys: the pattern");
  const float kw = a.fontRegular->CalcTextSizeA(fs, FLT_MAX, 0, k).x;
  const float ty = std::round(p0.y + (p1.y - p0.y - fs) * 0.5f);
  theme::textEllipsis(dl, monoFont(a), std::round(fs * 0.9f), ImVec2(p0.x + 6, ty + 1), p1.x - p0.x - kw - 24, P.textDim, text.c_str());
  dl->AddText(a.fontRegular, fs, ImVec2(p1.x - kw - 8, ty), keys ? P.text : P.textFaint, k);
}

// The values of the region (or the selected step) as stepper cells.
std::vector<Cell> valueCells(App& a, const PluginTarget& t, sui::Ed& e, View& v, float lw) {
  std::vector<Cell> out;
  if (e.m.slices.empty()) return out;
  const int k = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
  const Slice sl = e.m.slices[static_cast<size_t>(k)];
  const int si = curStep(e);
  const bool step = v.stepScope && si >= 0;
  auto add = [&](Cell c) {
    c.labelW = lw;
    out.push_back(std::move(c));
  };
  const float shift = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
  if (step) {
    const Step s = e.m.pattern.steps[static_cast<size_t>(si)];
    add(sStepper("##svpit", "Pitch", pitchValue(s.pitch), "This step's own pitch (semitones; Shift: tenths; click to type)",
                    [&a, t, &e, s, shift, si](int d) { stepField(a, t, e, 0, s.pitch + d * shift, dragKey(e, 0, si)); },
                    [&a, t, &e](double x) { stepField(a, t, e, 0, x, 0); }));
    add(sStepper("##svgain", "Gain", fmt("%.1f dB", s.gain), "This step's level (dB)",
                    [&a, t, &e, s, si](int d) { stepField(a, t, e, 1, s.gain + d * 0.5, dragKey(e, 1, si)); }, [&a, t, &e](double x) { stepField(a, t, e, 1, x, 0); }));
    add(sStepper("##svstart", "Start", fmt("%.0f %%", s.offset * 100.0), "Where in its region this step starts",
                    [&a, t, &e, s, si](int d) { stepField(a, t, e, 4, s.offset + d * 0.01, dragKey(e, 4, si)); },
                    [&a, t, &e](double x) { stepField(a, t, e, 4, x / 100.0, 0); }));
    add(sStepper("##svlen", "Length", fmt("%.0f %%", s.length * 100.0), "How much of the step sounds",
                    [&a, t, &e, s, si](int d) { stepField(a, t, e, 5, s.length + d * 0.01, dragKey(e, 5, si)); },
                    [&a, t, &e](double x) { stepField(a, t, e, 5, x / 100.0, 0); }));
    add(sStepper("##svstut", "Stutter", fmt("x%.0f", s.stutter), "Repeats inside the step (1 to 16)",
                    [&a, t, &e, s, si](int d) { stepField(a, t, e, 8, s.stutter + d, dragKey(e, 8, si)); }, [&a, t, &e](double x) { stepField(a, t, e, 8, x, 0); }));
    const int r = stepRegion(e.m, si);
    const int nSl = static_cast<int>(e.m.slices.size());
    add(sStepper("##svplays", "Plays", r >= 0 ? regionName(r) : std::string(i18n::tr("rest")), "The region this step plays (below A: a rest)",
                    [&a, t, &e, r, nSl](int d) { stepField(a, t, e, 9, std::clamp(r + d, -1, nSl - 1), 0); }));
    const double unit = std::max(kMinStepBeats, e.m.pattern.unitBeats);
    const double units = s.beats > 0 ? s.beats / unit : 1.0 + s.ties;
    add(sStepper("##svunits", "Steps", fmt("%.3g", units), "How many step lengths the step lasts",
                    [&a, t, &e, units](int d) { stepField(a, t, e, 10, std::max(1.0, units + d), 0); }));
  } else {
    add(sStepper("##svpit", "Pitch", pitchValue(sl.pitch), "The region's pitch (semitones; Shift: tenths; click to type)",
                    [&a, t, &e, sl, shift, k](int d) { regionField(a, t, e, 0, sl.pitch + d * shift, dragKey(e, 0, k)); },
                    [&a, t, &e](double x) { regionField(a, t, e, 0, x, 0); }));
    add(sStepper("##svgain", "Gain", fmt("%.1f dB", sl.gain), "The region's level (dB)",
                    [&a, t, &e, sl, k](int d) { regionField(a, t, e, 1, sl.gain + d * 0.5, dragKey(e, 1, k)); },
                    [&a, t, &e](double x) { regionField(a, t, e, 1, x, 0); }));
    add(sStepper("##svfin", "Fade in", fmt("%.0f ms", sl.fadeIn), "A fade at the region's start (ms)",
                    [&a, t, &e, sl, k](int d) { regionField(a, t, e, 2, sl.fadeIn + d * (ImGui::GetIO().KeyShift ? 10 : 1), dragKey(e, 2, k)); },
                    [&a, t, &e](double x) { regionField(a, t, e, 2, x, 0); }));
    add(sStepper("##svfout", "Fade out", fmt("%.0f ms", sl.fadeOut), "A fade at the region's end (ms)",
                    [&a, t, &e, sl, k](int d) { regionField(a, t, e, 3, sl.fadeOut + d * (ImGui::GetIO().KeyShift ? 10 : 1), dragKey(e, 3, k)); },
                    [&a, t, &e](double x) { regionField(a, t, e, 3, x, 0); }));
    add(sStepper("##svstart", "Start", fmt("%.0f %%", sl.offset * 100.0), "Where the region starts to play",
                    [&a, t, &e, sl, k](int d) { regionField(a, t, e, 4, sl.offset + d * 0.01, dragKey(e, 4, k)); },
                    [&a, t, &e](double x) { regionField(a, t, e, 4, x / 100.0, 0); }));
    add(sStepper("##svlen", "Length", fmt("%.0f %%", sl.length * 100.0), "How much of the region plays",
                    [&a, t, &e, sl, k](int d) { regionField(a, t, e, 5, sl.length + d * 0.01, dragKey(e, 5, k)); },
                    [&a, t, &e](double x) { regionField(a, t, e, 5, x / 100.0, 0); }));
    add(sStepper("##svchoke", "Choke", sl.choke ? std::to_string(sl.choke) : std::string(i18n::tr("off")), "Regions in one choke group cut each other off",
                    [&a, t, &e, sl](int d) { regionField(a, t, e, 6, sl.choke + d, 0); }, [&a, t, &e](double x) { regionField(a, t, e, 6, x, 0); }));
  }
  return out;
}

Cell scopeRow(View& v, sui::Ed& e) {
  const bool hasStep = curStep(e) >= 0;
  Cell step = sRadio("##svstep", "This step", "The values edit the selected step only (Tab)", v.stepScope, [&v, &e] {
    v.stepScope = true;
    if (e.selStep < 0) e.selStep = firstStepOf(e.m, e.selSlice);
  });
  step.enabled = hasStep;
  return row({sRadio("##svregion", "Region", "The values edit the region (every step that plays it)", !v.stepScope, [&v] { v.stepScope = false; }), step});
}

// The band modal boxes: a card's body (Grid, Loops, Chop, Steps, Takes), every value, save a layout.
void modals(App& a, const PluginTarget& t, sui::Ed& e, View& v, const LiveView& lv) {
  (void)lv;
  const float fs = ImGui::GetFontSize();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  if (v.modalAsk) {
    ImGui::OpenPopup("##sseltbox");
    v.boxId = ImGui::GetID("##sseltbox");
    v.modalAsk = false;
  }
  if (v.modal == ModalNone) return;
  const char* title = "";
  ImVec2 size(fs * 30, fs * 12);
  if (v.modal == ModalCard) {
    static const char* const titles[] = {"", i18n::N_("Region"), i18n::N_("Step"), i18n::N_("Sound"), i18n::N_("Tempo and grid"), i18n::N_("Loops to drag out"), i18n::N_("Chop"), i18n::N_("Steps"), i18n::N_("Takes")};
    title = titles[std::clamp(v.modalCard, 0, 8)];
    const ImVec2 cs = sui::cardSize(e, v.modalCard, vp->WorkSize.x - 40);
    size = ImVec2(std::min(vp->WorkSize.x - 20, cs.x + 24), std::min(vp->WorkSize.y - 20, cs.y + fs * 3 + 16));
  } else if (v.modal == ModalValues) {
    title = v.stepScope ? i18n::N_("This step") : i18n::N_("Region");
    size = ImVec2(std::min(vp->WorkSize.x - 20, fs * 34), std::min(vp->WorkSize.y - 20, fs * 13));
  } else if (v.modal == ModalSave) {
    title = i18n::N_("Save the layout");
    size = ImVec2(std::min(vp->WorkSize.x - 20, fs * 26), fs * 7);
  }
  if (!beginModalBox(a, "##sseltbox", i18n::tr(title), size)) {
    v.modal = ModalNone;
    return;
  }
  const ImVec2 w0 = ImGui::GetWindowPos(), w1(w0.x + ImGui::GetWindowWidth(), w0.y + ImGui::GetWindowHeight());
  const float rowH = std::round(fs + 7);
  const ImVec2 c0(w0.x + 12, w0.y + 16 + fs), c1(w1.x - 12, w1.y - 14 - rowH);
  if (v.modal == ModalCard) {
    sui::cardBody(a, t, e, v.modalCard, c0, c1);
  } else if (v.modal == ModalValues) {
    const float lw = labelColumn(a, {"Pitch", "Gain", "Fade in", "Fade out", "Start", "Length", "Choke", "Stutter", "Plays", "Steps"});
    std::vector<Cell> cells = valueCells(a, t, e, v, lw);
    cells.push_back(scopeRow(v, e));
    const float colW = std::floor((c1.x - c0.x - 8) / 2);
    const int per = (static_cast<int>(cells.size()) + 1) / 2;
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
      const float x = c0.x + static_cast<float>(i / per) * (colW + 8), y = c0.y + static_cast<float>(i % per) * (rowH + space::kGap);
      drawCell(a, cells[static_cast<size_t>(i)], ImVec2(x, y), ImVec2(x + colW, y + rowH), rowH);
    }
  } else if (v.modal == ModalSave) {
    ImGui::SetCursorScreenPos(c0);
    ImGui::SetNextItemWidth(c1.x - c0.x);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputTextWithHint("##ssavename", i18n::tr("Name for this layout"), v.saveName, sizeof v.saveName, ImGuiInputTextFlags_EnterReturnsTrue);
    const float bw = std::ceil(textW(a, "Save") + 4 * space::kPad);
    if ((bevelButton(a, "##ssavebtn", "Save", ImVec2(w1.x - 12 - bw * 2 - 4, w1.y - 10 - rowH), ImVec2(w1.x - 16 - bw, w1.y - 10), false,
                     "Save the chop settings and the sequence as your own layout (under Mine)") ||
         enter) &&
        v.saveName[0]) {
      if (sui::saveLayoutAs(a, t, e, v.saveName)) {
        v.saveName[0] = 0;
        ImGui::CloseCurrentPopup();
        v.modal = ModalNone;
      }
    }
  }
  const float bw = std::ceil(std::max(textW(a, "Close"), textW(a, "Save")) + 4 * space::kPad);
  if (bevelButton(a, "##sboxclose", "Close", ImVec2(w1.x - 12 - bw, w1.y - 10 - rowH), ImVec2(w1.x - 12, w1.y - 10), false, "Close the box (Esc)")) {
    ImGui::CloseCurrentPopup();
    v.modal = ModalNone;
  }
  noteCtl("##sboxclose");
  endModalBox();
}

// The More... menu's own entries (everything the panel may fold away is reachable here or on keys).
std::vector<Cell> moreEntries(App& a, const PluginTarget& t, sui::Ed& e, View& v) {
  std::vector<Cell> m;
  m.push_back(sButton("##mvalues", "Values...", "Every value of the region or the step in a box", [&v] {
    v.modal = ModalValues;
    v.modalAsk = true;
  }));
  m.push_back(sButton("##mchop", "Chop modes...", "Every chop mode, the snap, cuts at zero crossings", [&v] { openModalCard(v, sui::CardChop); }));
  m.push_back(sButton("##msteps", "Steps...", "The step length, add or remove a step, type the steps", [&v] { openModalCard(v, sui::CardSteps); }));
  m.push_back(sButton("##mloops", "Loops...", "The pattern, the best phrases and the sections to drag out", [&v] { openModalCard(v, sui::CardLoops); }));
  m.push_back(sButton("##mgrid", "Grid...", "Tap, nudge, meter, key, how sure, analyse again", [&v] { openModalCard(v, sui::CardGrid); }));
  m.push_back(sButton("##mtakes", "Takes...", "The takes recorded into this instance", [&v] { openModalCard(v, sui::CardTakes); }));
  LinkState ls = LinkNone;
  const bool linked = sui::linkedPatternOf(a, t, &ls) != kNoId;
  m.push_back(sButton("##mrelink", "Relink", "The linked pattern follows the sequence again (your note edits in it go)", [&a, t] { sui::relink(a, t); }, false,
                     linked && ls == LinkEdited));
  m.push_back(sButton("##munlink", "Unlink", "The pattern keeps its notes and stops following the sequence", [&a, t] { sui::unlink(a, t); }, false, linked));
  m.push_back(sButton("##mbar1", "Bar 1 here", "Bar 1 of the grid at the selected region's start", [&a, t, &e] { bar1Here(a, t, e); }));
  m.push_back(sButton("##manalyse", "Analyse again", "Run the tempo, beat and section analysis again (keeps the layout)", [&e] { analyseAgain(e); }));
  if (isSong(e))
    m.push_back(sCheck("##mstrip", "Whole song strip", "The whole file above the waveform: drag its box to pick the bars, double-click a section to take it",
                      !e.stripHidden, [&e] { e.stripHidden = !e.stripHidden; }));
  return m;
}

// ---- keys ----
void runChopKey(App& a, const PluginTarget& t, sui::Ed& e, View& v, const KeyEv& k) {
  const bool ctrl = (k.mods & (KMOD_CTRL | KMOD_GUI)) != 0, alt = (k.mods & KMOD_ALT) != 0, shift = (k.mods & KMOD_SHIFT) != 0;
  (void)ctrl;
  const int n = static_cast<int>(e.m.slices.size()), ns = static_cast<int>(e.m.pattern.steps.size());
  if (!e.hasModel || n == 0) return;
  if (alt) {
    if (k.sc >= SDL_SCANCODE_1 && k.sc <= SDL_SCANCODE_7) {
      sui::setChopSize(a, t, e, k.sc - SDL_SCANCODE_1);
      return;
    }
    switch (k.sc) {
      case SDL_SCANCODE_E: e.tool = (e.tool + 1) % ToolCount; return;
      case SDL_SCANCODE_R: reverseScope(a, t, e, v.stepScope); return;
      case SDL_SCANCODE_D: duplicateStep(a, t, e); return;
      case SDL_SCANCODE_P: sui::togglePattern(a, t, e); return;
      case SDL_SCANCODE_Q:
        if (e.pattern.on) sui::togglePattern(a, t, e);
        sui::auditionStop(a, t, e);
        return;
      case SDL_SCANCODE_UP:
      case SDL_SCANCODE_DOWN: {
        const float d = (k.sc == SDL_SCANCODE_UP ? 1.0f : -1.0f) * (shift ? 0.1f : 1.0f);
        const int si = curStep(e);
        if (v.stepScope && si >= 0) stepField(a, t, e, 0, e.m.pattern.steps[static_cast<size_t>(si)].pitch + d, 0);
        else regionField(a, t, e, 0, e.m.slices[static_cast<size_t>(std::clamp(e.selSlice, 0, n - 1))].pitch + d, 0);
        return;
      }
      default: return;
    }
  }
  switch (k.sc) {
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT: selectRegion(a, t, e, v, e.selSlice + (k.sc == SDL_SCANCODE_RIGHT ? 1 : -1), true); break;
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN: {
      const int d = k.sc == SDL_SCANCODE_DOWN ? 1 : -1;
      if (shift) moveStepBy(a, t, e, d);
      else if (ns > 0) selectStep(a, t, e, v, e.selStep < 0 ? (d > 0 ? 0 : ns - 1) : e.selStep + d, true);
      break;
    }
    case SDL_SCANCODE_TAB:
      v.stepScope = !v.stepScope;
      if (v.stepScope && e.selStep < 0) e.selStep = firstStepOf(e.m, e.selSlice);
      break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      if (shift) sui::togglePattern(a, t, e);
      else hearRegion(a, t, e, v.stepScope && curStep(e) >= 0 ? stepRegion(e.m, curStep(e)) : e.selSlice);
      break;
    case SDL_SCANCODE_INSERT: sui::splitRegionUi(a, t, e, e.selSlice, 2); break;
    case SDL_SCANCODE_BACKSPACE:
      if (n > 1) sui::deleteRegionUi(a, t, e, e.selSlice);
      break;
    case SDL_SCANCODE_DELETE: deleteStep(a, t, e); break;
    default: break;
  }
}

std::vector<const Preset*> groupItems(App& a, sui::Ed& e) {
  const std::string q = gLastCh != kNoId ? viewOf(gLastCh).search : "";
  std::vector<const Preset*> v = sui::layoutGroup(a, e, std::clamp(e.presetTab, 0, sui::layoutGroupCount() - 1), q);
  if (e.presetTab == 1 && e.hasCustom) v.insert(v.begin(), &sui::customLayout());
  return v;
}
void applyCursor(App& a, const PluginTarget& t, sui::Ed& e, View& v) {
  const auto items = groupItems(a, e);
  if (v.cursor < 0 || v.cursor >= static_cast<int>(items.size())) return;
  const Preset* p = items[static_cast<size_t>(v.cursor)];
  if (p == &sui::customLayout()) sui::restoreCustom(a, t, e);
  else sui::keepLayout(a, t, e, *p);
}
void starCursor(App& a, sui::Ed& e, View& v) {
  const auto items = groupItems(a, e);
  if (v.cursor < 0 || v.cursor >= static_cast<int>(items.size()) || items[static_cast<size_t>(v.cursor)] == &sui::customLayout()) return;
  sui::prefs(a, e).toggleStar(items[static_cast<size_t>(v.cursor)]->name);
  sui::savePrefs(a, e);
}

void runInstKey(App& a, const PluginTarget& t, sui::Ed& e, View& v, const KeyEv& k) {
  const bool alt = (k.mods & KMOD_ALT) != 0;
  const auto items = groupItems(a, e);
  const int ni = static_cast<int>(items.size());
  if (alt) {
    switch (k.sc) {
      case SDL_SCANCODE_P:
        if (!e.previewName.empty()) sui::previewStop(a, t, e);
        else if (v.cursor >= 0 && v.cursor < ni && items[static_cast<size_t>(v.cursor)] != &sui::customLayout()) sui::previewStart(a, t, e, *items[static_cast<size_t>(v.cursor)]);
        return;
      case SDL_SCANCODE_Q:
        sui::previewStop(a, t, e);
        if (e.pattern.on) sui::togglePattern(a, t, e);
        return;
      case SDL_SCANCODE_F: starCursor(a, e, v); return;
      case SDL_SCANCODE_UP:
      case SDL_SCANCODE_DOWN:
        pui::setParamValue(a, t, SmBaseNote, std::clamp(sui::baseNote(a, t) + (k.sc == SDL_SCANCODE_UP ? 1 : -1), 12, 115), false);
        return;
      default: return;
    }
  }
  switch (k.sc) {
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:
      if (ni > 0) v.cursor = std::clamp(v.cursor + (k.sc == SDL_SCANCODE_DOWN ? 1 : -1), 0, ni - 1);
      break;
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_TAB: {
      const int g = sui::layoutGroupCount();
      const int d = k.sc == SDL_SCANCODE_LEFT || (k.sc == SDL_SCANCODE_TAB && (k.mods & KMOD_SHIFT)) ? -1 : 1;
      e.presetTab = (e.presetTab + d + g) % g;
      v.cursor = 0, v.listTop = 0;
      break;
    }
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: applyCursor(a, t, e, v); break;
    default: break;
  }
}

void drainKeys(App& a, const PluginTarget& t, sui::Ed& e, View& v, int screen) {
  std::vector<KeyEv> ks;
  ks.swap(keyQueue());
  for (const KeyEv& k : ks) {
    if (screen == ScreenChop) runChopKey(a, t, e, v, k);
    else runInstKey(a, t, e, v, k);
  }
}

// The per-frame start of a screen: the model, the pattern latch, the gesture counter.
bool begin(App& a, ObjectId ch, sui::Ed*& out, LiveView& lv) {
  const PluginTarget t = targetOf(ch);
  if (!sui::prepare(a, t)) return false;
  sui::Ed& e = sui::ed(t);
  lv = sui::live(a, t);
  sui::syncPattern(a, t, e, lv);
  sui::previewTick(a, t, e);
  View& v = viewOf(ch);
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ++e.gesture;
  if (v.paramDrag && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    a.doc.endGesture();
    v.paramDrag = false;
  }
  if (e.hasModel && !(e.job && e.job->state == 0)) devsig::reach("seltmann-ready");  // (as the Ribbon: scripts wait for it)
  e.hosted = true;
  if (gLastCh != ch) {
    gLastCh = ch;
    v.seqTop = 0;
  }
  out = &e;
  return true;
}

// The Chop screen's button panel (rows 4: the whole band; 2: folded).
std::vector<Column> chopColumns(App& a, const PluginTarget& t, sui::Ed& e, View& v, int rows, float lw, bool page) {
  const bool have = e.hasModel && !e.m.slices.empty();
  const int n = static_cast<int>(e.m.slices.size());
  const int chopNow = sui::chopSizeOf(e.m);
  const bool playing = e.pattern.on;
  const int sync = static_cast<int>(std::lround(sui::paramOf(a, t, SmSync)));
  LinkState ls = LinkNone;
  const bool linked = sui::linkedPatternOf(a, t, &ls) != kNoId;
  const ObjectId ch = t.owner;
  auto chopBtn = [&](const char* id, const char* label, int which, const char* hint) {
    return sButton(id, label, hint, [&a, t, &e, which] { sui::setChopSize(a, t, e, which); }, chopNow == which, have);
  };
  Cell base = sStepper("##sbase", "Base", trkNote(a, sui::baseNote(a, t)), "The key region A plays (the regions go up from it, the pattern on the octave below)",
                          [&a, t, &v](int d) { setParam(a, t, v, SmBaseNote, std::clamp(sui::baseNote(a, t) + d * (ImGui::GetIO().KeyShift ? 12 : 1), 12, 115)); });
  base.labelW = std::ceil(textW(a, "Base") + space::kGroup);
  Cell stop = sButton("##sstop", "Stop", "Stops the sequence and the regions (Alt+Q)", [&a, t, &e] {
    if (e.pattern.on) sui::togglePattern(a, t, e);
    sui::auditionStop(a, t, e);
  });
  Cell pattern = sButton("##spattern", "Pattern", "Plays the sequence on the instrument, again: stops (Shift+Enter, Alt+P)", [&a, t, &e] { sui::togglePattern(a, t, e); },
                        playing, have);
  Cell tPlay = sRadio("##stplay", "Play", "Click a region to hear it (Alt+E: the next tool)", e.tool == ToolPlay, [&e] { e.tool = ToolPlay; });
  Cell tCut = sRadio("##stcut", "Cut", "Double-click adds a cut, drag moves one, right-click removes it", e.tool == ToolCut, [&e] { e.tool = ToolCut; });
  Cell tGrid = sRadio("##stgrid", "Grid", "Drag the bar-1 flag to move the grid, Alt+drag a bar line to stretch it", e.tool == ToolGrid, [&e] { e.tool = ToolGrid; });
  Cell split2 = sButton("##ssplit2", "Split 2", "Split the region in two (Ins)", [&a, t, &e] { sui::splitRegionUi(a, t, e, e.selSlice, 2); }, false, have);
  Cell split4 = sButton("##ssplit4", "Split 4", "Split the region in four", [&a, t, &e] { sui::splitRegionUi(a, t, e, e.selSlice, 4); }, false, have);
  Cell join = sButton("##sjoin", "Join", "Join the region with the one before it (Backspace)", [&a, t, &e] { sui::deleteRegionUi(a, t, e, e.selSlice); }, false, have && n > 1);
  Cell del = sButton("##sdelstep", "Delete", "Delete the step from the sequence (Del)", [&a, t, &e] { deleteStep(a, t, e); }, false,
                    have && curStep(e) >= 0 && e.m.pattern.steps.size() > 1);
  Cell dup = sButton("##sdup", "Duplicate", "Duplicate the step (the copy follows it; Alt+D)", [&a, t, &e] { duplicateStep(a, t, e); }, false, have && curStep(e) >= 0);
  Cell startHere = sButton("##sstarthere", "Start here", "The sequence starts on this step (the steps before move to the end)", [&a, t, &e] {
    const int s = curStep(e);
    if (s > 0) sui::startHereUi(a, t, e, s, true);
  }, false, have && curStep(e) > 0);
  Cell reset = sButton("##sreset", "Reset order", "Every region once, in order (A B C D ...)", [&a, t, &e] { resetOrder(a, t, e); }, false, have && !isNaturalOrder(e.m));
  Cell stepsBox = sButton("##sstepsbox", "Steps...", "The step length, add or remove a step, type the steps", [&v] { openModalCard(v, sui::CardSteps); }, false, have);
  Cell hearR = sButton("##shearregion", "Region", "Hear the region (Enter)", [&a, t, &e] { hearRegion(a, t, e, e.selSlice); }, false, have);
  Cell hearS = sButton("##shearstep", "Step", "Hear the selected step's region", [&a, t, &e] { hearRegion(a, t, e, stepRegion(e.m, curStep(e))); }, false,
                      have && curStep(e) >= 0);
  const int si = curStep(e);
  const bool revOn = have && (v.stepScope && si >= 0 ? e.m.pattern.steps[static_cast<size_t>(si)].reverse
                                                     : e.m.slices[static_cast<size_t>(std::clamp(e.selSlice, 0, n - 1))].reverse);
  Cell rev = sButton("##sreverse", "Reverse", "Play the region (or the step) backwards (Alt+R)", [&a, t, &e, &v] { reverseScope(a, t, e, v.stepScope); }, revOn, have);
  Cell rest = sButton("##srest", "Rest", "The step becomes a rest (again: it plays the region)", [&a, t, &e] { restStep(a, t, e); }, false, have && curStep(e) >= 0);
  Cell loopDrag = sDrag("##sloopdrag", "LOOP", "Drag the sequence out as audio at the project's tempo", [&a, t, &e] { sui::dragOut(a, t, e, "pattern", "pattern"); }, have);
  Cell midiDrag = sDrag("##smididrag", "MIDI", "Drag the sequence out as notes", [&a, t, &e] { sui::dragOut(a, t, e, "midi", "pattern"); }, have);
  Cell send = sButton("##ssend", linked ? "Send to project (linked)" : "Send to project",
                     "A pattern on this channel and a clip in the playlist; the pattern follows later edits of the sequence", [&a, t, &e] { sui::sendToProject(a, t, e); },
                     linked, have);
  Cell sendShort = sButton("##ssend", "Send", "A pattern on this channel and a clip in the playlist; the pattern follows later edits of the sequence",
                          [&a, t, &e] { sui::sendToProject(a, t, e); }, linked, have);
  Cell makeLoops = sButton("##smakeloops", "Make loops", "Render the pattern, the best phrases and the sections into the \"Seltmann loops\" folder",
                          [&a, t, &e] { sui::makeLoops(a, t, e); }, false, have);
  Cell loopsBox = sButton("##sloopsbox", "Loops...", "The pattern, the best phrases and the sections to drag out", [&v] { openModalCard(v, sui::CardLoops); }, false, have);
  Cell load = sButton("##sload", "Load...", "Load another audio file into this instance", [&a, t, &e] { sui::openLoadDialog(a, t, e); });
  Cell url = sButton("##surl", "URL...", "Import from URL: a web link's audio into this instance", [&a, t] { urlimportui::openForPlugin(a, t, 0); });
  Cell take = sButton("##stake", "Record take", "Record a new take into this instance (keep the chop or analyse it fresh)", [&a, t] { sui::openRecorder(a, t); },
                     sui::recorderOpenFor(t));
  Cell tempo = sStepper("##stempo", "Tempo", fmt("%.2f", e.m.grid.bpm), "The tempo the analysis found (click to type; bar 1 stays)",
                           [&a, t, &e](int d) { setTempo(a, t, e, e.m.grid.bpm + d * (ImGui::GetIO().KeyShift ? 0.01 : 1.0)); },
                           [&a, t, &e](double x) { setTempo(a, t, e, x); }, have);
  tempo.labelW = std::ceil(std::max(textW(a, "Tempo"), textW(a, "Follow")) + space::kGroup);
  Cell fixes = row({sButton("##shalf", "/2", "Half the tempo: the analysis heard double time", [&a, t, &e] {
                      Model nm = e.m;
                      gridHalve(nm.grid);
                      gridEdit(a, t, e, nm, i18n::N_("Half tempo"));
                    }, false, have),
                    sButton("##sdouble", "x2", "Double the tempo: the analysis heard half time", [&a, t, &e] {
                      Model nm = e.m;
                      gridDouble(nm.grid);
                      gridEdit(a, t, e, nm, i18n::N_("Double tempo"));
                    }, false, have),
                    sButton("##stap", "Tap", "Tap along on the beat; after four taps the tempo follows (bar 1 stays)", [&a, t, &e] { tapTempoNow(a, t, e); }, false, have)});
  Cell follow = sStepper("##sfollow", "Follow", i18n::tr(followName(sync)), "How the regions follow the project's tempo: Stretch, Resample or Off",
                            [&a, t, &v, sync](int d) { setParam(a, t, v, SmSync, std::clamp(sync + d, 0, 2)); });
  follow.labelW = tempo.labelW;
  Cell gridBox = sButton("##sgridbox", "Grid...", "Tap, nudge, meter, key, how sure, analyse again", [&v] { openModalCard(v, sui::CardGrid); }, false, have);
  Cell exitB = sButton("##sexit", "Exit", "Back to the pattern (Esc)", [&a] { trackereditors::close(a); });
  Cell pageB = sButton("##spage", "Chop page", "Everything on one page beside the Tracker's other pages", [&a] { showPage(a, "Chop"); });
  Cell instB = sButton("##sinst", "Instrument", "Sound, layouts, grid and the key map (Ctrl+Alt+I)", [&a, ch] { trackereditors::open(a, trackereditors::Editor::Instrument, ch); });
  Cell more = sButton("##smore", "More...", "Everything else: the values, the chop modes, the steps, loops, grid, takes, the link", [page] {
    ImGui::OpenPopup(page ? "##spagemore" : "##sseltmore");
    keepKeysHere();
  });
  Cell regionCap = sCaption("keys play the regions");
  std::vector<Cell> vals = valueCells(a, t, e, v, lw);
  std::vector<Column> c;
  auto col = [&](int prio, float units, std::vector<Cell> cells) {
    Column k;
    k.prio = prio, k.units = units, k.cells = std::move(cells);
    c.push_back(std::move(k));
  };
  auto at = [&](size_t i) { return i < vals.size() ? vals[i] : sCaption(""); };
  if (rows >= 4) {
    col(0, 6.5f, {base, stop, pattern, regionCap});
    col(5, 4.2f, {hearR, hearS, rev, rest});
    col(1, 9.6f, {sCaption("Chop"), row({chopBtn("##sc0", "Auto", 0, "The chop size the material wants (Alt+1)"), chopBtn("##sc1", "1 bar", 1, "A region per bar (Alt+2)"),
                                        chopBtn("##sc2", "1/2", 2, "A region per half bar (Alt+3)")}),
                  row({chopBtn("##sc3", "1/4", 3, "A region per beat (Alt+4)"), chopBtn("##sc4", "1/8", 4, "A region per eighth (Alt+5)"),
                       chopBtn("##sc5", "1/16", 5, "A region per sixteenth (Alt+6)")}),
                  row({chopBtn("##sc6", "Hits", 6, "A region per hit (Alt+7)"),
                       sButton("##schopmore", "More...", "Every chop mode, the snap, cuts at zero crossings", [&v] { openModalCard(v, sui::CardChop); }, false, have)})});
    col(3, 4.4f, {sCaption("Tool"), tPlay, tCut, tGrid});
    col(2, 5.2f, {split2, join, dup, reset});
    col(6, 5.2f, {split4, del, startHere, stepsBox});
    col(1, 9.0f, {at(0), at(1), at(2), at(3)});
    col(4, 9.0f, {at(4), at(5), at(6), scopeRow(v, e)});
    col(3, 8.2f, {row({loopDrag, midiDrag}), send, row({makeLoops, loopsBox}), row({load, url, take})});
    col(5, 8.0f, {tempo, fixes, follow, gridBox});
    if (page) col(0, 4.6f, {exitB, instB, more, sCaption("")});
    else col(0, 4.6f, {exitB, pageB, instB, more});
  } else {
    col(0, 6.0f, {base, row({tPlay, tCut, tGrid})});
    col(4, 4.0f, {stop, pattern});
    col(2, 7.5f, {row({chopBtn("##sc0", "Auto", 0, "The chop size the material wants (Alt+1)"), chopBtn("##sc2", "1/2", 2, "A region per half bar (Alt+3)"),
                       chopBtn("##sc3", "1/4", 3, "A region per beat (Alt+4)")}),
                  row({chopBtn("##sc4", "1/8", 4, "A region per eighth (Alt+5)"), chopBtn("##sc5", "1/16", 5, "A region per sixteenth (Alt+6)"),
                       chopBtn("##sc6", "Hits", 6, "A region per hit (Alt+7)")})});
    col(3, 4.0f, {split2, join});
    col(1, 8.0f, {at(0), at(1)});
    col(5, 8.0f, {follow, tempo});
    col(3, 5.5f, {row({loopDrag, midiDrag}), makeLoops});
    col(2, 5.0f, {sendShort, take});
    // (one row: More... and Exit side by side, the rest of each column in More... and on keys)
    if (page) col(6, 4.6f, {instB, sCaption("")});
    else col(6, 4.6f, {pageB, instB});
    if (rows == 1) col(0, 6.0f, {row({more, exitB})});
    else if (page) col(0, 4.0f, {more, sCaption("")});
    else col(0, 4.0f, {exitB, more});
  }
  // the cells the band has no room for: in More... (with the chop sizes it left out)
  if (rows < 4) {
    Column hidden;
    hidden.prio = 99;
    hidden.cells = {chopBtn("##sc1", "1 bar", 1, "A region per bar (Alt+2)"), hearR, hearS, rev, rest, split4, del, dup, startHere, reset, stepsBox, send, loopsBox, load, url, fixes, gridBox};
    hidden.least = 1e9f;
    c.push_back(std::move(hidden));
  }
  return c;
}

}  // namespace

// ================================================================== public

bool isSeltmann(App& a, ObjectId channel) {
  const Channel* c = channel == kNoId ? nullptr : a.doc.song().channel(channel);
  return c && c->plugin.type == kType;
}

float need(int screen, float fs, float workH) {
  const float share = std::round(workH * (workH < 760 ? 0.36f : 0.43f));
  return std::max(std::round(fs * (screen == ScreenChop ? 16.5f : 19.0f)), share);
}

bool takesKey(int screen, int sc, int kc, int mods) {
  (void)kc;
  const bool ctrl = (mods & (KMOD_CTRL | KMOD_GUI)) != 0, alt = (mods & KMOD_ALT) != 0;
  if (ctrl) return false;  // (Ctrl+W and Esc: the band closes the screen itself)
  if (alt) {
    if (screen == ScreenChop)
      return (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_7) || sc == SDL_SCANCODE_E || sc == SDL_SCANCODE_R || sc == SDL_SCANCODE_D || sc == SDL_SCANCODE_P ||
             sc == SDL_SCANCODE_Q || sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN;
    return sc == SDL_SCANCODE_P || sc == SDL_SCANCODE_Q || sc == SDL_SCANCODE_F || sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN;
  }
  switch (sc) {
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_TAB:
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return true;
    case SDL_SCANCODE_INSERT:
    case SDL_SCANCODE_BACKSPACE:
    case SDL_SCANCODE_DELETE: return screen == ScreenChop;
    default: return false;
  }
}

void queueKey(int sc, int kc, int mods) { keyQueue().push_back({sc, kc, mods}); }

// The open box's popup closed (it is not drawn again once the modal is none).
void closeBox(View& v) {
  ImGuiContext& g = *GImGui;
  for (int i = 0; v.boxId && i < g.OpenPopupStack.Size; ++i)
    if (g.OpenPopupStack[i].PopupId == v.boxId) {
      ImGui::ClosePopupToLevel(i, false);
      break;
    }
  v.modal = ModalNone;
}

bool closeInner(App& a, ObjectId channel) {
  (void)a;
  View& v = viewOf(channel);
  if (v.modal != ModalNone) {
    closeBox(v);
    return true;
  }
  return false;
}

void leave(App& a, ObjectId channel) {
  if (!isSeltmann(a, channel)) return;
  const PluginTarget t = targetOf(channel);
  sui::Ed& e = sui::ed(t);
  sui::auditionStop(a, t, e);
  sui::previewStop(a, t, e);
  keyQueue().clear();
  View& v = viewOf(channel);
  if (v.pressKey >= 0) a.engine.noteOff(channel, v.pressKey), v.pressKey = -1;
  if (v.modal != ModalNone) closeBox(v);
}

void chopScreen(App& a, ObjectId ch, ImVec2 p0, ImVec2 p1, bool keys) {
  sui::Ed* ep = nullptr;
  LiveView lv;
  if (!begin(a, ch, ep, lv)) return;
  sui::Ed& e = *ep;
  const PluginTarget t = targetOf(ch);
  View& v = viewOf(ch);
  drainKeys(a, t, e, v, ScreenChop);
  gScreen = "chop";
  gRows.clear();
  ImGui::PushID(static_cast<int>(ch));
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  const float titleH = std::round(fs + 8), rowH = std::round(fs + 7), statusH = std::round(fs + 6);
  int rows = 4;
  auto panelH = [&](int r) { return static_cast<float>(r) * rowH + static_cast<float>(r - 1) * space::kGap + 8; };
  // (a short band, the 1024 px window's: one row of buttons, the rest under More... and on keys,
  // so the waveform keeps its room)
  if (H - titleH - statusH - panelH(4) < fs * 9.0f) rows = 1;
  const float pH = panelH(rows);
  // ---- the title: what it is, the region stepper at the right
  {
    bevelBox(dl, p0, ImVec2(p1.x, p0.y + titleH), false);
    const float ty = std::round(p0.y + (titleH - fs) * 0.5f);
    dl->AddText(boldFont(a), fs, ImVec2(p0.x + 8, ty), P.text, i18n::tr("Chop"));
    const float x = p0.x + 16 + boldFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Chop")).x;
    const float stW = std::round(std::max(fs * 9.5f, textW(a, "Region") + monoW(a, "000 / 000") + fs * 3.5f));
    const float sx = p1.x - stW - 6;
    theme::textEllipsis(dl, monoFont(a), fs, ImVec2(x, ty), sx - x - 8, valueInk(), facts(a, t, e, W > fs * 70).c_str());
    if (e.hasModel && !e.m.slices.empty()) {
      char b[32];
      std::snprintf(b, sizeof b, "%02d / %02d", e.selSlice + 1, static_cast<int>(e.m.slices.size()));
      if (const int d = stepper(a, "##sregion", "Region", b, ImVec2(sx, p0.y + 2), stW, titleH - 4, "The previous / next region (Left / Right)",
                                std::ceil(textW(a, "Region") + space::kGroup), true))
        selectRegion(a, t, e, v, e.selSlice - d, true);
      noteCtl("##sregion");
    }
    notePart("title", p0, ImVec2(p1.x, p0.y + titleH));
  }
  // ---- the well and the sequence rows
  const float wellY0 = p0.y + titleH + 1, wellY1 = p1.y - statusH - pH - 1;
  const float seqW = e.hasModel ? std::round(std::clamp(fs * 20.0f, 180.0f, W * 0.3f)) : 0.0f;
  const ImVec2 w0(p0.x, wellY0), w1(p1.x - (seqW > 0 ? seqW + 2 : 0), wellY1);
  // a song: the whole file above the waveform while the well has the room (More...: Whole song strip)
  const float stripH = std::round(fs * 2.4f);
  const bool strip = e.hasModel && isSong(e) && !e.stripHidden && !sui::recorderOpenFor(t) && (w1.y - w0.y) >= stripH + fs * 7;
  if (strip) {
    sui::drawStrip(a, t, e, w0, ImVec2(w1.x, w0.y + stripH), lv);
    notePart("strip", w0, ImVec2(w1.x, w0.y + stripH));
  }
  const ImVec2 wv0(w0.x, strip ? w0.y + stripH + 2 : w0.y);
  if (e.hasModel && !sui::recorderOpenFor(t)) wave(a, t, e, wv0, w1, lv);
  else wellState(a, t, e, w0, w1);
  notePart("well", w0, w1);
  if (seqW > 0) {
    seqRows(a, t, e, v, ImVec2(w1.x + 2, wellY0), ImVec2(p1.x, wellY1), lv);
    notePart("sequence", ImVec2(w1.x + 2, wellY0), ImVec2(p1.x, wellY1));
  }
  // ---- the button panel
  {
    const float y0 = wellY1 + 4;
    const float lw = labelColumn(a, v.stepScope ? std::initializer_list<const char*>{"Pitch", "Gain", "Start", "Length", "Stutter", "Plays", "Steps"}
                                                : std::initializer_list<const char*>{"Pitch", "Gain", "Fade in", "Fade out", "Start", "Length", "Choke"});
    std::vector<Column> cols = chopColumns(a, t, e, v, rows, lw, false);
    panel(a, cols, p0.x + 4, p1.x - 4, y0, rows, rowH, "##sseltmore", moreEntries(a, t, e, v));
    notePart("panel", ImVec2(p0.x, wellY1), ImVec2(p1.x, wellY1 + pH));
  }
  // ---- the status line
  {
    std::string text = a.hint;
    if (text.empty()) {
      if (e.hasModel && !e.m.slices.empty()) {
        const int k = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
        const Slice& s = e.m.slices[static_cast<size_t>(k)];
        char b[256];
        std::snprintf(b, sizeof b, "%s %s %.3f s  \xC2\xB7  %s %s  \xC2\xB7  %s  \xC2\xB7  %s  \xC2\xB7  %s  \xC2\xB7  %s  \xC2\xB7  %s", i18n::tr("Region"),
                      regionName(k).c_str(), static_cast<double>(s.end - s.start) / std::max(1.0, e.m.srcRate), i18n::tr("key"),
                      trkNote(a, sui::baseNote(a, t) + k).c_str(), i18n::tr("Left Right: region"), i18n::tr("Up Down: step"), i18n::tr("Ins split"),
                      i18n::tr("Backspace join"), i18n::tr("Alt+1..7 chop"));
        text = b;
      } else {
        text = i18n::tr("Drop a file on the waveform, or Load... or Record...");
      }
    }
    statusLine(a, ImVec2(p0.x, p1.y - statusH), p1, text, keys);
    notePart("status", ImVec2(p0.x, p1.y - statusH), p1);
  }
  sui::drawRegionMenu(a, t, e);
  modals(a, t, e, v, lv);
  ImGui::PopID();
}

void instrumentScreen(App& a, ObjectId ch, ImVec2 p0, ImVec2 p1, bool keys) {
  sui::Ed* ep = nullptr;
  LiveView lv;
  if (!begin(a, ch, ep, lv)) return;
  sui::Ed& e = *ep;
  const PluginTarget t = targetOf(ch);
  View& v = viewOf(ch);
  drainKeys(a, t, e, v, ScreenInstrument);
  gScreen = "instrument";
  gRows.clear();
  ImGui::PushID(static_cast<int>(ch) + 0x5E17);
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  const float titleH = std::round(fs + 8), rowH = std::round(fs + 7), statusH = std::round(fs + 6);
  const bool folded = H < fs * 17;
  const float pianoH = std::round(folded ? std::max(fs * 2.0f, H * 0.14f) : std::clamp(H * 0.2f, fs * 3.0f, fs * 5.5f));
  // ---- the title
  {
    bevelBox(dl, p0, ImVec2(p1.x, p0.y + titleH), false);
    const float ty = std::round(p0.y + (titleH - fs) * 0.5f);
    const char* title = i18n::tr("Seltmann instrument");
    dl->AddText(boldFont(a), fs, ImVec2(p0.x + 8, ty), P.text, title);
    const float x = p0.x + 16 + boldFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, title).x;
    std::string info = facts(a, t, e, false);
    if (e.hasModel && !e.m.slices.empty()) {
      const int base = sui::baseNote(a, t);
      info += std::string("  \xC2\xB7  ") + i18n::tr("base") + " " + trkNote(a, base) + "  \xC2\xB7  " + regionName(0) + "-" +
              regionName(static_cast<int>(e.m.slices.size()) - 1) + " " + i18n::tr("on") + " " + trkNote(a, base) + "-" +
              trkNote(a, base + static_cast<int>(e.m.slices.size()) - 1) + "  \xC2\xB7  " + i18n::tr("pattern on") + " " + trkNote(a, base - 12);
    }
    std::string lay = e.m.preset.empty() ? std::string(i18n::tr("your own")) : e.m.preset;
    const std::string right = std::string(i18n::tr("Layout")) + "  " + lay;
    const float rw = monoW(a, right.c_str());
    theme::textEllipsis(dl, monoFont(a), fs, ImVec2(x, ty), p1.x - x - rw - 24, valueInk(), info.c_str());
    dl->AddText(monoFont(a), fs, ImVec2(p1.x - rw - 8, ty), P.hot, right.c_str());
    notePart("title", p0, ImVec2(p1.x, p0.y + titleH));
  }
  const float by0 = p0.y + titleH + 2, by1 = p1.y - statusH - pianoH - 3;
  // three boxes: Sound, Layouts, Grid
  const float soundW = std::round(W * 0.33f), gridW = std::round(W * 0.27f);
  const ImVec2 s0(p0.x, by0), s1(p0.x + soundW, by1);
  const ImVec2 l0(s1.x + 2, by0), l1(p1.x - gridW - 2, by1);
  const ImVec2 g0(p1.x - gridW, by0), g1(p1.x, by1);
  const float pad = space::kPad + 2;
  // ---- Sound
  {
    bevelBox(dl, s0, s1, false);
    boxTitle(a, ImVec2(s0.x + pad, s0.y + 3), "Sound");
    const float x0 = s0.x + pad, x1 = s1.x - pad - std::round(fs * 0.9f) - 2;
    float y = s0.y + fs + 8;
    const int sync = static_cast<int>(std::lround(sui::paramOf(a, t, SmSync)));
    const int speed = static_cast<int>(std::lround(sui::paramOf(a, t, SmSpeed)));
    const int keymap = static_cast<int>(std::lround(sui::paramOf(a, t, SmKeyMap)));
    std::vector<Cell> rowsV;
    const float rl = std::ceil(std::max({textW(a, "Follow"), textW(a, "Speed"), textW(a, "Keys")}) + space::kGroup);
    auto radios = [&](const char* label, std::vector<Cell> parts) {
      Cell r = row(std::move(parts));
      Cell lab = sCaption(label);
      Cell full = row({lab, r});
      (void)rl;
      return full;
    };
    rowsV.push_back(radios(i18n::N_("Follow"), {sRadio("##sfstretch", "Stretch", "The regions follow the project's tempo, keeping their pitch", sync == SyncStretch,
                                            [&a, t] { pui::setParamValue(a, t, SmSync, SyncStretch); }),
                                      sRadio("##sfresample", "Resample", "The regions follow the tempo as a record would (the pitch moves with it)",
                                            sync == SyncResample, [&a, t] { pui::setParamValue(a, t, SmSync, SyncResample); }),
                                      sRadio("##sfoff", "Off", "The regions play at their own speed", sync == SyncOriginal,
                                            [&a, t] { pui::setParamValue(a, t, SmSync, SyncOriginal); })}));
    rowsV.push_back(radios(i18n::N_("Speed"), {sRadio("##sshalf", "Half", "The pattern at half speed", speed == SpeedHalf, [&a, t] { pui::setParamValue(a, t, SmSpeed, SpeedHalf); }),
                                     sRadio("##ssnormal", "Normal", "The pattern at normal speed", speed == SpeedNormal,
                                           [&a, t] { pui::setParamValue(a, t, SmSpeed, SpeedNormal); }),
                                     sRadio("##ssdouble", "Double", "The pattern at double speed", speed == SpeedDouble,
                                           [&a, t] { pui::setParamValue(a, t, SmSpeed, SpeedDouble); })}));
    rowsV.push_back(radios(i18n::N_("Keys"), {sRadio("##skregions", "Regions", "Keys from the base note play the regions in order", keymap == KeySlices,
                                          [&a, t] { pui::setParamValue(a, t, SmKeyMap, KeySlices); }),
                                    sRadio("##skletters", "Letters", "Keys from the base note play the letters of older layouts", keymap == KeySlots,
                                          [&a, t] { pui::setParamValue(a, t, SmKeyMap, KeySlots); })}));
    const bool lock = sui::paramOf(a, t, SmLock) > 0.5, mono = sui::paramOf(a, t, SmMono) > 0.5, revAll = sui::paramOf(a, t, SmReverse) > 0.5;
    rowsV.push_back(row({sCheck("##slock", "Lock to song", "The pattern follows the song's position", lock, [&a, t, lock] { pui::setParamValue(a, t, SmLock, lock ? 0 : 1); }),
                         sCheck("##smono", "Mono", "One region at a time", mono, [&a, t, mono] { pui::setParamValue(a, t, SmMono, mono ? 0 : 1); })}));
    rowsV.push_back(sCheck("##srevall", "Reverse every region", "Every region plays backwards", revAll, [&a, t, revAll] { pui::setParamValue(a, t, SmReverse, revAll ? 0 : 1); }));
    struct PRow {
      int param;
      const char* id;
      const char* label;
      const char* hint;
    };
    static const PRow prs[] = {{SmBaseNote, "##spbase", i18n::N_("Base note"), i18n::N_("The key region A plays (the pattern on the octave below)")},
                               {SmSwing, "##spswing", i18n::N_("Swing"), i18n::N_("Odd steps of the pattern later")},
                               {SmGate, "##spgate", i18n::N_("Gate"), i18n::N_("How much of each step sounds")},
                               {SmVelocity, "##spvel", i18n::N_("Velocity"), i18n::N_("How much the velocity sets the level")},
                               {SmPitch, "##sppitch", i18n::N_("Pitch"), i18n::N_("Every region's pitch (semitones)")},
                               {SmAttack, "##spatt", i18n::N_("Attack"), i18n::N_("The level's rise at each region's start")},
                               {SmDecay, "##spdec", i18n::N_("Decay"), i18n::N_("The fall to the sustain level (0: none)")},
                               {SmSustain, "##spsus", i18n::N_("Sustain"), i18n::N_("The level held")},
                               {SmRelease, "##sprel", i18n::N_("Release"), i18n::N_("The fall after the key is let go")},
                               {SmDeclick, "##spdeclick", i18n::N_("Declick"), i18n::N_("A short fade at every region's edges")},
                               {SmCutoff, "##spcut", i18n::N_("Cutoff"), i18n::N_("The low-pass filter's frequency")},
                               {SmReso, "##spres", i18n::N_("Resonance"), i18n::N_("The filter's resonance")},
                               {SmPan, "##sppan", i18n::N_("Pan"), i18n::N_("Left and right")},
                               {SmVolume, "##spvol", i18n::N_("Volume"), i18n::N_("The instrument's level")}};
    const PluginInfo* info = t.info(a.doc.song());
    std::vector<Cell> steppers;
    const float plw = labelColumn(a, {"Base note", "Swing", "Gate", "Velocity", "Pitch", "Attack", "Decay", "Sustain", "Release", "Declick", "Cutoff", "Resonance", "Pan", "Volume"});
    for (const PRow& pr : prs) {
      const double val = sui::paramOf(a, t, pr.param);
      const ParamDesc* d = info && pr.param < static_cast<int>(info->params.size()) ? &info->params[static_cast<size_t>(pr.param)] : nullptr;
      std::string txt;
      double stepSz = 1;
      switch (pr.param) {
        case SmBaseNote: txt = trkNote(a, static_cast<int>(std::lround(val))); break;
        case SmPitch: txt = fmt("%+.2f", val); stepSz = 1; break;
        case SmPan: txt = std::fabs(val) < 0.005 ? std::string("C") : fmt(val < 0 ? "L%.0f" : "R%.0f", std::fabs(val) * 100.0); stepSz = 0.02; break;
        case SmAttack:
        case SmDecay:
        case SmRelease: txt = fmt("%.0f ms", val); stepSz = 5; break;
        case SmDeclick: txt = fmt("%.1f ms", val); stepSz = 0.5; break;
        case SmCutoff: txt = val >= 1000 ? fmt("%.1fk", val / 1000.0) : fmt("%.0f", val); stepSz = 0; break;
        default: txt = fmt("%.0f %%", val * 100.0); stepSz = 0.01; break;
      }
      const double lo = d ? d->min : 0, hi = d ? d->max : 1;
      const int param = pr.param;
      Cell c = sStepper(pr.id, pr.label, txt, pr.hint, [&a, t, &v, param, val, stepSz, lo, hi](int dd) {
        double nv = val;
        if (stepSz == 0) nv = val * std::pow(2.0, dd / 12.0);  // (the cutoff: a semitone a step)
        else nv = val + dd * stepSz * (ImGui::GetIO().KeyShift ? 0.1 : 1.0);
        setParam(a, t, v, param, std::clamp(nv, lo, hi));
      });
      c.labelW = plw;
      steppers.push_back(std::move(c));
    }
    // two columns of steppers when the box is wide
    const float colLeast = cellLeast(a, steppers[0], rowH) + fs * 2;
    const int ncol = (x1 - x0) >= colLeast * 2 + 6 ? 2 : 1;
    for (size_t i = 0; i < steppers.size(); i += static_cast<size_t>(ncol)) {
      if (ncol == 2 && i + 1 < steppers.size()) rowsV.push_back(row({steppers[i], steppers[i + 1]}));
      else rowsV.push_back(steppers[i]);
    }
    const float rh = rowH + space::kLine;
    const int vis = std::max(1, static_cast<int>((s1.y - pad - y) / rh));
    const int nrows = static_cast<int>(rowsV.size());
    v.soundTop = std::clamp(v.soundTop, 0, std::max(0, nrows - vis));
    if (ImGui::IsMouseHoveringRect(s0, s1) && ImGui::GetIO().MouseWheel != 0)
      v.soundTop = std::clamp(v.soundTop - static_cast<int>(ImGui::GetIO().MouseWheel), 0, std::max(0, nrows - vis));
    for (int r = 0; r < vis && v.soundTop + r < nrows; ++r) {
      Cell& c = rowsV[static_cast<size_t>(v.soundTop + r)];
      const float yy = y + static_cast<float>(r) * rh;
      if (c.kind == CRow && !c.parts.empty() && c.parts[0].kind == CCaption) {
        // a label column, then its radios
        drawCell(a, c.parts[0], ImVec2(x0, yy), ImVec2(x0 + rl, yy + rowH), rowH);
        drawCell(a, c.parts[1], ImVec2(x0 + rl, yy), ImVec2(x1, yy + rowH), rowH);
      } else {
        drawCell(a, c, ImVec2(x0, yy), ImVec2(x1, yy + rowH), rowH);
      }
    }
    if (nrows > vis) {
      double top = v.soundTop;
      if (vScrollBar(a, "##ssoundsb", ImVec2(s1.x - pad - std::round(fs * 0.9f), y), ImVec2(s1.x - pad, s1.y - pad), &top, vis, nrows, 1, "Scroll the sound's values"))
        v.soundTop = static_cast<int>(std::lround(top));
    }
    notePart("sound", s0, s1);
  }
  // ---- Layouts
  {
    bevelBox(dl, l0, l1, false);
    boxTitle(a, ImVec2(l0.x + pad, l0.y + 3), "Layouts");
    {
      char b[64];
      sui::layoutGroup(a, e, 7, "");  // (loads the person's own)
      std::snprintf(b, sizeof b, "%zu + %zu %s", factoryPresets().size(), sui::layoutGroup(a, e, 7, "").size(), i18n::tr("of yours"));
      const float tx = l0.x + pad + boldFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, i18n::tr("Layouts")).x + fs;
      const float bw2 = a.fontRegular->CalcTextSizeA(std::round(fs * 0.85f), FLT_MAX, 0, b).x;
      dl->AddText(monoFont(a), std::round(fs * 0.85f), ImVec2(tx, l0.y + 4), P.hot, b);
      // the search: names and hints, in the group shown
      const float sx0 = std::max(tx + bw2 + fs, l1.x - pad - fs * 11), sx1 = l1.x - pad;
      if (sx1 - sx0 >= fs * 5) {
        ImGui::SetCursorScreenPos(ImVec2(sx0, l0.y + 2));
        ImGui::SetNextItemWidth(sx1 - sx0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 1));
        if (ImGui::InputTextWithHint("##slsearch", i18n::tr("Search layouts"), v.search, sizeof v.search)) v.cursor = 0, v.listTop = 0;
        ImGui::PopStyleVar();
        noteCtl("##slsearch");
      }
    }
    const float top = l0.y + fs + 8, bot = l1.y - pad;
    const int ng = sui::layoutGroupCount();
    // the groups as radio rows (a stepper when the box is short)
    float gw = 0;
    for (int g = 0; g < ng; ++g) gw = std::max(gw, textW(a, sui::layoutGroupName(g)));
    gw = std::ceil(gw + 2 * std::max(3.0f, std::round(fs * 0.3f)) + 12 + space::kPad);
    const float rh = rowH + space::kLine;
    const bool groupRadios = (bot - top) >= ng * rh;
    float lx0 = l0.x + pad;
    if (groupRadios) {
      for (int g = 0; g < ng; ++g) {
        char id[24];
        std::snprintf(id, sizeof id, "##sgroup%d", g);
        if (radioRow(a, id, sui::layoutGroupName(g), ImVec2(lx0, top + g * rh), ImVec2(lx0 + gw, top + g * rh + rowH), e.presetTab == g, sui::layoutGroupHint(g)))
          e.presetTab = g, v.cursor = 0, v.listTop = 0;
        noteCtl(id);
      }
      lx0 += gw + space::kGroup;
    }
    // the buttons at the right
    const float bw = std::ceil(std::max({textW(a, "Apply"), textW(a, "Hear (hold)"), textW(a, "Star"), textW(a, "New take"), textW(a, "Save as..."), textW(a, "Delete")}) +
                               2 * space::kPad + 2);
    const float bx1 = l1.x - pad, bx0 = bx1 - bw;
    const auto items = groupItems(a, e);
    const int ni = static_cast<int>(items.size());
    v.cursor = std::clamp(v.cursor, 0, std::max(0, ni - 1));
    const Preset* curP = ni > 0 ? items[static_cast<size_t>(v.cursor)] : nullptr;
    const bool custom = curP == &sui::customLayout();
    float lyTop = top;
    if (!groupRadios) {
      // a group stepper above the list
      if (const int d = stepper(a, "##sgroupst", "Group", i18n::tr(sui::layoutGroupName(e.presetTab)), ImVec2(lx0, top), bx0 - lx0 - space::kGroup, rowH,
                                "The layouts' group (Left / Right)", std::ceil(textW(a, "Group") + space::kGroup), true)) {
        e.presetTab = (e.presetTab - d + ng) % ng;
        v.cursor = 0, v.listTop = 0;
      }
      noteCtl("##sgroupst");
      lyTop = top + rowH + space::kGap;
    }
    struct B {
      const char* id;
      const char* label;
      const char* hint;
      bool enabled;
    };
    const B bs[] = {{"##slapply", i18n::N_("Apply"), i18n::N_("Use the layout (Enter); your own sequence stays under Recent"), curP != nullptr},
                    {"##slhear", i18n::N_("Hear (hold)"), i18n::N_("Hold to hear the layout on your chop (Alt+P)"), curP != nullptr && !custom},
                    {"##slstar", i18n::N_("Star"), i18n::N_("Star the layout: it shows under Starred (Alt+F)"), curP != nullptr && !custom},
                    {"##slnewtake", i18n::N_("New take"), i18n::N_("A generated layout rolls a new take"), curP != nullptr && !custom && curP->gen != 0},
                    {"##slsave", i18n::N_("Save as..."), i18n::N_("Save the chop and the sequence as your own layout"), e.hasModel},
                    {"##sldelete", i18n::N_("Delete"), i18n::N_("Delete your own layout"), curP != nullptr && !custom && curP->category == CatUser}};
    const int fitB = std::max(1, static_cast<int>((bot - top + space::kLine) / rh));
    const int nb = 6;
    const bool moreB = fitB < nb;
    const int shown = moreB ? fitB - 1 : nb;
    auto act = [&](int i) {
      switch (i) {
        case 0: applyCursor(a, t, e, v); break;
        case 2: starCursor(a, e, v); break;
        case 3:
          if (curP && curP->gen) {
            ++e.seed;
            sui::applyLayout(a, t, e, *curP, true);
          }
          break;
        case 4:
          v.modal = ModalSave;
          v.modalAsk = true;
          break;
        case 5:
          if (curP) sui::deleteUserLayout(a, e, curP->name);
          break;
        default: break;
      }
    };
    for (int i = 0; i < shown; ++i) {
      const float yy = top + i * rh;
      if (!bs[i].enabled) ImGui::BeginDisabled();
      const bool starOn = i == 2 && curP && sui::prefs(a, e).starred(curP->name);
      const bool c = bevelButton(a, bs[i].id, bs[i].label, ImVec2(bx0, yy), ImVec2(bx1, yy + rowH), starOn || (i == 1 && !e.previewName.empty()), bs[i].hint);
      noteCtl(bs[i].id);
      if (i == 1 && bs[i].enabled && curP) sui::layoutGesture(a, t, e, *curP, false, true);
      if (!bs[i].enabled) ImGui::EndDisabled();
      if (c && bs[i].enabled && i != 1) act(i);
    }
    if (moreB) {
      const float yy = top + shown * rh;
      if (bevelButton(a, "##slmore", "More...", ImVec2(bx0, yy), ImVec2(bx1, yy + rowH), false, "The layout's other actions")) {
        ImGui::OpenPopup("##slmoremenu");
        keepKeysHere();
      }
      noteCtl("##slmore");
      if (ImGui::BeginPopup("##slmoremenu")) {
        for (int i = shown; i < nb; ++i)
          if (i != 1 && ImGui::MenuItem(i18n::tr(bs[i].label), nullptr, false, bs[i].enabled)) act(i);
        ImGui::EndPopup();
      }
    }
    // the list: number, star, name, the regions it plays on this chop
    const ImVec2 q0(lx0, lyTop), q1(bx0 - space::kGroup, bot);
    bevelBox(dl, q0, q1, true);
    const float lrh = std::round(fs + 3);
    const float sbW = std::round(fs * 0.9f);
    const int vis = std::max(1, static_cast<int>((q1.y - q0.y - 4) / lrh));
    if (v.cursor < v.listTop) v.listTop = v.cursor;
    if (v.cursor >= v.listTop + vis) v.listTop = v.cursor - vis + 1;
    v.listTop = std::clamp(v.listTop, 0, std::max(0, ni - vis));
    ImFont* mono = monoFont(a);
    const float nameX = q0.x + 4 + monoW(a, "00") + fs * 1.6f;
    // the names are never cut short: the letters start after the widest name shown, and give way
    float nameW = 0;
    for (int r = 0; r < vis && v.listTop + r < ni; ++r)
      nameW = std::max(nameW, boldFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, items[static_cast<size_t>(v.listTop + r)]->name.c_str()).x);
    const float chipX = std::max(nameX + std::min(fs * 9.0f, (q1.x - nameX) * 0.5f), nameX + nameW + fs);
    dl->PushClipRect(ImVec2(q0.x + 2, q0.y + 2), ImVec2(q1.x - sbW - 2, q1.y - 2), true);
    for (int r = 0; r < vis && v.listTop + r < ni; ++r) {
      const int i = v.listTop + r;
      const Preset* p = items[static_cast<size_t>(i)];
      const float y = q0.y + 2 + r * lrh;
      const bool sel = i == v.cursor, used = p->name == e.m.preset;
      if (sel) dl->AddRectFilled(ImVec2(q0.x + 2, y), ImVec2(q1.x - sbW - 2, y + lrh), theme::withAlpha(P.hot, 0.28f));
      const float ty = y + std::round((lrh - fs) * 0.5f);
      dl->AddText(mono, fs, ImVec2(q0.x + 4, ty), P.textDim, trk::hex2(i + 1).c_str());
      const bool star = p != &sui::customLayout() && sui::prefs(a, e).starred(p->name);
      dl->AddText(a.fontRegular, fs, ImVec2(q0.x + 6 + monoW(a, "00"), ty), star ? P.hot : P.textFaint, star ? "*" : "\xC2\xB7");
      dl->AddText(used ? boldFont(a) : a.fontRegular, fs, ImVec2(nameX, ty), used ? P.text : P.textDim, p->name.c_str());
      if (p != &sui::customLayout()) {
        const std::vector<int>& regs = sui::layoutChips(e, *p);
        float cx = chipX;
        for (size_t c = 0; c < regs.size(); ++c) {
          const std::string L = regs[c] >= 0 ? regionName(regs[c]) : std::string("\xC2\xB7");
          const float lw2 = mono->CalcTextSizeA(fs, FLT_MAX, 0, L.c_str()).x;
          if (cx + lw2 > q1.x - sbW - 6) break;
          dl->AddText(mono, fs, ImVec2(cx, ty), regs[c] >= 0 ? sui::regionColor(regs[c]) : P.textFaint, L.c_str());
          cx += lw2 + monoW(a, " ");
        }
      }
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(q0.x + 2, q0.y + 2));
    ImGui::InvisibleButton("##slist", ImVec2(std::max(1.0f, q1.x - q0.x - sbW - 4), std::max(1.0f, q1.y - q0.y - 4)));
    noteCtl("##slist");
    const int under = v.listTop + static_cast<int>((ImGui::GetIO().MousePos.y - q0.y - 2) / lrh);
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0)
      v.listTop = std::clamp(v.listTop - static_cast<int>(ImGui::GetIO().MouseWheel) * 2, 0, std::max(0, ni - vis));
    if (ImGui::IsItemActivated() && under >= 0 && under < ni) v.cursor = under;
    if (under >= 0 && under < ni && items[static_cast<size_t>(under)] != &sui::customLayout() && (ImGui::IsItemActive() || ImGui::IsItemHovered()))
      sui::layoutGesture(a, t, e, *items[static_cast<size_t>(under)], false, false);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && under >= 0 && under < ni) {
      v.cursor = under;
      applyCursor(a, t, e, v);
    }
    double lt = v.listTop;
    if (vScrollBar(a, "##slistsb", ImVec2(q1.x - sbW - 1, q0.y + 2), ImVec2(q1.x - 2, q1.y - 2), &lt, vis, std::max(ni, vis), 1, "Scroll the layouts"))
      v.listTop = static_cast<int>(std::lround(lt));
    notePart("layouts", l0, l1);
  }
  // ---- Grid (tempo, nudge, meter, key, how sure; takes; the way out)
  {
    bevelBox(dl, g0, g1, false);
    boxTitle(a, ImVec2(g0.x + pad, g0.y + 3), "Grid");
    const float x0 = g0.x + pad, x1 = g1.x - pad;
    const float rh = rowH + space::kLine;
    const bool have = e.hasModel;
    const float glw = std::ceil(std::max({textW(a, "Tempo"), textW(a, "Key"), textW(a, "Meter"), textW(a, "Nudge")}) + space::kGroup);
    std::vector<Cell> rowsG;
    Cell tempo = sStepper("##sgtempo", "Tempo", fmt("%.2f", e.m.grid.bpm), "The tempo the analysis found (click to type; bar 1 stays)",
                             [&a, t, &e](int d) { setTempo(a, t, e, e.m.grid.bpm + d * (ImGui::GetIO().KeyShift ? 0.01 : 1.0)); },
                             [&a, t, &e](double x) { setTempo(a, t, e, x); }, have);
    tempo.labelW = glw;
    rowsG.push_back(tempo);
    rowsG.push_back(row({sButton("##sghalf", "/2", "Half the tempo: the analysis heard double time", [&a, t, &e] {
                           Model nm = e.m;
                           gridHalve(nm.grid);
                           gridEdit(a, t, e, nm, i18n::N_("Half tempo"));
                         }, false, have),
                         sButton("##sgdouble", "x2", "Double the tempo: the analysis heard half time", [&a, t, &e] {
                           Model nm = e.m;
                           gridDouble(nm.grid);
                           gridEdit(a, t, e, nm, i18n::N_("Double tempo"));
                         }, false, have),
                         sButton("##sgtap", "Tap", "Tap along on the beat; after four taps the tempo follows", [&a, t, &e] { tapTempoNow(a, t, e); }, false, have)}));
    {
      std::vector<Cell> nudges;
      static const double ms[4] = {-10, -1, 1, 10};
      static const char* const ids[4] = {"##sgn0", "##sgn1", "##sgn2", "##sgn3"};
      static const char* const lab[4] = {"-10", "-1", "+1", "+10"};
      static const char* const hints[4] = {i18n::N_("Move the grid 10 ms earlier"), i18n::N_("Move the grid 1 ms earlier"), i18n::N_("Move the grid 1 ms later"), i18n::N_("Move the grid 10 ms later")};
      for (int i = 0; i < 4; ++i) {
        const double m = ms[i];
        nudges.push_back(sButton(ids[i], lab[i], hints[i], [&a, t, &e, m] {
          Model nm = e.m;
          gridNudge(nm.grid, m * 0.001 * nm.srcRate);
          gridEdit(a, t, e, nm, i18n::N_("Nudge the grid"), 0x5E17AAull);
        }, false, have));
      }
      rowsG.push_back(row(std::move(nudges)));
    }
    {
      const Analysis& g = e.m.grid;
      const int cur = g.beatsPerBar == 3 ? 0 : (g.beatsPerBar == 2 && g.compound) ? 2 : 1;
      auto meter = [&a, t, &e](int which) {
        Model nm = e.m;
        gridSetMeter(nm.grid, which == 0 ? 3 : which == 2 ? 2 : 4);
        nm.grid.compound = which == 2;
        gridEdit(a, t, e, nm, i18n::N_("Set the meter"));
      };
      rowsG.push_back(row({sRadio("##sgm34", "3/4", "Three beats per bar", cur == 0, [meter] { meter(0); }),
                           sRadio("##sgm44", "4/4", "Four beats per bar", cur == 1, [meter] { meter(1); }),
                           sRadio("##sgm68", "6/8", "Two dotted beats per bar (compound)", cur == 2, [meter] { meter(2); })}));
      Cell key = sStepper("##sgkey", "Key", i18n::tr(sui::keyName(g.key, g.minor)), "The key the analysis heard (generated layouts use it)",
                             [&a, t, &e](int d) { stepKey(a, t, e, d); }, nullptr, have);
      key.labelW = glw;
      rowsG.push_back(key);
      char sure[96];
      std::snprintf(sure, sizeof sure, "%s %.0f %%  %s %.0f %%  %s %.0f %%", i18n::tr("Tempo"), g.confidence * 100, i18n::tr("Bar 1"), g.downbeatConfidence * 100,
                    i18n::tr("Meter"), g.meterConfidence * 100);
      Cell c;
      c.kind = CCaption;
      static std::string sureText;
      sureText = sure;
      c.label = sureText.c_str();
      rowsG.push_back(c);
    }
    rowsG.push_back(row({sButton("##sganalyse", "Analyse again", "Run the tempo, beat and section analysis again (keeps the layout)", [&e] { analyseAgain(e); }, false, have),
                         sButton("##sgbar1", "Bar 1 here", "Bar 1 of the grid at the selected region's start", [&a, t, &e] { bar1Here(a, t, e); }, false, have)}));
    // the takes: this instance's file and the takes recorded into it this session
    const float footY = g1.y - pad - rowH;
    float y = g0.y + fs + 8;
    const int fitRows = std::max(0, static_cast<int>((footY - space::kGap - y) / rh));
    // the takes list takes what the rows leave (at least two lines when there is room)
    const int lineH = static_cast<int>(std::round(fs + 3));
    const int takeLines = static_cast<int>(e.takes.size()) + (e.path.empty() ? 0 : 1);
    int showRows = static_cast<int>(rowsG.size());
    float takesH = 0;
    if (fitRows < showRows) showRows = fitRows;
    else {
      const float left = footY - space::kGap - (y + showRows * rh);
      takesH = left >= fs * 3 ? std::min(left, static_cast<float>(takeLines + 1) * lineH + 6) : 0.0f;
    }
    for (int r = 0; r < showRows; ++r) {
      const float yy = y + static_cast<float>(r) * rh;
      drawCell(a, rowsG[static_cast<size_t>(r)], ImVec2(x0, yy), ImVec2(x1, yy + rowH), rowH);
    }
    y += showRows * rh;
    if (takesH > 0) {
      const ImVec2 t0(x0, y + 2), t1(x1, y + 2 + takesH - 4);
      dl->AddText(a.fontRegular, std::round(fs * 0.85f), ImVec2(t0.x, t0.y), P.textDim, i18n::tr("Takes"));
      const ImVec2 k0(t0.x, t0.y + lineH), k1(t1.x, t1.y);
      bevelBox(dl, k0, k1, true);
      float ly = k0.y + 2;
      ImFont* mono = monoFont(a);
      std::vector<std::pair<std::string, double>> list;
      if (!e.path.empty()) {
        bool known = false;
        for (const auto& tk : e.takes) known = known || tk.path == e.path;
        if (!known) list.push_back({e.path, e.hasModel && e.m.srcRate > 0 ? static_cast<double>(e.m.srcFrames) / e.m.srcRate : 0.0});
      }
      for (const auto& tk : e.takes) list.push_back({tk.path, static_cast<double>(tk.frames) / std::max(1.0, tk.rate)});
      for (size_t i = 0; i < list.size() && ly + lineH <= k1.y; ++i) {
        const bool inUse = list[i].first == e.path;
        const std::string nm = trk::hex2(static_cast<int>(i) + 1) + " " + fs::u8path(list[i].first).filename().u8string();
        const std::string rt = sui::fmtTime(list[i].second) + (inUse ? std::string("  ") + i18n::tr("in use") : std::string());
        const float rw = mono->CalcTextSizeA(fs, FLT_MAX, 0, rt.c_str()).x;
        if (inUse) dl->AddRectFilled(ImVec2(k0.x + 2, ly), ImVec2(k1.x - 2, ly + lineH), theme::withAlpha(P.hot, 0.22f));
        theme::textEllipsis(dl, mono, fs, ImVec2(k0.x + 4, ly + 1), k1.x - k0.x - rw - 16, inUse ? P.text : P.textDim, nm.c_str());
        dl->AddText(mono, fs, ImVec2(k1.x - rw - 4, ly + 1), P.textFaint, rt.c_str());
        // a click on another take: use it, keeping the chop
        if (!inUse) {
          ImGui::SetCursorScreenPos(ImVec2(k0.x + 2, ly));
          ImGui::PushID(static_cast<int>(i));
          if (ImGui::InvisibleButton("##stakerow", ImVec2(std::max(1.0f, k1.x - k0.x - 4), static_cast<float>(lineH)))) {
            for (const auto& tk : e.takes)
              if (tk.path == list[i].first) sui::useTake(a, t, tk.path, true, tk.frames, tk.rate);
          }
          if (ImGui::IsItemHovered()) ui::Hint(i18n::tr("Use this take with the regions and the sequence as they are (Takes... to analyse it fresh)"));
          ImGui::PopID();
        }
        ly += lineH;
      }
    }
    // the foot: record a take, loops, takes, exit (the rows left out: Grid...)
    std::vector<Cell> foot;
    foot.push_back(sButton("##sgrec", "Record take", "Record a new take into this instance (the Chop screen shows the recorder)", [&a, t, ch] {
      sui::openRecorder(a, t);
      trackereditors::open(a, trackereditors::Editor::Sample, ch);
    }));
    foot.push_back(sButton("##sgloops", "Loops...", "The pattern, the best phrases and the sections to drag out", [&v] { openModalCard(v, sui::CardLoops); }, false, have));
    if (showRows < static_cast<int>(rowsG.size()) || takesH <= 0)
      foot.push_back(sButton("##sgmore", "Grid...", "Tap, nudge, meter, key, how sure, analyse again; the takes", [&v, showRows, &rowsG] {
        openModalCard(v, showRows < static_cast<int>(rowsG.size()) ? sui::CardGrid : sui::CardTakes);
      }, false, have));
    foot.push_back(sButton("##sgexit", "Exit", "Back to the pattern (Esc)", [&a] { trackereditors::close(a); }));
    Cell footRow = row(std::move(foot));
    // (a narrow box: the foot's least width decides; Loops... goes first)
    if (cellLeast(a, footRow, rowH) > x1 - x0 && footRow.parts.size() > 2) footRow.parts.erase(footRow.parts.begin() + 1);
    if (cellLeast(a, footRow, rowH) > x1 - x0 && footRow.parts.size() > 2) footRow.parts.erase(footRow.parts.begin());
    drawCell(a, footRow, ImVec2(x0, footY), ImVec2(x1, footY + rowH), rowH);
    notePart("grid", g0, g1);
  }
  // ---- the key map
  {
    const ImVec2 k0(p0.x, by1 + 1), k1(p1.x, p1.y - statusH - 1);
    keyMap(a, t, e, v, k0, k1, lv);
    notePart("keys", k0, k1);
  }
  // ---- the status line
  {
    std::string text = a.hint;
    if (text.empty())
      text = std::string(i18n::tr("Up Down: layout")) + "  \xC2\xB7  " + i18n::tr("Left Right: group") + "  \xC2\xB7  " + i18n::tr("Enter apply") + "  \xC2\xB7  " +
             i18n::tr("Alt+P hear") + "  \xC2\xB7  " + i18n::tr("Alt+F star") + "  \xC2\xB7  " + i18n::tr("Alt+Up Down base note") + "  \xC2\xB7  " +
             i18n::tr("the note keys play the regions");
    statusLine(a, ImVec2(p0.x, p1.y - statusH), p1, text, keys);
    notePart("status", ImVec2(p0.x, p1.y - statusH), p1);
  }
  (void)folded;
  modals(a, t, e, v, lv);
  ImGui::PopID();
}

// ================================================================== the Chop page

bool projectHasSeltmann(App& a) {
  for (const Channel& c : a.doc.song().channels)
    if (c.plugin.type == kType) return true;
  return false;
}

void chopPage(App& a) {
  const Song& s = a.doc.song();
  std::vector<ObjectId> chans;
  for (const Channel& c : s.channels)
    if (c.plugin.type == kType) chans.push_back(c.id);
  if (chans.empty()) {
    trackerpages::pageTitle(a, i18n::tr("Chop"), "");
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    const float fs = ImGui::GetFontSize();
    bevelBox(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, fs * 4)), true);
    theme::centreText(ImGui::GetWindowDrawList(), a.fontRegular, fs, p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, fs * 4)), pal().textDim,
                      i18n::tr("No Seltmann in the project yet: add one as an instrument to chop a loop, a break or a whole song"));
    return;
  }
  ObjectId ch = isSeltmann(a, a.selChannel) ? a.selChannel : chans.front();
  sui::Ed* ep = nullptr;
  LiveView lv;
  if (!begin(a, ch, ep, lv)) return;
  sui::Ed& e = *ep;
  const PluginTarget t = targetOf(ch);
  View& v = viewOf(ch);
  gScreen = "page";
  gRows.clear();
  ImGui::PushID(static_cast<int>(ch) + 0x5E18);
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  trackerpages::pageTitle(a, i18n::tr("Chop"), facts(a, t, e, true));
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  const ImVec2 p1(p0.x + av.x, p0.y + std::max(av.y, fs * 20));
  const float W = p1.x - p0.x;
  const float rowH = std::round(fs + 7), rh = rowH + space::kLine;
  // which Seltmann (several in the project): a stepper at the top right
  float topY = p0.y;
  if (chans.size() > 1) {
    const Channel* c = s.channel(ch);
    const int ci = s.channelIndex(ch);
    const std::string nm = trk::hex2(ci + trackBase(a)) + " " + (c ? c->name : std::string());
    const float sw = std::ceil(std::max(fs * 12, textW(a, "Seltmann") + monoW(a, nm.c_str()) + fs * 4));
    if (const int d = stepper(a, "##spagech", "Seltmann", nm, ImVec2(p1.x - sw, p0.y), sw, rowH, "The Seltmann this page shows (it becomes the current instrument)",
                              std::ceil(textW(a, "Seltmann") + space::kGroup), true)) {
      const auto it = std::find(chans.begin(), chans.end(), ch);
      const int idx = static_cast<int>(it - chans.begin());
      const int n = static_cast<int>(chans.size());
      a.selChannel = chans[static_cast<size_t>(((idx - d) % n + n) % n)];
    }
    noteCtl("##spagech");
    topY += rowH + space::kGap;
  }
  const float libW = std::round(std::clamp(fs * 17.0f, 180.0f, W * 0.22f));
  const float rightW = std::round(std::clamp(fs * 16.0f, 170.0f, W * 0.22f));
  const ImVec2 lib0(p0.x, topY), lib1(p0.x + libW, p1.y);
  const ImVec2 r0(p1.x - rightW, topY), r1(p1.x, p1.y);
  const float cx0 = lib1.x + 3, cx1 = r0.x - 3;
  // ---- the library (the layout groups, folded open; the list; apply, hear, star, save, new take)
  {
    bevelBox(dl, lib0, lib1, false);
    boxTitle(a, ImVec2(lib0.x + 6, lib0.y + 3), "Layouts");
    const float bAreaH = 2 * rh + 4;
    const ImVec2 q0(lib0.x + 4, lib0.y + fs + 8), q1(lib1.x - 4, lib1.y - bAreaH - 4);
    bevelBox(dl, q0, q1, true);
    // the rows: a heading per group, its layouts under it while open
    struct LRow {
      int group;
      const Preset* p;
    };
    std::vector<LRow> rowsL;
    for (int g = 0; g < sui::layoutGroupCount(); ++g) {
      rowsL.push_back({g, nullptr});
      if (!e.libOpen[g]) continue;
      if (g == 1 && e.hasCustom) rowsL.push_back({g, &sui::customLayout()});
      for (const Preset* p : sui::layoutGroup(a, e, g, "")) rowsL.push_back({g, p});
    }
    const float lrh = std::round(fs + 3), sbW = std::round(fs * 0.9f);
    const int vis = std::max(1, static_cast<int>((q1.y - q0.y - 4) / lrh));
    const int n = static_cast<int>(rowsL.size());
    int libTop = static_cast<int>(e.libScroll);
    libTop = std::clamp(libTop, 0, std::max(0, n - vis));
    ImFont* mono = monoFont(a);
    dl->PushClipRect(ImVec2(q0.x + 2, q0.y + 2), ImVec2(q1.x - sbW - 2, q1.y - 2), true);
    const std::vector<const Preset*> curItems = groupItems(a, e);
    const Preset* cur = v.cursor >= 0 && v.cursor < static_cast<int>(curItems.size()) ? curItems[static_cast<size_t>(v.cursor)] : nullptr;
    // the names are never cut short: the letters start after the widest name shown, and give way
    float nameW = 0;
    for (int r = 0; r < vis && libTop + r < n; ++r)
      if (const Preset* p = rowsL[static_cast<size_t>(libTop + r)].p) nameW = std::max(nameW, boldFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, p->name.c_str()).x);
    for (int r = 0; r < vis && libTop + r < n; ++r) {
      const LRow& lr = rowsL[static_cast<size_t>(libTop + r)];
      const float y = q0.y + 2 + r * lrh, ty = y + std::round((lrh - fs) * 0.5f);
      if (!lr.p) {
        const std::string h = std::string(e.libOpen[lr.group] ? "- " : "+ ") + i18n::tr(sui::layoutGroupName(lr.group));
        dl->AddText(boldFont(a), fs, ImVec2(q0.x + 4, ty), P.hot, h.c_str());
        continue;
      }
      const bool sel = lr.p == cur && lr.group == e.presetTab, used = lr.p->name == e.m.preset;
      if (sel) dl->AddRectFilled(ImVec2(q0.x + 2, y), ImVec2(q1.x - sbW - 2, y + lrh), theme::withAlpha(P.hot, 0.28f));
      const bool star = lr.p != &sui::customLayout() && sui::prefs(a, e).starred(lr.p->name);
      dl->AddText(a.fontRegular, fs, ImVec2(q0.x + 8, ty), star ? P.hot : P.textFaint, star ? "*" : "\xC2\xB7");
      const float nx = q0.x + 8 + fs;
      const float chipX = std::max(nx + (q1.x - sbW - nx) * 0.55f, nx + nameW + fs);
      dl->AddText(used ? boldFont(a) : a.fontRegular, fs, ImVec2(nx, ty), used ? P.text : P.textDim, lr.p->name.c_str());
      if (lr.p != &sui::customLayout()) {
        float cxx = chipX;
        for (int k : sui::layoutChips(e, *lr.p)) {
          const std::string L = k >= 0 ? regionName(k) : std::string("\xC2\xB7");
          const float lw2 = mono->CalcTextSizeA(fs, FLT_MAX, 0, L.c_str()).x;
          if (cxx + lw2 > q1.x - sbW - 4) break;
          dl->AddText(mono, fs, ImVec2(cxx, ty), k >= 0 ? sui::regionColor(k) : P.textFaint, L.c_str());
          cxx += lw2 + monoW(a, " ");
        }
      }
    }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(q0.x + 2, q0.y + 2));
    ImGui::InvisibleButton("##splib", ImVec2(std::max(1.0f, q1.x - q0.x - sbW - 4), std::max(1.0f, q1.y - q0.y - 4)));
    noteCtl("##splib");
    const int under = libTop + static_cast<int>((ImGui::GetIO().MousePos.y - q0.y - 2) / lrh);
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) libTop = std::clamp(libTop - static_cast<int>(ImGui::GetIO().MouseWheel) * 2, 0, std::max(0, n - vis));
    if (under >= 0 && under < n) {
      const LRow& lr = rowsL[static_cast<size_t>(under)];
      if (ImGui::IsItemActivated()) {
        if (!lr.p) {
          e.libOpen[lr.group] = !e.libOpen[lr.group];
        } else {
          e.presetTab = lr.group;
          const auto items = groupItems(a, e);
          for (size_t i = 0; i < items.size(); ++i)
            if (items[i] == lr.p) v.cursor = static_cast<int>(i);
        }
      }
      if (lr.p && lr.p != &sui::customLayout() && (ImGui::IsItemActive() || ImGui::IsItemHovered())) sui::layoutGesture(a, t, e, *lr.p, false, false);
      if (lr.p && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) applyCursor(a, t, e, v);
    }
    double lt = libTop;
    if (vScrollBar(a, "##splibsb", ImVec2(q1.x - sbW - 1, q0.y + 2), ImVec2(q1.x - 2, q1.y - 2), &lt, vis, std::max(n, vis), 1, "Scroll the layouts")) libTop = static_cast<int>(lt);
    e.libScroll = static_cast<float>(libTop);
    // the buttons under it
    const float bx0 = lib0.x + 4, bx1 = lib1.x - 4, by = lib1.y - bAreaH;
    const bool custom = cur == &sui::customLayout();
    Cell r1 = row({sButton("##spapply", "Apply", "Use the layout (a double-click too)", [&a, t, &e, &v] { applyCursor(a, t, e, v); }, false, cur != nullptr),
                   [&] {
                     Cell h = sButton("##sphear", "Hear", "Hold to hear the layout on your chop", nullptr, !e.previewName.empty(), cur != nullptr && !custom);
                     return h;
                   }(),
                   sButton("##spstar", "Star", "Star the layout: it shows under Starred", [&a, &e, &v] { starCursor(a, e, v); },
                          cur && !custom && sui::prefs(a, e).starred(cur->name), cur != nullptr && !custom)});
    Cell r2 = row({sButton("##spsave", "Save as...", "Save the chop and the sequence as your own layout", [&v] {
                     v.modal = ModalSave;
                     v.modalAsk = true;
                   }, false, e.hasModel),
                   sButton("##spnewtake", "New take", "A generated layout rolls a new take", [&a, t, &e, cur] {
                     if (cur && cur->gen) {
                       ++e.seed;
                       sui::applyLayout(a, t, e, *cur, true);
                     }
                   }, false, cur != nullptr && !custom && cur->gen != 0)});
    // (Hear: hold it to listen; drawn by hand so the hold reaches the gesture)
    {
      float least = 0;
      for (const Cell& q : r1.parts) least += cellLeast(a, q, rowH);
      const float extra = std::max(0.0f, (bx1 - bx0) - 2 * space::kGap - least);
      float x = bx0;
      for (size_t i = 0; i < r1.parts.size(); ++i) {
        Cell& q = r1.parts[i];
        const float lq = cellLeast(a, q, rowH);
        const float w = i + 1 == r1.parts.size() ? bx1 - x : std::floor(lq + extra * (lq / std::max(1.0f, least)));
        drawCell(a, q, ImVec2(x, by), ImVec2(x + w, by + rowH), rowH);
        if (i == 1 && q.enabled && cur && !custom) sui::layoutGesture(a, t, e, *cur, false, true);
        x += w + space::kGap;
      }
    }
    drawCell(a, r2, ImVec2(bx0, by + rh), ImVec2(bx1, by + rh + rowH), rowH);
    notePart("library", lib0, lib1);
  }
  // ---- the right column: the sequence rows, takes, loops
  {
    const float h = r1.y - r0.y;
    const ImVec2 sq0(r0.x, r0.y), sq1(r1.x, r0.y + std::round(h * 0.56f));
    bevelBox(dl, sq0, sq1, false);
    boxTitle(a, ImVec2(sq0.x + 6, sq0.y + 3), "Sequence");
    if (e.hasModel) seqRows(a, t, e, v, ImVec2(sq0.x + 3, sq0.y + fs + 6), ImVec2(sq1.x - 3, sq1.y - rowH - 6), lv);
    {
      const float by = sq1.y - rowH - 3;
      Cell btns = row({sButton("##spaddstep", "Add step", "A step at the end (the next region)", [&a, t, &e] {
                         if (e.m.pattern.steps.size() >= static_cast<size_t>(kMaxSteps) || e.m.slices.empty()) return;
                         Model nm = e.m;
                         Step st;
                         st.slot = false;
                         st.src = static_cast<int>(nm.pattern.steps.size()) % std::max(1, static_cast<int>(nm.slices.size()));
                         nm.pattern.steps.push_back(st);
                         sui::markEdited(nm);
                         sui::commit(a, t, e, nm, i18n::N_("Add a step"));
                       }, false, e.hasModel),
                       sButton("##sprest", "Rest", "The step becomes a rest (again: it plays the region)", [&a, t, &e] { restStep(a, t, e); }, false,
                              e.hasModel && curStep(e) >= 0),
                       sButton("##spreset", "Reset", "Every region once, in order (A B C D ...)", [&a, t, &e] { resetOrder(a, t, e); }, false,
                              e.hasModel && !isNaturalOrder(e.m))});
      drawCell(a, btns, ImVec2(sq0.x + 4, by), ImVec2(sq1.x - 4, by + rowH), rowH);
    }
    notePart("sequence", sq0, sq1);
    // takes
    const ImVec2 tk0(r0.x, sq1.y + 3), tk1(r1.x, sq1.y + 3 + std::round(h * 0.17f));
    bevelBox(dl, tk0, tk1, false);
    boxTitle(a, ImVec2(tk0.x + 6, tk0.y + 3), "Takes");
    {
      ImFont* mono = monoFont(a);
      float ly = tk0.y + fs + 8;
      const float lineH = std::round(fs + 3);
      std::vector<std::pair<std::string, double>> list;
      if (!e.path.empty()) {
        bool known = false;
        for (const auto& tk : e.takes) known = known || tk.path == e.path;
        if (!known) list.push_back({e.path, e.hasModel && e.m.srcRate > 0 ? static_cast<double>(e.m.srcFrames) / e.m.srcRate : 0.0});
      }
      for (const auto& tk : e.takes) list.push_back({tk.path, static_cast<double>(tk.frames) / std::max(1.0, tk.rate)});
      for (size_t i = 0; i < list.size() && ly + lineH <= tk1.y - 3; ++i) {
        const bool inUse = list[i].first == e.path;
        const std::string nm = trk::hex2(static_cast<int>(i) + 1) + " " + fs::u8path(list[i].first).filename().u8string();
        if (inUse) dl->AddRectFilled(ImVec2(tk0.x + 3, ly), ImVec2(tk1.x - 3, ly + lineH), theme::withAlpha(P.hot, 0.22f));
        theme::textEllipsis(dl, mono, fs, ImVec2(tk0.x + 6, ly + 1), tk1.x - tk0.x - 12, inUse ? P.text : P.textDim, nm.c_str());
        if (!inUse) {
          ImGui::SetCursorScreenPos(ImVec2(tk0.x + 3, ly));
          ImGui::PushID(static_cast<int>(i) + 900);
          if (ImGui::InvisibleButton("##sptake", ImVec2(std::max(1.0f, tk1.x - tk0.x - 6), lineH)))
            for (const auto& tk : e.takes)
              if (tk.path == list[i].first) sui::useTake(a, t, tk.path, true, tk.frames, tk.rate);
          if (ImGui::IsItemHovered()) ui::Hint(i18n::tr("Use this take with the regions and the sequence as they are"));
          ImGui::PopID();
        }
        ly += lineH;
      }
    }
    notePart("takes", tk0, tk1);
    // loops: the pattern, the phrases and the sections (drag one out; Loops... for the whole list)
    const ImVec2 lp0(r0.x, tk1.y + 3), lp1 = r1;
    bevelBox(dl, lp0, lp1, false);
    boxTitle(a, ImVec2(lp0.x + 6, lp0.y + 3), "Loops");
    {
      std::vector<std::pair<std::string, std::string>> items;  // (item, label)
      items.push_back({"pattern", std::string(i18n::tr("Pattern"))});
      const int bpb = std::max(1, e.m.grid.beatsPerBar);
      for (const auto& l : e.m.grid.loops) {
        char it[64], lab[64];
        std::snprintf(it, sizeof it, "span:%d:%d", l.firstBeat, l.firstBeat + l.bars * bpb);
        std::snprintf(lab, sizeof lab, "%s %d  (%d %s)", i18n::tr("Phrase at bar"), (l.firstBeat - e.m.grid.bar1) / bpb + 1, l.bars, l.bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
        items.push_back({it, lab});
      }
      for (size_t i = 0; i < e.m.grid.sections.size(); ++i) {
        const auto& sct = e.m.grid.sections[i];
        char it[64], lab[64];
        std::snprintf(it, sizeof it, "span:%d:%d", sct.firstBeat, sct.endBeat);
        std::snprintf(lab, sizeof lab, "%s %s%zu", i18n::tr("Section"), slotName(sct.label), i + 1);
        items.push_back({it, lab});
      }
      float ly = lp0.y + fs + 8;
      const float lineH = rowH;
      for (size_t i = 0; i < items.size() && ly + lineH <= lp1.y - rowH - 6; ++i) {
        ImGui::PushID(static_cast<int>(i) + 700);
        bevelButton(a, "##sploop", items[i].second.c_str(), ImVec2(lp0.x + 4, ly), ImVec2(lp1.x - 4, ly + lineH - 1), false,
                    "Drag it into the playlist (audio clip), the channel rack (sampler) or the browser");
        if (e.hasModel) sui::dragOut(a, t, e, items[i].first, items[i].second);
        ImGui::PopID();
        ly += lineH + space::kLine;
      }
      Cell lb = sButton("##sploops", "Loops...", "Every loop to hear and drag out", [&v] { openModalCard(v, sui::CardLoops); }, false, e.hasModel);
      drawCell(a, lb, ImVec2(lp0.x + 4, lp1.y - rowH - 4), ImVec2(lp1.x - 4, lp1.y - 4), rowH);
    }
    notePart("loops", lp0, lp1);
  }
  // ---- the centre: the waveform and the sequence blocks on one time line, the region box, the
  // panel, the key map
  {
    const float H = p1.y - topY;
    const float keyH = std::round(std::clamp(H * 0.13f, fs * 2.5f, fs * 5.0f));
    const float panelH = 2 * rowH + space::kGap + 6;
    const float valuesH = 5 * rh + fs + 10;
    const float seqH = std::round(fs * 2.2f);
    const float waveH = std::max(fs * 5, H - keyH - panelH - valuesH - seqH - 16);
    float y = topY;
    const float stripH = std::round(fs * 2.4f);
    if (e.hasModel && isSong(e) && !e.stripHidden && !sui::recorderOpenFor(t) && waveH >= stripH + fs * 6) {
      sui::drawStrip(a, t, e, ImVec2(cx0, y), ImVec2(cx1, y + stripH), lv);
      notePart("strip", ImVec2(cx0, y), ImVec2(cx1, y + stripH));
      y += stripH + 2;
    }
    const ImVec2 wv0(cx0, y), wv1(cx1, topY + waveH);
    if (e.hasModel && !sui::recorderOpenFor(t)) {
      wave(a, t, e, wv0, wv1, lv);
      sui::drawSequence(a, t, e, ImVec2(cx0, wv1.y + 2), ImVec2(cx1, wv1.y + 2 + seqH), lv);
      if (e.card == sui::CardRegion || e.card == sui::CardStep) sui::closeCard(e);
    } else {
      wellState(a, t, e, wv0, ImVec2(cx1, wv1.y + 2 + seqH));
    }
    notePart("wave", wv0, ImVec2(cx1, wv1.y + 2 + seqH));
    y = wv1.y + 4 + seqH;
    // the region (or step) box
    const ImVec2 rb0(cx0, y), rb1(cx1, y + valuesH);
    bevelBox(dl, rb0, rb1, false);
    if (e.hasModel && !e.m.slices.empty()) {
      const int k = std::clamp(e.selSlice, 0, static_cast<int>(e.m.slices.size()) - 1);
      const Slice& sl = e.m.slices[static_cast<size_t>(k)];
      char b[160];
      std::snprintf(b, sizeof b, "%s %s", v.stepScope ? i18n::tr("Step") : i18n::tr("Region"), v.stepScope && curStep(e) >= 0 ? trk::hex2(curStep(e)).c_str() : regionName(k).c_str());
      dl->AddText(boldFont(a), fs, ImVec2(rb0.x + 6, rb0.y + 3), P.text, b);
      std::snprintf(b, sizeof b, "%.3f s  \xC2\xB7  %s %s", static_cast<double>(sl.end - sl.start) / std::max(1.0, e.m.srcRate), i18n::tr("key"),
                    trkNote(a, sui::baseNote(a, t) + k).c_str());
      dl->AddText(monoFont(a), std::round(fs * 0.85f), ImVec2(rb0.x + 12 + boldFont(a)->CalcTextSizeA(fs, FLT_MAX, 0, "Region XX").x, rb0.y + 4), P.hot, b);
      const float lw = labelColumn(a, {"Pitch", "Gain", "Fade in", "Fade out", "Start", "Length", "Choke", "Stutter", "Plays", "Steps"});
      std::vector<Cell> vals = valueCells(a, t, e, v, lw);
      const float cy = rb0.y + fs + 8;
      const float colsW = rb1.x - rb0.x - 12;
      const float btnW = std::min(colsW * 0.36f, std::ceil(std::max({textW(a, "Split 2"), textW(a, "Start here"), textW(a, "Duplicate")}) * 2 + 8 * space::kPad));
      const float valW = std::floor((colsW - btnW - 8) / 2);
      const int per = 4;
      for (size_t i = 0; i < vals.size(); ++i) {
        const float x = rb0.x + 6 + static_cast<float>(i / per) * (valW + 4), yy = cy + static_cast<float>(i % per) * rh;
        if (i / per >= 2) break;
        drawCell(a, vals[i], ImVec2(x, yy), ImVec2(x + valW, yy + rowH), rowH);
      }
      // the scope and the region's own edits
      const float bx0 = rb1.x - 6 - btnW, bx1 = rb1.x - 6;
      Cell scope = scopeRow(v, e);
      drawCell(a, scope, ImVec2(bx0, rb0.y + 3), ImVec2(bx1, rb0.y + 3 + rowH - 2), rowH);
      std::vector<Cell> eds = {
          row({sButton("##spsplit2", "Split 2", "Split the region in two", [&a, t, &e] { sui::splitRegionUi(a, t, e, e.selSlice, 2); }),
               sButton("##spsplit3", "Split 3", "Split the region in three", [&a, t, &e] { sui::splitRegionUi(a, t, e, e.selSlice, 3); })}),
          row({sButton("##spsplit4", "Split 4", "Split the region in four", [&a, t, &e] { sui::splitRegionUi(a, t, e, e.selSlice, 4); }),
               sButton("##spsplit8", "Split 8", "Split the region in eight", [&a, t, &e] { sui::splitRegionUi(a, t, e, e.selSlice, 8); })}),
          row({sButton("##spjoin", "Join", "Join the region with the one before it", [&a, t, &e] { sui::deleteRegionUi(a, t, e, e.selSlice); }, false, e.m.slices.size() > 1),
               sButton("##spdup", "Duplicate", "Duplicate the step", [&a, t, &e] { duplicateStep(a, t, e); }, false, curStep(e) >= 0)}),
          row({sButton("##spstart", "Start here", "The sequence starts on this step", [&a, t, &e] {
                 const int s2 = curStep(e);
                 if (s2 > 0) sui::startHereUi(a, t, e, s2, true);
               }, false, curStep(e) > 0),
               sButton("##spdel", "Delete", "Delete the step", [&a, t, &e] { deleteStep(a, t, e); }, false, curStep(e) >= 0 && e.m.pattern.steps.size() > 1)}),
      };
      for (size_t i = 0; i < eds.size(); ++i) drawCell(a, eds[i], ImVec2(bx0, cy + i * rh), ImVec2(bx1, cy + i * rh + rowH), rowH);
      // the region as audio: drag it out
      {
        char lab[32];
        std::snprintf(lab, sizeof lab, "region %s", regionName(k).c_str());
        const std::string item = "slice:" + std::to_string(k), label = lab;
        Cell wav = sDrag("##spwav", "WAV", "Drag the region out as audio: into the playlist, the channel rack or the browser",
                            [&a, t, &e, item, label] { sui::dragOut(a, t, e, item, label); });
        const float ww = cellLeast(a, wav, rowH);
        drawCell(a, wav, ImVec2(bx0 - ww - 6, rb0.y + 3), ImVec2(bx0 - 6, rb0.y + 3 + rowH - 2), rowH);
      }
    }
    notePart("region", rb0, rb1);
    y = rb1.y + 4;
    // the panel (two rows) and the key map
    {
      const float lw = labelColumn(a, {"Pitch", "Gain"});
      std::vector<Column> cols = chopColumns(a, t, e, v, 2, lw, true);
      // (the page's region box has the values: the panel keeps the play, chop, tool, drags and tempo)
      for (Column& c : cols)
        for (Cell& cell : c.cells)
          if (cell.kind == CStepper && (std::strcmp(cell.id, "##svpit") == 0 || std::strcmp(cell.id, "##svgain") == 0)) cell = sCaption("");
      panel(a, cols, cx0, cx1, y, 2, rowH, "##spagemore", moreEntries(a, t, e, v));
      notePart("panel", ImVec2(cx0, y), ImVec2(cx1, y + panelH));
      y += panelH;
    }
    const ImVec2 k0(cx0, std::max(y, p1.y - keyH)), k1(cx1, p1.y);
    keyMap(a, t, e, v, k0, k1, lv);
    notePart("keys", k0, k1);
  }
  ImGui::SetCursorScreenPos(p0);
  ImGui::Dummy(ImVec2(W, p1.y - p0.y));
  sui::drawRegionMenu(a, t, e);
  modals(a, t, e, v, lv);
  ImGui::PopID();
}

// ================================================================== the pattern's cells (T7)

std::string noteLabel(App& a, ObjectId channel, int midi) {
  const Channel* c = channel == kNoId ? nullptr : a.doc.song().channel(channel);
  if (!c || c->plugin.type != kType) return "";
  // (once a frame per channel: the pattern asks for every cell it draws)
  struct Cached {
    int frame = -1;
    std::vector<std::string> names;
  };
  static std::map<ObjectId, Cached> cache;
  Cached& k = cache[channel];
  if (k.frame != ImGui::GetFrameCount()) k.frame = ImGui::GetFrameCount(), k.names = instanceKeyNames(c->plugin);
  const std::vector<std::string>& names = k.names;
  if (midi < 0 || midi >= static_cast<int>(names.size())) return "";
  const std::string& n = names[static_cast<size_t>(midi)];
  if (n.empty()) return "";
  if (n == "Pattern") return "P";
  if (n.rfind("Pattern ", 0) == 0) return n.substr(8);  // "+1" .. "+11"
  return n;
}

// ================================================================== dev

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  const std::string cmd = args.size() > 2 ? args[2] : "dump";
  const ObjectId ch = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
  if (!isSeltmann(a, ch) && cmd != "at") {
    std::printf("tracker selt frame %d: not a seltmann\n", frame);
    std::fflush(stdout);
    return;
  }
  const PluginTarget t = targetOf(ch);
  sui::Ed& e = sui::ed(t);
  View& v = viewOf(ch);
  auto num = [&](size_t i, int d) { return args.size() > i ? std::atoi(args[i].c_str()) : d; };
  if (cmd == "region") {
    selectRegion(a, t, e, v, num(3, 0), false);
  } else if (cmd == "step") {
    selectStep(a, t, e, v, num(3, 0), false);
  } else if (cmd == "tool") {
    const std::string w = args.size() > 3 ? args[3] : "play";
    e.tool = w == "cut" ? ToolCut : w == "grid" ? ToolGrid : ToolPlay;
  } else if (cmd == "group") {
    e.presetTab = std::clamp(num(3, 2), 0, sui::layoutGroupCount() - 1);
    v.cursor = 0;
  } else if (cmd == "layout" && args.size() > 3) {
    std::string name = args[3];
    for (size_t i = 4; i < args.size(); ++i) name += " " + args[i];
    for (int g = 0; g < sui::layoutGroupCount(); ++g) {
      e.presetTab = g;
      const auto items = groupItems(a, e);
      bool found = false;
      for (size_t i = 0; i < items.size(); ++i)
        if (items[i]->name == name) v.cursor = static_cast<int>(i), found = true;
      if (found) break;
    }
  } else if (cmd == "at" && args.size() > 3) {
    std::string name = args[3];
    if (name.rfind("##", 0) == 0) name = name.substr(2);
    const std::vector<Ctl>& list = gCtlFrame == ImGui::GetFrameCount() ? gCtlsDrawn : gCtls;
    for (const std::vector<Ctl>* l : {static_cast<const std::vector<Ctl>*>(&gCtls), &list})
      for (const Ctl& c : *l)
        if (c.name == name) {
          gDevPt = ImVec2(std::round((c.p0.x + c.p1.x) * 0.5f), std::round((c.p0.y + c.p1.y) * 0.5f));
          gDevPtSet = true;
        }
    std::printf("tracker selt frame %d: at %s %s %.0f %.0f\n", frame, name.c_str(), gDevPtSet ? "found" : "missing", gDevPt.x, gDevPt.y);
    std::fflush(stdout);
    return;
  }
  // the dump
  std::string steps;
  for (size_t i = 0; i < e.m.pattern.steps.size() && i < 64; ++i) {
    const int r = stepRegion(e.m, static_cast<int>(i));
    steps += (i ? " " : "") + (r >= 0 ? regionName(r) : std::string("-"));
    const Step& s = e.m.pattern.steps[i];
    if (s.reverse) steps += ":r";
    if (std::fabs(s.pitch) >= 0.005f) steps += ":p" + fmt("%g", s.pitch);
  }
  std::string ctls;
  const std::vector<Ctl>& list = gCtls.empty() ? gCtlsDrawn : gCtls;
  for (const Ctl& c : list) ctls += " " + c.name + "@" + std::to_string(static_cast<int>(std::lround((c.p0.x + c.p1.x) * 0.5f))) + "," +
                                    std::to_string(static_cast<int>(std::lround((c.p0.y + c.p1.y) * 0.5f)));
  const auto items = groupItems(a, e);
  const std::string cur = v.cursor >= 0 && v.cursor < static_cast<int>(items.size()) ? items[static_cast<size_t>(v.cursor)]->name : "";
  const int k = std::clamp(e.selSlice, 0, std::max(0, static_cast<int>(e.m.slices.size()) - 1));
  const float pitch = e.m.slices.empty() ? 0.0f : e.m.slices[static_cast<size_t>(k)].pitch;
  LinkState ls = LinkNone;
  const ObjectId lp = sui::linkedPatternOf(a, t, &ls);
  const PluginState* sp = t.state(a.doc.song());
  std::printf("tracker selt frame %d: screen %s ready %d regions %zu steps %zu region %d step %d scope %s tool %d pitch %.2f chop %d preset \"%s\" group %d cursor \"%s\" "
              "playing %d linked %d recorder %d modal %d takes %zu base %d undo %zu \"%s\" bytes %zu\n",
              frame, gScreen.c_str(), e.hasModel && !(e.job && e.job->state == 0) ? 1 : 0, e.m.slices.size(), e.m.pattern.steps.size(), e.selSlice, e.selStep,
              v.stepScope ? "step" : "region", e.tool, static_cast<double>(pitch), sui::chopSizeOf(e.m), e.m.preset.c_str(), e.presetTab, cur.c_str(),
              e.pattern.on ? 1 : 0, lp != kNoId ? a.doc.song().patternIndex(lp) + 1 : 0, sui::recorderOpenFor(t) ? 1 : 0, v.modal, e.takes.size(),
              sui::baseNote(a, t), a.doc.history().position(), a.doc.history().position() > 0 ? a.doc.history().undoLabel().c_str() : "", sp ? sp->data.size() : 0);
  std::printf("tracker selt order: %s\n", steps.c_str());
  std::printf("tracker selt parts: %s\n", gRows.c_str());
  std::printf("tracker selt controls:%s\n", ctls.c_str());
  std::fflush(stdout);
}

bool takeDevPointer(float* x, float* y) {
  if (!gDevPtSet) return false;
  gDevPtSet = false;
  *x = gDevPt.x, *y = gDevPt.y;
  return true;
}

}  // namespace gl::packs::seltmann::tracker
