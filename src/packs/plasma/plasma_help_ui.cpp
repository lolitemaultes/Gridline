// Gridline — "plasma" pack: one plain sentence per control, shown when the pointer rests on it.
// The sentence says what the control does in everyday words; the value and its unit show beside it.
#include <string>
#include "../../core/I18n.h"
#include "plasma_ui.h"

namespace gl::packs::plasma::ui {

namespace {
const char* oscSentence(int q) {
  switch (q) {
    case O_Enable: return i18n::N_("Turns this layer on or off.");
    case O_Type: return i18n::N_("Chooses how this layer makes its sound: a wavetable, a multisample, a sample, grains or spectral bands.");
    case O_Table: return i18n::N_("The wavetable this layer plays.");
    case O_Level: return i18n::N_("How loud this layer is.");
    case O_Pan: return i18n::N_("Places this layer to the left or right.");
    case O_Octave: return i18n::N_("Moves this layer up or down in whole octaves.");
    case O_Semi: return i18n::N_("Moves this layer up or down in semitones.");
    case O_Fine: return i18n::N_("Fine tuning in cents, a hundredth of a semitone.");
    case O_Coarse: return i18n::N_("A smooth pitch offset in semitones, good for linking to motion.");
    case O_Ratio: return i18n::N_("The pitch as a ratio of the played note (used when Pitch steps is set to Ratio).");
    case O_HzOffset: return i18n::N_("Shifts the pitch by a fixed number of hertz, which makes it beat against other layers.");
    case O_PitchTrack: return i18n::N_("When on, the pitch follows the keys you play.");
    case O_PitchMode: return i18n::N_("How the Octave, Semitone and Offset values are counted.");
    case O_WtPos: return i18n::N_("Which frame of the wavetable plays: sweep it to change the tone.");
    case O_Phase: return i18n::N_("Where in the wave each note starts.");
    case O_RandPhase: return i18n::N_("Gives each note a random start point in the wave.");
    case O_PhaseMemory: return i18n::N_("Whether each voice starts fresh or keeps running from the last note.");
    case O_Smooth: return i18n::N_("Glides smoothly between wavetable frames instead of stepping.");
    case O_Unison: return i18n::N_("How many copies of this layer play at once for each note.");
    case O_UniStack: return i18n::N_("How the stacked copies are spread: same pitch, octaves, chords and more.");
    case O_UniDetune: return i18n::N_("How far apart in pitch the stacked copies are.");
    case O_UniBlend: return i18n::N_("How loud the outer copies are against the centre ones.");
    case O_UniWidth: return i18n::N_("How wide the stacked copies spread across the stereo field.");
    case O_UniSpan: return i18n::N_("Tilts the spread so copies lean up or down in pitch.");
    case O_UniRange: return i18n::N_("The widest detune, in semitones, that the Detune dial reaches.");
    case O_UniRotate: return i18n::N_("Turns the order of the stacked copies for each new note.");
    case O_UniRandStart: return i18n::N_("Gives each stacked copy its own random start point.");
    case O_UniWtPos: return i18n::N_("Spreads the stacked copies across nearby wavetable frames.");
    case O_UniWarp1: case O_UniWarp2: return i18n::N_("Spreads the morph amount across the stacked copies.");
    case O_DetuneMode: return i18n::N_("The shape of the detune spread across the copies.");
    case O_Warp1Mode: case O_Warp2Mode: return i18n::N_("The morph that reshapes the wave, such as bending, syncing or folding it.");
    case O_Warp1: case O_Warp2: return i18n::N_("How strongly the morph reshapes the wave.");
    case O_Warp1Var: case O_Warp2Var: return i18n::N_("A second setting some morphs use, such as which wave they blend with.");
    default: return nullptr;
  }
}
const char* filterSentence(int q) {
  switch (q) {
    case F_Enable: return i18n::N_("Turns this filter on or off.");
    case F_Type: return i18n::N_("The kind of filter: which frequencies it lets through and how it colours them.");
    case F_Freq: return i18n::N_("The cutoff: where the filter starts to act. Lower is darker.");
    case F_Res: return i18n::N_("Resonance: a peak at the cutoff that makes the filter sing.");
    case F_Drive: return i18n::N_("Drives the sound into the filter for grit and warmth.");
    case F_Var: return i18n::N_("A setting that depends on the filter type, such as its slope or vowel.");
    case F_Stereo: return i18n::N_("Moves the cutoff apart for the left and right sides.");
    case F_Wet: return i18n::N_("Blends the filtered sound with the unfiltered one.");
    case F_Level: return i18n::N_("How loud the filter's output is.");
    case F_KeyTrack: return i18n::N_("How much the cutoff follows the keys you play.");
    case F_X: case F_Y: return i18n::N_("Moves the filter's character on its morph pad.");
    default: return nullptr;
  }
}
const char* envSentence(int q) {
  switch (q) {
    case E_Attack: return i18n::N_("How long the envelope takes to rise when a note starts.");
    case E_Hold: return i18n::N_("How long it stays at the top before falling.");
    case E_Decay: return i18n::N_("How long it takes to fall to the sustain level.");
    case E_Sustain: return i18n::N_("The level it holds while you keep the key down.");
    case E_Release: return i18n::N_("How long it takes to fade after you let go.");
    case E_CurveA: case E_CurveD: case E_CurveR: return i18n::N_("Bends this part of the envelope: faster at first or slower at first.");
    case E_Start: return i18n::N_("The level the envelope starts from.");
    case E_End: return i18n::N_("The level the envelope ends on after the release.");
    case E_Bpm: return i18n::N_("Counts the times in beats so they follow the song's tempo.");
    case E_LegatoInv: return i18n::N_("Restarts the envelope even when notes are played joined together.");
    case E_Steal: return i18n::N_("Where a voice taken over by a new note starts its envelope from.");
    default: return nullptr;
  }
}
const char* lfoSentence(int q) {
  switch (q) {
    case L_Rate: return i18n::N_("How fast the shape repeats.");
    case L_Smooth: return i18n::N_("Softens sharp corners in the shape.");
    case L_Rise: return i18n::N_("Fades the motion in over this time after a note starts.");
    case L_Delay: return i18n::N_("Waits this long after a note starts before moving.");
    case L_Phase: return i18n::N_("Where in the shape each cycle starts.");
    case L_Mode: return i18n::N_("Free keeps running, Restart starts again on every note, Once plays the shape one time.");
    case L_Type: return i18n::N_("The kind of motion: a drawn shape, a path, a chaotic swirl or random steps.");
    case L_Bpm: return i18n::N_("Locks the rate to the song's tempo.");
    case L_Anchor: return i18n::N_("Lines the shape up with the song position.");
    case L_Dot: return i18n::N_("Makes a synced rate one and a half times longer.");
    case L_Trip: return i18n::N_("Makes a synced rate a triplet.");
    case L_GridX: case L_GridY: return i18n::N_("The editing grid for drawing the shape.");
    case L_Mono: return i18n::N_("One shared motion for all notes instead of one per note.");
    case L_Dir: return i18n::N_("Plays the shape forwards, backwards or back and forth.");
    case L_Rate10x: return i18n::N_("Makes the free rate ten times faster.");
    case L_Swing: return i18n::N_("Delays every second step for a swung feel.");
    case L_PhaseSnap: return i18n::N_("Snaps the start point to the grid.");
    case L_Shape: return i18n::N_("The shape from the library.");
    default: return nullptr;
  }
}
const char* globalSentence(int k) {
  switch (k) {
    case G_MasterVol: return i18n::N_("The overall output level.");
    case G_MasterTune: return i18n::N_("Tunes the whole instrument up or down in cents.");
    case G_Amp: return i18n::N_("How much Envelope 1 shapes each note's level.");
    case G_PortaTime: return i18n::N_("Glide: how long a note takes to slide to the next pitch.");
    case G_PortaCurve: return i18n::N_("The shape of the glide: quick at first or slow at first.");
    case G_PortaAlways: return i18n::N_("Glides on every note, not only on joined ones.");
    case G_PortaScaled: return i18n::N_("Longer jumps take longer to glide.");
    case G_BendUp: case G_BendDown: return i18n::N_("How far the pitch wheel bends, in semitones.");
    case G_Mono: return i18n::N_("Plays one note at a time.");
    case G_Legato: return i18n::N_("Joined notes slide without restarting.");
    case G_Poly: return i18n::N_("How many notes can sound at once.");
    case G_Transpose: return i18n::N_("Moves everything you play up or down in semitones.");
    case G_DirectVol: return i18n::N_("The level of the sound that skips the effects.");
    case G_Key: case G_Scale: return i18n::N_("Keeps notes and pitch motion inside this key and scale.");
    case G_Swing: return i18n::N_("Delays every second step of the arpeggiator and phrases.");
    case G_EnvScale: return i18n::N_("Makes every envelope time shorter or longer at once.");
    case G_LfoScale: return i18n::N_("Makes every LFO faster or slower at once.");
    case G_Quality: return i18n::N_("Higher quality sounds cleaner and uses more processing.");
    default: return nullptr;
  }
}
}  // namespace

std::string paramSentence(int k) {
  const char* s = nullptr;
  if (k >= 0 && k < G_Count) s = globalSentence(k);
  else if (k >= kOscBase && k < kSubBase) s = oscSentence((k - kOscBase) % O_Count);
  else if (k >= kSubBase && k < kNoiseBase) {
    switch (k - kSubBase) {
      case S_Enable: s = i18n::N_("Turns the Root on or off: a plain wave under the layers."); break;
      case S_Level: s = i18n::N_("How loud the Root is."); break;
      case S_Pan: s = i18n::N_("Places the Root to the left or right."); break;
      case S_Octave: s = i18n::N_("Moves the Root up or down in octaves."); break;
      case S_Coarse: s = i18n::N_("Moves the Root up or down in semitones."); break;
      case S_PitchTrack: s = i18n::N_("When on, the Root follows the keys you play."); break;
      case S_Shape: s = i18n::N_("The Root's wave shape."); break;
      case S_Phase: s = i18n::N_("Where in the wave each note starts."); break;
      case S_ContPhase: s = i18n::N_("Keeps the wave running between notes."); break;
      default: break;
    }
  } else if (k >= kNoiseBase && k < kFiltBase) {
    switch (k - kNoiseBase) {
      case N_Enable: s = i18n::N_("Turns the Texture on or off: noise or a recorded sound under the layers."); break;
      case N_Level: s = i18n::N_("How loud the Texture is."); break;
      case N_Pan: s = i18n::N_("Places the Texture to the left or right."); break;
      case N_Pitch: s = i18n::N_("Plays the Texture higher or lower."); break;
      case N_Color: s = i18n::N_("Makes the Texture darker or brighter."); break;
      case N_OneShot: s = i18n::N_("Plays the Texture once instead of looping it."); break;
      case N_PitchTrack: s = i18n::N_("When on, the Texture's pitch follows the keys you play."); break;
      default: break;
    }
  } else if (k >= kFiltBase && k < kRouteBase) s = filterSentence((k - kFiltBase) % F_Count);
  else if (k >= kRouteBase && k < kEnvBase) {
    switch ((k - kRouteBase) % R_Count) {
      case R_Dest: s = i18n::N_("Where this sound goes: into the filters, straight out, or past the effects."); break;
      case R_Balance: s = i18n::N_("How much goes into Filter 1 against Filter 2."); break;
      case R_Bus1: case R_Bus2: s = i18n::N_("How much of this sound is sent to a send chain."); break;
      default: break;
    }
  } else if (k >= kEnvBase && k < kLfoBase) s = envSentence((k - kEnvBase) % E_Count);
  else if (k >= kLfoBase && k < kMacroBase) s = lfoSentence((k - kLfoBase) % L_Count);
  else if (k >= kMacroBase && k < kBusBase) s = i18n::N_("A macro: one control you can link to many others.");
  else if (k >= kModBase && k < kFxBase) {
    switch ((k - kModBase) % M_Count) {
      case M_Amount: s = i18n::N_("How far this link moves its control."); break;
      default: s = i18n::N_("A setting of this link between a source and a control."); break;
    }
  } else if (k >= kFxBase && k < kTailBase) {
    if ((k - kFxBase) % X_Count == X_On) s = i18n::N_("Turns this effect on or off.");
  }
  return s ? i18n::tr(s) : std::string();
}

}  // namespace gl::packs::plasma::ui
