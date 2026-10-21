// Gridline — Q-13: which SVG (assets/icons/ui/<stem>.svg) draws each ui::Icon.
// Lucide (ISC) and Tabler (MIT) geometry, two Gridline originals on the same
// grid (slip, detune); sources and customisations in assets/icons/ui/NOTICE.md.
#include "../Widgets.h"

namespace gl::ui {

const char* iconSvgName(Icon icon) {
  switch (icon) {
    case Icon::Play: return "play-filled";
    case Icon::Pause: return "pause-filled";
    case Icon::Stop: return "stop-filled";
    case Icon::Record: return "record-filled";
    case Icon::Plus: return "plus";
    case Icon::Minus: return "minus";
    case Icon::Close: return "x";
    case Icon::Pattern: return "layout-grid";
    case Icon::Song: return "list-music";
    case Icon::Draw: return "pencil";
    case Icon::Paint: return "paintbrush-vertical";
    case Icon::Erase: return "eraser";
    case Icon::Select: return "square-dashed-mouse-pointer";
    case Icon::Slice: return "scissors";
    case Icon::Mute: return "volume-x";
    case Icon::Metronome: return "metronome";
    case Icon::Loop: return "repeat";
    case Icon::Undo: return "undo-2";
    case Icon::Redo: return "redo-2";
    case Icon::Save: return "save";
    case Icon::Open: return "folder-open";
    case Icon::NewFile: return "file-plus";
    case Icon::Keyboard: return "keyboard";
    case Icon::Browser: return "folder-tree";
    case Icon::Rack: return "grid-3x3";
    case Icon::Piano: return "piano";
    case Icon::Playlist: return "chart-gantt";
    case Icon::Mixer: return "sliders-vertical";
    case Icon::Settings: return "settings";
    case Icon::Folder: return "folder";
    case Icon::File: return "file";
    case Icon::Audio: return "audio-waveform";
    case Icon::Plug: return "plug";
    case Icon::Up: return "caret-up";
    case Icon::Down: return "caret-down";
    case Icon::Left: return "caret-left";
    case Icon::Right: return "caret-right";
    case Icon::Magnet: return "magnet";
    case Icon::Menu: return "menu";
    case Icon::Zoom: return "zoom-in";
    case Icon::Help: return "circle-help";
    case Icon::Wave: return "audio-waveform";
    case Icon::Clone: return "copy";
    case Icon::Trash: return "trash-2";
    case Icon::More: return "ellipsis";
    case Icon::Search: return "search";
    case Icon::Chevron: return "chevron-right";
    case Icon::ChevronDown: return "chevron-down";
    case Icon::Slip: return "slip";
    case Icon::Detune: return "detune";
    case Icon::Stamp: return "stamp";
    case Icon::Lock: return "lock";
    case Icon::Eye: return "eye";
    case Icon::EyeOff: return "eye-off";
    case Icon::ToolDraw: return "pencil";
    case Icon::ToolPaint: return "paintbrush-vertical";
    case Icon::ToolDelete: return "eraser";
    case Icon::ToolMute: return "volume-x";
    case Icon::ToolSlip: return "slip";
    case Icon::ToolSlice: return "scissors";
    case Icon::ToolSelect: return "square-dashed-mouse-pointer";
    case Icon::ToolZoom: return "zoom-in";
    case Icon::ToolPlay: return "step-forward";
    case Icon::ToolDetune: return "detune";
    case Icon::ToolSnap: return "magnet";
    case Icon::StretchMode: return "move-horizontal";
    case Icon::FadeHandles: return "fade-handles";
    case Icon::ZeroCross: return "zero-crossing";
    case Icon::Count: break;
  }
  return nullptr;
}

}  // namespace gl::ui
