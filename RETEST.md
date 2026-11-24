# What to re-test after processor time fixes

Each fix that changes how Gridline waits, wakes or skips work lists here what it touches and how to
check it. Run the listed tests quietly (hidden windows only) on the platforms named.

## The interface thread sleeps while nothing is shown (0.7.8)

What changed:

* `src/app/IdlePace.h` `planWait`: with no window of Gridline's on screen, the main loop no longer
  looks four times a second for changes without an event. It sleeps until its next duty (a changed
  project's backups and autosave, the CPU report's steps) or a wake, and wakes every 5 s for the freeze
  watchdog. With a game controller attached it still looks four times a second (SDL reads controller
  buttons only when the loop looks).
* `src/host/ui/NativeViewX11.cpp` `displayWaitStart` / `displaySleep` / `frameClockWake`: with SDL
  older than 2.0.16 on X11 (Debian 11, the Chromebook's Linux), the idle wait sleeps on SDL's X
  connection and a wake pipe instead of SDL_WaitEventTimeout, which looks for events every few
  milliseconds there. `GRIDLINE_DISPLAY_WAIT=1` forces this wait with any SDL, `0` turns it off.
* `src/app/main.cpp`: the wait above; events already queued (this thread's own pushes) end the wait
  before it sleeps; a sleep never lasts more than a third of the freeze watchdog's limit
  (`crash::frozenAfterSeconds`); a hidden run's SIGTERM ends a sleep at its next wake.
* `src/app/CpuReport.*`: the report's `main_loop` (sleeps, what ended them, turns, waiting time).
* `tests/k142_idle_budget_app.py`: hidden own time back to 0.5 x 1.5 % off macOS; the interface thread
  alone at most 0.15 % hidden.

What to re-test, and how:

| Area | Why | Test |
| --- | --- | --- |
| Idle budget, every layout | the change itself | `k142_idle_budget_app` alone, three times, machine quiet |
| Idle pacing and wakes | the wait's plan and its wakes | `k142_ui`, `k142_wake`, `k142_ui_wake_app`, `k142_ui_pacing_app`, `k142_wake_sources_app`, `k142_cpu_report_app` and the other `k142_*` |
| Hidden and quiet windows | nothing shown is the path that changed | `quiet_windows_app`, `k078_browser_hidden`, `test_browser_hidden`, `k078_mixer_hide` |
| MIDI input and hotplug | input from another thread must wake the sleep | `midihp1119_hotplug_app`, `midi1118_hidden_ports_app`, `k142_midi_send` |
| OSC and the AI assistant | requests wake the loop | `k201_osc_app`, `k142_wake_sources_app` |
| Collaboration | messages wake the loop; a session keeps the idle frames | `collab` and `*collab*` tests |
| Backups and autosave | a changed project's housekeeping turn while hidden | autosave and recovery tests (`*autosave*`, `*recovery*`, `*backup*`) |
| Browser | file index and hidden browser | `*browser*` tests |
| Freeze watchdog | a sleeping interface must not count as frozen | `*crash*` / `*freeze*` / `*watchdog*` tests; leave a hidden run idle over 30 s and check no freeze report |
| Linux with SDL older than 2.0.16 | the X11 wait | the ChromeOS workflow's Debian 11 jobs (x86_64 and arm64): `k142_*`, `k201_osc_app`, `midihp1119_hotplug_app`, collab tests |
| Linux with newer SDL | `GRIDLINE_DISPLAY_WAIT=1` exercises the same wait | the Linux test machine: `GRIDLINE_DISPLAY_WAIT=1 ctest -R "k142_|k201|midihp"` |
| Game controller | looks stay four a second while one is attached | on a machine with a controller: Gridline minimised, a controller button still reaches its action |

## MIDI devices while hidden (follow-up to the hidden idle wait)

The hidden loop no longer takes timed turns, and MIDI's device look ran only on turns: a controller
plugged in while Gridline was minimised opened 10 to 15 s later (CI: `k142_wake_sources_app`). Now
the system's "devices changed" notice (CoreMIDI, the Linux /dev/snd watch), a stand-in test device
and the background probe's answer each wake the loop, and the hidden wait also ends for MIDI's own
timed look (`MidiManager::secondsToNextLook`: the 1.5 s look where the system does not report
changes, a retry of a service that refused).

What changed:

* `src/core/MidiOpen.*`: `setOnAnswer`, run on the probe's thread once its answer is ready.
* `src/app/Midi.*`: the opener's answer pokes the loop; `secondsToNextLook`.
* `src/app/MidiBackend.cpp`: setup changes and stand-in devices poke the loop.
* `src/app/IdlePace.h` / `src/app/main.cpp`: `planWait` takes `dutyLeft` (a full turn when due).

| Area | Why | Test |
| --- | --- | --- |
| MIDI hotplug | the change itself | `midihp1119_hotplug_app`, `k142_wake_sources_app`, `midi_open` |
| MIDI ports in hidden runs | hidden runs keep no system ports | `midi1118_hidden_ports_app` |
| Idle pacing | the wait's plan | `k142_ui`, `k142_ui_wake_app`, `k142_ui_pacing_app` |
| Idle budget | a hidden run with system MIDI ports now wakes for MIDI's look (every 10 s where watched) | `k142_idle_budget_app` (CI decides) |
| Windows and the BSDs | no change notice there: the 1.5 s look keeps hidden hotplug as before | the Windows test machine: plug a controller in while Gridline is minimised, it works when restored |
