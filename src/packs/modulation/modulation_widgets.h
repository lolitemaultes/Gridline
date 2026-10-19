// Gridline — "modulation" pack: drawing helpers shared by the pack's editors
// (knobs and buttons addressed by ParamId, so the same editor code serves
// channel instruments, mixer slots and note processors).
#pragma once
#include <cmath>
#include <functional>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"

namespace gl::packs::modulation {

// What an editor edits: a parameter address per index, the current state and
// a way to replace the whole state (one undo step; mergeKey folds drags).
struct EditTarget {
  App* a = nullptr;
  std::function<ParamId(int)> pid;
  std::function<void(const PluginState&, const std::string&, uint64_t)> set;
  const PluginState* st = nullptr;
  ObjectId owner = kNoId;  // channel or mixer track (controller outputs, live state)
  int slot = -1;           // mixer slot, -1 = channel instrument
  int noteFx = -1;         // note processor index (>= 0 for note processors)
  uint64_t key = 0;        // unique per edited plugin (UI state, merge keys)
  float width = 600;
  float p(int k, float dflt = 0) const {
    return st && static_cast<size_t>(k) < st->params.size() ? st->params[static_cast<size_t>(k)] : dflt;
  }
  int ip(int k, int dflt = 0) const { return static_cast<int>(std::lround(p(k, static_cast<float>(dflt)))); }
  void setParam(int k, double v, bool merge = true) const { a->doc.setParam(pid(k), v, merge); }
  float output(int k) const;  // controller output (0 when not running)
};
EditTarget targetFor(App& a, const PluginTarget& t, float width);
EditTarget noteFxTarget(App& a, ObjectId channel, int index, float width);

namespace w {
// Knob with value and label in a fixed-width cell.
void knob(const EditTarget& e, int param, const char* label = nullptr, float cell = 62, float radius = 15);
// Segmented buttons for a choice (or toggle) parameter; `labels` overrides the choice names.
void choice(const EditTarget& e, int param, float width = 0, const std::vector<std::string>* labels = nullptr);
// Dropdown for a choice parameter.
void combo(const EditTarget& e, int param, float width);
// Pill button for a toggle parameter.
void toggle(const EditTarget& e, int param, const char* label = nullptr);
// Small-caps caption.
void caption(const char* text);
// Integer/continuous value as a draggable number box (e.g. note numbers).
void dragValue(const EditTarget& e, int param, float width, const char* id);

// Rolling history of values (per key and lane) for monitor graphs.
void pushHistory(uint64_t key, int lane, float v);
void drawHistory(ImDrawList* dl, ImVec2 p0, ImVec2 size, uint64_t key, int lane, ImU32 col, float thickness);
// Framed dark area used behind graphs.
void well(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
// Piano keyboard over keys [lo, hi]; `tint` gives an optional overlay per key
// (0 = none). Returns the key under the mouse or -1.
int keyboard(ImDrawList* dl, ImVec2 p0, ImVec2 size, int lo, int hi, const std::function<ImU32(int)>& tint);
// Bar lane editing parameters [first, first + count) with stride `stride`
// (drag across to draw). `bipolar` draws from the centre. `highlight` marks one bar.
void barLane(const EditTarget& e, const char* id, int first, int count, int stride, ImVec2 size, bool bipolar, int highlight,
             const char* const* labels = nullptr);
// Name of a MIDI note ("C4").
std::string noteName(int n);
}  // namespace w

}  // namespace gl::packs::modulation
