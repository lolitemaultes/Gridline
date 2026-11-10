// Gridline - "tracker" pack (K-76): the Tracker Sampler's runtime.
//
// Every note is one voice of one tracker channel. A note imported from a module carries the cells
// of its channel (core/TrackerStream.h); the voice runs them through the same channel logic the
// importer used (core/TrackerChannel.h), tick by tick in step with the song, and mixes its sample
// the way the tracker's mixer does: square-root pan law, a volume ramp over each tick (5 ms on
// triggers, cuts and volume sets), a short fade of the old sound when a sample starts again,
// forward and ping-pong loops, and interpolation (windowed sinc, cubic, linear or none). A note
// drawn or played without cells plays the instrument plainly: its envelopes, auto vibrato and
// fadeout, key off at the note's end. Those notes also follow the note's fine pitch, pan and the
// channel's pitch bend (K-120), and stereo sample files play in stereo.
//
// Audio thread: no allocation, no locks; every buffer is fixed in the voice.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "../Packs.h"
#include "../../core/TrackerStream.h"
#include "tracker.h"

namespace gl::packs::trackerpack {

namespace trk = gl::tracker;

namespace {

// ---- interpolation tables (8-tap Kaiser windowed sinc, four cut-offs chosen by the pitch step) ----

constexpr int kTaps = 8, kPhases = 256;
struct SincTables {
  // [kernel][phase (kPhases + 1)][tap]
  std::array<std::array<std::array<float, kTaps>, kPhases + 1>, 4> t{};
  SincTables() {
    const double beta[4] = {10.1326, 8.3378, 7.1979, 7.0};
    const double cutoff[4] = {1.0, 0.7639, 0.5212, 0.3333};
    auto bessel0 = [](double z) {
      double s = 1.0, ds = 1.0, d = 2.0;
      const double zz = z * z;
      do {
        ds *= zz / (d * d);
        s += ds;
        d += 2.0;
      } while (ds > s * 1e-11);
      return s;
    };
    const double pi = 3.14159265358979323846;
    for (int k = 0; k < 4; ++k) {
      const double norm = 1.0 / bessel0(beta[k]);
      for (int ph = 0; ph < kPhases; ++ph) {
        const double phase = static_cast<double>(ph) / kPhases;
        double w[kTaps], sum = 0;
        for (int j = 0; j < kTaps; ++j) {
          const double x = (j - (kTaps / 2 - 1)) - phase;
          const double n = x / (kTaps / 2);
          const double win = bessel0(beta[k] * std::sqrt(std::max(0.0, 1.0 - n * n))) * norm;
          const double s = x == 0.0 ? cutoff[k] : std::sin(cutoff[k] * pi * x) / (pi * x);
          w[j] = s * win;
          sum += w[j];
        }
        for (int j = 0; j < kTaps; ++j) t[static_cast<size_t>(k)][static_cast<size_t>(ph)][static_cast<size_t>(j)] = static_cast<float>(w[j] / sum);
      }
      for (int j = 0; j < kTaps; ++j)  // the phase after the last: the first, mirrored
        t[static_cast<size_t>(k)][kPhases][static_cast<size_t>(j)] = t[static_cast<size_t>(k)][0][static_cast<size_t>(kTaps - 1 - j)];
    }
  }
};
const SincTables& sinc() {
  static const SincTables s;
  return s;
}

// sqrt(i / 256) as the tracker rounds it (16.16).
const std::array<float, 257>& panTable() {
  static const auto t = [] {
    std::array<float, 257> v{};
    for (int i = 0; i <= 256; ++i) v[static_cast<size_t>(i)] = static_cast<float>(std::lround(65536.0 * std::sqrt(i / 256.0)) / 65536.0);
    return v;
  }();
  return t;
}

// ---- K-129: the editor's request to hear one sample ----
struct Audition {
  std::atomic<const InstrumentAux*> inst{nullptr};
  std::atomic<int> slot{0}, pitch{-1};
  std::atomic<uint32_t> serial{0};    // the last request
  std::atomic<uint32_t> pending{0};   // a request no note has taken yet (0 = none)
  std::atomic<uint32_t> sounding{0};  // the request whose voice plays (0 = none)
};
Audition& audition() {
  static Audition a;
  return a;
}

// ---- one playing sample (the tracker mixer's voice) ----

struct Mix {
  bool active = false;
  const float* base = nullptr;
  const float* baseR = nullptr;  // the right channel of a stereo file (null = mono)
  int32_t length = 0, loopStart = 0, loopLength = 0, sampleEnd = 0;
  uint8_t loop = trk::kLoopOff;
  int32_t pos = 0;
  uint32_t frac = 0;
  uint64_t delta = 0;
  bool back = false, looped = false;
  int kernel = 0;
  float vol = 0;
  uint8_t pan = 128;
  float curL = 0, curR = 0, tgtL = 0, tgtR = 0, dL = 0, dR = 0;
  int ramp = 0;
  bool fade = false;  // a fade-out tail: stops when its ramp ends

