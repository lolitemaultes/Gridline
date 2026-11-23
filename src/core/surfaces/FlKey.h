// Gridline - Novation FLkey keyboards as full controllers (k078-flkey).
//
// Models: the first generation (FLkey Mini, 37, 49, 61) and FLkey 2 (Mini 25, 37, 49, 61). Each has
// two USB ports: the MIDI port (keys, wheels, pedal, the keyboard's own drum / chord / custom
// layouts) plays like any keyboard; the DAW port is a control surface. On connect Gridline puts the
// keyboard in its DAW mode and drives it from the song: the pads show the selected channel's steps
// (or the rack's channels, the patterns, or a playable instrument layout), the knobs and faders move
// mixer and channel volume and pan or the focused plugin's parameters (the selected channel's macro
// knobs when no plugin window is open), the buttons run the transport, undo, metronome, score log,
// quantize and the channel / mixer / pattern selection, and the screen shows the name and value of
// what a control moves. Leaving (quit, unplug, another controller type) returns it to standalone.
//
// Novation publishes no programmer's guide for the FLkey. The DAW-mode numbers here follow the
// guides Novation publishes for the Launchkey families the FLkey is built on (first generation:
// Launchkey [MK3]; FLkey 2: Launchkey MK4); they are unverified on FLkey hardware, and every one of
// them can be corrected per model with a JSON file (applyOverrides). FLkey 2 also documents HUI on
// its DAW port: when the keyboard talks HUI there, `huiSeen()` turns true and the app hands the port
// to its HUI surface instead.
//
// Every edit goes through Document (one undo step each, synced in a shared session); selection,
// transport and notes through the Host.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "../ControllerScript.h"
#include "../Remote.h"

