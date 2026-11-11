# Opening files from the file manager

Gridline opens these files when you double-click them or pick Gridline in **Open with**:

| File | Name ends in | Gridline's part |
| --- | --- | --- |
| Gridline project | `.gridline` | Gridline's own type: opens in Gridline by double click |
| Gridline zipped project | `.gridlinez` | Gridline's own type: opens in Gridline by double click |
| Gridline project saved by an earlier version | `.gridline.json`, `.gridline.zip` | still Gridline projects; see below for double click |
| Tracker module | `.xm`, `.mod` | listed in Open with; converted into a new project when opened |

A project opens in place of the one that is open, after the usual question about unsaved
changes. A tracker module is converted and opens as a new project; the module itself is never
changed. Paths with spaces and with letters of any language work on every system.

## Project file names

Gridline saves projects as `Name.gridline` and zipped projects (the project with every file it
uses) as `Name.gridlinez`. Each name has a single extension because macOS and Windows tell a
file's type by its last extension only: they saw an older `Name.gridline.json` file as a plain
JSON file and opened it in a text editor.

A `.gridline` file is the same JSON text as before and a `.gridlinez` file the same zip archive,
so text editors and archive tools can still read them. `.gridlinez` follows the habit of adding
a `z` for a packed form of a format (as `.svgz` is to `.svg`); no other well-known program uses
either name.

Projects saved by earlier versions keep opening everywhere: File > Open, the browser, Recent
files, the start screen, a drop on the window, the command line and the file manager. Save
writes such a project back under its own name; **Save as** starts with the new name. To save in
the older form (for someone on an earlier Gridline), choose **Gridline project, older name
(.gridline.json)** or **Gridline zipped project, older name (.gridline.zip)** in the Save as type
list. An earlier Gridline reads a `.gridline` file it is handed (from the file manager or the
command line) as JSON, but its File > Open and browser do not list it, and it cannot open a
`.gridlinez` file: save in the older form for it.

Templates, backups, crash recovery copies, exported project folders and collaborative project
folders use the new name too. A collaborative project made by an earlier version renames its
own project file to the new name the next time it opens (its folder keeps its name).

## When Gridline is already running

A file you open while Gridline is running opens in that Gridline, which comes to the front, on
every system:

- **macOS:** the system sends the file to the running Gridline.
- **Linux, the BSDs and Windows:** the new launch hands its files to the Gridline that is running
  and closes at once. Starting Gridline again from the menu or a launcher icon, without a file,
  brings the running window to the front instead of opening a second Gridline: each Gridline
  needs the sound card, the MIDI ports and the window layout to itself.

To run several Gridline windows side by side anyway, start Gridline with the environment variable
`GRIDLINE_SINGLE_INSTANCE=0`; every launch then runs on its own.

How it works: the first Gridline listens on a connection only your user account can use, a socket
in your private runtime folder (`$XDG_RUNTIME_DIR`, or Gridline's settings folder when there is
none) on Linux and the BSDs, a named pipe that admits only your account on Windows. One per
display on Linux, so two desktops of one user each have their own. If Gridline crashed, the
socket it left behind is replaced by the next launch. Some desktops (GNOME and KDE on Wayland,
for example) do not let a program come to the front by itself; the later launch hands the running
Gridline the activation token the desktop gave it, so it can come forward (see
[wayland.md](wayland.md)).

## Linux

The `.deb`, `.rpm`, AppImage tools, Flatpak, AUR, FreeBSD packages and the `.tar.gz` `install.sh`
install two things:

- the file types `application/x-gridline-project` (`*.gridline`, and `*.gridline.json` from
  earlier versions) and `application/x-gridline-zipped-project` (`*.gridlinez`, and
  `*.gridline.zip`). Each is a kind of JSON or zip file, so text editors and archive tools still
  offer to open it, but its own name pattern wins, so the file manager shows it as a Gridline
  project;
- the desktop entry, which lists those two types and the standard tracker module types
  `audio/x-xm` and `audio/x-mod`.

Gridline is the only program that lists its own project types, so the file manager opens them
with Gridline right after installing. The system's caches are refreshed by the package manager
(dpkg, rpm, pacman and pkg triggers; `install.sh` runs the same tools for your user).

Tracker modules usually already have a default program, often a media player. To open them with
Gridline by double click, choose it once: right-click a module, **Open With Other Application**
(or **Properties**, **Open With**), pick Gridline and set it as the default. You can do the same
for a Gridline project if you had picked another program for it before.

The Snap package cannot add file types to the system: there, a `.gridline` file shows as a
plain file, and you choose Gridline with Open with as above. An AppImage adds nothing by itself;
an AppImage integration tool, or the `.tar.gz` with `install.sh`, adds the file types and the
entry.

## macOS

Gridline.app declares its project types and the tracker module types. `.gridline` and
`.gridlinez` files are Gridline's own and open in Gridline by double click.

The Finder names a file's type by its last extension only, so it sees a project saved by an
earlier version (`.gridline.json`) as JSON and a `.gridline.zip` file as a zip archive; these open
with the program set for all JSON or zip files. Gridline is listed in **Open With** for them. To
open one with Gridline by double click, select it, choose **File, Get Info**, pick Gridline under
**Open with** (this file only; **Change All** would open every JSON file with Gridline), or open it
in Gridline and save it again, which gives it the new name. Tracker modules list Gridline in Open
With too.

The first time, macOS learns about Gridline's file types when Gridline.app is in the Applications
folder and has been opened once.

## Windows

The installer's **File types** page has three choices:

- Gridline projects, on by default: `.gridline` and `.gridlinez` open with Gridline, and Gridline
  is offered in **Open with** for projects saved by earlier versions (`.gridline.json`,
  `.gridline.zip`);
- tracker modules (`.xm`, `.mod`), on by default, in **Open with**;
- other music programs' projects that Gridline can import, off by default, in **Open with**.

Gridline becomes the default program only for its own `.gridline` and `.gridlinez` files. For
every other type the default program stays yours to choose. Windows names a file's type by its
last extension, so a `.gridline.json` file is a `.json` file: open it from Gridline's own browser
or start screen, or save it again from Gridline to give it the new name. For tracker modules that
no other program claims, Windows asks the first time and lists Gridline at the top.

The package managers that run the setup program install the same entries with the default
choices. The portable `.zip`, also what Scoop installs, registers no file types.