  // The sample value at index i, with the loop's continuation around it.
  float tap(int64_t i) const { return tapOf(base, i); }
  float tapOf(const float* b, int64_t i) const {
    if (loop == trk::kLoopForward) {
      const int64_t le = static_cast<int64_t>(loopStart) + loopLength;
      if (i >= le) i = loopStart + (i - le) % loopLength;
      else if (looped && i < loopStart) i = le - 1 - ((loopStart - 1 - i) % loopLength);
    } else if (loop == trk::kLoopPingPong) {
      const int64_t le = static_cast<int64_t>(loopStart) + loopLength;
      if (i >= le) i = std::max<int64_t>(loopStart, 2 * le - 1 - i);
      else if (looped && i < loopStart) i = std::min<int64_t>(le - 1, 2 * static_cast<int64_t>(loopStart) - 1 - i);
    }
    // Past either edge the tracker repeats the edge's sample point (a sample that ends without a
    // loop does not fall to silence between its last points).
    if (length < 1) return 0.0f;
    if (i < 0) return b[0];
    if (i >= length) return b[length - 1];
    return b[i];
  }
  float sampleAt(int interp) const { return sampleOf(base, interp); }
  float sampleOf(const float* src, int interp) const {
    int64_t i = pos;
    uint32_t f = frac;
    if (back) {  // ping-pong, backwards: the mirrored place in the loop
      i = static_cast<int64_t>(loopStart) + loopStart + loopLength - 1 - pos;
      f = ~frac;
    }
    const int64_t hiLimit = loop == trk::kLoopOff ? length : static_cast<int64_t>(loopStart) + loopLength;
    const int64_t loLimit = looped ? loopStart : 0;
    const bool inside = i - 3 >= loLimit && i + 4 < hiLimit;
    auto at = [&](int k) { return inside ? src[i + k] : tapOf(src, i + k); };
    switch (interp) {
      case trk::InterpNone: return at(0);
      case trk::InterpLinear: {
        const float x = static_cast<float>(f >> 1) * (1.0f / 2147483648.0f);
        const float p1 = at(0), p2 = at(1);
        return p1 + (p2 - p1) * x;
      }
      case trk::InterpCubic: {
        const float x = static_cast<float>(f >> 1) * (1.0f / 2147483648.0f);
        const float p1 = at(-1), p2 = at(0), p3 = at(1), p4 = at(2);
        const float a = ((p4 - p1) * 0.5f) + ((p2 - p3) * 1.5f);
        const float b = ((p1 - (p2 * 2.5f)) + (p3 * 2.0f)) - (p4 * 0.5f);
        const float c = (p3 - p1) * 0.5f;
        return ((a * x + b) * x + c) * x + p2;
      }
      default: {
        const auto& tb = sinc().t[static_cast<size_t>(kernel)];
        const uint32_t ph = f >> 24;
        const float pf = static_cast<float>(f & 0xFFFFFF) * (1.0f / 16777216.0f);
        const auto& w1 = tb[ph];
        const auto& w2 = tb[ph + 1];
        float s = 0;
        for (int k = 0; k < kTaps; ++k) s += at(k - 3) * (w1[static_cast<size_t>(k)] + (w2[static_cast<size_t>(k)] - w1[static_cast<size_t>(k)]) * pf);
        return s;
      }
    }
  }
  // One sample on: false when the sample ended.
  bool advance() {
    const uint64_t f = static_cast<uint64_t>(frac) + (delta & 0xFFFFFFFFull);
    frac = static_cast<uint32_t>(f);
    pos += static_cast<int32_t>(delta >> 32) + static_cast<int32_t>(f >> 32);
    if (pos < sampleEnd) return true;
    if (loop == trk::kLoopOff) {
      active = false;
      return false;
    }
    if (loopLength <= 0) {
      active = false;
      return false;
    }
    if (loop == trk::kLoopForward) {
      pos = loopStart + (pos - sampleEnd) % loopLength;
    } else {
      while (pos >= sampleEnd) {
        pos -= loopLength;
        back = !back;
      }
    }
    looped = true;
    return true;
  }
};

constexpr int kVoices = 48;

struct Voice {
  bool used = false;
  uint32_t id = 0;
  bool inNote = false;    // its note still holds
  bool handOver = false;  // the note hands the voice over to the next pattern's note
  bool waiting = false;   // the note ended, waiting for that note (handOver)
  int waitLeft = 0;       // samples
  bool ending = false;    // no more ticks: plays out its fade, then frees
  bool live = false;      // a note without cells
  bool released = false;  // (live) key off given
  float detune = 0;       // (live) semitones of fine pitch beyond the key
  double basePitch = 0;   // K-144: the pitch the detune is counted from (live: the key; else the note)
  double slideStep = 0, slideTo = 0;  // K-144: detune gliding (slide notes, bend lines)
  int64_t slideLeft = 0;
  int panOffset = 0;      // (live) the note's pan, in tracker pan steps
  uint8_t lane = 0;
  int transpose = 0;
  float gain = 1;
  uint64_t age = 0;
  trk::Channel ch;
  std::array<uint8_t, trk::kStreamMax> buf{};
  size_t len = 0;
  trk::StreamReader rd;
  trk::Cell first{};        // live: the cell of the first row
  double rel = 0;           // song ticks since the note (or its continuation) started
  double startSong = 0;     // song tick of that start (when the song plays)
  bool synced = false;
  // The song ticks per frame the voice runs at. The song gives its tempo once per block; when the
  // module's tempo changes on a row inside a block, a voice in step with it follows from that row.
  double tps = 0.01;
  int rowLen = 0;           // the current row's length (trk::rowLength), 0 = not known
  bool inStep = false;      // the song plays at the module's tempo (as imported)
  // The song fires a note on the first frame at or after its position; the tracker starts it on
  // the frame its time falls in, one earlier when the time is between frames. Then the voice
  // starts one frame in.
  bool skipFrame = false;
  int row = -1, tick = 0, ticksInRow = 6, speed = 6, endTick = -1;
  bool rowEnded = false;    // the row's end mark said: stop when the row ends
  trk::Cell cell{};
  Mix mix, tail;
  bool audition = false;     // K-129: the editor's preview of one sample (plays it once)
  uint32_t auditionId = 0;
  int midiKey = -1;          // K-129: the key sent to the MIDI output (-1 = none)
  uint8_t midiCh = 0;
};

class TrackerGen final : public Generator {
 public:
  TrackerGen() {
    (void)sinc();
    (void)panTable();
    (void)trk::emptyInstrument();
    (void)trk::periodHz(1000, true);
  }
  bool active() const override {
    for (const auto& v : voices_)
      if (v.used) return true;
    return false;
  }
  float noteBendRange() const override { return bendRange_; }  // K-129: the instrument's wheel range
  void setMidiOut(MidiOutRing* r) override { out_ = r; }

