// Gridline - K-105: First-run setup wizard & interactive guided tour core logic.
// Pure data, trigger decisions, theme look presets, layout-adaptive tour steps,
// and callout card placement geometry (no ImGui/SDL; unit-testable headless).
#pragma once
#include <string>
#include <vector>
#include "LayoutPreset.h"
#include "Workspace.h"

namespace gl {
class Settings;
struct Song;
}

namespace gl::setuptour {

// ---- first-run trigger policy ----
struct FirstRunInputs {
  bool setupDone = false;     // ui.setupDone
  bool layoutChosen = false;  // ui.layoutChosen (treated as done for upgrades from <=0.7.8)
  bool plainLaunch = false;   // launched without CLI args / project files / OS open-doc
  bool hidden = false;        // --hidden
  bool quietWindows = false;  // GRIDLINE_QUIET_WINDOWS=1
  bool automated = false;     // --screenshot, --profile-frames, --dev-input, --menu, --show, etc.
  bool embedded = false;      // running inside another host (plugin build)
  bool hasPluginArg = false;  // --plugin
};

// True when a brand-new user is launching interactively and has not completed setup yet.
// Existing users upgrading with ui.layoutChosen=true, and all automated/quiet/headless/CLI
// launches, return false so they are never blocked by a modal wizard.
bool shouldShowFirstRunWizard(const FirstRunInputs& in);

// True when settings already indicate first-run setup is complete (including upgrades).
bool isSetupDone(const Settings& s);

// Marks setup as complete in settings (sets both ui.setupDone and ui.layoutChosen).
void markSetupDone(Settings& s);

// ---- wizard steps ----
enum class WizardStep : int { Workspace = 0, Look = 1, Audio = 2, Ready = 3, Count = 4 };
constexpr int kWizardStepCount = static_cast<int>(WizardStep::Count);

// ---- Step 2: Look & Theme presets ----
enum class LookPreset : int { Dark = 0, Light = 1, Night = 2, Tinted = 3, Count = 4 };
constexpr int kLookPresetCount = static_cast<int>(LookPreset::Count);

struct LookSpec {
  LookPreset preset = LookPreset::Dark;
  const char* id = "";
  const char* name = "";   // translatable (N_)
  const char* blurb = "";  // translatable (N_)
  bool dark = true;
  float brightness = 0.0f;
  float contrast = 0.0f;
  float hue = 0.0f;
  float saturation = 0.0f;
  bool whiteText = false;
};

const LookSpec& lookSpec(LookPreset p);
// Writes the preset's mode and tweak values into settings (does not trigger Keep/Revert).
void applyLookToSettings(LookPreset p, Settings& s);
// Best-matching preset for the current settings (when reopening the wizard).
LookPreset detectLookPreset(const Settings& s);

// ---- Interactive Guided Tour ----
enum class TourTarget : int {
  Transport = 0,
  Browser,
  ChannelRack,
  PianoRoll,
  Playlist,
  Mixer,
  DeviceChain,
  Inspector,
  TrackerHeader,
  PatternEditor,
};

struct TourStep {
  TourTarget target = TourTarget::Transport;
  const char* id = "";
  const char* title = "";  // translatable (N_)
  const char* body = "";   // translatable (N_)
  const char* tip = "";    // translatable (N_) — short "Try it" action
};

// Layout-adaptive sequence of tour steps: every step highlights a real panel present
// in `preset` (Studio, Clips/Live, Smooth/Logic, Tracker).
const std::vector<TourStep>& tourStepsFor(layout::Preset preset);

// True when a song has no notes, no active steps, and no playlist clips.
bool isSongEmpty(const Song& song);

// ---- Spotlight & Callout Card Placement Geometry ----
using Rect = workspace::Rect;

enum class CardSide : int { Right = 0, Left = 1, Bottom = 2, Top = 3, Inset = 4 };

struct CardPlacement {
  Rect card{};
  CardSide side = CardSide::Right;
  float anchorX = 0.0f;  // point on the spotlight edge the card points toward
  float anchorY = 0.0f;
};

// Expands `target` by `pad` and clamps strictly inside `viewport` (with outer margin `edge`).
Rect clampSpotlightRect(const Rect& viewport, const Rect& target, float pad = 4.0f, float edge = 2.0f);

// Places a floating callout card (`cardW` x `cardH`) inside `viewport` (respecting `margin`)
// without overlapping `spotlight` (separated by `gap`) whenever space permits; falls back
// to a clean inset corner if `spotlight` covers almost the entire viewport.
CardPlacement placeCalloutCard(const Rect& viewport, const Rect& spotlight, float cardW, float cardH, float margin = 16.0f,
                               float gap = 16.0f);

}  // namespace gl::setuptour
