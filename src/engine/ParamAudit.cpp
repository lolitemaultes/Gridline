// Gridline — J-92 parameter audit, see ParamAudit.h.
#include "ParamAudit.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <cstring>
#include <thread>
#if defined(_WIN32)
#include <io.h>
namespace {
int rawWrite(int fd, const void* p, size_t n) { return _write(fd, p, static_cast<unsigned>(n)); }  // async-signal-safe write
}  // namespace
#else
#include <unistd.h>
namespace {
long rawWrite(int fd, const void* p, size_t n) { return ::write(fd, p, n); }  // async-signal-safe write
}  // namespace
#endif
#include <filesystem>
#include "../core/AudioBuffer.h"
#include "../core/Params.h"
#include "../core/Platform.h"
#include "../core/WavWriter.h"
#include "../core/Project.h"
#include "../core/Song.h"
#include "../packs/Packs.h"
#include "Effects.h"
#include "Generators.h"
#include "NoteFx.h"
#include "Program.h"

namespace gl::paramaudit {

const char* statusName(Status s) {
  switch (s) {
    case Status::Working: return "working";
    case Status::Combination: return "combination";
    case Status::NoEffect: return "no-effect";
    case Status::Routing: return "routing";
    case Status::Broken: return "broken";
    case Status::Inert: return "inert";
    case Status::Verified: return "verified";
  }
  return "?";
}

// Parameters that change nothing on purpose. `param` is an exact name, a
// prefix ending in '*' or a suffix starting with '*'; type "*" = any plugin.
namespace {
struct InertDoc {
  const char* type;
  const char* param;
  const char* reason;
};
const InertDoc kInert[] = {
    {"*", "(unused)", "reserved slot of the stored layout: no function"},
    {"*", "(spare)", "reserved slot of the stored layout: no function"},
    // Visual tools: every setting is about the display.
    {"gl.analyzer", "*", "display setting of the analyzer (no audio path)"},
    {"gl.scope", "*", "display setting of the scope (no audio path)"},
    {"gl.spectro", "*", "display setting of the spectrogram (no audio path)"},
    {"gl.meter", "*", "display setting of the meter (no audio path)"},
    {"gl.tuner", "*", "display setting of the tuner (no audio path)"},
    {"gl.clock", "*", "display setting of the clock (no audio path)"},
    {"gl.notes", "Page", "notebook page shown (no audio path)"},
    // Editor views and tools.
    {"gl.eq", "Histogram", "analyzer display in the editor"},
    {"gl.eq", "Histogram range", "analyzer display in the editor"},
    {"gl.eq", "Histogram average", "analyzer display in the editor"},
    {"gl.eq", "Heat map", "analyzer display in the editor"},
    {"gl.eq", "Time smoothing", "analyzer display in the editor"},
    {"gl.eq", "Pivot slope", "analyzer display tilt in the editor"},
    {"gl.eq", "Show phase", "editor display: phase curve"},
    {"gl.eq", "Band tokens", "editor display: band markers"},
    {"gl.bankeq", "Edit curve", "editor tool: which curve the mouse edits"},
    {"gl.bankeq", "Edit master", "editor tool: which curve the mouse edits"},
    {"gl.bankeq", "Draw tool", "editor tool"},
    {"gl.limiter", "View", "editor page"},
    {"gl.maximizer", "Edit stage", "editor page: the stage being edited"},
    {"gl.splitter", "Graph shows", "editor display"},
    {"gl.splitter", "Main view", "editor display"},
    {"gl.splitter", "Send view", "editor display"},
    {"gl.spread", "Show input", "editor display"},
    {"gl.spread", "Show output", "editor display"},
    {"gl.vocoder", "Editor curve", "editor display: the curve the mouse edits"},
    {"gl.convolver", "Reported latency", "read-only: the latency the engine reports"},
    // Routing (resolved by the engine graph / MIDI ports; covered by the routing tests).
    {"gl.midiout", "Port", "MIDI output port (routing)"},
    {"gl.send", "Target track", "send destination (routing)"},
    {"gl.send", "Target numbering", "send destination numbering (routing)"},
    {"gl.bussend", "Bus", "bus destination (routing)"},
    {"gl.busreturn", "Bus", "bus source (routing)"},
    {"gl.splitter", "Send numbering", "send destination numbering (routing)"},
    {"gl.tideline", "MIDI port", "MIDI trigger source port (routing; covered by test_tideline)"},
    {"gl.tideline", "MIDI note", "MIDI trigger note filter (needs MIDI input; covered by test_tideline)"},
    {"gl.seltmann", "Stretch mode", "engine of the region stretch a worker thread renders after playback asks for a tempo; the audit hears the resampled fallback (covered by test_seltmann)"},
    {"gl.transplit", "Transient send route", "send destination (routing)"},
    {"gl.transplit", "Sustain send route", "send destination (routing)"},
    {"gl.pitcher", "MIDI port", "MIDI input port for the harmony notes (routing)"},
    {"gl.vocoder", "Carrier MIDI port", "MIDI input port for the carrier notes (routing)"},
    {"gl.hall", "Sidechain source", "sidechain input route (routing)"},
    // The audio editor's recorder and its own preview transport.
    {"gl.edison", "*", "recorder / preview playback setting of the audio editor (acts while recording or previewing, not on the effect's audio)"},
    // Acts on the transport, not on audio while playing.
    {"gl.timegate", "Run when stopped", "only acts while the transport is stopped"},
    {"gl.hitloop", "Clear on stop", "acts when the transport stops"},
    // Data-dependent: the parameter acts on content the default state has none of.
    {"gl.patcher", "Param *", "drives the control the loaded patch maps to it; the default patch maps none"},
    {"gl.patcherfx", "Param *", "drives the control the loaded patch maps to it; the default patch maps none"},
    {"gl.formulactl", "b", "used only by a formula that refers to b (the default formula uses a)"},
    {"gl.formulactl", "c", "used only by a formula that refers to c (the default formula uses a)"},
    {"gl.pixeltone", "Blue bends pitch", "acts on the blue channel of the loaded picture (the default picture has none)"},
    {"gl.pixeltone", "Max rows", "applies when the loaded picture is taller (the default picture is 16 rows)"},
    {"gl.grainscan", "Transient source", "only automatic detection exists: Gridline samples carry no slice / marker data"},
    // Kept for older projects and the FL round trip; the current engine has no use for them.
    {"gl.multiband", "*(legacy)", "earlier model's setting, kept for older projects; the current engine has its own per-band controls"},
    {"gl.flex", "Post filter type", "FLEX preset value kept for the FL round trip; the stand-in has one post filter"},
    {"gl.flex", "Delay type", "FLEX preset value kept for the FL round trip; the stand-in has one delay"},
    {"gl.flex", "Limiter type", "FLEX preset value kept for the FL round trip; the stand-in has one limiter"},
    {"gl.sweepkick", "Seed", "kept for the FL round trip; the sweep has no random element"},
    {"gl.fm", "Op 1 thru", "operator 1 is a carrier in every algorithm (control disabled with a hint)"},
    // Data-dependent (state.data content the default state lacks), verified in the engines.
    {"gl.harmor", "* F1 env", "scales the filter-frequency articulator, neutral until an envelope / LFO / map is drawn"},
    {"gl.harmor", "* F2 env", "scales the filter-frequency articulator, neutral until an envelope / LFO / map is drawn"},
    {"gl.harmor", "* prism", "offsets come from the Prism curve, flat until drawn"},
    {"gl.harmor", "* prism multiply", "changes how the Prism curve applies, flat until drawn"},
    {"gl.harmor", "* pitch env range", "scales the pitch articulator, neutral until drawn"},
    {"gl.harmor", "* image *", "acts only when the part has an image (picture / resynthesis)"},
    {"gl.harmor", "A poly release", "scales the fallback release used only when the volume envelope is switched off"},
    {"gl.harmor", "Mod *", "global X / Y / Z feed articulator mapping curves, flat until drawn"},
    {"gl.alloy", "Mod X", "input of drawn X map curves (none by default)"},
    {"gl.alloy", "Mod Y", "input of drawn Y map curves (none by default)"},
    {"gl.alloy", "Mod smooth", "smooths X / Y, which act only through drawn maps"},
    {"gl.alloy", "Declick", "used only when the volume envelope is off (on by default)"},
    {"gl.alloy", "One voice per key", "acts only when a held key is struck again"},
    {"gl.alloy", "Velocity to volume", "the host velocity curve always applies (J-65); kept for the FL round trip"},
    {"gl.alloy", "Quality", "the sine bank has no quality levels; kept for the FL round trip"},
    {"gl.alloy", "Natural EQ *", "changes how drawn timbre EQ curves are read (none by default)"},
    {"gl.alloy", "Vol env decay", "the default volume envelope is flat through its decay"},
    {"gl.alloy", "Vol LFO *", "the volume LFO is off in the default data"},
    {"gl.alloy", "Cut *", "the filter envelope / LFO are off in the default data"},
    {"gl.alloy", "Res *", "the resonance envelope / LFO are off in the default data"},
    {"gl.alloy", "Pitch *", "the pitch envelope / LFO are off in the default data"},
    {"gl.alloy", "Uni pitch *", "the unison-pitch envelope / LFO are off in the default data"},
    {"gl.octafilter", "Unit * envelope amount", "scales follower maps, pattern envelopes and the cutoff LFO, all drawn curves (none by default)"},
    {"gl.octafilter", "Unit * LFO *", "a target's LFO runs only when an LFO curve is drawn for it (none by default)"},
    {"gl.octafilter", "Master LFO level", "scales the per-target LFOs, which need drawn LFO curves"},
    {"gl.octafilter", "Follower *", "the follower drives only follower maps (none by default)"},
    {"gl.octafilter", "Pattern", "selects a pattern envelope (none by default)"},
    {"gl.octafilter", "Sync pattern changes", "acts on pattern envelopes (none by default)"},
    {"gl.octafilter", "Modulation X", "read through drawn X maps (none by default)"},
    {"gl.octafilter", "Modulation Y", "read through drawn Y maps (none by default)"},
    {"gl.octafilter", "Smooth modulation", "smooths X / Y, which act only through drawn maps"},
    {"gl.octafilter", "Unit 8 to next unit", "unit 8 is the last unit: there is no next unit"},
    {"gl.enhance", "Program: * point 1 tension", "the first curve point has no segment leading into it"},
    {"gl.enhance", "Program: Band mix", "the Amount knob sets the band mix in this program"},
    {"gl.enhance", "Band mix (ref.)", "the Amount knob sets the band mix before the reference engine runs"},
    {"gl.enhance", "Engine", "Program 3 selects the reference engine; kept for the FL round trip"},
    {"gl.master", "* point 1 tension", "the first curve point has no segment leading into it"},
    {"gl.slicer", "Master level", "read by the region engine, which needs a region model (FL import)"},
    {"gl.slicer", "Slice * *", "slices 33..64 exist only when a sample has more than 32 slices (the test sample has 4 hits)"},
    {"gl.amprack", "Module * automation block", "kept for the FL round trip; the engine does not use it"},
    {"gl.amprack", "Module * smoothing", "smooths changes of the filter cutoff / resonance; no effect on fixed settings"},
    {"gl.amprack", "Board value *", "pedal-board slot not read by any pedal type (kept for the FL round trip)"},
    {"gl.membranekit", "Ch * reserved", "reserved slot, never read"},
    {"gl.membranekit", "Reserved *", "reserved slot, never read"},
    {"gl.membranekit", "Swing", "used by the kit's own step sequencer, which needs pattern data"},
    {"gl.spectramorph", "Loop", "loop / one-shot / hold differ only after the analysis run (0.78 s), longer than the audit's notes"},
    {"gl.splinescratch", "Continuous output", "acts only when a note outlasts the 4-beat patch"},
    {"gl.scratcher", "Start time", "spin-up time: acts when the motor restarts during playback"},
    {"gl.stepbass", "Program", "selects a sequence from the bank in the data; the default bank's programs are identical"},
    {"gl.delay", "Filter follows the sample rate", "classic model only; no effect at 44.1 kHz by definition"},
    {"gl.eq", "Monitor", "graph source in the editor; the DSP does not use it"},
    {"gl.banddelay", "Bank morph", "morphs between the band banks in the data; the default banks are identical"},
    {"gl.convolver", "EQ mix", "blends the EQ curve drawn in the data (flat by default)"},
    {"gl.convolver", "IR envelope", "applies the envelope points drawn in the data (none by default)"},
    {"gl.convolver", "IR build", "rebuilds the impulse response on the UI thread (see the live column)"},
    {"gl.hitloop", "Look-ahead", "the engine always looks ahead; kept for the FL round trip"},
    {"gl.reverb", "*(legacy)", "read only when migrating old projects"},
    {"gl.splitter", "Linked crossovers", "editor: links the crossover handles"},
    {"gl.vocoder", "Subtract noise floor", "applies the noise-floor curve drawn in the data (zero by default)"},
    {"gl.waveshaper", "HQ (no truncation)", "reference engine only, which needs a curve in the data"},
    {"gl.flex", "*", "FLEX preset value kept for the FL round trip; the stand-in engine does not model it"},
    {"gl.pitcher", "Harmony * pan", "harmony voices exist only for held MIDI-port notes"},
    {"gl.pitcher", "MIDI octaves", "maps held MIDI-port notes"},
    {"gl.pitcher", "Harmony velocity", "scales harmony voices from held MIDI-port notes"},
    {"gl.envctl", "Art * LFO*", "the articulator LFO record is off in the default model data"},
    {"gl.envctl", "Art * envelope", "the envelope record is on only for articulator 1 in the default data"},
    {"gl.envctl", "Art * scale", "applies to an enabled envelope record (default: articulator 1 only)"},
    {"gl.envctl", "Art * sustain offset", "applies to an enabled envelope record (default: articulator 1 only)"},
    {"gl.envctl", "Mod X", "read through the Mod X map, flat by default"},
    {"gl.envctl", "Mod Y", "read through the Mod Y map, flat by default"},
    {"gl.waveprogram", "*", "program FX / LFOs / mod values: switched on and routed by the program data (off in the default program)"},
    {"gl.notescript", "*", "used by the script text, which is empty by default"},
    {"gl.patcher", "Macro *", "drives what the patch maps to it; the default patch maps macros 1-3"},
    {"gl.patcherfx", "Macro *", "drives what the patch maps to it; the default patch maps macros 1-3"},
    {"gl.patcher", "Mix", "the instrument host has no dry path (the effect version uses it)"},
    {"gl.colormap", "Group *", "needs notes of that colour / velocity layer and an assigned target channel"},
    {"gl.multisampler", "Round robin", "applies to zones with a round-robin group in the zone map (factory sets have none)"},
    {"gl.velscale", "Shown target", "editor: which target the editor shows"},
    {"gl.matrixfm", "Matrix RM * pan", "reserved column of the ring-mod layer; kept for the FL round trip"},
    {"gl.matrixfm", "Matrix RM * effects", "reserved column of the ring-mod layer; kept for the FL round trip"},
    {"gl.matrixfm", "Matrix RM * output", "reserved column of the ring-mod layer; kept for the FL round trip"},
    {"gl.matrixfm", "Matrix RM (unused) *", "reserved rows of the ring-mod layer"},
    {"gl.dcosynth", "Arp quantize first step", "kept for the FL round trip; not used by the engine"},
    // Works, but the step lies after the audit's 0.6 s render window (beat 2.75+ at 128 BPM).
    {"gl.acid", "Step 13 *", "VERIFIED: works with the sequencer on; the step starts after the audit's 0.6 s render window"},
    {"gl.acid", "Step 14 *", "VERIFIED: works with the sequencer on; the step starts after the audit's 0.6 s render window"},
    {"gl.acid", "Step 15 *", "VERIFIED: works with the sequencer on; the step starts after the audit's 0.6 s render window"},
    {"gl.acid", "Step 16 *", "VERIFIED: works with the sequencer on; the step starts after the audit's 0.6 s render window"},
    {"gl.timegate", "Gate step 1*", "VERIFIED: works with Gate on; steps 12-16 start after the audit's render window"},
    {"gl.hexsynth", "Step *", "VERIFIED: works with the sequencer on; later steps start after the audit's render window"},
    {"gl.notesq", "Step *", "VERIFIED: works; later steps start after the audit's render window"},
    // Works under settings the automated search did not reach within its budget
    // (several switches or a chain of settings); each verified in the engine source.
    {"gl.acid", "Step *", "VERIFIED: step note / octave / accent / slide act with the sequencer on and the step gated"},
    {"gl.amprack", "Module *", "VERIFIED: read by the module kind / algorithm that uses it (rack model 1, modules enabled)"},
    {"gl.dcosynth", "Matrix * source", "VERIFIED: needs a target and an amount on the same row"},
    {"gl.dcosynth", "Restart envelopes on release", "VERIFIED: acts in mono mode on overlapping notes released newest-first"},
    {"gl.dcosynth", "Arp classic order", "VERIFIED: acts with the arpeggiator on and several held notes"},
    {"gl.dcosynth", "LFO trigger", "VERIFIED: pressing it restarts the LFOs (J-92 fix); a static value has nothing to restart"},
    {"gl.delay", "Saturation", "VERIFIED: legacy model with Distortion off"},
    {"gl.drawneq", "Smoothing", "VERIFIED: glides morph / depth changes while a curve is drawn"},
    {"gl.enhance", "*point*", "VERIFIED: drawn-curve points act in a chain (curve points >= N, point N-1 x < 1) with the band's curve engine on"},
    {"gl.enhance", "Program: * curve points", "VERIFIED: acts when the added points differ from the curve's end point"},
    {"gl.enhance", "Program: * low threshold", "VERIFIED: upward / downward action below it needs quiet material under the threshold with a below ratio set"},
    {"gl.enhance", "* (ref.)", "VERIFIED: reference-engine band timing / options act with Program 3 and the band driven (pre gain)"},
    {"gl.master", "*point*", "VERIFIED: drawn-curve points act in a chain (curve points >= N, point N-1 x < 1) with the band's curve engine on"},
    {"gl.master", "* curve points", "VERIFIED: acts when the added points differ from the curve's end point"},
    {"gl.envctl", "Env * point *", "VERIFIED: simple engine with that many points and sustain off"},
    {"gl.envctl", "Env * LFO rate", "VERIFIED: simple engine with the LFO on and free (not synced)"},
    {"gl.gms", "DELAY Tone", "VERIFIED: filters the feedback, audible from the second echo on"},
    {"gl.minihalt", "Start Time", "VERIFIED: shapes the spin-up after Run is switched back on (a static state never restarts)"},
    {"gl.grainscan", "Transient hold", "VERIFIED: with Key to = Transient (J-92 fix: now read)"},
    {"gl.hall", "Env sidechain", "VERIFIED: needs the sidechain input and an envelope target"},
    {"gl.harmor", "B F1 freq", "VERIFIED: part B on with its filter set"},
    {"gl.harmor", "B F2 freq", "VERIFIED: part B on with its filter set"},
    {"gl.harmor", "B F1 reso", "VERIFIED: part B on with its filter set"},
    {"gl.harmor", "B F2 reso", "VERIFIED: part B on with its filter set"},
    {"gl.harmor", "Delay", "VERIFIED: the default delay (1.5 beats) is longer than the audit's period; audible with a shorter time"},
    {"gl.harmor", "Delay time", "VERIFIED: the default delay (1.5 beats) is longer than the audit's period; audible with a shorter time"},
    {"gl.hexsynth", "Osc * to *", "VERIFIED: FM amount from an oscillator that is on with a level"},
    {"gl.hexsynth", "Mod * target", "VERIFIED: needs a source and an amount on the same row"},
    {"gl.hexsynth", "Osc * decay", "VERIFIED: the oscillator on with sustain below 1"},
    {"gl.hexsynth", "Osc * phase", "VERIFIED: the oscillator on with free run off"},
    {"gl.hexsynth", "Sequencer *", "VERIFIED: sequencer options act with the sequencer on and several steps set"},
    {"gl.limiter", "Comp detector input", "VERIFIED: compressor engaged (threshold / ratio) with the sidechain connected"},
    {"gl.membranekit", "Ch * Mid *", "VERIFIED: the channel unmuted with its mid band level up"},
    {"gl.multidelay", "Tap *", "VERIFIED: the tap on (taps 3-8 are off by default), its filter / gate / mod section on"},
    {"gl.pads", "Pad * layer *", "VERIFIED: the layer on with a sample and a velocity range that the hit falls in"},
    {"gl.pads", "Pad * hold", "VERIFIED: the pad's envelope acts with one shot off and the note held"},
    {"gl.pads", "Pad * decay", "VERIFIED: the pad's envelope acts with one shot off and the note held"},
    {"gl.pads", "Pad * sustain", "VERIFIED: the pad's envelope acts with one shot off and the note held"},
    {"gl.pads", "Pad * release", "VERIFIED: the pad's envelope acts with one shot off"},
    {"gl.pads", "Pad * layer mode", "VERIFIED: chooses among several enabled layers"},
    {"gl.pads", "Pad * velocity scaling", "VERIFIED: velocity to level of the pad"},
    {"gl.pocketrack", "Slot * control *", "VERIFIED: read by the module loaded in the slot (each module uses its own number of controls)"},
    {"gl.wavetable", "Env * decay", "VERIFIED: the envelope routed in the modulation matrix"},
    {"gl.wavetable", "LFO * division", "VERIFIED: the LFO routed and tempo-synced"},
    {"gl.matrixfm", "Op *", "VERIFIED: operator articulator settings act with that articulator's envelope / LFO on and the operator in the output"},
    {"gl.matrixfm", "Matrix *", "VERIFIED: a matrix route acts when its source operator sounds and its destination reaches the output"},
    {"gl.matrixfm", "Filter *", "VERIFIED: filters 2-4 act with an operator routed into them and the filter routed to the output (engine reads them, matrixfm_dsp.cpp)"},
    {"gl.matrixfm", "EQ *", "VERIFIED: EQ bands act with an EQ target set and the band gain away from 0"},
    {"gl.matrixfm", "Delay*", "VERIFIED: delay units act when switched on with a volume"},
    {"gl.matrixfm", "Reverb*", "VERIFIED: the reverb acts when switched on with wet level"},
    {"gl.matrixfm", "Chorus *", "VERIFIED: the chorus acts with voices and a volume above 0"},
    {"gl.matrixfm", "Effects pan", "VERIFIED: pans the effect returns (chorus / delay / reverb on)"},
    {"gl.matrixfm", "Unison *", "VERIFIED: unison settings act with unison order above 1"},
    {"gl.matrixfm", "Portamento time", "VERIFIED: with portamento on and overlapping notes"},
    {"gl.matrixfm", "Randomness", "VERIFIED: randomizes operator start phases / pitches per note when enabled"},
    {"gl.matrixfm", "Smooth modulation", "VERIFIED: smooths Mod X / Y changes"},
    {"gl.matrixfm", "Mod X", "VERIFIED: read through articulator X mappings (drawn)"},
    {"gl.matrixfm", "Mod Y", "VERIFIED: read through articulator Y mappings (drawn)"},
    // gl.plasma: the coverage test in tests/packs/test_plasma.cpp renders each parameter under the
    // settings that use it (a source routed, a module type, an oscillator type with a synthetic sample).
    {"gl.plasma", "Legacy Warp Mode", "kept for the round trip of imported presets; it has no audible behaviour of its own"},
    {"gl.plasma", "LFO * Grid Y", "drawing grid of the LFO editor (vertical snap); not part of the sound"},
    {"gl.plasma", "FX * Param 28", "reserved column of the effect slot (no module uses 28 parameters)"},
    {"gl.plasma", "Filter * Filter Balance", "a filter output goes whole into the other filter (serial), so the F1 / F2 balance of the filter routes is unused"},
    {"gl.plasma", "A *", "VERIFIED: acts with the oscillator type / settings it belongs to (sample, granular, spectral, multisample, warps, unison, key zone); tests/packs/test_plasma.cpp, parameter coverage"},
    {"gl.plasma", "B *", "VERIFIED: oscillator B: same code as A (parameter coverage test)"},
    {"gl.plasma", "C *", "VERIFIED: oscillator C: same code as A (parameter coverage test)"},
    {"gl.plasma", "Sub *", "VERIFIED: acts with the sub on (and routed to a filter for its balance; parameter coverage test)"},
    {"gl.plasma", "Noise *", "VERIFIED: acts with the noise on and a sample / one-shot type (parameter coverage test)"},
    {"gl.plasma", "Filter *", "VERIFIED: acts with the filter on and a source routed into it (parameter coverage test)"},
    {"gl.plasma", "Env *", "VERIFIED: acts when the envelope is routed through the matrix, with legato / mono / tempo for those switches (parameter coverage test)"},
    {"gl.plasma", "LFO Bus *", "VERIFIED: moves an LFO point that a bus link names (parameter coverage test)"},
    {"gl.plasma", "LFO *", "VERIFIED: acts when the LFO is routed through the matrix (rate, sync, snap, swing, mode, shape; parameter coverage test)"},
    {"gl.plasma", "Macro *", "VERIFIED: acts when the macro is routed through the matrix (parameter coverage test)"},
    {"gl.plasma", "Retrigger *", "VERIFIED: acts on legato notes in mono legato (parameter coverage test)"},
    {"gl.plasma", "Mod *", "VERIFIED: a matrix slot acts with a source and a destination set (parameter coverage test)"},
    {"gl.plasma", "FX *", "VERIFIED: an effect slot acts with a module type on it; each parameter acts for a module that has it (parameter coverage test)"},
    {"gl.plasma", "Arp *", "VERIFIED: acts with the arpeggiator on (parameter coverage test)"},
    {"gl.plasma", "Clip *", "VERIFIED: acts with the clip player on (parameter coverage test)"},
    {"gl.plasma", "Swing*", "VERIFIED: swings the arpeggiator / clip player / LFO swing (parameter coverage test)"},
    {"gl.plasma", "Voice Ctrl *", "VERIFIED: acts with per-voice random / sequence values (parameter coverage test)"},
    {"gl.plasma", "Bus *", "VERIFIED: acts with a bus send above 0 (parameter coverage test)"},
    {"gl.plasma", "Direct Volume", "VERIFIED: acts with a source routed to DIRECT (parameter coverage test)"},
    {"gl.plasma", "Bend *", "VERIFIED: acts with the pitch wheel moved (parameter coverage test)"},
    {"gl.plasma", "Mod Wheel", "VERIFIED: acts when the mod wheel is routed through the matrix (parameter coverage test)"},
    {"gl.plasma", "Key", "VERIFIED: snaps arpeggiator notes to the scale (parameter coverage test)"},
    {"gl.plasma", "Scale", "VERIFIED: scale of the arpeggiator / pitch mode Steps (parameter coverage test)"},
    {"gl.plasma", "MPE*", "VERIFIED: acts with MPE input (per-channel bend / slide; parameter coverage test)"},
    {"gl.plasma", "Porta *", "VERIFIED: acts with portamento on overlapping notes (parameter coverage test)"},
    {"gl.plasma", "LFO Scal*", "VERIFIED: scales LFO rates when an LFO is routed (parameter coverage test)"},
    {"gl.plasma", "Disable Smoothing", "VERIFIED: removes the amplitude smoothing (parameter coverage test)"},
    // Known gaps: the engine does not implement these yet (listed in AUDIT.md).
    {"gl.harmor", "* sub mode", "GAP: not implemented (fixed sub ratios)"},
    {"gl.harmor", "* FX / dry", "GAP: not implemented"},
    {"gl.harmor", "* vel > *", "GAP: not implemented"},
    {"gl.harmor", "* blur *", "GAP: not implemented"},
    {"gl.harmor", "* harm shift step", "GAP: not implemented"},
    {"gl.harmor", "* harm gap*", "GAP: not implemented"},
    {"gl.harmor", "* unison pan", "GAP: values above 1 act like 1"},
    {"gl.harmor", "* unison phase", "GAP: not implemented"},
    {"gl.harmor", "* glide variable", "GAP: not implemented"},
    {"gl.harmor", "B glide*", "GAP: part B uses part A's glide (one pitch glide per voice)"},
    {"gl.harmor", "* F2 self-osc", "GAP: only filter 1 self-oscillates"},
    {"gl.harmor", "* adaptive *", "GAP: not implemented"},
    {"gl.harmor", "* formant*", "GAP: not implemented"},
    {"gl.harmor", "* predelay", "GAP: not implemented"},
    {"gl.harmor", "B poly release", "GAP: part B uses part A's release scale"},
    {"gl.harmor", "Legato limit", "GAP: not implemented"},
    {"gl.harmor", "Strum *", "GAP: strum not implemented"},
    {"gl.harmor", "LFO amount", "GAP: not implemented"},
    {"gl.harmor", "Delay offset", "GAP: not implemented"},
    {"gl.harmor", "Delay pan", "GAP: not implemented"},
    {"gl.harmor", "Delay low cut", "GAP: not implemented"},
    {"gl.harmor", "Delay high cut", "GAP: not implemented"},
    {"gl.harmor", "Delay damping", "GAP: not implemented"},
    {"gl.harmor", "Reverb low cut", "GAP: not implemented"},
    {"gl.harmor", "Reverb high cut", "GAP: not implemented"},
    {"gl.harmor", "Reverb predelay", "GAP: not implemented"},
    {"gl.harmor", "Reverb diffusion", "GAP: not implemented"},
    {"gl.harmor", "Reverb damping", "GAP: not implemented"},
    {"gl.harmor", "Reverb colour", "GAP: not implemented"},
    {"gl.harmor", "Comp low", "GAP: not implemented"},
    {"gl.harmor", "Comp mid", "GAP: not implemented"},
    {"gl.harmor", "Comp high", "GAP: not implemented"},
    {"gl.harmor", "EQ curve mix", "GAP: not implemented"},
    {"gl.alloy", "Delay offset", "GAP: not implemented"},
    {"gl.alloy", "Reverb low cut", "GAP: not implemented"},
    {"gl.alloy", "Reverb high cut", "GAP: not implemented"},
    {"gl.alloy", "Reverb predelay", "GAP: not implemented"},
    {"gl.alloy", "Reverb tempo", "GAP: not implemented"},
    {"gl.alloy", "Reverb diffusion", "GAP: not implemented"},
    {"gl.alloy", "Reverb damping", "GAP: not implemented"},
    {"gl.alloy", "Chorus spread", "GAP: not implemented"},
    {"gl.alloy", "Chorus cross", "GAP: not implemented"},
    {"gl.alloy", "Velocity to attack *", "GAP: not implemented"},
    {"gl.alloy", "Release follows volume", "GAP: not implemented"},
    {"gl.alloy", "Auto loudness", "GAP: not implemented"},
    {"gl.harmor", "Chorus spread", "GAP: not implemented"},
    {"gl.harmor", "Chorus cross", "GAP: not implemented"},
    {"gl.timegate", "Trigger slots from notes", "GAP: effects get no note input path for it yet"},
    {"gl.surface", "Control *", "GAP: controls 17-64 output, but links read only 16 controller outputs"},
};
// Settings that enable whole sections, tried first by the condition search
// ("Name=value|Name=value", plugin units): sections that need several
// switches at once, which the generic search would only reach by luck.
struct HintDoc {
  const char* type;
  const char* set;
};
const HintDoc kHints[] = {
    {"gl.hexsynth", "FX 1 on=1|FX 1 type=0"},
    {"gl.hexsynth", "FX 1 on=1|FX 1 type=1"},
    {"gl.hexsynth", "FX 1 on=1|FX 1 type=2"},
    {"gl.hexsynth", "FX 1 on=1|FX 1 type=3"},
    {"gl.hexsynth", "FX 1 on=1|FX 1 type=4"},
    {"gl.hexsynth", "FX 2 on=1|FX 2 type=0"},
    {"gl.hexsynth", "FX 2 on=1|FX 2 type=1"},
    {"gl.hexsynth", "FX 2 on=1|FX 2 type=2"},
    {"gl.hexsynth", "FX 2 on=1|FX 2 type=3"},
    {"gl.hexsynth", "FX 2 on=1|FX 2 type=4"},
    {"gl.hexsynth", "EQ on=1"},
    {"gl.hexsynth", "Sequencer on=1|Sequencer steps=1"},
    {"gl.hexsynth", "Sequencer on=1|Step 2=1"},
    {"gl.hexsynth", "Sequencer on=1|Sequencer steps=0.032|Step 2=1"},
    {"gl.hexsynth", "Mod 1 target=1|Mod 1 amount=1"},
    {"gl.hexsynth", "Mod 1 source=1|Mod 1 amount=1"},
    {"gl.hexsynth", "Mod 1 source=1|Mod 1 target=1"},
    {"gl.hexsynth", "Osc 2 on=1|Osc 2 level=1"},
    {"gl.hexsynth", "Osc 2 on=1"},
    {"gl.hexsynth", "Osc 2 on=1|Osc 2 level=1|Osc 2 free run=0"},
    {"gl.hexsynth", "Osc 2 on=1|Osc 2 level=1|Osc 1 to 2=1"},
    {"gl.hexsynth", "Osc 1 free run=0"},
    {"gl.hexsynth", "Osc 1 sustain=0"},
    {"gl.hexsynth", "Osc 6 on=1|Osc 6 level=1"},
    {"gl.hexsynth", "Osc 1 LFO 1=1"},
    {"gl.dcosynth", "Matrix 1 source=5|Matrix 1 target=156|Matrix 1 amount=1"},
    {"gl.dcosynth", "Matrix 1 source=5|Matrix 1 target=156"},
    {"gl.dcosynth", "Matrix 1 source=7|Matrix 1 target=156|Matrix 1 amount=1"},
    {"gl.dcosynth", "Chorus=1|Chorus noise=1"},
    {"gl.dcosynth", "Mono=1|VCA=0"},
    {"gl.dcosynth", "Mono=1|Legato=1|VCA=0"},
    {"gl.dcosynth", "Arpeggiator=1|Arp mode=2"},
    {"gl.dcosynth", "EQ=1|EQ low gain=1"},
    {"gl.dcosynth", "EQ=1|EQ mid gain=1"},
    {"gl.dcosynth", "EQ=1|EQ high gain=1"},
    {"gl.dcosynth", "Saturation=1|Saturation amount=1"},
    {"gl.warmsynth", "LFO=1|Retrigger=1|Osc 1 LFO=1"},
    {"gl.wavetable", "Mod 2 source=4|Mod 2 target=12|Mod 2 amount=1"},
    {"gl.wavetable", "Mod 2 target=12|Mod 2 amount=1"},
    {"gl.wavetable", "Mod 2 source=4|Mod 2 amount=1"},
    {"gl.wavetable", "Mod 2 source=4|Mod 2 target=12"},
    {"gl.wavetable", "Osc B on=1|Osc B warp amount=1"},
    {"gl.wavetable", "Osc B on=1|Osc B warp=3"},
    {"gl.wavetable", "Osc B on=1|Osc B unison=4"},
    {"gl.wavetable", "Osc B on=1|Osc B random phase=0"},
    {"gl.wavetable", "Mod 1 source=2|Mod 1 amount=1"},
    {"gl.wavetable", "Mod 1 source=7|Mod 1 amount=1"},
    {"gl.wavetable", "Mod 1 source=7|Mod 1 amount=1|LFO 4 tempo sync=1"},
    {"gl.wavetable", "Mod 1 source=13|Mod 1 amount=1"},
    {"gl.wavetable", "Delay=1|Delay time=0"},
    {"gl.gms", "DIST on=1|DIST LFO amount=1"},
    {"gl.gms", "CRUSH on=1|CRUSH LFO amount=1"},
    {"gl.gms", "FLANGE on=1|FLANGE LFO amount=1"},
    {"gl.gms", "LOW PASS on=1|LOW PASS LFO amount=1"},
    {"gl.gms", "HIGH PASS on=1|HIGH PASS LFO amount=1"},
    {"gl.gms", "REVERB on=1|REVERB LFO amount=1"},
    {"gl.gms", "DELAY on=1|DELAY Feedback=1"},
    {"gl.gms", "DELAY on=1|DELAY LFO amount=1"},
    {"gl.gms", "GATE on=1|GATE LFO amount=1"},
    {"gl.sawswarm", "Mod 1 source=5|Mod 1 amount=1"},
    {"gl.sawswarm", "Mod 1 source=5"},
    {"gl.sawswarm", "Filter env amount=1|Cutoff=0|Filter sustain=0"},
    {"gl.sawswarm", "Delay=1|Delay left=0|Delay right=0"},
    {"gl.sawswarm", "Arpeggiator=1|Arp mode=1"},
    {"gl.twinva", "Mono=1|Arpeggiator=1|Glide=0.5"},
    {"gl.analog", "Delay=1|Delay time=2"},
    {"gl.acid", "Sequencer=1|Step 12 gate=1"},
    {"gl.pocketrack", "Slot 1 module=36"},
    {"gl.pocketrack", "Slot 1 module=36|Slot 1 control 27=1"},
    {"gl.harmor", "Delay=1|Delay time=0.5"},
    {"gl.harmor", "Chorus voices=3"},
    {"gl.harmor", "A unison=4"},
    {"gl.harmor", "A F2 shape=0|A F2 freq=0.6|A F2 reso=1"},
    {"gl.harmor", "B on=1"},
    {"gl.harmor", "B on=1|B blend=1"},
    {"gl.harmor", "B on=1|B tremolo=1"},
    {"gl.harmor", "B on=1|B harmonic blur=8"},
    {"gl.harmor", "B on=1|B harmonizer=1"},
    {"gl.harmor", "B on=1|B unison=4"},
    {"gl.harmor", "B on=1|B vibrato=50"},
    {"gl.harmor", "B on=1|B F1 freq=0.6|B F1 reso=1"},
    {"gl.harmor", "B on=1|B F2 shape=0|B F2 freq=0.6|B F2 reso=1"},
    {"gl.harmor", "B on=1|B phaser=1"},
    {"gl.enhance", "Program=1|Program: Low engine=1|Program: Low pre gain=24"},
    {"gl.enhance", "Program=1|Program: Low engine=1|Program: Low pre gain=24|Program: Low point 3 y=1"},
    {"gl.enhance", "Program=1|Program: Mid engine=1|Program: Mid pre gain=24|Program: Mid point 3 y=1"},
    {"gl.enhance", "Program=1|Program: High engine=1|Program: High pre gain=24"},
    {"gl.enhance", "Program=1|Program: High engine=1|Program: High pre gain=24|Program: High point 3 y=1"},
    {"gl.enhance", "Program=1|Program: Mid engine=1|Program: Mid pre gain=24|Program: Mid curve points=4|Program: Mid point 3 x=0.75"},
    {"gl.enhance", "Program=1|Program: Master engine=1|Program: Master pre gain=24|Program: Master curve points=4|Program: Master point 3 x=0.75"},
    {"gl.enhance", "Program=1|Program: Master engine=1|Program: Master pre gain=24|Program: Master curve points=4|Program: Master point 3 x=0.75|Program: Master point 4 y=1"},
    {"gl.enhance", "Program=1|Program: Low engine=1|Program: Low saturation=-24"},
    {"gl.enhance", "Program=1|Program: Mid engine=1|Program: Mid saturation=-24"},
    {"gl.enhance", "Program=1|Program: High engine=1|Program: High saturation=-24"},
    {"gl.enhance", "Program=3|Low pre gain (ref.)=2000|Program: Low point 3 y=1"},
    {"gl.enhance", "Program=3|Mid pre gain (ref.)=2000|Program: Mid point 3 y=1"},
    {"gl.enhance", "Program=3|High pre gain (ref.)=2000|Program: High point 3 y=1"},
    {"gl.enhance", "Program=3|Master pre gain (ref.)=2000|Program: Master curve points=4|Program: Master point 3 x=0.75|Program: Master point 4 y=1"},
    {"gl.enhance", "Program=3|Low saturation (ref.)=-500"},
    {"gl.enhance", "Program=3|Mid saturation (ref.)=-500"},
    {"gl.enhance", "Program=3|High saturation (ref.)=-500"},
    {"gl.enhance", "Program=3|Master saturation (ref.)=-500"},
    {"gl.enhance", "Program=2|Input gain=12"},
    {"gl.master", "Low engine=1|Low pre gain=24"},
    {"gl.master", "Low engine=1|Low pre gain=24|Low point 3 y=1"},
    {"gl.master", "High engine=1|High pre gain=24|High point 3 y=1"},
    {"gl.master", "Mid engine=1|Mid pre gain=24|Mid point 3 y=1"},
    {"gl.master", "Mid engine=1|Mid pre gain=24|Mid curve points=4|Mid point 3 x=0.75"},
    {"gl.master", "Mid engine=1|Mid pre gain=24|Mid curve points=4|Mid point 3 x=0.75|Mid point 4 y=1"},
    {"gl.master", "High engine=1|High pre gain=24|High curve points=4|High point 3 x=0.75"},
    {"gl.master", "Master engine=1|Master pre gain=24|Master curve points=4|Master point 3 x=0.75"},
    {"gl.master", "Master engine=1|Master pre gain=24|Master curve points=4|Master point 3 x=0.75|Master point 4 y=1"},
    {"gl.master", "Engine=1|Low pre gain (ref.)=2000|Low point 3 y=1"},
    {"gl.master", "Engine=1|Mid pre gain (ref.)=2000|Mid point 3 y=1"},
    {"gl.master", "Engine=1|High pre gain (ref.)=2000|High point 3 y=1"},
    {"gl.master", "Engine=1|Master pre gain (ref.)=2000|Master curve points=4|Master point 3 x=0.75|Master point 4 y=1"},
    {"gl.master", "Engine=1|Low saturation (ref.)=-500"},
    {"gl.master", "Engine=1|Mid saturation (ref.)=-500"},
    {"gl.master", "Engine=1|High saturation (ref.)=-500"},
    {"gl.master", "Engine=1|Master saturation (ref.)=-500"},
    {"gl.master", "Engine=1|Solo (ref.)=1"},
    {"gl.master", "Engine=1|Linear-phase split (ref.)=1"},
    {"gl.pads", "Pad 1 sustain=0|Pad 1 decay=0.05"},
    {"gl.pads", "Pad 1 hold=0|Pad 1 decay=0.05"},
    {"gl.pads", "Pad 1 hold=0|Pad 1 sustain=0"},
    {"gl.pads", "Pad 1 one shot=0"},
    {"gl.pads", "Pad 2 note=60"},
    {"gl.pads", "Pad 11 note=60"},
    {"gl.pads", "Pad 11 note=60|Pad 11 layer 2 on=1"},
    {"gl.pads", "Pad 23 layer 2 on=1"},
    {"gl.slicer", "Slicing=4|Slice 26 cut group=99"},
    {"gl.slicer", "Engine=2|Slice 1 position=0|Processed pitch=600"},
    {"gl.slicer", "Engine=2|Slice 1 position=0|Slice 2 position=0.25"},
    {"gl.slicer", "Engine=2|Slice 1 position=0|Slice 2 position=0.25|Hold=1"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 algorithm=1"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 algorithm=2"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 algorithm=3"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 algorithm=14"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 algorithm=15"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 algorithm=16|Module 1 p33=1"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 algorithm=17"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 kind=1|Module 1 filter type=2|Module 1 p39=5000"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 kind=2"},
    {"gl.amprack", "Rack model=1|Modules=4|Module 1 kind=3"},
    {"gl.amprack", "Rack model=2|Board value 2=1|Board value 3=1"},
    {"gl.amprack", "Rack model=2|Board value 46=1|Board value 47=1|Board value 48=1|Board value 49=1"},
    {"gl.membranekit", "Ch 1 mute=0|Ch 1 Velocity 2 to=7"},
    {"gl.membranekit", "Ch 1 mute=0|Ch 1 Velocity 2 amount=1"},
    {"gl.membranekit", "EQ band 1=1"},
    {"gl.membranekit", "Limiter gain=1"},
    {"gl.membranekit", "Limiter on=1|Limiter gain=1"},
    {"gl.drumkit", "Pad 10 bend=12"},
    {"gl.grainscan", "Key to=3"},
    {"gl.delay", "Delay model=0|Distortion=0"},
    {"gl.eq", "Band model=0|Band 2 gain=12"},
    {"gl.bbdchorus", "Chorus model=0|Mode=3|Tempo sync=1"},
    {"gl.drawneq", "A 1.2 kHz=18"},
    {"gl.limiter", "Comp threshold=-30|Comp ratio=4"},
    {"gl.transplit", "Engine=1|Ref Transient effect=4|Ref T delay sync=1"},
    {"gl.transplit", "Engine=1|Ref Sustain effect=4|Ref S delay sync=1"},
    {"gl.hall", "Shimmer on=1|Shimmer mode=1|Voice 1 level=1"},
    {"gl.hall", "Shimmer on=1|Shimmer mode=1|Voice 1 level=1|Voice 1 delay sync=1"},
    {"gl.hall", "Envelope target=1|Env scale=1"},
    {"gl.multidelay", "Tap 3 on=1|Tap 3 tempo time=0"},
    {"gl.multidelay", "Tap 6 on=1|Tap 6 time (steps)=2"},
    {"gl.multidelay", "Tap 3 on=1"},
    {"gl.multidelay", "Tap 3 on=1|Tap 3 input filter=1"},
    {"gl.multidelay", "Tap 3 on=1|Tap 3 input filter=6"},
    {"gl.multidelay", "Tap 3 on=1|Tap 3 feedback filter=6"},
    {"gl.multidelay", "Tap 3 on=1|Tap 3 mod depth=0.5"},
    {"gl.multidelay", "Tap 3 on=1|Tap 3 gate division=4"},
    {"gl.multidelay", "Tap 3 on=1|Tap 4 on=1"},
    {"gl.pitcher", "Scale=9"},
    {"gl.pitcher", "Scale=9|Note C#=0|Note D=0|Note D#=0|Note E=0|Note F=0|Note F#=0|Note G=0|Note G#=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note D=0|Note D#=0|Note E=0|Note F=0|Note F#=0|Note G=0|Note G#=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D#=0|Note E=0|Note F=0|Note F#=0|Note G=0|Note G#=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D=0|Note E=0|Note F=0|Note F#=0|Note G=0|Note G#=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D=0|Note D#=0|Note E=0|Note F#=0|Note G=0|Note G#=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D=0|Note D#=0|Note E=0|Note F=0|Note G=0|Note G#=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D=0|Note D#=0|Note E=0|Note F=0|Note F#=0|Note G#=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D=0|Note D#=0|Note E=0|Note F=0|Note F#=0|Note G=0|Note A=0|Note A#=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D=0|Note D#=0|Note E=0|Note F=0|Note F#=0|Note G=0|Note G#=0|Note A=0|Note B=0"},
    {"gl.pitcher", "Scale=9|Note C=0|Note C#=0|Note D=0|Note D#=0|Note E=0|Note F=0|Note F#=0|Note G=0|Note G#=0|Note A=0|Note A#=0"},
    {"gl.envctl", "Env A point 5 level=1|Engine=0|Env A points=6|Env A sustain=0"},
    {"gl.envctl", "Env A point 6 level=1|Engine=0|Env A points=6|Env A sustain=0"},
    {"gl.envctl", "Engine=0|Env A points=6|Env A sustain=0"},
    {"gl.envctl", "Engine=0|Env A LFO sync=0|Env A LFO=1"},
    {"gl.notesq", "Rate=5"},
    {"gl.sampler", "One shot=0|Filter=1|Filter env=-1|Filter sustain=1|Release=8|Loop=1"},
    {"gl.sampler", "LFO target=4|LFO amount=1"},
    {"gl.sampler", "LFO target=4"},
    {"gl.matrixfm", "Op 1 PITCH envelope=1"},
    {"gl.matrixfm", "Op 1 PITCH LFO=1"},
    {"gl.matrixfm", "Op 1 PAN envelope=1"},
    {"gl.matrixfm", "Op 1 PAN LFO=1"},
    {"gl.matrixfm", "Op 1 MOD envelope=1|Matrix FM op 1 to op 1=1"},
    {"gl.matrixfm", "Op 1 MOD LFO=1|Matrix FM op 1 to op 1=1"},
    {"gl.matrixfm", "Op 2 PITCH envelope=1|Matrix op 2 output=1"},
    {"gl.matrixfm", "Op 2 PAN envelope=1|Matrix op 2 output=1"},
    {"gl.matrixfm", "Op 2 MOD envelope=1|Matrix op 2 output=1|Matrix FM op 2 to op 2=1"},
    {"gl.matrixfm", "Matrix op 2 output=1"},
    {"gl.matrixfm", "Matrix RM op 2 to op 1=1"},
};

// Glob: '*' matches any run of characters.
bool globMatch(const char* p, const char* s) {
  if (*p == 0) return *s == 0;
  if (*p == '*') return globMatch(p + 1, s) || (*s && globMatch(p, s + 1));
  return *s && *p == *s && globMatch(p + 1, s + 1);
}
bool nameMatches(const char* pat, const std::string& name) { return globMatch(pat, name.c_str()); }
}  // namespace

const char* inertReason(const std::string& type, const std::string& paramName) {
  for (const InertDoc& d : kInert)
    if ((std::strcmp(d.type, "*") == 0 || type == d.type) && nameMatches(d.param, paramName)) return d.reason;
  return nullptr;
}

namespace {

constexpr double kSr = 44100;
constexpr int kBlock = 256;
constexpr int kPeriod = 104 * kBlock;  // 0.60 s
constexpr double kPi = 3.141592653589793;
constexpr double kNone = -300;

// ------------------------------------------------------------- stimulus

struct NoteE {
  int at;
  int kind;  // 0 on, 1 off, 2 glide
  uint32_t id;
  double pitch;
  float vel;
  NoteProps props;
};

struct Stim {
  std::vector<float> inL, inR, scL, scR, rcL, rcR;  // two periods
  std::vector<NoteE> notes;
};

uint32_t xs(uint32_t& s) {
  s ^= s << 13, s ^= s >> 17, s ^= s << 5;
  return s;
}
double nz(uint32_t& s) { return static_cast<double>(static_cast<int32_t>(xs(s))) / 2147483648.0; }

Stim makeStim() {
  Stim S;
  const int P = kPeriod, T = 2 * P;
  for (auto* v : {&S.inL, &S.inR, &S.scL, &S.scR, &S.rcL, &S.rcR}) v->assign(static_cast<size_t>(T), 0.f);
  for (int per = 0; per < 2; ++per) {
    uint32_t sl = 0x1234567u, sr = 0x7654321u, sh = 0x2468ACEu;
    for (int i = 0; i < P; ++i) {
      const double f = static_cast<double>(i) / P;
      const double t = static_cast<double>(i) / kSr;
      double l = 0, r = 0;
      const double nl = nz(sl), nr = nz(sr), nh = nz(sh);
      if (f < 0.25) {  // noise burst with a soft ramp
        const double env = std::min(1.0, f / 0.02) * (0.35 + 0.15 * std::sin(2 * kPi * 6 * t));
        l = env * nl, r = env * (0.7 * nl + 0.3 * nr);
      } else if (f < 0.65) {  // drum loop: kick / snare / hats
        const double g = f - 0.25;
        auto hit = [&](double at) { return g >= at ? (g - at) * P / kSr : -1.0; };
        for (double at : {0.0, 0.2}) {  // kick
          const double u = hit(at);
          if (u >= 0 && u < 0.18) {
            const double ph = 2 * kPi * (50 * u + 9 * (1 - std::exp(-u * 30)));
            const double k = 0.9 * std::sin(ph) * std::exp(-u * 18);
            l += k, r += k;
          }
        }
        for (double at : {0.1, 0.3}) {  // snare
          const double u = hit(at);
          if (u >= 0 && u < 0.15) {
            const double s = std::exp(-u * 25) * (0.45 * nl + 0.3 * std::sin(2 * kPi * 190 * u));
            l += s, r += 0.8 * s + 0.2 * std::exp(-u * 25) * nr;
          }
        }
        for (int h = 0; h < 8; ++h) {  // hats
          const double u = hit(0.05 * h);
          if (u >= 0 && u < 0.04) {
            const double hh = 0.25 * std::exp(-u * 120) * (nh - 0.6 * nl);
            l += hh * (h % 2 ? 0.6 : 1.0), r += hh * (h % 2 ? 1.0 : 0.6);
          }
        }
      } else if (f < 0.9) {  // tone with harmonics
        const double u = (f - 0.65) * P / kSr;
        const double env = std::min(1.0, u / 0.005) * (f > 0.85 ? (0.9 - f) / 0.05 : 1.0);
        double a = 0, b = 0;
        for (int h = 1; h <= 5; ++h) {
          a += 0.32 / h * std::sin(2 * kPi * 220 * h * u);
          b += 0.30 / h * std::sin(2 * kPi * 330 * h * u + 0.3 * h);
        }
        l = env * a, r = env * (0.6 * a + 0.4 * b);
      }
      const size_t k = static_cast<size_t>(per * P + i);
      S.inL[k] = static_cast<float>(l), S.inR[k] = static_cast<float>(r);
      // Sidechain: a kick every eighth of the period; return bus: a 440 Hz tone.
      const double ku = std::fmod(f, 0.125) * P / kSr;
      const float sc = ku < 0.12 ? static_cast<float>(0.8 * std::sin(2 * kPi * 60 * ku) * std::exp(-ku * 25)) : 0.f;
      S.scL[k] = S.scR[k] = sc;
      S.rcL[k] = static_cast<float>(0.3 * std::sin(2 * kPi * 440 * t));
      S.rcR[k] = static_cast<float>(0.3 * std::sin(2 * kPi * 440 * t + 0.5));
    }
    // Notes: chord, bass with a glide, high notes at low / high velocity.
    const int o = per * P;
    const uint32_t b = static_cast<uint32_t>(per * 100);
    auto on = [&](double at, uint32_t id, double pitch, float vel, NoteProps pr = {}) {
      S.notes.push_back({o + static_cast<int>(at * P), 0, b + id, pitch, vel, pr});
    };
    auto off = [&](double at, uint32_t id) { S.notes.push_back({o + static_cast<int>(at * P), 1, b + id, 0, 0, {}}); };
    NoteProps pr;
    pr.pan = -0.4f, pr.modX = 0.8f, pr.modY = 0.25f, pr.release = 0.7f;
    on(0.0, 1, 60, 0.8f);
    on(0.02, 2, 64, 0.45f);
    on(0.045, 3, 67, 1.0f, pr);
    off(0.28, 1), off(0.28, 2), off(0.30, 3);
    on(0.31, 4, 36, 0.65f);
    S.notes.push_back({o + static_cast<int>(0.38 * P), 2, b + 4, 43, 0, {}});  // glide
    off(0.45, 4);
    on(0.46, 5, 84, 0.25f);
    on(0.47, 6, 72, 0.95f);
    off(0.56, 5), off(0.56, 6);
    // Controllers from a MIDI keyboard: pitch bend, mod wheel (CC 1), channel pressure, CC 74.
    auto midi = [&](double at, int st, int d1, int d2) {
      S.notes.push_back({o + static_cast<int>(at * P), 3, static_cast<uint32_t>(st | (d1 << 8) | (d2 << 16)), 0, 0, {}});
    };
    midi(0.08, 0xE0, 0, 0x60), midi(0.12, 0xB0, 1, 110), midi(0.16, 0xD0, 90, 0), midi(0.18, 0xB0, 74, 20);
    midi(0.26, 0xE0, 0, 0x40), midi(0.27, 0xB0, 1, 0), midi(0.27, 0xD0, 0, 0), midi(0.27, 0xB0, 74, 64);
    // Every key once (pads, slices, drum voices, key zones), velocities 0.3 / 1.0 / 0.6.
    for (int k = 0; k < 128; ++k) {
      const double at = 0.58 + 0.25 * k / 128.0;
      on(at, 10 + static_cast<uint32_t>(k), k, k % 3 == 0 ? 0.3f : k % 3 == 1 ? 1.0f : 0.6f);
      off(at + 0.015, 10 + static_cast<uint32_t>(k));
    }
  }
  std::stable_sort(S.notes.begin(), S.notes.end(), [](const NoteE& x, const NoteE& y) { return x.at < y.at; });
  return S;
}
const Stim& stim() {
  static const Stim s = makeStim();
  return s;
}

// ------------------------------------------------------------- rendering

struct Out {
  std::vector<float> L, R, X;  // X: aux / send outputs, control outputs, note events
  bool finite = true;
  double peak = 0;
};

enum class Kind { Gen, Fx, NoteFx };
Kind kindOf(const PluginInfo& info) {
  return info.kind == PluginKind::Generator ? Kind::Gen : info.kind == PluginKind::Effect ? Kind::Fx : Kind::NoteFx;
}

struct RenderSpec {
  const PluginState* state = nullptr;   // from the start
  const PluginState* prep = nullptr;    // what prepareHost sees (null = state)
  const PluginState* second = nullptr;  // live switch at the second period (null = none)
  int periods = 1;
  bool sidechain = false;
};

void finish(Out& o) {
  for (const auto* v : {&o.L, &o.R, &o.X})
    for (float x : *v) {
      if (!std::isfinite(x)) {
        o.finite = false;
        continue;
      }
      if (v != &o.X) o.peak = std::max(o.peak, static_cast<double>(std::fabs(x)));  // audio only (X holds events / controls)
    }
}

thread_local const PluginState* crashState = nullptr;  // writeActive: the state being rendered

Out render(const PluginInfo& info, const RenderSpec& rs) {
  crashState = rs.second ? rs.second : rs.state;  // (a live switch: the state after it)
  const Stim& S = stim();
  const int total = kPeriod * rs.periods;
  Out o;
  PluginState cur = *rs.state;
  const PluginState& prep = rs.prep ? *rs.prep : cur;
  ProcessContext c;
  c.sampleRate = kSr, c.tempo = 128, c.ppq = 96, c.playing = true, c.offline = true;
  const Kind kind = kindOf(info);
  float co[32];
  if (kind == Kind::Gen) {
    auto g = createGenerator(cur.type);
    o.L.assign(static_cast<size_t>(total), 0.f), o.R.assign(static_cast<size_t>(total), 0.f);
    if (!g) return o;
    ChannelAux aux;
    prepareChannelAux(cur, aux);
    g->prepareHost(prep, kSr);
    const bool glide = g->supportsGlide();
    auto ring = std::make_unique<MidiOutRing>();  // MIDI Out channels: their messages count as output
    g->setMidiOut(ring.get());
    Generator::MidiOutEvent mo[64];
    size_t ei = 0;
    for (int pos = 0; pos < total; pos += kBlock) {
      const int n = std::min(kBlock, total - pos);
      if (rs.second && pos == kPeriod) cur = *rs.second;
      c.tick = pos * (c.tempo * c.ppq / 60.0) / kSr;
      c.jumped = pos == 0;
      while (ei < S.notes.size() && S.notes[ei].at < pos + n) {
        const NoteE& e = S.notes[ei++];
        if (e.at >= total) continue;
        if (e.kind == 0) g->noteOn(e.id, e.pitch, e.vel, e.at - pos, e.props);
        else if (e.kind == 1) g->noteOff(e.id, e.at - pos);
        else if (e.kind == 3) g->midi(static_cast<uint8_t>(e.id), static_cast<uint8_t>(e.id >> 8), static_cast<uint8_t>(e.id >> 16), e.at - pos);
        else if (glide) g->glide(e.id, e.pitch, 0.05, e.at - pos);
      }
      g->process(cur, aux, c, o.L.data() + pos, o.R.data() + pos, n);
      for (int k = 0; k < g->auxOutputCount(); ++k) {
        const float *l = nullptr, *r = nullptr;
        if (g->auxOutput(k, l, r) && l && r)
          for (int i = 0; i < n; i += 4) o.X.push_back(l[i] + r[i]);
      }
      const int m = g->controlOutputs(co, 32);
      for (int k = 0; k < m; ++k) o.X.push_back(co[k]);
      for (MidiOutRing::Msg msg; ring->pop(msg);) o.X.insert(o.X.end(), {static_cast<float>(pos / 441.0), msg.s * 0.1f, msg.a * 0.1f, msg.b * 0.1f});
      for (int k = 0, t = g->takeMidiOut(mo, 64); k < t; ++k)
        o.X.insert(o.X.end(), {static_cast<float>((pos + mo[k].offset) / 441.0), mo[k].s * 0.1f, mo[k].a * 0.1f, mo[k].b * 0.1f});
    }
  } else if (kind == Kind::Fx) {
    auto fx = createEffect(cur.type);
    o.L.assign(S.inL.begin(), S.inL.begin() + total), o.R.assign(S.inR.begin(), S.inR.begin() + total);
    if (!fx) return o;
    ChannelAux aux;
    packs::prepareAux(cur, aux);
    fx->prepareHost(prep, kSr);
    std::vector<float> sink(static_cast<size_t>(kBlock) * 2 * (kMaxSendOutputs + 1), 0.f);
    const size_t nsend = std::min<size_t>(info.sendParams.size(), kMaxSendOutputs);
    for (int pos = 0; pos < total; pos += kBlock) {
      const int n = std::min(kBlock, total - pos);
      if (rs.second && pos == kPeriod) cur = *rs.second;
      c.tick = pos * (c.tempo * c.ppq / 60.0) / kSr;
      c.jumped = pos == 0;
      std::fill(sink.begin(), sink.end(), 0.f);
      SideIo io;
      if (rs.sidechain) io.scL = S.scL.data() + pos, io.scR = S.scR.data() + pos;
      io.recvL = S.rcL.data() + pos, io.recvR = S.rcR.data() + pos;
      io.sendL = sink.data(), io.sendR = sink.data() + kBlock;
      for (size_t k = 0; k < nsend; ++k)
        io.multiL[k] = sink.data() + (2 + 2 * k) * kBlock, io.multiR[k] = sink.data() + (3 + 2 * k) * kBlock;
      fx->setSideIo(io);
      fx->processAux(cur, aux, c, o.L.data() + pos, o.R.data() + pos, n);
      for (size_t k = 0; k < 2 + 2 * nsend; ++k)
        for (int i = 0; i < n; i += 4) o.X.push_back(sink[k * kBlock + static_cast<size_t>(i)]);
      const int m = fx->controlOutputs(co, 32);
      for (int k = 0; k < m; ++k) o.X.push_back(co[k]);
    }
  } else {
    auto fx = createNoteFx(cur.type);
    if (!fx) return o;
    NoteFxAux aux;
    prepareNoteFxAux(cur, aux);
    uint32_t ids = 1;
    NoteBuffer in, res;
    size_t ei = 0;
    for (int pos = 0; pos < total; pos += kBlock) {
      const int n = std::min(kBlock, total - pos);
      if (rs.second && pos == kPeriod) cur = *rs.second;
      in.clear(), res.clear();
      while (ei < S.notes.size() && S.notes[ei].at < pos + n) {
        const NoteE& e = S.notes[ei++];
        if (e.at >= total || e.kind >= 2) continue;
        NoteMsg m;
        m.kind = e.kind == 0 ? NoteMsg::On : NoteMsg::Off;
        m.offset = e.at - pos, m.id = e.id, m.pitch = static_cast<float>(e.pitch), m.velocity = e.vel;
        m.pan = e.props.pan, m.release = e.props.release, m.modX = e.props.modX, m.modY = e.props.modY;
        in.push(m);
      }
      in.sort();
      NoteFxContext ctx;
      ctx.sampleRate = kSr, ctx.tempo = 128, ctx.ppq = 96, ctx.from = 0, ctx.len = n, ctx.anchorTick = 0, ctx.clock = pos;
      ctx.spt = kSr * 60.0 / (ctx.tempo * ctx.ppq), ctx.playing = true, ctx.idCounter = &ids;
      fx->process(cur, aux, ctx, in, res);
      res.sort();
      for (int i = 0; i < res.count; ++i) {
        const NoteMsg& m = res.ev[i];
        // Events as numbers (positions scaled so a shift of a few ms counts like a pitch step).
        o.X.push_back(static_cast<float>((pos + m.offset) / 441.0));
        o.X.push_back(static_cast<float>(m.kind));
        o.X.push_back(m.pitch);
        o.X.push_back(m.velocity * 10.f);
        o.X.push_back(m.target);
        o.X.push_back(m.pan * 10.f);
        o.X.push_back(m.release * 10.f);
        o.X.push_back(m.modX * 10.f);
        o.X.push_back(m.modY * 10.f);
        o.X.push_back(m.color);
      }
    }
  }
  finish(o);
  return o;
}

double sumSq(const std::vector<float>& v, size_t from = 0) {
  double s = 0;
  for (size_t i = from; i < v.size(); ++i) s += static_cast<double>(v[i]) * v[i];
  return s;
}

// RMS of (a - ref) relative to ref (dB). Audio from frame `from` on; X compared whole.
double diffDb(const Out& a, const Out& ref, size_t from = 0) {
  if (a.L.size() != ref.L.size()) return 0;
  double num = 0, den = 0;
  for (size_t i = from; i < a.L.size(); ++i) {
    const double dl = static_cast<double>(a.L[i]) - ref.L[i], dr = static_cast<double>(a.R[i]) - ref.R[i];
    if (std::isfinite(dl) && std::isfinite(dr)) num += dl * dl + dr * dr;
    else num += 1e6;
    den += static_cast<double>(ref.L[i]) * ref.L[i] + static_cast<double>(ref.R[i]) * ref.R[i];
  }
  if (a.X.size() != ref.X.size()) return 0;  // different event counts / outputs: fully different
  for (size_t i = 0; i < a.X.size(); ++i) {
    const double d = static_cast<double>(a.X[i]) - ref.X[i];
    num += std::isfinite(d) ? d * d * 64 : 1e6;  // control outputs / events count per sample block
    den += static_cast<double>(ref.X[i]) * ref.X[i] * 64;
  }
  const size_t n = (a.L.size() - from) * 2 + a.X.size() * 64;
  den = std::max(den, static_cast<double>(std::max<size_t>(n, 1)) * 1e-10);  // -100 dBFS floor
  return num <= 0 ? kNone : 10.0 * std::log10(num / den);
}

double levelDb(const Out& o) {
  const double s = sumSq(o.L) + sumSq(o.R);
  const size_t n = o.L.size() * 2;
  return n && s > 0 ? 10.0 * std::log10(s / static_cast<double>(n)) : kNone;
}
double brightDb(const Out& o) {
  double d = 0, s = 0;
  for (const auto* v : {&o.L, &o.R})
    for (size_t i = 1; i < v->size(); ++i) {
      const double x = (*v)[i], y = x - (*v)[i - 1];
      d += y * y, s += x * x;
    }
  return s > 1e-12 && d > 0 ? 10.0 * std::log10(d / s) : kNone;
}

// ------------------------------------------------------------- values

bool sameValue(const ParamDesc& d, double a, double b) {
  const double span = std::max(1e-9, std::fabs(d.max - d.min));
  return std::fabs(d.clamp(a) - d.clamp(b)) <= span * 1e-6;
}

std::vector<double> testValues(const ParamDesc& d, double base) {
  std::vector<double> v;
  auto add = [&](double x) {
    x = d.clamp(x);
    if (sameValue(d, x, base)) return;
    for (double y : v)
      if (sameValue(d, x, y)) return;
    v.push_back(x);
  };
  if (d.kind == ParamKind::Toggle) {
    add(base >= 0.5 ? 0.0 : 1.0);
  } else if (d.kind == ParamKind::Choice) {
    const int n = d.choices.empty() ? static_cast<int>(d.max) + 1 : static_cast<int>(d.choices.size());
    if (n <= 32) {
      for (int i = 0; i < n; ++i) add(i);
    } else {
      for (int i = 0; i < 32; ++i) add(std::round(static_cast<double>(i) * (n - 1) / 31.0));
    }
  } else {
    add(d.fromNormalized(0.0));
    add(d.fromNormalized(1.0));
    add(d.fromNormalized(0.5));
    const double nb = d.toNormalized(base);
    if (nb < 0.02 || nb > 0.98) add(d.fromNormalized(0.25)), add(d.fromNormalized(0.75));
  }
  return v;
}

// One or two values for the condition search (farthest from the base value).
std::vector<double> searchValues(const ParamDesc& d, double base) {
  if (d.kind == ParamKind::Toggle) return {base >= 0.5 ? 0.0 : 1.0};
  std::vector<double> all = testValues(d, base);
  if (all.size() <= 2) return all;
  if (d.kind == ParamKind::Choice) return {all.front(), all.back()};
  const double nb = d.toNormalized(base);
  std::sort(all.begin(), all.end(), [&](double x, double y) { return std::fabs(d.toNormalized(x) - nb) > std::fabs(d.toNormalized(y) - nb); });
  // The farthest value first, then the middle (cyclic parameters: a phase at 100% equals 0%).
  const double mid = d.clamp(d.fromNormalized(0.5));
  if (!sameValue(d, mid, all.front()) && !sameValue(d, mid, base)) return {all.front(), mid};
  return {all.front()};
}

std::string fmtValue(const ParamDesc& d, double v) {
  std::string s = d.format(v);
  for (char& c : s)
    if (c == '|' || c == '\t' || c == '\n') c = '/';
  return s;
}

bool isRouting(const PluginInfo& info, int k) {
  if (k == info.sideInParam || k == info.routeModeParam) return true;
  return std::find(info.sendParams.begin(), info.sendParams.end(), k) != info.sendParams.end();
}

// A base for the condition search: parameter changes on top of the defaults
// (-1 = sidechain connected; kLive + k = parameter k changed live at the
// second period, for parameters that act on changes: smoothing, glide, keep
// pitch, MIDI controller messages).
constexpr int kLive = 1 << 20;
struct Base {
  std::vector<std::pair<int, double>> set;
  std::string text;
};

std::vector<Base> searchBases(const PluginInfo& info, const PluginState& def, int budget) {
  std::vector<Base> toggles, choices, conts, all;
  const int n = static_cast<int>(info.params.size());
  if (info.kind == PluginKind::Effect) all.push_back({{{-1, 1.0}}, "sidechain input connected"});
  for (int j = 0; j < n; ++j) {
    if (isRouting(info, j)) continue;
    const ParamDesc& d = info.params[static_cast<size_t>(j)];
    const double b = def.params[static_cast<size_t>(j)];
    if (d.kind == ParamKind::Toggle) {
      toggles.push_back({{{j, b >= 0.5 ? 0.0 : 1.0}}, d.name + " = " + fmtValue(d, b >= 0.5 ? 0.0 : 1.0)});
    } else if (d.kind == ParamKind::Choice) {
      std::vector<double> vs = testValues(d, b);
      if (vs.size() > 12) {
        std::vector<double> few;
        for (size_t i = 0; i < 12; ++i) few.push_back(vs[i * (vs.size() - 1) / 11]);
        vs = few;
      }
      for (double v : vs) choices.push_back({{{j, v}}, d.name + " = " + fmtValue(d, v)});
    } else {
      for (double nn : {1.0, 0.0}) {
        const double v = d.fromNormalized(nn);
        if (!sameValue(d, v, b)) conts.push_back({{{j, v}}, d.name + " = " + fmtValue(d, v)});
      }
    }
  }
  // Switches that enable a section first.
  std::stable_partition(toggles.begin(), toggles.end(), [](const Base& b) {
    std::string t = b.text;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* w : {" on =", "enabled", "active", "mute", "solo", "bypass"})
      if (t.find(w) != std::string::npos) return true;
    return false;
  });
  for (auto* g : {&toggles, &choices, &conts}) all.insert(all.end(), g->begin(), g->end());
  // Several switches at once.
  Base on{{}, "every switch on"}, onKnobs{{}, "every switch on, knobs at 75%"};
  for (int j = 0; j < n; ++j) {
    if (isRouting(info, j)) continue;
    const ParamDesc& d = info.params[static_cast<size_t>(j)];
    if (d.kind == ParamKind::Toggle) on.set.push_back({j, 1.0}), onKnobs.set.push_back({j, 1.0});
    else if (d.kind == ParamKind::Continuous) onKnobs.set.push_back({j, d.fromNormalized(0.75)});
  }
  if (static_cast<int>(all.size()) > budget) all.resize(static_cast<size_t>(budget));  // the first ones of each group
  if (!on.set.empty()) all.push_back(on);
  if (!onKnobs.set.empty()) all.push_back(onKnobs);
  // One parameter changed while the plugin runs (continuous ones first).
  std::vector<Base> live;
  for (int pass = 0; pass < 2; ++pass)
    for (int j = 0; j < n && live.size() < 64; ++j) {
      if (isRouting(info, j)) continue;
      const ParamDesc& d = info.params[static_cast<size_t>(j)];
      if ((d.kind == ParamKind::Continuous) != (pass == 0)) continue;
      const auto vs = searchValues(d, def.params[static_cast<size_t>(j)]);
      if (vs.empty()) continue;
      double lv = vs.back();  // continuous: to the middle (min / max ranges stay distinguishable)
      if (d.kind == ParamKind::Continuous && !sameValue(d, d.fromNormalized(0.5), def.params[static_cast<size_t>(j)])) lv = d.fromNormalized(0.5);
      live.push_back({{{kLive + j, lv}}, "while " + d.name + " changes to " + fmtValue(d, lv)});
    }
  all.insert(all.end(), live.begin(), live.end());
  return all;
}

struct Applied {
  PluginState st, second;
  bool sc = false, live = false;
  void set(int k, double v) {
    st.params[static_cast<size_t>(k)] = static_cast<float>(v);
    second.params[static_cast<size_t>(k)] = static_cast<float>(v);
  }
  float get(int k) const { return st.params[static_cast<size_t>(k)]; }
};

Applied applyBase(const PluginInfo& info, const PluginState& def, const std::vector<std::pair<int, double>>& set) {
  Applied a;
  a.st = def;
  for (const auto& [k, v] : set) {
    if (k < 0) a.sc = true;
    else if (k < kLive && static_cast<size_t>(k) < a.st.params.size())
      a.st.params[static_cast<size_t>(k)] = static_cast<float>(info.params[static_cast<size_t>(k)].clamp(v));
  }
  a.second = a.st;
  for (const auto& [k, v] : set)
    if (k >= kLive && static_cast<size_t>(k - kLive) < a.second.params.size()) {
      a.live = true;
      a.second.params[static_cast<size_t>(k - kLive)] = static_cast<float>(info.params[static_cast<size_t>(k - kLive)].clamp(v));
    }
  return a;
}

Out renderApplied(const PluginInfo& info, const Applied& a) {
  RenderSpec rs;
  rs.state = &a.st, rs.sidechain = a.sc;
  if (a.live) rs.second = &a.second, rs.periods = 2;
  return render(info, rs);
}

bool baseTouches(const std::vector<std::pair<int, double>>& set, int k) {
  for (const auto& [j, v] : set)
    if (j == k || j == kLive + k) return true;
  return false;
}

// A test sample (1.2 s: hits, a tone with harmonics, noise), written once to
// the temp folder: sample instruments get material with transients, pitch,
// sustain and loop room.
const std::string& auditSample() {
  // Never destroyed: the exit hook below reads it after static destructors have run.
  static const std::string& path = *new std::string([] {
    std::error_code ec;
    const std::string p = (std::filesystem::temp_directory_path(ec) / ("gridline_param_audit_" + std::to_string(platform::processId()) + ".wav")).string();
    const size_t n = static_cast<size_t>(1.2 * kSr);
    AudioBuffer buf(2, n);
    uint32_t sd = 0xBEEF1u;
    for (size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / kSr;
      double x = 0;
      for (double at : {0.0, 0.3, 0.6, 0.9}) {  // hits (slicer onsets)
        const double u = t - at;
        if (u >= 0 && u < 0.2) x += 0.6 * std::exp(-u * 30) * (std::sin(2 * kPi * (60 + 200 * std::exp(-u * 40)) * u) + 0.4 * nz(sd));
      }
      for (int h = 1; h <= 6; ++h) x += 0.25 / h * std::sin(2 * kPi * 261.63 * h * t) * std::min(1.0, t / 0.01);
      x += 0.03 * nz(sd);
      buf.plane(0)[i] = static_cast<float>(x * 0.7);
      buf.plane(1)[i] = static_cast<float>(x * 0.6 + 0.05 * std::sin(2 * kPi * 523.25 * t));
    }
    writeWav(p, buf, static_cast<int>(kSr), 16);
    std::atexit([] {
      std::error_code e;
      std::filesystem::remove(auditSample(), e);
    });
    return p;
  }());
  return path;
}

PluginState defaultsFor(const PluginInfo& info) {
  PluginState s = defaultPluginState(info.type);
  if (info.usesSample && s.data.empty()) s.data = auditSample();
  if (info.type == "gl.pads") {  // a sample on every pad and velocity layer
    static const char* kit[] = {"kick", "snare", "clap", "hat", "openhat", "tomlo", "tomhi", "rim"};
    std::string d;
    for (int k = 0; k < 16; ++k) d += std::string("builtin:") + kit[k % 8] + "\n";
    for (int pad = 0; pad < 32; ++pad)
      for (int l = 0; l < 4; ++l)
        if (pad >= 16 || l > 0) d += "L" + std::to_string(pad + 1) + "." + std::to_string(l + 1) + " " + (l % 2 ? auditSample() : std::string("builtin:") + kit[(pad + l) % 8]) + "\n";
    s.data = d;
  }
  return s;
}

// ------------------------------------------------------------- song level

void roundTrip(const PluginInfo& info, PluginReport& rep) {
  Song s = makeEmptySong();
  PluginState st = defaultsFor(info);
  ObjectId owner = kNoId;
  std::function<ParamId(int)> pid;
  if (info.kind == PluginKind::Effect) {
    if (s.mixer.size() < 2) return;
    s.mixer[1].slots[0].plugin = st;
    owner = s.mixer[1].id;
    pid = [owner](int k) { return slotParam(owner, 0, k); };
  } else {
    Channel c;
    c.id = s.allocateId();
    c.name = "audit";
    if (info.kind == PluginKind::Generator) c.plugin = st;
    else {
      c.plugin = defaultPluginState("gl.synth");
      c.noteFx.push_back(st);
    }
    owner = c.id;
    s.channels.push_back(c);
    if (info.kind == PluginKind::Generator) pid = [owner](int k) { return channelPluginParam(owner, k); };
    else pid = [owner](int k) { return noteFxParam(owner, 0, k); };
  }
  std::vector<float> want(st.params.begin(), st.params.end());
  for (auto& pr : rep.params) {
    const ParamDesc& d = info.params[static_cast<size_t>(pr.index)];
    const std::vector<double> vs = testValues(d, d.def);
    if (vs.empty()) continue;
    const double v = vs[vs.size() / 2];
    const ParamId id = pid(pr.index);
    auto desc = describeParam(s, id);
    if (!desc || desc->name != d.name || desc->min != d.min || desc->max != d.max) {
      pr.roundTrip = false, pr.note += "ParamId does not describe the parameter; ";
      continue;
    }
    if (!setParam(s, id, v)) {
      pr.roundTrip = false, pr.note += "setParam failed; ";
      continue;
    }
    const auto got = getParam(s, id);
    const float exp = static_cast<float>(d.clamp(v));
    if (!got || static_cast<float>(*got) != exp) {
      pr.roundTrip = false, pr.note += "getParam returned another value; ";
      continue;
    }
    // Automation clips store 0..1 and map through the descriptor.
    const double back = d.clamp(d.fromNormalized(d.toNormalized(v)));
    if (!sameValue(d, back, v) && std::fabs(back - v) > 1e-4 * std::max(1.0, std::fabs(v))) {
      pr.roundTrip = false, pr.note += "automation mapping does not round-trip; ";
    }
    want[static_cast<size_t>(pr.index)] = exp;
  }
  // Project save / load.
  Json j;
  Song back;
  std::string err;
  if (!jsonParse(jsonStringify(songToJson(s)), j) || !songFromJson(j, back, &err)) {
    for (auto& pr : rep.params) pr.roundTrip = false, pr.note += "project round trip failed: " + err + "; ";
    return;
  }
  const PluginState* got = nullptr;
  if (info.kind == PluginKind::Effect) {
    for (const auto& m : back.mixer)
      if (m.id == owner) got = &m.slots[0].plugin;
  } else {
    for (const auto& c : back.channels)
      if (c.id == owner) got = info.kind == PluginKind::Generator ? &c.plugin : (c.noteFx.empty() ? nullptr : &c.noteFx[0]);
  }
  for (auto& pr : rep.params) {
    const size_t k = static_cast<size_t>(pr.index);
    if (!got || k >= got->params.size() || got->params[k] != want[k]) {
      pr.roundTrip = false;
      pr.note += "value lost in project save/load; ";
    }
  }
}

}  // namespace

// ------------------------------------------------------------- audit

PluginReport auditPlugin(const PluginInfo& info, const Options& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  PluginReport rep;
  rep.type = info.type, rep.name = info.name;
  rep.kind = !info.outputs.empty() ? "controller"
             : info.kind == PluginKind::Generator ? "generator"
             : info.kind == PluginKind::Effect    ? "effect"
                                                  : "notefx";
  PluginState def = defaultsFor(info);
  for (const auto& [k, v] : opt.base)  // --base: audit on top of other settings
    if (k >= 0 && static_cast<size_t>(k) < def.params.size()) def.params[static_cast<size_t>(k)] = static_cast<float>(info.params[static_cast<size_t>(k)].clamp(v));
  const int n = static_cast<int>(info.params.size());
  auto R = [&](const PluginState& st, bool sc = false) {
    ++rep.renders;
    RenderSpec rs;
    rs.state = &st, rs.sidechain = sc;
    return render(info, rs);
  };
  const Out ref = R(def);
  if (opt.probeOnly) {  // --probe: render the (based) defaults once
    rep.refLevelDb = levelDb(ref);
    return rep;
  }
  {
    const Out ref2 = R(def);
    rep.noiseFloorDb = diffDb(ref2, ref);
  }
  rep.refLevelDb = levelDb(ref);
  const double thr = std::max(opt.thresholdDb, rep.noiseFloorDb + 10);
  const double refBright = brightDb(ref), refLevel = rep.refLevelDb;

  rep.params.resize(static_cast<size_t>(n));
  for (int k = 0; k < n; ++k) {
    ParamReport& pr = rep.params[static_cast<size_t>(k)];
    const ParamDesc& d = info.params[static_cast<size_t>(k)];
    pr.index = k, pr.name = d.name;
    if (isRouting(info, k)) {
      pr.status = Status::Routing;
      continue;
    }
    bool broken = false;
    for (double v : testValues(d, def.params[static_cast<size_t>(k)])) {
      PluginState st = def;
      st.params[static_cast<size_t>(k)] = static_cast<float>(v);
      const Out o = R(st);
      Probe p;
      p.value = v, p.finite = o.finite, p.peak = o.peak;
      p.diffDb = diffDb(o, ref);
      p.levelDb = levelDb(o) - refLevel;
      p.brightDb = brightDb(o) - refBright;
      if (!o.finite || o.peak > 100) {
        broken = broken || !o.finite;  // loud (peak > +40 dBFS) is reported, not failed
        pr.note += (o.finite ? "output peak " + std::to_string(static_cast<int>(o.peak)) : std::string("non-finite output")) + " at " +
                   fmtValue(d, v) + "; ";
      }
      if (p.diffDb > pr.bestDiffDb) pr.bestDiffDb = p.diffDb, pr.bestValue = v;
      pr.probes.push_back(p);
    }
    pr.status = broken ? Status::Broken : pr.bestDiffDb > thr ? Status::Working : Status::NoEffect;
  }

  // Condition search for parameters without effect: one other parameter
  // moved, then pairs (a condition found for some parameter plus one more
  // move: e.g. the legacy model and its feedback amount).
  if (opt.combos) {
    std::vector<int> dead;
    for (const auto& pr : rep.params)
      if (pr.status == Status::NoEffect) dead.push_back(pr.index);
    const std::vector<Base> level1 = dead.empty() ? std::vector<Base>{} : searchBases(info, def, opt.maxBases);
    // Group testing: every remaining parameter moved at once; only a group
    // that changes the output is split (halves) down to single parameters.
    auto pass = [&](const std::vector<Base>& bases, int budget) {
      budget += rep.renders;
      for (const Base& b : bases) {
        if (dead.empty() || rep.renders > budget) break;
        const Applied bs = applyBase(info, def, b.set);
        const Out bref = renderApplied(info, bs);
        ++rep.renders;
        if (!bref.finite) continue;
        std::vector<int> group;
        for (int k : dead)
          if (!baseTouches(b.set, k)) group.push_back(k);
        std::set<int> found;
        std::function<void(const std::vector<int>&, int)> probe = [&](const std::vector<int>& g, int which) {
          if (g.empty() || rep.renders > budget) return;
          Applied st = bs;
          for (int k : g) {
            const auto vs = searchValues(info.params[static_cast<size_t>(k)], bs.get(k));
            if (!vs.empty()) st.set(k, which ? vs.back() : vs.front());
          }
          const Out o = renderApplied(info, st);
          ++rep.renders;
          const double dd = diffDb(o, bref);
          if (!(o.finite && dd > thr)) return;
          if (g.size() == 1) {
            const int k = g[0];
            ParamReport& pr = rep.params[static_cast<size_t>(k)];
            pr.status = Status::Combination;
            pr.condition = b.set;
            pr.conditionText = b.text;
            pr.conditionDiffDb = dd;
            pr.bestValue = st.get(k);
            found.insert(k);
            return;
          }
          const auto mid = g.begin() + static_cast<long>(g.size() / 2);
          probe(std::vector<int>(g.begin(), mid), which);
          probe(std::vector<int>(mid, g.end()), which);
        };
        probe(group, 0);
        std::vector<int> rest;
        for (int k : group)
          if (!found.count(k) && searchValues(info.params[static_cast<size_t>(k)], bs.get(k)).size() > 1) rest.push_back(k);
        probe(rest, 1);
        dead.erase(std::remove_if(dead.begin(), dead.end(), [&](int k) { return found.count(k) > 0; }), dead.end());
      }
    };
    {  // Known enabling settings (kHints) first: sections that need several switches at once.
      std::vector<Base> hb;
      for (const HintDoc& h : kHints) {
        if (info.type != h.type) continue;
        Base b;
        b.text = h.set;
        bool ok = true;
        std::string spec = h.set;
        for (size_t a = 0; a < spec.size() && ok;) {
          size_t e = spec.find('|', a);
          if (e == std::string::npos) e = spec.size();
          const std::string item = spec.substr(a, e - a);
          const size_t eq = item.rfind('=');
          int k = -1;
          for (int j = 0; j < n && eq != std::string::npos; ++j)
            if (info.params[static_cast<size_t>(j)].name == item.substr(0, eq)) k = j;
          if (k < 0) ok = false;
          else b.set.push_back({k, std::atof(item.c_str() + eq + 1)});
          a = e + 1;
        }
        if (ok) hb.push_back(b);
      }
      pass(hb, 4000);
    }
    pass(level1, 6000);
    // Repeated groups (pads, units, slots, knobs, bands): a parameter whose
    // twin in another group works under a condition is tried under the same
    // condition moved to its own group ("Pad 1 layer 2 on" -> "Pad 17 layer 2 on").
    auto analogPass = [&]() {
      if (dead.empty()) return;
      auto tokens = [](const std::string& s, std::string& pat) {
        std::vector<std::string> nums;
        pat.clear();
        for (size_t i = 0; i < s.size();) {
          if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            size_t j = i;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
            nums.push_back(s.substr(i, j - i));
            pat += '#';
            i = j;
          } else {
            pat += s[i++];
          }
        }
        return nums;
      };
      std::map<std::string, int> byName;
      for (int j = 0; j < n; ++j) byName.emplace(info.params[static_cast<size_t>(j)].name, j);
      std::vector<Base> analog;
      std::set<std::vector<std::pair<int, double>>> seen;
      for (int k : dead) {
        std::string pk;
        const auto nk = tokens(info.params[static_cast<size_t>(k)].name, pk);
        if (nk.empty()) continue;
        for (const auto& pj : rep.params) {
          if (pj.status != Status::Combination || pj.condition.empty()) continue;
          std::string pat;
          const auto nj = tokens(pj.name, pat);
          if (pat != pk || nj.size() != nk.size()) continue;
          Base b;
          bool ok = true;
          for (const auto& [c, v] : pj.condition) {
            if (c < 0) {
              b.set.push_back({c, v});
              continue;
            }
            const int real = c >= kLive ? c - kLive : c;
            std::string pc;
            auto nc = tokens(info.params[static_cast<size_t>(real)].name, pc);
            for (auto& t : nc)
              for (size_t q = 0; q < nj.size(); ++q)
                if (t == nj[q] && nj[q] != nk[q]) {
                  t = nk[q];
                  break;
                }
            std::string name;
            for (size_t i = 0, t = 0; i < pc.size(); ++i) name += pc[i] == '#' ? nc[t++] : std::string(1, pc[i]);
            const auto it = byName.find(name);
            if (it == byName.end()) {
              ok = false;
              break;
            }
            b.set.push_back({(c >= kLive ? kLive : 0) + it->second, v});
            b.text += (b.text.empty() ? "" : " + ") + (c >= kLive ? "while " + name + " changes" : name + " = " + fmtValue(info.params[static_cast<size_t>(it->second)], v));
          }
          if (ok && !baseTouches(b.set, k) && seen.insert(b.set).second) analog.push_back(b);
          break;  // one twin is enough
        }
      }
      pass(analog, std::max(8000, 6 * static_cast<int>(dead.size())));
    };
    analogPass();
    // Numbered groups as a whole ("Mod 2 source / target / amount", "LFO 1
    // ..."): every parameter of a group moved at once, each choice of the
    // group stepped through, so routings that need several settings (a
    // modulation row needs a source, a target and an amount) come alive —
    // optionally on top of each engine model.
    if (!dead.empty()) {
      std::map<std::string, std::vector<int>> groups;
      for (int j = 0; j < n; ++j) {
        if (isRouting(info, j)) continue;
        const std::string& nm = info.params[static_cast<size_t>(j)].name;
        size_t i = 0;
        while (i < nm.size() && !std::isdigit(static_cast<unsigned char>(nm[i]))) ++i;
        if (i == nm.size() || i == 0) continue;
        while (i < nm.size() && std::isdigit(static_cast<unsigned char>(nm[i]))) ++i;
        groups[nm.substr(0, i)].push_back(j);
      }
      std::vector<std::pair<int, double>> models = {{-2, 0}};
      for (int j = 0; j < n; ++j) {
        const ParamDesc& d = info.params[static_cast<size_t>(j)];
        std::string lname = d.name;
        for (char& ch : lname) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (d.kind == ParamKind::Choice && (j == info.legacyModelParam || lname == "engine" || lname.find("model") != std::string::npos))
          for (double v : testValues(d, def.params[static_cast<size_t>(j)])) models.push_back({j, v});
      }
      std::vector<Base> gb;
      for (const auto& [m, mv] : models)
        for (const auto& [pre, js] : groups) {
          if (js.size() < 2) continue;
          bool anyDead = false;
          for (int k : dead)
            for (int j : js) anyDead |= j == k;
          if (!anyDead && m < 0) continue;  // a group can still enable others (LFO via a mod row) on a model
          Base all;
          for (int j : js) {
            const auto vs = searchValues(info.params[static_cast<size_t>(j)], def.params[static_cast<size_t>(j)]);
            if (!vs.empty()) all.set.push_back({j, vs.back()});
          }
          std::vector<Base> variants{all};
          // Conditions already found for members of the group (a step's gate
          // needs the pattern length): the group moved on top of each.
          std::vector<std::pair<std::vector<std::pair<int, double>>, std::string>> conds = {{{}, ""}};
          for (int j : js) {
            const ParamReport& q = rep.params[static_cast<size_t>(j)];
            if (q.status != Status::Combination || q.condition.empty() || q.condition.size() > 2) continue;
            bool dup = false;
            for (const auto& c : conds) dup |= c.first == q.condition;
            if (!dup && conds.size() < 4) conds.push_back({q.condition, q.conditionText});
          }
          for (const auto& [cset, ctext] : conds)
            for (int k : js) {  // the others moved: tests k itself
              Base b;
              for (const auto& e : all.set)
                if (e.first != k) b.set.push_back(e);
              bool clash = false;
              for (const auto& e : cset) {
                for (auto& x : b.set) clash |= x.first == e.first || x.first + kLive == e.first || x.first == e.first + kLive;
                if (e.first == k || e.first == k + kLive) clash = true;
              }
              if (clash) continue;
              b.set.insert(b.set.end(), cset.begin(), cset.end());
              b.text = pre + " group moved except " + info.params[static_cast<size_t>(k)].name + (ctext.empty() ? "" : " + " + ctext);
              variants.push_back(b);
            }
          for (int c : js) {
            const ParamDesc& d = info.params[static_cast<size_t>(c)];
            if (d.kind != ParamKind::Choice) continue;
            std::vector<double> vs = testValues(d, def.params[static_cast<size_t>(c)]);
            if (vs.size() > 12) vs.resize(12);
            for (double v : vs) {
              Base b = all;
              for (auto& e : b.set)
                if (e.first == c) e.second = v;
              b.text = pre + " group, " + d.name + " = " + fmtValue(d, v);
              variants.push_back(b);
            }
          }
          for (auto& b : variants) {
            if (b.text.empty()) b.text = pre + " group moved";
            if (m >= 0) {
              bool clash = false;
              for (const auto& e : b.set) clash |= e.first == m;
              if (clash) continue;
              b.set.push_back({m, mv});
              b.text += " (" + info.params[static_cast<size_t>(m)].name + " = " + fmtValue(info.params[static_cast<size_t>(m)], mv) + ")";
            }
            gb.push_back(b);
          }
        }
      pass(gb, std::max(8000, 6 * static_cast<int>(dead.size())));
    }
    analogPass();  // conditions the group pass found, moved to the other groups
    if (!dead.empty()) {
      // Enablers: single-move conditions found above (and every model of FL-mapped plugins).
      std::vector<Base> enablers;
      auto addEnabler = [&](const Base& e) {
        if (e.set.size() != 1) return;
        for (const Base& x : enablers)
          if (x.set == e.set) return;
        enablers.push_back(e);
      };
      if (info.legacyModelParam >= 0) {
        const ParamDesc& d = info.params[static_cast<size_t>(info.legacyModelParam)];
        for (double v : testValues(d, def.params[static_cast<size_t>(info.legacyModelParam)]))
          addEnabler({{{info.legacyModelParam, v}}, d.name + " = " + fmtValue(d, v)});
      }
      for (const auto& pr : rep.params)
        if (pr.status == Status::Combination) addEnabler({pr.condition, pr.conditionText});
      std::vector<Base> level2;
      for (const Base& e : enablers)
        for (const Base& b : level1) {
          if (b.set.size() != 1 || b.set[0].first == e.set[0].first || b.set[0].first == e.set[0].first + kLive) continue;
          level2.push_back({{e.set[0], b.set[0]}, e.text + " + " + b.text});
        }
      pass(level2, 12000);
    }
  }