 protected:
  bool handlesNotePan() const override { return true; }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    auto pv = [&](int i, float d) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : d; };
    interp_ = std::clamp(static_cast<int>(std::lround(pv(trk::PInterp, trk::InterpSinc))), 0, 3);
    linear_ = pv(trk::PAmiga, 0) < 0.5f;
    rowsPerBeat_ = trk::rowsPerBeatOf(pv(trk::PRowsPerBeat, 2));
    liveTicks_ = std::clamp(static_cast<int>(std::lround(pv(trk::PTicks, 6))), 1, 31);
    level_ = std::max(0.0f, pv(trk::PLevel, 1));
    ramps_ = pv(trk::PRamp, 1) >= 0.5f;
    const auto* ia = dynamic_cast<const InstrumentAux*>(aux.custom.get());
    if (ia != inst_) {
      // Another instrument (an edit): voices of the old one cannot go on with its data.
      for (auto& v : voices_) {
        midiOff(v);
        endAudition(v);
        v = Voice{};
      }
      inst_ = ia;
    }
    // K-129: the MIDI settings. A program change goes out when they are switched on or change.
    const MidiSettings m = inst_ ? inst_->midi : MidiSettings{};
    bendRange_ = static_cast<float>(m.bend);
    if (m.on && out_ && (!midi_.on || m.channel != midi_.channel || m.program != midi_.program))
      out_->push(static_cast<uint8_t>(0xC0 | (m.channel & 15)), static_cast<uint8_t>(m.program & 127), 0);
    if (!m.on || m.channel != midi_.channel)
      for (auto& v : voices_) midiOff(v);
    midi_ = m;
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    ppq_ = ctx.ppq > 0 ? ctx.ppq : 96;
    const double tempo = ctx.tempo > 1 ? ctx.tempo : 120.0;
    tps_ = tempo * ppq_ / (60.0 * sr_);
    rowTicks_ = static_cast<double>(ppq_) / rowsPerBeat_;
    quick_ = std::max(1, static_cast<int>(std::lround(sr_ * 220.0 / 44000.0)));
    blockTick_ = ctx.tick;
    playing_ = ctx.playing;
    done_ = 0;
    // In step with the song: voices follow its position.
    if (ctx.playing && !ctx.jumped)
      for (auto& v : voices_)
        if (v.used && v.synced && !v.live) {
          const double expect = ctx.tick - v.startSong;
          if (std::abs(expect - v.rel) < rowTicks_ * 0.5) v.rel = expect;
        }
    for (auto& v : voices_) {
      v.tps = tps_;
      v.inStep = !v.live && v.rowLen > 0 && atModuleTempo(v.rowLen);
    }
  }

