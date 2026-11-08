GRIDLINE  @VERSION@
The free, open source music workstation
Windows 10 or 11, @ARCH@
========================================================================

Run
  gridline-app.exe   the application
  gridline.exe       the command line tool (render projects, convert FL Studio projects,
                     MCP server): gridline.exe --help
  The assets folder must stay next to the programs.

Unsigned build
  Gridline is not code signed. Windows SmartScreen may show "Windows protected your PC":
  choose "More info", then "Run anyway". The setup program shows the same prompt.

Plugins (optional)
  Gridline also works inside other hosts as a plugin. The setup program can install them;
  from the zip, copy:
      plugins\VST3\Gridline.vst3        -> C:\Program Files\Common Files\VST3
      plugins\CLAP\Gridline.clap and
      plugins\CLAP\Gridline-assets      -> C:\Program Files\Common Files\CLAP

Gridline Suite
  Gridline's instruments and effects as plugins for other music programs. The setup program
  always installs them into Common Files\VST3 and Common Files\CLAP. From the zip, Gridline
  offers to copy plugins\VST3\Gridline Suite.vst3 and plugins\CLAP\Gridline Suite.clap (with
  Gridline Suite-assets) into your plug-in folders the first time it starts.

Opening files
  The setup program makes Gridline the program for Gridline projects (.gridline,
  .gridlinez), which open in the Gridline that is running. It adds Gridline to "Open with"
  for projects saved by earlier versions (.gridline.json, .gridline.zip) and for tracker
  modules (.xm, .mod), and never changes the default program of those types: Windows sees a
  .gridline.json file as .json, so choose Gridline once with Open with, or save the project
  again from Gridline. The zip registers no file types.

Settings live in %APPDATA%\gridline.
========================================================================
https://github.com/lolitemaultes/Gridline
