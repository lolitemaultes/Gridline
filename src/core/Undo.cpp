#include "Undo.h"

namespace gl {

void UndoStack::pushApplied(CommandPtr cmd) {
  if (pos_ < cmds_.size()) {  // new edit discards the redo branch
    cmds_.resize(pos_);
    serials_.resize(pos_);
    mergeOpen_ = false;
  }
  if (mergeOpen_ && pos_ > 0 && cmd->mergeKey() != 0 && cmds_[pos_ - 1]->mergeKey() == cmd->mergeKey() &&
      cmds_[pos_ - 1]->mergeWith(*cmd)) {
    serials_[pos_ - 1] = nextSerial_++;
    return;
  }
  cmds_.push_back(std::move(cmd));
  serials_.push_back(nextSerial_++);
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

bool UndoStack::stepPosition(int dir) {
  if (dir < 0 ? !canUndo() : !canRedo()) return false;
  pos_ = dir < 0 ? pos_ - 1 : pos_ + 1;
  mergeOpen_ = false;
  return true;
}

void UndoStack::jumpTo(Song& song, size_t n) {
  if (n > cmds_.size()) n = cmds_.size();
  while (pos_ > n) undo(song);
  while (pos_ < n) redo(song);
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

void UndoStack::clear() {
  cmds_.clear();
  serials_.clear();
  pos_ = 0;
  mergeOpen_ = false;
  baseSerial_ = nextSerial_++;
}

void UndoStack::trim() {
  // Drop the oldest applied steps beyond the limit (never the redo branch).
  while (cmds_.size() > max_ && pos_ > 0) {
    baseSerial_ = serials_.front();
    cmds_.erase(cmds_.begin());
    serials_.erase(serials_.begin());
    --pos_;
  }
}

}  // namespace gl
