// Gridline — controller scripts (SC-03): a Lua script per MIDI input device,
// an alternative to J-17's JSON controller templates. Settings → MIDI picks
// "Script: <name>" as the device's controller type.
//
// Hooks the script may define (all optional):
//   onInit()             once after loading
//   onMidiMsg(msg)       every message of the device; return true (or set
//                        msg.handled = true) when the script used it, so
//                        Gridline does not also play / link it
//   onIdle()             about 50 times a second
//   onRefresh(flags)     something changed: flags is a table
//                        {mixer, channels, transport, selection, plugin} of booleans
//   onDeinit()           before the script is unloaded
// msg = {status, data1, data2, channel (1..16), kind ("noteOn", "noteOff",
// "cc", "pitchBend", "program", "keyPressure", "pressure", "other"), note,
// velocity, cc, value, bend (-8192..8191), port}.
//
// Modules (indices: mixer 0 = master, 1.. = inserts; channels 1.. = rack order):
//   transport.play() stop() toggle() record() isPlaying() isRecording()
//            songMode() setSongMode(b) tempo() setTempo(bpm) position()
//            setPosition(ticks) ppq() action(name)   -- name: templateActionKey()
//   mixer.count() name(i) volume(i) setVolume(i, v) pan(i) setPan(i, v)
//        muted(i) setMuted(i, b) solo(i) setSolo(i, b) armed(i) setArmed(i, b)
//        selected() select(i)        -- volume / pan normalized 0..1 (pan 0.5 = centre)
//   channels.count() name(i) selected() select(i) volume(i) setVolume(i, v)
//           pan(i) setPan(i, v) muted(i) setMuted(i, b)
//   plugin.name() paramCount() paramName(k) param(k) setParam(k, v) paramText(k)
//                                    -- focused plugin window; k 1-based, v 0..1
//   device.send(status, d1, d2) sendSysex({bytes}) led(note, on [, channel 1..16])
//          name()
//   ui.hint(text)
// Every edit goes through Document (undoable; controller moves merge until a pause).
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "Document.h"
#include "Remote.h"
#include "Script.h"

namespace gl::script {

// What a controller script can reach besides the Document.
class ControllerHost {
 public:
  explicit ControllerHost(Document& d) : doc(d) {}
  virtual ~ControllerHost() = default;
  Document& doc;
  virtual void action(RemoteAction a) = 0;
  virtual bool playing() const = 0;
  virtual bool recording() const = 0;
  virtual bool songMode() const = 0;
  virtual Tick position() const = 0;
  virtual void setPosition(Tick t) = 0;
  virtual int selectedMixer() const = 0;   // index into Song::mixer
  virtual void selectMixer(int index) = 0;
  virtual bool armed(int index) const = 0;
  virtual void setArmed(int index, bool on) = 0;
  virtual int selectedChannel() const = 0;  // rack index, -1 = none
  virtual void selectChannel(int index) = 0;
  // Focused plugin window: owner (channel or mixer track) and slot (-1 = the
  // channel's instrument); owner kNoId = none.
  virtual std::pair<ObjectId, int> focusedPlugin() const = 0;
  virtual void midiOut(uint8_t status, uint8_t d1, uint8_t d2) = 0;
  virtual void sysexOut(const std::vector<uint8_t>& bytes) = 0;
  virtual void hint(const std::string& text) = 0;
  virtual std::string deviceName() const { return ""; }
};

// A host that keeps its own transport / selection state and records output:
// tests, and the script console when no device is involved.
class SimpleHost : public ControllerHost {
 public:
  using ControllerHost::ControllerHost;
  void action(RemoteAction a) override;
  bool playing() const override { return isPlaying; }
  bool recording() const override { return isRecording; }
  bool songMode() const override { return isSongMode; }
  Tick position() const override { return pos; }
  void setPosition(Tick t) override { pos = t; }
  int selectedMixer() const override { return mixerSel; }
  void selectMixer(int index) override { mixerSel = index; }
  bool armed(int index) const override;
  void setArmed(int index, bool on) override;
  int selectedChannel() const override { return channelSel; }
  void selectChannel(int index) override { channelSel = index; }
  std::pair<ObjectId, int> focusedPlugin() const override { return focus; }
  void midiOut(uint8_t s, uint8_t d1, uint8_t d2) override { out.push_back({s, d1, d2}); }
  void sysexOut(const std::vector<uint8_t>& bytes) override { sysex.push_back(bytes); }
  void hint(const std::string& text) override { hints.push_back(text); }
  std::string deviceName() const override { return device; }

  bool isPlaying = false, isRecording = false, isSongMode = false;
  Tick pos = 0;
  int mixerSel = 0, channelSel = -1;
  std::vector<int> armedTracks;
  std::pair<ObjectId, int> focus{kNoId, -1};
  std::vector<std::vector<uint8_t>> out, sysex;
  std::vector<std::string> hints;
  std::vector<RemoteAction> actions;
  std::string device = "Test device";
};

// Registers the modules above in `vm` (vm.user = &host).
void openControllerApi(Vm& vm, ControllerHost& host);

struct RefreshFlags {
  bool mixer = false, channels = false, transport = false, selection = false, plugin = false;
  bool any() const { return mixer || channels || transport || selection || plugin; }
  static RefreshFlags all() { return {true, true, true, true, true}; }
};

class ControllerScript {
 public:
  ControllerScript() = default;
  ~ControllerScript();

  bool loadFile(const std::string& path, ControllerHost& host, std::string* err = nullptr);
  bool loadSource(const std::string& source, const std::string& name, ControllerHost& host, std::string* err = nullptr);

  // Each returns false when the script failed (it is then disabled).
  bool onMidiMsg(const MidiMessage& m, bool* handled);
  bool onIdle();
  bool onRefresh(const RefreshFlags& f);
  void unload();  // calls onDeinit

  bool active() const { return vm_ && error_.empty(); }
  const std::string& error() const { return error_; }
  const std::string& name() const { return name_; }
  const std::string& path() const { return path_; }
  const Header& header() const { return header_; }
  // Device name substrings from the "match" header ("a; b").
  std::vector<std::string> match() const;
  Vm* vm() { return vm_.get(); }

  Limits limits;  // per hook call

 private:
  bool callHook(const char* fn, int nargs, int nres);
  std::unique_ptr<Vm> vm_;
  ControllerHost* host_ = nullptr;
  std::string name_, path_, error_;
  Header header_;
};

// Script console (SC-06): runs a snippet in `vm` (controller API available
// when opened), returns the printed lines and the error, if any.
std::vector<std::string> runConsole(Vm& vm, const std::string& code, std::string* err);

}  // namespace gl::script
