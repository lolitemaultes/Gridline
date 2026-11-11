// Gridline — meter changes in the interface (see MeterUi.h).
#include "MeterUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "../core/I18n.h"
#include "../core/MeterEdit.h"
#include "../core/MenuOpsExtra.h"
#include "App.h"
#include "DialogLayout.h"
#include "J31Ui.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::meterui {

namespace {

// The editor: which change it edits, by position (indexes move when markers sort).
struct Editor {
  bool open = false;
  bool base = false;  // the song's meter at the start
  Tick pos = 0;
  bool place = false;  // place the window by the pointer on the next frame
  ImVec2 at{0, 0};
  bool alsoPatterns = false;
  char name[128] = {};
  Tick nameFor = -1;
};
Editor ed;

// Lane drag state.
struct Drag {
  bool active = false;
  int index = -1;
  float x0 = 0;
  bool moved = false;
  Tick to = 0;
};
Drag drag;
int menuIndex = -1;

int indexAtPos(const Song& s, Tick pos) { return meter::changeExactlyAt(s, pos); }

std::string label(int num, int den) { return meter::text(num, den); }

}  // namespace

float laneHeight(const Song& s) { return meter::changeCount(s) > 0 ? kLaneH : 0.0f; }

void openEditor(App& a, int index) {
  const Song& s = a.doc.song();
  ed = Editor{};
  ed.open = true;
  ed.place = true;
  ed.at = ImGui::GetIO().MousePos;
  if (index < 0 || index >= static_cast<int>(s.markers.size()) || !s.markers[static_cast<size_t>(index)].isMeter()) {
    ed.base = true;
    ed.pos = 0;
  } else {
    ed.pos = s.markers[static_cast<size_t>(index)].pos;
  }
}

void addAndEdit(App& a, Tick pos) {
  const Song& s = a.doc.song();
  const MeterMap map(s);
  const Tick at = std::max<Tick>(0, map.barStartAt(std::max<Tick>(0, pos)));
  if (at == 0) {
    openEditor(a, -1);
    return;
  }
  int num = 0, den = 0;
  meter::meterAt(s, at, num, den);
  int i = indexAtPos(s, at);
  if (i < 0) i = meter::add(a.doc, at, num, den);
  openEditor(a, i);
}

