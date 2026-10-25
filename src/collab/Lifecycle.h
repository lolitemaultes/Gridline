// Gridline - J-111e collaboration: what happens to the files of a collaborative
// project when it ends for this computer (leave, removed, stop sharing,
// abandoned, keep a private copy). One code path turns the shared folder (or a
// copy of it) into a normal project: <name>.gridline.json + Audio/, without
// .gridline-collab (the shared history, other members' identities and names).
// Deleting a copy goes to the system trash, never a plain delete.
#pragma once
#include <cstdint>
#include <string>
#include "../core/Song.h"

namespace gl::collab {

// Writes `song` as a normal project <destFolder>/<name>.gridline.json. Audio
// files of `srcFolder`/Audio the song uses are copied to <destFolder>/Audio
// (nothing is copied when the folders are the same) and referenced relative to
// the project file. With `removeHistory` (in place: Leave > keep, Stop sharing,
// abandoned) the folder's .gridline-collab is deleted afterwards. The song is
// written as it is: no member identities, fingerprints or emails are part of a
// project file. `outFile` receives the project file.
bool exportPlainProject(const Song& song, const std::string& srcFolder, const std::string& destFolder, const std::string& name,
                        bool removeHistory, std::string* outFile, std::string* err = nullptr);

// Moves `path` to the system trash (macOS ~/.Trash, freedesktop Trash on Linux /
// BSD). False (and nothing deleted) when there is no trash on the same disk.
// Tests: GRIDLINE_TEST_TRASH_DIR is used as the trash, and only paths inside
// GRIDLINE_TEST_TRASH_ROOT may be moved there.
bool moveToTrash(const std::string& path, std::string* err = nullptr);

// Bytes of every regular file under `path` (0 when missing).
uint64_t folderSize(const std::string& path);
// "12.4 MB"
std::string formatBytes(uint64_t bytes);

}  // namespace gl::collab
