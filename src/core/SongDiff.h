// Gridline — K-92: what changed between two versions of a song, as short lines a person reads
// ("Tempo 120 -> 128", "Pattern 'Verse', channel 'Lead': 4 notes added, 1 removed"). The AI
// assistant panel shows it for a proposed change before the user accepts or discards it.
#pragma once
#include <string>
#include <vector>
#include "Song.h"

namespace gl {

std::vector<std::string> songDiff(const Song& before, const Song& after, size_t maxLines = 60);

}  // namespace gl