bool lane(App& a, ImDrawList* dl, const MeterMap& map, ImVec2 p0, ImVec2 p1, float originX, float pxPerTick, bool hovered) {
  const auto& P = theme::pal();
  const Song& s = a.doc.song();
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 m = io.MousePos;
  const bool in = hovered && m.x >= p0.x && m.x < p1.x && m.y >= p0.y && m.y < p1.y;
  auto xOf = [&](Tick t) { return std::round(originX + static_cast<float>(t) * pxPerTick); };
  auto tickAt = [&](float x) { return static_cast<Tick>(std::floor((x - originX) / std::max(1e-6f, pxPerTick))); };
  dl->PushClipRect(p0, p1, true);
  dl->AddRectFilled(p0, p1, theme::withAlpha(P.surface900, 0.9f));
  dl->AddLine(ImVec2(p0.x, p1.y - 0.5f), ImVec2(p1.x, p1.y - 0.5f), theme::withAlpha(P.line, 0.6f));

  // Short bars (a change inside a bar): a hatched cell.
  const auto& segs = map.segments();
  int shortHover = -1;
  for (size_t i = 1; i < segs.size(); ++i) {
    const Tick b0 = map.barStartAt(segs[i].start - 1);
    if (segs[i].start - b0 >= segs[i - 1].bar) continue;
    const float x0 = xOf(b0), x1 = xOf(segs[i].start);
    if (x1 < p0.x || x0 > p1.x) continue;
    const ImU32 hc = theme::withAlpha(P.textFaint, 0.35f);
    for (float x = x0 - (p1.y - p0.y); x < x1; x += 4.0f) {
      const float xa = std::max(x, x0), xb = std::min(x + (p1.y - p0.y), x1);
      if (xb <= xa) continue;
      dl->AddLine(ImVec2(xa, p1.y - (xa - x)), ImVec2(xb, p1.y - (xb - x)), hc);
    }
    if (in && m.x >= x0 && m.x < x1) shortHover = static_cast<int>(i);
  }

  // Labels: the start's meter, then every meter marker.
  struct Item {
    int index;  // marker index, -1 = the start
    Tick pos;
    int num, den;
  };
  std::vector<Item> items;
  {
    const MeterSeg& g0 = map.segments().front();
    if (indexAtPos(s, 0) < 0) items.push_back({-1, 0, g0.num, g0.den});
  }
  for (size_t i = 0; i < s.markers.size(); ++i)
    if (s.markers[i].isMeter()) items.push_back({static_cast<int>(i), s.markers[i].pos, s.markers[i].timeSigNum, s.markers[i].timeSigDen});
  int hover = -2;  // item index into `items` under the pointer (-2 = none)
  const float fs = theme::kFontLabel;
  for (size_t k = 0; k < items.size(); ++k) {
    const Item& it = items[k];
    const bool dragged = drag.index == it.index && drag.moved && it.index >= 0;
    const float x = xOf(dragged ? drag.to : it.pos);
    const std::string l = label(it.num, it.den);
    const float tw = a.fontMono->CalcTextSizeA(fs, 1e9f, 0, l.c_str()).x;
    if (x + tw + 8 < p0.x || x > p1.x) continue;
    const bool hov = in && m.x >= x - 2 && m.x < x + tw + 7;
    if (hov) hover = static_cast<int>(k);
    const ImU32 ink = hov || dragged ? P.text : P.textDim;
    dl->AddRectFilled(ImVec2(x, p0.y + 2), ImVec2(x + 2, p1.y - 1), theme::withAlpha(theme::rgb(theme::role::kGraph), hov || dragged ? 0.95f : 0.7f));
    dl->AddText(a.fontMono, fs, ImVec2(x + 5, p0.y + std::floor((p1.y - p0.y - fs) * 0.5f) - 0.5f), ink, l.c_str());
  }
  dl->PopClipRect();

  // Pointer.
  if (in) {
    if (hover >= 0) {
      const Item& it = items[static_cast<size_t>(hover)];
      const MeterMap::Bbt b = map.toBbt(it.pos);
      char buf[160];
      if (it.index < 0)
        std::snprintf(buf, sizeof buf, i18n::tr("Meter %s at the song start: click to edit, right-click for more"), label(it.num, it.den).c_str());
      else
        std::snprintf(buf, sizeof buf, i18n::tr("Meter %s from bar %lld: click to edit, drag to move, right-click for more"),
                      label(it.num, it.den).c_str(), static_cast<long long>(b.bar + 1));
      a.setHint(buf);
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else if (shortHover > 0) {
      const MeterSeg& g = segs[static_cast<size_t>(shortHover) - 1];
      const Tick b0 = map.barStartAt(segs[static_cast<size_t>(shortHover)].start - 1);
      const long long beats = static_cast<long long>((segs[static_cast<size_t>(shortHover)].start - b0 + g.beat - 1) / g.beat);
      char buf[128];
      std::snprintf(buf, sizeof buf, i18n::tr("Short bar %lld: %lld of %d beats (a meter change inside the bar)"),
                    static_cast<long long>(map.barIndex(b0) + 1), beats, g.num);
      a.setHint(buf);
    } else {
      a.setHint(i18n::tr("Meter lane: double-click to add a meter change at that bar"));
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hover < 0) {
      addAndEdit(a, std::max<Tick>(0, tickAt(m.x)));
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover >= 0) {
      drag = Drag{};
      drag.active = true;
      drag.index = items[static_cast<size_t>(hover)].index;
      drag.x0 = m.x;
      drag.to = items[static_cast<size_t>(hover)].pos;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hover >= 0) {
      menuIndex = items[static_cast<size_t>(hover)].index;
      ImGui::OpenPopup("##metermenu");
    }
    if (hover >= 0 && items[static_cast<size_t>(hover)].index >= 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
        !io.WantTextInput)
      meter::remove(a.doc, items[static_cast<size_t>(hover)].index);
  }
  // Drag a change (snapped to bar lines; Alt places it freely); a click opens the editor.
  if (drag.active) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (drag.index >= 0 && (drag.moved || std::fabs(m.x - drag.x0) > 3.0f)) {
        drag.moved = true;
        const Marker& mk = s.markers[static_cast<size_t>(std::min<int>(drag.index, static_cast<int>(s.markers.size()) - 1))];
        const Tick raw = std::max<Tick>(1, mk.pos + tickAt(m.x) - tickAt(drag.x0));
        std::vector<Marker> others = s.markers;
        if (drag.index < static_cast<int>(others.size())) others.erase(others.begin() + drag.index);
        MeterMap without;
        without.build(s.ppq, s.timeSigNum, s.timeSigDen, others);
        drag.to = io.KeyAlt ? raw : std::max<Tick>(without.nearestBar(raw), without.nextBar(0));
        a.setHint(i18n::tr("Moving the meter change (snaps to bar lines; Alt: free)"));
      }
    } else {
      if (drag.moved && drag.index >= 0 && drag.index < static_cast<int>(s.markers.size()))
        meter::move(a.doc, drag.index, drag.to, true);
      else if (!drag.moved)
        openEditor(a, drag.index);
      drag = Drag{};
    }
  }
  if (ImGui::BeginPopup("##metermenu")) {
    const bool base = menuIndex < 0 || menuIndex >= static_cast<int>(s.markers.size());
    if (ImGui::MenuItem(i18n::tr("Edit meter..."))) openEditor(a, menuIndex);
    if (!base) {
      const Marker& mk = s.markers[static_cast<size_t>(menuIndex)];
      if (ImGui::BeginMenu(i18n::tr("Meter"))) {
        for (const auto& c : kCommon)
          if (ImGui::MenuItem(label(c[0], c[1]).c_str(), nullptr, mk.timeSigNum == c[0] && mk.timeSigDen == c[1]))
            meter::set(a.doc, menuIndex, c[0], c[1]);
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem(i18n::tr("Jump here"))) {
        if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
        j31::songJump(a, mk.pos);
      }
      bool dup = false;
      for (size_t j = 0; j < s.markers.size(); ++j)
        dup = dup || (static_cast<int>(j) != menuIndex && s.markers[j].isMeter() && s.markers[j].pos == mk.pos);
      if (dup && ImGui::MenuItem(i18n::tr("Remove the duplicate"))) meter::removeDuplicates(a.doc);
      ImGui::Separator();
      if (!mk.name.empty() && ImGui::MenuItem(i18n::tr("Convert to plain marker"))) meter::remove(a.doc, menuIndex);
      if (ImGui::MenuItem(i18n::tr("Remove meter change"), "Del")) meter::remove(a.doc, menuIndex);
    }
    ImGui::EndPopup();
  }
  return in;
}