  // Live change (automation path): prepared with the parameter at its
  // maximum (what the engine does for automated parameters), switched at the
  // second period without any re-prepare.
  if (opt.liveness) {
    RenderSpec ra;
    ra.state = &def, ra.periods = 2;
    const Out A = render(info, ra);
    ++rep.renders;
    for (auto& pr : rep.params) {
      if (pr.status != Status::Working) continue;
      const ParamDesc& d = info.params[static_cast<size_t>(pr.index)];
      PluginState v = def, prep = def;
      v.params[static_cast<size_t>(pr.index)] = static_cast<float>(pr.bestValue);
      prep.params[static_cast<size_t>(pr.index)] = static_cast<float>(d.max);
      RenderSpec rb;
      rb.state = &def, rb.prep = &prep, rb.second = &v, rb.periods = 2;
      RenderSpec rc;
      rc.state = &v, rc.periods = 2;
      const Out B = render(info, rb), C = render(info, rc);
      rep.renders += 2;
      pr.liveTested = true;
      pr.liveDiffDb = diffDb(B, A, kPeriod);
      pr.preparedDiffDb = diffDb(C, A, kPeriod);
      if (pr.preparedDiffDb > thr + 20 && pr.liveDiffDb < std::max(thr, pr.preparedDiffDb - 50)) {
        pr.live = false;
        pr.note += "only applies when the plugin is prepared (no effect when changed live / by automation); ";
      }
    }
  }

