// Gridline — K-128: Smooth layout panels (Event List, Project/Track Notes, Score Editor).
// Strictly theme-role driven (Theme.h): zero hard-coded light neutral colour literals.
#include "SmoothPanels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/MenuOpsExtra.h"
#include "../core/Meter.h"
#include "../core/Params.h"
#include "../core/Project.h"
#include "../core/SmoothScore.h"
#include "../core/SongTools.h"
#include "App.h"
#include "LayoutParts.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::smoothpanels {

using theme::pal;
using namespace smoothscore;

namespace {

// ---- Shared active pattern / channel & selection sync with Piano Roll ----

struct ActiveTarget {
  const Pattern* pat = nullptr;
  const Channel* ch = nullptr;
  ObjectId patId = kNoId;
  ObjectId chId = kNoId;
};

ActiveTarget resolveTarget(App& a) {
  ActiveTarget t;
  const Song& s = a.doc.song();
  t.pat = s.pattern(a.curPattern);
  if (!t.pat && !s.patterns.empty()) t.pat = &s.patterns[0];
  t.ch = s.channel(a.selChannel);
  if (!t.ch && !s.channels.empty()) t.ch = &s.channels[0];
  if (t.pat) t.patId = t.pat->id;
  if (t.ch) t.chId = t.ch->id;
  return t;
}

void syncSelectionContext(App& a, ObjectId patId, ObjectId chId) {
  if (a.piano.selPattern != patId || a.piano.selChannel != chId) {
    a.piano.selected.clear();
    a.piano.selPattern = patId;
    a.piano.selChannel = chId;
  }
}

std::vector<Note> notesCopy(const Pattern* pat, ObjectId chId) {
  if (!pat || chId == kNoId) return {};
  auto it = pat->notes.find(chId);
  return it == pat->notes.end() ? std::vector<Note>{} : it->second;
}

void deleteSelectedNotes(App& a, ObjectId patId, ObjectId chId) {
  const Pattern* pat = a.doc.song().pattern(patId);
  if (!pat || chId == kNoId) return;
  syncSelectionContext(a, patId, chId);
  if (a.piano.selected.empty()) return;
  auto ns = notesCopy(pat, chId);
  std::vector<Note> kept;
  kept.reserve(ns.size());
  for (size_t i = 0; i < ns.size(); ++i)
    if (!a.piano.selected.count(i)) kept.push_back(ns[i]);
  if (kept.size() == ns.size()) return;
  a.piano.selected.clear();
  a.doc.setNotes(patId, chId, std::move(kept), "delete notes");
}

// ============================================================================
// 1. EVENT LIST (Smooth Lists right-hand drawer: Event | Marker | Tempo | Sig)
// ============================================================================

struct EventListState {
  int tab = 0;  // 0 Event, 1 Marker, 2 Tempo, 3 Signature
  EventFilter filter{};
  // Inline text edit state (cell: 0 Pos, 1 Pitch, 2 Vel, 3 Len; -1 = none).
  int editRow = -1;
  int editCol = -1;
  size_t editNoteIndex = 0;
  char editBuf[64] = {};
  bool editFocusNext = false;
  // Drag-edit state for smooth mouse value scrubbing.
  bool dragging = false;
  int dragCol = -1;  // 0 Pos, 1 Pitch, 2 Vel, 3 Len
  size_t dragNoteIndex = 0;
  float dragStartY = 0;
  float dragStartX = 0;
  Note dragBaseNote;
  std::vector<Note> dragBaseList;
  int dragPreviewPitch = -1;
  ObjectId dragPreviewCh = kNoId;
  bool dragChanged = false;
  // Tempo inline edit.
  bool editTempo = false;
  char tempoBuf[32] = {};
  bool tempoFocusNext = false;
};

EventListState gEventList;

void stopEventPreview(App& a) {
  if (gEventList.dragPreviewPitch >= 0 && gEventList.dragPreviewCh != kNoId) {
    a.engine.noteOff(gEventList.dragPreviewCh, gEventList.dragPreviewPitch);
  }
  gEventList.dragPreviewPitch = -1;
  gEventList.dragPreviewCh = kNoId;
}

void startEventPreview(App& a, ObjectId chId, int pitch, int vel) {
  if (a.engine.playing()) return;
  if (gEventList.dragPreviewPitch == pitch && gEventList.dragPreviewCh == chId) return;
  stopEventPreview(a);
  a.engine.noteOn(chId, pitch, std::clamp(vel, 1, 127) / 127.0f);
  gEventList.dragPreviewPitch = pitch;
  gEventList.dragPreviewCh = chId;
}

// Small Smooth filter chip button (strictly theme-role driven).
bool filterChip(const char* id, const char* label, bool on, const char* hint) {
  const auto& P = pal();
  const char* txt = i18n::tr(label);
  const float fs = theme::kFontCaption;
  const float tw = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, txt).x;
  const float w = std::ceil(tw) + 14.0f;
  const float h = 20.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + w, p0.y + h);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = on ? P.accent : hov ? P.surface700 : P.surface900;
  const ImU32 border = on ? P.accent : P.line;
  const ImU32 fg = on ? theme::contrastText(P.accent) : hov ? P.text : P.textDim;
  dl->AddRectFilled(p0, p1, bg, 4.0f);
  dl->AddRect(p0, p1, border, 4.0f, 0, 1.0f);
  dl->AddText(ImGui::GetFont(), fs, ImVec2(std::round(p0.x + (w - tw) * 0.5f), std::round(p0.y + (h - fs) * 0.5f)), fg, txt);
  if (hint) ui::Hint(hint);
  return clicked;
}

void addDefaultEventNote(App& a) {
  const ActiveTarget tgt = resolveTarget(a);
  if (!tgt.pat || tgt.chId == kNoId) return;
  const Song& s = a.doc.song();
  syncSelectionContext(a, tgt.patId, tgt.chId);
  auto ns = notesCopy(tgt.pat, tgt.chId);
  Tick start = 0;
  if (!ns.empty()) {
    Tick maxEnd = 0;
    for (const Note& n : ns) maxEnd = std::max(maxEnd, n.start + n.length);
    const Tick beat = std::max<Tick>(1, s.patternBeatTicks(*tgt.pat));
    start = ((maxEnd + beat - 1) / beat) * beat;
    const Tick patLen = std::max<Tick>(beat, s.patternLength(*tgt.pat));
    if (start >= patLen) start = static_cast<Tick>(std::max<double>(0.0, a.engine.position())) % patLen;
  }
  const Tick len = std::max<Tick>(1, s.patternBeatTicks(*tgt.pat));
  Note n(start, len, 60, std::clamp(a.piano.lastVelocity, 1, 127));
  const size_t newIdx = ns.size();
  ns.push_back(n);
  a.doc.setNotes(tgt.patId, tgt.chId, std::move(ns), "add note");
  a.piano.selected.clear();
  a.piano.selected.insert(newIdx);
}

