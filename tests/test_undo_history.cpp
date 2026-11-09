// K-141: the project history itself: notes kept with each step (through merges, amends, trims,
// truncation and clearing), the levels range and the memory budget, clearing and rebuilding a
// history, stepping on a scratch song, the move callback, and rollbacks that never take an
// unrelated step.
#include <memory>
#include <string>
#include <vector>
#include "../src/core/Document.h"
#include "../src/core/Keymap.h"
#include "Check.h"

using namespace gl;

namespace {

struct Tag final : StepNote {
  int v = 0;
  size_t bytes = 64;
  explicit Tag(int x, size_t b = 64) : v(x), bytes(b) {}
  size_t approxBytes() const override { return bytes; }
};

int tagAt(const Document& d, size_t i, NoteSlot slot = NoteSlot::Ui) {
  const auto* t = dynamic_cast<const Tag*>(d.stepNote(i, slot));
  return t ? t->v : -1;
}

ObjectId firstChannel(Document& d) {
  if (d.song().channels.empty()) d.addChannel("Lead", "gl.trio");
  return d.song().channels.front().id;
}

void rename(Document& d, ObjectId ch, const std::string& n) {
  d.editChannel(ch, "rename channel", [&](Channel& c) { c.name = n; });
}

void testNotesFollowSteps() {
  Document d;
  const ObjectId ch = firstChannel(d);
  d.clearHistory();
  rename(d, ch, "a");
  d.setStepNote(0, NoteSlot::Ui, std::make_shared<Tag>(1));
  rename(d, ch, "b");
  d.setStepNote(1, NoteSlot::Ui, std::make_shared<Tag>(2));
  d.setStepNote(1, NoteSlot::Collab, std::make_shared<Tag>(20));
  expect(tagAt(d, 0) == 1 && tagAt(d, 1) == 2 && tagAt(d, 1, NoteSlot::Collab) == 20, "notes: one per step and slot");
  // A merged edit keeps the top step's note.
  d.editChannel(ch, "colour", [](Channel& c) { c.name = "m1"; }, 77);
  d.setStepNote(2, NoteSlot::Ui, std::make_shared<Tag>(3));
  d.editChannel(ch, "colour", [](Channel& c) { c.name = "m2"; }, 77);
  expect(d.history().size() == 3 && tagAt(d, 2) == 3, "notes: a merge keeps the note");
  // Undo then a new edit drops the redo branch with its notes.
  d.undo();
  d.undo();
  rename(d, ch, "c");
  expect(d.history().size() == 2 && tagAt(d, 0) == 1 && tagAt(d, 1) == -1 && tagAt(d, 1, NoteSlot::Collab) == -1,
         "notes: a new edit after undo drops the redo branch's notes");
  // Trimming by levels drops the oldest steps and their notes together.
  d.setMaxUndoLevels(UndoStack::kMinLevels);
  for (int i = 0; i < 15; ++i) {
    rename(d, ch, "n" + std::to_string(i));
    d.setStepNote(d.history().size() - 1, NoteSlot::Ui, std::make_shared<Tag>(100 + i));
  }
  expect(d.history().size() == UndoStack::kMinLevels, "levels: trimmed to the limit");
  expect(tagAt(d, 0) == 105 && tagAt(d, UndoStack::kMinLevels - 1) == 114, "levels: notes trimmed with their steps");
  // Truncation (edits that were never kept).
  d.discardStepsFrom(4);
  expect(d.history().size() == 4 && d.history().position() == 4 && tagAt(d, 3) == 108, "truncate: steps and notes from the index go");
  d.clearHistory();
  expect(d.history().size() == 0 && d.stepNote(0, NoteSlot::Ui) == nullptr, "clear: no notes left");
}

void testByteBudget() {
  Document d;
  const ObjectId ch = firstChannel(d);
  d.clearHistory();
  d.setMaxUndoLevels(UndoStack::kMaxLevels);
  d.setMaxUndoBytes(64 * 1024);
  for (int i = 0; i < 40; ++i) {
    rename(d, ch, "x" + std::to_string(i));
    d.setStepNote(d.history().size() - 1, NoteSlot::Collab, std::make_shared<Tag>(i, 8 * 1024));
    d.touchStepNote(d.history().size() - 1);
  }
  expect(d.history().bytes() <= 64 * 1024 + 9 * 1024, "budget: the history stays near its byte budget");
  expect(d.history().size() >= 1 && d.history().size() < 40, "budget: the oldest steps went");
  expect(tagAt(d, d.history().size() - 1, NoteSlot::Collab) == 39, "budget: the newest step stays");
  // One step larger than the whole budget is still kept (the newest step always stays).
  rename(d, ch, "huge");
  d.setStepNote(d.history().size() - 1, NoteSlot::Collab, std::make_shared<Tag>(99, 1 << 20));
  d.touchStepNote(d.history().size() - 1);
  rename(d, ch, "after");
  expect(d.history().size() >= 1 && d.undo() && d.song().channel(ch)->name == "huge", "budget: undo still works after a huge step");
}

void testClearKeepsSong() {
  Document d;
  const ObjectId ch = firstChannel(d);
  d.markSaved();
  rename(d, ch, "edited");
  expect(d.modified(), "clear: modified before");
  d.clearHistory();
  expect(d.song().channel(ch)->name == "edited" && !d.history().canUndo() && d.modified(), "clear: song and modified flag stay");
  d.markSaved();
  d.clearHistory();
  expect(!d.modified(), "clear: a saved project stays saved");
}

void testRestoreHistory() {
  Document d;
  firstChannel(d);
  d.markSaved();
  std::vector<StepNotePtr> notes{std::make_shared<Tag>(1), std::make_shared<Tag>(2), std::make_shared<Tag>(3)};
  d.restoreHistory({"one", "two", "three"}, 2, notes);
  expect(d.history().size() == 3 && d.history().position() == 2, "restore: steps and position");
  expect(d.history().labelAt(1) == "two" && tagAt(d, 2, NoteSlot::Collab) == 3, "restore: labels and notes in place");
  expect(!d.modified(), "restore: not marked modified");
}

void testScratchStep() {
  Document d;
  const ObjectId ch = firstChannel(d);
  d.clearHistory();
  const std::string was = d.song().channel(ch)->name;
  rename(d, ch, "new");
  Song scratch = d.song();
  expect(d.stepHistory(-1, &scratch), "scratch: step back");
  expect(d.song().channel(ch)->name == "new" && scratch.channel(ch)->name == was, "scratch: reverted on the scratch song only");
  expect(d.stepHistory(1, &scratch) && scratch.channel(ch)->name == "new", "scratch: step forward applies there");
}

void testMoveCallback() {
  Document d;
  const ObjectId ch = firstChannel(d);
  d.clearHistory();
  rename(d, ch, "a");
  rename(d, ch, "b");
  std::vector<std::pair<int, size_t>> calls;
  d.onHistoryMove = [&](int dir, size_t i) { calls.emplace_back(dir, i); };
  d.undo();
  d.undo();
  d.redo();
  expect(calls.size() == 3, "move callback: once per step");
  expect(calls.size() == 3 && calls[0] == std::make_pair(-1, size_t{1}) && calls[1] == std::make_pair(-1, size_t{0}) &&
             calls[2] == std::make_pair(1, size_t{0}),
         "move callback: the index of the step moved over");
  calls.clear();
  d.jumpTo(2);
  expect(!calls.empty() && calls.back().second == 1, "move callback: jumpTo reports where it ended");
}

// S6: a rollback after an edit that recorded nothing never takes the step before it.
void testRollbackGuard() {
  Document d;
  const ObjectId ch = firstChannel(d);
  d.clearHistory();
  rename(d, ch, "kept");
  const uint64_t serial0 = d.history().stateSerial();
  d.editSong("import nothing", [](Song&) {});
  expect(d.history().stateSerial() == serial0, "rollback: a no-op edit records nothing");
  if (d.history().stateSerial() != serial0 && d.undo()) d.discardStepsFrom(d.history().position());
  expect(d.song().channel(ch)->name == "kept" && d.history().size() == 1, "rollback: the earlier step stays");
  const uint64_t serial1 = d.history().stateSerial();
  d.beginGroup("insert");
  d.endGroup();
  expect(d.history().stateSerial() == serial1, "rollback: an empty group records nothing");
  rename(d, ch, "failed");
  if (d.history().stateSerial() != serial1 && d.undo()) d.discardStepsFrom(d.history().position());
  expect(d.song().channel(ch)->name == "kept" && !d.history().canRedo(), "rollback: only the failed step goes, with no redo left");
}

// S4: while a group is open, undo does nothing (the interface queues the press).
void testOpenGroup() {
  Document d;
  const ObjectId ch = firstChannel(d);
  d.clearHistory();
  rename(d, ch, "one");
  d.beginGroup("drag");
  rename(d, ch, "dragging");
  expect(!d.undo() && d.song().channel(ch)->name == "dragging", "open group: undo waits");
  d.endGroup();
  expect(d.undo() && d.song().channel(ch)->name == "one", "open group: after it closes, undo takes the whole drag");
}

void testKeymap() {
  keys::Keymap km;
  bool alt = false, ctrlAlt = false;
  for (const auto& c : km.bindings("edit.history")) {
    alt |= c.key == keys::Key::Z && c.mods == keys::kAlt;
    ctrlAlt |= c.key == keys::Key::Z && c.mods == (keys::kCtrl | keys::kAlt);
  }
  expect(alt && ctrlAlt, "keymap: the history has Alt+Z and Ctrl+Alt+Z (Alt+Z zips in the channel rack)");
}

}  // namespace

int main() {
  testNotesFollowSteps();
  testByteBudget();
  testClearKeepsSong();
  testRestoreHistory();
  testScratchStep();
  testMoveCallback();
  testRollbackGuard();
  testOpenGroup();
  testKeymap();
  return finish();
}