void drawEditor(App& a) {
  if (!ed.open) return;
  const Song& s = a.doc.song();
  const auto& P = theme::pal();
  int index = ed.base ? -1 : indexAtPos(s, ed.pos);
  if (!ed.base && index < 0) {  // removed (or undone) meanwhile
    ed.open = false;
    return;
  }
  if (ed.place) {
    const ImVec2 vp = ImGui::GetMainViewport()->WorkPos, vs = ImGui::GetMainViewport()->WorkSize;
    const ImVec2 want(330, 0);
    ImVec2 pos(std::clamp(ed.at.x - 40.0f, vp.x + 8, vp.x + vs.x - want.x - 8), std::clamp(ed.at.y + 14.0f, vp.y + 8, vp.y + vs.y - 300));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ed.place = false;
  }
  bool open = true;
  const std::string title = ui::windowTitle(ed.base ? "Meter at the song start" : "Meter change");
  if (!ImGui::Begin((title + "###meteredit").c_str(), &open,
                    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    ed.open = open;
    return;
  }
  ui::KeepWindowInViewport();
  const float labelW = 92.0f;
  auto rowLabel = [&](const char* l) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr(l));
    ImGui::SameLine(labelW);
  };
  int num = s.timeSigNum, den = s.timeSigDen;
  if (!ed.base) {
    num = s.markers[static_cast<size_t>(index)].timeSigNum;
    den = s.markers[static_cast<size_t>(index)].timeSigDen;
  }
  auto apply = [&](int n, int d) {
    if (ed.base) a.doc.setTimeSignature(std::clamp(n, 1, 16), d, ed.alsoPatterns);
    else meter::set(a.doc, index, n, d);
  };
  rowLabel("Beats");
  if (ui::BeginCombo("##mnum", std::to_string(num).c_str(), 64)) {
    for (int n = 1; n <= 16; ++n)
      if (ImGui::Selectable(std::to_string(n).c_str(), n == num)) apply(n, den);
    ImGui::EndCombo();
  }
  ImGui::SameLine(0, theme::kS2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "/");
  ImGui::SameLine(0, theme::kS2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Note"));
  ImGui::SameLine(0, theme::kS2);
  if (ui::BeginCombo("##mden", std::to_string(den).c_str(), 64)) {
    for (int d : {2, 4, 8, 16})
      if (ImGui::Selectable(std::to_string(d).c_str(), d == den)) apply(num, d);
    ImGui::EndCombo();
  }
  rowLabel("Common");
  const float bw = 46.0f;
  for (size_t k = 0; k < std::size(kCommon); ++k) {
    if (k == 4) {
      ImGui::Dummy(ImVec2(labelW - ImGui::GetStyle().ItemSpacing.x, 0));
      ImGui::SameLine(labelW);
    } else if (k > 0) {
      ImGui::SameLine(0, 4);
    }
    const std::string l = label(kCommon[k][0], kCommon[k][1]);
    ImGui::PushID(static_cast<int>(k));
    if (ui::TextButton(l.c_str(), num == kCommon[k][0] && den == kCommon[k][1], bw)) apply(kCommon[k][0], kCommon[k][1]);
    ImGui::PopID();
  }
  if (!ed.base) {
    const MeterMap map(s);
    rowLabel("From bar");
    int bar = static_cast<int>(map.barIndex(ed.pos) + 1);
    ImGui::SetNextItemWidth(64);
    if (ImGui::InputInt("##mbar", &bar, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && bar >= 2) {
      std::vector<Marker> others = s.markers;
      others.erase(others.begin() + index);
      MeterMap without;
      without.build(s.ppq, s.timeSigNum, s.timeSigDen, others);
      const Tick to = without.barStart(bar - 1);
      if (to > 0 && meter::move(a.doc, index, to, true) >= 0) ed.pos = to;
    }
    if (map.shortBar(std::max<Tick>(0, ed.pos - 1))) {
      ImGui::SameLine(0, theme::kS2);
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("inside a bar (short bar before it)"));
    }
    rowLabel("Name");
    const Marker& mk = s.markers[static_cast<size_t>(index)];
    if (ed.nameFor != ed.pos) {
      std::snprintf(ed.name, sizeof ed.name, "%s", mk.name.c_str());
      ed.nameFor = ed.pos;
    }
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##mname", i18n::tr("optional"), ed.name, sizeof ed.name);
    if (ImGui::IsItemDeactivatedAfterEdit() && mk.name != ed.name) ops::renameMarker(a.doc, index, ed.name);
    ImGui::Dummy(ImVec2(0, 2));
    bool forPatterns = mk.meterForPatterns;
    if (ImGui::Checkbox(i18n::tr("New patterns here use this meter"), &forPatterns)) meter::setForPatterns(a.doc, index, forPatterns);
  } else {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Checkbox(i18n::tr("Also change the meter of existing patterns"), &ed.alsoPatterns);
    ui::Hint("Off: existing patterns keep their meter and length, so nothing moves");
  }
  ImGui::Dummy(ImVec2(0, 4));
  const float doneW = 72.0f, remW = ed.base ? 0.0f : 80.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, avail - doneW - remW - (remW > 0 ? theme::kS2 : 0)));
  if (!ed.base) {
    if (ui::DangerButton("Remove", false, remW)) {
      meter::remove(a.doc, index);
      open = false;
    }
    ImGui::SameLine(0, theme::kS2);
  }
  if (ui::PrimaryButton("Done", doneW)) open = false;
  ImGui::End();
  ed.open = open;
}