void drawEventTab(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ActiveTarget tgt = resolveTarget(a);
  if (tgt.pat && tgt.chId != kNoId) syncSelectionContext(a, tgt.patId, tgt.chId);

  // Filter chips + add/delete row.
  const float chipH = 20.0f;
  if (ui::IconButton("lgaddmk", ui::Icon::Plus, false, chipH, "Add a note event to the active pattern")) addDefaultEventNote(a);
  ImGui::SameLine(0, 3);
  {
    const bool canDel = tgt.pat && tgt.chId != kNoId && a.piano.selPattern == tgt.patId && a.piano.selChannel == tgt.chId &&
                        !a.piano.selected.empty();
    ImGui::BeginDisabled(!canDel);
    if (ui::IconButton("lgdelev", ui::Icon::Trash, false, chipH, "Delete selected events")) deleteSelectedNotes(a, tgt.patId, tgt.chId);
    ImGui::EndDisabled();
  }
  ImGui::SameLine(0, 6);
  if (filterChip("##evf_notes", "Notes", gEventList.filter.notes, "Show MIDI note events")) gEventList.filter.notes = !gEventList.filter.notes;
  ImGui::SameLine(0, 4);
  if (filterChip("##evf_vel", "Velocity", gEventList.filter.velocity, "Show velocity / pressure events"))
    gEventList.filter.velocity = !gEventList.filter.velocity;
  ImGui::SameLine(0, 4);
  if (filterChip("##evf_cc", "Mod/CC", gEventList.filter.modCc, "Show modulation and controller events"))
    gEventList.filter.modCc = !gEventList.filter.modCc;

  if (tgt.ch) {
    ImGui::SameLine(0, 8);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rh = 20.0f;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 4.0f, p.y + rh * 0.5f), 3.5f, theme::content(tgt.ch->color));
    ImGui::Dummy(ImVec2(10.0f, rh));
    ImGui::SameLine(0, 2);
    const float remW = std::max(24.0f, ImGui::GetContentRegionAvail().x);
    const std::string tag = tgt.ch->name;
    theme::textEllipsis(ImGui::GetWindowDrawList(), a.fontRegular, theme::kFontCaption,
                        ImVec2(ImGui::GetCursorScreenPos().x, theme::textTop(a.fontRegular, theme::kFontCaption, p.y, rh)), remW, P.textDim,
                        tag.c_str());
    ImGui::Dummy(ImVec2(remW, rh));
  }

  ImGui::Dummy(ImVec2(0, 2));

  std::vector<EventRow> rows;
  if (tgt.pat && tgt.chId != kNoId) rows = buildEventRows(s, *tgt.pat, tgt.chId, gEventList.filter);

  // Keyboard Delete removes selected note events when this window is focused and no text field is active.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput && gEventList.editRow < 0 &&
      (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
    deleteSelectedNotes(a, tgt.patId, tgt.chId);
  }

  // Handle active drag commit / release.
  if (gEventList.dragging) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && tgt.pat && tgt.chId != kNoId &&
        gEventList.dragNoteIndex < gEventList.dragBaseList.size()) {
      const ImVec2 mp = ImGui::GetIO().MousePos;
      const float dy = gEventList.dragStartY - mp.y;  // drag up = increase
      const float dx = mp.x - gEventList.dragStartX;
      Note edited = gEventList.dragBaseNote;
      const Tick step = std::max<Tick>(1, s.ppq / 4);
      if (gEventList.dragCol == 0) {
        const Tick delta = static_cast<Tick>(std::lround((dx + dy) / 6.0f)) * step;
        edited.start = std::max<Tick>(0, gEventList.dragBaseNote.start + delta);
      } else if (gEventList.dragCol == 1) {
        const int dp = static_cast<int>(std::lround(dy / 6.0f));
        edited.pitch = std::clamp(gEventList.dragBaseNote.pitch + dp, 0, 127);
        if (edited.pitch != gEventList.dragPreviewPitch) startEventPreview(a, tgt.chId, edited.pitch, edited.velocity);
      } else if (gEventList.dragCol == 2) {
        const int dv = static_cast<int>(std::lround(dy / 2.0f));
        edited.velocity = std::clamp(gEventList.dragBaseNote.velocity + dv, 0, 127);
      } else if (gEventList.dragCol == 3) {
        const Tick delta = static_cast<Tick>(std::lround((dx + dy) / 6.0f)) * step;
        edited.length = std::max<Tick>(step, gEventList.dragBaseNote.length + delta);
      }
      if (!(edited == gEventList.dragBaseNote)) {
        auto next = gEventList.dragBaseList;
        next[gEventList.dragNoteIndex] = edited;
        a.doc.setNotes(tgt.patId, tgt.chId, std::move(next), "edit event", 0xE7157001ull + gEventList.dragNoteIndex);
        gEventList.dragChanged = true;
      }
    } else {
      stopEventPreview(a);
      if (gEventList.dragChanged) a.doc.endGesture();
      gEventList.dragging = false;
      gEventList.dragChanged = false;
    }
  }

  const float footH = 24.0f;
  const ImVec2 av = ImGui::GetContentRegionAvail();
  const float tableH = std::max(40.0f, av.y - footH - ImGui::GetStyle().ItemSpacing.y);

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5, 2));
  const int nCols = 6;
  const ImGuiTableFlags tflags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoBordersInBody;
  if (ImGui::BeginTable("##lgevents", nCols, tflags, ImVec2(av.x, tableH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(i18n::tr("Position"), ImGuiTableColumnFlags_WidthStretch, 1.35f);
    ImGui::TableSetupColumn(i18n::tr("Status"), ImGuiTableColumnFlags_WidthStretch, 0.80f);
    ImGui::TableSetupColumn(i18n::tr("Ch"), ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn(i18n::tr("Pitch"), ImGuiTableColumnFlags_WidthStretch, 0.72f);
    ImGui::TableSetupColumn(i18n::tr("Vel"), ImGuiTableColumnFlags_WidthStretch, 0.70f);
    ImGui::TableSetupColumn(i18n::tr("Length"), ImGuiTableColumnFlags_WidthStretch, 1.15f);
    ImGui::TableHeadersRow();
    {
      ImGuiTable* tb = ImGui::GetCurrentTable();
      const ImRect hr = ImGui::TableGetCellBgRect(tb, 0);
      for (int k = 1; k < nCols; ++k) {
        const float x = std::round(ImGui::TableGetCellBgRect(tb, k).Min.x) - 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, hr.Min.y + 4), ImVec2(x, hr.Max.y - 4), theme::withAlpha(P.text, 0.18f));
      }
    }

    auto commitInline = [&](int col, size_t noteIdx, const char* text) {
      if (!tgt.pat || tgt.chId == kNoId) return;
      auto ns = notesCopy(tgt.pat, tgt.chId);
      if (noteIdx >= ns.size()) return;
      Note edited = ns[noteIdx];
      bool ok = false;
      if (col == 0) {
        Tick pos = 0;
        ok = parseBbtPos(s, text, pos, tgt.pat);
        if (ok) edited.start = pos;
      } else if (col == 1) {
        int pitch = 60;
        ok = parsePitch(text, pitch);
        if (ok) edited.pitch = pitch;
      } else if (col == 2) {
        char* end = nullptr;
        const long v = std::strtol(text, &end, 10);
        if (end != text) {
          edited.velocity = std::clamp(static_cast<int>(v), 0, 127);
          ok = true;
        }
      } else if (col == 3) {
        Tick len = 1;
        ok = parseBbtLen(s, text, len, tgt.pat);
        if (ok) edited.length = len;
      }
      if (ok && !(edited == ns[noteIdx])) {
        ns[noteIdx] = edited;
        a.doc.setNotes(tgt.patId, tgt.chId, std::move(ns), "edit event");
      }
    };

    for (size_t i = 0; i < rows.size(); ++i) {
      const EventRow& r = rows[i];
      const bool isNote = r.kind == EventKind::Note;
      const bool sel = isNote && a.piano.selected.count(r.noteIndex) > 0;
      ImGui::TableNextRow(ImGuiTableRowFlags_None, 20.0f);
      if (sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, theme::withAlpha(P.accent, 0.24f));
      else if (i % 2) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, theme::withAlpha(P.text, 0.03f));

      ImGui::PushID(static_cast<int>(i));
      auto selectRow = [&]() {
        if (!isNote) {
          a.engine.setPosition(r.pos);
          return;
        }
        const bool multi = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
        if (multi) {
          if (!a.piano.selected.erase(r.noteIndex)) a.piano.selected.insert(r.noteIndex);
        } else {
          a.piano.selected.clear();
          a.piano.selected.insert(r.noteIndex);
        }
      };

      auto editableCell = [&](int colId, const std::string& disp, const char* hintText) {
        if (gEventList.editRow == static_cast<int>(i) && gEventList.editCol == colId) {
          ImGui::SetNextItemWidth(-1.0f);
          if (gEventList.editFocusNext) {
            ImGui::SetKeyboardFocusHere();
            gEventList.editFocusNext = false;
          }
          const bool enter = ImGui::InputText("##ed", gEventList.editBuf, sizeof gEventList.editBuf,
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
          if (enter) {
            commitInline(colId, gEventList.editNoteIndex, gEventList.editBuf);
            gEventList.editRow = -1;
            gEventList.editCol = -1;
          } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            gEventList.editRow = -1;
            gEventList.editCol = -1;
          } else if (ImGui::IsItemDeactivatedAfterEdit()) {
            commitInline(colId, gEventList.editNoteIndex, gEventList.editBuf);
            gEventList.editRow = -1;
            gEventList.editCol = -1;
          } else if (ImGui::IsItemDeactivated()) {
            gEventList.editRow = -1;
            gEventList.editCol = -1;
          }
          return;
        }
        ImGui::PushID(colId);
        if (a.fontMono) ImGui::PushFont(a.fontMono);
        const bool clicked = ImGui::Selectable(disp.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        if (a.fontMono) ImGui::PopFont();
        if (clicked) selectRow();
        if (isNote && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          gEventList.editRow = static_cast<int>(i);
          gEventList.editCol = colId;
          gEventList.editNoteIndex = r.noteIndex;
          std::snprintf(gEventList.editBuf, sizeof gEventList.editBuf, "%s", disp.c_str());
          gEventList.editFocusNext = true;
        } else if (isNote && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) && !gEventList.dragging) {
          auto ns = notesCopy(tgt.pat, tgt.chId);
          if (r.noteIndex < ns.size()) {
            selectRow();
            gEventList.dragging = true;
            gEventList.dragCol = colId;
            gEventList.dragNoteIndex = r.noteIndex;
            gEventList.dragStartX = ImGui::GetIO().MousePos.x;
            gEventList.dragStartY = ImGui::GetIO().MousePos.y;
            gEventList.dragBaseNote = ns[r.noteIndex];
            gEventList.dragBaseList = std::move(ns);
            gEventList.dragChanged = false;
          }
        }
        if (hintText) ui::Hint(hintText);
        ImGui::PopID();
      };

      // Col 0: Position
      ImGui::TableNextColumn();
      editableCell(0, formatBbtPos(s, r.pos, tgt.pat), "Click: select; drag or double-click: edit position");

      // Col 1: Status
      ImGui::TableNextColumn();
      if (ImGui::Selectable(i18n::tr(r.status.c_str()), false)) selectRow();

      // Col 2: Ch
      ImGui::TableNextColumn();
      ImGui::TextColored(theme::vec(P.textDim), "%d", r.channelNum);

      // Col 3: Pitch / Detail
      ImGui::TableNextColumn();
      if (isNote) editableCell(1, r.detail, "Drag up/down or double-click to edit pitch");
      else ImGui::TextColored(theme::vec(P.textDim), "%s", r.detail.c_str());

      // Col 4: Velocity / Value (with subtle micro bar)
      ImGui::TableNextColumn();
      {
        const ImVec2 cellPos = ImGui::GetCursorScreenPos();
        const float cellW = ImGui::GetContentRegionAvail().x;
        const float frac = std::clamp(r.value / 127.0f, 0.0f, 1.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(cellPos.x, cellPos.y + 14.0f), ImVec2(cellPos.x + cellW * frac, cellPos.y + 17.0f),
            theme::withAlpha(sel ? P.accent : P.hot, 0.45f), 1.5f);
        if (isNote) editableCell(2, std::to_string(r.value), "Drag up/down or double-click to edit velocity");
        else ImGui::TextColored(theme::vec(P.textDim), "%d", r.value);
      }

      // Col 5: Length
      ImGui::TableNextColumn();
      if (isNote) editableCell(3, formatBbtLen(s, r.length, tgt.pat), "Drag or double-click to edit length");
      else ImGui::TextColored(theme::vec(P.textFaint), "-");

      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();

  // Footer.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = ImGui::GetCursorScreenPos();
  const float fw = ImGui::GetContentRegionAvail().x;
  dl->AddLine(ImVec2(f0.x, f0.y + 0.5f), ImVec2(f0.x + fw, f0.y + 0.5f), P.dark ? P.ink : P.line);
  const float fs = theme::kFontLabel;
  const float ty = theme::textTop(a.fontRegular, fs, f0.y + 1.0f, footH - 1.0f);
  const std::string leftLabel = tgt.pat ? tgt.pat->name : std::string(i18n::tr("Event"));
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(f0.x + 8.0f, ty), fw * 0.55f, P.textDim, leftLabel.c_str());
  char cnt[64];
  if (!a.piano.selected.empty())
    std::snprintf(cnt, sizeof cnt, "%zu %s (%zu)", rows.size(), i18n::tr(rows.size() == 1 ? "event" : "events"), a.piano.selected.size());
  else
    std::snprintf(cnt, sizeof cnt, "%zu %s", rows.size(), i18n::tr(rows.size() == 1 ? "event" : "events"));
  const float cw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, cnt).x;
  dl->AddText(a.fontRegular, fs, ImVec2(std::round(f0.x + fw - 8.0f - cw), ty), P.textDim, cnt);
  ImGui::Dummy(ImVec2(fw, footH));
}

