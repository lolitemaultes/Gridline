// Gridline — default workspace geometry (J-28): browser docked left, mixer
// docked right as a thin column, playlist in the centre taking the rest, and
// the channel rack floating over the centre-left of the playlist, fully in
// view and sized to its rows. Pure numbers (no UI): main.cpp turns them into
// dock splits; tests check them for every supported window size.
#pragma once

namespace gl::workspace {

// Bumped whenever the default layout changes: saved layouts older than this
// are replaced once by the new default (settings key "ui.layoutVersion").
constexpr int kLayoutVersion = 2;

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  bool contains(const Rect& o) const {
    return o.x >= x - 0.5f && o.y >= y - 0.5f && o.right() <= right() + 0.5f && o.bottom() <= bottom() + 0.5f;
  }
  bool overlaps(const Rect& o) const { return x < o.right() - 0.5f && o.x < right() - 0.5f && y < o.bottom() - 0.5f && o.y < bottom() - 0.5f; }
};

struct Layout {
  Rect area;      // the dock space (window minus top bar and status bar)
  Rect browser;   // docked left
  Rect playlist;  // central node
  Rect mixer;     // docked right
  Rect rack;      // floating window, over the playlist's left part
  float browserFrac = 0, mixerFrac = 0;  // split ratios for the dock builder
};

// Layout for a dock space of w x h logical pixels (origin 0,0) holding
// `channels` channel rows in the rack.
Layout defaultLayout(float w, float h, int channels);

// Height of the channel rack window that shows `channels` rows (toolbar,
// rows, the add-channel row and window chrome).
float rackHeightFor(int channels);

}  // namespace gl::workspace
