// Gridline — "modulation" pack: Voltage In (gl.cvin, K-170). A silent controller channel that reads
// control voltages from the audio device's inputs (ProcessContext::deviceIn): eight controller
// outputs any parameter can link to, and notes from a pitch and a gate input, sent sample by sample
// through the channel's MIDI output port (Generator::takeMidiOut) with their fractional pitch.
// Audio thread: fixed-size state only, nothing allocated, no locks.
#include <algorithm>
#include <cmath>
#include "../../engine/Generators.h"
#include "cv.h"
#include "modulation.h"
#include "modulation_dsp.h"

namespace gl::packs::modulation {

namespace {

inline float prm(const PluginState& s, int k, float dflt = 0) {
  return static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : dflt;
}
inline int iprm(const PluginState& s, int k, int dflt = 0) { return static_cast<int>(std::lround(prm(s, k, static_cast<float>(dflt)))); }

void storeD(std::atomic<double>& a, double v) { a.store(v, std::memory_order_relaxed); }

class VoltageIn final : public Generator, public CvInMonitor {
 public:
  bool active() const override { return enabled_; }
  // One render per block: event offsets and device input indices are block positions.
  bool wholeBlock() const override { return true; }
  int controlOutputs(float* out, int max) const override {
    const int k = std::min(static_cast<int>(CoCount), max);
    for (int i = 0; i < k; ++i) out[i] = outs_[i];
    return k;
  }
  int takeMidiOut(MidiOutEvent* out, int max) override {
    const int k = std::min(midiCount_, max);
    for (int i = 0; i < k; ++i) out[i] = midi_[i];
    midiCount_ = 0;
    return k;
  }

  // CvInMonitor (UI thread)
  double slotVolts(int slot) const override { return slot >= 0 && slot < kCvInputs ? slotUi_[slot].load(std::memory_order_relaxed) : 0.0; }
  double deviceVolts(int input) const override {
    return input >= 0 && input < kCvDeviceIns ? devUi_[input].load(std::memory_order_relaxed) : 0.0;
  }
  bool gate() const override { return gateUi_.load(std::memory_order_relaxed); }
  double note() const override { return noteUi_.load(std::memory_order_relaxed); }
  double clockBpm() const override { return bpmUi_.load(std::memory_order_relaxed); }
  int run() const override { return runUi_.load(std::memory_order_relaxed); }
  bool hasInput() const override { return inputUi_.load(std::memory_order_relaxed); }