void drawEventListImpl(App& a) {
  const Song& s = a.doc.song();
  const auto& P = pal();
  const char* tabs[] = {i18n::tr("Event"), i18n::tr("Marker"), i18n::tr("Tempo"), i18n::tr("Signature")};
  const char* hints[] = {i18n::tr("MIDI events in the active pattern"), i18n::tr("The song's markers"),
                         i18n::tr("The song's tempo"), i18n::tr("The song's time signatures")};
  {
    const float fh = ImGui::GetFrameHeight(), x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float W = ImGui::GetContentRegionAvail().x;
    // Full width for the 4 tabs so "Signature" never clips in a 300 px right drawer.
    float leftUsed = 0.0f;
    if (gEventList.tab == 1) {
      if (ui::IconButton("lgaddmk", ui::Icon::Plus, false, fh, "Add a marker at the song position"))
        ops::addMarker(a.doc, static_cast<Tick>(a.engine.position()), ops::nextMarkerName(s));
      leftUsed = fh + 4.0f;
      ImGui::SameLine(0, 0);
    }
    const float availSeg = std::max(100.0f, W - leftUsed - 4.0f);
    const float segW = std::min(76.0f, std::floor(availSeg / 4.0f));
    ImGui::SetCursorPos(ImVec2(x0 + std::max(leftUsed, std::floor((W - segW * 4.0f - 4.0f) * 0.5f)), y0));
    ui::Segmented("##lglists", tabs, 4, &gEventList.tab, segW, 0, hints);
  }
  ImGui::Dummy(ImVec2(0, 2));

  if (gEventList.tab == 0) {
    drawEventTab(a);
    return;
  }

  // Marker / Tempo / Signature tabs.
  struct Row {
    Tick pos;
    std::string a, b;
  };
  std::vector<Row> rows;
  const char* cols[3] = {i18n::tr("Position"), i18n::tr("Name"), nullptr};
  char t[32] = {};
  if (gEventList.tab == 1) {
    for (const Marker& m : s.markers)
      if (!m.isMeter()) rows.push_back({m.pos, m.name, i18n::tr(m.isKey() ? "Key" : "Marker")});
    cols[2] = i18n::tr("Kind");
  } else if (gEventList.tab == 2) {
    std::snprintf(t, sizeof t, "%.2f", s.tempo);
    rows.push_back({0, t, ""});
    cols[1] = i18n::tr("Tempo");
  } else {
    std::snprintf(t, sizeof t, "%d/%d", s.timeSigNum, s.timeSigDen);
    rows.push_back({0, t, ""});
    for (const Marker& m : s.markers)
      if (m.isMeter()) {
        std::snprintf(t, sizeof t, "%d/%d", m.timeSigNum, m.timeSigDen);
        rows.push_back({m.pos, t, ""});
      }
    cols[1] = i18n::tr("Signature");
  }
  const int nCols = cols[2] ? 3 : 2;
  const float footH = 24.0f;
  const ImVec2 av = ImGui::GetContentRegionAvail();
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 3));
  const float tableH = std::max(40.0f, av.y - footH - ImGui::GetStyle().ItemSpacing.y);
  if (ImGui::BeginTable("##lglist", nCols, ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(av.x, tableH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(cols[0], ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(cols[1], ImGuiTableColumnFlags_WidthStretch, 1.3f);
    if (nCols == 3) ImGui::TableSetupColumn(cols[2], ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableHeadersRow();
    {
      ImGuiTable* tb = ImGui::GetCurrentTable();
      const ImRect hr = ImGui::TableGetCellBgRect(tb, 0);
      for (int k = 1; k < nCols; ++k) {
        const float x = std::round(ImGui::TableGetCellBgRect(tb, k).Min.x) - 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, hr.Min.y + 4), ImVec2(x, hr.Max.y - 4), theme::withAlpha(P.text, 0.18f));
      }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
      const Row& r = rows[i];
      ImGui::TableNextRow(ImGuiTableRowFlags_None, 20.0f);
      if (i % 2) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, theme::withAlpha(P.text, 0.03f));
      ImGui::TableNextColumn();
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(formatBbtPos(s, r.pos).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
        a.engine.setPosition(r.pos);
        if (gEventList.tab == 2 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          gEventList.editTempo = true;
          std::snprintf(gEventList.tempoBuf, sizeof gEventList.tempoBuf, "%.2f", s.tempo);
          gEventList.tempoFocusNext = true;
        }
      }
      ui::Hint(gEventList.tab == 2 ? "Double-click: edit song tempo" : "Click: move the song position here");
      ImGui::PopID();
      ImGui::TableNextColumn();
      if (gEventList.tab == 2 && gEventList.editTempo && i == 0) {
        ImGui::SetNextItemWidth(-1.0f);
        if (gEventList.tempoFocusNext) {
          ImGui::SetKeyboardFocusHere();
          gEventList.tempoFocusNext = false;
        }
        const bool enter = ImGui::InputText("##lgtempo", gEventList.tempoBuf, sizeof gEventList.tempoBuf,
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
          char* end = nullptr;
          const double bpm = std::strtod(gEventList.tempoBuf, &end);
          if (end != gEventList.tempoBuf && std::isfinite(bpm)) a.doc.setParam(songParam(SongParam::Tempo), std::clamp(bpm, 20.0, 999.0), false);
          gEventList.editTempo = false;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsItemDeactivated()) {
          gEventList.editTempo = false;
        }
      } else {
        ImGui::TextUnformatted(r.a.c_str());
      }
      if (nCols == 3) {
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::vec(P.textDim), "%s", r.b.c_str());
      }
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = ImGui::GetCursorScreenPos();
  const float fw = ImGui::GetContentRegionAvail().x;
  dl->AddLine(ImVec2(f0.x, f0.y + 0.5f), ImVec2(f0.x + fw, f0.y + 0.5f), P.dark ? P.ink : P.line);
  const size_t n = rows.size();
  const std::string cnt = gEventList.tab == 1 ? std::to_string(n) + " " + i18n::tr(n == 1 ? "marker" : "markers")
                          : gEventList.tab == 2 ? std::string(t) + " BPM"
                                                : std::to_string(n) + " " + i18n::tr(n == 1 ? "signature" : "signatures");
  const float fs = theme::kFontLabel;
  const float ty = theme::textTop(a.fontRegular, fs, f0.y + 1.0f, footH - 1.0f);
  dl->AddText(a.fontRegular, fs, ImVec2(f0.x + 8.0f, ty), P.textDim, tabs[gEventList.tab]);
  const float cw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, cnt.c_str()).x;
  dl->AddText(a.fontRegular, fs, ImVec2(std::round(f0.x + fw - 8.0f - cw), ty), P.textDim, cnt.c_str());
  ImGui::Dummy(ImVec2(fw, footH));
}

// ============================================================================
// 2. PROJECT & TRACK NOTES (Smooth Notepad right-hand drawer)
// ============================================================================

struct NotesState {
  int tab = 0;  // 0 Project, 1 Track
  std::string projBuf;
  std::string trackBuf;
  ObjectId trackBufCh = kNoId;
  bool editing = false;
  bool focus = false;
  bool blur = false;
};

NotesState gNotes;

int growStringCallback(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* s = static_cast<std::string*>(d->UserData);
    s->resize(static_cast<size_t>(d->BufTextLen));
    d->Buf = s->data();
  }
  return 0;
}

