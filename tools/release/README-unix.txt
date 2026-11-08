GRIDLINE  @VERSION@
The free, open source music workstation
@SYSTEM@, @ARCH@
========================================================================

Install: ./install.sh puts Gridline in your menus and launchers with its icon (for you in
~/.local, or as root for everyone in /usr/local; --prefix DIR for another place, --uninstall
to remove it). Or run bin/gridline-app straight from this folder.

Opening files: after ./install.sh (or the .deb / .rpm) Gridline projects (.gridline,
.gridlinez, and .gridline.json, .gridline.zip from earlier versions) open in Gridline from the
file manager, in the Gridline that is running, and Gridline is listed under Open With for
tracker modules (.xm, .mod); choose it there once to open them by double click.

Contents (an install tree; extract anywhere, or into /usr/local):
  bin/gridline-app                 the application
  bin/gridline                     the command line tool (gridline --help)
  share/gridline/assets            fonts, sounds, scripts (found relative to bin/)
  share/applications, share/icons, share/mime   desktop entry, icons, project file type
  install.sh                       installs bin/ and share/ (see above)
  lib/vst3/Gridline.vst3, lib/clap/Gridline.clap   Gridline as a plugin for other hosts
                                                    (copy to ~/.vst3 and ~/.clap)
  lib/vst3/Gridline Suite.vst3, lib/clap/Gridline Suite.clap   the Gridline Suite: Gridline's
                                   instruments and effects as plugins for other hosts; Gridline
                                   offers to copy them to ~/.vst3 and ~/.clap the first time
                                   it starts (or: gridline suite install)

Needs the system's SDL2 (and on Linux ALSA; @DEPS@).
Wayland: the interfaces of hosted plugins are X11 windows, so Gridline runs through XWayland
whenever it is available, whatever SDL_VIDEODRIVER says. Without XWayland it runs on Wayland and
plugins show their parameters in Gridline's own panel. GRIDLINE_VIDEO_DRIVER=wayland keeps it on
Wayland anyway.

Settings live in $XDG_CONFIG_HOME/gridline (~/.config/gridline).
========================================================================
https://github.com/lolitemaultes/Gridline
