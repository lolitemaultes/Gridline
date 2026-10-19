// Gridline — automatic project backups (PJ-02). Timed snapshots of the open
// project, kept up to a maximum count, in a global folder or next to the project.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Song.h"

namespace gl {

// How often backups are written. "Before risky operations" levels also back up
// right before destructive edits (callers use backupBeforeRiskyEdit()).
enum class BackupFrequency : int {
  Never = 0,           // no backups, only a reminder every 10 minutes
  Rarely = 1,          // every 15 minutes
  Occasionally = 2,    // every 10 minutes
  Regularly = 3,       // every 5 minutes (default)
  Frequently = 4,      // every 5 minutes + before risky edits
  VeryFrequently = 5,  // every minute + before risky edits
};

struct BackupPolicy {
  BackupFrequency frequency = BackupFrequency::Regularly;
  int maxCount = 100;
  bool inProjectFolder = false;  // false: global backup folder
  std::string globalFolder;      // used when !inProjectFolder (or project unsaved)

  int64_t intervalSeconds() const;
  bool backupBeforeRiskyEdit() const { return frequency >= BackupFrequency::Frequently; }
};

class BackupManager {
 public:
  explicit BackupManager(BackupPolicy policy) : policy_(std::move(policy)) {}
  const BackupPolicy& policy() const { return policy_; }

  // Folder for backups of `projectPath` ("" = unsaved project).
  std::string folderFor(const std::string& projectPath) const;
  // True when a timed backup is due at `nowSec` (Unix seconds).
  bool due(int64_t nowSec) const;
  // Writes "<stem> (backup YYYY-MM-DD HH-MM-SS).gridline.json" and prunes the
  // oldest backups of the same project beyond maxCount. Returns the file path, "" on failure.
  std::string write(const Song& song, const std::string& projectPath, int64_t nowSec, std::string* err = nullptr);
  // Existing backups of `projectPath`, oldest first.
  std::vector<std::string> list(const std::string& projectPath) const;

 private:
  BackupPolicy policy_;
  int64_t lastBackup_ = -1;
};

}  // namespace gl