void drawNotesImpl(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Channel* ch = s.channel(a.selChannel);
  if (!ch && !s.channels.empty()) ch = &s.channels[0];

  // Top segmented tabs: Project | Track.
  {
    const float W = ImGui::GetContentRegionAvail().x;
    const char* tabs[] = {i18n::tr("Project"), i18n::tr("Track")};
    const char* hints[] = {i18n::tr("Song-level project notes"), i18n::tr("Notes for the selected track")};
    const float segW = std::min(110.0f, std::floor((W - 8.0f) * 0.5f));
    const float x0 = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(x0 + std::max(0.0f, std::floor((W - segW * 2.0f - 4.0f) * 0.5f)));
    int nextTab = gNotes.tab;
    if (ui::Segmented("##lgnotestabs", tabs, 2, &nextTab, segW, 0, hints)) {
      if (gNotes.editing) ImGui::ClearActiveID();
      gNotes.tab = nextTab;
    }
  }

  ImGui::Dummy(ImVec2(0, 3));

  // Subtle header strip showing context (Project title or Track colour + name) and character count.
  const bool isTrack = gNotes.tab == 1;
  const std::string& activeSource = isTrack ? (ch ? ch->notes : std::string{})
                                            : s.info.comments;
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 h0 = ImGui::GetCursorScreenPos();
    const float hw = ImGui::GetContentRegionAvail().x;
    const float hh = 24.0f;
    dl->AddRectFilled(h0, ImVec2(h0.x + hw, h0.y + hh), P.surface900, 4.0f);
    dl->AddRect(h0, ImVec2(h0.x + hw, h0.y + hh), P.line, 4.0f, 0, 1.0f);
    float tx = h0.x + 8.0f;
    const float fs = theme::kFontCaption;
    const float ty = theme::textTop(a.fontRegular, fs, h0.y, hh);
    if (isTrack && ch) {
      dl->AddCircleFilled(ImVec2(tx + 4.0f, h0.y + hh * 0.5f), 4.0f, theme::content(ch->color));
      tx += 14.0f;
    }
    const std::string title = isTrack ? (ch ? ch->name : std::string(i18n::tr("No track selected")))
                                      : (s.info.title.empty() ? std::string(i18n::tr("Project notes")) : s.info.title);
    const size_t chars = (gNotes.editing ? (isTrack ? gNotes.trackBuf.size() : gNotes.projBuf.size()) : activeSource.size());
    char countBuf[48];
    std::snprintf(countBuf, sizeof countBuf, "%zu %s", chars, i18n::tr(chars == 1 ? "char" : "chars"));
    const float cw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, countBuf).x;
    theme::textEllipsis(dl, a.fontBold, fs, ImVec2(tx, ty), std::max(20.0f, hw - (tx - h0.x) - cw - 14.0f), P.text, title.c_str());
    dl->AddText(a.fontRegular, fs, ImVec2(std::round(h0.x + hw - 8.0f - cw), ty), P.textDim, countBuf);
    ImGui::Dummy(ImVec2(hw, hh));
  }

  ImGui::Dummy(ImVec2(0, 3));

  if (isTrack && !ch) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 90.0f)), "No track selected",
                   "Select a track to write notes for it");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 90.0f)));
    return;
  }

  std::string& buf = isTrack ? gNotes.trackBuf : gNotes.projBuf;
  const ObjectId chId = ch ? ch->id : kNoId;
  if (!gNotes.editing || (isTrack && gNotes.trackBufCh != chId)) {
    buf = activeSource;
    if (isTrack) gNotes.trackBufCh = chId;
  }

  if (gNotes.focus) {
    ImGui::SetKeyboardFocusHere();
    gNotes.focus = false;
  }
  if (gNotes.blur && gNotes.editing) {
    ImGui::ClearActiveID();
    gNotes.blur = false;
  }
  if (gNotes.editing && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::ClearActiveID();

  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(P.field));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.line));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));
  const char* inputId = isTrack ? "##track_notes" : "##notes";
  ImGui::InputTextMultiline(inputId, buf.data(), buf.capacity() + 1, ImGui::GetContentRegionAvail(),
                            ImGuiInputTextFlags_CallbackResize, growStringCallback, &buf);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);

  gNotes.editing = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    if (isTrack && ch) {
      if (ch->notes != buf) {
        const std::string next = buf;
        a.doc.editChannel(ch->id, "edit track notes", [&next](Channel& c) { c.notes = next; });
      }
    } else {
      if (s.info.comments != buf) {  // the project's comments (Project info shows the same text)
        SongInfo info = s.info;
        info.comments = buf;
        a.doc.setInfo(info);
      }
    }
  }
  ui::Hint(isTrack ? "Notes for this track (saved with the project, shared with collaborators)"
                   : "Project notes (saved with the project, shared with collaborators)");
}

// ============================================================================
// 3. SCORE EDITOR (Smooth lower pane: Piano Roll | Score | Audio)
// ============================================================================

struct ScoreState {
  int clefMode = static_cast<int>(ClefMode::Grand);  // 0 Treble, 1 Bass, 2 Grand
  bool userChoseClef = false;
  int quantIdx = 2;      // 0: 1/4, 1: 1/8, 2: 1/16
  int tool = 0;          // 0 Select, 1 Draw
  float pxPerBeat = 72.0f;
  float scrollX = 0.0f;
  ObjectId lastPat = kNoId;
  ObjectId lastCh = kNoId;
  // Dragging a note vertically (pitch) or horizontally (position).
  bool dragging = false;
  size_t dragNoteIdx = 0;
  float dragStartY = 0.0f;
  float dragStartX = 0.0f;
  Note dragBaseNote;
  std::vector<Note> dragBaseList;
  int dragPreviewPitch = -1;
  ObjectId dragPreviewCh = kNoId;
  bool dragChanged = false;
  int lastRenderedNotes = 0;
};

ScoreState gScore;

void stopScorePreview(App& a) {
  if (gScore.dragPreviewPitch >= 0 && gScore.dragPreviewCh != kNoId) {
    a.engine.noteOff(gScore.dragPreviewCh, gScore.dragPreviewPitch);
  }
  gScore.dragPreviewPitch = -1;
  gScore.dragPreviewCh = kNoId;
}

void startScorePreview(App& a, ObjectId chId, int pitch, int vel) {
  if (a.engine.playing()) return;
  if (gScore.dragPreviewPitch == pitch && gScore.dragPreviewCh == chId) return;
  stopScorePreview(a);
  a.engine.noteOn(chId, pitch, std::clamp(vel, 1, 127) / 127.0f);
  gScore.dragPreviewPitch = pitch;
  gScore.dragPreviewCh = chId;
}

// Vector musical glyphs drawn purely with ImDrawList (no font dependency, crisp at any scale).

void drawSharpGlyph(ImDrawList* dl, ImVec2 c, float sp, ImU32 col) {
  const float dx = sp * 0.22f, h = sp * 0.95f, w = sp * 0.45f, tilt = sp * 0.14f;
  dl->AddLine(ImVec2(c.x - dx, c.y - h), ImVec2(c.x - dx, c.y + h), col, 1.3f);
  dl->AddLine(ImVec2(c.x + dx, c.y - h - tilt), ImVec2(c.x + dx, c.y + h - tilt), col, 1.3f);
  dl->AddLine(ImVec2(c.x - w, c.y - sp * 0.25f + tilt), ImVec2(c.x + w, c.y - sp * 0.25f - tilt), col, 2.0f);
  dl->AddLine(ImVec2(c.x - w, c.y + sp * 0.30f + tilt), ImVec2(c.x + w, c.y + sp * 0.30f - tilt), col, 2.0f);
}

void drawFlatGlyph(ImDrawList* dl, ImVec2 c, float sp, ImU32 col) {
  const float x0 = c.x - sp * 0.22f;
  dl->AddLine(ImVec2(x0, c.y - sp * 1.25f), ImVec2(x0, c.y + sp * 0.42f), col, 1.5f);
  dl->AddBezierCubic(ImVec2(x0, c.y - sp * 0.15f), ImVec2(x0 + sp * 0.65f, c.y - sp * 0.55f),
                     ImVec2(x0 + sp * 0.65f, c.y + sp * 0.20f), ImVec2(x0, c.y + sp * 0.45f), col, 1.7f);
}

void drawNaturalGlyph(ImDrawList* dl, ImVec2 c, float sp, ImU32 col) {
  const float dx = sp * 0.20f, yTop = sp * 0.32f;
  dl->AddLine(ImVec2(c.x - dx, c.y - sp * 0.95f), ImVec2(c.x - dx, c.y + yTop), col, 1.3f);
  dl->AddLine(ImVec2(c.x + dx, c.y - yTop), ImVec2(c.x + dx, c.y + sp * 0.95f), col, 1.3f);
  dl->AddLine(ImVec2(c.x - dx, c.y - yTop + sp * 0.08f), ImVec2(c.x + dx, c.y - yTop - sp * 0.08f), col, 1.9f);
  dl->AddLine(ImVec2(c.x - dx, c.y + yTop + sp * 0.08f), ImVec2(c.x + dx, c.y + yTop - sp * 0.08f), col, 1.9f);
}

void drawAccidentalGlyph(ImDrawList* dl, int acc, ImVec2 c, float sp, ImU32 col) {
  if (acc > 0) drawSharpGlyph(dl, c, sp, col);
  else if (acc < 0) drawFlatGlyph(dl, c, sp, col);
  else drawNaturalGlyph(dl, c, sp, col);
}

// Vector Treble (G) clef centred horizontally at cx; `yTop` is F5 line, `sp` is line spacing.
void drawTrebleClef(ImDrawList* dl, float cx, float yTop, float sp, ImU32 col) {
  const float yG = yTop + 3.0f * sp;  // G4 line (2nd from bottom)
  const float yBot = yTop + 4.0f * sp;
  const float th = std::max(1.8f, sp * 0.19f);
  // Central stem with slight classical lean and lower hook.
  dl->AddBezierCubic(ImVec2(cx + sp * 0.10f, yTop - sp * 1.15f), ImVec2(cx - sp * 0.05f, yTop + sp * 1.5f),
                     ImVec2(cx + sp * 0.15f, yBot + sp * 0.8f), ImVec2(cx - sp * 0.12f, yBot + sp * 1.28f), col, th);
  dl->AddCircleFilled(ImVec2(cx - sp * 0.36f, yBot + sp * 1.16f), sp * 0.22f, col, 12);
  dl->AddBezierCubic(ImVec2(cx - sp * 0.12f, yBot + sp * 1.28f), ImVec2(cx - sp * 0.45f, yBot + sp * 1.38f),
                     ImVec2(cx - sp * 0.50f, yBot + sp * 1.05f), ImVec2(cx - sp * 0.36f, yBot + sp * 1.16f), col, th * 0.9f);
  // Top loop rising above F5 and curving down through B4.
  dl->AddBezierCubic(ImVec2(cx + sp * 0.10f, yTop - sp * 1.15f), ImVec2(cx + sp * 0.72f, yTop - sp * 0.55f),
                     ImVec2(cx + sp * 0.65f, yTop + sp * 0.75f), ImVec2(cx - sp * 0.18f, yTop + sp * 1.65f), col, th * 1.15f);
  // Wide belly sweeping down to E4 and curling around G4.
  dl->AddBezierCubic(ImVec2(cx - sp * 0.18f, yTop + sp * 1.65f), ImVec2(cx - sp * 0.98f, yTop + sp * 2.65f),
                     ImVec2(cx - sp * 0.62f, yBot + sp * 0.22f), ImVec2(cx + sp * 0.16f, yBot + sp * 0.02f), col, th * 1.25f);
  dl->AddBezierCubic(ImVec2(cx + sp * 0.16f, yBot + sp * 0.02f), ImVec2(cx + sp * 0.85f, yBot - sp * 0.22f),
                     ImVec2(cx + sp * 0.78f, yG - sp * 0.82f), ImVec2(cx + sp * 0.08f, yG - sp * 0.52f), col, th * 1.15f);
  dl->AddBezierCubic(ImVec2(cx + sp * 0.08f, yG - sp * 0.52f), ImVec2(cx - sp * 0.34f, yG - sp * 0.32f),
                     ImVec2(cx - sp * 0.28f, yG + sp * 0.34f), ImVec2(cx + sp * 0.14f, yG + sp * 0.24f), col, th);
}

