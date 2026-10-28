# Gridline Suite

The Gridline Suite is Gridline's own instruments and effects as plugins for other music programs. Each member is a plugin of its own, with its own name, vendor "Gridline", a category, its parameters, factory presets and its editor, and runs the same sound engine and the same editor code as inside Gridline. The suite comes with every Gridline download and is installed with the application on every system.

## Formats and bundles

| System | Bundles |
|---|---|
| macOS | `Gridline Suite.vst3`, `Gridline Suite.clap`, `Gridline Suite.component` (Audio Unit v2) |
| Windows | `Gridline Suite.vst3`, `Gridline Suite.clap` (plus the `Gridline Suite-assets` folder beside it) |
| Linux, FreeBSD, OpenBSD, NetBSD | `Gridline Suite.vst3`, `Gridline Suite.clap` (plus `Gridline Suite-assets`) |

One bundle per format holds every member, so a host lists them all under the vendor Gridline. VST3 and CLAP on every system and the Audio Unit on macOS cover Live, Logic (Audio Unit only), Reaper, Bitwig, FL Studio and Cubase. Every member passes `auval -strict`, and the test `suite` loads every member in every format through Gridline's own plugin host.

## Where it is installed

| Download | Installed to | How |
|---|---|---|
| Windows installer | `C:\Program Files\Common Files\VST3\Gridline Suite.vst3`, `C:\Program Files\Common Files\CLAP\Gridline Suite.clap` | always, by the installer; removed by the uninstaller |
| Windows `.zip` | `plugins\VST3`, `plugins\CLAP` beside the program | Gridline offers to copy them to `%LOCALAPPDATA%\Programs\Common\VST3` and `...\CLAP` |
| macOS `.dmg` | inside `Gridline.app/Contents/Resources/Suite` | Gridline offers to copy them to `~/Library/Audio/Plug-Ins/VST3`, `.../CLAP` and `.../Components` |
| Linux `.deb`, `.rpm` | `/usr/lib/vst3/Gridline Suite.vst3`, `/usr/lib/clap/Gridline Suite.clap` | always, by the package |
| Linux AppImage | inside the image (`usr/share/gridline/suite`) | Gridline offers to copy them to `~/.vst3` and `~/.clap` |
| Linux and BSD `.tar.gz` | `lib/vst3`, `lib/clap` in the archive | Gridline offers to copy them to `~/.vst3` and `~/.clap` |

Where Gridline offers the copy, it asks once, after its window is up the first time. **Install** copies the bundles and keeps them at Gridline's version: when an updated Gridline starts, it updates the copies. **Never** leaves the plug-in folders alone. **Settings, Files, Gridline Suite** shows the state and installs, updates or removes the copies at any time, and from a terminal:

```sh
gridline suite status     # shipped and installed versions, the folders
gridline suite install    # copy (or update) into this user's plug-in folders
gridline suite remove     # delete those copies (system-wide copies belong to their package)
```

Each bundle carries `gridline-suite.txt` (`Gridline Suite <version> <build>`), which Gridline compares to tell whether a copy is current. Automated runs (`--hidden`, screenshots, `GRIDLINE_NO_PLUGIN_SCAN`, `GRIDLINE_NO_SUITE_INSTALL`) never ask and never copy. After installing, rescan plugins in the other program.

## What each plugin does

- **Audio:** instruments have a stereo output and take MIDI (notes, pitch bend, controllers). Effects have a stereo input and output; the effects that use a sidechain have a stereo sidechain input (marked below). Effects that play from notes also take MIDI.
- **Parameters:** every parameter of the plugin as a host parameter, with its name, unit, range, steps or choices and value text, automatable with begin and end of each gesture. Effects add **Mix** and **Bypass** (the format's bypass switch). Parameter ids are the parameter's index in Gridline (Mix and Bypass are 1048576 and 1048577), so automation stays attached across versions.
- **Presets:** the plugin's factory presets in the editor's preset menu; the Audio Unit also lists them as factory presets.
- **State:** the plugin's state exactly as Gridline saves it in a project (JSON: parameters, sample references by path, the plugin's own data), with Mix, Bypass and the editor's zoom. A suite plugin also loads a Gridline preset file's state of the same plugin.
- **Latency** is reported to the host and updated when it changes; effects report a tail.
- **Editor:** the plugin's editor from Gridline, with its header (presets, menu) and, for sample-based instruments, a sample loader. The window is resizable: the editor zooms to fit (0.5 to 3 times) and keeps its proportions.
- **Cipher** (the vocoder) plays its carrier from the host's MIDI when its **Carrier MIDI port** parameter is 0 (the default, -1, uses the keys stored in the plugin); **Plumb** and **Tideline** hear the host's MIDI by default.

