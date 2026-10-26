GRIDLINE  @VERSION@
The free, open source music workstation
@SYSTEM@, @ARCH@
========================================================================

Contents (an install tree; extract anywhere, or into /usr/local):
  bin/gridline-app                 the application
  bin/gridline                     the command line tool (gridline --help)
  share/gridline/assets            fonts, sounds, scripts (found relative to bin/)
  share/applications, share/icons, share/mime   desktop entry, icons, project file type
  lib/vst3/Gridline.vst3, lib/clap/Gridline.clap   Gridline as a plugin for other hosts
                                                    (copy to ~/.vst3 and ~/.clap)

Needs the system's SDL2 (and on Linux ALSA; @DEPS@).
Wayland: Gridline runs natively through SDL; the interfaces of hosted plugins are X11 windows
and open through XWayland.

Settings live in $XDG_CONFIG_HOME/gridline (~/.config/gridline).
========================================================================
https://github.com/lolitemaultes/Gridline
