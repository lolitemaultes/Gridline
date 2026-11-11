// Gridline — opening the system MIDI service without trusting it.
// The system MIDI service can hang (a stuck CoreMIDI server never answers) or refuse (an
// error from the driver). MidiOpener runs the open-and-list step (`Probe`) on a background
// thread with a timeout, so startup and the interface never wait on it, and tries again on
// its own with a growing delay, or at once when asked (the Retry button).
// A probe that hangs is never abandoned for a second one running beside it: while it is
// still out, a retry keeps waiting on it, and its late answer is taken when it comes.
// Time is passed in (seconds, any steady origin) so the state machine is testable.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gl {

struct MidiProbeResult {
  bool ok = false;
  std::string error;  // why it failed (ok == false)
  std::vector<std::string> inputs, outputs;
};

enum class MidiOpenState {
  Idle,           // never started
  Opening,        // a probe is out, within its timeout
  Ready,          // the service answered: ports listed
  Failed,         // the service refused (an error); a retry is scheduled
  NotResponding,  // the probe passed its timeout without an answer; a retry is scheduled
};

const char* midiOpenStateName(MidiOpenState s);  // "idle", "opening", "ready", "failed", "not-responding"

class MidiOpener {
 public:
  using Probe = std::function<MidiProbeResult()>;
  // `retryDelays`: seconds before each automatic retry after a failure (the last repeats).
  explicit MidiOpener(Probe probe, double timeoutSec = 3.0, std::vector<double> retryDelays = {5.0, 15.0, 30.0, 60.0});
  ~MidiOpener();
  MidiOpener(const MidiOpener&) = delete;
  MidiOpener& operator=(const MidiOpener&) = delete;

  // Starts a probe now (a rescan): no-op while one is out.
  void start(double now);
  // The Retry button: a probe now, or keep waiting on the one still out. Resets the backoff.
  void retryNow(double now);
  // Call regularly (every frame). True when a probe's answer was taken in this call: the
  // state is then Ready or Failed and takeResult() holds it.
  bool update(double now);
  // Blocks up to `seconds` (wall clock) for the probe that is out to answer (startup gives
  // a working service a moment); returns whether it answered. update() still takes it.
  bool waitFor(double seconds);

  MidiOpenState state() const { return state_; }
  const std::string& error() const { return error_; }
  int attempts() const { return attempts_; }        // probes started
  bool probeRunning() const;                        // a probe (maybe hung) is still out
  double nextRetryIn(double now) const;             // seconds; < 0 when none is scheduled
  MidiProbeResult takeResult();

 private:
  struct Shared;
  void launch(double now);
  void schedule(double now);
  Probe probe_;
  double timeout_;
  std::vector<double> delays_;
  std::shared_ptr<Shared> cur_;  // the probe that is out (or answered and not taken)
  MidiOpenState state_ = MidiOpenState::Idle;
  std::string error_;
  MidiProbeResult result_;
  int attempts_ = 0, failures_ = 0;
  double startedAt_ = 0, retryAt_ = -1;
};

}  // namespace gl