## Identifiers

Identifiers come from the plugin type `gl.<name>` alone, never from the suite, so they never change, and a type can move to another suite without hosts losing it.

| Format | Identifier |
|---|---|
| CLAP | `org.gridline.suite.<name>` |
| VST3 | class id `47524944-4C494E45-53554954-<hash>` (`GRIDLINESUIT` + hash) |
| Audio Unit | manufacturer `GrLn`, type `aumu` (instrument), `aufx` (effect) or `aumf` (effect that takes MIDI), subtype four characters `[a-z][a-z0-9]{3}` from the hash |

`<hash>` is the 32-bit FNV-1a hash of the type (`gl.comp`). Gridline's own plugins (Gridline and Gridline FX) are `org.gridline.gridline` / `org.gridline.gridline-fx`, VST3 `GRIDLINEPLUG` 1 and 2 and Audio Units `GrIn` / `GrFx` (upper case), so they never collide with suite members; the test `suite` checks this, and that the members' identifiers are unique. `gridline suite-info gridline` prints every member with its identifiers.

## Members

118 plugins: 50 instruments and 68 effects. The CLAP id is `org.gridline.suite.` followed by the type without `gl.`; the VST3 class id ends in the hash shown; the Audio Unit is `aumu` (instruments), `aufx` or `aumf` (effects that take MIDI) with the subtype shown and manufacturer `GrLn`. Effects also have Mix and Bypass besides the parameters counted here.

### Instruments

