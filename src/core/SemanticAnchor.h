// Gridline - semantic anchors (K-112): what a collaborator is on, by (kind, id), in any layout.
//
// The cursor anchors (app/CursorAnchor.h) say where a pointer is on a surface both computers
// draw: a playlist track, a piano roll grid, a rack row, a mixer strip, or an ImGui item of a
// window. The layouts draw the same objects in different windows (an instrument in the channel
// settings window in Studio, in the device chain in another layout), so ImGui items and window
// fractions do not carry across. A target names the object itself: a channel, a mixer track (and
// an effect slot), a pattern (and its channel), a clip, a playlist track, a marker, or a layout
// part. Every layout can show any target (app: layoutui::reveal), and a collaborator's cursor
// that cannot be placed here is placed at its target when that is on screen.
#pragma once
#include <cstdint>
#include <string>

namespace gl {
struct Song;
}

namespace gl::anchor {

enum class Kind : uint8_t { None = 0, Channel, MixerTrack, Pattern, Clip, PlaylistTrack, Marker, Count };

struct Target {
  Kind kind = Kind::None;
  uint32_t id = 0;   // object id (Channel, MixerTrack, Pattern, Clip), track index (PlaylistTrack), tick (Marker)
  uint32_t sub = 0;  // Pattern: its channel (0 none); MixerTrack: effect slot + 1 (0 the track)
  bool operator==(const Target& o) const { return kind == o.kind && id == o.id && sub == o.sub; }
  bool operator!=(const Target& o) const { return !(*this == o); }
};

const char* kindName(Kind k);  // "channel", "mixer", "pattern", "clip", "track", "marker", "none"
Kind kindByName(const std::string& name);  // Count when unknown
// "channel:12", "pattern:4/7", "mixer:3/2", "none".
std::string toText(const Target& t);
bool fromText(const std::string& text, Target& out);

// The target behind a cursor anchor (CursorAnchor.h kinds and ids) and the plugin editor it is
// in (J-111g: the channel or mixer track, the slot: -1 the instrument). None when the anchor
// names no object (a window fraction, an ImGui item outside a plugin editor).
Target fromCursor(int cursorKind, uint32_t a, uint32_t b, uint32_t plugin, int pluginSlot);

// The object still exists in the song.
bool exists(const Song& s, const Target& t);
// A name for it as people see it ("Kick", "Insert 3", "Pattern 2", "Track 4"), "" when gone.
std::string describe(const Song& s, const Target& t);

}  // namespace gl::anchor