namespace gl::flkey {

enum class Gen : uint8_t { First, Second };  // FLkey, FLkey 2

struct Model {
  const char* id;    // controller type id ("flkey-37", "flkey2-61")
  const char* name;  // shown in Settings ("Novation FLkey 37")
  const char* port;  // what its USB port names contain ("FLkey 37", "FLkey 2 61")
  Gen gen;
  bool mini;    // Mini layout: no page buttons, no faders
  bool faders;  // 49 / 61: nine faders and nine fader buttons
  bool screen;  // a text screen (all but the first FLkey Mini)
};
const std::vector<Model>& models();
const Model* modelById(const std::string& id);

// A MIDI port name: the FLkey model it belongs to and whether it is the DAW port.
struct PortMatch {
  const Model* model = nullptr;
  bool daw = false;
  // The name carries no size ("FLkey DAW Out", as Novation's guides write it): the full-size model of
  // that generation (49) is taken, the superset of the 37's controls; Settings picks the exact one.
  bool sizeGuessed = false;
};
PortMatch matchPort(const std::string& portName);
// The output that pairs with an input of the keyboard ("FLkey 37 DAW Out" -> "FLkey 37 DAW In",
// "MIDIIN2 (FLkey 37)" -> "MIDIOUT2 (FLkey 37)"); empty when none is listed.
std::string pairedOutput(const std::string& input, const std::vector<std::string>& outputs);

// What a button does in Gridline.
enum class Button : uint8_t {
  Play, Stop, Record, Loop, ScoreLog, Quantize, Metronome, Undo, Redo, PatternSong, Shift,
  ChannelUp, ChannelDown, MixerLeft, MixerRight, PageLeft, PageRight, KnobBankUp, KnobBankDown, Solo,
  Count
};
const char* buttonId(Button b);  // "play", "channelUp" (override files)

// The pads' page (host pages are drawn by Gridline; Device = the keyboard's own layout).
enum class PadPage : uint8_t { Steps, Channels, Patterns, Instrument, Device };
const char* padPageName(PadPage p);
// What the eight knobs move.
enum class KnobMode : uint8_t { Plugin, MixerVolume, MixerPan, ChannelVolume, ChannelPan, Transport, Device };
const char* knobModeName(KnobMode k);
// What the faders move (49 / 61).
enum class FaderMode : uint8_t { MixerVolume, ChannelVolume, Plugin, Device };

// The wire numbers of one model (defaults: defaultWire; corrections: applyOverrides).
struct Wire {
  uint8_t sysexId = 0x0F;     // F0 00 20 29 02 <sysexId> ...
  uint8_t padTop = 0x60;      // DAW layout pads: notes on channel 1, top row padTop.., bottom padBottom..
  uint8_t padBottom = 0x70;
  uint8_t knobCc = 0x15;      // eight knobs, CC on channel 16
  uint8_t faderCc = 0x35;     // nine faders (49 / 61)
  uint8_t faderButtonCc = 0x25;  // nine fader buttons
  bool relativeTransport = false;  // FLkey 2: the transport knob mode sends relative CCs
  uint8_t transportCc = 0x55;
  std::array<int, static_cast<size_t>(Button::Count)> buttons{};  // CC per button, -1 = none
  int modeChannel = 15;       // channel (0-based) of the mode report / select CCs
  int padModeCc = 3, knobModeCc = 9, faderModeCc = 10;
  int padDawValue = 2;        // the pads' DAW layout
  std::map<int, PadPage> padModes;      // reported pad mode value -> page (missing: logged, unchanged)
  std::map<int, KnobMode> knobModes;    // reported knob mode value -> mode
  std::map<int, FaderMode> faderModes;  // reported fader mode value -> mode
  bool rgb = false;           // pads and fader buttons take RGB sysex (else the palette)
  bool oled = false;          // FLkey 2 screen messages (configure + text), else the 2 x 16 LCD ones
  // Scale: FLkey 2 feature controls (channel 7) or first generation CCs (channel 16).
  int scaleChannel = 15, scaleOnCc = 14, scaleTypeCc = 15, scaleRootCc = 16;
  std::vector<std::string> scaleNames;  // the keyboard's scale list (type value = index)
};
Wire defaultWire(const Model& m);
// Corrections from a JSON object {"<model id>": {"sysexId": 15, "padTop": 96, "buttons": {"play": 115}, ...}}
// ("*" applies to every model). False + message when the text is not JSON.
bool applyOverrides(Wire& w, const Model& m, const std::string& json, std::string* err = nullptr);

// The nearest palette entry to a colour (first generation LEDs take palette indexes only).
int paletteIndex(uint32_t rgb);
uint32_t paletteColour(int index);

// What the surface needs from the app beyond a controller host.
class Host : public script::ControllerHost {
 public:
  using ControllerHost::ControllerHost;
  virtual ObjectId currentPattern() const = 0;
  virtual void selectPattern(ObjectId pattern) = 0;
  virtual void note(ObjectId channel, int pitch, int velocity) = 0;  // velocity 0 = off
  virtual int scoreLog() = 0;  // puts the notes played lately into the pattern; returns how many
  virtual double now() const = 0;  // seconds (holds, throttling)
  // Preset up / down: the next (`by` > 0) or previous preset of the selected channel's instrument, as
  // the plugin window's arrows load it. Returns its name (empty: nothing to step to).
  virtual std::string stepPreset(int by) = 0;
};

// A host for tests: transport and selection of its own, notes and score log recorded.
class TestHost : public Host {
 public:
  using Host::Host;
  void action(RemoteAction a) override;
  bool playing() const override { return isPlaying; }
  bool recording() const override { return isRecording; }
  bool songMode() const override { return isSongMode; }
  Tick position() const override { return pos; }
  void setPosition(Tick t) override { pos = t; }
  int selectedMixer() const override { return mixerSel; }
  void selectMixer(int index) override { mixerSel = index; }
  bool armed(int) const override { return false; }
  void setArmed(int, bool) override {}
  int selectedChannel() const override { return channelSel; }
  void selectChannel(int index) override { channelSel = index; }
  std::pair<ObjectId, int> focusedPlugin() const override { return focus; }
  void midiOut(uint8_t, uint8_t, uint8_t) override {}
  void sysexOut(const std::vector<uint8_t>&) override {}
  void hint(const std::string& text) override { hints.push_back(text); }
  ObjectId currentPattern() const override { return pattern; }
  void selectPattern(ObjectId p) override { pattern = p; }
  void note(ObjectId ch, int pitch, int velocity) override { notes.push_back({ch, pitch, velocity}); }
  int scoreLog() override { return ++scoreLogs, 0; }
  double now() const override { return clock; }
  std::string stepPreset(int by) override {
    presetSteps.push_back(by);
    return presetName.empty() ? std::string() : presetName + " " + std::to_string(static_cast<int>(presetSteps.size()));
  }

  std::vector<int> presetSteps;
  std::string presetName = "Preset";
  bool isPlaying = false, isRecording = false, isSongMode = false, metronome = false;
  Tick pos = 0;
  int mixerSel = 0, channelSel = -1, scoreLogs = 0;
  ObjectId pattern = kNoId;
  std::pair<ObjectId, int> focus{kNoId, -1};
  struct Played {
    ObjectId channel;
    int pitch, velocity;
  };
  std::vector<Played> notes;
  std::vector<RemoteAction> actions;
  std::vector<std::string> hints;
  double clock = 0;
};

class Surface {
 public:
  using Out = std::function<void(const std::vector<uint8_t>& bytes)>;  // to the DAW port
  Surface(Host& host, const Model& model, Wire wire, Out out);

