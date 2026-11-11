# SFZ instruments

The Multisampler plays SFZ instruments (`.sfz`) as they are written: every region, with its
envelopes, modulators, controllers, keyswitches, filters and effects. Nothing is converted on the
way in, so the instrument sounds the way its author made it, and the project keeps a reference to
the `.sfz` file itself.

## Opening an instrument

- In the browser, `.sfz` and `.sf2` files are listed under **Multisample instruments**. Click one
  to hear a note of it, double-click it for a new Multisampler channel, or drag it onto the
  channel rack (a new channel, or onto a channel to replace its instrument), the playlist or an
  open Multisampler window.
- **File > Import > Multisample instrument (.sfz, .sf2)...** adds a Multisampler channel playing
  it. The command palette finds the same entry.
- In the Multisampler, the **SFZ** button loads an instrument into the open channel.

Samples are read when the instrument opens. A sample that is missing is listed in the editor and
the regions that have their samples still play. A file that is not an SFZ instrument, or an
opcode Gridline does not know, never stops the instrument from loading.

## The editor

While the Multisampler plays an SFZ instrument its window shows:

- the number of regions, groups and samples, and how many files are missing;
- the region map: keys across, velocity up, one colour per group, release regions paler. Point
  at a region to see its sample, ranges, root key, labels and keyswitch. The keyboard under the
  map plays notes; keyswitch keys are marked, the default one with a green dot;
- **Controls**: a knob for every controller the instrument names (`label_ccN`). Their settings are
  saved with the project;
- **Keyswitches**: the named articulations;
- **Reader notes**: missing files, words that are not SFZ opcodes, and opcodes Gridline has no
  counterpart for.

**Convert to zones** turns the instrument into ordinary Multisampler zones that you can edit:
mapping, tuning, level, pan, sample points, loops and the amplitude envelope come along. What
zones cannot hold (controllers, region filters, modulators, keyswitches, release regions) is
left out, and a notice says how many regions used it. Undo returns to the SFZ instrument.

## Playing

- MIDI controllers, pitch bend, channel and key pressure, program changes, the sustain and
  sostenuto pedals, the MIDI channel and release velocity of each note reach the instrument.
- Slide notes glide the sounding regions to the slide's pitch.
- The channel's volume and its transpose and fine tune apply on top of the instrument.
- The instrument's own effects (`<effect>`) run inside the channel, on their buses, before the
  channel's mixer track.

## Projects

The project keeps the path of the `.sfz` file and the controller knobs' settings. Zipped
projects, **Export project data files** and collaborative projects carry the instrument with its
samples, its included files, its picture and its impulse responses, rewritten to name them
beside it.

## Opcode coverage

Every opcode of SFZ v1, SFZ v2 and the widely used extensions to v2 is read. Each one is:

- **Plays**: it changes the sound or the playing the way the format describes;
- **Mapped**: it is taken onto the nearest thing Gridline has (the note says what);
- **Shown**: a label, shown in the editor;
- **Reported**: Gridline has no counterpart; it is listed under Reader notes and the region
  plays without it.

In the names below, `N`, `X` and `Y` stand for numbers (`cutoffN_onccX` covers `cutoff2_oncc74`).

| Support | Opcodes |
|---|---|
| Plays | 814 |
| Mapped | 20 |
| Shown | 6 |
| Reported | 9 |
| All | 849 |

### Instrument settings

| Opcode | Support | Note |
|---|---|---|
| `#define` | Plays |  |
| `#include` | Plays |  |
| `*_mod` | Mapped | modulations add; variables follow varNN_mod |
| `amplitude` | Plays |  |
| `bitred` | Plays |  |
| `cutoff` | Plays |  |
| `cutoff2` | Plays |  |
| `decim` | Plays |  |
| `default_path` | Plays |  |
| `fil2_gain` | Plays |  |
| `fil_gain` | Plays |  |
| `global_label` | Shown | shown in the editor |
| `group` | Plays |  |
| `group_label` | Shown | shown in the editor |
| `hint_*` | Mapped | kept; Gridline needs no engine hints |
| `label_ccN` | Shown | shown in the editor |
| `loop_end_offset` | Plays |  |
| `master_label` | Shown | shown in the editor |
| `note_offset` | Plays |  |
| `note_polyphony` | Plays |  |
| `note_selfmask` | Plays |  |
| `octave_offset` | Plays |  |
| `off_by` | Plays |  |
| `off_curve` | Plays |  |
| `off_mode` | Plays |  |
| `off_shape` | Plays |  |
| `off_time` | Plays |  |
| `offby` | Plays |  |
| `output` | Mapped | every output mixes into the channel |
| `pan` | Plays |  |
| `pitch` | Plays |  |
| `polyphony` | Plays |  |
| `polyphony_group` | Plays |  |
| `polyphony_stealing` | Mapped | the oldest voice is taken |
| `position` | Plays |  |
| `region_label` | Shown | shown in the editor |
| `resonance` | Plays |  |
| `resonance2` | Plays |  |
| `rt_dead` | Plays |  |
| `set_ccN` | Plays |  |
| `set_hdccN` | Plays |  |
| `set_realccN` | Plays |  |
| `sw_note_offset` | Plays |  |
| `sw_octave_offset` | Plays |  |
| `tune` | Plays |  |
| `volume` | Plays |  |
| `width` | Plays |  |

### Loading

| Opcode | Support | Note |
|---|---|---|
| `image` | Mapped | the picture travels with the instrument; the editor shows the map |
| `load_end` | Mapped | samples load in full when the instrument opens |
| `load_mode` | Mapped | samples load in full when the instrument opens |
| `load_start` | Mapped | samples load in full when the instrument opens |
| `sample_quality` | Mapped | picks the interpolation: 1-2 linear, 3-5 cubic, 6 and up band-limited |

