// Gridline - K-105: First-run setup wizard & interactive guided tour core logic.
#include "SetupTour.h"
#include <algorithm>
#include <cmath>
#include "I18n.h"
#include "Project.h"
#include "Settings.h"

namespace gl::setuptour {

using i18n::N_;

bool shouldShowFirstRunWizard(const FirstRunInputs& in) {
  if (in.setupDone || in.layoutChosen) return false;
  if (!in.plainLaunch || in.hidden || in.quietWindows || in.automated || in.embedded || in.hasPluginArg) return false;
  return true;
}

bool isSetupDone(const Settings& s) {
  return s.getBool("ui.setupDone") || s.getBool("ui.layoutChosen");
}

void markSetupDone(Settings& s) {
  s.set("ui.setupDone", true);
  s.set("ui.layoutChosen", true);
}

const LookSpec& lookSpec(LookPreset p) {
  static const LookSpec kSpecs[kLookPresetCount] = {
      {LookPreset::Dark, "dark", N_("Dark"), N_("Dark grey panels"), true, 0.0f, 0.0f, 0.0f, 0.0f, false},
      {LookPreset::Light, "light", N_("Light"), N_("Light panels"), false, 0.0f, 0.0f, 0.0f, 0.0f, false},
      {LookPreset::Night, "night", N_("Night"), N_("Darker panels for dim rooms"), true, -0.92f, 0.14f, 0.0f, -0.15f, false},
      {LookPreset::Tinted, "tinted", N_("Tinted"), N_("Blue-grey panels"), true, -0.12f, 0.06f, 0.08f, 0.45f, false},
  };
  const int idx = std::clamp(static_cast<int>(p), 0, kLookPresetCount - 1);
  return kSpecs[idx];
}

void applyLookToSettings(LookPreset p, Settings& s) {
  const LookSpec& sp = lookSpec(p);
  s.set("theme.mode", std::string(sp.dark ? "dark" : "light"));
  s.set("ui.darkTheme", sp.dark);
  s.set("theme.brightness", static_cast<double>(sp.brightness));
  s.set("theme.contrast", static_cast<double>(sp.contrast));
  s.set("theme.hue", static_cast<double>(sp.hue));
  s.set("theme.saturation", static_cast<double>(sp.saturation));
  s.set("theme.whiteText", sp.whiteText);
}

LookPreset detectLookPreset(const Settings& s) {
  const std::string mode = s.getString("theme.mode");
  const bool dark = mode == "light" ? false : mode == "dark" ? true : s.getBool("ui.darkTheme");
  if (!dark) return LookPreset::Light;
  const double br = s.getNumber("theme.brightness");
  const double sat = s.getNumber("theme.saturation");
  if (br <= -0.55) return LookPreset::Night;
  if (sat >= 0.25) return LookPreset::Tinted;
  return LookPreset::Dark;
}

const std::vector<TourStep>& tourStepsFor(layout::Preset preset) {
  static const std::vector<TourStep> kStudio = {
      {TourTarget::Transport, "transport", N_("Transport and top bar"),
       N_("Control playback, tempo, pattern or song mode, recording, and your master output level from one strip."),
       N_("Press Space to start or stop playback, and switch between PAT and SONG mode.")},
      {TourTarget::Browser, "browser", N_("Browser"),
       N_("Your sounds, instruments, effects and presets. Click one to hear it, or drag it into the project."),
       N_("Click any sample to audition it, or drag an instrument onto the rack or playlist.")},
      {TourTarget::ChannelRack, "rack", N_("Channel rack"),
       N_("Your instruments and step sequencer for sketching drum grooves and layering synth patterns fast."),
       N_("Click a step to add a beat there, and click it again to remove it.")},
      {TourTarget::PianoRoll, "piano", N_("Piano roll"),
       N_("Write melodies, chords, and expressive note velocities on a full musical grid with scale helpers."),
       N_("Left-click to place a note, drag its edge to resize, or right-click to delete it.")},
      {TourTarget::Playlist, "playlist", N_("Playlist"),
       N_("Arrange your patterns, audio recordings, and automation lanes across tracks to build a complete song."),
       N_("Drag patterns from the picker onto tracks, or click and drag clips to rearrange them.")},
      {TourTarget::Mixer, "mixer", N_("Mixer and effects"),
       N_("Balance channel levels, stereo pan, sends, and stack effect chains on any insert track or the master."),
       N_("Drag a fader to balance a track, or click an empty effect slot on the right to add an effect.")},
  };
  static const std::vector<TourStep> kLive = {
      {TourTarget::Transport, "transport", N_("Control bar and transport"),
       N_("Launch playback, dial in tempo, toggle loop and metronome, and watch your master meters along the top."),
       N_("Press Space to play or pause, and press Tab anytime to switch between clip grid and timeline.")},
      {TourTarget::Browser, "browser", N_("Browser"),
       N_("Find sounds, instruments, effects, and presets on the left. Click to preview or drag straight onto any track."),
       N_("Drag an effect from the Browser directly into the Device chain at the bottom.")},
      {TourTarget::Playlist, "playlist", N_("Clip grid and arrangement"),
       N_("Play loops in the clip grid, or place them in order on the timeline."),
       N_("Click a clip's launch triangle to trigger it in time, or double-click a clip to inspect its notes.")},
      {TourTarget::PianoRoll, "piano", N_("Clip and note editor"),
       N_("Shape MIDI notes, timing, and velocities for the selected clip right in the lower editor view."),
       N_("Click in the grid to add notes, or drag the velocity stalks below to add groove.")},
      {TourTarget::DeviceChain, "device", N_("Device chain"),
       N_("See every instrument and effect on the selected track laid out left to right as live device cards."),
       N_("Click + to add an effect. Click a device's lamp to switch it off and on and compare.")},
      {TourTarget::Mixer, "mixer", N_("Mixer"),
       N_("Adjust track volumes, pan, sends, mute, and solo across every track in your set."),
       N_("Click S to solo a track and hear how it sits in the mix.")},
  };
  static const std::vector<TourStep> kLogic = {
      {TourTarget::Transport, "transport", N_("Control bar and transport"),
       N_("Play, record, toggle cycle loop, check tempo and time signature, and toggle panes from the top control bar."),
       N_("Press Space to play, and use the toolbar icons on the left to show or hide panes.")},
      {TourTarget::Inspector, "inspector", N_("Track inspector"),
       N_("View and edit the selected track's settings, instrument, audio inserts, and channel strip right beside the tracks."),
       N_("Select any track header to inspect its fader, sends, and effect slots immediately.")},
      {TourTarget::Browser, "browser", N_("Browser and library"),
       N_("Browse instruments, presets, and audio loops to load onto your selected track or drag into the arrangement."),
       N_("Click a sample to preview it, or drag it onto an empty track lane.")},
      {TourTarget::Playlist, "playlist", N_("Tracks area"),
       N_("Arrange MIDI regions, audio clips, and automation lanes across your song timeline."),
       N_("Drag regions to move them, or drag the top ruler to adjust the cycle loop range.")},
      {TourTarget::PianoRoll, "piano", N_("Piano roll editor"),
       N_("Edit notes, chords, and dynamics for the selected region inside the lower editor pane."),
       N_("Click to draw notes or drag a selection box to transpose a whole phrase together.")},
      {TourTarget::Mixer, "mixer", N_("Mixer"),
       N_("Mix every channel strip side by side with faders, meters, pan knobs, sends, and insert effects."),
       N_("Click an insert slot on any channel strip to add EQ, compression, or reverb.")},
  };
  static const std::vector<TourStep> kTracker = {
      {TourTarget::TrackerHeader, "header", N_("Song header and order list"),
       N_("Control playback, tempo, pattern sequence order, instruments, and page views from the top command band."),
       N_("Click any page tab in the header to jump between Pattern, Song, Roll, Steps, and Mix.")},
      {TourTarget::PatternEditor, "pattern", N_("Pattern editor"),
       N_("Enter notes, instrument numbers, volume, and effect commands row by row directly from your keyboard."),
       N_("Press Esc to toggle Edit mode, then play notes with your typing keyboard to write rows.")},
      {TourTarget::Browser, "browser", N_("Browser and files"),
       N_("Load samples, instruments, and presets straight into your instrument slots or browse your project library."),
       N_("Click any sound to preview it before loading it into the selected instrument slot.")},
      {TourTarget::PianoRoll, "piano", N_("Roll view"),
       N_("Switch to the visual piano roll whenever you want to sketch chords, melodies, or note lengths visually."),
       N_("Edits here stay completely in sync with the pattern editor rows.")},
      {TourTarget::Playlist, "playlist", N_("Song arrangement"),
       N_("View and arrange your patterns along the full song timeline alongside audio clips and automation."),
       N_("Place patterns on tracks or drag clips to structure your intro, drop, and outro.")},
      {TourTarget::Mixer, "mixer", N_("Mix view"),
       N_("Balance channel strips, route tracks, and add effect chains across your whole mix."),
       N_("Adjust faders or open an effect slot on any insert track to sculpt your sound.")},
  };
  switch (preset) {
    case layout::Preset::Live: return kLive;
    case layout::Preset::Logic: return kLogic;
    case layout::Preset::Tracker: return kTracker;
    default: return kStudio;
  }
}

bool isSongEmpty(const Song& song) {
  if (!song.playlist.empty()) return false;
  for (const Pattern& pat : song.patterns) {
    for (const auto& [ch, notes] : pat.notes)
      if (!notes.empty()) return false;
    for (const auto& [ch, steps] : pat.steps)
      for (uint8_t v : steps)
        if (v > 0) return false;
  }
  return true;
}

Rect clampSpotlightRect(const Rect& viewport, const Rect& target, float pad, float edge) {
  const float vx0 = viewport.x + edge;
  const float vy0 = viewport.y + edge;
  const float vx1 = std::max(vx0 + 1.0f, viewport.x + viewport.w - edge);
  const float vy1 = std::max(vy0 + 1.0f, viewport.y + viewport.h - edge);
  const float x0 = std::clamp(target.x - pad, vx0, vx1 - 1.0f);
  const float y0 = std::clamp(target.y - pad, vy0, vy1 - 1.0f);
  const float x1 = std::clamp(target.x + target.w + pad, x0 + 1.0f, vx1);
  const float y1 = std::clamp(target.y + target.h + pad, y0 + 1.0f, vy1);
  return {x0, y0, x1 - x0, y1 - y0};
}

CardPlacement placeCalloutCard(const Rect& viewport, const Rect& spotlight, float cardW, float cardH, float margin, float gap) {
  const float minX = viewport.x + margin;
  const float minY = viewport.y + margin;
  const float maxX = std::max(minX, viewport.x + viewport.w - margin - cardW);
  const float maxY = std::max(minY, viewport.y + viewport.h - margin - cardH);

  const float spRight = spotlight.x + spotlight.w;
  const float spBottom = spotlight.y + spotlight.h;
  const float spCx = spotlight.x + spotlight.w * 0.5f;
  const float spCy = spotlight.y + spotlight.h * 0.5f;

  // Compute clear space on each side of the spotlight inside the viewport.
  const float spaceRight = (viewport.x + viewport.w - margin) - (spRight + gap);
  const float spaceLeft = (spotlight.x - gap) - (viewport.x + margin);
  const float spaceBottom = (viewport.y + viewport.h - margin) - (spBottom + gap);
  const float spaceTop = (spotlight.y - gap) - (viewport.y + margin);

  struct Cand {
    CardSide side;
    bool fits;
    float slack;
    float x;
    float y;
    float ax;
    float ay;
  };
  const float alignY = std::clamp(spCy - cardH * 0.5f, minY, maxY);
  const float alignX = std::clamp(spCx - cardW * 0.5f, minX, maxX);

  const Cand cands[4] = {
      {CardSide::Right, spaceRight >= cardW, spaceRight - cardW, std::clamp(spRight + gap, minX, maxX), alignY, spRight,
       std::clamp(alignY + cardH * 0.5f, spotlight.y + 12.0f, std::max(spotlight.y + 12.0f, spBottom - 12.0f))},
      {CardSide::Left, spaceLeft >= cardW, spaceLeft - cardW, std::clamp(spotlight.x - gap - cardW, minX, maxX), alignY, spotlight.x,
       std::clamp(alignY + cardH * 0.5f, spotlight.y + 12.0f, std::max(spotlight.y + 12.0f, spBottom - 12.0f))},
      {CardSide::Bottom, spaceBottom >= cardH, spaceBottom - cardH, alignX, std::clamp(spBottom + gap, minY, maxY),
       std::clamp(alignX + cardW * 0.5f, spotlight.x + 12.0f, std::max(spotlight.x + 12.0f, spRight - 12.0f)), spBottom},
      {CardSide::Top, spaceTop >= cardH, spaceTop - cardH, alignX, std::clamp(spotlight.y - gap - cardH, minY, maxY),
       std::clamp(alignX + cardW * 0.5f, spotlight.x + 12.0f, std::max(spotlight.x + 12.0f, spRight - 12.0f)), spotlight.y},
  };

  // Prefer the first fitting candidate with the best clear room (Right > Left > Bottom > Top on ties).
  int best = -1;
  for (int i = 0; i < 4; ++i) {
    if (!cands[i].fits) continue;
    if (best < 0 || cands[i].slack > cands[best].slack + 24.0f) best = i;
  }

  CardPlacement out;
  if (best >= 0) {
    out.card = {std::round(cands[best].x), std::round(cands[best].y), cardW, cardH};
    out.side = cands[best].side;
    out.anchorX = cands[best].ax;
    out.anchorY = cands[best].ay;
    return out;
  }

  // Fallback: spotlight fills almost the whole viewport -> cleanly inset at bottom-right.
  out.card = {std::round(maxX), std::round(maxY), cardW, cardH};
  out.side = CardSide::Inset;
  out.anchorX = spCx;
  out.anchorY = spCy;
  return out;
}

}  // namespace gl::setuptour
