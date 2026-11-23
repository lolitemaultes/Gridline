// Gridline - the Novation Launchpad Pro [MK3] as a control surface, from Novation's published
// Programmer's Reference Guide and User Guide (no other software's scripts).
//
// Gridline talks to the unit's DAW port only. Connecting puts the unit in DAW mode and selects its
// Session layout, where Gridline draws its own pages: Session (the layout's clips: Clips' clip grid,
// an arrangement overview in Studio and Smooth, the patterns in the Tracker), Keys (the selected
// instrument in the piano roll's key), Drums (four 4x4 blocks) and Steps (the selected channel's step
// row). Volume, Pan, Sends and Device open the unit's own DAW fader layout. The unit's Note, Chord,
// Custom and Sequencer modes stay its own (they play on its MIDI port, an ordinary input); with the
// option on, Note and Sequencer bring up Gridline's Keys / Drums and Steps pages instead.
// Disconnecting gives the unit back its standalone mode.
//
// The Controller holds the page state, Shift and the held buttons; it reads Gridline through a Model
// (implemented by the app per layout, by a fake in the tests) and writes MIDI through `out`.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "../Remote.h"
#include "Surface.h"

namespace gl::surfaces::lpp3 {

// ---- protocol (Programmer's Reference Guide) ----
inline constexpr uint8_t kHeader[6] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0E};
enum Cmd : uint8_t {
  CmdLayout = 0x00, CmdFaderSetup = 0x01, CmdLed = 0x03, CmdProgrammer = 0x0E, CmdDrumrack = 0x0F,
  CmdDawMode = 0x10, CmdNoteColour = 0x17, CmdStopFaders = 0x19
};
enum Layout : uint8_t { LSession = 0, LFader = 1, LChord = 2, LCustom = 3, LNote = 4, LScale = 5, LSeqFirst = 6, LSeqLast = 16, LProgrammer = 17, LSettings = 18 };
// Buttons (CC numbers in the Session and Programmer layouts).
enum Cc : uint8_t {
  CShift = 90, CLeft = 91, CRight = 92, CSession = 93, CNote = 94, CChord = 95, CCustom = 96, CSequencer = 97, CProjects = 98,
  CLogo = 99, CUp = 80, CDown = 70, CClear = 60, CDuplicate = 50, CQuantise = 40, CFixedLength = 30, CPlay = 20, CRecord = 10,
  CRecordArm = 1, CMute = 2, CSolo = 3, CVolume = 4, CPan = 5, CSends = 6, CDevice = 7, CStopClip = 8,
  CTrackSelect = 101,  // .. 108
  CScene = 89          // top scene button; 79, 69 .. 19 below it
};
// Palette entries used for Gridline's own signals.
enum Colour : uint8_t { KOff = 0, KDimWhite = 1, KWhite = 3, KRed = 5, KDimRed = 7, KAmber = 9, KDimAmber = 11, KGreen = 21, KDimGreen = 23, KBlue = 45, KDimBlue = 47, KGold = 96 };

Bytes sysex(std::initializer_list<uint8_t> body);  // header + body + F7
Bytes dawMode(bool on);                             // 10 <0|1>
Bytes selectLayout(int layout, int page);           // 00 <layout> <page> 00
Bytes deviceInquiry();                              // universal F0 7E 7F 06 01 F7
struct FaderSpec {
  int index = 0;  // 0..7
  bool bipolar = false;
  int cc = 0;
  int colour = 0;  // palette entry; 0 hides the fader
};
Bytes faderSetup(int bank, bool horizontal, const std::vector<FaderSpec>& faders);
// The device's 128 colours (the guide's colour chart, its pastel swatches stretched back to full range).
const std::array<Rgb, 128>& palette();
// Grid pad number (11..88) of column c (0 left) and row r (0 top), and back. False: not a grid pad.
inline int padOf(int c, int r) { return (8 - r) * 10 + c + 1; }
inline bool isPad(int n) { return n >= 11 && n <= 88 && n % 10 >= 1 && n % 10 <= 8; }
inline int padCol(int n) { return n % 10 - 1; }
inline int padRow(int n) { return 8 - n / 10; }
// Port names: the DAW port of a Launchpad Pro [MK3] (macOS / Linux "LPProMK3 DAW", Windows its third interface).
bool isDawPort(const std::string& name);
bool isLaunchpadPort(const std::string& name);  // any of its ports
// The output to answer on for the DAW input `in` (Windows names its outputs MIDIOUTn for MIDIINn).
std::string outputFor(const std::string& in, const std::vector<std::string>& outputs);

