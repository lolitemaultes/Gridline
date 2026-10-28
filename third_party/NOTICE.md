# Third-party code in Gridline

Gridline is GPL-3.0. The code below is vendored in this folder under its own licence, all
GPL-3.0-compatible; each folder holds the licence text and a VERSION.txt with the exact version,
where it comes from and whether it was changed.

| Folder | Version | Licence | Used for |
|---|---|---|---|
| `imgui/` | Dear ImGui 1.92.9 (docking) | MIT | the interface |
| `lua/` | Lua 5.4 | MIT | scripting (one patch, see README.gridline.md) |
| `rtmidi/` | RtMidi | MIT-style | MIDI devices |
| `signalsmith-stretch/`, `signalsmith-linear/` | Signalsmith Stretch / Linear | MIT | time stretching, pitch shifting |
| `clap/` | CLAP headers | MIT | hosting CLAP plugins, the CLAP build |
| `vst3/` | VST 3 plug-in interfaces | MIT | hosting VST3 plugins, the VST3 build |
| `lv2/` | LV2 headers | ISC | hosting LV2 plugins |
| `dr_libs/`, `stb/stb_vorbis.c` | dr_wav / dr_flac / dr_mp3, stb_vorbis | public domain or MIT-0 / MIT | audio decoding |
| `nanosvg/` | nanosvg | zlib | interface icons |
| `stb/stb_image.h`, `stb/stb_image_write.h` | 2.30 / 1.16 | public domain or MIT | profile pictures (J-111) |
| `libjuice/` | libjuice 1.7.4 | MPL-2.0 | collaboration: ICE, STUN, TURN (J-111) |
| `libplum/` | libplum 0.6.0 | MPL-2.0 | collaboration: router port mapping (J-111) |
| `monocypher/` | Monocypher 4.0.2 | BSD-2-Clause or CC0-1.0 | collaboration: encryption, signatures (J-111) |

MPL-2.0 (libjuice, libplum) is compatible with the GPL-3.0 through MPL-2.0 section 3.3; neither
library carries an "Incompatible With Secondary Licenses" notice. Their source files are unmodified
(libplum's internal helper names are renamed at compile time so both link into one program, see
CMakeLists.txt); the MPL requires that the source of these files stays available, which this
repository does.

Not vendored, but credited: the layout of Serum 1 `.fxp` / VST2 chunks that Plasma reads
(`src/packs/plasma/plasma_serum_dsp.cpp`: parameter order, modulation source ids, menu orders)
follows the format notes of serum2vital (https://github.com/btesser/serum2vital, GPL-3.0).

Not vendored, but credited: Platter's scratch model (`src/packs/platter/platter_dsp.cpp`: the
jog wheel's speed from an alpha-beta filter on its ticks, with the filter values each controller
mapping uses; the mouse's position loop with its throw and release; the ramp back to the motor;
the level curve at low speeds) follows the approach of Mixxx (https://github.com/mixxxdj/mixxx,
GPL-2.0-or-later). No Mixxx source is included; the code is Gridline's own.
