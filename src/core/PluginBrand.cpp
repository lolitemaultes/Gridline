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
      // ---- flagships: samples and beats
      {"gl.pads", F::Sample, "Sixteen pads, counted out on a grid.", "drum pads, drum machine, pad sampler, kit"},
      {"gl.slicer", F::Sample, "Cuts loops into slices along the beat.", "slicer, beat slicer, loop slicer, chop"},
      {"gl.waveprogram", F::Sample, "Zones, layers and loops bound into one instrument.", "wave program, sampler, multisampler, zones, keymap"},
      {"gl.edison", F::Sample, "The close-up audio editor: waveform, spectrum, markers.", "audio editor, wave editor, recorder, sample editor"},
      {"gl.seltmann", F::Sample, "Finds the beat in any song and chops it into loops.",
       "chop, chopper, sample chop, flip, loop maker, microsampling, beat slicer, bpm, tempo detection, sampler, phrase"},
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
