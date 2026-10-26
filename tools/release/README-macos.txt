GRIDLINE  @VERSION@
The free, open source music workstation
macOS 11 or later, Apple Silicon and Intel
========================================================================

Install
  Drag Gridline.app onto the Applications folder.

First start
  This build is not notarized by Apple (it is signed ad hoc). The first time, open it with
  right-click (or Control-click) on Gridline.app > Open, then Open in the dialog. If macOS
  says the app is damaged or cannot be opened, run this once in Terminal:
      xattr -dr com.apple.quarantine /Applications/Gridline.app

Plugins (optional)
  Gridline also works inside other hosts as a plugin. Copy the bundles from the Plugins folder:
      Gridline.vst3       -> ~/Library/Audio/Plug-Ins/VST3
      Gridline.clap       -> ~/Library/Audio/Plug-Ins/CLAP
      Gridline.component  -> ~/Library/Audio/Plug-Ins/Components   (Audio Unit)
  then remove the quarantine flag from them the same way (xattr -dr com.apple.quarantine <bundle>).

Command line
  "gridline" is the command line tool (render projects, convert FL Studio projects, MCP server):
      ./gridline --help
  Copy it somewhere on your PATH, e.g. /usr/local/bin.

Settings live in ~/.config/gridline.
========================================================================
https://github.com/lolitemaultes/Gridline
