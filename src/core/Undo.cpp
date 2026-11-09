#include "Undo.h"

namespace gl {

size_t UndoStack::stepBytes(size_t i) const {
  size_t n = cmds_[i]->approxBytes();
  for (const auto& note : notes_[i])
    if (note) n += note->approxBytes();
  return n;
}

void UndoStack::pushApplied(CommandPtr cmd) {
  if (pos_ < cmds_.size()) {  // new edit discards the redo branch
    truncate(pos_);
    mergeOpen_ = false;
  }
  if (mergeOpen_ && pos_ > 0 && cmd->mergeKey() != 0 && cmds_[pos_ - 1]->mergeKey() == cmd->mergeKey() &&
      cmds_[pos_ - 1]->mergeWith(*cmd)) {
    serials_[pos_ - 1] = nextSerial_++;
    touchBytes(pos_ - 1);
    return;
  }
  cmds_.push_back(std::move(cmd));
  serials_.push_back(nextSerial_++);
  notes_.push_back({});
  sizes_.push_back(0);
  touchBytes(cmds_.size() - 1);
  ++pushCount_;
  pos_ = cmds_.size();
  mergeOpen_ = true;
  trim();
}

void UndoStack::amendTop(CommandPtr cmd) {
  if (!canAmend()) {
    pushApplied(std::move(cmd));
    return;
  }
  CommandPtr& top = cmds_[pos_ - 1];
  if (auto* g = dynamic_cast<GroupCommand*>(top.get())) {
    g->addMerged(std::move(cmd));
  } else if (!(top->mergeKey() != 0 && top->mergeKey() == cmd->mergeKey() && top->mergeWith(*cmd))) {
    auto group = std::make_unique<GroupCommand>(top->label());
    group->add(std::move(top));
    group->add(std::move(cmd));
    top = std::move(group);
  }
  serials_[pos_ - 1] = nextSerial_++;
  touchBytes(pos_ - 1);
  trim();
}

bool UndoStack::undo(Song& song) {
  if (!canUndo()) return false;
  cmds_[--pos_]->revert(song);
  mergeOpen_ = false;
  return true;
}

bool UndoStack::redo(Song& song) {
  if (!canRedo()) return false;
  cmds_[pos_++]->apply(song);
  mergeOpen_ = false;
  return true;
}

bool UndoStack::stepPosition(int dir, Song* scratch) {
  if (dir < 0 ? !canUndo() : !canRedo()) return false;
  if (dir < 0) {
    --pos_;
    if (scratch) cmds_[pos_]->revert(*scratch);
  } else {
    if (scratch) cmds_[pos_]->apply(*scratch);
    ++pos_;
  }
  mergeOpen_ = false;
  return true;
}

void UndoStack::jumpTo(Song& song, size_t n) {
  if (n > cmds_.size()) n = cmds_.size();
  while (pos_ > n) undo(song);
  while (pos_ < n) redo(song);
}

void UndoStack::truncate(size_t n) {
  if (n >= cmds_.size()) return;
  for (size_t i = n; i < sizes_.size(); ++i) bytes_ -= sizes_[i];
  cmds_.resize(n);
  serials_.resize(n);
  notes_.resize(n);
  sizes_.resize(n);
  if (pos_ > n) pos_ = n;
  mergeOpen_ = false;
}

std::vector<std::string> UndoStack::labels() const {
  std::vector<std::string> out;
  for (const auto& c : cmds_) out.push_back(c->label());
  return out;
}

void UndoStack::setMaxLevels(size_t n) {
  max_ = n > 0 ? n : 1;
  trim();
}

void UndoStack::setMaxBytes(size_t bytes) {
  maxBytes_ = bytes > 0 ? bytes : 1;
  trim();
}

void UndoStack::setNote(size_t i, NoteSlot slot, StepNotePtr n) {
  if (i >= notes_.size()) return;
  notes_[i][static_cast<size_t>(slot)] = std::move(n);
  touchBytes(i);
}

void UndoStack::touchBytes(size_t i) {
  if (i >= sizes_.size()) return;
  bytes_ -= sizes_[i];
  sizes_[i] = stepBytes(i);
  bytes_ += sizes_[i];
}

void UndoStack::clear() {
  cmds_.clear();
  serials_.clear();
  notes_.clear();
  sizes_.clear();
  bytes_ = 0;
  pos_ = 0;
  mergeOpen_ = false;
  baseSerial_ = nextSerial_++;
}

void UndoStack::dropFront() {
  baseSerial_ = serials_.front();
  bytes_ -= sizes_.front();
  cmds_.erase(cmds_.begin());
  serials_.erase(serials_.begin());
  notes_.erase(notes_.begin());
  sizes_.erase(sizes_.begin());
  --pos_;
}

void UndoStack::trim() {
  // Drop the oldest applied steps beyond the limits (never the redo branch, never the newest step).
  while (cmds_.size() > max_ && pos_ > 0) dropFront();
  while (bytes_ > maxBytes_ && cmds_.size() > 1 && pos_ > 1) dropFront();
}

}  // namespace gl
