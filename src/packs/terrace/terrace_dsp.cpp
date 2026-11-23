// Gridline — "terrace" pack: Terrace's engine (see terrace.h). Audio thread: no allocation, no
// locks, no I/O in process(); the step bank is sized once and refilled when the state text changes.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include "../../engine/Dsp.h"
#include "../../engine/EffectRest.h"
#include "../../engine/ShaperSteps.h"
#include "../Packs.h"
#include "terrace.h"

namespace gl::packs::terrace {

namespace {

using namespace dsp;

float par(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
int ipar(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(par(s, i, static_cast<float>(dflt)))); }
bool bpar(const PluginState& s, int i, bool dflt = false) { return par(s, i, dflt ? 1.f : 0.f) >= 0.5f; }
double coefMs(double ms, double sr) { return std::exp(-1.0 / std::max(1.0, ms * 0.001 * sr)); }

// The step sizes (indices of pp::kShaperRateBeats) from the shortest to the longest.
constexpr int kSizeOrder[11] = {2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 11};
// A step edge falls on the same sample whatever the block size: positions computed as block start
// plus offset land a hair either side of an exact edge, this keeps them past it.
constexpr double kBeatBias = 1e-9;

// RBJ sections (bilinear, prewarped at hz): 0 low pass, 1 high pass, 2 all pass.
void rbj(Section& s, int type, double sr, double hz, double q) {
  hz = std::clamp(hz, 10.0, sr * 0.49);
  const double w = kTwoPi * hz / sr, cw = std::cos(w), sw = std::sin(w), alpha = sw / (2.0 * q), a0 = 1 + alpha;
  double b0, b1, b2;
  if (type == 0) b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2;
  else if (type == 1) b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2;
  else b0 = 1 - alpha, b1 = -2 * cw, b2 = 1 + alpha;
  s.b0 = b0 / a0, s.b1 = b1 / a0, s.b2 = b2 / a0, s.a1 = -2 * cw / a0, s.a2 = (1 - alpha) / a0;
}

// The Butterworth sections of each slope: a Linkwitz-Riley filter is a Butterworth filter twice.
constexpr double kQ2 = 0.70710678118654752, kQ4a = 0.54119610014619698, kQ4b = 1.30656296487637652;

// Deterministic random in [-1, 1] for an LFO's cycle n.
double cycleRandom(int64_t n, int lfo) {
  uint64_t z = static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(lfo + 7) << 52);
  z ^= z >> 30, z *= 0xBF58476D1CE4E5B9ull;
  z ^= z >> 27, z *= 0x94D049BB133111EBull;
  z ^= z >> 31;
  return static_cast<double>(z >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

// An LFO's value (-1..1) at `cycles` cycles from its origin.
double lfoValue(int shape, double cycles, int lfo) {
  const double f = std::floor(cycles), p = cycles - f;
  switch (shape) {
    case ShSine: return std::sin(kTwoPi * p);
    case ShTriangle: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
    case ShSawUp: return 2 * p - 1;
    case ShSawDown: return 1 - 2 * p;
    case ShSquare: return p < 0.5 ? 1 : -1;
    case ShRandom: return cycleRandom(static_cast<int64_t>(f), lfo);
    default: {
      const double a = cycleRandom(static_cast<int64_t>(f), lfo), b = cycleRandom(static_cast<int64_t>(f) + 1, lfo);
      return a + (b - a) * (0.5 - 0.5 * std::cos(kPi * p));
    }
  }
}

// Prepared on the UI thread (prepareAux): the note source channel.
struct TerraceAux final : AuxData {
  uint32_t channel = 0;
};

struct Telemetry {
  std::atomic<float> play[kBands][kLanes];
  std::atomic<float> level[kBands];
  std::atomic<float> lfo[kLfos];
  std::atomic<float> follower{0}, sidechain{0};
  std::atomic<int> pattern{0}, chainSlot{-1};
  std::atomic<bool> playing{false};
  Telemetry() {
    for (auto& b : play)
      for (auto& l : b) l.store(-1, std::memory_order_relaxed);
    for (auto& l : level) l.store(0, std::memory_order_relaxed);
    for (auto& l : lfo) l.store(0, std::memory_order_relaxed);
  }
};

class TerraceFx final : public Effect {
 public:
  // K-142 (0.7.8): rests by the hosted rule (stopped, fed silence, its output inaudible); while
  // it rests, its steps and LFOs move on: its idle render runs unheard (EffectRest.h).
  bool restsWhenFaint() const override { return true; }
  void rested(const PluginState& s, const ChannelAux& aux, const ProcessContext& c, int n) override { restRenderFx(*this, s, aux, c, n); }
  TerraceFx() : bank_(std::make_unique<Bank>()) {
    clear(*bank_);
    reset();
  }

  void setSideIo(const SideIo& io) override { io_ = io; }

  void processAux(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override {
    const auto* ta = dynamic_cast<const TerraceAux*>(aux.custom.get());
    srcChannel_ = ta ? ta->channel : 0;
    process(s, ctx, L, R, n);
  }

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    const double tempo = ctx.tempo > 1 ? ctx.tempo : 120.0;
    const double spb = 60.0 / tempo * sr;  // samples per beat
    const int bands = ipar(s, TrBands, 1) == 0 ? 3 : 4;
    const int slope = std::clamp(ipar(s, TrSlope, Slope24), 0, 2);
    const double len = kLengthBeats[std::clamp(ipar(s, TrLength, 1), 0, 3)];
    const double sm = coefMs(par(s, TrSmooth, 3), sr);
    const double outDb = par(s, TrOutput), mixP = std::clamp(static_cast<double>(par(s, TrMix, 1)), 0.0, 1.0);
    const double splitP[3] = {par(s, TrSplit1, 120), par(s, TrSplit2, 900), par(s, TrSplit3, 5000)};

    // The step bank, parsed when the text changes.
    if (s.data.empty() != bankEmpty_ || (!s.data.empty() && std::hash<std::string>{}(s.data) != dataHash_)) {
      bankEmpty_ = s.data.empty();
      dataHash_ = bankEmpty_ ? 0 : std::hash<std::string>{}(s.data);
      parse(s.data.c_str(), *bank_);
    }
    const Bank& bank = *bank_;

    // Which pattern plays: the Pattern control, a note's pick (until the next note, a change of the
    // control or a jump of the position), or the chain (one slot per Length).
    const int patParam = std::clamp(ipar(s, TrPattern), 0, kPatterns - 1);
    if (patParam != lastPatParam_) lastPatParam_ = patParam, latched_ = -1;
    if (ctx.playing && ctx.jumped) latched_ = -1, restart_ = 0, restarted_ = false;
    const bool chain = bpar(s, TrChain) && bank.chainLen > 0;

    // This block's notes from the chosen source (in block order).
    struct NoteAt {
      int off, note;
      bool hasNote;
    };
    NoteAt notes[64];
    int noteN = 0;
    const int listen = std::clamp(ipar(s, TrScListen, LsLevel), 0, 2);
    const bool retrig = bpar(s, TrScRetrig), notePats = bpar(s, TrNotePatterns);
    const bool wantNotes = listen != LsLevel || retrig || notePats;
    if (wantNotes && ctx.hits) {
      const int src = std::clamp(ipar(s, TrNoteSrc, NsChannel), 0, 2), port = ipar(s, TrNotePort, -1);
      for (int k = 0; k < ctx.hits->count && noteN < 64; ++k) {
        const NoteHitBus::Hit& h = ctx.hits->hits[static_cast<size_t>(k)];
        bool ok;
        if (src == NsChannel) ok = srcChannel_ != 0 && h.channel == srcChannel_ && h.kind != NoteHitBus::Input;
        else if (src == NsInput) ok = h.kind == NoteHitBus::Input;
        else ok = h.kind == NoteHitBus::Note && h.port >= 0 && (port < 0 || h.port == port);
        if (!ok) continue;
        NoteAt e{std::clamp(static_cast<int>(h.offset), 0, std::max(0, n - 1)), h.note, h.kind != NoteHitBus::ClipStart};
        int at = noteN++;
        for (; at > 0 && notes[at - 1].off > e.off; --at) notes[at] = notes[at - 1];
        notes[at] = e;
      }
    }
    const int noteBase = std::clamp(ipar(s, TrNoteBase, 48), 0, 127);

    // Lanes as set (the routes add to them per control block).
    LaneSet lane[kBands][kLanes];
    bool solo[kBands], mute[kBands], anySolo = false;
    double gainDb[kBands];
    for (int b = 0; b < kBands; ++b) {
      for (int l = 0; l < kLanes; ++l) {
        LaneSet& e = lane[b][l];
        e.steps = std::clamp(ipar(s, laneParam(b, l, LpSteps), 15) + 1, 1, kSteps);
        e.size = std::clamp(ipar(s, laneParam(b, l, LpSize), 3), 0, pp::kShaperRates - 1);
        e.swing = std::clamp(static_cast<double>(par(s, laneParam(b, l, LpSwing))), 0.0, 1.0);
        e.depth = std::clamp(static_cast<double>(par(s, laneParam(b, l, LpDepth), 1)), 0.0, 1.0);
        e.curve = std::clamp(static_cast<double>(par(s, laneParam(b, l, LpCurve))), -1.0, 1.0);
      }
      solo[b] = bpar(s, bandParam(b, BpSolo)) && b < bands;
      mute[b] = bpar(s, bandParam(b, BpMute));
      gainDb[b] = par(s, bandParam(b, BpGain));
      anySolo = anySolo || solo[b];
    }
    // The routes in use.
    struct Route {
      int src, tgt;
      double amt;
    };
    Route routes[kRoutes];
    int routeN = 0;
    for (int r = 0; r < kRoutes; ++r) {
      const int src = std::clamp(ipar(s, routeParam(r, RtSource)), 0, SrcCount - 1), tgt = std::clamp(ipar(s, routeParam(r, RtTarget)), 0, TgCount - 1);
      const double amt = std::clamp(static_cast<double>(par(s, routeParam(r, RtAmount))), -1.0, 1.0);
      if (src != SrcOff && tgt != TgNone && amt != 0) routes[routeN++] = {src, tgt, amt};
    }
    struct Lfo {
      bool sync, restart;
      int shape;
      double rate, hz, phase, amount;
    };
    Lfo lfo[kLfos];
    for (int i = 0; i < kLfos; ++i) {
      Lfo& o = lfo[i];
      o.sync = bpar(s, lfoParam(i, LfSync), true);
      o.rate = kLfoRateBeats[std::clamp(ipar(s, lfoParam(i, LfRate), 9), 0, kLfoRates - 1)];
      o.hz = std::clamp(static_cast<double>(par(s, lfoParam(i, LfHz), 1)), 0.001, 100.0);
      o.shape = std::clamp(ipar(s, lfoParam(i, LfShape)), 0, ShCount - 1);
      o.phase = par(s, lfoParam(i, LfPhase)) / 360.0;
      o.amount = std::clamp(static_cast<double>(par(s, lfoParam(i, LfAmount), 1)), -1.0, 1.0);
      o.restart = ipar(s, lfoParam(i, LfRetrig)) == 1;
    }
    const int folSrc = std::clamp(ipar(s, TrFolSource), 0, kBands);
    const double folAtt = coefMs(par(s, TrFolAttack, 10), sr), folRel = coefMs(par(s, TrFolRelease, 150), sr);
    const double folGain = dbToGain(par(s, TrFolGain)), folAmt = std::clamp(static_cast<double>(par(s, TrFolAmount, 1)), -1.0, 1.0);
    const double scThr = par(s, TrScThreshold, -24), scRel = coefMs(par(s, TrScRelease, 80), sr), scAtt = coefMs(1.0, sr);
    const double scAmt = std::clamp(static_cast<double>(par(s, TrScAmount, 1)), -1.0, 1.0);
    const float* scL = io_.scL;
    const float* scR = io_.scR ? io_.scR : io_.scL;

    // Control blocks of kCtl samples (modulation, filter coefficients) on the plugin's own sample
    // count, not the host's block: any block size plays the same.
    auto& eff = ctl_.eff;
    auto& stepBeats = ctl_.stepBeats;
    auto& gainLin = ctl_.gainLin;
    auto& cutAdd = ctl_.cutAdd;
    auto& panAdd = ctl_.panAdd;
    const double& mixE = ctl_.mixE;
    const double& outLin = ctl_.outLin;
    for (int i0 = 0; i0 < n;) {
      const int i1 = std::min(n, i0 + (kCtl - ctlPhase_));
      const bool fresh = ctlPhase_ == 0 || !ctl_.ready;
      ctlPhase_ = (ctlPhase_ + (i1 - i0)) % kCtl;
      if (fresh) {
      ctl_.ready = true;
      const double beat0 = clockAt(ctx, i0, spb);
      // ---- sources
      double srcVal[SrcCount] = {};
      for (int i = 0; i < kLfos; ++i) {
        const Lfo& o = lfo[i];
        double origin = 0;
        if (o.restart) origin = std::max(std::floor(beat0 / len) * len, restarted_ ? restart_ : 0.0);
        const double t = beat0 - origin;
        const double cycles = (o.sync ? t / o.rate : t * 60.0 / tempo * o.hz) + o.phase;
        srcVal[SrcLfo1 + i] = lfoValue(o.shape, cycles, i) * o.amount;
      }
      srcVal[SrcFollower] = std::clamp(fol_ * folGain, 0.0, 1.0) * folAmt;
      const double scDb = 20.0 * std::log10(std::max(1e-9, scEnv_));
      const double lvl = std::clamp((scDb - scThr) / 12.0, 0.0, 1.0);
      srcVal[SrcSidechain] = (listen == LsLevel ? lvl : listen == LsNotes ? noteEnv_ : std::max(lvl, noteEnv_)) * scAmt;
      double mod[TgCount] = {};
      for (int r = 0; r < routeN; ++r) mod[routes[r].tgt] += srcVal[routes[r].src] * routes[r].amt;
      // ---- the crossover (splits ascending, at least a third of an octave apart)
      double f[3];
      for (int k = 0; k < 3; ++k) f[k] = std::clamp(splitP[k] * std::pow(2.0, 2.0 * mod[TgSplit1 + k]), 20.0, 20000.0);
      for (int k = 1; k < 3; ++k) f[k] = std::max(f[k], f[k - 1] * 1.26);
      for (int c = 0; c < 2; ++c) xo_[c].set(sr, bands, slope, f);
      // ---- each band's lanes and levels with the routes
      for (int b = 0; b < kBands; ++b) {
        const double* m = mod + bandTarget(b, 0);
        for (int l = 0; l < kLanes; ++l) {
          LaneSet e = lane[b][l];
          e.depth = std::clamp(e.depth + m[TbVolDepth + l], 0.0, 1.0);
          e.swing = std::clamp(e.swing + m[TbSwing], 0.0, 1.0);
          int size = e.size;
          const int shift = static_cast<int>(std::lround(m[TbSize] * 4.0));
          if (shift != 0) {
            if (size == 0) size = len <= 2 ? 1 : len <= 4 ? 3 : len <= 8 ? 5 : 7;  // Auto: the size it plays
            int at = 0;  // along the sizes from shortest to longest: a positive route makes steps longer
            while (at < 10 && kSizeOrder[at] != size) ++at;
            size = kSizeOrder[std::clamp(at + shift, 0, 10)];
          }
          stepBeats[b][l] = size == 0 ? len / pp::kShaperPoints : pp::kShaperRateBeats[size];
          eff[b][l] = e;
        }
        gainLin[b] = dbToGain(std::clamp(gainDb[b] + 24.0 * m[TbGain], -96.0, 24.0));
        cutAdd[b] = m[TbCutoff];
        panAdd[b] = m[TbPan];
      }
      ctl_.mixE = std::clamp(mixP + mod[TgMix], 0.0, 1.0);
      ctl_.outLin = dbToGain(std::clamp(outDb + 24.0 * mod[TgOutput], -96.0, 24.0));
      // ---- the cutoff filters (each band's low pass, from its smoothed cutoff lane)
      for (int b = 0; b < bands; ++b) {
        const double lo = b == 0 ? 20.0 : f[b - 1];
        const double fc = lo * std::pow(20000.0 / lo, std::clamp(cut_[b], 0.0, 1.0));
        for (int c = 0; c < 2; ++c) svf_[b][c].set(sr, fc, 0.1);
      }
      }  // fresh
      int pat = patParam;
      for (int i = i0; i < i1; ++i) {
        const double beat = clockAt(ctx, i, spb);
        while (nextNote_ < noteN && notes[nextNote_].off <= i) {
          const NoteAt& e = notes[nextNote_++];
          if (listen != LsLevel) noteEnv_ = 1;
          if (retrig) restart_ = beat, restarted_ = true;
          if (notePats && e.hasNote && e.note >= noteBase && e.note < noteBase + kPatterns) latched_ = e.note - noteBase;
        }
        noteEnv_ *= scRel;
        pat = patParam;
        int slotNow = -1;
        if (latched_ >= 0) {
          pat = latched_;
        } else if (chain) {
          const int64_t slot = static_cast<int64_t>(std::floor(beat / len));
          slotNow = static_cast<int>(((slot % bank.chainLen) + bank.chainLen) % bank.chainLen);
          if (bank.chain[slotNow] >= 0) pat = bank.chain[slotNow];  // an empty slot plays the chosen pattern
        }
        chainSlot_ = slotNow;
        const double lb = restarted_ ? beat - restart_ + kBeatBias : beat;  // the lanes' clock
        // ---- the lanes
        for (int b = 0; b < bands; ++b) {
          double v[kLanes];
          for (int l = 0; l < kLanes; ++l) {
            const LaneSet& e = eff[b][l];
            const shsteps::LaneRef ref{bank.chance[pat][b][l], bank.hits[pat][b][l], bank.anyChance[pat][b][l], bank.anyHits[pat][b][l], b * kLanes + l,
                                       pat, laneRest(l)};
            int k;
            double fr;
            const int64_t abs = shsteps::laneStep(lb, stepBeats[b][l], e.swing, e.steps, k, fr);
            const float* vals = bank.val[pat][b][l];
            v[l] = shsteps::laneValue(ref, k, fr, abs, e.steps, e.curve, [vals](int kk) { return static_cast<double>(vals[kk]); });
            if (i == i0) pos_[b][l] = static_cast<float>(k + fr);
          }
          const double vol = 1.0 - eff[b][LnVol].depth * (1.0 - v[LnVol]);
          const double cut = std::clamp(1.0 - eff[b][LnCut].depth * (1.0 - v[LnCut]) + cutAdd[b], 0.0, 1.0);
          const double pan = std::clamp(eff[b][LnPan].depth * v[LnPan] + panAdd[b], -1.0, 1.0);
          const double on = (anySolo ? (solo[b] ? 1.0 : 0.0) : 1.0) * (mute[b] ? 0.0 : 1.0);
          vol_[b] = vol + (vol_[b] - vol) * sm;
          cut_[b] = cut + (cut_[b] - cut) * sm;
          pan_[b] = pan + (pan_[b] - pan) * sm;
          gain_[b] = gainLin[b] * on + (gain_[b] - gainLin[b] * on) * sm;
        }
        // ---- the signal
        const double x[2] = {L[i], R[i]};
        double band[2][kBands];
        for (int c = 0; c < 2; ++c) xo_[c].split(x[c], band[c]);
        const double folIn = folSrc == 0 ? std::max(std::fabs(x[0]), std::fabs(x[1]))
                                         : (folSrc <= bands ? std::max(std::fabs(band[0][folSrc - 1]), std::fabs(band[1][folSrc - 1])) : 0.0);
        fol_ = folIn > fol_ ? folIn + (fol_ - folIn) * folAtt : folIn + (fol_ - folIn) * folRel;
        if (scL) {
          const double a = std::max(std::fabs(static_cast<double>(scL[i])), std::fabs(static_cast<double>(scR[i])));
          scEnv_ = a > scEnv_ ? a + (scEnv_ - a) * scAtt : a + (scEnv_ - a) * scRel;
        } else {
          scEnv_ *= scRel;
        }
        double wet[2] = {0, 0};
        for (int b = 0; b < bands; ++b) {
          double pl, pr;
          panGains(pan_[b], pl, pr);
          const double w = std::clamp((1.0 - cut_[b]) * 50.0, 0.0, 1.0);  // the open filter is no filter: flat at rest
          const double g = vol_[b] * gain_[b];
          double peak = 0;
          for (int c = 0; c < 2; ++c) {
            double y = band[c][b];
            const double lp = svf_[b][c].process(y);
            y += w * (lp - y);
            y *= g * (c ? pr : pl);
            wet[c] += y;
            peak = std::max(peak, std::fabs(y));
          }
          peakHold_[b] = std::max(peakHold_[b] * 0.9997, peak);
        }
        if (mix_ < 0) mix_ = mixE, out_ = outLin;
        mix_ = mixE + (mix_ - mixE) * sm, out_ = outLin + (out_ - outLin) * sm;
        for (int c = 0; c < 2; ++c) {
          const double dry = xo_[c].dry(x[c]);
          const double y = (dry * (1.0 - mix_) + wet[c] * mix_) * out_;
          (c ? R : L)[i] = static_cast<float>(y);
        }
      }
      lastPat_ = pat;
      i0 = i1;
    }
    nextNote_ = 0;
    // ---- what the editor shows
    tel_.playing.store(ctx.playing, std::memory_order_relaxed);
    tel_.pattern.store(lastPat_, std::memory_order_relaxed);
    tel_.chainSlot.store(chainSlot_, std::memory_order_relaxed);
    for (int b = 0; b < kBands; ++b) {
      for (int l = 0; l < kLanes; ++l) tel_.play[b][l].store(b < bands ? pos_[b][l] : -1.f, std::memory_order_relaxed);
      tel_.level[b].store(static_cast<float>(peakHold_[b]), std::memory_order_relaxed);
    }
    {
      const double beat = clockPeek(ctx, n, spb);
      for (int i = 0; i < kLfos; ++i) {
        const Lfo& o = lfo[i];
        double origin = 0;
        if (o.restart) origin = std::max(std::floor(beat / len) * len, restarted_ ? restart_ : 0.0);
        const double t = beat - origin;
        tel_.lfo[i].store(static_cast<float>(lfoValue(o.shape, (o.sync ? t / o.rate : t * 60.0 / tempo * o.hz) + o.phase, i)), std::memory_order_relaxed);
      }
    }
    tel_.follower.store(static_cast<float>(std::clamp(fol_ * folGain, 0.0, 1.0)), std::memory_order_relaxed);
    {
      const double scDb = 20.0 * std::log10(std::max(1e-9, scEnv_));
      const double lvl = std::clamp((scDb - scThr) / 12.0, 0.0, 1.0);
      tel_.sidechain.store(static_cast<float>(listen == LsLevel ? lvl : listen == LsNotes ? noteEnv_ : std::max(lvl, noteEnv_)), std::memory_order_relaxed);
    }
    if (!ctx.playing) {
      free_ += n / spb;
      if (free_ > 1e6) free_ = 0, restart_ = 0, restarted_ = false;
    }
  }

  void reset() override {
    for (auto& x : xo_) x.clear();
    for (auto& b : svf_)
      for (auto& c : b) c.reset();
    for (int b = 0; b < kBands; ++b) {
      vol_[b] = cut_[b] = gain_[b] = 1, pan_[b] = 0, peakHold_[b] = 0;
      for (auto& p : pos_[b]) p = 0;
    }
    free_ = 0, fol_ = 0, scEnv_ = 0, noteEnv_ = 0, mix_ = -1, out_ = 1;
    ctlPhase_ = 0, ctl_.ready = false;
    latched_ = -1, restart_ = 0, restarted_ = false, nextNote_ = 0;
  }

  const Telemetry& telemetry() const { return tel_; }

 private:
  // Song position in beats at sample i of the block (stopped: the plugin's own free clock, from
  // the start of this block).
  double clockAt(const ProcessContext& ctx, int i, double spb) const {
    if (ctx.playing) return ctx.tick / std::max(1, ctx.ppq) + i / spb + kBeatBias;
    return free_ + i / spb + kBeatBias;
  }
  double clockPeek(const ProcessContext& ctx, int n, double spb) const { return clockAt(ctx, n, spb); }

  static constexpr int kCtl = 16;
  struct LaneSet {
    int steps, size;
    double swing, depth, curve;
  };
  // What a control block worked out (kept across host blocks until the next one).
  struct Ctl {
    LaneSet eff[kBands][kLanes] = {};
    double stepBeats[kBands][kLanes] = {}, gainLin[kBands] = {}, cutAdd[kBands] = {}, panAdd[kBands] = {};
    double mixE = 1, outLin = 1;
    bool ready = false;
  } ctl_;
  int ctlPhase_ = 0;
  std::unique_ptr<Bank> bank_;
  size_t dataHash_ = 0;
  bool bankEmpty_ = true;
  Crossover xo_[2];
  Svf svf_[kBands][2];
  double vol_[kBands], cut_[kBands], pan_[kBands], gain_[kBands], peakHold_[kBands];
  float pos_[kBands][kLanes];
  double free_ = 0, fol_ = 0, scEnv_ = 0, noteEnv_ = 0, restart_ = 0, mix_ = -1, out_ = 1;  // mix_ < 0: not started (no fade in)
  bool restarted_ = false;
  int lastPatParam_ = -1, latched_ = -1, nextNote_ = 0, lastPat_ = 0, chainSlot_ = -1;
  SideIo io_;
  uint32_t srcChannel_ = 0;
  Telemetry tel_;
};

}  // namespace

// ------------------------------------------------------------------ the crossover

void Crossover::set(double sampleRate, int bandCount, int slopeChoice, const double* splits) {
  bandCount = std::clamp(bandCount, 3, 4);
  slopeChoice = std::clamp(slopeChoice, 0, 2);
  const bool same = sampleRate == sr && bandCount == bands && slopeChoice == slope && splits[0] == hz[0] && splits[1] == hz[1] &&
                    (bandCount == 3 || splits[2] == hz[2]);
  if (same) return;
  sr = sampleRate, bands = bandCount, slope = slopeChoice;
  secs = slope == Slope12 ? 1 : slope == Slope24 ? 2 : 4;
  apSecs = slope == Slope48 ? 2 : 1;
  sign = slope == Slope12 ? -1.0 : 1.0;  // the 12 dB pair adds up in phase with the high side turned over
  Section ap[3][kMaxAp];
  for (int k = 0; k < bands - 1; ++k) {
    hz[k] = splits[k];
    const double f = splits[k];
    if (slope == Slope12) {
      rbj(lp[k][0], 0, sr, f, 0.5), rbj(hp[k][0], 1, sr, f, 0.5);
      Section& a = ap[k][0];  // low pass minus high pass: a first-order all-pass
      a = lp[k][0];
      a.b0 = lp[k][0].b0 - hp[k][0].b0, a.b1 = lp[k][0].b1 - hp[k][0].b1, a.b2 = lp[k][0].b2 - hp[k][0].b2;
    } else if (slope == Slope24) {
      for (int s = 0; s < 2; ++s) rbj(lp[k][s], 0, sr, f, kQ2), rbj(hp[k][s], 1, sr, f, kQ2);
      rbj(ap[k][0], 2, sr, f, kQ2);
    } else {
      for (int s = 0; s < 4; ++s) rbj(lp[k][s], 0, sr, f, s % 2 ? kQ4b : kQ4a), rbj(hp[k][s], 1, sr, f, s % 2 ? kQ4b : kQ4a);
      rbj(ap[k][0], 2, sr, f, kQ4a), rbj(ap[k][1], 2, sr, f, kQ4b);
    }
  }
  if (bands == 3) hz[2] = splits[2];
  auto copyCoefs = [](Section& to, const Section& from) { to.b0 = from.b0, to.b1 = from.b1, to.b2 = from.b2, to.a1 = from.a1, to.a2 = from.a2; };
  for (int k = 0; k < bands - 1; ++k)
    for (int s = 0; s < apSecs; ++s) {
      copyCoefs(apDry[k][s], ap[k][s]);
      for (int i = 0; i < k; ++i) copyCoefs(apBand[i][k][s], ap[k][s]);
    }
}

void Crossover::split(double x, double* out) {
  double r = x;
  const int ns = bands - 1;
  for (int i = 0; i < ns; ++i) {
    double lo = r, hi = r;
    for (int s = 0; s < secs; ++s) lo = lp[i][s].process(lo), hi = hp[i][s].process(hi);
    for (int j = i + 1; j < ns; ++j)  // the phase of the splits above this band
      for (int s = 0; s < apSecs; ++s) lo = apBand[i][j][s].process(lo);
    out[i] = lo;
    r = hi * sign;
  }
  out[ns] = r;
}

double Crossover::dry(double x) {
  for (int k = 0; k < bands - 1; ++k)
    for (int s = 0; s < apSecs; ++s) x = apDry[k][s].process(x);
  return x;
}

void Crossover::clear() {
  for (auto& k : lp)
    for (auto& s : k) s.clear();
  for (auto& k : hp)
    for (auto& s : k) s.clear();
  for (auto& b : apBand)
    for (auto& k : b)
      for (auto& s : k) s.clear();
  for (auto& k : apDry)
    for (auto& s : k) s.clear();
}

// ------------------------------------------------------------------ pack entry points

std::unique_ptr<Generator> createGenerator(const std::string&) { return nullptr; }

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == kType) return std::make_unique<TerraceFx>();
  return nullptr;
}

bool prepareAux(const PluginState& s, ChannelAux& aux) {
  if (s.type != kType) return false;
  auto ta = std::make_shared<TerraceAux>();
  ta->channel = noteChannel(s.data);
  aux.custom = std::move(ta);
  return true;
}

bool liveView(const Effect* fx, Live& out) {
  const auto* t = dynamic_cast<const TerraceFx*>(fx);
  if (!t) return false;
  const Telemetry& m = t->telemetry();
  for (int b = 0; b < kBands; ++b) {
    for (int l = 0; l < kLanes; ++l) out.play[b][l] = m.play[b][l].load(std::memory_order_relaxed);
    out.level[b] = m.level[b].load(std::memory_order_relaxed);
  }
  for (int i = 0; i < kLfos; ++i) out.lfo[i] = m.lfo[i].load(std::memory_order_relaxed);
  out.follower = m.follower.load(std::memory_order_relaxed);
  out.sidechain = m.sidechain.load(std::memory_order_relaxed);
  out.pattern = m.pattern.load(std::memory_order_relaxed);
  out.chainSlot = m.chainSlot.load(std::memory_order_relaxed);
  out.playing = m.playing.load(std::memory_order_relaxed);
  return true;
}

}  // namespace gl::packs::terrace