### Sound source

| Opcode | Support | Note |
|---|---|---|
| `count` | Plays |  |
| `delay` | Plays |  |
| `delay_beats` | Plays |  |
| `delay_beats_curveccN` | Plays |  |
| `delay_beats_onccN` | Plays |  |
| `delay_beats_random` | Plays |  |
| `delay_ccN` | Plays |  |
| `delay_curveccN` | Plays |  |
| `delay_onccN` | Plays |  |
| `delay_random` | Plays |  |
| `delay_samples` | Plays |  |
| `delay_samples_onccN` | Plays |  |
| `direction` | Plays |  |
| `end` | Plays |  |
| `loop_count` | Plays |  |
| `loop_crossfade` | Plays |  |
| `loop_end` | Plays |  |
| `loop_length_onccN` | Plays |  |
| `loop_lengthccN` | Plays |  |
| `loop_mode` | Plays |  |
| `loop_start` | Plays |  |
| `loop_start_onccN` | Plays |  |
| `loop_startccN` | Plays |  |
| `loop_tune` | Plays |  |
| `loop_type` | Plays |  |
| `loopcount` | Plays |  |
| `loopend` | Plays |  |
| `loopmode` | Plays |  |
| `loopstart` | Plays |  |
| `looptune` | Plays |  |
| `looptype` | Plays |  |
| `md5` | Mapped | checked: a sample file that differs is listed in the report |
| `offset` | Plays |  |
| `offset_ccN` | Plays |  |
| `offset_mode` | Plays |  |
| `offset_onccN` | Plays |  |
| `offset_random` | Plays |  |
| `reverse_hiccN` | Plays |  |
| `reverse_loccN` | Plays |  |
| `sample` | Plays |  |
| `sample_dyn_paramN` | Reported | parameters for scripted sample generators |
| `sample_dyn_paramN_onccX` | Reported | parameters for scripted sample generators |
| `sample_fadeout` | Plays |  |
| `stop_beats` | Plays |  |
| `sync_beats` | Plays |  |
| `sync_offset` | Plays |  |
| `waveguide` | Reported | no waveguide synthesis; the region plays its sample |

### Wavetable oscillator

| Opcode | Support | Note |
|---|---|---|
| `oscillator` | Plays |  |
| `oscillator_detune` | Plays |  |
| `oscillator_detune_onccN` | Plays |  |
| `oscillator_mod_depth` | Plays |  |
| `oscillator_mod_depth_onccN` | Plays |  |
| `oscillator_mod_smoothccN` | Plays |  |
| `oscillator_mode` | Plays |  |
| `oscillator_multi` | Plays |  |
| `oscillator_phase` | Plays |  |
| `oscillator_quality` | Mapped | the band-limited reader |
| `oscillator_table_size` | Mapped | the sample is the table |

### Region logic

| Opcode | Support | Note |
|---|---|---|
| `eqNbw` | Plays |  |
| `eqNfreq` | Plays |  |
| `eqNgain` | Plays |  |
| `hibend` | Plays |  |
| `hibpm` | Plays |  |
| `hiccN` | Plays |  |
| `hichan` | Plays |  |
| `hichanaft` | Plays |  |
| `hihdccN` | Plays |  |
| `hikey` | Plays |  |
| `hipolyaft` | Plays |  |
| `hiprog` | Plays |  |
| `hirand` | Plays |  |
| `hitimer` | Plays |  |
| `hivel` | Plays |  |
| `key` | Plays |  |
| `lobend` | Plays |  |
| `lobpm` | Plays |  |
| `loccN` | Plays |  |
| `lochan` | Plays |  |
| `lochanaft` | Plays |  |
| `lohdccN` | Plays |  |
| `lokey` | Plays |  |
| `lopolyaft` | Plays |  |
| `loprog` | Plays |  |
| `lorand` | Plays |  |
| `lotimer` | Plays |  |
| `lovel` | Plays |  |
| `on_hiccN` | Plays |  |
| `on_hihdccN` | Plays |  |
| `on_loccN` | Plays |  |
| `on_lohdccN` | Plays |  |
| `sample_dyn_paramX` | Reported | parameters for scripted sample generators |
| `seq_length` | Plays |  |
| `seq_position` | Plays |  |
| `sostenuto_cc` | Plays |  |
| `sostenuto_lo` | Plays |  |
| `sostenuto_sw` | Plays |  |
| `start_hiccN` | Plays |  |
| `start_hihdccN` | Plays |  |
| `start_loccN` | Plays |  |
| `start_lohdccN` | Plays |  |
| `stop_hiccN` | Plays |  |
| `stop_hihdccN` | Plays |  |
| `stop_loccN` | Plays |  |
| `stop_lohdccN` | Plays |  |
| `sustain_cc` | Plays |  |
| `sustain_lo` | Plays |  |
| `sustain_sw` | Plays |  |
| `sw_default` | Plays |  |
| `sw_down` | Plays |  |
| `sw_hikey` | Plays |  |
| `sw_hilast` | Plays |  |
| `sw_label` | Shown | shown in the editor |
| `sw_last` | Plays |  |
| `sw_lokey` | Plays |  |
| `sw_lolast` | Plays |  |
| `sw_previous` | Plays |  |
| `sw_up` | Plays |  |
| `sw_vel` | Plays |  |
| `trigger` | Plays |  |
| `varNN_*` | Plays |  |
| `varNN_curveccX` | Plays |  |
| `varNN_mod` | Plays |  |
| `varNN_onccX` | Plays |  |

