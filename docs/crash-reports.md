# Crash reports and recovery

When something goes wrong, Gridline shows a window with a **crash code**: a short text that says
what failed and where. Nothing is sent anywhere. You decide whether to share the code.

## What you see

| What happened | What Gridline does | Your project |
| --- | --- | --- |
| A serious error (a crash, in Gridline or in a plugin running inside it) | It closes, because its memory can no longer be trusted, and a small window appears at once with the code. | **Restart and recover project** opens a recovery copy. |
| The interface stops answering for 30 seconds | A small window appears next to the frozen one with **Keep waiting** and **Force restart**. If Gridline comes back, the window closes itself and nothing is kept. | On macOS, Linux and the BSDs, Force restart first tries to save a fresh copy. On Windows the last automatic copy is offered. |
| A lesser error (a project that fails to load, an export or a plugin call that fails) | The same window, inside Gridline, which keeps running. | Still open. |
| Gridline ended and could not say why (closed from outside, or the computer ran out of memory) | The window appears with what is known. | The last recovery copy is offered. |

A plugin in **Run in sandbox** that crashes does not take Gridline with it: the plugin shows as
crashed, as before, and you can reload it.

If the small window could not start, the next start of Gridline shows the same report.

## Recovery copies

While a project has unsaved changes, Gridline keeps a copy of it, about every 30 seconds. When it
crashes on macOS, Linux or a BSD, a separate process also tries to save the project as it was at
that moment. That can fail, or the copy can contain damage if the error had already damaged the
project in memory, so the window tells you which copy you have and how old it is, and **a recovered
project opens as unsaved**: look it over before you save over the original. On Windows the copy is
the last automatic one.

The copies never replace your project file. They stay for 30 days, or until you recover and save,
in the `recovery` folder of Gridline's settings folder. A normal quit removes them.

Recovery copies are separate from the timed backups you can set in **Settings**, which are kept
whether or not Gridline crashes.

## Sharing a code

**Copy code** puts the whole text on the clipboard. Paste it in a [GitHub
issue](https://github.com/lolitemaultes/Gridline/issues) or wherever you prefer to ask, and say what
you were doing if you can.

The code holds: the Gridline version and build, the system and its version, the graphics card, the
kind of error, the failing thread, the stack as file names of Gridline's and plugins' libraries with
offsets, and your last actions by name ("edit add channel", "project save", "export start"). The
names of plugin libraries can appear there, and a channel name if your last edit's name contained
one. Text from error messages has its folders and your user name removed. It holds no file paths, no
project names, no samples, and nothing from your project.

Read it before you paste it: it is plain text for that reason.

Reports stay in the `crash-reports` folder of Gridline's settings folder for 30 days (at most 30 of
them). **Settings, Debug, Crash reports** opens that folder and the recovery folder.

## For developers: from a code to a line

Each release has `Gridline-<version>-symbols-<target>.zip` on its
[releases page](https://github.com/lolitemaultes/Gridline/releases) for macOS, Windows and Linux.

```
unzip Gridline-0.7.3-symbols-linux-x86_64.zip -d symbols
python3 tools/crash_symbolize.py code.txt --symbols symbols
```

`code.txt` can be the whole message the code was pasted in, quote marks included; `-` reads standard
input. It needs `atos` (macOS), `addr2line` or `llvm-symbolizer` (Linux, BSD), or `llvm-symbolizer`
(Windows). Frames ending in ` ?` came from scanning the stack where the frame chain ended early and
can be stale. Reports of BSD builds name modules and offsets, but those builds have no symbol files
yet: a build of the same commit with `-DGRIDLINE_RELEASE_SYMBOLS=ON` resolves them.

`GRIDLINE_NO_CRASH_HANDLER=1` turns all of this off, for working with a debugger or core files.
