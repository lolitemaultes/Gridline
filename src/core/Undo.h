// Gridline — command/undo framework (UN-01, UN-02 model side).
// Every model mutation is a Command with apply and revert. The UI, scripts and
// controllers all edit through Document (Document.h), so they share one history.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Song.h"

namespace gl {

class Command {
 public:
  virtual ~Command() = default;
  virtual void apply(Song& song) = 0;
  virtual void revert(Song& song) = 0;
  virtual std::string label() const = 0;
  // Commands with the same non-zero merge key, pushed back to back, fold into
  // one history step (e.g. one knob drag = one undo step).
  virtual uint64_t mergeKey() const { return 0; }
  // Absorb `next` (same merge key, already applied). Returns false to refuse.
  virtual bool mergeWith(const Command& next) { (void)next; return false; }
  // K-64: false when this command can never change the tempo map (Document then skips the tempo
  // map work every edit otherwise pays: a large project with tempo automation spends 10 ms on it).
  virtual bool mayMoveTempo() const { return true; }
  // K-141: rough memory the command holds (the history's byte budget). Commands that copy
  // large objects say so; the default is a small closure.
  virtual size_t approxBytes() const { return 256; }
};

using CommandPtr = std::unique_ptr<Command>;

// K-141: data a history step carries besides its command, owned by the step (dropped with
// it when the history is trimmed, cleared or a redo branch is discarded; kept through merges
// and amends). Slot kCollab: the collaboration session's record of what the step wrote to the
// shared project; slot kUi: what the interface restores on undo / redo (the selection).
struct StepNote {
  virtual ~StepNote() = default;
  virtual size_t approxBytes() const { return 64; }
};
using StepNotePtr = std::shared_ptr<StepNote>;
enum class NoteSlot : int { Collab = 0, Ui = 1 };

// Generic command from two closures (apply/revert capture their own state).
class LambdaCommand final : public Command {
 public:
  LambdaCommand(std::string label, std::function<void(Song&)> apply, std::function<void(Song&)> revert)
      : label_(std::move(label)), apply_(std::move(apply)), revert_(std::move(revert)) {}
  void apply(Song& s) override { apply_(s); }
  void revert(Song& s) override { revert_(s); }
  std::string label() const override { return label_; }

 private:
  std::string label_;
  std::function<void(Song&)> apply_, revert_;
};

// Several commands applied as one history step; reverted in reverse order.
class GroupCommand final : public Command {
 public:
  explicit GroupCommand(std::string label) : label_(std::move(label)) {}
  void add(CommandPtr c) { parts_.push_back(std::move(c)); }
  // Adds `c` (already applied), folding it into the newest part with the same merge key when
  // that part accepts it (a drag inside a gesture step stays one part, not one per frame).
  void addMerged(CommandPtr c) {
    if (c->mergeKey() != 0)
      for (auto it = parts_.rbegin(); it != parts_.rend(); ++it)
        if ((*it)->mergeKey() == c->mergeKey()) {
          if ((*it)->mergeWith(*c)) return;
          break;
        }
    parts_.push_back(std::move(c));
  }
  bool empty() const { return parts_.empty(); }
  void apply(Song& s) override {
    for (auto& c : parts_) c->apply(s);
  }
  size_t approxBytes() const override {
    size_t n = 64;
    for (const auto& c : parts_) n += c->approxBytes();
    return n;
  }
  void revert(Song& s) override {
    for (auto it = parts_.rbegin(); it != parts_.rend(); ++it) (*it)->revert(s);
  }
  std::string label() const override { return label_; }

 private:
  std::string label_;
  std::vector<CommandPtr> parts_;
};

// Linear history with a level limit. Index `pos_` = number of applied commands.
class UndoStack {
 public:
  static constexpr size_t kDefaultMaxLevels = 100;
  static constexpr size_t kMinLevels = 10, kMaxLevels = 10000;  // the settings range
  static constexpr size_t kDefaultMaxBytes = size_t{256} << 20;   // K-141: memory budget