### Performance

| Opcode | Support | Note |
|---|---|---|
| `amp_keycenter` | Plays |  |
| `amp_keytrack` | Plays |  |
| `amp_random` | Plays |  |
| `amp_velcurve_N` | Plays |  |
| `amp_veltrack` | Plays |  |
| `amp_veltrack_ccN` | Plays |  |
| `amp_veltrack_curveccN` | Plays |  |
| `amp_veltrack_onccN` | Plays |  |
| `amp_veltrack_random` | Plays |  |
| `amplitude_ccN` | Plays |  |
| `amplitude_curveccN` | Plays |  |
| `amplitude_onccN` | Plays |  |
| `amplitude_smoothccN` | Plays |  |
| `bend_down` | Plays |  |
| `bend_smooth` | Plays |  |
| `bend_step` | Plays |  |
| `bend_stepdown` | Plays |  |
| `bend_stepup` | Plays |  |
| `bend_up` | Plays |  |
| `benddown` | Plays |  |
| `bendstep` | Plays |  |
| `bendup` | Plays |  |
| `cutoff2_ccN` | Plays |  |
| `cutoff2_chanaft` | Plays |  |
| `cutoff2_curveccN` | Plays |  |
| `cutoff2_onccN` | Plays |  |
| `cutoff2_polyaft` | Plays |  |
| `cutoff2_random` | Plays |  |
| `cutoff2_smoothccN` | Plays |  |
| `cutoff2_stepccN` | Plays |  |
| `cutoff_ccN` | Plays |  |
| `cutoff_chanaft` | Plays |  |
| `cutoff_curveccN` | Plays |  |
| `cutoff_onccN` | Plays |  |
| `cutoff_polyaft` | Plays |  |
| `cutoff_random` | Plays |  |
| `cutoff_smoothccN` | Plays |  |
| `cutoff_stepccN` | Plays |  |
| `eqN_bw` | Plays |  |
| `eqN_bw_onccX` | Plays |  |
| `eqN_bwccX` | Plays |  |
| `eqN_dynamic` | Plays |  |
| `eqN_freq` | Plays |  |
| `eqN_freq_onccX` | Plays |  |
| `eqN_freqccX` | Plays |  |
| `eqN_gain` | Plays |  |
| `eqN_gain_onccX` | Plays |  |
| `eqN_gainccX` | Plays |  |
| `eqN_mode` | Mapped | one EQ mode |
| `eqN_type` | Plays |  |
| `eqN_vel2freq` | Plays |  |
| `eqN_vel2gain` | Plays |  |
| `fil2_gain_onccN` | Plays |  |
| `fil2_keycenter` | Plays |  |
| `fil2_keytrack` | Plays |  |
| `fil2_mode` | Plays |  |
| `fil2_type` | Plays |  |
| `fil2_veltrack` | Plays |  |
| `fil_gain_onccN` | Plays |  |
| `fil_keycenter` | Plays |  |
| `fil_keytrack` | Plays |  |
| `fil_mode` | Plays |  |
| `fil_random` | Plays |  |
| `fil_type` | Plays |  |
| `fil_veltrack` | Plays |  |
| `filtype` | Plays |  |
| `gain_ccN` | Plays |  |
| `gain_onccN` | Plays |  |
| `gain_random` | Plays |  |
| `global_amplitude` | Plays |  |
| `global_tune` | Plays |  |
| `global_volume` | Plays |  |
| `group_amplitude` | Plays |  |
| `group_tune` | Plays |  |
| `group_volume` | Plays |  |
| `master_amplitude` | Plays |  |
| `master_tune` | Plays |  |
| `master_volume` | Plays |  |
| `noise_filter` | Plays |  |
| `noise_level` | Plays |  |
| `noise_level_onccN` | Plays |  |
| `noise_level_smoothccN` | Plays |  |
| `noise_step` | Plays |  |
| `noise_step_onccN` | Plays |  |
| `noise_stereo` | Plays |  |
| `noise_tone` | Plays |  |
| `noise_tone_onccN` | Plays |  |
| `pan_ccN` | Plays |  |
| `pan_curveccN` | Plays |  |
| `pan_keycenter` | Plays |  |
| `pan_keytrack` | Plays |  |
| `pan_law` | Plays |  |
| `pan_onccN` | Plays |  |
| `pan_random` | Plays |  |
| `pan_smoothccN` | Plays |  |
| `pan_stepccN` | Plays |  |
| `pan_veltrack` | Plays |  |
| `phase` | Plays |  |
| `pitch_curveccN` | Plays |  |
| `pitch_keycenter` | Plays |  |
| `pitch_keytrack` | Plays |  |
| `pitch_onccN` | Plays |  |
| `pitch_random` | Plays |  |
| `pitch_smoothccN` | Plays |  |
| `pitch_stepccN` | Plays |  |
| `pitch_veltrack` | Plays |  |
| `position_curveccN` | Plays |  |
| `position_keycenter` | Plays |  |
| `position_keytrack` | Plays |  |
| `position_onccN` | Plays |  |
| `position_random` | Plays |  |
| `position_smoothccN` | Plays |  |
| `position_stepccN` | Plays |  |
| `position_veltrack` | Plays |  |
| `resonance2_ccN` | Plays |  |
| `resonance2_curveccN` | Plays |  |
| `resonance2_onccN` | Plays |  |
| `resonance2_random` | Plays |  |
| `resonance2_smoothccN` | Plays |  |
| `resonance2_stepccN` | Plays |  |
| `resonance_ccN` | Plays |  |
| `resonance_curveccN` | Plays |  |
| `resonance_onccN` | Plays |  |
| `resonance_random` | Plays |  |
| `resonance_smoothccN` | Plays |  |
| `resonance_stepccN` | Plays |  |
| `rt_decay` | Plays |  |
| `rt_decayN` | Plays |  |
| `rt_decayN_time` | Plays |  |
| `transpose` | Plays |  |
| `tune_ccN` | Plays |  |
| `tune_curveccN` | Plays |  |
| `tune_keytrack` | Plays |  |
| `tune_onccN` | Plays |  |
| `tune_random` | Plays |  |
| `tune_smoothccN` | Plays |  |
| `tune_stepccN` | Plays |  |
| `tune_veltrack` | Plays |  |
| `volume_curveccN` | Plays |  |
| `volume_onccN` | Plays |  |
| `volume_smoothccN` | Plays |  |
| `volume_stepccN` | Plays |  |
| `width_curveccN` | Plays |  |
| `width_onccN` | Plays |  |
| `width_smoothccN` | Plays |  |
| `width_stepccN` | Plays |  |
| `xf_cccurve` | Plays |  |
| `xf_keycurve` | Plays |  |
| `xf_velcurve` | Plays |  |
| `xfin_hiccN` | Plays |  |
| `xfin_hikey` | Plays |  |
| `xfin_hivel` | Plays |  |
| `xfin_loccN` | Plays |  |
| `xfin_lokey` | Plays |  |
| `xfin_lovel` | Plays |  |
| `xfout_hiccN` | Plays |  |
| `xfout_hikey` | Plays |  |
| `xfout_hivel` | Plays |  |
| `xfout_loccN` | Plays |  |
| `xfout_lokey` | Plays |  |
| `xfout_lovel` | Plays |  |