 protected:
  // Voltage In plays no notes of its own: notes it is given are ignored.
  void onNoteOn(uint32_t, double, float) override {}
  void onNoteOff(uint32_t) override {}
  void onAllOff() override {}
  void onKill() override {
    killOff_ = killOff_ || noteOn_;  // its note off goes out with the next block
    noteOn_ = false;
    waitLow_ = true;  // a gate still held after a stop starts its note again only once it reopens
    trigLeft_ = 0;
  }
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    st_ = &s;
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    in_ = ctx.deviceIn;
    inCount_ = ctx.deviceIn ? std::clamp(ctx.deviceInCount, 0, kCvDeviceIns) : 0;
    cal_ = dynamic_cast<const CvAux*>(aux.custom.get());
    const bool en = prm(s, CiEnable, 1) >= 0.5f;
    if (!en && enabled_ && noteOn_) {
      killOff_ = true;
      noteOn_ = false;
    }
    enabled_ = en;
    glideSec_ = std::max(0.0f, prm(s, CiGlide));
    glideCoef_ = glideSec_ > 0 ? 1.0 - std::exp(-1.0 / (glideSec_ * sr_ / 3.0)) : 1.0;  // about 95 % after the glide time
    const double sm = std::max(0.0f, prm(s, CiSmooth, 0.001f));
    smoothCoef_ = sm > 0 ? 1.0 - std::exp(-1.0 / (sm * sr_)) : 1.0;
  }
  void render(float*, float*, int n) override {
    // Events nobody took (no channel listens on the port) are dropped: the engine takes each
    // block's events before the next process() call.
    midiCount_ = 0;
    if (killOff_) {
      pushOff(0);
      noteOn_ = false;
      killOff_ = false;
    }
    if (!enabled_) {
      absPos_ += n;
      publishIdle();
      return;
    }
    const PluginState& s = *st_;
    const double fullScale = std::max(0.1f, prm(s, CiFullScale, 10));
    const bool useCal = prm(s, CiUseCal, 1) >= 0.5f;
    const double thr = prm(s, CiThreshold, 1.5f), hyst = std::max(0.0f, prm(s, CiHysteresis, 0.25f));
    const double vpo = iprm(s, CiPitchScale) == 1 ? 1.2 : 1.0;
    const double zero = prm(s, CiZeroNote, 60);
    const int qMode = std::clamp(iprm(s, CiQuantise, CqSemitone), 0, 2);
    const auto& sc = scales();
    const unsigned mask = sc.empty() ? 0xFFFu : sc[static_cast<size_t>(std::clamp(iprm(s, CiScale, 1), 0, static_cast<int>(sc.size()) - 1))].mask;
    const int root = std::clamp(iprm(s, CiRoot), 0, 11);
    const bool trigMode = iprm(s, CiGateMode) == 1;
    const int64_t trigLen = std::max<int64_t>(1, std::llround(std::max(0.001f, prm(s, CiTrigLen, 0.05f)) * sr_));
    const bool retrig = prm(s, CiRetrigger) >= 0.5f;
    const int velSrc = std::clamp(iprm(s, CiVelSource), 0, 4);
    const float velFixed = std::clamp(prm(s, CiVelocity, 100), 1.0f, 127.0f);
    const int ppq = kCvPpq[std::clamp(iprm(s, CiClockPpq, 5), 0, kCvPpqCount - 1)];
    // Where each input reads from, its range and trims.
    const float* src[kCvInputs] = {};
    double lo[kCvInputs], hi[kCvInputs], trimMv[kCvInputs], trimPct[kCvInputs];
    CvCal cal[kCvInputs];
    bool any = false;
    for (int k = 0; k < kCvInputs; ++k) {
      const int d = iprm(s, cvInParam(k, CfSource)) - 1;
      if (d >= 0 && d < inCount_ && in_[d]) src[k] = in_[d], any = true;
      const int range = std::clamp(iprm(s, cvInParam(k, CfRange)), 0, 2);
      lo[k] = rangeMin(range), hi[k] = rangeMax(range);
      trimMv[k] = prm(s, cvInParam(k, CfOffset)), trimPct[k] = prm(s, cvInParam(k, CfScale));
      cal[k] = useCal && cal_ && d >= 0 && d < kCvDeviceIns ? cal_->in[d] : CvCal{};
    }
    auto devIn = [&](int param) -> const float* {
      const int d = iprm(s, param) - 1;
      return d >= 0 && d < inCount_ ? in_[d] : nullptr;
    };
    const float* clockIn = devIn(CiClockIn);
    const float* runIn = devIn(CiRunIn);
    const CvCal clockCal = useCal && cal_ && iprm(s, CiClockIn) > 0 ? cal_->in[std::clamp(iprm(s, CiClockIn) - 1, 0, kCvDeviceIns - 1)] : CvCal{};
    const CvCal runCal = useCal && cal_ && iprm(s, CiRunIn) > 0 ? cal_->in[std::clamp(iprm(s, CiRunIn) - 1, 0, kCvDeviceIns - 1)] : CvCal{};
    double sum[kCvInputs] = {};
    for (int i = 0; i < n; ++i) {
      double v[kCvInputs];
      for (int k = 0; k < kCvInputs; ++k) {
        double x = src[k] ? static_cast<double>(src[k][i]) : 0.0;
        if (!std::isfinite(x)) x = 0.0;
        v[k] = std::clamp(inputVolts(x, fullScale, cal[k], trimMv[k], trimPct[k]), lo[k], hi[k]);
        sum[k] += v[k];
      }
      // CV A..D: 0..1 across their ranges, smoothed.
      for (int k = 0; k < 4; ++k) {
        // An unassigned (or absent) input rests at 0.
        const double u = src[CisA + k] ? (v[CisA + k] - lo[CisA + k]) / std::max(1e-9, hi[CisA + k] - lo[CisA + k]) : 0.0;
        cvOut_[k] += (u - cvOut_[k]) * smoothCoef_;
      }
      // Pitch: volts to notes, quantised, slewed for the Pitch output.
      double q = src[CisPitch] ? quantisePitch(voltsToPitch(v[CisPitch], vpo, zero), qMode, mask, root) : zero;
      q = std::clamp(q, 0.0, 127.0);
      if (!pitchPrimed_) pitchOut_ = q, pitchPrimed_ = true;
      pitchOut_ += (q - pitchOut_) * glideCoef_;
      // Gate and notes.
      if (src[CisGate]) {
        const bool was = gateDet_.high;
        gateDet_.step(v[CisGate], thr, hyst);
        if (waitLow_ && !gateDet_.high) waitLow_ = false;
        const bool rise = !was && gateDet_.high && !waitLow_;
        const bool fall = was && !gateDet_.high;
        float vel = velFixed;
        if (velSrc > 0) vel = static_cast<float>(1.0 + std::clamp(cvOut_[velSrc - 1], 0.0, 1.0) * 126.0);
        if (rise) {
          if (noteOn_) pushOff(i);
          pushOn(i, q, vel);
          trigLeft_ = trigMode ? trigLen : 0;
          sinceMove_ = 0;
        } else if (fall && noteOn_ && !trigMode) {
          pushOff(i);
        } else if (noteOn_ && !trigMode && gateDet_.high) {
          // A pitch change while the gate is held: a new note (retrigger) or a glide (legato).
          const double moved = std::fabs(q - notePitch_);
          const bool step = qMode == CqOff ? moved > 0.02 && sinceMove_ >= 64 : moved > 0.5;
          if (step) {
            if (retrig) {
              pushOff(i);
              pushOn(i, q, vel);
            } else {
              pushGlide(i, q);
            }
            sinceMove_ = 0;
          }
        }
        ++sinceMove_;
        if (trigMode && noteOn_ && trigLeft_ > 0 && --trigLeft_ == 0) pushOff(i);
        if (rise) velOut_ = vel / 127.0f;
      } else if (noteOn_) {
        pushOff(i);
      }
      // Clock and run.
      if (clockIn) {
        double cx = static_cast<double>(clockIn[i]);
        if (!std::isfinite(cx)) cx = 0;
        const bool was = clockDet_.high;
        clockDet_.step(inputVolts(cx, fullScale, clockCal, 0, 0), thr, hyst);
        if (!was && clockDet_.high) {  // a clock pulse starts
          clock_.edge(absPos_ + i);
          clockPulse_ = static_cast<int>(0.005 * sr_) + 1;
        }
      }
      if (runIn) {
        double rx = static_cast<double>(runIn[i]);
        if (!std::isfinite(rx)) rx = 0;
        runDet_.step(inputVolts(rx, fullScale, runCal, 0, 0), thr, hyst);
      }
      if (clockPulse_ > 0) --clockPulse_;
    }
    absPos_ += n;
    // Outputs and the editor's live values.
    outs_[CoPitch] = static_cast<float>(pitchOut_ / 127.0);
    outs_[CoGate] = noteOn_ ? 1.0f : 0.0f;
    for (int k = 0; k < 4; ++k) outs_[CoA + k] = static_cast<float>(std::clamp(cvOut_[k], 0.0, 1.0));
    outs_[CoClock] = clockPulse_ > 0 ? 1.0f : 0.0f;
    outs_[CoVelocity] = velOut_;
    for (int k = 0; k < kCvInputs; ++k) storeD(slotUi_[k], n > 0 ? sum[k] / n : 0.0);
    for (int d = 0; d < kCvDeviceIns; ++d) {
      double acc = 0;
      if (d < inCount_ && in_[d] && n > 0) {
        for (int i = 0; i < n; ++i) acc += std::isfinite(in_[d][i]) ? static_cast<double>(in_[d][i]) : 0.0;
        acc = acc / n * fullScale;
      }
      storeD(devUi_[d], acc);
    }
    gateUi_.store(gateDet_.high, std::memory_order_relaxed);
    storeD(noteUi_, noteOn_ ? notePitch_ : -1.0);
    const double bpm = clockIn ? clock_.bpm(sr_, ppq) : 0.0;
    const bool clockLive = clockIn && clock_.lastEdge() >= 0 && absPos_ - clock_.lastEdge() < static_cast<int64_t>(0.5 * sr_);
    storeD(bpmUi_, clockLive ? bpm : 0.0);
    runUi_.store(runIn ? (runDet_.high ? 1 : 0) : (clockIn ? (clockLive ? 1 : 0) : -1), std::memory_order_relaxed);
    inputUi_.store(any || clockIn || runIn, std::memory_order_relaxed);
  }

