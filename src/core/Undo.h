// Gridline — command/undo framework (UN-01, UN-02 model side).
// Every model mutation is a Command with apply and revert. The UI, scripts and
// controllers all edit through Document (Document.h), so they share one history.
#pragma once
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
};

using CommandPtr = std::unique_ptr<Command>;

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
  bool empty() const { return parts_.empty(); }
  void apply(Song& s) override {
    for (auto& c : parts_) c->apply(s);
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

  size_t size() const { return cmds_.size(); }
  size_t position() const { return pos_; }
  std::vector<std::string> labels() const;  // oldest first
  std::string undoLabel() const { return canUndo() ? cmds_[pos_ - 1]->label() : ""; }
  std::string redoLabel() const { return canRedo() ? cmds_[pos_]->label() : ""; }

  void setMaxLevels(size_t n);
  size_t maxLevels() const { return max_; }
  // Stops the next push from merging into the current top (e.g. on mouse-up).
  void breakMerge() { mergeOpen_ = false; }
  void clear();

  // Monotonic counter of the state; equal values mean the same document state
  // (used for the "changed since save" flag).
  uint64_t stateSerial() const { return pos_ == 0 ? baseSerial_ : serials_[pos_ - 1]; }

 private:
  void trim();
  std::vector<CommandPtr> cmds_;
  std::vector<uint64_t> serials_;  // serial of the state after each command
  size_t pos_ = 0;
  size_t max_ = kDefaultMaxLevels;
  bool mergeOpen_ = false;
  uint64_t nextSerial_ = 1;
  uint64_t baseSerial_ = 0;  // serial of the state before cmds_[0]
};

}  // namespace gl