| Plugin | Type | Category | VST3 hash | AU | Parameters | Presets | Notes |
|---|---|---|---|---|---|---|---|
| Tercet | `gl.synth` | Synth | `B52A16F6` | `kcxv` | 48 | 0 |  |
| Sampler | `gl.sampler` | Sampler | `C34DF060` | `eo8f` | 61 | 5 |  |
| Kick | `gl.kick` | Drums | `11F6C47E` | `gmiq` | 17 | 4 |  |
| Snare | `gl.snare` | Drums | `C09D119D` | `lzf8` | 16 | 4 |  |
| Hi-hat | `gl.hat` | Drums | `F0DB98CD` | `n27g` | 11 | 4 |  |
| Clap | `gl.clap` | Drums | `E4AE75BE` | `e7m2` | 11 | 4 |  |
| Pluck | `gl.pluck` | Synth | `8CEFA7D1` | `vwai` | 9 | 4 |  |
| Abacus | `gl.pads` | Drums | `F1E5D9D2` | `ke2u` | 1731 | 0 |  |
| FM Synth | `gl.fm` | Synth | `B7737B3D` | `trgi` | 74 | 5 |  |
| Acid Bass | `gl.acid` | Synth | `0C64F93D` | `bogp` | 105 | 4 |  |
| Keys | `gl.keys` | Synth | `7D2CDE14` | `asyi` | 20 | 4 |  |
| Kerf | `gl.slicer` | Sampler | `3FDD38A0` | `m86j` | 866 | 4 |  |
| Granular | `gl.granular` | Sampler | `5209A718` | `yysw` | 21 | 4 |  |
| Analog Synth | `gl.analog` | Synth | `9A8F41C8` | `qmex` | 107 | 12 |  |
| Isoline | `gl.wavetable` | Synth | `D8849DCB` | `fxgu` | 131 | 11 |  |
| Lattice | `gl.matrixfm` | Synth | `04916A50` | `ecpg` | 1150 | 5 |  |
| Hybrid Synth | `gl.gms` | Synth | `7D987301` | `fc7b` | 140 | 6 |  |
| Twin VA | `gl.twinva` | Synth | `A2C5B267` | `bdhi` | 81 | 5 |  |
| Saw Swarm | `gl.sawswarm` | Synth | `060EB547` | `tca2` | 96 | 5 |  |
| Finger Bass | `gl.fingerbass` | Synth | `DDB150DE` | `ejwe` | 3 | 4 |  |
| DCO Synth | `gl.dcosynth` | Synth | `839D8968` | `cthl` | 174 | 6 |  |
| Warm Trio | `gl.warmsynth` | Synth | `23C25569` | `f4pu` | 58 | 5 |  |
| FM Duo | `gl.fmduo` | Synth | `4BD8389F` | `vn98` | 22 | 5 |  |
| Hex Synth | `gl.hexsynth` | Synth | `E2466BD1` | `jyhs` | 340 | 4 |  |
| Pocket Synth | `gl.pocketsynth` | Synth | `45CB268C` | `q8fk` | 43 | 4 |  |
| Pixel Tone | `gl.pixeltone` | Synth | `32E51138` | `kpr6` | 8 | 3 |  |
| Sample Keys | `gl.samplekeys` | Synth | `C0DE6BFE` | `itjr` | 15 | 3 |  |
| Multisampler | `gl.multisampler` | Sampler | `F917CD0F` | `heid` | 33 | 5 |  |
| Folio | `gl.waveprogram` | Sampler | `AC529E57` | `donl` | 32 | 0 |  |
| Scratch Sampler | `gl.scratch` | Sampler | `F5360B4A` | `ifqo` | 15 | 5 |  |
| Junction | `gl.patcher` | Utility | `3C044259` | `b2ic` | 138 | 3 |  |
| Facet | `gl.additive` | Synth | `21B8E884` | `kflo` | 155 | 10 |  |
| Halftone | `gl.harmor` | Synth | `43035B7F` | `vj23` | 259 | 5 |  |
| Image Synth | `gl.imagesynth` | Synth | `C1937EC1` | `l1pj` | 18 | 9 |  |
| String Model | `gl.string` | Synth | `3407BBF9` | `dd3v` | 40 | 12 |  |
| Drum Model | `gl.drumkit` | Drums | `7697A654` | `eqch` | 329 | 8 |  |
| Membrane Pad | `gl.membranepad` | Drums | `1CEFC772` | `yfth` | 40 | 5 |  |
| Membrane Kit | `gl.membranekit` | Drums | `FDE7F43F` | `h6xx` | 720 | 3 |  |
| Kick Sculptor | `gl.kicksculpt` | Drums | `4B009801` | `b2vl` | 24 | 4 |  |
| Sweep Kick | `gl.sweepkick` | Drums | `44422846` | `uz3b` | 7 | 3 |  |
| Twin String | `gl.twinstring` | Synth | `BAF0059B` | `bvdq` | 115 | 6 |  |
| Pluck Model | `gl.plucker` | Synth | `DD9F486A` | `wwtf` | 5 | 5 |  |
| Step Bass | `gl.stepbass` | Synth | `FE262599` | `jqwa` | 47 | 5 |  |
| Alloy | `gl.alloy` | Synth | `C00D7B67` | `lg6g` | 119 | 5 |  |
| Spectra Morph | `gl.spectramorph` | Synth | `F85EADC2` | `gogd` | 28 | 3 |  |
| Grain Scanner | `gl.grainscan` | Sampler | `42FB5F62` | `ehjo` | 23 | 4 |  |
| Spline Scratch | `gl.splinescratch` | Sampler | `10B6BE51` | `ng3f` | 8 | 3 |  |
| Seltmann | `gl.seltmann` | Sampler | `A19C8FCA` | `mtkg` | 21 | 6 |  |
| Platter | `gl.platter` | Sampler | `74A19BB4` | `q8wc` | 56 | 6 | Takes MIDI from DJ controllers |
| Plasma | `gl.plasma` | Synth | `F95755DE` | `cw2s` | 2192 | 24 |  |

### Effects

