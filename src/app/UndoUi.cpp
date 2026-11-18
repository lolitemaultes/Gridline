#include "UndoUi.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../collab/Session.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "App.h"
#include "CollabUi.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LiveLook.h"
#include "LogicPlugins.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::undoui {
namespace {

using theme::pal;

// What was selected around a step: the piano roll's notes (by value, so they are found again
// after the step is undone or redone) and the playlist's clips.
struct Selection {
  ObjectId pattern = kNoId, channel = kNoId;
  std::vector<Note> notes;
  std::set<ObjectId> clips;
  bool operator==(const Selection& o) const {
    if (pattern != o.pattern || channel != o.channel || clips != o.clips || notes.size() != o.notes.size()) return false;
    for (size_t i = 0; i < notes.size(); ++i)
      if (notes[i].start != o.notes[i].start || notes[i].pitch != o.notes[i].pitch || notes[i].length != o.notes[i].length) return false;
    return true;
  }
};

struct SelectionNote final : StepNote {
  Selection before, after;
  size_t approxBytes() const override {
    return 64 + (before.notes.size() + after.notes.size()) * sizeof(Note) + (before.clips.size() + after.clips.size()) * 40;
  }
};

const std::vector<Note>* noteList(const Song& s, ObjectId pattern, ObjectId channel) {
  const Pattern* p = s.pattern(pattern);
  if (!p) return nullptr;
  auto it = p->notes.find(channel);
  return it == p->notes.end() ? nullptr : &it->second;
}

Selection capture(const App& a) {
  Selection sel;
  sel.pattern = a.piano.selPattern;
  sel.channel = a.piano.selChannel;
  if (const auto* list = noteList(a.doc.song(), sel.pattern, sel.channel))
    for (size_t i : a.piano.selected)
      if (i < list->size()) sel.notes.push_back((*list)[i]);
  sel.clips = a.playlist.selected;
  return sel;
}

void restore(App& a, const Selection& sel) {
  // The piano roll: only while it still shows the same notes list.
  if (sel.pattern == a.piano.selPattern && sel.channel == a.piano.selChannel) {
    a.piano.selected.clear();
    if (const auto* list = noteList(a.doc.song(), sel.pattern, sel.channel)) {
      std::vector<bool> taken(list->size(), false);
      for (const Note& w : sel.notes)
        for (size_t i = 0; i < list->size(); ++i) {
          const Note& n = (*list)[i];
          if (!taken[i] && n.start == w.start && n.pitch == w.pitch && n.length == w.length) {
            taken[i] = true;
            a.piano.selected.insert(i);
            break;
          }
        }
    }
  }
  a.playlist.selected.clear();
  for (ObjectId id : sel.clips)
    if (a.doc.song().clip(id)) a.playlist.selected.insert(id);
}

// Per frame: the selection when the frame started, and what the history looked like.
struct Tracker {
  bool init = false;
  Selection last;
  uint64_t pushes = 0, serial = 0;
  size_t size = 0;
  int queued = 0;        // an Undo / Redo pressed while an edit was still open
  int queuedFrames = 0;
  int claimedFrame = -1, claimedDir = 0;
  double movedAt = -1e9;
};
Tracker& tr() {
  static Tracker t;
  return t;
}

// Steps made since the last frame get the selection from before them and after them.
void noteSteps(App& a) {
  Tracker& t = tr();
  const UndoStack& h = a.doc.history();
  const Selection now = capture(a);
  if (t.init && h.position() > 0 && h.position() == h.size()) {
    const size_t top = h.position() - 1;
    const bool newStep = a.doc.pushCount() != t.pushes || h.size() != t.size;
    const bool changed = h.stateSerial() != t.serial;
    auto* n = dynamic_cast<SelectionNote*>(a.doc.stepNote(top, NoteSlot::Ui));
    if (newStep && !n) {
      auto note = std::make_shared<SelectionNote>();
      note->before = t.last;
      note->after = now;
      a.doc.setStepNote(top, NoteSlot::Ui, note);
    } else if (n && (changed || newStep) && !(n->after == now)) {
      n->after = now;  // a step still growing (a drag, merged edits)
      a.doc.touchStepNote(top);
    }
  }
  t.init = true;
  t.last = now;
  t.pushes = a.doc.pushCount();
  t.serial = h.stateSerial();
  t.size = h.size();
}

bool anyPopup() { return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel); }