### Modulation

| Opcode | Support | Note |
|---|---|---|
| `amp_attack` | Plays |  |
| `amp_decay` | Plays |  |
| `amp_delay` | Plays |  |
| `amp_hold` | Plays |  |
| `amp_release` | Plays |  |
| `amp_sustain` | Plays |  |
| `amp_vel2attack` | Plays |  |
| `amp_vel2decay` | Plays |  |
| `amp_vel2delay` | Plays |  |
| `amp_vel2hold` | Plays |  |
| `amp_vel2release` | Plays |  |
| `amp_vel2sustain` | Plays |  |
| `ampeg_attack` | Plays |  |
| `ampeg_attack_curveccN` | Plays |  |
| `ampeg_attack_onccN` | Plays |  |
| `ampeg_attack_shape` | Plays |  |
| `ampeg_attackccN` | Plays |  |
| `ampeg_decay` | Plays |  |
| `ampeg_decay_curveccN` | Plays |  |
| `ampeg_decay_onccN` | Plays |  |
| `ampeg_decay_shape` | Plays |  |
| `ampeg_decay_zero` | Plays |  |
| `ampeg_decayccN` | Plays |  |
| `ampeg_delay` | Plays |  |
| `ampeg_delay_curveccN` | Plays |  |
| `ampeg_delay_onccN` | Plays |  |
| `ampeg_delayccN` | Plays |  |
| `ampeg_dynamic` | Plays |  |
| `ampeg_hold` | Plays |  |
| `ampeg_hold_curveccN` | Plays |  |
| `ampeg_hold_onccN` | Plays |  |
| `ampeg_holdccN` | Plays |  |
| `ampeg_release` | Plays |  |
| `ampeg_release_curveccN` | Plays |  |
| `ampeg_release_onccN` | Plays |  |
| `ampeg_release_shape` | Plays |  |
| `ampeg_release_zero` | Plays |  |
| `ampeg_releaseccN` | Plays |  |
| `ampeg_start` | Plays |  |
| `ampeg_start_curveccN` | Plays |  |
| `ampeg_start_onccN` | Plays |  |
| `ampeg_startccN` | Plays |  |
| `ampeg_sustain` | Plays |  |
| `ampeg_sustain_curveccN` | Plays |  |
| `ampeg_sustain_onccN` | Plays |  |
| `ampeg_sustainccN` | Plays |  |
| `ampeg_vel2attack` | Plays |  |
| `ampeg_vel2decay` | Plays |  |
| `ampeg_vel2delay` | Plays |  |
| `ampeg_vel2hold` | Plays |  |
| `ampeg_vel2release` | Plays |  |
| `ampeg_vel2sustain` | Plays |  |
| `amplfo_delay` | Plays |  |
| `amplfo_depth` | Plays |  |
| `amplfo_depth_onccN` | Plays |  |
| `amplfo_depthccN` | Plays |  |
| `amplfo_depthchanaft` | Plays |  |
| `amplfo_depthpolyaft` | Plays |  |
| `amplfo_fade` | Plays |  |
| `amplfo_freq` | Plays |  |
| `amplfo_freqccN` | Plays |  |
| `amplfo_freqchanaft` | Plays |  |
| `amplfo_freqpolyaft` | Plays |  |
| `egN_ampeg` | Plays |  |
| `egN_amplitude` | Plays |  |
| `egN_amplitude_onccX` | Plays |  |
| `egN_bitred` | Plays |  |
| `egN_bitred_onccX` | Plays |  |
| `egN_curveX` | Plays |  |
| `egN_cutoff` | Plays |  |
| `egN_cutoff2` | Plays |  |
| `egN_cutoff2_onccX` | Plays |  |
| `egN_cutoff_onccX` | Plays |  |
| `egN_decim` | Plays |  |
| `egN_decim_onccX` | Plays |  |
| `egN_depth_lfoX` | Plays |  |
| `egN_depthadd_lfoX` | Plays |  |
| `egN_driveshape` | Plays |  |
| `egN_driveshape_onccX` | Plays |  |
| `egN_dynamic` | Plays |  |
| `egN_eqXbw` | Plays |  |
| `egN_eqXbw_onccY` | Plays |  |
| `egN_eqXfreq` | Plays |  |
| `egN_eqXfreq_onccY` | Plays |  |
| `egN_eqXgain` | Plays |  |
| `egN_eqXgain_onccY` | Plays |  |
| `egN_freq_lfoX` | Plays |  |
| `egN_levelX` | Plays |  |
| `egN_levelX_onccY` | Plays |  |
| `egN_loop` | Plays |  |
| `egN_loop_count` | Plays |  |
| `egN_noiselevel` | Plays |  |
| `egN_noiselevel_onccX` | Plays |  |
| `egN_noisestep` | Plays |  |
| `egN_noisestep_onccX` | Plays |  |
| `egN_noisetone` | Plays |  |
| `egN_noisetone_onccX` | Plays |  |
| `egN_pan` | Plays |  |
| `egN_pan_curve` | Mapped | the region pan law |
| `egN_pan_curveccX` | Mapped | the region pan law |
| `egN_pan_onccX` | Plays |  |
| `egN_pitch` | Plays |  |
| `egN_pitch_onccX` | Plays |  |
| `egN_points` | Plays |  |
| `egN_rectify` | Plays |  |
| `egN_rectify_onccX` | Plays |  |
| `egN_resonance` | Plays |  |
| `egN_resonance2` | Plays |  |
| `egN_resonance2_onccX` | Plays |  |
| `egN_resonance_onccX` | Plays |  |
| `egN_ringmod` | Plays |  |
| `egN_ringmod_onccX` | Plays |  |
| `egN_sample_dyn_paramX` | Reported | parameters for scripted sample generators |
| `egN_sample_dyn_paramX_onccY` | Reported | parameters for scripted sample generators |
| `egN_shapeX` | Plays |  |
| `egN_sustain` | Plays |  |
| `egN_timeX` | Plays |  |
| `egN_timeX_onccY` | Plays |  |
| `egN_volume` | Plays |  |
| `egN_volume_onccX` | Plays |  |
| `egN_width` | Plays |  |
| `egN_width_onccX` | Plays |  |
| `fil_attack` | Plays |  |
| `fil_decay` | Plays |  |
| `fil_delay` | Plays |  |
| `fil_depth` | Plays |  |
| `fil_hold` | Plays |  |
| `fil_release` | Plays |  |
| `fil_sustain` | Plays |  |
| `fil_vel2attack` | Plays |  |
| `fil_vel2decay` | Plays |  |
| `fil_vel2delay` | Plays |  |
| `fil_vel2depth` | Plays |  |
| `fil_vel2hold` | Plays |  |
| `fil_vel2release` | Plays |  |
| `fil_vel2sustain` | Plays |  |
| `fileg_attack` | Plays |  |
| `fileg_attack_curveccN` | Plays |  |
| `fileg_attack_onccN` | Plays |  |
| `fileg_attack_shape` | Plays |  |
| `fileg_attackccN` | Plays |  |
| `fileg_decay` | Plays |  |
| `fileg_decay_curveccN` | Plays |  |
| `fileg_decay_onccN` | Plays |  |
| `fileg_decay_shape` | Plays |  |
| `fileg_decay_zero` | Plays |  |
| `fileg_decayccN` | Plays |  |
| `fileg_delay` | Plays |  |
| `fileg_delay_curveccN` | Plays |  |
| `fileg_delay_onccN` | Plays |  |
| `fileg_delayccN` | Plays |  |
| `fileg_depth` | Plays |  |
| `fileg_depth_curveccN` | Plays |  |
| `fileg_depth_onccN` | Plays |  |
| `fileg_depthccN` | Plays |  |
| `fileg_dynamic` | Plays |  |
| `fileg_hold` | Plays |  |
| `fileg_hold_curveccN` | Plays |  |
| `fileg_hold_onccN` | Plays |  |
| `fileg_holdccN` | Plays |  |
| `fileg_release` | Plays |  |
| `fileg_release_curveccN` | Plays |  |
| `fileg_release_onccN` | Plays |  |
| `fileg_release_shape` | Plays |  |
| `fileg_release_zero` | Plays |  |
| `fileg_releaseccN` | Plays |  |
| `fileg_start` | Plays |  |
| `fileg_start_curveccN` | Plays |  |
| `fileg_start_onccN` | Plays |  |
| `fileg_startccN` | Plays |  |
| `fileg_sustain` | Plays |  |
| `fileg_sustain_curveccN` | Plays |  |
| `fileg_sustain_onccN` | Plays |  |
| `fileg_sustainccN` | Plays |  |
| `fileg_vel2attack` | Plays |  |
| `fileg_vel2decay` | Plays |  |
| `fileg_vel2delay` | Plays |  |
| `fileg_vel2depth` | Plays |  |
| `fileg_vel2hold` | Plays |  |
| `fileg_vel2release` | Plays |  |
| `fileg_vel2sustain` | Plays |  |
| `fillfo_delay` | Plays |  |
| `fillfo_depth` | Plays |  |
| `fillfo_depth_onccN` | Plays |  |
| `fillfo_depthccN` | Plays |  |
| `fillfo_depthchanaft` | Plays |  |
| `fillfo_depthpolyaft` | Plays |  |
| `fillfo_fade` | Plays |  |
| `fillfo_freq` | Plays |  |
| `fillfo_freqccN` | Plays |  |
| `fillfo_freqchanaft` | Plays |  |
| `fillfo_freqpolyaft` | Plays |  |
| `lfoN_amplitude` | Plays |  |
| `lfoN_amplitude_onccX` | Plays |  |
| `lfoN_amplitude_smoothccX` | Plays |  |
| `lfoN_amplitude_stepccX` | Plays |  |
| `lfoN_bitred` | Plays |  |
| `lfoN_bitred_onccX` | Plays |  |
| `lfoN_bitred_smoothccX` | Plays |  |
| `lfoN_bitred_stepccX` | Plays |  |
| `lfoN_count` | Plays |  |
| `lfoN_cutoff` | Plays |  |
| `lfoN_cutoff2` | Plays |  |
| `lfoN_cutoff2_onccX` | Plays |  |
| `lfoN_cutoff2_smoothccX` | Plays |  |
| `lfoN_cutoff2_stepccX` | Plays |  |
| `lfoN_cutoff_onccX` | Plays |  |
| `lfoN_cutoff_smoothccX` | Plays |  |
| `lfoN_cutoff_stepccX` | Plays |  |
| `lfoN_decim` | Plays |  |
| `lfoN_decim_onccX` | Plays |  |
| `lfoN_decim_smoothccX` | Plays |  |
| `lfoN_decim_stepccX` | Plays |  |
| `lfoN_delay` | Plays |  |
| `lfoN_delay_onccX` | Plays |  |
| `lfoN_depth_lfoX` | Plays |  |
| `lfoN_depthadd_lfoX` | Plays |  |
| `lfoN_drive` | Plays |  |
| `lfoN_drive_onccX` | Plays |  |
| `lfoN_drive_smoothccX` | Plays |  |
| `lfoN_drive_stepccX` | Plays |  |
| `lfoN_eqXbw` | Plays |  |
| `lfoN_eqXbw_onccY` | Plays |  |
| `lfoN_eqXbw_smoothccY` | Plays |  |
| `lfoN_eqXbw_stepccY` | Plays |  |
| `lfoN_eqXfreq` | Plays |  |
| `lfoN_eqXfreq_onccY` | Plays |  |
| `lfoN_eqXfreq_smoothccY` | Plays |  |
| `lfoN_eqXfreq_stepccY` | Plays |  |
| `lfoN_eqXgain` | Plays |  |
| `lfoN_eqXgain_onccY` | Plays |  |
| `lfoN_eqXgain_smoothccY` | Plays |  |
| `lfoN_eqXgain_stepccY` | Plays |  |
| `lfoN_fade` | Plays |  |
| `lfoN_fade_onccX` | Plays |  |
| `lfoN_freq` | Plays |  |
| `lfoN_freq_lfoX` | Plays |  |
| `lfoN_freq_lfoX_onccY` | Plays |  |
| `lfoN_freq_onccX` | Plays |  |
| `lfoN_freq_smoothccX` | Plays |  |
| `lfoN_freq_stepccX` | Plays |  |
| `lfoN_noiselevel` | Plays |  |
| `lfoN_noiselevel_onccX` | Plays |  |
| `lfoN_noiselevel_smoothccX` | Plays |  |
| `lfoN_noiselevel_stepccX` | Plays |  |
| `lfoN_noisestep` | Plays |  |
| `lfoN_noisestep_onccX` | Plays |  |
| `lfoN_noisestep_smoothccX` | Plays |  |
| `lfoN_noisestep_stepccX` | Plays |  |
| `lfoN_noisetone` | Plays |  |
| `lfoN_noisetone_onccX` | Plays |  |
| `lfoN_noisetone_smoothccX` | Plays |  |
| `lfoN_noisetone_stepccX` | Plays |  |
| `lfoN_offset` | Plays |  |
| `lfoN_offsetX` | Plays |  |
| `lfoN_pan` | Plays |  |
| `lfoN_pan_onccX` | Plays |  |
| `lfoN_pan_smoothccX` | Plays |  |
| `lfoN_pan_stepccX` | Plays |  |
| `lfoN_phase` | Plays |  |
| `lfoN_phase_onccX` | Plays |  |
| `lfoN_pitch` | Plays |  |
| `lfoN_pitch_curveccX` | Plays |  |
| `lfoN_pitch_onccX` | Plays |  |
| `lfoN_pitch_smoothccX` | Plays |  |
| `lfoN_pitch_stepccX` | Plays |  |
| `lfoN_ratio` | Plays |  |
| `lfoN_ratioX` | Plays |  |
| `lfoN_resonance` | Plays |  |
| `lfoN_resonance2` | Plays |  |
| `lfoN_resonance2_onccX` | Plays |  |
| `lfoN_resonance2_smoothccX` | Plays |  |
| `lfoN_resonance2_stepccX` | Plays |  |
| `lfoN_resonance_onccX` | Plays |  |
| `lfoN_resonance_smoothccX` | Plays |  |
| `lfoN_resonance_stepccX` | Plays |  |
| `lfoN_sample_dyn_paramX` | Reported | parameters for scripted sample generators |
| `lfoN_sample_dyn_paramX_onccY` | Reported | parameters for scripted sample generators |
| `lfoN_scale` | Plays |  |
| `lfoN_scaleX` | Plays |  |
| `lfoN_smooth` | Plays |  |
| `lfoN_smooth_onccX` | Plays |  |
| `lfoN_steps` | Plays |  |
| `lfoN_stepX` | Plays |  |
| `lfoN_stepX_onccY` | Plays |  |
| `lfoN_volume` | Plays |  |
| `lfoN_volume_onccX` | Plays |  |
| `lfoN_volume_smoothccX` | Plays |  |
| `lfoN_volume_stepccX` | Plays |  |
| `lfoN_wave` | Plays |  |
| `lfoN_wave_onccX` | Plays |  |
| `lfoN_waveX` | Plays |  |
| `lfoN_width` | Plays |  |
| `lfoN_width_onccX` | Plays |  |
| `lfoN_width_smoothccX` | Plays |  |
| `lfoN_width_stepccX` | Plays |  |
| `pitch_attack` | Plays |  |
| `pitch_decay` | Plays |  |
| `pitch_delay` | Plays |  |
| `pitch_depth` | Plays |  |
| `pitch_hold` | Plays |  |
| `pitch_release` | Plays |  |
| `pitch_sustain` | Plays |  |
| `pitch_vel2attack` | Plays |  |
| `pitch_vel2decay` | Plays |  |
| `pitch_vel2delay` | Plays |  |
| `pitch_vel2depth` | Plays |  |
| `pitch_vel2hold` | Plays |  |
| `pitch_vel2release` | Plays |  |
| `pitch_vel2sustain` | Plays |  |
| `pitcheg_attack` | Plays |  |
| `pitcheg_attack_curveccN` | Plays |  |
| `pitcheg_attack_onccN` | Plays |  |
| `pitcheg_attack_shape` | Plays |  |
| `pitcheg_decay` | Plays |  |
| `pitcheg_decay_curveccN` | Plays |  |
| `pitcheg_decay_onccN` | Plays |  |
| `pitcheg_decay_shape` | Plays |  |
| `pitcheg_decay_zero` | Plays |  |
| `pitcheg_delay` | Plays |  |
| `pitcheg_delay_curveccN` | Plays |  |
| `pitcheg_delay_onccN` | Plays |  |
| `pitcheg_depth` | Plays |  |
| `pitcheg_depth_curveccN` | Plays |  |
| `pitcheg_depth_onccN` | Plays |  |
| `pitcheg_depthccN` | Plays |  |
| `pitcheg_dynamic` | Plays |  |
| `pitcheg_hold` | Plays |  |
| `pitcheg_hold_curveccN` | Plays |  |
| `pitcheg_hold_onccN` | Plays |  |
| `pitcheg_release` | Plays |  |
| `pitcheg_release_curveccN` | Plays |  |
| `pitcheg_release_onccN` | Plays |  |
| `pitcheg_release_shape` | Plays |  |
| `pitcheg_release_zero` | Plays |  |
| `pitcheg_start` | Plays |  |
| `pitcheg_start_curveccN` | Plays |  |
| `pitcheg_start_onccN` | Plays |  |
| `pitcheg_sustain` | Plays |  |
| `pitcheg_sustain_curveccN` | Plays |  |
| `pitcheg_sustain_onccN` | Plays |  |
| `pitcheg_vel2attack` | Plays |  |
| `pitcheg_vel2decay` | Plays |  |
| `pitcheg_vel2delay` | Plays |  |
| `pitcheg_vel2depth` | Plays |  |
| `pitcheg_vel2hold` | Plays |  |
| `pitcheg_vel2release` | Plays |  |
| `pitcheg_vel2sustain` | Plays |  |
| `pitchlfo_delay` | Plays |  |
| `pitchlfo_depth` | Plays |  |
| `pitchlfo_depth_onccN` | Plays |  |
| `pitchlfo_depthccN` | Plays |  |
| `pitchlfo_depthchanaft` | Plays |  |
| `pitchlfo_depthpolyaft` | Plays |  |
| `pitchlfo_fade` | Plays |  |
| `pitchlfo_freq` | Plays |  |
| `pitchlfo_freqccN` | Plays |  |
| `pitchlfo_freqchanaft` | Plays |  |
| `pitchlfo_freqpolyaft` | Plays |  |

