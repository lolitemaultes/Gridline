Gridline @VERSION@ for Windows (@ARCH@, Windows 10 or 11)

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

Settings live in %APPDATA%\gridline.
More: https://github.com/lolitemaultes/Gridline