// N49-06: in Clips, Undo and Redo wait while anything records (the reference ignores them then): an
// undo in the middle of a take cut it short, and the take was lost from the history.
bool recordingBlocks(App& a) {
  if (!livelook::active(a)) return false;
  if (!(a.engine.recordingOn() || a.recorder.active() || livelook::sessionRecording(a) || livelook::audioTakePending(a))) return false;
  a.setHint(i18n::tr("Stop recording to undo"));
  a.notify(i18n::tr("Stop recording to undo"));
  return true;
}

bool step(App& a, int dir) {
  if (recordingBlocks(a)) return false;
  const bool moved = dir < 0 ? a.doc.undo() : a.doc.redo();
  if (!moved && !collabui::session(a)) {
    const UndoStack& h = a.doc.history();
    if (dir < 0 ? !h.canUndo() : !h.canRedo()) a.setHint(i18n::tr(dir < 0 ? "Nothing to undo" : "Nothing to redo"));
  }
  return moved;
}

}  // namespace

unsigned long long gHistoryMoves = 0;

unsigned long long historyMoves() { return gHistoryMoves; }

void install(App& a) {
  App* ap = &a;
  a.doc.onHistoryMove = [ap](int dir, size_t index) {
    ++gHistoryMoves;
    if (auto* n = dynamic_cast<SelectionNote*>(ap->doc.stepNote(index, NoteSlot::Ui))) restore(*ap, dir < 0 ? n->before : n->after);
    Tracker& t = tr();  // what undo / redo did is not a new step
    t.movedAt = ap->now;
    t.last = capture(*ap);
    t.pushes = ap->doc.pushCount();
    t.serial = ap->doc.history().stateSerial();
    t.size = ap->doc.history().size();
  };
}

bool editorPressed(int dir) {
  if (anyPopup()) return false;
  Tracker& t = tr();
  const int frame = ImGui::GetFrameCount();
  if (t.claimedFrame == frame) return t.claimedDir == dir;
  if (!keys::pressed(dir < 0 ? "edit.undo" : "edit.redo")) return false;
  t.claimedFrame = frame;
  t.claimedDir = dir;
  return true;
}

double lastMoveAt() { return tr().movedAt; }

bool undo(App& a) { return step(a, -1); }
bool redo(App& a) { return step(a, 1); }

void frame(App& a) {
  Tracker& t = tr();
  noteSteps(a);
  if (auto* s = collabui::session(a)) s->setSkipEmptyUndo(a.settings.getBool("undo.skipEmpty"));
  // A press that waited for an open edit (a drag, a recording pass) to close.
  if (t.queued != 0) {
    if (a.doc.groupDepth() == 0) {
      const int dir = t.queued;
      t.queued = 0;
      step(a, dir);
    } else if (++t.queuedFrames > 120) {
      t.queued = 0;
    }
  }
  if (t.claimedFrame == ImGui::GetFrameCount() || anyPopup()) return;
  int dir = 0;
  if (keys::pressed("edit.undo")) dir = -1;
  else if (keys::pressed("edit.redo")) dir = 1;
  if (dir == 0) return;
  if (recordingBlocks(a)) return;
  if (a.doc.groupDepth() > 0) {
    t.queued = dir;
    t.queuedFrames = 0;
    return;
  }
  step(a, dir);
}