  for (auto& pr : rep.params)
    if (pr.status == Status::NoEffect)
      if (const char* why = inertReason(info.type, pr.name))
        pr.status = std::strncmp(why, "VERIFIED", 8) == 0 ? Status::Verified : Status::Inert, pr.inertReason = why;
  roundTrip(info, rep);
  rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return rep;
}

namespace {
constexpr int kActiveSlots = 64;
std::atomic<const char*> activeTypes[kActiveSlots] = {};
thread_local int activeSlot = -1;
}  // namespace

void writeActive(int fd) {
  for (int k = 0; k < kActiveSlots; ++k)
    if (const char* t = activeTypes[k].load()) {
      (void)!rawWrite(fd, k == activeSlot ? "* auditing " : "  auditing ", 11);  // * = the crashing thread
      (void)!rawWrite(fd, t, std::strlen(t));
      (void)!rawWrite(fd, "\n", 1);
    }
  // The crashing thread's settings that differ from the defaults (best effort).
  if (crashState) {
    (void)!rawWrite(fd, "  rendering ", 12);
    (void)!rawWrite(fd, crashState->type.c_str(), crashState->type.size());
    (void)!rawWrite(fd, "\n", 1);
  }
  if (crashState)
    if (const PluginInfo* info = findPlugin(crashState->type)) {
      char buf[96];
      for (size_t k = 0; k < crashState->params.size() && k < info->params.size(); ++k)
        if (crashState->params[k] != static_cast<float>(info->params[k].def)) {
          const int n = std::snprintf(buf, sizeof buf, "    #%zu %s = %g\n", k, info->params[k].name.c_str(), static_cast<double>(crashState->params[k]));
          (void)!rawWrite(fd, buf, static_cast<size_t>(std::max(0, std::min(n, static_cast<int>(sizeof buf) - 1))));
        }
    }
}