 private:
  void publishIdle() {
    for (auto& o : outs_) o = 0;
    gateUi_.store(false, std::memory_order_relaxed);
    storeD(noteUi_, -1.0);
    inputUi_.store(false, std::memory_order_relaxed);
  }
  void push(const MidiOutEvent& e) {
    if (midiCount_ < kMidi) midi_[midiCount_++] = e;
  }
  void pushOn(int at, double pitch, float vel) {
    MidiOutEvent e{};
    e.offset = at;
    e.s = 0x90;
    e.a = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(pitch)), 0, 127));
    e.b = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(vel)), 1, 127));
    e.pitch = static_cast<float>(pitch);
    push(e);
    noteOn_ = true;
    noteKey_ = e.a;
    noteVel_ = e.b;
    notePitch_ = pitch;
  }
  void pushOff(int at) {
    MidiOutEvent e{};
    e.offset = at;
    e.s = 0x80;
    e.a = noteKey_;
    e.b = 0;
    push(e);
    noteOn_ = false;
  }
  void pushGlide(int at, double pitch) {
    MidiOutEvent e{};
    e.offset = at;
    e.s = MidiOutEvent::kGlide;
    e.a = noteKey_;
    e.b = noteVel_;
    e.pitch = static_cast<float>(pitch);
    e.time = static_cast<float>(glideSec_);
    push(e);
    notePitch_ = pitch;
  }

  static constexpr int kMidi = 256;
  MidiOutEvent midi_[kMidi] = {};
  int midiCount_ = 0;
  const PluginState* st_ = nullptr;
  const CvAux* cal_ = nullptr;
  const float* const* in_ = nullptr;
  int inCount_ = 0;
  double sr_ = 44100.0;
  bool enabled_ = true;
  double glideSec_ = 0, glideCoef_ = 1, smoothCoef_ = 1;
  GateDetector gateDet_, clockDet_, runDet_;
  ClockTracker clock_;
  int clockPulse_ = 0;
  bool noteOn_ = false, waitLow_ = false, pitchPrimed_ = false, killOff_ = false;
  uint8_t noteKey_ = 60, noteVel_ = 100;
  double notePitch_ = 60, pitchOut_ = 60;
  int64_t trigLeft_ = 0, sinceMove_ = 0, absPos_ = 0;
  double cvOut_[4] = {};
  float velOut_ = 0;
  float outs_[CoCount] = {};
  std::atomic<double> slotUi_[kCvInputs] = {};
  std::atomic<double> devUi_[kCvDeviceIns] = {};
  std::atomic<bool> gateUi_{false}, inputUi_{false};
  std::atomic<double> noteUi_{-1.0}, bpmUi_{0.0};
  std::atomic<int> runUi_{-1};
};

}  // namespace

std::unique_ptr<Generator> createVoltageIn() { return std::make_unique<VoltageIn>(); }

void prepareVoltageAux(const PluginState& state, ChannelAux& aux) {
  auto a = std::make_shared<CvAux>();
  if (state.type == "gl.cvin") {
    for (int d = 0; d < kCvDeviceIns; ++d) a->in[d] = inputCal(d);
  } else {  // gl.cvctl: the device outputs this instance drives (main pair 0, 1; direct pair k: 2 + 2k, 3 + 2k)
    const bool direct = static_cast<size_t>(CvOutMode) < state.params.size() && state.params[CvOutMode] >= 0.5f;
    const int pair = static_cast<size_t>(CvOutPair) < state.params.size() ? static_cast<int>(std::lround(state.params[CvOutPair])) : 0;
    for (int side = 0; side < 2; ++side) a->out[side] = outputCal(cvOutputChannel(direct, pair, side));
  }
  aux.custom = a;
}

}  // namespace gl::packs::modulation