void patternMeterMenu(App& a, ObjectId pattern) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(pattern);
  if (!ImGui::BeginMenu(i18n::tr("Meter"), p != nullptr)) return;
  char songLabel[64];
  std::snprintf(songLabel, sizeof songLabel, i18n::tr("Song (%s)"), label(s.timeSigNum, s.timeSigDen).c_str());
  if (ImGui::MenuItem(songLabel, nullptr, !p->ownMeter())) meter::setPatternMeter(a.doc, pattern, 0, 0);
  ImGui::Separator();
  for (const auto& c : kCommon)
    if (ImGui::MenuItem(label(c[0], c[1]).c_str(), nullptr, p->ownMeter() && p->meterNum == c[0] && p->meterDen == c[1]))
      meter::setPatternMeter(a.doc, pattern, c[0], c[1]);
  ImGui::Separator();
  ImGui::TextDisabled("%s", i18n::tr("Its bars stay, so its length follows the meter"));
  ImGui::EndMenu();
}

void clipMeterItems(App& a, ObjectId clip) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c || c->kind != ClipKind::Pattern) return;
  const Pattern* p = s.pattern(c->pattern);
  if (!p || meter::changeCount(s) == 0) return;
  const MeterSeg g = MeterMap(s).segAt(c->start);
  const bool same = s.patternMeterNum(*p) == g.num && s.patternMeterDen(*p) == g.den;
  char l[96];
  std::snprintf(l, sizeof l, i18n::tr("Make pattern meter match here (%s)"), label(g.num, g.den).c_str());
  if (ImGui::MenuItem(l, nullptr, false, !same)) {
    if (!meter::matchPatternMeter(a.doc, p->id, c->start, true)) {
      meter::matchPatternMeter(a.doc, p->id, c->start, false);
      a.notify(i18n::tr("The pattern's length is not whole bars of that meter: its bars were kept, so its length changed"));
    }
  }
}

}  // namespace gl::meterui