std::vector<PluginReport> auditAll(const Options& opt, const std::function<bool(const PluginInfo&)>& filter, int threads,
                                   const std::function<void(const PluginReport&)>& done) {
  std::vector<const PluginInfo*> todo;
  for (const PluginInfo& info : pluginCatalog())
    if (!isHostedType(info.type) && (!filter || filter(info))) todo.push_back(&info);
  // Warm-up on this thread (lazily built tables) before the parallel runs.
  for (const PluginInfo* info : todo) {
    const PluginState st = defaultsFor(*info);
    RenderSpec rs;
    rs.state = &st;
    (void)render(*info, rs);
  }
  std::vector<size_t> order(todo.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return todo[a]->params.size() > todo[b]->params.size(); });
  std::vector<PluginReport> out(todo.size());
  std::atomic<size_t> next{0};
  std::mutex m;
  auto work = [&](int slot) {
    for (size_t k; (k = next.fetch_add(1)) < order.size();) {
      const size_t i = order[k];
      activeSlot = slot;
      if (slot < kActiveSlots) activeTypes[slot].store(todo[i]->type.c_str());
      out[i] = auditPlugin(*todo[i], opt);
      if (slot < kActiveSlots) activeTypes[slot].store(nullptr);
      if (done) {
        std::lock_guard<std::mutex> lk(m);
        done(out[i]);
      }
    }
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < std::max(1, threads); ++t) pool.emplace_back(work, t);
  work(0);
  for (auto& t : pool) t.join();
  return out;
}

