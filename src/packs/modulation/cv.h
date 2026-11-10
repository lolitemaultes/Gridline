// Gridline — "modulation" pack: control voltage in and out (K-170).
//  - "gl.cvin" Voltage In: reads control voltages from the audio device's inputs (a DC-coupled
//    interface) and turns them into eight controller outputs and notes (pitch + gate).
//  - the shared voltage maths of Voltage In and Voltage Out (gl.cvctl): scaling, gate detection
//    with hysteresis, pitch quantising, clock tempo, and the calibration fits and store.
// Calibration belongs to this computer (per audio device and channel): it lives in its own file in
// the config folder, never in a project, and collaboration never sends it.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gl::packs::modulation {

// ----------------------------------------------------------------- Voltage In parameters
inline constexpr int kCvInputs = 6;     // pitch, gate, CV A..D
inline constexpr int kCvDeviceIns = 16;  // device inputs a source can name (In 1..In 16)
enum CvInSlot { CisPitch, CisGate, CisA, CisB, CisC, CisD };
// Per input: source (0 = off, k = device input k), range (CvRange choices), trim offset (mV), trim scale (%).
enum CvInField { CfSource, CfRange, CfOffset, CfScale, CfFields };
inline constexpr int cvInParam(int slot, int field) { return slot * CfFields + field; }
inline constexpr int CiBase = kCvInputs * CfFields;
enum CvIn {
  CiEnable = CiBase,
  CiFullScale,   // volts at digital full scale (1.0)
  CiThreshold,   // gate threshold, volts
  CiHysteresis,  // gate hysteresis, volts (the gate opens at threshold + h/2, closes below threshold - h/2)
  CiGateMode,    // 0 gate (note as long as the gate), 1 trigger (fixed-length notes)
  CiTrigLen,     // trigger mode note length, seconds
  CiPitchScale,  // 0 = 1 V/oct, 1 = 1.2 V/oct
  CiZeroNote,    // MIDI note at 0 V
  CiQuantise,    // CvQuantise
  CiScale,       // index into scales()
  CiRoot,        // 0..11
  CiGlide,       // pitch slew, seconds (also the glide time of legato notes)
  CiRetrigger,   // a pitch change while the gate is held starts a new note (else the note glides)
  CiVelSource,   // 0 fixed, 1..4 = CV A..D
  CiVelocity,    // fixed velocity 1..127
  CiSmooth,      // CV A..D output smoothing, seconds
  CiClockIn,     // 0 off, k = device input k
  CiClockPpq,    // index into kCvPpq
  CiRunIn,       // 0 off, k = device input k (high = run)
  CiFollow,      // CvFollow
  CiUseCal,      // apply this computer's calibration of the inputs
  CiCount
};
enum CvQuantise { CqOff, CqSemitone, CqScale };
enum CvFollow { CfwOff, CfwTempo, CfwTempoRun };
// Controller outputs (0..1).
enum CvInOutput { CoPitch, CoGate, CoA, CoB, CoC, CoD, CoClock, CoVelocity, CoCount };
inline constexpr int kCvPpqCount = 10;
inline constexpr int kCvPpq[kCvPpqCount] = {2, 4, 8, 12, 16, 24, 32, 48, 64, 96};

// ----------------------------------------------------------------- voltage maths
// Lower and upper volts of a range choice (0 = +/-5 V, 1 = +/-10 V, 2 = 0 to 10 V).
inline double rangeMin(int range) { return range == 2 ? 0.0 : (range == 1 ? -10.0 : -5.0); }
inline double rangeMax(int range) { return range == 0 ? 5.0 : 10.0; }

// What a device channel's volts really are: measured = gain * true + offset.
struct CvCal {
  double gain = 1.0, offset = 0.0;
  bool valid = false;
  bool identity() const { return !valid || (gain == 1.0 && offset == 0.0); }
};

// Volts on an input from its digital sample: full scale, calibration, then the user's trims.
inline double inputVolts(double digital, double fullScale, const CvCal& cal, double trimMv, double trimPct) {
  double v = digital * fullScale;
  if (cal.valid && std::fabs(cal.gain) > 1e-6) v = (v - cal.offset) / cal.gain;
  return v * (1.0 + trimPct / 100.0) + trimMv / 1000.0;
}
// The digital sample that makes an output carry `volts`: the user's trims, calibration, full scale.
// With no trims, no calibration and a 10 V full scale this is volts / 10 exactly.
inline double outputDigital(double volts, double fullScale, const CvCal& cal, double trimMv, double trimPct) {
  double v = volts;
  if (trimPct != 0.0) v *= 1.0 + trimPct / 100.0;
  if (trimMv != 0.0) v += trimMv / 1000.0;
  if (cal.valid && std::fabs(cal.gain) > 1e-6) v = (v - cal.offset) / cal.gain;
  return v / (fullScale > 1e-3 ? fullScale : 1e-3);
}

// Pitch (fractional MIDI) of a pitch voltage, and back.
inline double voltsToPitch(double volts, double voltsPerOct, double zeroNote) { return zeroNote + 12.0 * volts / voltsPerOct; }
inline double pitchToVolts(double pitch, double voltsPerOct, double zeroNote) { return (pitch - zeroNote) / 12.0 * voltsPerOct; }