| Plugin | Type | Category | VST3 hash | AU | Parameters | Presets | Notes |
|---|---|---|---|---|---|---|---|
| Parametric EQ | `gl.eq` | Filter | `D36FD5BA` | `izjk` | 79 | 6 |  |
| Filter | `gl.filter` | Filter | `A3573D62` | `sujd` | 14 | 6 |  |
| Compressor | `gl.comp` | Dynamics | `571A3C67` | `pyjy` | 15 | 4 | sidechain |
| Limiter | `gl.limiter` | Dynamics | `58B8C450` | `o7rc` | 22 | 5 | sidechain |
| Gate | `gl.gate` | Dynamics | `98DDA067` | `f30h` | 10 | 3 |  |
| Tracer | `gl.delay` | Delay & reverb | `59E737DF` | `fz0o` | 75 | 7 |  |
| Reverb | `gl.reverb` | Delay & reverb | `88E559C4` | `awnm` | 37 | 6 |  |
| Chorus | `gl.chorus` | Modulation | `7ACB76E2` | `e5pl` | 18 | 6 |  |
| Flanger | `gl.flanger` | Modulation | `563D9DC9` | `ltg1` | 20 | 5 |  |
| Phaser | `gl.phaser` | Modulation | `D679336F` | `dwgk` | 23 | 5 |  |
| Distortion | `gl.dist` | Distortion | `470D4020` | `qqwy` | 13 | 4 |  |
| Bitcrusher | `gl.bitcrush` | Distortion | `B78EB91A` | `m5fz` | 16 | 7 |  |
| Clipper | `gl.clipper` | Dynamics | `1CEDF6A5` | `lbae` | 6 | 3 |  |
| Stereo Width | `gl.stereo` | Misc | `DB6C2BEC` | `whm0` | 18 | 6 |  |
| Transient Shaper | `gl.transient` | Dynamics | `706D3E2E` | `q9g7` | 10 | 5 |  |
| Multiband Compressor | `gl.multiband` | Dynamics | `C3836306` | `qx6b` | 37 | 4 |  |
| Analyzer | `gl.analyzer` | Misc | `BF652842` | `kusd` | 8 | 0 |  |
| Graphic EQ | `gl.geq` | Filter | `6B84CEF3` | `ndtb` | 11 | 4 |  |
| Waveshaper | `gl.waveshaper` | Distortion | `9FB72D10` | `yij8` | 29 | 5 |  |
| Auto Pan | `gl.autopan` | Modulation | `41A9A4E6` | `qpof` | 13 | 5 |  |
| Shaper | `gl.shaper` | Filter | `ABFDD055` | `bwp0` | 69 | 3 |  |
| Shuttle | `gl.timegate` | Modulation | `BA72D1CE` | `ckvy` | 40 | 6 |  |
| Maximizer | `gl.maximizer` | Dynamics | `99C93FC8` | `cpa8` | 40 | 4 |  |
| Freq Shifter | `gl.freqshift` | Modulation | `BEA0D5E2` | `syyr` | 18 | 4 |  |
| Sub Focus | `gl.lowlift` | Filter | `AEFDBEFB` | `v99h` | 26 | 4 |  |
| Tuner | `gl.tuner` | Misc | `A7592FE4` | `gpxs` | 5 | 0 |  |
| Utility | `gl.utility` | Misc | `66AFDAB8` | `eg1h` | 11 | 4 |  |
| Convolver | `gl.convolver` | Delay & reverb | `6933D242` | `e5ha` | 27 | 6 |  |
| Multi Delay | `gl.multidelay` | Delay & reverb | `F7B01398` | `gjvx` | 255 | 6 |  |
| Band Delay | `gl.banddelay` | Delay & reverb | `F7ACE9FE` | `u3pr` | 24 | 5 |  |
| Cloister | `gl.hall` | Delay & reverb | `CA76AE4B` | `nxrg` | 63 | 6 | sidechain |
| Ensemble | `gl.ensemble` | Modulation | `D2192A4F` | `b921` | 24 | 5 |  |
| Bucket Chorus | `gl.bbdchorus` | Modulation | `A6BE001A` | `iz3e` | 19 | 6 |  |
| Swirl | `gl.swirl` | Modulation | `F5B54E87` | `nz8j` | 16 | 5 |  |
| Retro Phaser | `gl.vphaser` | Modulation | `E132AEF1` | `ll7v` | 42 | 6 |  |
| Cipher | `gl.vocoder` | Spectral | `3E0875BC` | `oxh8` | 64 | 6 | sidechain, MIDI in |
| Pitch Shifter | `gl.pitch` | Spectral | `CE5B40E6` | `y10a` | 23 | 6 |  |
| Plumb | `gl.pitcher` | Spectral | `25320011` | `j2sp` | 61 | 6 | MIDI in |
| Turntable | `gl.scratcher` | Spectral | `F6DC4F1D` | `v6zh` | 13 | 5 |  |
| Stereo Spread | `gl.spread` | Spectral | `5CA9EDFF` | `xuiv` | 18 | 6 |  |
| Hit Repeater | `gl.hitloop` | Delay & reverb | `5E1CF78B` | `ft5w` | 28 | 4 | sidechain |
| Crest | `gl.master` | Dynamics | `BD005956` | `ksx9` | 293 | 6 |  |
| Loudness | `gl.emphasis` | Dynamics | `2B2F87CE` | `geek` | 25 | 5 |  |
| Enhance | `gl.enhance` | Dynamics | `7E374FCA` | `if6w` | 302 | 5 |  |
| Pedal Rack | `gl.amprack` | Distortion | `97D4B7BD` | `xxs6` | 347 | 6 |  |
| Overdrive | `gl.overdrive` | Distortion | `EA200D1E` | `we2c` | 9 | 5 |  |
| Clip Dist | `gl.fastdist` | Distortion | `A23A8162` | `wkrz` | 10 | 6 |  |
| Pinch | `gl.squeeze` | Distortion | `9CC0CC04` | `y0j9` | 23 | 6 |  |
| Multi FX | `gl.effector` | Misc | `B5B0D948` | `es05` | 24 | 6 |  |
| Transient Split | `gl.transplit` | Dynamics | `542E98EB` | `fhgk` | 73 | 6 | sidechain |
| Pocket FX | `gl.pocketrack` | Multi FX | `4DA44541` | `hz03` | 140 | 6 |  |
| Junction FX | `gl.patcherfx` | Utility | `ABCF70CF` | `fzii` | 138 | 3 |  |
| Sidechain Ducker | `gl.scduck` | Dynamics | `6385C947` | `ztiq` | 7 | 4 | sidechain |
| Curve EQ | `gl.drawneq` | Filter | `CAECD9B6` | `qtlu` | 99 | 5 |  |
| Morph EQ | `gl.bankeq` | Filter | `07342630` | `u8zw` | 945 | 6 |  |
| Biquad Filter | `gl.freefilter` | Filter | `BEFCC9D8` | `u8sq` | 7 | 6 |  |
| Quick Lowpass | `gl.fastlp` | Filter | `04948A32` | `y3rm` | 5 | 4 |  |
| Bass Boost | `gl.bassboost` | Filter | `A27E7FF6` | `sc0n` | 3 | 4 |  |
| Graticule | `gl.scope` | Visual | `6444AF14` | `oxs1` | 33 | 6 |  |
| Spectrogram | `gl.spectro` | Visual | `461C0BA6` | `q9rx` | 7 | 4 |  |
| Level Meter | `gl.meter` | Visual | `DE8773C1` | `jqdz` | 5 | 4 |  |
| Clock | `gl.clock` | Visual | `B733FFA6` | `cwz2` | 2 | 4 |  |
| Notes | `gl.notes` | Visual | `434BD40D` | `dr00` | 1 | 2 |  |
| Mute | `gl.mute` | Misc | `E2DBA6F9` | `noqu` | 3 | 4 |  |
| Stereo Matrix | `gl.stereomatrix` | Misc | `07BDDB15` | `f2tc` | 11 | 6 |  |
| Octa Filter | `gl.octafilter` | Filter | `E5FD1DB3` | `lui5` | 481 | 6 |  |
| Loupe | `gl.edison` | Misc | `A9077CCC` | `c3t1` | 9 | 5 |  |
| Tideline | `gl.tideline` | Dynamics | `84DEDC4E` | `wyfy` | 37 | 11 | sidechain, MIDI in |

## Not in the suite

Plugins that only make sense inside Gridline are left out: routing and controller plugins that drive other Gridline parameters, note processors, the multi-output and send plugins and the stand-ins for third-party plugins. `gridline suite-info gridline --excluded` lists them with the reason.

## Adding a plugin, or another suite

- Every built-in instrument and effect joins the suite `gridline` unless it is excluded (`kExcluded` in `src/core/PluginSuite.cpp`).
- To put a plugin in another suite, add one line to `kAssignments` in `src/core/PluginSuite.cpp`: `{"gl.<name>", "<suite id>"}`.
- A new suite is one line in `kSuites` (`{"<id>", "<bundle name>", "<macOS bundle id>"}`) and one `gridline_suite(<id> "<bundle name>" <bundle id>)` line in `cmake/GridlinePlugins.cmake`; the build then makes `<bundle name>.vst3`, `.clap` and `.component`, `cmake --install` installs them on Linux and the BSDs, and the app's install offer covers every suite. The macOS, Windows and AppImage packaging name the bundles: add the new names next to `Gridline Suite` in `tools/release/package-macos.sh` (`SUITE_BUNDLES`), `tools/windows/gridline.iss` and `.github/workflows/release.yml`.