// ------------------------------------------------------------- reports

namespace {
std::string db(double v) {
  if (v <= kNone + 1) return "-inf";
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}
std::string condString(const std::vector<std::pair<int, double>>& c) {
  std::string s;
  for (const auto& [k, v] : c) {
    char b[64];
    std::snprintf(b, sizeof b, "%s%d=%.9g", s.empty() ? "" : ";", k, v);
    s += b;
  }
  return s;
}
}  // namespace

Json toJson(const std::vector<PluginReport>& r) {
  Json all = Json::makeArray();
  for (const auto& p : r) {
    Json jp = Json::makeObject();
    jp.object["type"] = Json::makeString(p.type);
    jp.object["name"] = Json::makeString(p.name);
    jp.object["kind"] = Json::makeString(p.kind);
    jp.object["refLevelDb"] = Json::makeNumber(p.refLevelDb);
    jp.object["noiseFloorDb"] = Json::makeNumber(p.noiseFloorDb);
    jp.object["seconds"] = Json::makeNumber(p.seconds);
    jp.object["renders"] = Json::makeNumber(p.renders);
    Json ps = Json::makeArray();
    for (const auto& q : p.params) {
      Json jq = Json::makeObject();
      jq.object["index"] = Json::makeNumber(q.index);
      jq.object["name"] = Json::makeString(q.name);
      jq.object["status"] = Json::makeString(statusName(q.status));
      jq.object["bestDiffDb"] = Json::makeNumber(q.bestDiffDb);
      jq.object["bestValue"] = Json::makeNumber(q.bestValue);
      if (!q.condition.empty()) {
        jq.object["condition"] = Json::makeString(condString(q.condition));
        jq.object["conditionText"] = Json::makeString(q.conditionText);
        jq.object["conditionDiffDb"] = Json::makeNumber(q.conditionDiffDb);
      }
      if (q.liveTested) {
        jq.object["live"] = Json::makeNumber(q.live ? 1 : 0);
        jq.object["liveDiffDb"] = Json::makeNumber(q.liveDiffDb);
        jq.object["preparedDiffDb"] = Json::makeNumber(q.preparedDiffDb);
      }
      jq.object["roundTrip"] = Json::makeNumber(q.roundTrip ? 1 : 0);
      if (!q.note.empty()) jq.object["note"] = Json::makeString(q.note);
      if (!q.inertReason.empty()) jq.object["inertReason"] = Json::makeString(q.inertReason);
      Json pr = Json::makeArray();
      for (const auto& x : q.probes) {
        Json jx = Json::makeObject();
        jx.object["value"] = Json::makeNumber(x.value);
        jx.object["diffDb"] = Json::makeNumber(x.diffDb);
        jx.object["levelDb"] = Json::makeNumber(x.levelDb);
        jx.object["brightDb"] = Json::makeNumber(x.brightDb);
        jx.object["finite"] = Json::makeNumber(x.finite ? 1 : 0);
        pr.array.push_back(std::move(jx));
      }
      jq.object["probes"] = std::move(pr);
      ps.array.push_back(std::move(jq));
    }
    jp.object["params"] = std::move(ps);
    all.array.push_back(std::move(jp));
  }
  return all;
}

