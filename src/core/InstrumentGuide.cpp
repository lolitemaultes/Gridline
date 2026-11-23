// Gridline — the guide to the built-in instruments: see InstrumentGuide.h. Keep the order and the
// words in step with docs/instruments.md (the test "instrument_guide" compares them).
#include "InstrumentGuide.h"
#include "I18n.h"

namespace gl::guide {

namespace {
using i18n::N_;
const char* kFlag = N_("Flagships");
const char* kSub = N_("Subtractive and virtual analog");
const char* kWave = N_("Wavetable and hybrid");
const char* kFm = N_("FM");
const char* kAdd = N_("Additive, resynthesis and pictures");
const char* kModel = N_("Physical models");
const char* kSamp = N_("Samplers");
const char* kDrum = N_("Drums");
const char* kUtil = N_("Patching, MIDI and control");
}  // namespace

const std::vector<InstrumentEntry>& instruments() {
  static const std::vector<InstrumentEntry> v = {
      // Flagships, in this order (Tideline, Chater, Loupe and Terrace are effects).
      {"gl.plasma", kFlag, N_("Wavetable hybrid synth: three layers of wavetables, samples, grains or resynthesis, two filters and effect lanes"),
       N_("modern leads, basses, pads and sound design")},
      {"gl.seltmann", kFlag, N_("Automatic sample chopper: finds the tempo, the bars and the hits, then plays a chop layout in time"),
       N_("flipping breaks, loops, vocals and whole songs")},
      {"gl.platter", kFlag, N_("Two DJ decks and a mixer with cues, loops, beat jumps and scratching"),
       N_("mixing tracks live, scratching and recording DJ sets")},
      {"gl.tideline", kFlag, N_("Effect: a tempo-synced volume shaper, triggered by the beat, another track, notes or MIDI"),
       N_("pumping and ducking without routing a sidechain")},
      {"gl.chater", kFlag, N_("Effect: soft glitch breaks, glitter grains, tape haze and a long shimmering hall"),
       N_("dreamy textures, stutters and spacey tails")},
      {"gl.edison", kFlag, N_("Effect: an audio editor in a mixer slot that records, cuts, fades and cleans up audio"),
       N_("editing recordings and samples inside the project")},
      {"gl.terrace", kFlag, N_("Effect: a multiband rhythmic gate with volume, filter and pan steps per band, LFOs and a sidechain"),
       N_("chopped pads, rolling grooves and moving mixes")},
      // Subtractive and virtual analog.
      {"gl.synth", kSub, N_("Three oscillators and noise into a filter"), N_("quick basses, leads and plucks")},
      {"gl.analog", kSub, N_("Virtual analog: three oscillators with unison, sync, ring and FM, a multimode filter, arpeggiator and effects"),
       N_("classic pads, basses, brass and arpeggios")},
      {"gl.twinva", kSub, N_("Two oscillators with unison stacks, hard sync and ring modulation into a resonant filter"),
       N_("supersaw leads, plucks and stacked chords")},
      {"gl.sawswarm", kSub, N_("Stacks of detuned saws with hard sync and a sub saw"), N_("huge detuned leads and chords")},
      {"gl.dcosynth", kSub, N_("Two digitally controlled oscillators with pulse width, sub and cross modulation, a filter and chorus"),
       N_("warm polyphonic pads, strings and keys")},
      {"gl.warmsynth", kSub, N_("Three single-cycle oscillators into a warm resonant filter"), N_("simple, warm basses and leads")},
      {"gl.pocketsynth", kSub, N_("A small wavetable voice with stacked or detuned copies, slide and a filter"), N_("fast mono leads and slides")},
      {"gl.hexsynth", kSub, N_("Two oscillators over a drawn wave set, a filter, drive, effects and a plate reverb"),
       N_("retro digital pads and bright keys")},
      {"gl.acid", kSub, N_("Monophonic saw or square bass with a resonant low pass, accent, slide and a step sequencer"), N_("squelchy acid lines")},
      {"gl.stepbass", kSub, N_("Monophonic bass voice with a 24 dB ladder low pass, accents, distortion, delay and reverb"),
       N_("driving sequenced bass lines")},
      // Wavetable and hybrid.
      {"gl.wavetable", kWave, N_("Two wavetable oscillators with warp, unison, two filters and a modulation matrix"),
       N_("growls, morphing pads and modern basses")},
      {"gl.gms", kWave, N_("Band-limited wave oscillators from built-in or your own single-cycle files, with filters and modulation"),
       N_("layered digital pads and leads")},
      // FM.
      {"gl.matrixfm", kFm, N_("Six operators and three filters wired by a modulation matrix, with drawn envelopes"),
       N_("bells, electric pianos, metallic basses and evolving tones")},
      {"gl.fm", kFm, N_("Four-operator FM with eight algorithms"), N_("electric pianos, bells and punchy basses")},
      {"gl.fmduo", kFm, N_("One sine carrier with two modulators"), N_("simple bells, basses and keys")},
      // Additive, resynthesis and pictures.
      {"gl.additive", kAdd, N_("Additive: up to 256 sine partials shaped by a harmonic filter, a phaser, pluck and a harmonizer"),
       N_("organs, glassy pads, bells and filter sweeps made of partials")},
      {"gl.harmor", kAdd, N_("Resynthesis: plays a sample or a picture as sine partials in two parts and reshapes them"),
       N_("resynthesized samples, vocal textures and spectral morphs")},
      {"gl.spectramorph", kAdd, N_("Resynthesized harmonic frames from four corners, blended on an X / Y pad"), N_("morphing tones and moving pads")},
      {"gl.alloy", kAdd, N_("Harmonic spectra from two seeded random timbres, morphed and tilted"), N_("bells, gongs, metallic hits and drones")},
      {"gl.imagesynth", kAdd, N_("Reads a picture as a spectrogram: each row a sine, each column a moment"), N_("turning images into sound")},
      {"gl.pixeltone", kAdd, N_("Plays your own pictures column by column as waves"), N_("lo-fi tones and textures from images")},
      // Physical models.
      {"gl.string", kModel, N_("Two coupled waveguide strings, plucked, picked, hammered or bowed, with a body and an amp"),
       N_("guitars, harps, bowed strings and strums")},
      {"gl.twinstring", kModel, N_("An overdriven exciter into two tuned waveguide strings and a comb"), N_("plucked, reedy and metallic tones")},
      {"gl.plucker", kModel, N_("A lean plucked string (a tuned noise loop)"), N_("plucks, harps and quick arpeggios")},
      {"gl.pluck", kModel, N_("Karplus-Strong string, two detuned strings for stereo"), N_("simple plucks")},
      {"gl.fingerbass", kModel, N_("A plucked bass-guitar waveguide with a three-band tone stack"), N_("fingered bass lines")},
      {"gl.keys", kModel, N_("An electric piano model with hardness, tremolo and overdrive"), N_("electric piano chords and keys")},
      {"gl.samplekeys", kModel, N_("Sampled keyboards with key groups, velocity layers and tuning (needs sound sets on this computer)"),
       N_("pianos and keys from your own sound sets")},
      // Samplers.
      {"gl.sampler", kSamp, N_("Plays one sample across the keyboard, with loop points, filter and envelopes"), N_("one-shots, loops and pitched samples")},
      {"gl.multisampler", kSamp, N_("Zones of samples across keys and velocities; opens SFZ and SoundFont files"),
       N_("sampled instruments and SoundFonts")},
      {"gl.patchplayer", kSamp, N_("Plays one patch of a SoundFont file as the file describes it, with its envelopes, modulators and sends"),
       N_("General MIDI songs and SoundFont instruments")},
      {"gl.waveprogram", kSamp, N_("Sample programs: zones, a modulation matrix, filters, insert effects and buses"),
       N_("layered sample instruments")},
      {"gl.slicer", kSamp, N_("Region slicer that cuts a sample on the beat grid and plays the slices from keys"), N_("chopping loops into playable slices")},
      {"gl.granular", kSamp, N_("Grains read from the channel's sample"), N_("clouds, stretched textures and pads")},
      {"gl.grainscan", kSamp, N_("Grains that scan through the channel's sample at their own speed"), N_("time stretched, frozen and scanned sounds")},
      {"gl.scratch", kSamp, N_("A sample played back and forth by drawn scratch moves"), N_("scratches and rewinds from any sample")},
      {"gl.splinescratch", kSamp, N_("Each key plays its own drawn position and volume curves over the sample"), N_("tempo-synced scratch patterns")},
      {"gl.tracker", kSamp, N_("The Tracker's sample instrument: samples, loops and envelopes as a tracker keeps them"),
       N_("module-style songs and chip sounds")},
      // Drums.
      {"gl.pads", kDrum, N_("32 sample pads with velocity layers, choke groups and envelopes"), N_("drum kits from your own samples")},
      {"gl.drumkit", kDrum, N_("16 pads of modal drums struck by a mallet, with a noise layer"), N_("synthesized acoustic and electronic kits")},
      {"gl.membranepad", kDrum, N_("One keyboard-pitched membrane drum"), N_("tuned toms, hand drums and percussion")},
      {"gl.membranekit", kDrum, N_("16 membrane drums with a step sequencer, an EQ and a limiter"), N_("whole synthesized kits and grooves")},
      {"gl.kicksculpt", kDrum, N_("A pitch-swept sine kick with click, drive and noise layers"), N_("shaping a kick from scratch")},
      {"gl.sweepkick", kDrum, N_("A sine sweeping down with click and distortion"), N_("quick electronic kicks")},
      {"gl.kick", kDrum, N_("A synthesized kick drum"), N_("simple kicks")},
      {"gl.snare", kDrum, N_("A synthesized snare: tone and noise"), N_("simple snares")},
      {"gl.hat", kDrum, N_("Synthesized hi-hats"), N_("simple hats")},
      {"gl.clap", kDrum, N_("A synthesized hand clap"), N_("simple claps")},
      // Patching, MIDI and control.
      {"gl.patcher", kUtil, N_("A patch of instruments and effects joined with cables, with eight macros"),
       N_("layering, splitting and building your own instruments")},
      {"gl.midiout", kUtil, N_("Sends the channel's notes and controllers to a MIDI device or to other channels"), N_("playing hardware synths and other channels")},
      {"gl.envctl", kUtil, N_("Turns notes into an envelope that can move any knob"), N_("rhythmic modulation of other plugins")},
      {"gl.keyctl", kUtil, N_("Turns the notes played into control values"), N_("key-tracked modulation of other plugins")},
      {"gl.cvctl", kUtil, N_("Turns notes into pitch and gate voltages on an audio interface"), N_("playing modular synths")},
      {"gl.cvin", kUtil, N_("Reads voltages from an audio interface and turns them into notes"), N_("playing Gridline from modular gear")},
      {"gl.flex", kUtil, N_("A stand-in for a preset player: one of Gridline's engines, chosen by the preset's kind of sound, with its macros"),
       N_("playing imported projects that used a preset player")},
  };
  return v;
}

const InstrumentEntry* find(const std::string& type) {
  for (const InstrumentEntry& e : instruments())
    if (type == e.type) return &e;
  return nullptr;
}

std::string shortLine(const std::string& type) {
  const InstrumentEntry* e = find(type);
  if (!e) return {};
  return std::string(i18n::tr(e->engine)) + ". " + i18n::tr("Good for") + " " + i18n::tr(e->goodFor) + ".";
}

const char* pageUrl() { return "https://github.com/lolitemaultes/Gridline/blob/main/docs/instruments.md"; }

}  // namespace gl::guide