void historyWindow(App& a) {
  static bool wasShown = false;
  if (!a.showHistory) {
    wasShown = false;
    return;
  }
  // J-40: opening it (menu, Alt+Z, --show history) brings it to the front,
  // also when it is docked behind another tab (the browser by default).
  const bool opened = !wasShown;
  wasShown = true;
  if (opened) ImGui::SetNextWindowFocus();
  ImGui::SetNextWindowSize(ImVec2(300, 420), ImGuiCond_FirstUseEver);
  // K-314 (C-03): where it floats (Smooth), it opens centred as the other dialogs do, not at the corner.
  if (layoutui::active() != layout::Preset::Studio && layout::windowHome("Undo history", layoutui::active()).kind == layout::HomeKind::Float) {
    ui::PlaceNextDialog(ImVec2(0, 0), ImGuiCond_Appearing);
  }
  // K-112b: the Tracks layout lists the steps as a table: a header (step number, action), striped
  // rows edge to edge with the number right-aligned in its column, and Undo / Redo at the bottom right.
  const bool table = logicplug::active(a);
  if (table) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const bool open = ImGui::Begin(ui::windowTitle("Undo history").c_str(), &a.showHistory);
  if (table) ImGui::PopStyleVar();
  if (!open) {
    ImGui::End();
    return;
  }
  const collab::Session* session = collabui::session(a);
  bool showPeers = a.settings.getBool("undo.showCollaborators");
  if (session) {
    if (table) ImGui::SetCursorPos(ImVec2(theme::kS3, theme::kS2 + ImGui::GetCursorPosY()));
    if (ImGui::Checkbox(i18n::tr("Show collaborators' edits"), &showPeers)) a.settings.set("undo.showCollaborators", showPeers);
    ui::Hint("Lists what others changed between your steps, greyed. Undo only ever goes back through your own steps.");
    ImGui::Separator();
  }
  const UndoStack& h = a.doc.history();
  const size_t n = h.size();
  const auto& P = pal();
  const float numW = 52.0f, rowH = ImGui::GetFontSize() + 10.0f, footH = table ? ImGui::GetFrameHeight() + 2 * theme::kS3 : 0.0f;
  if (table) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, hh = ImGui::GetFontSize() + 8.0f;
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + hh), theme::mix(P.surface800, P.text, 0.04f));
    dl->AddLine(ImVec2(q.x, q.y + hh - 1), ImVec2(q.x + w, q.y + hh - 1), P.dark ? theme::withAlpha(P.ink, 0.6f) : P.lineStrong);
    dl->AddLine(ImVec2(q.x + numW + 4, q.y + 4), ImVec2(q.x + numW + 4, q.y + hh - 5), theme::withAlpha(P.textDim, 0.4f));
    const float ty = std::round(q.y + (hh - ImGui::GetFontSize()) * 0.5f);
    const char* stepH = i18n::tr("Step");
    dl->AddText(ImVec2(std::round(q.x + numW - ImGui::CalcTextSize(stepH).x - 4), ty), P.textDim, stepH);
    dl->AddText(ImVec2(q.x + numW + 12, ty), P.textDim, i18n::tr("Action"));
    ImGui::Dummy(ImVec2(w, hh));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  }
  ImGui::BeginChild("##steps", ImVec2(0, -footH));
  // One row of the table: stripes, the number right-aligned, the action after it; the current step lit.
  auto tableRow = [&](size_t i, const char* text, bool current, bool applied) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (i % 2 == 1) dl->AddRectFilled(q, ImVec2(q.x + w, q.y + rowH), theme::withAlpha(P.text, 0.035f));
    ImGui::PushID(static_cast<int>(i));
    const bool hit = ImGui::Selectable("##row", current, 0, ImVec2(w, rowH));
    ImGui::PopID();
    const float ty = std::round(q.y + (rowH - ImGui::GetFontSize()) * 0.5f);
    const ImU32 col = current ? P.text : applied ? P.text : P.textFaint;
    if (i > 0) {
      const std::string num = std::to_string(i);
      dl->AddText(ImVec2(std::round(q.x + numW - ImGui::CalcTextSize(num.c_str()).x - 4), ty), applied ? P.textDim : P.textFaint, num.c_str());
    }
    theme::textEllipsis(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(q.x + numW + 12, ty), w - numW - 16, col, text);
    return hit;
  };
  // A shared project's history starts when you shared or joined it (unless older steps were dropped).
  const char* startRow = session && n < h.maxLevels() ? i18n::tr("Start of shared history") : i18n::tr("Start of history");
  // K-112: in Live, as the refs list steps, a line under each row.
  const bool ruled = livelook::active(a);
  auto rule = [&] {
    if (!ruled) return;
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    const float y = std::floor(r1.y + ImGui::GetStyle().ItemSpacing.y * 0.5f) + 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, y), ImVec2(r1.x, y), pal().dark ? theme::withAlpha(pal().textFaint, 0.35f) : pal().line);
  };
  if (table ? tableRow(0, startRow, h.position() == 0, true) : ImGui::Selectable(startRow, h.position() == 0)) a.doc.jumpTo(0);
  rule();
  const std::deque<collab::PeerEdit>* peers = session && showPeers ? &session->peerEdits() : nullptr;
  size_t pi = 0;
  auto peerRow = [&](const collab::PeerEdit& e) {
    std::string objs;
    for (const auto& o : e.objects) objs += (objs.empty() ? "" : ", ") + session->describeObject(o);
    if (objs.empty()) objs = i18n::tr("the project");
    char buf[512];
    std::snprintf(buf, sizeof buf, i18n::tr("%s changed %s"), e.name.empty() ? session->nameOf(e.site).c_str() : e.name.c_str(), objs.c_str());
    ImGui::Indent(theme::kS3);
    ImGui::TextDisabled("%s", buf);
    ImGui::Unindent(theme::kS3);
  };
  for (size_t i = 0; i < n; ++i) {
    const auto* cs = dynamic_cast<const collab::CollabStep*>(a.doc.stepNote(i, NoteSlot::Collab));
    if (peers && cs && cs->madeAtMs)
      while (pi < peers->size() && (*peers)[pi].atMs < cs->madeAtMs) peerRow((*peers)[pi++]);
    const bool applied = i < h.position();
    std::string l = i18n::tr(h.labelAt(i).c_str());
    std::string mark;
    if (cs) {
      using S = collab::CollabStep::State;
      switch (cs->state) {
        case S::NothingUndone: mark = i18n::tr("nothing left to undo"); break;
        case S::PartlyUndone: mark = i18n::tr("partly undone"); break;
        case S::NothingRedone: mark = i18n::tr("nothing left to redo"); break;
        case S::PartlyRedone: mark = i18n::tr("partly redone"); break;
        default: break;
      }
      if (mark.empty() && !cs->changedBy.empty()) {
        std::string who;
        for (collab::SiteId s : cs->changedBy) who += (who.empty() ? "" : ", ") + session->nameOf(s);
        char buf[256];
        std::snprintf(buf, sizeof buf, i18n::tr("changed by %s"), who.c_str());
        mark = buf;
      }
    }
    std::string row = mark.empty() ? l : l + "  (" + mark + ")";
    if (table) {
      if (tableRow(i + 1, row.c_str(), i + 1 == h.position(), applied)) a.doc.jumpTo(i + 1);
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(applied ? pal().text : pal().textFaint));
      row += "##" + std::to_string(i);
      if (ImGui::Selectable(row.c_str(), i + 1 == h.position())) a.doc.jumpTo(i + 1);
      ImGui::PopStyleColor();
      rule();
    }
    if (opened && i + 1 == h.position()) ImGui::SetScrollHereY(0.5f);  // show the current step
  }
  if (peers)
    while (pi < peers->size()) peerRow((*peers)[pi++]);
  if (!opened && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  if (table) {  // Undo / Redo at the bottom right, above a hairline
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, bw = 72.0f;
    dl->AddLine(q, ImVec2(q.x + w, q.y), P.dark ? theme::withAlpha(P.ink, 0.6f) : P.lineStrong);
    ImGui::SetCursorScreenPos(ImVec2(std::round(q.x + w - 2 * bw - theme::kS2 - theme::kS3), q.y + theme::kS3));
    ImGui::BeginDisabled(h.position() == 0);
    if (ui::TextButton(i18n::tr("Undo"), false, bw, "Undo the last step")) step(a, -1);
    ImGui::EndDisabled();
    ImGui::SameLine(0, theme::kS2);
    ImGui::BeginDisabled(h.position() >= n);
    if (ui::TextButton(i18n::tr("Redo"), false, bw, "Redo the next step")) step(a, 1);
    ImGui::EndDisabled();
  }
  ImGui::End();
}

}  // namespace gl::undoui