  void connect();     // DAW mode on, areas set up, everything drawn
  void disconnect();  // the keyboard back to standalone (DAW mode off)
  bool connected() const { return connected_; }
  // A message from the DAW port: true when the surface used it.
  bool onMidi(const MidiMessage& m);
  // About 30 times a second: sends what changed (pads, buttons, screen, scale), nothing else.
  void idle(double now);
  // Sends everything again on the next idle.
  void invalidate();

  const Model& model() const { return model_; }
  const Wire& wire() const { return wire_; }
  bool huiSeen() const { return huiSeen_; }
  PadPage page() const { return page_; }
  KnobMode knobMode() const { return knobMode_; }
  FaderMode faderMode() const { return faderMode_; }
  int stepPage() const { return stepPage_; }
  int channelBank() const { return channelBank_; }
  int mixerBank() const { return mixerBank_; }
  int patternBank() const { return patternBank_; }
  int knobBank() const { return knobBank_; }
  bool soloMode() const { return soloMode_; }
  int octave() const { return octave_; }
  // The colour each pad shows (top row 0..7, bottom row 8..15; 0 = off), as last drawn.
  uint32_t padColour(int pad) const { return pad >= 0 && pad < 16 ? padShown_[static_cast<size_t>(pad)] : 0; }
  // What the screen shows for knob k (0..7) or fader k (8..16): name and value, as last sent.
  std::pair<std::string, std::string> controlText(int k) const;
  // The knob k's target name and value text now (tests, the screen).
  std::pair<std::string, std::string> knobLabel(int k) const;

  // The pad index (0..15) of a DAW layout note, -1 when not a pad.
  int padOf(int note) const;
  int padNote(int pad) const;

 private:
  struct Target {  // what a knob or fader moves
    ParamId pid{kNoId, 0};
    std::string name;
    bool macro = false;  // a channel macro knob (LayerMacro)
  };
  void sendBytes(std::vector<uint8_t> b) { out_(b); }
  void sendSysex(std::initializer_list<uint8_t> body, const std::string& text = {});
  void selectModes();
  void pressPad(int pad, int velocity);
  void releasePad(int pad);
  void button(Button b, bool down);
  void turnKnob(int k, int value);
  void moveFader(int k, int value);
  void faderButton(int k);
  void transportKnob(int k, int delta);
  void stepKnob(int k, int value);  // knobs while steps are held
  Target knobTarget(int k) const;
  Target target(KnobMode mode, int k) const;
  Target faderTarget(int k) const;
  void showControl(int idx, const Target& t);
  void flash(const std::string& line1, const std::string& line2);
  std::array<uint32_t, 16> drawPads() const;
  uint32_t buttonColour(Button b) const;
  void drawScreen();
  void syncScale();
  int stepCount() const;
  int playStep() const;
  ObjectId channelId(int rackIndex) const;
  void moveChannel(int by);
  void moveMixer(int by);
  void stepPreset(int by);  // the selected channel's next / previous preset, its name on the screen
  int instrumentPitch(int pad) const;

  Host& host_;
  const Model& model_;
  Wire wire_;
  Out out_;
  bool connected_ = false;
  bool huiSeen_ = false;
  bool shift_ = false;
  PadPage page_ = PadPage::Steps;
  PadPage lastHostPage_ = PadPage::Steps;
  KnobMode knobMode_ = KnobMode::Plugin;
  FaderMode faderMode_ = FaderMode::MixerVolume;
  int stepPage_ = 0, channelBank_ = 0, mixerBank_ = 0, patternBank_ = 0, knobBank_ = 0, octave_ = 0;
  bool soloMode_ = false;
  double lastSolo_ = -10;
  std::array<double, 16> heldSince_{};  // steps held (Steps page), 0 = not held
  std::array<int, 16> padPitch_{};      // Instrument page: the pitch a held pad plays, -1 none
  bool stepEdited_ = false;             // a held step was edited with a knob (no toggle on release)
  std::array<int, 17> pickup_{};        // absolute knobs / faders: -1 picked up, else the side it started on
  // What the keyboard shows now (unknown entries are sent on the next idle).
  std::array<uint32_t, 16> padShown_{};
  std::array<bool, 16> padKnown_{};
  std::map<int, int> buttonShown_;  // CC -> colour (palette index or RGB | 1 << 24)
  std::array<std::pair<std::string, std::string>, 17> textShown_{};
  std::array<bool, 17> textKnown_{};
  std::string screenTop_, screenBottom_;
  bool screenKnown_ = false;
  int scaleRoot_ = -1, scaleType_ = -1;
  double lastIdle_ = -1;
  double flashUntil_ = 0;
  std::string flashTop_, flashBottom_;
  std::map<std::string, int> unknownLogged_;
};

}  // namespace gl::flkey