### Curves

| Opcode | Support | Note |
|---|---|---|
| `curve_index` | Plays |  |
| `vNNN` | Plays |  |

### Effects

| Opcode | Support | Note |
|---|---|---|
| `apan_depth` | Plays |  |
| `apan_depth_onccN` | Plays |  |
| `apan_dry` | Plays |  |
| `apan_dry_onccN` | Plays |  |
| `apan_freq` | Plays |  |
| `apan_freq_onccN` | Plays |  |
| `apan_phase` | Plays |  |
| `apan_phase_onccN` | Plays |  |
| `apan_waveform` | Plays |  |
| `apan_wet` | Plays |  |
| `apan_wet_onccN` | Plays |  |
| `bitred_curveccN` | Plays |  |
| `bitred_onccN` | Plays |  |
| `bitred_smoothccN` | Plays |  |
| `bitred_stepccN` | Plays |  |
| `bus` | Plays |  |
| `bypass_onccN` | Plays |  |
| `comp_attack` | Plays |  |
| `comp_gain` | Plays |  |
| `comp_ratio` | Plays |  |
| `comp_release` | Plays |  |
| `comp_stlink` | Plays |  |
| `comp_threshold` | Plays |  |
| `decim_curveccN` | Plays |  |
| `decim_onccN` | Plays |  |
| `decim_smoothccN` | Plays |  |
| `decim_stepccN` | Plays |  |
| `delay_cutoff` | Plays |  |
| `delay_cutoff_onccN` | Plays |  |
| `delay_damphi` | Plays |  |
| `delay_damphi_onccN` | Plays |  |
| `delay_damplo` | Plays |  |
| `delay_damplo_onccN` | Plays |  |
| `delay_dry` | Plays |  |
| `delay_dry_onccN` | Plays |  |
| `delay_feedback` | Plays |  |
| `delay_feedback_onccN` | Plays |  |
| `delay_filter` | Plays |  |
| `delay_input` | Plays |  |
| `delay_input_onccN` | Plays |  |
| `delay_levelc` | Plays |  |
| `delay_levell` | Plays |  |
| `delay_levelr` | Plays |  |
| `delay_lfofreq` | Plays |  |
| `delay_lfofreq_onccN` | Plays |  |
| `delay_moddepth` | Plays |  |
| `delay_moddepth_onccN` | Plays |  |
| `delay_mode` | Plays |  |
| `delay_panc` | Plays |  |
| `delay_panc_onccN` | Plays |  |
| `delay_panl` | Plays |  |
| `delay_panl_onccN` | Plays |  |
| `delay_panr` | Plays |  |
| `delay_panr_onccN` | Plays |  |
| `delay_resonance` | Plays |  |
| `delay_resonance_onccN` | Plays |  |
| `delay_spread` | Plays |  |
| `delay_spread_onccN` | Plays |  |
| `delay_syncc_onccN` | Plays |  |
| `delay_syncl_onccN` | Plays |  |
| `delay_syncr_onccN` | Plays |  |
| `delay_time_tap` | Mapped | the delay time in seconds |
| `delay_timec` | Plays |  |
| `delay_timec_onccN` | Plays |  |
| `delay_timel` | Plays |  |
| `delay_timel_onccN` | Plays |  |
| `delay_timer` | Plays |  |
| `delay_timer_onccN` | Plays |  |
| `delay_wet` | Plays |  |
| `delay_wet_onccN` | Plays |  |
| `directtomain` | Plays |  |
| `disto_depth` | Plays |  |
| `disto_depth_onccN` | Plays |  |
| `disto_dry` | Plays |  |
| `disto_dry_onccN` | Plays |  |
| `disto_stages` | Plays |  |
| `disto_tone` | Plays |  |
| `disto_tone_onccN` | Plays |  |
| `disto_wet` | Plays |  |
| `disto_wet_onccN` | Plays |  |
| `dsp_order` | Mapped | effects run in the order written |
| `effect1` | Plays |  |
| `effect2` | Plays |  |
| `effect3` | Plays |  |
| `effect4` | Plays |  |
| `eq_bw` | Plays |  |
| `eq_bw_onccN` | Plays |  |
| `eq_freq` | Plays |  |
| `eq_freq_onccN` | Plays |  |
| `eq_gain` | Plays |  |
| `eq_gain_onccN` | Plays |  |
| `eq_type` | Plays |  |
| `filter_cutoff` | Plays |  |
| `filter_cutoff_onccN` | Plays |  |
| `filter_resonance` | Plays |  |
| `filter_resonance_onccN` | Plays |  |
| `filter_type` | Plays |  |
| `fxNtomain` | Plays |  |
| `fxNtomix` | Plays |  |
| `gate_attack` | Plays |  |
| `gate_onccN` | Plays |  |
| `gate_release` | Plays |  |
| `gate_stlink` | Plays |  |
| `gate_threshold` | Plays |  |
| `internal` | Mapped | a switch for one vendor's own features; it stays off |
| `param_offset` | Mapped | effect parameters are read by name |
| `phaser_depth` | Plays |  |
| `phaser_depth_onccN` | Plays |  |
| `phaser_feedback` | Plays |  |
| `phaser_feedback_onccN` | Plays |  |
| `phaser_freq` | Plays |  |
| `phaser_freq_onccN` | Plays |  |
| `phaser_phase_onccN` | Plays |  |
| `phaser_stages` | Plays |  |
| `phaser_waveform` | Plays |  |
| `phaser_wet` | Plays |  |
| `phaser_wet_onccN` | Plays |  |
| `reverb_damp` | Plays |  |
| `reverb_damp_onccN` | Plays |  |
| `reverb_dry` | Plays |  |
| `reverb_dry_onccN` | Plays |  |
| `reverb_input` | Plays |  |
| `reverb_input_onccN` | Plays |  |
| `reverb_predelay` | Plays |  |
| `reverb_predelay_onccN` | Plays |  |
| `reverb_size` | Plays |  |
| `reverb_size_onccN` | Plays |  |
| `reverb_tone` | Plays |  |
| `reverb_tone_onccN` | Plays |  |
| `reverb_type` | Mapped | one room shape for every type |
| `reverb_wet` | Plays |  |
| `reverb_wet_onccN` | Plays |  |
| `static_cyclic_level` | Plays |  |
| `static_cyclic_time` | Plays |  |
| `static_filter` | Plays |  |
| `static_level` | Plays |  |
| `static_level_onccN` | Plays |  |
| `static_random_level` | Plays |  |
| `static_random_maxtime` | Plays |  |
| `static_random_mintime` | Plays |  |
| `static_stereo` | Plays |  |
| `static_tone` | Plays |  |
| `strings_number` | Plays |  |
| `strings_wet_onccN` | Plays |  |
| `tdfir_dry` | Plays |  |
| `tdfir_dry_onccN` | Plays |  |
| `tdfir_gain` | Plays |  |
| `tdfir_impulse` | Plays |  |
| `tdfir_wet` | Plays |  |
| `tdfir_wet_onccN` | Plays |  |
| `type` | Plays |  |
| `vendor_specific` | Reported | player-specific settings |