  void onNoteOn(uint32_t id, double pitch, float velocity) override { onNoteOnEx(id, pitch, velocity, NoteProps{}); }

  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& props) override {
    if (!inst_) return;
    const double now = blockTick_ + static_cast<double>(done_) * tps_;
    const float gain = std::clamp(velocity, 0.0f, 1.0f) * (127.0f / 100.0f);
    if (props.tracker && props.trackerLen > 0 && props.trackerLen <= trk::kStreamMax) {
      trk::StreamHeader h;
      trk::StreamReader probe;
      if (!probe.open(props.tracker, props.trackerLen, h)) return;
      if (h.flags & trk::kTie) {
        for (auto& v : voices_)
          if (v.used && !v.live && !v.ending && v.lane == h.lane && (v.waiting || (v.inNote && v.handOver))) {
            adopt(v, id, props, h, now);
            return;
          }
      }
      Voice& v = take();
      v.used = true;
      v.id = id;
      v.inNote = true;
      midiOn(v, noteKey(pitch, props), velocity);
      v.handOver = (h.flags & trk::kHandOver) != 0;
      v.basePitch = pitch, v.detune = 0, v.slideLeft = 0;
      v.lane = h.lane;
      v.gain = gain;
      v.age = ++clock_;
      std::memcpy(v.buf.data(), props.tracker, props.trackerLen);
      v.len = props.trackerLen;
      trk::StreamHeader hh;
      v.rd.open(v.buf.data(), v.len, hh);
      startChannel(v);
      if (h.flags & trk::kHasState) v.ch.s = h.state;
      v.ch.s.status = 0;
      // Transposed in the piano roll: every note of the voice moves with it.
      int base = 0, bpm0 = 0;
      {
        trk::StreamReader scan;
        trk::StreamHeader sh;
        scan.open(v.buf.data(), v.len, sh);
        for (int guard = 0; guard < 4096 && !scan.done(); ++guard) {
          const trk::StreamRow r = scan.next();
          if (guard == 0) bpm0 = r.bpm;
          if (r.cell.note >= 1 && r.cell.note <= 96) {
            base = r.cell.note;
            break;
          }
        }
      }
      v.transpose = base > 0 ? noteKey(pitch, props) - (base + trk::kPitchOffset) : 0;
      v.startSong = noteStart(now);
      v.rel = now - v.startSong;
      v.synced = playing_;
      // A time exactly on a frame: the tracker sums its ticks in 52-bit fixed point, so when that
      // falls short of the exact tick length it is just before the frame.
      const bool onTick = v.startSong > 0 && v.startSong == std::floor(v.startSong);
      v.skipFrame = !(h.flags & trk::kTie) && onTick && (v.rel > 1e-4 * tps_ || ticksFallShort(bpm0));
      if (h.flags & trk::kTie) resume(v);  // the voice before it is gone (playback started here): from its state
      return;
    }
    // A plain note, or the editor's preview of one sample (K-129).
    Audition& au = audition();
    const uint32_t want = au.pending.load(std::memory_order_acquire);
    bool preview = false;
    if (want != 0 && au.inst.load(std::memory_order_acquire) == inst_ && au.pitch.load(std::memory_order_acquire) == noteKey(pitch, props)) {
      // Sounding before it stops pending, so the editor never sees it as neither.
      au.sounding.store(want, std::memory_order_release);
      uint32_t expect = want;
      preview = au.pending.compare_exchange_strong(expect, 0, std::memory_order_acq_rel);
      if (!preview) {
        expect = want;
        au.sounding.compare_exchange_strong(expect, 0, std::memory_order_acq_rel);
      }
    }
    Voice& v = take();
    v.used = true;
    v.id = id;
    v.inNote = true;
    v.live = true;
    v.gain = gain;
    v.age = ++clock_;
    v.len = 0;
    if (preview) {
      // That sample on every key, once through: no loop, no envelopes, no vibrato.
      const int slot = std::clamp(au.slot.load(std::memory_order_acquire), 0, trk::kMaxSamples - 1);
      previewDef_ = inst_->text.def;
      previewDef_.keymap.fill(static_cast<uint8_t>(slot));
      previewDef_.vol = trk::Envelope{};
      previewDef_.pan = trk::Envelope{};
      previewDef_.vibDepth = 0;
      previewDef_.fadeout = 0;
      previewDef_.mute = false;
      for (auto& sd : previewDef_.samples) sd.loop = trk::kLoopOff;
      v.audition = true;
      v.auditionId = want;
    } else {
      midiOn(v, noteKey(pitch, props), velocity);
    }
    startChannel(v);
    v.first = trk::Cell{static_cast<uint8_t>(std::clamp(noteKey(pitch, props) - trk::kPitchOffset, 1, 96)), 1, 0, 0, 0};
    v.detune = static_cast<float>(pitch - noteKey(pitch, props));
    v.basePitch = noteKey(pitch, props), v.slideLeft = 0;
    v.panOffset = static_cast<int>(std::lround(std::clamp(props.pan, -1.0f, 1.0f) * 127.0f));
    v.rel = 0;
    v.startSong = now;
    v.synced = false;
  }

  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_) {
      if (!v.used || v.id != id || !v.inNote) continue;
      v.inNote = false;
      midiOff(v);
      if (v.live) {
        if (!v.released) {
          v.released = true;
          v.ch.keyOff();
        }
      } else if (v.handOver && !v.ending) {
        v.waiting = true;
        v.waitLeft = static_cast<int>(sr_ * 0.05);
      } else {
        end(v);
      }
    }
  }
  void onCut(uint32_t id) override {
    for (auto& v : voices_)
      if (v.used && v.id == id) {
        v.inNote = false;
        midiOff(v);
        end(v);
      }
  }
  void onAllOff() override {
    for (auto& v : voices_)
      if (v.used) {
        v.inNote = false;
        midiOff(v);
        if (v.live && !v.released) {
          v.released = true;
          v.ch.keyOff();
        } else if (!v.live) {
          end(v);
        }
      }
  }
  void onKill() override {
    for (auto& v : voices_) {
      midiOff(v);
      endAudition(v);
      v = Voice{};
    }
    bend_ = 0;
  }
  // Pitch bend moves the notes played without cells (imported notes bend with their own effects).
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) != 0xE0) return;
    const int raw = (static_cast<int>(d2 & 0x7F) << 7) | (d1 & 0x7F);
    bend_ = static_cast<float>(raw - 8192) / 8192.0f * noteBendRange();
    if (midi_.on && out_) out_->push(static_cast<uint8_t>(0xE0 | (midi_.channel & 15)), d1, d2);
    for (auto& v : voices_)
      if (v.used && v.live && v.mix.active) setPitch(v.mix, v.ch.s.finalPeriod, liveRatio(v));
  }

  // K-144: slide notes and bend lines glide every note through its fine pitch (on top of what an
  // imported note's own effects do with it).
  bool supportsGlide() const override { return true; }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& v : voices_) {
      if (!v.used || v.id != id || v.audition) continue;
      v.slideTo = pitch - v.basePitch;
      v.slideLeft = std::max<int64_t>(1, static_cast<int64_t>(seconds * sr_));
      v.slideStep = (v.slideTo - static_cast<double>(v.detune)) / static_cast<double>(v.slideLeft);
    }
  }

  void render(float* L, float* R, int n) override {
    bool sliding = false;
    for (const auto& v : voices_) sliding = sliding || (v.used && v.slideLeft > 0);
    // While a note glides, the pitch moves every 32 samples.
    for (int pos = 0; pos < n;) {
      const int m = sliding ? std::min(32, n - pos) : n - pos;
      for (auto& v : voices_) {
        if (!v.used || v.slideLeft <= 0) continue;
        const int64_t k = std::min<int64_t>(v.slideLeft, m);
        v.slideLeft -= k;
        v.detune = static_cast<float>(v.slideLeft == 0 ? v.slideTo : static_cast<double>(v.detune) + v.slideStep * static_cast<double>(k));
        if (v.mix.active) setPitch(v.mix, v.ch.s.finalPeriod, liveRatio(v));
      }
      for (auto& v : voices_)
        if (v.used) renderVoice(v, L + pos, R + pos, m);
      done_ += m;
      pos += m;
    }
  }

 private:
  Voice& take() {
    for (auto& v : voices_)
      if (!v.used) {
        v.tps = tps_;
        v.rowLen = 0;
        v.inStep = false;
        v.skipFrame = false;
        return v;
      }
    // All busy: the oldest ending voice, else the oldest.
    Voice* best = nullptr;
    for (auto& v : voices_)
      if (v.ending && (!best || v.age < best->age)) best = &v;
    if (!best)
      for (auto& v : voices_)
        if (!best || v.age < best->age) best = &v;
    midiOff(*best);
    endAudition(*best);
    *best = Voice{};
    best->tps = tps_;
    return *best;
  }

  void startChannel(Voice& v) {
    v.ch.reset();
    v.ch.fixed = v.audition ? &previewDef_ : &inst_->text.def;
    v.ch.ins = nullptr;
    v.ch.linear = linear_;
    v.ch.globalVol = 64;
    v.row = -1;
    v.tick = 0;
    v.ticksInRow = v.speed = v.live ? liveTicks_ : 6;
    v.endTick = -1;
    v.rowEnded = false;
    v.mix = Mix{};
    v.tail = Mix{};
  }

  void adopt(Voice& v, uint32_t id, const NoteProps& props, const trk::StreamHeader& h, double now) {
    v.id = id;
    v.inNote = true;
    v.waiting = false;
    v.handOver = (h.flags & trk::kHandOver) != 0;
    std::memcpy(v.buf.data(), props.tracker, props.trackerLen);
    v.len = props.trackerLen;
    trk::StreamHeader hh;
    v.rd.open(v.buf.data(), v.len, hh);
    v.startSong = noteStart(now);
    v.rel = now - v.startSong;
    v.synced = playing_;
    v.row = -1;
    v.tick = 0;
    v.endTick = -1;
    v.rowEnded = false;
  }

  // A continuation whose voice is not there: the sound as it was at this point (no new trigger).
  void resume(Voice& v) {
    const trk::SampleDef& sd = inst_->text.def.samples[v.ch.s.smpNum & 15];
    v.ch.ins = &inst_->text.def;
    if (!startSample(v.mix, v.ch.s.smpNum & 15, v.ch.s.smpStartPos) || !sd.present()) return;
    v.mix.vol = v.ch.s.finalVol;
    v.mix.pan = v.ch.s.finalPan;
    setPitch(v.mix, v.ch.s.finalPeriod);
    updateVolumes(v, trk::kQuickRamp);
  }

  bool startSample(Mix& m, int slot, int32_t position, const trk::InstrumentDef* def = nullptr) {
    const trk::SampleDef& sd = (def ? *def : inst_->text.def).samples[static_cast<size_t>(slot)];
    const float* d = inst_->data[static_cast<size_t>(slot)];
    m.active = false;
    if (!d || sd.length < 1) return false;
    m.base = d;
    m.baseR = inst_->dataR[static_cast<size_t>(slot)];
    m.length = sd.length;
    m.loopStart = sd.loopStart;
    m.loopLength = sd.loopLength;
    m.loop = sd.loopLength < 1 ? static_cast<uint8_t>(trk::kLoopOff) : sd.loop;
    m.sampleEnd = m.loop == trk::kLoopOff ? sd.length : sd.loopStart + sd.loopLength;
    m.pos = position;
    m.frac = 0;
    m.back = false;
    m.looped = false;
    m.fade = false;
    if (m.pos >= m.sampleEnd || m.pos < 0) return false;
    m.active = true;
    return true;
  }

  // The pitch ratio of a note played without cells: its fine pitch and the channel's bend.
  double liveRatio(const Voice& v) const {
    const double semis = static_cast<double>(v.detune) + (v.live ? static_cast<double>(bend_) : 0.0);
    return semis == 0.0 ? 1.0 : std::exp2(semis / 12.0);
  }
  void setPitch(Mix& m, uint16_t period, double tune = 1.0) {
    double hz = trk::periodHz(period, linear_);
    if (tune != 1.0) hz *= tune;
    const double d = hz / sr_ * 4294967296.0;
    m.delta = d > 0 ? static_cast<uint64_t>(d + 0.5) : 0;
    const double ratio = hz / sr_;
    m.kernel = ratio <= 1.1875 ? 0 : ratio <= 1.5 ? 1 : ratio <= 2.0 ? 2 : 3;
  }

  // The tracker's volume update for a tick's status.
  void updateVolumes(Voice& v, uint8_t st) {
    Mix& m = v.mix;
    const auto& pt = panTable();
    const float g = v.gain * level_;
    m.tgtL = m.vol * g * pt[static_cast<size_t>(256 - m.pan)];
    m.tgtR = m.vol * g * pt[m.pan];
    if (!ramps_) {
      m.curL = m.tgtL;
      m.curR = m.tgtR;
      m.ramp = 0;
      return;
    }
    const bool trig = (st & trk::kTrigger) != 0;
    if (trig) {
      if (m.curL > 0 || m.curR > 0) {
        v.tail = m;
        v.tail.fade = true;
        v.tail.ramp = quick_;
        v.tail.dL = -v.tail.curL / static_cast<float>(quick_);
        v.tail.dR = -v.tail.curR / static_cast<float>(quick_);
      }
      m.curL = m.curR = 0;
    }
    if (!trig && m.tgtL == m.curL && m.tgtR == m.curR) {
      m.ramp = 0;
      return;
    }
    const int len = (st & trk::kQuickRamp) ? quick_ : std::max(1, tickLen_);
    m.ramp = len;
    m.dL = (m.tgtL - m.curL) / static_cast<float>(len);
    m.dR = (m.tgtR - m.curR) / static_cast<float>(len);
  }

  void applyStatus(Voice& v, uint8_t st) {
    if (st == 0) return;
    Mix& m = v.mix;
    if (st & trk::kUpdVol) m.vol = v.ch.s.finalVol;
    if (st & trk::kUpdPan) m.pan = v.live ? static_cast<uint8_t>(std::clamp(v.ch.s.finalPan + v.panOffset, 0, 255)) : v.ch.s.finalPan;
    if (st & (trk::kUpdVol | trk::kUpdPan)) updateVolumes(v, st);
    if (st & trk::kUpdPeriod) setPitch(m, v.ch.s.finalPeriod, liveRatio(v));
    if (st & trk::kTrigger) {
      const float cl = m.curL, cr = m.curR;
      startSample(m, v.ch.s.smpNum & 15, v.ch.s.smpStartPos, v.audition ? &previewDef_ : nullptr);
      m.curL = cl;
      m.curR = cr;
    }
  }

  // Ends the voice the way a new note ends the old one: its sound fades in 5 ms.
  void end(Voice& v) {
    v.ending = true;
    v.waiting = false;
    if (v.mix.active && (v.mix.curL > 0 || v.mix.curR > 0 || v.mix.ramp > 0)) {
      v.tail = v.mix;
      v.tail.fade = true;
      v.tail.ramp = quick_;
      v.tail.dL = -v.tail.curL / static_cast<float>(quick_);
      v.tail.dR = -v.tail.curR / static_cast<float>(quick_);
    }
    v.mix.active = false;
  }

  // The tracker starts a tick on the frame its time falls in (it counts whole frames per tick and
  // carries the fraction), so a tick is due on the frame whose span reaches its time.
  static constexpr double kDueEps = 1e-6;
  bool due(const Voice& v) const { return v.rel + v.tps > boundary(v) + kDueEps * v.tps; }
  // Whether the tracker's fixed-point tick length at this tick rate is below the exact one.
  bool ticksFallShort(int bpm) const {
    if (bpm <= 0 || sr_ != std::floor(sr_) || sr_ < 1 || sr_ > 1e6) return false;
    const uint64_t k = static_cast<uint64_t>(110000 / std::clamp(bpm, 32, 255));
    const double spt = sr_ / (44000.0 / static_cast<double>(k));
    double whole = 0;
    const auto fixed = static_cast<uint64_t>(std::modf(spt, &whole) * 4503599627370496.0);  // 2^52
    // The exact fraction a / 44000 in the same scale: a * 2^52 / 44000, in two steps of 2^26.
    const uint64_t a = (static_cast<uint64_t>(sr_) * k) % 44000u;
    const uint64_t hi = a << 26, q1 = hi / 44000u, r1 = hi % 44000u;
    const uint64_t q = (q1 << 26) + (r1 << 26) / 44000u, rem = (r1 << 26) % 44000u;
    return fixed < q || (fixed == q && rem > 0);
  }
  // Whether the block's tempo is the module's for a row of this length.
  bool atModuleTempo(int len) const {
    const double want = rowTicks_ * 44000.0 / (static_cast<double>(len) * sr_);
    return std::abs(tps_ - want) <= 1e-4 * want;
  }
  // A new row of a song voice: when the module's tempo changes on it (inside this block), the
  // voice's tick rate changes with it.
  void followTempo(Voice& v, int len) {
    if (v.live || len <= 0) return;
    if (v.rowLen <= 0) {  // the voice's first row: the song row before it, as the note says
      v.rowLen = v.rd.priorLength() > 0 ? v.rd.priorLength() : len;
      v.inStep = atModuleTempo(v.rowLen);
    }
    if (v.inStep && len != v.rowLen) v.tps *= static_cast<double>(v.rowLen) / len;
    v.rowLen = len;
  }
  // The song position an imported note starts at: notes sit on whole ticks, and the song fires
  // a note on the first frame at or after its position, so that tick is within the frame before.
  double noteStart(double now) const {
    const double t = std::floor(now + 1e-9);
    return now - t <= tps_ + 1e-9 ? t : now;
  }

  double boundary(const Voice& v) const {
    if (v.row < 0) return 0;
    return v.row * rowTicks_ + v.tick * rowTicks_ / std::max(1, v.ticksInRow);
  }

  trk::Cell transposed(trk::Cell c, int t) const {
    if (t != 0 && c.note >= 1 && c.note <= 96) c.note = static_cast<uint8_t>(std::clamp(c.note + t, 1, 96));
    return c;
  }

  // One tracker tick of the voice. false: no tick now (waiting for its continuation).
  bool doTick(Voice& v) {
    if (v.row < 0 || v.tick >= v.ticksInRow) {
      if (v.rowEnded) {
        end(v);
        return false;
      }
      // A voice handed over to the next pattern's note plays the rows it has, then waits for it.
      const bool mayHold = !v.live && (v.waiting || v.handOver);
      if (mayHold && v.rd.done()) return false;
      trk::StreamRow r;
      if (v.live) {
        if (v.row < 0) r.cell = v.first;
        r.speed = static_cast<uint8_t>(liveTicks_);
        r.passes = 1;
      } else {
        r = v.rd.next();
        if (mayHold && v.rd.done()) return false;
      }
      ++v.row;
      v.tick = 0;
      v.speed = std::max<int>(1, r.speed);
      v.ticksInRow = v.speed * std::max<int>(1, r.passes);
      v.endTick = r.endTick;
      v.rowEnded = r.endTick >= v.ticksInRow;
      v.cell = transposed(r.cell, v.transpose);
      if (!v.live) followTempo(v, trk::rowLength(r.bpm, r.speed, r.passes));
    }
    if (v.endTick >= 0 && v.endTick < v.ticksInRow && v.tick == v.endTick) {
      end(v);
      return false;
    }
    // A new tick: the last one's ramps are done.
    if (ramps_) {
      v.mix.curL = v.mix.tgtL;
      v.mix.curR = v.mix.tgtR;
      v.mix.ramp = 0;
    }
    tickLen_ = std::max(1, static_cast<int>(rowTicks_ / v.ticksInRow / v.tps));
    trk::SongFx fx;
    const int j = v.tick % v.speed;
    if (v.tick == 0) v.ch.row(v.cell, v.speed, fx);
    else v.ch.tick(v.speed, j == 0 ? v.speed : v.speed - j, fx);
    v.ch.updateEnvelopes();
    applyStatus(v, v.ch.takeStatus());
    ++v.tick;
    return true;
  }

  void mixInto(Mix& m, float* L, float* R, int n) {
    for (int i = 0; i < n && m.active; ++i) {
      if (m.curL != 0 || m.curR != 0 || m.ramp > 0) {
        const float s = m.sampleAt(interp_);
        L[i] += s * m.curL;
        R[i] += (m.baseR ? m.sampleOf(m.baseR, interp_) : s) * m.curR;
      }
      if (m.ramp > 0) {
        m.curL += m.dL;
        m.curR += m.dR;
        if (--m.ramp == 0) {
          if (m.fade) {
            m.active = false;
            break;
          }
          m.curL = m.tgtL;
          m.curR = m.tgtR;
        }
      }
      m.advance();
    }
  }

  void renderVoice(Voice& v, float* L, float* R, int n) {
    int i = 0;
    while (i < n) {
      // Ticks due now.
      bool held = false;
      for (int guard = 0; guard < 64 && !v.ending && due(v); ++guard)
        if (!doTick(v)) {
          held = !v.ending;
          break;
        }
      int seg = n - i;
      if (!v.ending && !held) {
        // The first frame whose span reaches the boundary (see due()).
        const double d = (boundary(v) + kDueEps * v.tps - v.rel) / v.tps;
        seg = std::clamp(static_cast<int>(std::floor(d)), 1, n - i);
      }
      if (v.skipFrame) {
        v.skipFrame = false;
        float dl = 0, dr = 0;
        if (v.mix.active) mixInto(v.mix, &dl, &dr, 1);
        if (v.tail.active) mixInto(v.tail, &dl, &dr, 1);
      }
      if (v.mix.active) mixInto(v.mix, L + i, R + i, seg);
      if (v.tail.active) mixInto(v.tail, L + i, R + i, seg);
      v.rel += seg * v.tps;
      if (v.waiting) {
        v.waitLeft -= seg;
        if (v.waitLeft <= 0) end(v);
      }
      i += seg;
      // K-129: a preview ends with its sample.
      if (v.audition && !v.ending && v.row >= 0 && !v.mix.active) end(v);
      // A plain note after its release: done once silent.
      if (v.live && v.released && !v.ending) {
        const bool silent = !v.mix.active || (v.ch.s.finalVol <= 0.0f && v.mix.ramp == 0 && v.mix.curL == 0 && v.mix.curR == 0);
        if (silent) end(v);
      }
      if (v.ending && !v.mix.active && !v.tail.active) {
        midiOff(v);
        endAudition(v);
        v = Voice{};
        return;
      }
    }
  }

  // K-129: MIDI output of the notes (when the instrument's MIDI settings say so).
  void midiOn(Voice& v, int key, float velocity) {
    v.midiKey = -1;
    if (!midi_.on || !out_) return;
    v.midiKey = std::clamp(key, 0, 127);
    v.midiCh = static_cast<uint8_t>(midi_.channel & 15);
    const int vel = std::clamp(static_cast<int>(std::lround(velocity * 127.0f)), 1, 127);
    out_->push(static_cast<uint8_t>(0x90 | v.midiCh), static_cast<uint8_t>(v.midiKey), static_cast<uint8_t>(vel));
  }
  void midiOff(Voice& v) {
    if (v.midiKey < 0) return;
    if (out_) out_->push(static_cast<uint8_t>(0x80 | v.midiCh), static_cast<uint8_t>(v.midiKey), 0);
    v.midiKey = -1;
  }
  static void endAudition(Voice& v) {
    if (!v.audition) return;
    uint32_t id = v.auditionId;
    audition().sounding.compare_exchange_strong(id, 0, std::memory_order_acq_rel);
    v.audition = false;
  }

  std::array<Voice, kVoices> voices_{};
  trk::InstrumentDef previewDef_{};
  MidiOutRing* out_ = nullptr;
  MidiSettings midi_{};
  float bendRange_ = 2;
  const InstrumentAux* inst_ = nullptr;
  int interp_ = trk::InterpSinc, rowsPerBeat_ = 4, liveTicks_ = 6, quick_ = 221, tickLen_ = 882, ppq_ = 96;
  bool linear_ = true, ramps_ = true, playing_ = false;
  float level_ = 1, bend_ = 0;
  double sr_ = 44100, tps_ = 0.01, rowTicks_ = 24, blockTick_ = 0;
  int64_t done_ = 0;
  uint64_t clock_ = 0;
};