// Vector Bass (F) clef; `yTop` is A3 line, `sp` is line spacing.
void drawBassClef(ImDrawList* dl, float cx, float yTop, float sp, ImU32 col) {
  const float yF = yTop + sp;  // F3 line (2nd from top)
  const float th = std::max(2.0f, sp * 0.22f);
  const float x0 = cx - sp * 0.45f;
  dl->AddCircleFilled(ImVec2(x0 + sp * 0.12f, yF), sp * 0.32f, col, 14);
  dl->AddBezierCubic(ImVec2(x0 + sp * 0.05f, yF), ImVec2(x0 + sp * 0.05f, yTop - sp * 0.25f),
                     ImVec2(cx + sp * 0.75f, yTop - sp * 0.15f), ImVec2(cx + sp * 0.62f, yTop + sp * 1.15f), col, th * 1.15f);
  dl->AddBezierCubic(ImVec2(cx + sp * 0.62f, yTop + sp * 1.15f), ImVec2(cx + sp * 0.52f, yTop + sp * 2.25f),
                     ImVec2(cx - sp * 0.10f, yTop + sp * 3.05f), ImVec2(cx - sp * 0.60f, yTop + sp * 3.40f), col, th);
  // Two dots straddling the F3 line.
  const float dotX = cx + sp * 0.80f;
  dl->AddCircleFilled(ImVec2(dotX, yF - sp * 0.44f), sp * 0.17f, col, 10);
  dl->AddCircleFilled(ImVec2(dotX, yF + sp * 0.44f), sp * 0.17f, col, 10);
}

// Grand staff curly brace connecting Treble top to Bass bottom.
void drawGrandBrace(ImDrawList* dl, float x, float y0, float y1, ImU32 col) {
  const float ym = (y0 + y1) * 0.5f;
  const float w = 8.0f;
  dl->AddBezierCubic(ImVec2(x + w, y0), ImVec2(x - 2.0f, y0 + (ym - y0) * 0.22f),
                     ImVec2(x + w * 0.6f, ym - (ym - y0) * 0.18f), ImVec2(x, ym), col, 2.0f);
  dl->AddBezierCubic(ImVec2(x, ym), ImVec2(x + w * 0.6f, ym + (y1 - ym) * 0.18f),
                     ImVec2(x - 2.0f, y1 - (y1 - ym) * 0.22f), ImVec2(x + w, y1), col, 2.0f);
}

// Tilted elliptical notehead (filled or hollow).
void drawNotehead(ImDrawList* dl, ImVec2 c, float rx, float ry, bool hollow, ImU32 col, ImU32 bgCol) {
  const int segs = 18;
  const float tilt = -0.34f;  // classic ~-20 deg engraving slant
  const float ct = std::cos(tilt), st = std::sin(tilt);
  ImVec2 pts[18];
  for (int i = 0; i < segs; ++i) {
    const float a = static_cast<float>(i) * (6.2831853f / static_cast<float>(segs));
    const float lx = std::cos(a) * rx;
    const float ly = std::sin(a) * ry;
    pts[i] = ImVec2(c.x + lx * ct - ly * st, c.y + lx * st + ly * ct);
  }
  dl->AddConvexPolyFilled(pts, segs, col);
  if (hollow) {
    ImVec2 inner[18];
    const float irx = rx * 0.64f, iry = ry * 0.44f;
    for (int i = 0; i < segs; ++i) {
      const float a = static_cast<float>(i) * (6.2831853f / static_cast<float>(segs));
      const float lx = std::cos(a) * irx;
      const float ly = std::sin(a) * iry;
      inner[i] = ImVec2(c.x + lx * ct - ly * st, c.y + lx * st + ly * ct);
    }
    dl->AddConvexPolyFilled(inner, segs, bgCol);
  }
}

void drawFlag(ImDrawList* dl, ImVec2 tip, bool stemUp, int count, float sp, ImU32 col) {
  const float dir = stemUp ? 1.0f : -1.0f;
  for (int f = 0; f < count; ++f) {
    const float oy = dir * static_cast<float>(f) * sp * 0.65f;
    const ImVec2 p0(tip.x, tip.y + oy);
    const ImVec2 p1(tip.x + sp * 0.72f, tip.y + oy + dir * sp * 0.45f);
    const ImVec2 p2(tip.x + sp * 0.85f, tip.y + oy + dir * sp * 1.15f);
    const ImVec2 p3(tip.x + sp * 0.35f, tip.y + oy + dir * sp * 1.65f);
    dl->AddBezierCubic(p0, p1, p2, p3, col, 2.0f);
  }
}