// Quantises a pitch: off, to the nearest semitone, or to the nearest key of a scale (bit i of `mask`
// = pitch class root + i; ties go up). An empty mask quantises to semitones.
double quantisePitch(double pitch, int mode, unsigned mask, int root);

// Gate detector with hysteresis: opens at or above threshold + hyst / 2, closes below threshold - hyst / 2.
struct GateDetector {
  bool high = false;
  // True when the state changed on this sample.
  bool step(double volts, double threshold, double hyst) {
    const double h = hyst > 0 ? hyst * 0.5 : 0.0;
    if (!high && volts >= threshold + h) return high = true;
    if (high && volts < threshold - h) {
      high = false;
      return true;
    }
    return false;
  }
};

// Tempo from clock edges (absolute sample positions). Averages the intervals of the last beat;
// a jump of more than a third resets the average (tempo change, restart). Fixed size, no allocation.
class ClockTracker {
 public:
  void reset() { count_ = 0, last_ = -1; }
  void edge(int64_t at);
  // Beats per minute at `ppq` pulses per quarter note; 0 until three edges arrived.
  double bpm(double sampleRate, int ppq) const;
  int64_t lastEdge() const { return last_; }
  int intervals() const { return count_; }

 private:
  static constexpr int kMax = 96;
  double iv_[kMax] = {};
  int count_ = 0, head_ = 0;
  int64_t last_ = -1;
};

// ----------------------------------------------------------------- calibration fits
// Least-squares line y = gain * x + offset. rmsError in y units. ok needs two distinct x.
struct LineFit {
  double gain = 1.0, offset = 0.0, rmsError = 0.0;
  bool ok = false;
};
LineFit fitLine(const std::vector<double>& x, const std::vector<double>& y);

// Coupling check of a held step: `v` sampled at times `t` (seconds from the step start).
// A path that blocks steady voltages (AC-coupled) decays towards 0 V.
struct CouplingResult {
  bool ok = false;          // enough samples to tell
  bool dcCoupled = false;   // the step held (at least 80 % after `t` end)
  double startVolts = 0;    // mean over 30..130 ms
  double endVolts = 0;      // mean over the last 25 %
  double held = 0;          // endVolts / startVolts
  double noise = 0;         // standard deviation of the last 25 %, volts
};
CouplingResult analyseCoupling(const std::vector<double>& t, const std::vector<double>& v);

// V/oct fit from a tuner: each voltage step sent and the frequency detected for it.
struct TunerFit {
  bool ok = false;
  double octavesPerVolt = 1.0;  // measured
  double gain = 1.0;            // calibration gain of the output (measured volts per sent volt)
  std::vector<double> centsBefore, centsAfter;  // error of each step, relative to the first step
};
TunerFit fitTuner(const std::vector<double>& volts, const std::vector<double>& hz, double voltsPerOct);

// ----------------------------------------------------------------- calibration store
// Key of a device channel: "in|<device>|<channel>" or "out|<device>|<channel>".
std::string calKey(bool input, const std::string& device, int channel);
class CalStore {
 public:
  bool load(const std::string& path);  // a missing file is an empty store (true)
  bool save(const std::string& path) const;
  CvCal get(const std::string& key) const;
  void set(const std::string& key, const CvCal& c);
  void erase(const std::string& key);
  const std::map<std::string, CvCal>& all() const { return map_; }
  std::string toText() const;
  bool fromText(const std::string& text);

 private:
  std::map<std::string, CvCal> map_;
};
// The computer's store (UI thread and program builds; guarded). The app names the devices open
// now; prepareAux reads the calibration of those devices into the plugins' prepared data.
CalStore calStoreCopy();
void setCalStore(const CalStore& s);
void setCalDevices(const std::string& inputDevice, const std::string& outputDevice);
std::string calInputDevice();
std::string calOutputDevice();
// Calibration of device input `ch` (0-based) / output `ch` of the devices open now.
CvCal inputCal(int ch);
CvCal outputCal(int ch);

// ----------------------------------------------------------------- runtime access (UI thread)
// Voltage In's live values, read by its editor, the calibration window and clock follow.
class CvInMonitor {
 public:
  virtual ~CvInMonitor() = default;
  // Volts of input slot k (CvInSlot) over the last block, after calibration and trims.
  virtual double slotVolts(int slot) const = 0;
  // Raw volts of a device input (0-based) over the last block: full scale only.
  virtual double deviceVolts(int input) const = 0;
  virtual bool gate() const = 0;
  virtual double note() const = 0;   // the note playing (fractional), -1 = none
  virtual double clockBpm() const = 0;  // 0 = no clock
  virtual int run() const = 0;          // 1 run, 0 stopped, -1 no run signal or clock
  virtual bool hasInput() const = 0;    // the device input reached it in the last block
};
// Voltage Out's test voltage (calibration): while set, side 0/1 carries `volts` instead of its source.
class CvOutTester {
 public:
  virtual ~CvOutTester() = default;
  virtual void setTest(int side, double volts) = 0;
  virtual void clearTest() = 0;
};

}  // namespace gl::packs::modulation