// ---- the instrument from its state (UI thread), shared while its data stays the same ----

std::mutex& cacheMu() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::weak_ptr<const InstrumentAux>>& cache() {
  static std::map<std::string, std::weak_ptr<const InstrumentAux>> c;
  return c;
}

}  // namespace

std::shared_ptr<const InstrumentAux> instrumentFor(const PluginState& state) {
  const int sound = state.params.size() > static_cast<size_t>(trk::PSound)
                        ? std::clamp(static_cast<int>(std::lround(state.params[static_cast<size_t>(trk::PSound)])), 0, kSoundCount)
                        : 0;
  const std::string key = state.data + '\x01' + static_cast<char>('0' + sound);
  {
    std::lock_guard<std::mutex> lk(cacheMu());
    auto it = cache().find(key);
    if (it != cache().end())
      if (auto p = it->second.lock()) return p;
  }
  auto a = std::make_shared<InstrumentAux>();
  // The instrument text; without an instrument part, a bare sample file (one sample on every key
  // at its own rate) or the built-in sound's instrument.
  const Edit e = readEdit(state.data, sound);
  trk::InstrumentText& t = a->text;
  t = e.text;
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const int f = t.fileOf[static_cast<size_t>(k)];
    if (f < 0 || f >= static_cast<int>(t.files.size()) || t.files[static_cast<size_t>(f)].empty()) continue;
    SamplePtr sp = sampleCache().get(t.files[static_cast<size_t>(f)]);
    if (!sp || sp->frames() == 0) continue;
    trk::SampleDef& sd = t.def.samples[static_cast<size_t>(k)];
    const auto frames = static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF));
    if (sd.length == 0) tuneForRate(sd, sp->sampleRate);  // pitch from the file's rate: C-4 plays it at that rate
    // The sample's start and end in its file (K-120); loop points are file frames.
    const int32_t c0 = std::clamp(e.cuts.start[static_cast<size_t>(k)], 0, frames - 1);
    const int32_t c1 = e.cuts.end[static_cast<size_t>(k)] > 0 ? std::clamp(e.cuts.end[static_cast<size_t>(k)], c0 + 1, frames) : frames;
    if (c0 > 0 || c1 < frames) {
      const int32_t len = c1 - c0;
      const int32_t ls = std::clamp(sd.loopStart - c0, 0, len);
      const int32_t le = std::clamp(sd.loopStart + sd.loopLength - c0, ls, len);
      sd.loopStart = ls;
      sd.loopLength = le - ls;
    }
    // The file may have been replaced by another of a different length: its loop stays inside.
    sd.length = c1 - c0;
    if (sd.loopStart + sd.loopLength > sd.length) {
      if (sd.loopStart >= sd.length) sd.loopStart = sd.loopLength = 0;
      else sd.loopLength = sd.length - sd.loopStart;
    }
    if (sd.loopLength < 1) sd.loop = trk::kLoopOff;
    a->files[static_cast<size_t>(k)] = sp;
    a->data[static_cast<size_t>(k)] = sp->left.data() + c0;
    if (!sp->mono() && sp->right.size() == sp->left.size()) a->dataR[static_cast<size_t>(k)] = sp->right.data() + c0;
  }
  // K-129: samples played at 8 bits: their values rounded to 256 steps, in copies of their own.
  a->midi = e.midi;
  a->eightBit = e.eightBit;
  for (int k = 0; k < trk::kMaxSamples; ++k) {
    const auto i = static_cast<size_t>(k);
    if (!e.eightBit[i] || !a->data[i]) continue;
    const auto n = static_cast<size_t>(std::max(0, t.def.samples[i].length));
    a->ownL[i].resize(n);
    for (size_t j = 0; j < n; ++j) a->ownL[i][j] = eightBitValue(a->data[i][j]);
    a->data[i] = a->ownL[i].data();
    if (a->dataR[i]) {
      a->ownR[i].resize(n);
      for (size_t j = 0; j < n; ++j) a->ownR[i][j] = eightBitValue(a->dataR[i][j]);
      a->dataR[i] = a->ownR[i].data();
    }
  }
  // No sample of its own: the built-in sound plays on every key.
  bool any = false;
  for (const float* d : a->data) any = any || d != nullptr;
  if (!any)
    if (const BuiltinSound* b = builtinSound(sound)) {
      t.def.samples[0] = b->sample;
      t.def.keymap.fill(0);
      a->data[0] = b->frames.data();
      a->builtin = sound;
    }
  std::lock_guard<std::mutex> lk(cacheMu());
  for (auto it = cache().begin(); it != cache().end();) it = it->second.expired() ? cache().erase(it) : std::next(it);
  cache()[key] = a;
  return a;
}