// ---- what Gridline shows ----
enum class Page : uint8_t { Session = 0, Keys, Drums, Steps, Faders, Device };  // Device: the unit's own mode is showing
enum class TrackMode : uint8_t { Select = 0, Arm, Mute, Solo, Stop };
enum class Mod : uint8_t { None = 0, Shift, Clear, Duplicate, Double, Quantise };
enum class State : uint8_t {
  Empty = 0,     // off
  Filled,        // a clip (static, its colour)
  Playing,       // pulses in its colour
  Queued,        // flashes green over its colour
  Recording,     // pulses red
  RecordQueued,  // flashes red
  ArmedEmpty,    // dim red
  Dim,           // its colour, dimmed
  Selected,      // white
  Marker         // dim white (the play position, an empty bar under it)
};
struct Cell {
  State state = State::Empty;
  Rgb colour = 0;
};
enum class Button : uint8_t { PlayPause, Stop, Record, Capture, Undo, Redo, Click, TapTempo, StopAll, LoopRecord };
struct Transport {
  bool playing = false, recording = false, click = false, loopRecord = false;
};
struct Keys {
  int root = 0;              // 0..11
  uint16_t scale = 0x0FFF;   // bit k: root + k semitones is in the key (0x0FFF: no key, chromatic)
  Rgb colour = 0x4C8DFF;     // the channel's
  bool drums = false;        // a drum instrument: the Drums page
};
struct Steps {
  int count = 0;                 // steps in the row
  std::vector<uint8_t> velocity;  // per step, 0 = off
  int playhead = -1;
  Rgb colour = 0x4C8DFF;
};
struct Fader {
  bool shown = false;
  double value = 0;  // 0..1 (bipolar: 0.5 = centre)
  bool bipolar = false;
  Rgb colour = 0;
};
enum class Bank : uint8_t { Volume = 0, Pan, Sends, Device };

class Model {
 public:
  virtual ~Model() = default;
  // Session page. Columns and rows in the model's own numbering (the controller scrolls the view).
  virtual int sessionCols() const = 0;
  virtual int sessionRows() const = 0;
  virtual Cell sessionCell(int col, int row) const = 0;
  virtual void sessionPress(int col, int row, Mod mod) = 0;
  virtual void sessionRelease(int col, int row) { (void)col, (void)row; }
  // The scene column beside row `row`, and the Track Select button under column `col`.
  virtual Cell sideCell(int row, TrackMode m) const = 0;
  virtual void sidePress(int row, TrackMode m, bool shift) = 0;
  virtual Cell bottomCell(int col, TrackMode m) const = 0;
  virtual void bottomPress(int col, TrackMode m) = 0;
  // Transport and the printed buttons.
  virtual Transport transport() const = 0;
  virtual void button(Button b) = 0;
  // Channels (Keys, Drums, Steps): the rack in order.
  virtual int channelCount() const = 0;
  virtual Rgb channelColour(int i) const = 0;
  virtual int selectedChannel() const = 0;
  virtual void selectChannel(int i) = 0;
  virtual Keys keys() const = 0;
  virtual void shiftRoot(int by) = 0;
  virtual Steps steps() const = 0;
  virtual void toggleStep(int i) = 0;
  virtual void setStepLength(int n) = 0;
  virtual int patternCount() const = 0;
  virtual Rgb patternColour(int i) const = 0;
  virtual int selectedPattern() const = 0;
  virtual void selectPattern(int i) = 0;
  // Faders: bank, absolute index (track / macro). setFader: a move by hand (0..1).
  virtual int faderCount(Bank b) const = 0;
  virtual Fader fader(Bank b, int index) const = 0;
  virtual void setFader(Bank b, int index, double v) = 0;
  // The Session view's first column, when the faders follow it (Clips: columns are tracks). -1: no.
  virtual bool fadersFollowSession() const { return false; }
};