void drawScoreImpl(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ActiveTarget tgt = resolveTarget(a);

  if (tgt.pat && tgt.chId != kNoId) {
    syncSelectionContext(a, tgt.patId, tgt.chId);
    if (gScore.lastPat != tgt.patId || gScore.lastCh != tgt.chId) {
      gScore.lastPat = tgt.patId;
      gScore.lastCh = tgt.chId;
      if (!gScore.userChoseClef) {
        gScore.clefMode = static_cast<int>(autoClefForNotes(notesCopy(tgt.pat, tgt.chId)));
      }
    }
  }

  // Determine key signature from song key marker or piano roll scale.
  int keyRoot = a.piano.scaleRoot;
  int keyScale = a.piano.scale;
  if (tgt.pat) {
    const ops::KeyAt ka = ops::patternKey(s, tgt.patId);
    if (ka.marker >= 0) {
      keyRoot = ka.root;
      keyScale = ka.scale;
    }
  }
  const int keyFifths = keySignatureFifths(keyRoot, keyScale);
  const std::string keyLabel = keySignatureLabel(keyRoot, keyScale);
  const int tsNum = tgt.pat ? s.patternMeterNum(*tgt.pat) : s.timeSigNum;
  const int tsDen = tgt.pat ? s.patternMeterDen(*tgt.pat) : s.timeSigDen;

  // ---- Top local toolbar strip ----
  {
    const float fh = ImGui::GetFrameHeight();
    if (ui::IconButton("sc_sel", ui::Icon::ToolSelect, gScore.tool == 0, fh, "Select and drag notes", theme::litRole(theme::role::kSelect)))
      gScore.tool = 0;
    ImGui::SameLine(0, 3);
    if (ui::IconButton("sc_draw", ui::Icon::ToolDraw, gScore.tool == 1, fh, "Draw notes on the staff", theme::litRole(theme::role::kDraw)))
      gScore.tool = 1;
    ImGui::SameLine(0, 3);
    const bool canDel = tgt.pat && tgt.chId != kNoId && !a.piano.selected.empty();
    ImGui::BeginDisabled(!canDel);
    if (ui::IconButton("sc_del", ui::Icon::Trash, false, fh, "Delete selected notes")) deleteSelectedNotes(a, tgt.patId, tgt.chId);
    ImGui::EndDisabled();

    ImGui::SameLine(0, 12);
    const char* clefs[] = {i18n::tr("Treble"), i18n::tr("Bass"), i18n::tr("Grand")};
    const char* clefHints[] = {i18n::tr("Treble clef staff"), i18n::tr("Bass clef staff"), i18n::tr("Grand staff (Treble and Bass)")};
    int nextClef = gScore.clefMode;
    if (ui::Segmented("##scclef", clefs, 3, &nextClef, 58.0f, 0, clefHints)) {
      gScore.clefMode = nextClef;
      gScore.userChoseClef = true;
    }

    ImGui::SameLine(0, 10);
    const char* quants[] = {"1/4", "1/8", "1/16"};
    const char* qHints[] = {i18n::tr("Display quantize: quarter notes"), i18n::tr("Display quantize: eighth notes"),
                            i18n::tr("Display quantize: sixteenth notes")};
    ui::Segmented("##scquant", quants, 3, &gScore.quantIdx, 38.0f, 0, qHints);

    ImGui::SameLine(0, 10);
    if (ui::IconButton("sc_zout", ui::Icon::Minus, false, fh, "Zoom out score")) gScore.pxPerBeat = std::max(36.0f, gScore.pxPerBeat / 1.2f);
    ImGui::SameLine(0, 2);
    if (ui::IconButton("sc_zin", ui::Icon::Plus, false, fh, "Zoom in score")) gScore.pxPerBeat = std::min(160.0f, gScore.pxPerBeat * 1.2f);

    // Right-aligned key + meter + active track badge.
    ImGui::SameLine(0, 8);
    char badge[96];
    std::snprintf(badge, sizeof badge, "%s  %d/%d%s%s", keyLabel.c_str(), tsNum, tsDen,
                  tgt.ch ? "  ·  " : "", tgt.ch ? tgt.ch->name.c_str() : "");
    const float fs = theme::kFontCaption;
    const float bw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, badge).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > 24.0f) {
      const ImVec2 cp = ImGui::GetCursorScreenPos();
      const float drawX = std::max(cp.x, std::round(cp.x + avail - bw - 4.0f));
      theme::textEllipsis(ImGui::GetWindowDrawList(), a.fontRegular, fs,
                          ImVec2(drawX, theme::textTop(a.fontRegular, fs, cp.y, fh)), avail - 4.0f, P.textDim, badge);
      ImGui::Dummy(ImVec2(avail, fh));
    }
  }

  ImGui::Dummy(ImVec2(0, 2));

  if (!tgt.pat || tgt.chId == kNoId) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 100.0f)), "No active pattern",
                   "Select a track and pattern to view its score");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 100.0f)));
    return;
  }

  // Delete key when Score focused.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
      (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
    deleteSelectedNotes(a, tgt.patId, tgt.chId);
  }

  const auto rawNotes = notesCopy(tgt.pat, tgt.chId);
  const ClefMode clef = static_cast<ClefMode>(std::clamp(gScore.clefMode, 0, 2));
  const QuantGrid qgrid = static_cast<QuantGrid>(std::clamp(gScore.quantIdx, 0, 2));
  const auto scoreNotes = layoutScoreNotes(s, *tgt.pat, rawNotes, clef, qgrid, keyFifths);
  gScore.lastRenderedNotes = static_cast<int>(scoreNotes.size());

  const Tick barTicks = std::max<Tick>(1, s.patternBarTicks(*tgt.pat));
  const Tick beatTicks = std::max<Tick>(1, s.patternBeatTicks(*tgt.pat));
  const Tick patLen = std::max<Tick>(barTicks, s.patternContentLength(*tgt.pat));
  const int numBars = std::max(2, static_cast<int>((patLen + barTicks - 1) / barTicks));
  const float beatsPerBar = static_cast<float>(barTicks) / static_cast<float>(beatTicks);
  const float barW = beatsPerBar * gScore.pxPerBeat;

  const ImVec2 canvasAvail = ImGui::GetContentRegionAvail();
  const float canvasW = std::max(120.0f, canvasAvail.x);
  const float canvasH = std::max(110.0f, canvasAvail.y);

  // Staff vertical layout metrics.
  const bool grand = clef == ClefMode::Grand;
  const float sp = grand ? std::clamp(canvasH / 20.5f, 7.5f, 11.5f) : std::clamp(canvasH / 12.5f, 8.5f, 13.0f);
  const float staffH = 4.0f * sp;
  const float grandGap = 5.2f * sp;
  const float totalH = grand ? staffH * 2.0f + grandGap : staffH;
  const float rulerH = 22.0f;
  const float yTopBase = std::round(rulerH + std::max(sp * 2.2f, (canvasH - rulerH - totalH) * 0.5f));
  const float yTrebleTop = yTopBase;
  const float yBassTop = grand ? (yTrebleTop + staffH + grandGap) : yTopBase;

  // Header area (clef + key sig + time sig) width.
  const int nKeyAcc = std::abs(keyFifths);
  const float headerW = 54.0f + static_cast<float>(nKeyAcc) * (sp * 1.05f) + 34.0f;
  const float contentW = headerW + static_cast<float>(numBars) * barW + 48.0f;
  const float maxScroll = std::max(0.0f, contentW - canvasW);
  gScore.scrollX = std::clamp(gScore.scrollX, 0.0f, maxScroll);

  // Step <-> Y conversions:
  // Treble top line = F5 (step 38); Bass top line = A3 (step 26). Each diatonic step is sp * 0.5.
  auto stepToY = [&](int diatonicStep, bool bass) -> float {
    const float topY = bass ? yBassTop : yTrebleTop;
    const int topStep = bass ? 26 : 38;
    return topY + static_cast<float>(topStep - diatonicStep) * (sp * 0.5f);
  };
  auto yToStaffAndStep = [&](float localY, bool& outBass) -> int {
    if (clef == ClefMode::Bass) outBass = true;
    else if (clef == ClefMode::Treble) outBass = false;
    else outBass = localY > (yTrebleTop + staffH + grandGap * 0.5f);
    const float topY = outBass ? yBassTop : yTrebleTop;
    const int topStep = outBass ? 26 : 38;
    return topStep - static_cast<int>(std::lround((localY - topY) / (sp * 0.5f)));
  };

  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.well));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::BeginChild("##scorecanvas", ImVec2(canvasW, canvasH), ImGuiChildFlags_Borders,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + canvasW, p0.y + canvasH);
    const float pxPerTick = gScore.pxPerBeat / static_cast<float>(beatTicks);
    const float musicX0 = p0.x + headerW - gScore.scrollX;

    // Horizontal wheel scrolling.
    if (ImGui::IsWindowHovered()) {
      const float wx = ImGui::GetIO().MouseWheelH;
      const float wy = ImGui::GetIO().MouseWheel;
      if (wx != 0.0f) gScore.scrollX = std::clamp(gScore.scrollX - wx * 48.0f, 0.0f, maxScroll);
      else if (wy != 0.0f && ImGui::GetIO().KeyShift) gScore.scrollX = std::clamp(gScore.scrollX - wy * 48.0f, 0.0f, maxScroll);
      else if (wy != 0.0f && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper)) {
        gScore.pxPerBeat = std::clamp(gScore.pxPerBeat * (wy > 0 ? 1.12f : 0.89f), 36.0f, 160.0f);
      } else if (wy != 0.0f) {
        gScore.scrollX = std::clamp(gScore.scrollX - wy * 40.0f, 0.0f, maxScroll);
      }
    }

    // Background & top bar-number ruler.
    dl->AddRectFilled(p0, p1, P.well);
    dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + rulerH), P.surface900);
    dl->AddLine(ImVec2(p0.x, p0.y + rulerH), ImVec2(p1.x, p0.y + rulerH), P.line);

    const ImU32 staffCol = theme::withAlpha(P.textDim, 0.58f);
    const ImU32 barCol = theme::withAlpha(P.textDim, 0.80f);
    const ImU32 beatCol = theme::withAlpha(P.textDim, 0.14f);
    const ImU32 glyphCol = P.text;
    const ImU32 noteCol = tgt.ch ? theme::clipInk(theme::content(tgt.ch->color)) : P.text;

    const float sysTop = p0.y + (clef == ClefMode::Bass ? yBassTop : yTrebleTop);
    const float sysBot = p0.y + (clef == ClefMode::Treble ? yTrebleTop + staffH : yBassTop + staffH);

    // Clip scrolling music region to right of sticky clef header.
    dl->PushClipRect(ImVec2(p0.x + headerW, p0.y), p1, true);

    // Alternating bar tint, beat guide lines, bar lines, bar numbers.
    for (int b = 0; b <= numBars; ++b) {
      const float bx = std::round(musicX0 + static_cast<float>(b) * barW);
      if (b < numBars && b % 2 == 1) {
        dl->AddRectFilled(ImVec2(bx, p0.y + rulerH), ImVec2(bx + barW, p1.y), theme::withAlpha(P.text, 0.025f));
      }
      if (b < numBars) {
        char bnum[16];
        std::snprintf(bnum, sizeof bnum, "%d", b + 1);
        dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(bx + 5.0f, theme::textTop(a.fontRegular, theme::kFontLabel, p0.y, rulerH)),
                    P.textDim, bnum);
        for (int bt = 1; bt < tsNum; ++bt) {
          const float btx = std::round(bx + static_cast<float>(bt) * gScore.pxPerBeat);
          dl->AddLine(ImVec2(btx, sysTop), ImVec2(btx, sysBot), beatCol, 1.0f);
        }
      }
      if (b == numBars) {
        // Double final bar line.
        dl->AddLine(ImVec2(bx - 4.0f, sysTop), ImVec2(bx - 4.0f, sysBot), barCol, 1.0f);
        dl->AddLine(ImVec2(bx, sysTop), ImVec2(bx, sysBot), barCol, 2.5f);
      } else if (b > 0) {
        dl->AddLine(ImVec2(bx, sysTop), ImVec2(bx, sysBot), barCol, 1.1f);
      }
    }

    // Staff lines across the music region.
    const float staffEndX = musicX0 + static_cast<float>(numBars) * barW;
    auto drawStaffLines = [&](float topY, float xStart, float xEnd) {
      for (int l = 0; l < 5; ++l) {
        const float y = std::round(p0.y + topY + static_cast<float>(l) * sp) + 0.5f;
        dl->AddLine(ImVec2(xStart, y), ImVec2(xEnd, y), staffCol, 1.0f);
      }
    };
    if (clef != ClefMode::Bass) drawStaffLines(yTrebleTop, p0.x + headerW, staffEndX);
    if (clef != ClefMode::Treble) drawStaffLines(yBassTop, p0.x + headerW, staffEndX);

    // Precompute screen coordinates & stem endpoints for every score note (also used for beaming & hit-testing).
    struct DrawnNote {
      size_t snIdx;
      ImVec2 head;
      ImVec2 stemTip;
      float rx, ry;
    };
    std::vector<DrawnNote> drawn;
    drawn.reserve(scoreNotes.size());
    const float rx = sp * 0.62f;
    const float ry = sp * 0.44f;
    const float stemLen = sp * 3.35f;

    // Collect beam groups.
    std::map<int, std::vector<size_t>> beams;
    for (size_t i = 0; i < scoreNotes.size(); ++i) {
      const ScoreNote& sn = scoreNotes[i];
      const float nx = musicX0 + static_cast<float>(sn.qStart) * pxPerTick + rx + 4.0f;
      const float ny = p0.y + stepToY(sn.diatonicStep, sn.bassStaff);
      const float stemX = sn.stemUp ? (nx + rx - 1.0f) : (nx - rx + 1.0f);
      const float tipY = sn.stemUp ? (ny - stemLen) : (ny + stemLen);
      drawn.push_back({i, ImVec2(nx, ny), ImVec2(stemX, tipY), rx, ry});
      if (sn.beamGroup >= 0) beams[sn.beamGroup].push_back(i);
    }

    // Align beamed stem tips onto a clean sloped beam line.
    struct BeamBar {
      ImVec2 a, b;
      bool stemUp;
      int maxFlags;
      bool anySelected;
      std::vector<size_t> members;
    };
    std::vector<BeamBar> beamBars;
    for (const auto& [gid, idxs] : beams) {
      if (idxs.size() < 2) continue;
      const bool up = scoreNotes[idxs.front()].stemUp;
      const size_t first = idxs.front(), last = idxs.back();
      float xA = drawn[first].stemTip.x, yA = drawn[first].stemTip.y;
      float xB = drawn[last].stemTip.x, yB = drawn[last].stemTip.y;
      // Limit beam slope to keep engraving balanced.
      const float dx = std::max(1.0f, xB - xA);
      const float maxDy = std::min(sp * 1.4f, dx * 0.22f);
      const float dy = std::clamp(yB - yA, -maxDy, maxDy);
      const float midY = (yA + yB) * 0.5f;
      yA = midY - dy * 0.5f;
      yB = midY + dy * 0.5f;
      // Ensure every member stem has at least 2.3*sp length.
      float shift = 0.0f;
      int maxF = 1;
      bool anySel = false;
      for (size_t idx : idxs) {
        const float t = (drawn[idx].stemTip.x - xA) / dx;
        const float lineY = yA + (yB - yA) * t;
        const float minTipY = up ? (drawn[idx].head.y - sp * 2.4f) : (drawn[idx].head.y + sp * 2.4f);
        if (up && lineY > minTipY) shift = std::min(shift, minTipY - lineY);
        if (!up && lineY < minTipY) shift = std::max(shift, minTipY - lineY);
        maxF = std::max(maxF, flagCount(scoreNotes[idx].value));
        anySel = anySel || a.piano.selected.count(scoreNotes[idx].noteIndex) > 0;
      }
      yA += shift;
      yB += shift;
      for (size_t idx : idxs) {
        const float t = (drawn[idx].stemTip.x - xA) / dx;
        drawn[idx].stemTip.y = yA + (yB - yA) * t;
      }
      beamBars.push_back({ImVec2(xA, yA), ImVec2(xB, yB), up, maxF, anySel, idxs});
    }

    // Draw notes (ledger lines, accidentals, noteheads, dots, stems, standalone flags).
    for (const DrawnNote& dn : drawn) {
      const ScoreNote& sn = scoreNotes[dn.snIdx];
      const bool sel = a.piano.selected.count(sn.noteIndex) > 0;
      const ImU32 col = sel ? P.accent : noteCol;

      // Ledger lines above / below staff.
      const int botLine = sn.bassStaff ? 18 : 30;
      const int topLine = sn.bassStaff ? 26 : 38;
      const ImU32 ledgerCol = theme::withAlpha(P.text, 0.72f);
      if (sn.diatonicStep <= botLine - 2) {
        for (int stp = botLine - 2; stp >= sn.diatonicStep; stp -= 2) {
          const float ly = std::round(p0.y + stepToY(stp, sn.bassStaff)) + 0.5f;
          dl->AddLine(ImVec2(dn.head.x - rx * 1.55f, ly), ImVec2(dn.head.x + rx * 1.55f, ly), ledgerCol, 1.15f);
        }
      } else if (sn.diatonicStep >= topLine + 2) {
        for (int stp = topLine + 2; stp <= sn.diatonicStep; stp += 2) {
          const float ly = std::round(p0.y + stepToY(stp, sn.bassStaff)) + 0.5f;
          dl->AddLine(ImVec2(dn.head.x - rx * 1.55f, ly), ImVec2(dn.head.x + rx * 1.55f, ly), ledgerCol, 1.15f);
        }
      }

      // Selection halo.
      if (sel) {
        dl->AddCircleFilled(dn.head, rx * 1.65f, theme::withAlpha(P.accent, 0.25f), 16);
      }

      // Accidental to the left of the notehead.
      if (sn.drawnAccidental >= -1) {
        drawAccidentalGlyph(dl, sn.drawnAccidental, ImVec2(dn.head.x - rx * 2.15f, dn.head.y), sp, col);
      }

      // Notehead.
      drawNotehead(dl, dn.head, dn.rx, dn.ry, isHollowHead(sn.value), col, P.well);

      // Augmentation dot for dotted half / quarter.
      if (isDotted(sn.value)) {
        const float dotY = (sn.diatonicStep % 2 == 0) ? (dn.head.y - sp * 0.28f) : dn.head.y;
        dl->AddCircleFilled(ImVec2(dn.head.x + rx * 1.65f, dotY), sp * 0.17f, col, 10);
      }

      // Stem & unbeamed flags.
      if (hasStem(sn.value)) {
        dl->AddLine(ImVec2(dn.stemTip.x, dn.head.y), dn.stemTip, col, 1.55f);
        if (sn.beamGroup < 0 && flagCount(sn.value) > 0) {
          drawFlag(dl, dn.stemTip, sn.stemUp, flagCount(sn.value), sp, col);
        }
      }
    }

    // Draw beam bars connecting beamed 8th/16th runs.
    for (const BeamBar& bb : beamBars) {
      const ImU32 bcol = bb.anySelected ? P.accent : noteCol;
      const float bthick = std::max(3.0f, sp * 0.38f);
      const float dir = bb.stemUp ? 1.0f : -1.0f;
      // Primary eighth beam.
      ImVec2 q[4] = {bb.a, bb.b, ImVec2(bb.b.x, bb.b.y + dir * bthick), ImVec2(bb.a.x, bb.a.y + dir * bthick)};
      dl->AddConvexPolyFilled(q, 4, bcol);
      // Secondary sixteenth sub-beam where adjacent members are 16ths.
      if (bb.maxFlags >= 2) {
        const float off = dir * (bthick + sp * 0.26f);
        for (size_t k = 0; k < bb.members.size(); ++k) {
          const size_t idx = bb.members[k];
          if (flagCount(scoreNotes[idx].value) < 2) continue;
          const ImVec2 t0 = drawn[idx].stemTip;
          ImVec2 t1 = t0;
          if (k + 1 < bb.members.size() && flagCount(scoreNotes[bb.members[k + 1]].value) >= 2) {
            t1 = drawn[bb.members[k + 1]].stemTip;
          } else if (k > 0 && flagCount(scoreNotes[bb.members[k - 1]].value) >= 2) {
            continue;  // already covered by previous segment
          } else {
            const float stub = (k + 1 < bb.members.size()) ? sp * 0.85f : -sp * 0.85f;
            const float dx = std::max(1.0f, bb.b.x - bb.a.x);
            t1 = ImVec2(t0.x + stub, t0.y + (bb.b.y - bb.a.y) * (stub / dx));
          }
          ImVec2 q2[4] = {ImVec2(t0.x, t0.y + off), ImVec2(t1.x, t1.y + off), ImVec2(t1.x, t1.y + off + dir * bthick * 0.85f),
                          ImVec2(t0.x, t0.y + off + dir * bthick * 0.85f)};
          dl->AddConvexPolyFilled(q2, 4, bcol);
        }
      }
    }

    // Playhead line sweeping cleanly during playback.
    {
      double playTick = a.engine.position();
      if (a.engine.mode() == PlayMode::Song) {
        const Tick patSongStart = ops::patternSongPosition(s, tgt.patId);
        if (playTick >= static_cast<double>(patSongStart)) playTick -= static_cast<double>(patSongStart);
      }
      if (patLen > 0) playTick = std::fmod(std::max(0.0, playTick), static_cast<double>(patLen));
      const float px = std::round(musicX0 + static_cast<float>(playTick) * pxPerTick) + 0.5f;
      if (px >= p0.x + headerW && px <= p1.x) {
        dl->AddLine(ImVec2(px, p0.y), ImVec2(px, p1.y), P.playhead, 1.8f);
        dl->AddTriangleFilled(ImVec2(px - 5.0f, p0.y), ImVec2(px + 5.0f, p0.y), ImVec2(px, p0.y + 7.0f), P.playhead);
      }
    }

    dl->PopClipRect();

    // ---- Sticky Left Header (Clefs, Brace, Key Signature, Time Signature) ----
    dl->AddRectFilled(ImVec2(p0.x, p0.y + rulerH), ImVec2(p0.x + headerW, p1.y), P.well);
    dl->AddLine(ImVec2(p0.x + headerW, p0.y + rulerH), ImVec2(p0.x + headerW, p1.y), theme::withAlpha(P.line, 0.5f));
    if (clef != ClefMode::Bass) drawStaffLines(yTrebleTop, p0.x + 14.0f, p0.x + headerW);
    if (clef != ClefMode::Treble) drawStaffLines(yBassTop, p0.x + 14.0f, p0.x + headerW);
    // Left system bar line & grand brace.
    dl->AddLine(ImVec2(p0.x + 14.0f, sysTop), ImVec2(p0.x + 14.0f, sysBot), barCol, 1.4f);
    if (grand) drawGrandBrace(dl, p0.x + 4.0f, sysTop, sysBot, glyphCol);

    if (clef != ClefMode::Bass) drawTrebleClef(dl, p0.x + 32.0f, p0.y + yTrebleTop, sp, glyphCol);
    if (clef != ClefMode::Treble) drawBassClef(dl, p0.x + 32.0f, p0.y + yBassTop, sp, glyphCol);

    // Key signature accidentals.
    const float keyX0 = p0.x + 54.0f;
    if (keyFifths != 0) {
      const int acc = keyFifths > 0 ? 1 : -1;
      if (clef != ClefMode::Bass) {
        const auto steps = keySigSteps(keyFifths, true);
        for (size_t k = 0; k < steps.size(); ++k)
          drawAccidentalGlyph(dl, acc, ImVec2(keyX0 + static_cast<float>(k) * sp * 1.02f, p0.y + stepToY(steps[k], false)), sp, glyphCol);
      }
      if (clef != ClefMode::Treble) {
        const auto steps = keySigSteps(keyFifths, false);
        for (size_t k = 0; k < steps.size(); ++k)
          drawAccidentalGlyph(dl, acc, ImVec2(keyX0 + static_cast<float>(k) * sp * 1.02f, p0.y + stepToY(steps[k], true)), sp, glyphCol);
      }
    }

    // Time signature.
    const float tsX = keyX0 + static_cast<float>(nKeyAcc) * sp * 1.05f + 12.0f;
    const float tsFont = std::clamp(sp * 2.15f, 14.0f, 24.0f);
    char numStr[8], denStr[8];
    std::snprintf(numStr, sizeof numStr, "%d", tsNum);
    std::snprintf(denStr, sizeof denStr, "%d", tsDen);
    auto drawTimeSig = [&](float topY) {
      const float nw = a.fontBold->CalcTextSizeA(tsFont, 1e9f, 0, numStr).x;
      const float dw = a.fontBold->CalcTextSizeA(tsFont, 1e9f, 0, denStr).x;
      dl->AddText(a.fontBold, tsFont, ImVec2(std::round(tsX - nw * 0.5f), std::round(p0.y + topY - sp * 0.15f)), glyphCol, numStr);
      dl->AddText(a.fontBold, tsFont, ImVec2(std::round(tsX - dw * 0.5f), std::round(p0.y + topY + 2.0f * sp - sp * 0.15f)), glyphCol, denStr);
    };
    if (clef != ClefMode::Bass) drawTimeSig(yTrebleTop);
    if (clef != ClefMode::Treble) drawTimeSig(yBassTop);

    // ---- Interactive mouse handling ----
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##scorehit", ImVec2(canvasW, canvasH));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mp = ImGui::GetIO().MousePos;

    // Hit-test noteheads.
    auto hitTestNote = [&](ImVec2 pos) -> int {
      int best = -1;
      float bestDist = (rx + 7.0f) * (rx + 7.0f);
      for (size_t i = 0; i < drawn.size(); ++i) {
        const float dx = pos.x - drawn[i].head.x;
        const float dy = pos.y - drawn[i].head.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 <= bestDist) {
          bestDist = d2;
          best = static_cast<int>(i);
        }
      }
      return best;
    };

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (mp.y < p0.y + rulerH && mp.x >= p0.x + headerW) {
        // Click in ruler: move playhead.
        const Tick t = std::max<Tick>(0, static_cast<Tick>(std::llround((mp.x - musicX0) / pxPerTick)));
        a.engine.setPosition(t);
      } else if (mp.x >= p0.x + headerW) {
        const int hit = hitTestNote(mp);
        if (hit >= 0) {
          const ScoreNote& sn = scoreNotes[drawn[static_cast<size_t>(hit)].snIdx];
          const bool multi = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
          if (multi) {
            if (!a.piano.selected.erase(sn.noteIndex)) a.piano.selected.insert(sn.noteIndex);
          } else if (!a.piano.selected.count(sn.noteIndex)) {
            a.piano.selected.clear();
            a.piano.selected.insert(sn.noteIndex);
          }
          if (sn.noteIndex < rawNotes.size()) {
            gScore.dragging = true;
            gScore.dragNoteIdx = sn.noteIndex;
            gScore.dragStartX = mp.x;
            gScore.dragStartY = mp.y;
            gScore.dragBaseNote = rawNotes[sn.noteIndex];
            gScore.dragBaseList = rawNotes;
            gScore.dragChanged = false;
            startScorePreview(a, tgt.chId, gScore.dragBaseNote.pitch, gScore.dragBaseNote.velocity);
          }
        } else if (gScore.tool == 1 || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          // Draw tool (or double-click): add a new note on the staff!
          bool onBass = false;
          const int step = yToStaffAndStep(mp.y - p0.y, onBass);
          const int midiPitch = diatonicToMidi(step, keyFifths);
          const Tick rawTick = std::max<Tick>(0, static_cast<Tick>(std::llround((mp.x - musicX0 - rx - 4.0f) / pxPerTick)));
          const Tick qTick = quantizeTick(rawTick, s.ppq, qgrid);
          const Tick dur = gridStepTicks(s.ppq, qgrid);
          auto ns = rawNotes;
          const size_t newIdx = ns.size();
          ns.emplace_back(qTick, dur, midiPitch, std::clamp(a.piano.lastVelocity, 1, 127));
          a.doc.setNotes(tgt.patId, tgt.chId, std::move(ns), "add score note");
          a.piano.selected.clear();
          a.piano.selected.insert(newIdx);
          startScorePreview(a, tgt.chId, midiPitch, std::clamp(a.piano.lastVelocity, 1, 127));
        } else {
          a.piano.selected.clear();
        }
      }
    }

    if (gScore.dragging) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && gScore.dragNoteIdx < gScore.dragBaseList.size()) {
        const float dy = gScore.dragStartY - mp.y;
        const float dx = mp.x - gScore.dragStartX;
        const int semiDelta = static_cast<int>(std::lround(dy / std::max(4.0f, sp * 0.55f)));
        Note edited = gScore.dragBaseNote;
        edited.pitch = std::clamp(gScore.dragBaseNote.pitch + semiDelta, 0, 127);
        if (std::fabs(dx) > 10.0f) {
          const Tick step = gridStepTicks(s.ppq, qgrid);
          const Tick dt = static_cast<Tick>(std::llround((dx / pxPerTick) / static_cast<float>(step))) * step;
          edited.start = std::max<Tick>(0, gScore.dragBaseNote.start + dt);
        }
        if (edited.pitch != gScore.dragPreviewPitch) startScorePreview(a, tgt.chId, edited.pitch, edited.velocity);
        if (!(edited == gScore.dragBaseNote)) {
          auto next = gScore.dragBaseList;
          next[gScore.dragNoteIdx] = edited;
          a.doc.setNotes(tgt.patId, tgt.chId, std::move(next), "move score note", 0x5C0AE001ull + gScore.dragNoteIdx);
          gScore.dragChanged = true;
        }
      } else {
        stopScorePreview(a);
        if (gScore.dragChanged) a.doc.endGesture();
        gScore.dragging = false;
        gScore.dragChanged = false;
      }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && gScore.dragPreviewPitch >= 0) {
      stopScorePreview(a);
    }

    // Hover hint showing pitch & beat under cursor.
    if (hovered && mp.x >= p0.x + headerW && mp.y >= p0.y + rulerH) {
      bool b = false;
      const int stp = yToStaffAndStep(mp.y - p0.y, b);
      const int p = diatonicToMidi(stp, keyFifths);
      const Tick t = std::max<Tick>(0, static_cast<Tick>(std::llround((mp.x - musicX0) / pxPerTick)));
      a.setHint(formatPitch(p) + "  ·  " + formatBbtPos(s, t, tgt.pat));
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

}  // namespace