uint32_t requestAudition(const InstrumentAux* inst, int slot, int pitch) {
  Audition& au = audition();
  au.inst.store(inst, std::memory_order_release);
  au.slot.store(slot, std::memory_order_release);
  au.pitch.store(pitch, std::memory_order_release);
  uint32_t id = au.serial.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (id == 0) id = au.serial.fetch_add(1, std::memory_order_acq_rel) + 1;
  au.pending.store(inst ? id : 0, std::memory_order_release);
  return id;
}

bool auditionSounding(uint32_t serial) {
  if (serial == 0) return false;
  const Audition& au = audition();
  return au.sounding.load(std::memory_order_acquire) == serial || au.pending.load(std::memory_order_acquire) == serial;
}

bool auditionPlaying(uint32_t serial) { return serial != 0 && audition().sounding.load(std::memory_order_acquire) == serial; }

void cancelAudition(uint32_t serial) {
  uint32_t id = serial;
  if (id != 0) audition().pending.compare_exchange_strong(id, 0, std::memory_order_acq_rel);
}

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (type == trk::kInstrumentType) return std::make_unique<TrackerGen>();
  return nullptr;
}
std::unique_ptr<Effect> createEffect(const std::string&) { return nullptr; }

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != trk::kInstrumentType) return false;
  aux = ChannelAux{};
  auto a = instrumentFor(state);
  for (const auto& sp : a->files)
    if (sp) aux.samples.push_back(sp);
  aux.custom = a;
  return true;
}

}  // namespace gl::packs::trackerpack