  // Applies `cmd` to `song` and records it (dropping any redo branch).
  void push(Song& song, CommandPtr cmd) {
    cmd->apply(song);
    pushApplied(std::move(cmd));
  }
  // Records a command whose effect is already in the song.
  void pushApplied(CommandPtr cmd);
  bool canUndo() const { return pos_ > 0; }
  bool canRedo() const { return pos_ < cmds_.size(); }
  bool undo(Song& song);
  bool redo(Song& song);
  // Moves to the state after `n` applied steps (0 = oldest kept state).
  void jumpTo(Song& song, size_t n);
  // K-141: drops every step from index `n` on (applied or not), without touching the song
  // (edits that were never kept, e.g. made while a collaboration review previewed a side).
  void truncate(size_t n);

  size_t size() const { return cmds_.size(); }
  size_t position() const { return pos_; }
  std::vector<std::string> labels() const;  // oldest first
  std::string undoLabel() const { return canUndo() ? cmds_[pos_ - 1]->label() : ""; }
  std::string labelAt(size_t i) const { return i < cmds_.size() ? cmds_[i]->label() : std::string(); }
  std::string redoLabel() const { return canRedo() ? cmds_[pos_]->label() : ""; }

  void setMaxLevels(size_t n);
  size_t maxLevels() const { return max_; }
  // K-141: the oldest steps are dropped while the history holds more than `bytes` (at least
  // the newest step stays).
  void setMaxBytes(size_t bytes);
  size_t maxBytes() const { return maxBytes_; }
  size_t bytes() const { return bytes_; }
  // K-141: per-step data (see StepNote), index 0 = oldest kept step.
  StepNote* note(size_t i, NoteSlot slot) const {
    return i < notes_.size() ? notes_[i][static_cast<size_t>(slot)].get() : nullptr;
  }
  StepNotePtr noteShared(size_t i, NoteSlot slot) const {
    return i < notes_.size() ? notes_[i][static_cast<size_t>(slot)] : nullptr;
  }
  void setNote(size_t i, NoteSlot slot, StepNotePtr n);
  uint64_t serialAt(size_t i) const { return i < serials_.size() ? serials_[i] : 0; }
  // Recomputes the byte count of step `i` (its note grew).
  void touchBytes(size_t i);
  // Stops the next push from merging into the current top (e.g. on mouse-up).
  void breakMerge() { mergeOpen_ = false; }
  void clear();
  // J-111 (collaboration): true when `cmd`, pushed now, would fold into the top step.
  bool wouldMerge(const Command& cmd) const {
    return mergeOpen_ && pos_ > 0 && pos_ == cmds_.size() && cmd.mergeKey() != 0 && cmds_[pos_ - 1]->mergeKey() == cmd.mergeKey();
  }
  // J-111: moves the position by one step (-1 undo, +1 redo) without applying or
  // reverting the command in the song (the collaboration session changes the song itself).
  // K-141: with `scratch`, the command is reverted (undo) or applied (redo) there instead, so
  // the session can take the parts that stay on this computer from it.
  bool stepPosition(int dir, Song* scratch = nullptr);
  // Records `cmd` (already applied) as a part of the newest step instead of a step of its own
  // (the later edits of one pointer gesture; Document::beginPointerGesture). Needs the
  // position at the end of the history; the step keeps its label. Like a merge, the state
  // serial changes and pushCount() does not.
  bool canAmend() const { return pos_ > 0 && pos_ == cmds_.size(); }
  void amendTop(CommandPtr cmd);
  // J-111: number of new (not merged) steps pushed so far.
  uint64_t pushCount() const { return pushCount_; }

  // Monotonic counter of the state; equal values mean the same document state
  // (used for the "changed since save" flag).
  uint64_t stateSerial() const { return pos_ == 0 ? baseSerial_ : serials_[pos_ - 1]; }

 private:
  void trim();
  void dropFront();
  size_t stepBytes(size_t i) const;
  std::vector<CommandPtr> cmds_;
  std::vector<uint64_t> serials_;  // serial of the state after each command
  std::vector<std::array<StepNotePtr, 2>> notes_;
  std::vector<size_t> sizes_;      // approxBytes of each step (command + notes)
  size_t bytes_ = 0;
  size_t maxBytes_ = kDefaultMaxBytes;
  size_t pos_ = 0;
  size_t max_ = kDefaultMaxLevels;
  bool mergeOpen_ = false;
  uint64_t nextSerial_ = 1;
  uint64_t baseSerial_ = 0;  // serial of the state before cmds_[0]
  uint64_t pushCount_ = 0;
};

}  // namespace gl