std::string toMarkdown(const std::vector<PluginReport>& r) {
  std::ostringstream o;
  int counts[7] = {}, total = 0, notLive = 0, rt = 0;
  for (const auto& p : r)
    for (const auto& q : p.params) {
      ++counts[static_cast<int>(q.status)], ++total;
      notLive += q.liveTested && !q.live;
      rt += !q.roundTrip;
    }
  o << "| plugins | parameters | working | combination | no effect | routing | broken | inert (documented) | verified in source | not live | round-trip failures |\n"
       "|---|---|---|---|---|---|---|---|---|---|---|\n";
  o << "| " << r.size() << " | " << total << " | " << counts[0] << " | " << counts[1] << " | " << counts[2] << " | " << counts[3] << " | "
    << counts[4] << " | " << counts[5] << " | " << counts[6] << " | " << notLive << " | " << rt << " |\n\n";
  for (const auto& p : r) {
    const PluginInfo* info = findPlugin(p.type);
    o << "### " << p.name << " (`" << p.type << "`, " << p.kind << ")\n\n";
    o << "default level " << db(p.refLevelDb) << " dBFS, noise floor " << db(p.noiseFloorDb) << " dB, " << p.renders << " renders, "
      << db(p.seconds) << " s\n\n";
    o << "| # | parameter | status | change vs default (dB rel.) | condition | live (dB) | notes |\n|---|---|---|---|---|---|---|\n";
    for (const auto& q : p.params) {
      const ParamDesc* d = info && static_cast<size_t>(q.index) < info->params.size() ? &info->params[static_cast<size_t>(q.index)] : nullptr;
      o << "| " << q.index << " | " << q.name << " | " << statusName(q.status) << " | ";
      size_t shown = 0;
      for (const auto& x : q.probes) {
        if (shown++ == 6) {
          o << " …";
          break;
        }
        o << (shown > 1 ? ", " : "") << (d ? fmtValue(*d, x.value) : std::to_string(x.value)) << ": " << db(x.diffDb);
      }
      o << " | ";
      if (!q.conditionText.empty()) o << q.conditionText << " (" << db(q.conditionDiffDb) << ")";
      o << " | ";
      if (q.liveTested) o << (q.live ? "yes " : "**NO** ") << db(q.liveDiffDb) << " / " << db(q.preparedDiffDb);
      o << " | " << q.note << (q.inertReason.empty() ? "" : q.inertReason) << (q.roundTrip ? "" : "**round trip**") << " |\n";
    }
    o << "\n";
  }
  return o.str();
}