void drawEventList(App& a) { drawEventListImpl(a); }
void drawNotes(App& a) { drawNotesImpl(a); }
void drawScore(App& a) { drawScoreImpl(a); }

bool devCommand(App& a, const std::vector<std::string>& args) {
  if (args.size() < 2) return false;
  if (args[0] == "notes") {
    std::string rest;
    for (size_t i = 2; i < args.size(); ++i) rest += (i > 2 ? " " : "") + args[i];
    if (args[1] == "focus") {
      gNotes.focus = true;
    } else if (args[1] == "blur") {
      gNotes.blur = true;
    } else if (args[1] == "tab") {
      gNotes.tab = (rest == "track" || rest == "1") ? 1 : 0;
    } else if (args[1] == "set" && !rest.empty()) {
      std::ifstream f(rest, std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      SongInfo info = a.doc.song().info;
      info.comments = text;
      a.doc.setInfo(info);
    } else if (args[1] == "projtext") {
      SongInfo info = a.doc.song().info;
      info.comments = rest;
      a.doc.setInfo(info);
    } else if (args[1] == "dump" && !rest.empty()) {
      const Song& s = a.doc.song();
      writeFileAtomic(rest, s.info.comments);
    } else if (args[1] == "trackset" && !rest.empty()) {
      const ActiveTarget tgt = resolveTarget(a);
      if (tgt.chId != kNoId) {
        std::ifstream f(rest, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        a.doc.editChannel(tgt.chId, "edit track notes", [&text](Channel& c) { c.notes = text; });
      }
    } else if (args[1] == "tracktext") {
      const ActiveTarget tgt = resolveTarget(a);
      if (tgt.chId != kNoId) {
        a.doc.editChannel(tgt.chId, "edit track notes", [&rest](Channel& c) { c.notes = rest; });
      }
    } else if (args[1] == "trackdump" && !rest.empty()) {
      const ActiveTarget tgt = resolveTarget(a);
      writeFileAtomic(rest, tgt.ch ? tgt.ch->notes : std::string{});
    } else if (args[1] == "state") {
      const Song& s = a.doc.song();
      const ActiveTarget tgt = resolveTarget(a);
      std::printf("notes tab %d projLen %zu trackLen %zu\n", gNotes.tab, s.info.comments.size(), tgt.ch ? tgt.ch->notes.size() : 0u);
    }
    return true;
  }
  if (args[0] == "eventlist") {
    const ActiveTarget tgt = resolveTarget(a);
    if (args[1] == "tab" && args.size() >= 3) {
      if (args[2] == "event" || args[2] == "0") gEventList.tab = 0;
      else if (args[2] == "marker" || args[2] == "1") gEventList.tab = 1;
      else if (args[2] == "tempo" || args[2] == "2") gEventList.tab = 2;
      else if (args[2] == "signature" || args[2] == "3") gEventList.tab = 3;
    } else if (args[1] == "filter" && args.size() >= 4) {
      const bool on = args[3] != "0";
      if (args[2] == "notes") gEventList.filter.notes = on;
      else if (args[2] == "velocity") gEventList.filter.velocity = on;
      else if (args[2] == "modcc") gEventList.filter.modCc = on;
    } else if (args[1] == "add") {
      addDefaultEventNote(a);
    } else if (args[1] == "delete") {
      deleteSelectedNotes(a, tgt.patId, tgt.chId);
    } else if (args[1] == "select" && args.size() >= 3) {
      if (tgt.pat && tgt.chId != kNoId) {
        syncSelectionContext(a, tgt.patId, tgt.chId);
        a.piano.selected.clear();
        a.piano.selected.insert(static_cast<size_t>(std::atoi(args[2].c_str())));
      }
    } else if (args[1] == "state") {
      std::vector<EventRow> rows;
      if (tgt.pat && tgt.chId != kNoId) rows = buildEventRows(a.doc.song(), *tgt.pat, tgt.chId, gEventList.filter);
      std::printf("eventlist tab %d rows %zu sel %zu notes %d vel %d modcc %d\n", gEventList.tab, rows.size(),
                  a.piano.selected.size(), gEventList.filter.notes ? 1 : 0, gEventList.filter.velocity ? 1 : 0,
                  gEventList.filter.modCc ? 1 : 0);
    }
    return true;
  }
  if (args[0] == "score") {
    if (args[1] == "clef" && args.size() >= 3) {
      gScore.userChoseClef = true;
      if (args[2] == "treble" || args[2] == "0") gScore.clefMode = 0;
      else if (args[2] == "bass" || args[2] == "1") gScore.clefMode = 1;
      else gScore.clefMode = 2;
    } else if (args[1] == "quant" && args.size() >= 3) {
      if (args[2] == "4" || args[2] == "0") gScore.quantIdx = 0;
      else if (args[2] == "8" || args[2] == "1") gScore.quantIdx = 1;
      else gScore.quantIdx = 2;
    } else if (args[1] == "tool" && args.size() >= 3) {
      gScore.tool = (args[2] == "draw" || args[2] == "1") ? 1 : 0;
    } else if (args[1] == "state") {
      std::printf("score clef %d quant %d tool %d rendered %d sel %zu\n", gScore.clefMode, gScore.quantIdx, gScore.tool,
                  gScore.lastRenderedNotes, a.piano.selected.size());
    }
    return true;
  }
  return false;
}

}  // namespace gl::smoothpanels
