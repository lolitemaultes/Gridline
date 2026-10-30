// Gridline — audio clip playback (arrangement). Audio thread: no allocation.
//
// Plain clips read the source at a fixed rate through a band-limited
// windowed-sinc kernel; every output sample follows from the song position
// alone, so playback can start, seek or loop anywhere.
//
// J-87: pitched, stretched and tempo-synced clips play their offline-quality
// render (ProgAudioClip::hq, engine/TimeStretch) once it is ready, indexed by
// the sample count since the clip's first sample.
//
// J-153: while the render is on its way, and while the channel pitch is off
// the render's pitch (automation), the clip plays through the same stretch in
// realtime (ProgAudioClip::rt: one continuous engine stream; a jump restarts
// it, primed with the audio before, so its first frame is already settled).
// J-145: switching between the render and the realtime stretch crossfades
// over 10 ms.
#include <algorithm>
#include <cmath>
#include "Program.h"

namespace gl {

namespace {
constexpr double kDeclick = 0.002;     // minimum fade at clip edges, seconds
constexpr double kSwitchFade = 0.010;  // seconds: render <-> realtime crossfade
constexpr double kPrimeAhead = 0.15;   // J-170: seconds ahead a clip's Rubber Band stream begins priming

// The realtime stretch's reader: source frames in clip direction (reverse
// clips read from the end), silence outside the sound and past the window.
void readClip(void* ctx, float* const* dst, int frames) {
  auto* v = static_cast<ClipVoice*>(ctx);
  const auto n = static_cast<int64_t>(v->src ? v->src->frames() : 0);
  for (int i = 0; i < frames; ++i) {
    const int64_t k = v->cursor + i;
    const int64_t f = v->reverse ? n - 1 - k : k;
    const bool in = k < v->endFrame && f >= 0 && f < n;
    dst[0][i] = in ? v->src->left[static_cast<size_t>(f)] : 0.f;
    dst[1][i] = in ? v->src->right[static_cast<size_t>(f)] : 0.f;
  }
  v->cursor += frames;
}

// Band-limited read (J-87): plain clips, sample-rate conversion.
inline void readFrameSinc(const Sample& s, double sec, bool reverse, float& l, float& r, const stretch::SincTable& t, double rate) {
  double f = sec * s.sampleRate;
  const double last = static_cast<double>(s.frames() - 1);
  if (reverse) f = last - f;
  if (!(f >= -1e-6) || f > last + 1e-6) {
    l = r = 0.f;
    return;
  }
  const double fr = f - std::floor(f);
  if (rate <= 1.000001 && (fr < 1e-7 || fr > 1 - 1e-7)) {  // on a sample, not reading faster: exact
    const auto i = static_cast<size_t>(std::clamp(std::llround(f), 0LL, static_cast<long long>(s.frames() - 1)));
    l = s.left[i];
    r = s.right[i];
    return;
  }
  stretch::readSinc2(s.left.data(), s.right.data(), static_cast<int64_t>(s.frames()), f, rate, t, l, r);
}

// J-170: the stream primed ahead is the one a start at the clip's first
// frame makes: the same sound, window, direction and speed, and the same
// source frame within rounding (the source position each block computes for
// the clip's first frame carries its own rounding, so the frame it floors to
// can differ by one; tempo-synced clips: the speed within rounding too). A
// channel pitch that moved since it began priming is glided to (the stream
// followed it while priming).
bool primedFits(const ClipVoice& rv, const Sample& s, const ProgAudioClip& c, double speed, double pitch, int64_t cursor0) {
  return rv.src == &s && rv.reverse == c.reverse && rv.endFrame == static_cast<int64_t>(std::ceil(c.srcEnd * s.sampleRate)) &&
         std::llabs(rv.primeCursor - cursor0) <= 1 && std::fabs(speed - rv.primeSpeed) <= 1e-9 * std::max(1.0, speed) && rv.primePitch > 0 && pitch > 0 &&
         std::fabs(std::log(pitch / rv.primePitch)) < std::log(2.0) / 24.0;
}

// The heard pitch (before the channel's) at `secRate` source seconds per
// playback second. J-193: a resampled tempo-locked clip's follows its speed.
double pitchOf(const ProgAudioClip& c, double secRate) {
  if (c.resampleSync) return (c.granular ? c.pitch : 1.0) * secRate;
  return c.granular ? c.pitch : c.speed;
}

}  // namespace

std::shared_ptr<ClipVoice> makeClipVoice(double sampleRate, stretch::Engine engine) {
  auto v = std::make_shared<ClipVoice>();
  v->voice.prepare(2, sampleRate, 8.0, 8.0, false, engine);
  return v;
}

void renderAudioClips(const std::vector<ProgAudioClip>& clips, const TempoMap& map, double sr, double anchorTick,
                      int64_t clock, int from, int len, float* L, float* R, double pitchSemis) {
  if (clips.empty() || len <= 0) return;
  const double chanRatio = pitchSemis != 0 ? std::pow(2.0, pitchSemis / 12.0) : 1.0;
  const stretch::SincTable& rt = stretch::sincTable(stretch::Quality::Realtime);  // built by the program builder
  const double t0 = map.tickAfterSamples(anchorTick, static_cast<double>(clock), sr);
  const double t1 = map.tickAfterSamples(anchorTick, static_cast<double>(clock + len), sr);
  const double sec0 = map.secondsAt(t0);
  auto sampleOf = [&](double tick) {  // same rounding as note dispatch
    return static_cast<int64_t>(std::ceil(map.samplesBetween(anchorTick, tick, sr) - 1e-9)) - clock;
  };
  auto tickAt = [&](int64_t i) { return t0 + (t1 - t0) * static_cast<double>(i) / static_cast<double>(len); };
  for (const ProgAudioClip& c : clips) {
    if (static_cast<double>(c.start) > t1 + 1.0) break;  // sorted by start
    if (static_cast<double>(c.end) < t0 - 1.0 || !c.sample) continue;
    int64_t a = std::clamp<int64_t>(sampleOf(static_cast<double>(c.start)), 0, len);
    int64_t b = std::clamp<int64_t>(sampleOf(static_cast<double>(c.end)), 0, len);
    if (b <= a) continue;
    const Sample& s = *c.sample;
    // Source position at playback time u (seconds since the clip start).
    auto srcAt = [&](double u, double tickAtU) {
      return c.secPerTick > 0 ? c.srcStart + (tickAtU - static_cast<double>(c.start)) * c.secPerTick : c.srcStart + u * c.speed;
    };
    auto uOf = [&](int64_t i) { return sec0 + static_cast<double>(i) / sr - c.startSec; };
    // Declick only where the clip cuts into the audio (an explicit fade replaces it).
    // J-43: FL's per-edge declicker mode: 2 bypasses it, 1 / 3 always declick.
    // J-196: the levels once per block (the UI thread may patch them).
    const uint8_t declickIn = c.declickIn, declickOut = c.declickOut;
    const double fadeIn = c.fadeIn, fadeOut = c.fadeOut;
    const double gain = c.gain;
    const bool dIn = declickIn == 2 ? false : declickIn == 1 || declickIn == 3 || c.srcStart > 0;
    const bool dOut = declickOut == 2 ? false : declickOut == 1 || declickOut == 3 || c.cutEnd;
    const double fin = fadeIn > 0 ? fadeIn : (dIn ? kDeclick : 0.0);
    const double fout = fadeOut > 0 ? fadeOut : (dOut ? kDeclick : 0.0);
    // J-43: FL's fade law, sampled like FL (n = round(length · rate) steps).
    const FadeLaw lawIn = FadeLaw::make(c.shapeIn, c.tensionIn), lawOut = FadeLaw::make(c.shapeOut, c.tensionOut);
    const double nIn = fadeSamples(fadeIn, sr), nOut = fadeSamples(fadeOut, sr);
    // J-87: the offline render, while it applies.
    // (A render in progress publishes its finished start: frames below hqReady.)
    // J-145: a render at another pitch still plays while the realtime stretch fades in.
    const stretch::RenderedClip* hq = c.hq.get();
    const bool hqPitch = hq && std::fabs(pitchSemis - c.hqSemis) < 1e-6;
    const int64_t hqReady = hq ? hq->readyFrames.load(std::memory_order_acquire) : 0;
    const int64_t hqFrames = hq ? hq->frames() : 0;
    const double mixStep = 1.0 / std::max(1.0, kSwitchFade * sr);
    const int64_t first = sampleOf(static_cast<double>(c.start));  // block index of the clip's first sample
    const double readRate = c.speed * s.sampleRate / sr;              // source frames per output frame (plain reads)
    // J-193: a resampled tempo-locked clip's read rate over this block.
    const double syncRate = c.resampleSync ? std::max(1e-6, c.secPerTick * (t1 - t0) * s.sampleRate / static_cast<double>(len)) : 1.0;
    const bool plain = !c.granular && chanRatio == 1.0;
    // J-153: the realtime stretch runs over the frames of this block unless
    // the render covers all of them (and the crossfade to it is complete).
    ClipVoice* rv = c.rt.get();
    bool rtOn = false;
    if (rv && !plain) {
      const int64_t ma = a - first, mb = b - first;
      const bool covered = hqPitch && ma >= 0 && (mb <= hqReady || hqReady >= hqFrames);
      rtOn = !(covered && (c.rtMixAt != ma || c.rtMix >= 1.0));
    }
    for (int64_t s0 = a; s0 < b; s0 += ClipVoice::kOut) {
      const int64_t s1 = std::min<int64_t>(b, s0 + ClipVoice::kOut);
      if (rtOn) {
        const int n = static_cast<int>(s1 - s0);
        const int64_t m0 = s0 - first;
        const double src0 = srcAt(uOf(s0), tickAt(s0)), src1 = srcAt(uOf(s1), tickAt(s1));
        // Source seconds per playback second over this stretch of the block.
        const double secRate = c.secPerTick > 0 ? std::max(0.0, (src1 - src0) * sr / n) : c.speed;
        const double toFrames = s.sampleRate / sr;
        const double speed = secRate * toFrames;
        const double pitch = pitchOf(c, secRate) * chanRatio * toFrames;
        bool restart = rv->next != m0 || rv->gen != c.rtGen;  // a jump (or the first block, J-196: a new timing): a fresh stream, primed
        const int pre = rv->voice.prerollFor(speed);
        const int64_t cursor0 = static_cast<int64_t>(std::floor(src0 * s.sampleRate)) - pre;
        if (!restart && rv->primed)  // J-170: begun ahead; it plays only if it is the stream starting here would make
          restart = !primedFits(*rv, s, c, speed, pitch, cursor0);
        if (restart) {
          rv->gen = c.rtGen;
          rv->src = &s;
          rv->reverse = c.reverse;
          rv->endFrame = static_cast<int64_t>(std::ceil(c.srcEnd * s.sampleRate));
          rv->cursor = cursor0;
          rv->voice.start(&readClip, rv, speed, pitch, pre);
        }
        rv->primed = false;
        float* o[2] = {rv->out[0], rv->out[1]};
        rv->voice.render(o, n, speed, pitch, &readClip, rv);
        rv->next = m0 + n;
      }
      for (int64_t i = s0; i < s1; ++i) {
        const double u = uOf(i);
        double g = gain;
        // Fades (J-38: curve shape and tension; the 2 ms declick stays linear).
        if (fin > 0 && u < fin) g *= fadeIn > 0 ? lawIn.gain(fadeProgressIn(u * sr, nIn)) : std::max(0.0, u / fin);
        const double rest = c.durSec - u;
        if (fout > 0 && rest < fout) g *= fadeOut > 0 ? lawOut.gain(fadeProgressOut(rest * sr, nOut)) : std::max(0.0, rest / fout);
        if (g <= 0.0) continue;
        float l = 0.f, r = 0.f;
        const int64_t m = i - first;
        // The render's frame, if it has one here (past its end: silence).
        float hl = 0.f, hr = 0.f;
        const bool hqHas = hq && m >= 0 && (m < hqReady || hqReady >= hqFrames);
        if (hqHas && m < hqReady) hl = hq->L[static_cast<size_t>(m)], hr = hq->R[static_cast<size_t>(m)];
        const double target = hqHas && hqPitch ? 1.0 : 0.0;
        double mix = c.rtMixAt == m ? c.rtMix : target;  // after a jump: no fade
        mix = mix < target ? std::min(target, mix + mixStep) : std::max(target, mix - mixStep);
        if (!hqHas) mix = 0;
        c.rtMix = mix, c.rtMixAt = m + 1;
        if (hqPitch && m < 0) continue;
        if (mix >= 1.0) {
          l = hl, r = hr;
        } else if (rtOn) {
          l = rv->out[0][i - s0], r = rv->out[1][i - s0];
        } else {  // plain read (also the fallback without a realtime stretch: the timing stays)
          const double src = srcAt(u, tickAt(i));
          if (src < c.srcEnd) readFrameSinc(s, src, c.reverse, l, r, rt, c.resampleSync ? syncRate : c.secPerTick > 0 ? 1.0 : readRate);
        }
        if (mix > 0.0 && mix < 1.0) {  // crossfading (equal gain: both carry the same sound)
          // An S-curve over the linear ramp: no corner where the fade starts or ends.
          const double w = mix * mix * (3.0 - 2.0 * mix);
          l = static_cast<float>(w * hl + (1.0 - w) * l);
          r = static_cast<float>(w * hr + (1.0 - w) * r);
        }
        L[from + i] += l * static_cast<float>(g);
        R[from + i] += r * static_cast<float>(g);
      }
    }
  }

  // J-170: the realtime Rubber Band stream of a clip starting soon begins
  // now and does its start-up work (pre-roll, the engine's start delay: 17
  // engine steps at 44.1 kHz) spread over the blocks before the clip, which
  // then plays it from its first frame (above) as if it had started there.
  const auto ahead = static_cast<int64_t>(std::llround(kPrimeAhead * sr));
  const double tA = map.tickAfterSamples(anchorTick, static_cast<double>(clock + len + ahead), sr);
  for (const ProgAudioClip& c : clips) {
    if (static_cast<double>(c.start) > tA + 1.0) break;  // sorted by start
    ClipVoice* rv = c.rt.get();
    if (!rv || c.engine != stretch::Engine::RubberBand || !c.sample || (!c.granular && chanRatio == 1.0)) continue;
    const int64_t first = sampleOf(static_cast<double>(c.start));
    if (first < len || first >= len + ahead) continue;
    if (c.hq && std::fabs(pitchSemis - c.hqSemis) < 1e-6 && c.hq->readyFrames.load(std::memory_order_acquire) > 0) continue;  // its render plays
    const Sample& s = *c.sample;
    // As the block it starts in will compute them.
    const double toFrames = s.sampleRate / sr;
    const double secRate = c.secPerTick > 0 ? std::max(0.0, c.secPerTick * (t1 - t0) * sr / len) : c.speed;
    const double speed = secRate * toFrames;
    const double pitch = pitchOf(c, secRate) * chanRatio * toFrames;
    const double u = sec0 + static_cast<double>(first) / sr - c.startSec;
    const double tk = t0 + (t1 - t0) * static_cast<double>(first) / static_cast<double>(len);
    const double src0 = c.secPerTick > 0 ? c.srcStart + (tk - static_cast<double>(c.start)) * c.secPerTick : c.srcStart + u * c.speed;
    const int pre = rv->voice.prerollFor(speed);
    const int64_t cursor0 = static_cast<int64_t>(std::floor(src0 * s.sampleRate)) - pre;
    if (!(rv->primed && rv->next == 0 && rv->gen == c.rtGen && primedFits(*rv, s, c, speed, pitch, cursor0))) {
      rv->gen = c.rtGen;
      rv->src = &s;
      rv->reverse = c.reverse;
      rv->endFrame = static_cast<int64_t>(std::ceil(c.srcEnd * s.sampleRate));
      rv->cursor = cursor0;
      rv->voice.startLater(&readClip, rv, speed, pitch, pre);
      rv->primed = true, rv->next = 0;
      rv->primeSpeed = speed, rv->primePitch = pitch, rv->primeCursor = cursor0, rv->credit = 0;
    }
    const int left = rv->voice.primeLeft(speed);
    if (left <= 0) continue;
    rv->credit += first <= 2 * static_cast<int64_t>(len) ? 1e9 : static_cast<double>(left) * len / static_cast<double>(first - len);
    if (rv->credit < std::min(256.0, static_cast<double>(left))) continue;  // (whole engine steps)
    const int64_t f0 = rv->voice.fedFrames();
    rv->voice.prime(&readClip, rv, speed, pitch, static_cast<int>(std::min(rv->credit, 1e9)));
    rv->credit -= static_cast<double>(rv->voice.fedFrames() - f0);
  }
}

}  // namespace gl