std::string toBaseline(const std::vector<PluginReport>& r) {
  std::string s = "# type\tindex\tname\tstatus\tvalue\tcondition\treason\n";
  char b[64];
  for (const auto& p : r)
    for (const auto& q : p.params) {
      std::snprintf(b, sizeof b, "%.9g", q.bestValue);
      std::string name = q.name;
      for (char& c : name)
        if (c == '\t' || c == '\n') c = ' ';
      s += p.type + "\t" + std::to_string(q.index) + "\t" + name + "\t" + statusName(q.status) + "\t" + b + "\t" + condString(q.condition) + (q.inertReason.empty() ? "" : "\t" + q.inertReason) + "\n";
    }
  return s;
}

std::vector<BaselineRow> parseBaseline(const std::string& text) {
  std::vector<BaselineRow> rows;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    size_t a = 0;
    for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
    f.push_back(line.substr(a));
    if (f.size() < 6) continue;
    BaselineRow r;
    r.type = f[0], r.index = std::atoi(f[1].c_str()), r.name = f[2], r.status = f[3], r.value = std::atof(f[4].c_str());
    std::istringstream cs(f[5]);
    std::string part;
    while (std::getline(cs, part, ';')) {
      const size_t eq = part.find('=');
      if (eq != std::string::npos) r.condition.push_back({std::atoi(part.substr(0, eq).c_str()), std::atof(part.substr(eq + 1).c_str())});
    }
    rows.push_back(std::move(r));
  }
  return rows;
}

std::vector<double> checkRows(const PluginInfo& info, const std::vector<const BaselineRow*>& rows) {
  std::vector<double> out(rows.size(), kNone);
  const PluginState def = defaultsFor(info);
  std::vector<std::pair<std::vector<std::pair<int, double>>, Out>> refs;
  for (size_t i = 0; i < rows.size(); ++i) {
    const BaselineRow& row = *rows[i];
    if (row.index < 0 || static_cast<size_t>(row.index) >= info.params.size()) continue;
    const Applied bs = applyBase(info, def, row.condition);
    const Out* ref = nullptr;
    for (const auto& [c, o] : refs)
      if (c == row.condition) ref = &o;
    if (!ref) {
      refs.push_back({row.condition, renderApplied(info, bs)});
      ref = &refs.back().second;
    }
    Applied st = bs;
    st.set(row.index, info.params[static_cast<size_t>(row.index)].clamp(row.value));
    out[i] = diffDb(renderApplied(info, st), *ref);
  }
  return out;
}

}  // namespace gl::paramaudit
