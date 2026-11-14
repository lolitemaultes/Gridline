// Gridline: the OSC address space (docs/osc.md). Applies received messages to the project on the
// UI thread, the same way MIDI controller links do: parameter changes go through the Document
// (undo merges a gesture), transport and notes through the host (the app, or a test stand-in),
// which hands them to the engine on its existing lock-free paths. Also learns addresses for
// "Link to controller", runs OSC controller links, and builds the feedback messages that keep
// touch surfaces in sync.
//
// Numbers follow Gridline's interface: mixer 0 is the master and 1.. the inserts; channels,
// patterns, markers, effect slots and plugin parameters count from 1. Continuous values are the
// control's position, 0..1. Address patterns (* ? [] {}) may stand for numbers and words.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "Expr.h"
#include "Osc.h"
#include "Params.h"
#include "Remote.h"

namespace gl {

class Document;

// What the address space drives besides the document. The app implements it over its engine and
// state; tests implement it with plain fields.
class OscHost {
 public:
  virtual ~OscHost() = default;
  virtual Document& doc() = 0;
  virtual bool playing() const = 0;
  virtual void play() = 0;   // starts playback (nothing when already playing)
  virtual void pause() = 0;  // stops where it is
  virtual void stop() = 0;   // stops and returns (the Stop button)
  virtual bool recording() const = 0;  // recording armed
  virtual void setRecording(bool on) = 0;
  virtual bool songMode() const = 0;
  virtual void setSongMode(bool song) = 0;
  virtual double position() const = 0;  // ticks
  virtual void setPosition(double ticks) = 0;
  virtual void setLoop(Tick start, Tick end) = 0;  // end <= start: no loop range
  virtual ObjectId currentPattern() const = 0;
  virtual void selectPattern(ObjectId pattern) = 0;
  virtual ObjectId selectedChannel() const = 0;
  virtual void note(ObjectId channel, int pitch, int velocity) = 0;  // velocity 0 = note off
  virtual void action(RemoteAction a) = 0;                           // an action link fired
  // Learn: the app takes the address for the link being learned. True when it was taken.
  virtual bool learn(const std::string& address) {
    (void)address;
    return false;
  }
  virtual void edited() {}  // a value changed (the app closes the undo gesture after a pause)
};

class OscControl {
 public:
  enum class Result { Learned, Linked, Handled, Reserved, Unknown, BadArgs };
  static const char* resultName(Result r);
  static constexpr size_t kMaxWatched = 1024;    // fixed addresses kept in sync by feedback
  static constexpr size_t kMaxFeedbackPerCall = 64;

  explicit OscControl(OscHost& host) : h_(host) {}

  Result handle(const osc::Message& m);

  // Feedback due now (`now` in seconds, any steady clock): the transport state, the position
  // while it moves (positionRate per second), the values of OSC-linked parameters and of fixed
  // addresses received before, when they change (rate per second). Values just set by OSC are
  // not echoed back. Appends at most kMaxFeedbackPerCall messages.
  void feedback(double now, std::vector<osc::Message>& out, double rate = 30.0, double positionRate = 10.0);
  // Everything goes out again on the next feedback (a new target).
  void resetFeedback();
  // Releases notes started by OSC that are still held (the server stops).
  void releaseNotes();
  size_t heldNotes() const { return held_.size(); }
  size_t watched() const { return watch_.size(); }
  RemoteMapper& mapper() { return mapper_; }

  // The control position (0..1) a link's target value corresponds to (inverse of its range and
  // curve); feedback sends this for linked parameters.
  static double linkPosition(const ControllerLink& l, double targetNorm, const Expr* formula = nullptr);

 private:
  struct Watch {
    enum Kind { Param, Solo } kind = Param;
    ParamId pid;
    ObjectId id = kNoId;
    double last = -1;
  };
  Result transport(const std::vector<std::string>& seg, const osc::Message& m);
  Result mixer(const std::vector<std::string>& seg, const osc::Message& m);
  Result channel(const std::vector<std::string>& seg, const osc::Message& m);
  Result param(const std::vector<std::string>& seg, const osc::Message& m);
  Result pattern(const std::vector<std::string>& seg, const osc::Message& m);
  Result marker(const std::vector<std::string>& seg, const osc::Message& m);
  bool links(const osc::Message& m);
  // Sets a parameter from a 0..1 position (or toggles a toggle on an impulse); remembers the
  // address for feedback.
  bool setNorm(ParamId pid, const osc::Message& m, const std::string& address);
  void watchParam(const std::string& address, ParamId pid, double norm);
  double paramNorm(ParamId pid) const;
  const Expr* formula(const std::string& text);

  OscHost& h_;
  RemoteMapper mapper_;
  std::map<std::string, Watch> watch_;
  std::map<uint64_t, double> linkSent_;  // RemoteMapper::key -> last position sent / set
  std::map<std::string, Expr> exprs_;
  std::set<std::pair<ObjectId, int>> held_;
  // Feedback state.
  double nextValues_ = 0, nextPos_ = 0;
  int sentPlaying_ = -1, sentRecording_ = -1, sentPattern_ = -1;
  double sentTempo_ = -1, sentBars_ = -1e9;
  bool pong_ = false;
};

}  // namespace gl
