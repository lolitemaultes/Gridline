# The CPU report (development)

`gridline-app --cpu-report <seconds>` measures what Gridline costs while it sits idle: a project
open, the transport stopped, nobody touching anything. It is for development and tests; a normal
start of Gridline never runs it.

```
gridline-app <project> --hidden --cpu-report 20 [--cpu-settle 10] \
    [--cpu-states visible,unfocused,minimised,hidden] [--cpu-report-out report.json] \
    [--layout studio|live|logic|tracker]
```

Always run it hidden, with `SDL_AUDIODRIVER=dummy` and `HOME` and `GRIDLINE_CONFIG_HOME` in a
scratch folder (`tools/cpu/cpurun.py` does all of that). Once the project has loaded, for each
state in turn the window is put in that state, the run waits `--cpu-settle` seconds (default 10)
for meters to fall and frames to slow to the idle pace, and then records for the given seconds. A
hidden run's window is never on screen, so the unfocused, minimised and hidden states are given as
the events the window system would send. Then the report is written (by default
`cpu-report.json` in the config folder) and the run quits without saving.

Interactive frame pacing stays on during the report, as in a real session.

## What it records

For each state:

- **process**: processor time (user and system) as a percentage of one core, context switches per
  second and, on macOS, system calls per second and bytes read from and written to disk;
- **threads**: every thread's processor time, one by one and summed by name. Every thread Gridline
  starts names itself (`gl-main`, `gl-audio`, `gl-audio-pool`, `gl-index`, `gl-midi-send`,
  `gl-warp`, `gl-collab-hub`, ...), so the names also show in system tools and crash reports.
  Per-thread times are read on macOS and Linux;
- **cpu_split**: the process split into Gridline's own threads and the system's. Own is every
  thread Gridline starts (`gl-*`; the interface thread counts whole, its waits for events and the
  window system's work on it included) and, of the audio device thread, only the time inside
  Gridline's audio callback: the audio driver makes that thread, and what it spends waiting and
  waking between callbacks is listed as the system's `gl-audio (driver)`. System is everything
  else: the driver's share, CoreAudio and HAL threads, the display link, SDL's own threads and the
  system's unnamed threads (and threads that ended while recording). The split also gives the
  audio thread's total, its callback and driver shares, the interface thread's drawing time, and
  the threads of each side by name;
- **frames** drawn and frames per second;
- **audio**: device callbacks, engine blocks, blocks that carried sound, and how often the audio
  worker pool woke;
- **nodes**: every instrument and effect in the project with its type and the number of blocks it
  ran its DSP while recording (a resting one runs none), and for instruments the blocks spent at
  rest;
- **fs_calls**: file system questions asked through Gridline's own checked wrappers;
- **worker_wakeups**: how often each long-lived worker woke from waiting, by thread name.

With the report off, none of these counters count; each costs one check at its place, and nothing
you hear or see changes either way (renders are bit-identical with counting on and off).

## The sweep

`tools/cpu/sweep.py` runs the report over every layout, the demo song, an empty project and a
large one (200 channels, 2000 clips), in each window state, one run after another, and writes the
JSON of each run and a table per project:

```
python3 tools/cpu/sweep.py --app build/gridline-app --out <folder> [--seconds 20] [--settle 10]
```

## Tests

- `k142_harness`: the counters change nothing heard, count only while on, and thread names and
  wakeups read back.
- `k142_cpu_report_app`: a short report of the demo song through every window state is complete.
- `k142_idle_budget_app`: the demo song idle in every layout, visible and hidden, keeps to the
  idle budget: frames at most the idle pace while visible and none hidden, no DSP in silent
  instruments and effects, no worker waking more than 4 times a second, no display link while
  hidden, and Gridline's own threads at most 2 % of one core visible and 0.5 % hidden (times
  `GRIDLINE_K142_CPU_SCALE`, default 1.5; 0 leaves the time unjudged). The system's threads are
  printed beside them and fail the test only past a generous sanity bound, which means Gridline
  is driving them abnormally (an audio driver woken far too often, a display link kept running).