struct Options {
  bool modePages = true;   // Note / Sequencer bring up Gridline's pages
  bool keysPage = true;    // Keys and Drums pages
  bool stepsPage = true;
  bool faders = true;
  int velocity = 0;        // VelocityCurve: 0 as played, 1 soft, 2 hard, 3 fixed
  int brightness = 0;      // 0 full, 1 medium, 2 low
  bool exactColours = false;  // RGB SysEx for static colours
  bool operator==(const Options& o) const {
    return modePages == o.modePages && keysPage == o.keysPage && stepsPage == o.stepsPage && faders == o.faders &&
           velocity == o.velocity && brightness == o.brightness && exactColours == o.exactColours;
  }
  // "key=value;..." (as kept in midi.json), unknown keys ignored.
  std::string save() const;
  static Options load(const std::string& s);
};

class Controller {
 public:
  Controller(Model& m, Options o, Out out);
  // The handshake: DAW mode, the Session layout, the fader banks, every LED again.
  void connect();
  // Gives the unit back its standalone mode.
  void disconnect();
  bool connected() const { return connected_; }
  // A message from the DAW port (true when used: always, for this port).
  bool onMidi(const MidiMessage& m, double now);
  void onSysex(const Bytes& b, double now);
  // About 30 times a second: works out every LED and fader and sends what changed.
  void idle(double now);

  Page page() const { return page_; }
  void setPage(Page p, double now = 0);
  Bank bank() const { return bank_; }
  TrackMode trackMode() const { return trackMode_; }
  int colOffset() const { return col0_; }
  int rowOffset() const { return row0_; }
  bool shift() const { return shift_; }
  const Options& options() const { return opt_; }
  void setOptions(const Options& o);
  const std::string& firmware() const { return firmware_; }  // "1.2.1" once the unit answered
  int lastLayout() const { return layout_; }                  // the layout the unit reported last (-1: none)

  // Keys / Drums: the pitch each grid pad plays (note number 11..88 -> pitch, -1: none). serial()
  // changes whenever the map does, so the app republishes it to the MIDI driver.
  const std::array<int16_t, 128>& padPitch() const { return pads_; }
  uint64_t padSerial() const { return padSerial_; }
  int octave() const { return octave_; }
  int drumBase() const { return drumBase_; }

  static constexpr int kMaxPerIdle = 96;  // LED messages per round
  static constexpr int kFaderCcBase = 20;  // fader CCs: 20 + bank * 8 + index

 private:
  void press(int cc, bool down, double now);
  void padPress(int n, bool down);
  void computeLeds();
  Led cellLed(const Cell& c) const;
  Led colour(Rgb c, float f = 1.0f) const;
  int pal(Rgb c, float f = 1.0f) const;
  void sendLeds();
  void computeFaders(double now);
  void computePads();
  void requestLayout(int layout, int page, double now);
  void arrow(int dx, int dy);
  void openFaders(Bank b, double now);
  Mod mod() const;
  int faderOffset() const;

  Model& m_;
  Options opt_;
  Out out_;
  bool connected_ = false;
  Page page_ = Page::Session, before_ = Page::Session;
  Bank bank_ = Bank::Volume;
  TrackMode trackMode_ = TrackMode::Select;
  int col0_ = 0, row0_ = 0;     // Session view
  int chan0_ = 0;               // Keys / Drums / Steps: first channel in view
  int step0_ = 0, pat0_ = 0;    // Steps
  int fad0_ = 0;                // Faders (when not following the Session view)
  int octave_ = 4, shiftKey_ = 0, drumBase_ = 36, drumHit_ = -1;
  bool shift_ = false, clear_ = false, dup_ = false, quant_ = false;
  double bankPressAt_ = -1;     // Volume .. Device pressed (momentary when held)
  bool bankOpened_ = false;
  std::array<bool, 128> held_{};  // grid pads down (Keys / Drums: lit white)
  LedCache leds_;
  std::array<int16_t, 128> pads_{};
  uint64_t padSerial_ = 1;
  // Faders: the set-up sent per bank and the positions sent / last touched.
  std::array<std::vector<FaderSpec>, 4> faderSent_;
  std::array<std::array<int, 8>, 4> posSent_{};
  std::array<std::array<double, 8>, 4> touched_{};
  int layout_ = -1;
  int expectLayout_ = -1;
  double expectUntil_ = 0;
  std::string firmware_;
};

}  // namespace gl::surfaces::lpp3
