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

## The Open and Save dialog

Gridline's own Open and Save dialog (File > Open, Save as, importing, exporting, loading samples
and every other "choose a file" in Gridline) has three parts:

- **On the left, places.** **Drives** lists this computer's drives and volumes with an icon for
  their kind (the system disk, other disks, USB sticks and card readers, network drives, discs)
  and the free space where it can be read without delay. A drive you plug in or take out shows up
  or goes within a couple of seconds while the dialog is open. **Places** has Home, Desktop,
  Documents, Music and Downloads (on Linux the folders your desktop names in its user folder
  settings), and Gridline's own folders when they exist: the project's folder, **Recorded** and
  **Rendered**. **Favourites** are folders you pin with the star above the list; hover a favourite
  and click its x to remove it. **Recent folders** are the folders you last opened or saved in.
- **In the middle, the folder.** The path bar shows where you are: click a part to go there, or
  click the free end of the bar (or press Ctrl+L, or start typing a path with `/` or `~`) to type a
  path. Tab completes a folder or file name; Enter opens a folder, or opens the file (in a Save
  dialog it names it). Back, Forward and Up are beside it. The list has **Name**, **Size**, **Kind**
  and **Modified** columns: click a column to sort by it, again to reverse; folders stay on top. The
  filter box shows only names that contain what you type, the eye shows or hides hidden files, and
  the file types list chooses which files are shown.
- **At the bottom,** the file name, the type list, **Cancel** and **Open** or **Save**. When saving, the
  name box holds the name alone; the chosen type's extension shows faintly at its end and is added
  to the file, and picking another type changes it. Saving over
  a file that exists asks once: the button turns into **Replace**.

Keys: the arrows, Page Up / Page Down, Home and End move through the list; typing the start of a
name jumps to it; Enter opens a folder or the selected file; Backspace or Alt+Up goes up a folder;
Alt+Left and Alt+Right go back and forward; Esc closes the dialog. Clicking a sound plays it, and
the play button at the start of its row starts or stops it.

The dialog remembers its size, the sort, whether hidden files show, and for each kind of dialog
the folder it was last used in. Drives are looked at in the background and network drives are
never asked for their free space, so a slow or offline drive never holds up the dialog; a folder
on a web or network drive is read in the background too, and shows when it has arrived.

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
