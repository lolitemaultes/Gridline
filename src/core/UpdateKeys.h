// Gridline - the public keys that sign Gridline's releases (minisign, see docs/updates.md).
//
// Each entry is the second line of a minisign .pub file ("RWQ..."). While this list is empty, the
// build accepts releases checked by SHA-256 alone (and the system's own prompts for system
// packages). As soon as one key is listed, every automatic install needs a valid signature from
// one of them, and the release workflow refuses to publish a release it could not sign.
// To rotate a key: add the new one, release with the old one, then sign with the new one and
// remove the old one in a later release.
#pragma once

namespace gl::update {

inline constexpr const char* kReleaseKeys[] = {
    nullptr,  // the end of the list (keys go above this line)
};

}  // namespace gl::update
