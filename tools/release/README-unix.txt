GRIDLINE  @VERSION@
The free, open source music workstation
@SYSTEM@, @ARCH@
========================================================================

Install: ./install.sh puts Gridline in your menus and launchers with its icon (for you in
~/.local, or as root for everyone in /usr/local; --prefix DIR for another place, --uninstall
to remove it). Or run bin/gridline-app straight from this folder.

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
Wayland: Gridline runs natively through SDL; the interfaces of hosted plugins are X11 windows
and open through XWayland.

Settings live in $XDG_CONFIG_HOME/gridline (~/.config/gridline).
========================================================================
https://github.com/lolitemaultes/Gridline
