// Gridline — plugin brand system (see PluginBrand.h).
#include "PluginBrand.h"
#include <cctype>
#include "PluginInfo.h"

namespace gl {

namespace {
using F = BrandFamily;
}

const std::vector<PluginBrand>& pluginBrands() {
  static const std::vector<PluginBrand> v = {
      // ---- flagships: sound sources
      {"gl.synth", F::Synth, "Three oscillators, one clean line.", "trio synth, 3 oscillator, subtractive, basic synth"},
      {"gl.matrixfm", F::Synth, "Six FM operators wired through one modulation grid.", "matrix fm, fm synth, operator, frequency modulation"},
      {"gl.wavetable", F::Synth, "Wavetables scanned like contour lines.", "wavetable, wavetable synth, morph"},
      {"gl.additive", F::Synth, "Additive synthesis, one harmonic facet at a time.", "additive, additive synth, harmonics, partials"},
      {"gl.harmor", F::Synth, "Resynthesis that prints sound as fields of partials.", "resynth, resynthesis, additive, image synth, partials"},
      {"gl.plasma", F::Synth, "Wavetables, grains and spectra, routed through a deep matrix.",
       "plasma, wavetable, wavetable synth, hybrid synth, granular, spectral, supersaw, bass, preset import"},
      // ---- flagships: samples and beats
      {"gl.pads", F::Sample, "Sixteen pads, counted out on a grid.", "drum pads, drum machine, pad sampler, kit"},
      {"gl.slicer", F::Sample, "Cuts loops into slices along the beat.", "slicer, beat slicer, loop slicer, chop"},
      {"gl.waveprogram", F::Sample, "Zones, layers and loops bound into one instrument.", "wave program, sampler, multisampler, zones, keymap"},
      {"gl.edison", F::Sample, "The close-up audio editor: waveform, spectrum, markers.", "audio editor, wave editor, recorder, sample editor"},
      {"gl.seltmann", F::Sample, "Finds the beat in any song and chops it into loops.",
       "chop, chopper, sample chop, flip, loop maker, microsampling, beat slicer, bpm, tempo detection, sampler, phrase"},
      {"gl.platter", F::Sample, "One deck of a DJ setup: scratch, cue and mix any track in time.",
       "scratch, turntable, vinyl, dj, deck, ddj, controller, jog, cue, beatmatch, sync"},
      // ---- flagships: time and space
      {"gl.delay", F::Space, "Echoes that trace the beat, from tape to digital.", "delay, echo, ping pong, tape delay"},
      {"gl.hall", F::Space, "Long algorithmic halls with a quiet floor.", "hall, reverb, hall reverb, space"},
      {"gl.timegate", F::Space, "Redraws time and volume on the grid.", "time gate, stutter, gater, tape stop, volume shaper, glitch"},
      // ---- flagships: dynamics
      {"gl.master", F::Dynamics, "Multiband loudness for the master.", "mastering dynamics, maximizer, multiband, mastering, loudness"},
      {"gl.tideline", F::Dynamics, "Pumps the mix on the beat, no sidechain needed.",
       "sidechain, pump, pumping, ducking, ducker, duck, volume pump, kickstart"},
      // ---- flagships: voice
      {"gl.vocoder", F::Voice, "Speaks any sound through a voice.", "vocoder, talk box, robot voice, carrier"},
      {"gl.pitcher", F::Voice, "Pulls every note true to the scale.", "pitch corrector, pitch correction, tuning, vocal, scale"},
      // ---- flagships: structure
      {"gl.patcher", F::Structure, "Patch plugins together into one instrument.", "patch host, modular, rack, routing, graph"},
      {"gl.patcherfx", F::Structure, "Patch plugins together into one effect.", "patch host fx, modular, rack, routing, graph"},
      {"gl.scope", F::Structure, "Scope, spectrum and phase on one measured screen.", "scope, oscilloscope, visualiser, visualizer, spectrum, vectorscope"},
      // ---- renamed tools (descriptive names; former names stay searchable unless
      // they were also the name of an FL Studio plugin: clean room)
      {"gl.analog", F::None, "", "analog, subtractive, virtual analog"},
      {"gl.gms", F::None, "", "groove synth, wavetable, hybrid"},
      {"gl.plucker", F::None, "", "plucker, pluck, karplus, string"},
      {"gl.envctl", F::None, "", "envelope, adsr, modulation, controller"},
      {"gl.keyctl", F::None, "", "keyboard, key tracking, modulation, controller"},
      {"gl.formulactl", F::None, "", "formula, expression, modulation, controller"},
      {"gl.surface", F::None, "", "panel, knobs, sliders, controls, controller"},
      {"gl.clipper", F::None, "", "clip, soft clip, saturation"},
      {"gl.stereo", F::None, "", "width, stereo, imager, widener"},
      {"gl.lowlift", F::None, "", "bass, low end, sub, lift"},
      {"gl.scratcher", F::None, "", "scratch, vinyl, dj"},
      {"gl.spread", F::None, "", "spread, stereo, width, unison"},
      {"gl.hitloop", F::None, "", "hit looper, repeat, stutter, loop"},
      {"gl.amprack", F::None, "", "amp rack, amp, guitar, pedals, distortion"},
      {"gl.squeeze", F::None, "", "distortion, waveshaping, crush"},
      {"gl.pocketrack", F::None, "", "pocket rack, multi fx, effect rack"},
      {"gl.keymap", F::None, "", "remap, key map, transpose, scale"},
      {"gl.split", F::None, "", "key splitter, split, keyboard split, zones"},
      {"gl.velscale", F::None, "", "velocity, scale, level"},
      {"gl.freefilter", F::None, "", "biquad, filter, eq"},
      {"gl.splinescratch", F::None, "", "spline, scratch, turntable, sampler"},
      // ---- everyday tools: extra search words
      {"gl.reverb", F::None, "", "reverb, room, plate, space"},
      {"gl.eq", F::None, "", "parametric eq, equalizer, equaliser"},
      {"gl.pitch", F::None, "", "pitch shifter, transpose, harmonizer"},
      // ---- Gridline Suite Mini (J-132): function words for the picker search
      {"gl.minitrim", F::None, "", "gain, volume, level, polarity, phase invert, mini"},
      {"gl.minibreadth", F::None, "", "stereo width, mono, mid side, pan, bass mono, mini"},
      {"gl.miniskew", F::None, "", "stereo delay, precedence, widener, short delay, mini"},
      {"gl.miniweave", F::None, "", "channel matrix, swap channels, mono sum, left right, mini"},
      {"gl.minibands", F::None, "", "three band eq, tone, low mid high, crossover, mini"},
      {"gl.minisieve", F::None, "", "filter, low pass, high pass, band pass, notch, shelf, mini"},
      {"gl.minirungs", F::None, "", "ladder filter, resonant low pass, analog filter, mini"},
      {"gl.miniscorch", F::None, "", "driven filter, distorted filter, saturating filter, mini"},
      {"gl.minivowel", F::None, "", "vowel filter, formant, talk, mouth, mini"},
      {"gl.minitines", F::None, "", "comb, tuned delay, metallic, resonance, mini"},
      {"gl.minichime", F::None, "", "resonator, tuned resonance, bell, harmonics, mini"},
      {"gl.minipress", F::None, "", "compressor, dynamics, glue, sidechain, mini"},
      {"gl.miniceiling", F::None, "", "limiter, brickwall, loudness, peak control, mini"},
      {"gl.minishear", F::None, "", "clipper, hard clip, soft clip, peaks, mini"},
      {"gl.minipack", F::None, "", "leveller, leveler, auto gain, level rider, mini"},
      {"gl.minispan", F::None, "", "upward compression, expander, two threshold dynamics, mini"},
      {"gl.minilatch", F::None, "", "gate, noise gate, ducker, sidechain, mini"},
      {"gl.minipunch", F::None, "", "transient, attack, sustain, drum shaping, mini"},
      {"gl.miniburn", F::None, "", "distortion, saturation, overdrive, tube, fold, mini"},
      {"gl.minibend", F::None, "", "waveshaper, shaper, transfer curve, fold, mini"},
      {"gl.miniwarp", F::None, "", "phase distortion, self modulation, grit, mini"},
      {"gl.minicrumble", F::None, "", "bit crusher, sample rate reduction, lo fi, mini"},
      {"gl.miniclang", F::None, "", "ring modulator, am, metallic, robot, mini"},
      {"gl.minislide", F::None, "", "frequency shifter, inharmonic, barber pole, mini"},
      {"gl.miniinterval", F::None, "", "pitch shifter, octave, transpose, grain, mini"},
      {"gl.minidouble", F::None, "", "chorus, doubler, vibrato, width, mini"},
      {"gl.minicrowd", F::None, "", "unison, ensemble, thickener, detune, mini"},
      {"gl.minijet", F::None, "", "flanger, jet, comb sweep, mini"},
      {"gl.miniorbit", F::None, "", "phaser, allpass sweep, swirl, mini"},
      {"gl.miniecho", F::None, "", "delay, echo, ping pong, tempo delay, mini"},
      {"gl.minivolley", F::None, "", "dual delay, stereo delay, cross feedback, mini"},
      {"gl.minivault", F::None, "", "reverb, room, hall, space, mini"},
      {"gl.minirewind", F::None, "", "reverse, backwards, reverser, mini"},
      {"gl.minihalt", F::None, "", "tape stop, slow down, spin up, brake, mini"},
      {"gl.minipulse", F::None, "", "gate sequencer, volume gate, chopper, stutter, mini"},
  };
  return v;
}

const PluginBrand* pluginBrand(const std::string& type) {
  for (const auto& b : pluginBrands())
    if (type == b.type) return &b;
  return nullptr;
}

bool isFlagship(const std::string& type) {
  const PluginBrand* b = pluginBrand(type);
  return b && b->family != BrandFamily::None;
}

std::string pluginKeywords(const std::string& type) {
  const PluginBrand* b = pluginBrand(type);
  return b ? b->keywords : "";
}

std::string pluginTagline(const std::string& type) {
  const PluginBrand* b = pluginBrand(type);
  return b ? b->tagline : "";
}

std::string pluginDisplayName(const std::string& type) {
  const PluginInfo* info = findPlugin(type);
  return info ? info->name : type;
}

std::string pluginTitle(const std::string& type) {
  std::string s = pluginDisplayName(type);
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace gl
