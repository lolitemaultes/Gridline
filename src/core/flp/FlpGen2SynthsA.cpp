// Gridline — FLP import, second wave: Fruity DX10, MiniSynth, Ogun, Autogun, BeepMap.
//
//   Fruity DX10  → gl.fm        (carrier + two modulators: algorithm "4→3→1 + 2→1", op 4 silent)
//   MiniSynth    → gl.synth     (one oscillator model, filter + envelopes, LFO)
//   Ogun/Autogun → gl.additive  (metallic partial bank: "Glass" timbre, seed, decays, filter, unison, FX)
//   BeepMap      → gl.imagesynth (embedded picture → canvas, scale, pixel time, switches)
//
// Layouts (observed in FL's supplied projects and presets; see
// analysis/demos/instruments_wave2.md):
//   DX10     i32 version, then i32 per control tag 0..21 (0..65536; tag 21 octave −3..3).
//   MiniSynth "SSLF" + size, then an FLS chunk file: "HEAD" (name) and "DATA" = "0rev"
//            + 43 floats 0..1 in the plugin's parameter order.
//   Ogun     u32 version, zlib stream; inflated: i32 per control tag 0..48, then timbre
//            name / harmonic data. Autogun uses the same state.
//   BeepMap  i32 header (tag 0..6 at words 1..7), then a Windows BMP ("BM").
#include <algorithm>
#include <cmath>
#include "../../packs/models/models.h"
#include "../PluginParams.h"
#include "FlpGen2.h"

namespace gl::flp::gen2 {

namespace {

// ------------------------------------------------------------------ inflate
// Minimal RFC 1950/1951 decoder (stored, fixed and dynamic Huffman blocks),
// stopping after `limit` output bytes. Returns false on malformed data.
class Inflater {
 public:
  Inflater(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool run(std::vector<uint8_t>& out, size_t limit) {
    if (n_ < 2 || (p_[0] & 0x0F) != 8 || ((p_[0] << 8) | p_[1]) % 31 != 0) return false;
    pos_ = 2;
    bool last = false;
    while (!last && out.size() < limit) {
      last = bits(1) != 0;
      int type = static_cast<int>(bits(2));
      if (bad_) return false;
      if (type == 0) {
        bitBuf_ = bitCnt_ = 0;
        if (pos_ + 4 > n_) return false;
        size_t len = p_[pos_] | (p_[pos_ + 1] << 8);
        pos_ += 4;
        if (pos_ + len > n_) return false;
        out.insert(out.end(), p_ + pos_, p_ + pos_ + len);
        pos_ += len;
      } else if (type == 1 || type == 2) {
        Huff lit, dist;
        if (type == 1) {
          uint8_t l[288], d[30];
          for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
          for (int i = 0; i < 30; ++i) d[i] = 5;
          lit.build(l, 288), dist.build(d, 30);
        } else if (!dynamic(lit, dist)) {
          return false;
        }
        if (!block(out, lit, dist, limit)) return false;
      } else {
        return false;
      }
    }
    return true;
  }

 private:
  struct Huff {
    uint16_t count[16] = {}, sym[320] = {};
    void build(const uint8_t* len, int n) {
      uint16_t offs[16] = {};
      for (int i = 0; i < 16; ++i) count[i] = 0;
      for (int i = 0; i < n; ++i) count[len[i]]++;
      count[0] = 0;
      for (int i = 1; i < 16; ++i) offs[i] = static_cast<uint16_t>(offs[i - 1] + count[i - 1]);
      for (int i = 0; i < n; ++i)
        if (len[i]) sym[offs[len[i]]++] = static_cast<uint16_t>(i);
    }
  };
  uint32_t bits(int k) {
    while (bitCnt_ < k) {
      if (pos_ >= n_) {
        bad_ = true;
        return 0;
      }
      bitBuf_ |= static_cast<uint32_t>(p_[pos_++]) << bitCnt_;
      bitCnt_ += 8;
    }
    uint32_t v = bitBuf_ & ((1u << k) - 1);
    bitBuf_ >>= k;
    bitCnt_ -= k;
    return v;
  }
  int decode(const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= static_cast<int>(bits(1));
      if (bad_) return -1;
      int c = h.count[len];
      if (code - c < first) return h.sym[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    return -1;
  }
  bool dynamic(Huff& lit, Huff& dist) {
    int hlit = static_cast<int>(bits(5)) + 257, hdist = static_cast<int>(bits(5)) + 1, hclen = static_cast<int>(bits(4)) + 4;
    if (bad_ || hlit > 286 || hdist > 30) return false;
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t cl[19] = {};
    for (int i = 0; i < hclen; ++i) cl[order[i]] = static_cast<uint8_t>(bits(3));
    Huff clh;
    clh.build(cl, 19);
    uint8_t lens[320] = {};
    int i = 0;
    while (i < hlit + hdist) {
      int s = decode(clh);
      if (s < 0) return false;
      if (s < 16) {
        lens[i++] = static_cast<uint8_t>(s);
        continue;
      }
      int rep = 0;
      uint8_t v = 0;
      if (s == 16) {
        if (i == 0) return false;
        v = lens[i - 1], rep = 3 + static_cast<int>(bits(2));
      } else if (s == 17) {
        rep = 3 + static_cast<int>(bits(3));
      } else {
        rep = 11 + static_cast<int>(bits(7));
      }
      if (bad_ || i + rep > hlit + hdist) return false;
      while (rep--) lens[i++] = v;
    }
    lit.build(lens, hlit);
    dist.build(lens + hlit, hdist);
    return true;
  }
  bool block(std::vector<uint8_t>& out, const Huff& lit, const Huff& dist, size_t limit) {
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (out.size() < limit) {
      int s = decode(lit);
      if (s < 0) return false;
      if (s < 256) {
        out.push_back(static_cast<uint8_t>(s));
      } else if (s == 256) {
        return true;
      } else {
        s -= 257;
        if (s >= 29) return false;
        size_t len = lbase[s] + bits(lext[s]);
        int ds = decode(dist);
        if (ds < 0 || ds >= 30) return false;
        size_t d = dbase[ds] + bits(dext[ds]);
        if (bad_ || d > out.size()) return false;
        size_t from = out.size() - d;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
      }
    }
    return true;
  }
  const uint8_t* p_;
  size_t n_, pos_ = 0;
  uint32_t bitBuf_ = 0;
  int bitCnt_ = 0;
  bool bad_ = false;
};

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ------------------------------------------------------------------ Fruity DX10
// Control tags (= FL parameter indices).
enum Dx10Tag {
  DxAttack, DxDecay, DxRelease, DxM1Coarse, DxM1Fine, DxM1Init, DxM1Time, DxM1Sus, DxM1Rel, DxM1Vel,
  DxVibrato, DxWaveform, DxModThru, DxLfoRate, DxM2Coarse, DxM2Fine, DxM2Init, DxM2Time, DxM2Sus, DxM2Rel, DxM2Vel,
  DxOctave, DxTags
};

// Envelope rates are exponential time constants (seconds) of the knob position;
// Gridline's envelope segments take ~3 time constants to settle.
double dxAttack(double p) { return clampd(3.0 * std::exp(8.0 * p - 8.0), 0.001, 5.0); }
double dxDecay(double p) { return clampd(3.0 * std::exp(8.0 * p - 5.0), 0.005, 10.0); }
double dxRelease(double p) { return clampd(3.0 * std::exp(5.0 * p - 5.0), 0.005, 10.0); }
double dxModTime(double p) { return clampd(3.0 * std::exp(7.0 * p - 6.0), 0.005, 10.0); }
double dxModRel(double p) { return clampd(3.0 * std::exp(8.0 * p - 5.0), 0.005, 10.0); }
// Modulator frequency ratio: integer part from coarse (quadratic 0..40), fraction from fine.
double dxRatio(double coarse, double fine) {
  double ri = std::floor(40.1 * coarse * coarse), rf;
  if (fine < 0.5) {
    rf = 0.2 * fine * fine;
  } else {
    switch (static_cast<int>(8.9 * fine)) {
      case 4: rf = 0.25; break;
      case 5: rf = 1.0 / 3; break;
      case 6: rf = 0.5; break;
      case 7: rf = 2.0 / 3; break;
      default: rf = 0.75; break;
    }
  }
  return clampd(ri + rf, 0.5, 16.0);
}
double dxLfoHz(double p) { return clampd(25.0 * p * p, 0.1, 20.0); }

// One DX10 modulator envelope (init level → sustain level over "time") onto an operator.
void dxModulator(PluginMapping& m, int op, double init, double time, double sus, double rel, double vel) {
  using namespace pp;
  double peak = std::max(init, sus);
  set(m, fmParam(op, FmLevel), peak);
  set(m, fmParam(op, FmVel), vel);
  set(m, fmParam(op, FmRelease), dxModRel(rel));
  if (peak <= 0) return;
  if (init >= sus) {
    set(m, fmParam(op, FmAttack), 0.001);
    set(m, fmParam(op, FmDecay), dxModTime(time));
    set(m, fmParam(op, FmSustain), sus / peak);
    set(m, fmExt(op, FmInit), 0.0);
  } else {
    set(m, fmParam(op, FmAttack), clampd(dxModTime(time), 0.001, 5.0));
    set(m, fmParam(op, FmSustain), 1.0);
    set(m, fmExt(op, FmInit), init / peak);
  }
}

bool decodeDx10(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob b = stateBlob(evs);
  if (b.size() < 4 + 4 * DxTags) return false;
  auto k = [&](int tag) { return knob(b.i32(4 + 4 * static_cast<size_t>(tag))); };
  begin(m, "gl.fm");
  set(m, FmAlgorithm, 3);  // 4→3→1 + 2→1: op 1 carrier, ops 2 and 3 modulate it
  set(m, FmFeedback, 0);
  set(m, fmParam(3, FmLevel), 0);
  // Carrier: attack / decay (no sustain: the note fades at the decay rate) / release.
  set(m, fmParam(0, FmRatio), 1);
  set(m, fmParam(0, FmLevel), 1);
  set(m, fmParam(0, FmAttack), dxAttack(k(DxAttack)));
  bool hold = k(DxDecay) > 0.98;
  set(m, fmParam(0, FmDecay), hold ? 10.0 : dxDecay(k(DxDecay)));
  set(m, fmParam(0, FmSustain), hold ? 1.0 : 0.0);
  set(m, fmParam(0, FmRelease), dxRelease(k(DxRelease)));
  set(m, fmExt(0, FmWave), k(DxWaveform));
  // Modulators.
  set(m, fmParam(1, FmRatio), dxRatio(k(DxM1Coarse), k(DxM1Fine)));
  dxModulator(m, 1, k(DxM1Init), k(DxM1Time), k(DxM1Sus), k(DxM1Rel), k(DxM1Vel));
  set(m, fmExt(1, FmThru), k(DxModThru) * k(DxModThru));
  set(m, fmParam(2, FmRatio), dxRatio(k(DxM2Coarse), k(DxM2Fine)));
  dxModulator(m, 2, k(DxM2Init), k(DxM2Time), k(DxM2Sus), k(DxM2Rel), k(DxM2Vel));
  // Global.
  set(m, FmVibrato, 50.0 * k(DxVibrato) * k(DxVibrato));
  set(m, FmLfoRate, dxLfoHz(k(DxLfoRate)));
  set(m, FmOctave, clampd(b.i32(4 + 4 * DxOctave), -3, 3));
  m.exact = false;  // same structure, but the modulation index / envelope curves are approximations
  linkParams(m, {
                    {DxAttack, fmParam(0, FmAttack), dxAttack},
                    {DxRelease, fmParam(0, FmRelease), dxRelease},
                    {DxM1Coarse, fmParam(1, FmRatio), [](double n) { return dxRatio(n, 0); }},
                    {DxM1Init, fmParam(1, FmLevel), [](double n) { return n; }},
                    {DxM1Vel, fmParam(1, FmVel), [](double n) { return n; }},
                    {DxVibrato, FmVibrato, [](double n) { return 50.0 * n * n; }},
                    {DxWaveform, fmExt(0, FmWave), [](double n) { return n; }},
                    {DxModThru, fmExt(1, FmThru), [](double n) { return n * n; }},
                    {DxLfoRate, FmLfoRate, dxLfoHz},
                    {DxM2Coarse, fmParam(2, FmRatio), [](double n) { return dxRatio(n, 0); }},
                    {DxM2Init, fmParam(2, FmLevel), [](double n) { return n; }},
                    {DxM2Vel, fmParam(2, FmVel), [](double n) { return n; }},
                    {DxOctave, FmOctave, [](double n) { return std::round(n * 6.0 - 3.0); }},
                },
             // Knobs 0..65536, octave -3..3.
             {{DxAttack, 0, 65536}, {DxRelease, 0, 65536}, {DxM1Coarse, 0, 65536}, {DxM1Init, 0, 65536}, {DxM1Vel, 0, 65536},
              {DxVibrato, 0, 65536}, {DxWaveform, 0, 65536}, {DxModThru, 0, 65536}, {DxLfoRate, 0, 65536}, {DxM2Coarse, 0, 65536},
              {DxM2Init, 0, 65536}, {DxM2Vel, 0, 65536}, {DxOctave, -3, 3}});
  return true;
}

// ------------------------------------------------------------------ MiniSynth
// Parameter order of the 43 stored floats (= FL parameter indices).
enum MiniParam {
  MnVoicesMode, MnTranspose, MnSlide, MnMwDest, MnMwAmount, MnOscKind, MnOscMod, MnNoise,
  MnCutoff, MnPeak, MnKbd, MnVel, MnFAtk, MnFDec, MnFSus, MnFRel, MnFAmt,
  MnLfoShape, MnLfoDest, MnLfoRate, MnLfoAmt, MnAAtk, MnADec, MnASus, MnARel,
  MnOverdrive, MnDecimator, MnDist, MnDelay, MnDelayTime, MnDelayFb, MnDelayFilter, MnDelayMix,
  MnChorus, MnChorusRate, MnChorusDepth, MnChorusMix, MnPhaser, MnPhaserSpeed, MnPhaserWidth, MnPhaserFb, MnPhaserMix,
  MnPhaserFreq, MnCount
};

double mnTime(double p, double maxS) { return std::max(0.001, maxS * p * p * p); }
double mnCutoff(double p) { return 30.0 * std::pow(20000.0 / 30.0, clampd(p, 0, 1)); }
double mnLfoHz(double p) { return clampd(0.05 * std::pow(400.0, clampd(p, 0, 1)), 0.05, 20.0); }

bool decodeMiniSynth(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob b = stateBlob(evs);
  size_t at = b.find("0rev");
  if (at == std::string::npos || !b.has(at + 4, 4 * MnCount)) return false;
  auto f = [&](int i) { return clampd(b.f32(at + 4 + 4 * static_cast<size_t>(i)), 0.0, 1.0); };
  begin(m, "gl.synth");
  // Oscillator model (10 kinds in steps of 1/9) → shapes, plus a detuned / interval
  // second oscillator for the spread kinds. "OSC Mod" sets the spread / detune amount.
  int kind = static_cast<int>(std::lround(f(MnOscKind) * 9.0));
  double mod = f(MnOscMod);
  static const int kShape[10] = {2, 3, 6, 0, 1, 2, 2, 2, 2, 5};  // saw, square, pulse, sine, triangle, saw spreads, rounded saw
  set(m, SynOsc1Shape, kShape[std::clamp(kind, 0, 9)]);
  set(m, SynOsc1Level, 0.8);
  set(m, SynOsc2Level, 0);
  set(m, SynOsc3Level, 0);
  if (kind >= 5 && kind <= 8) {  // spread variants: a second saw, detuned or at an interval
    static const int kInterval[4] = {0, 0, 7, 12};
    set(m, SynOsc2Shape, 2);
    set(m, SynOsc2Level, 0.6);
    set(m, SynOsc2Coarse, kInterval[kind - 5]);
    set(m, SynOsc2Fine, 5.0 + 45.0 * mod);
    set(m, SynOsc1Fine, -(5.0 + 45.0 * mod) * 0.5);
  } else {
    set(m, SynOsc2Shape, kShape[std::clamp(kind, 0, 9)]);
    set(m, SynOsc2Level, 0.3 * mod);
    set(m, SynOsc2Coarse, 0);
    set(m, SynOsc2Fine, 12.0 * mod);
  }
  double transpose = std::round((f(MnTranspose) - 0.5) * 24.0);
  set(m, SynOsc1Coarse, transpose);
  set(m, SynOsc2Coarse, clampd(transpose + m.state.params[SynOsc2Coarse], -24, 24));
  set(m, SynNoise, f(MnNoise));
  // Filter + its envelope.
  set(m, SynFilterType, 1);  // 24 dB low pass
  set(m, SynCutoff, mnCutoff(f(MnCutoff)));
  set(m, SynResonance, f(MnPeak) * 0.9);
  set(m, SynKeyTrack, f(MnKbd));
  set(m, SynVelCutoff, f(MnVel));
  set(m, SynFilterEnv, (f(MnFAmt) - 0.5) * 2.0);
  set(m, SynFilterDecay, clampd(mnTime(f(MnFDec), 10.0), 0.005, 5.0));
  // Amplitude envelope.
  set(m, SynAttack, clampd(mnTime(f(MnAAtk), 10.0), 0.001, 5.0));
  set(m, SynDecay, clampd(mnTime(f(MnADec), 10.0), 0.005, 5.0));
  set(m, SynSustain, f(MnASus));
  set(m, SynRelease, clampd(mnTime(f(MnARel), 10.0), 0.005, 8.0));
  // Voices: poly / mono / lead (mono legato with slide).
  bool mono = f(MnVoicesMode) > 0.25;
  set(m, SynVoices, mono ? 1 : 16);
  set(m, SynGlide, mono ? clampd(mnTime(f(MnSlide), 2.0), 0.001, 2.0) : 0.001);
  // LFO: shape (sine / square / saw?), destination (pitch / filter / volume), bipolar amount.
  double amt = (f(MnLfoAmt) - 0.5) * 2.0;
  if (std::fabs(amt) > 0.01) {
    static const int kDest[3] = {1, 2, 3};   // pitch, cutoff, volume (Trio Synth LFO targets)
    static const int kShapeL[3] = {0, 2, 3};  // sine, square, saw
    set(m, SynLfoTarget, kDest[std::clamp(static_cast<int>(std::lround(f(MnLfoDest) * 2.0)), 0, 2)]);
    set(m, SynLfoShape, kShapeL[std::clamp(static_cast<int>(std::lround(f(MnLfoShape) * 2.0)), 0, 2)]);
    set(m, SynLfoRate, mnLfoHz(f(MnLfoRate)));
    set(m, SynLfoAmount, std::fabs(amt));
  } else {
    set(m, SynLfoTarget, 0);
  }
  set(m, SynVolume, 0.7);
  m.exact = false;  // different oscillator models; built-in FX (delay, chorus, phaser, distortion) not carried
  linkParams(m, {
                    {MnCutoff, SynCutoff, mnCutoff},
                    {MnPeak, SynResonance, [](double n) { return n * 0.9; }},
                    {MnFAmt, SynFilterEnv, [](double n) { return (n - 0.5) * 2.0; }},
                    {MnNoise, SynNoise, [](double n) { return n; }},
                    {MnASus, SynSustain, [](double n) { return n; }},
                    {MnARel, SynRelease, [](double n) { return clampd(mnTime(n, 10.0), 0.005, 8.0); }},
                    {MnAAtk, SynAttack, [](double n) { return clampd(mnTime(n, 10.0), 0.001, 5.0); }},
                    {MnLfoRate, SynLfoRate, mnLfoHz},
                });
  return true;
}

// ------------------------------------------------------------------ Ogun / Autogun
enum OgunTag {
  OgVolume, OgCoarse, OgFine, OgModX, OgModY, OgPreDecay, OgDecay, OgRelease, OgFullness, OgRandom,
  OgSeed1, OgSeed2, OgUniOrder, OgUniPan, OgUniVol, OgUniPitch, OgUniPhase, OgCutoff, OgReso,
  OgChOrder, OgChDepth, OgChSpeed, OgChDelay, OgChSpread, OgChCross, OgChWet,
  OgDelFb, OgDelTime, OgDelOfs, OgDelVol, OgDelMode,
  OgRevLowCut, OgRevHighCut, OgRevPre, OgRevRoom, OgRevDiff, OgRevDecay, OgRevDamp, OgRevColor, OgRevVol,
  OgEqLevel1, OgEqLevel2, OgEqLevel3, OgEqFreq1, OgEqFreq2, OgEqFreq3, OgEqBw1, OgEqBw2, OgEqBw3,
  OgTags
};

double ogCutoff(double n) { return 30.0 * std::pow(20000.0 / 30.0, clampd(0.5 + n * 0.5, 0, 1)); }  // n = tag/256

bool decodeOgun(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace packs::models;
  Blob b = stateBlob(evs);
  if (b.size() < 8) return false;
  std::vector<uint8_t> raw;
  raw.reserve(4 * OgTags + 64);
  Inflater inf(b.p + 4, b.size() - 4);
  if (!inf.run(raw, 4 * OgTags) || raw.size() < 4 * OgTags) return false;
  Blob s(raw.data(), raw.size());
  auto t = [&](int tag) { return s.i32(4 * static_cast<size_t>(tag)); };
  begin(m, "gl.additive");
  // Timbre: a bank of inharmonic, randomly levelled partials (seeded), shaped by fullness.
  set(m, AdSpectrumA, SpGlass);
  set(m, AdSpectrumB, SpRandom);
  set(m, AdMorph, clampd(t(OgRandom) / 1000.0, 0, 1) * 0.5);
  set(m, AdScatter, clampd(t(OgRandom) / 1000.0, 0, 1));
  set(m, AdSeed, 1 + ((t(OgSeed1) + 31 * t(OgSeed2)) % 64 + 64) % 64);
  set(m, AdPartials, 256);
  set(m, AdTilt, clampd(t(OgFullness) / 1000.0, -1, 1) * 12.0);
  set(m, AdFltType, AfLowPass);
  set(m, AdFltCutoff, ogCutoff(t(OgCutoff) / 256.0));
  set(m, AdFltReso, clampd(t(OgReso) / 256.0, 0, 1) * 0.8);
  // Amplitude: pre-decay (fast initial drop) → pluck; decay / release in ms.
  set(m, AdAttack, 0.001);
  set(m, AdDecay, clampd(t(OgDecay) / 1000.0, 0.01, 20.0));
  set(m, AdSustain, 0.0);
  set(m, AdRelease, clampd(t(OgRelease) / 1000.0, 0.005, 10.0));
  set(m, AdPluck, clampd(t(OgPreDecay) / 1000.0, 0, 1));
  set(m, AdPluckTime, clampd(t(OgDecay) / 4000.0, 0.05, 10.0));
  // Unison.
  set(m, AdUniOrder, clampd(t(OgUniOrder), 1, 5));
  set(m, AdUniDetune, clampd(t(OgUniPitch) / 256.0 * 100.0, 0, 100));
  set(m, AdUniSpread, clampd(t(OgUniPan) / 128.0, 0, 1));
  // Pitch, level.
  set(m, AdPitch, clampd(t(OgCoarse), -24, 24));
  set(m, AdFine, clampd(t(OgFine) * 2.0, -100, 100));
  set(m, AdVolume, clampd(t(OgVolume) / 256.0, 0, 1) * 0.8);
  // Built-in effects: chorus (order 0 = off), delay, reverb.
  set(m, AdFx + FxChorus, t(OgChOrder) > 0 ? clampd(t(OgChWet) / 128.0, 0, 1) : 0.0);
  set(m, AdFx + FxDelay, clampd(t(OgDelVol) / 128.0, 0, 1) * 0.5);
  set(m, AdFx + FxDelayTime, clampd(t(OgDelTime) / 192.0, 0.02, 1.5));  // 1/96-beat steps at ~120 BPM
  set(m, AdFx + FxDelayFeedback, clampd(t(OgDelFb) / 127.0, 0, 1) * 0.9);
  set(m, AdFx + FxReverb, clampd(t(OgRevVol) / 128.0, 0, 1));
  set(m, AdFx + FxRoomSize, clampd(t(OgRevRoom) / 100.0, 0, 1));
  m.exact = false;  // Gridline's partial generator, not Ogun's harmonic tables; mix EQ not carried
  linkParams(m, {
                    {OgVolume, AdVolume, [](double n) { return n * 0.8; }},
                    {OgCutoff, AdFltCutoff, ogCutoff},
                    {OgReso, AdFltReso, [](double n) { return n * 0.8; }},
                    {OgDecay, AdDecay, [](double n) { return clampd(n * 10.0, 0.01, 20.0); }},
                    {OgRelease, AdRelease, [](double n) { return clampd(n * 10.0, 0.005, 10.0); }},
                    {OgPreDecay, AdPluck, [](double n) { return n; }},
                    {OgRandom, AdScatter, [](double n) { return n; }},
                },
             {{OgVolume, 0, 256}, {OgCutoff, 0, 256}, {OgReso, 0, 256}, {OgDecay, 0, 10000}, {OgRelease, 0, 10000},
              {OgPreDecay, 0, 1000}, {OgRandom, 0, 1000}});
  return true;
}

// ------------------------------------------------------------------ BeepMap
enum BeepTag { BmRange, BmLength, BmScale, BmUseBlue, BmGrainy, BmLoop, BmWiden, BmTags };

double bmPixel(double v) { return clampd(0.01 * v / 2048.0, 0.0005, 1.0); }  // seconds per source pixel

// Downscales a 24/32-bit BMP into the 64 × 32 canvas (brightness of red/green, 0..15).
std::string bmpToCanvas(const Blob& bmp, int& srcW) {
  using namespace packs::models;
  srcW = 0;
  if (bmp.size() < 54 || bmp.u8(0) != 'B' || bmp.u8(1) != 'M') return {};
  uint32_t off = bmp.u32(10);
  int32_t w = bmp.i32(18), h = bmp.i32(22);
  int bpp = bmp.u16(28);
  bool topDown = h < 0;
  h = std::abs(h);
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (bpp != 24 && bpp != 32)) return {};
  size_t stride = (static_cast<size_t>(w) * static_cast<size_t>(bpp) / 8 + 3) & ~static_cast<size_t>(3);
  if (!bmp.has(off, stride * static_cast<size_t>(h))) return {};
  srcW = w;
  std::vector<uint8_t> px(static_cast<size_t>(kCanvasW * kCanvasH), 0);
  for (int cy = 0; cy < kCanvasH; ++cy)
    for (int cx = 0; cx < kCanvasW; ++cx) {
      int x0 = cx * w / kCanvasW, x1 = std::max(x0 + 1, (cx + 1) * w / kCanvasW);
      int y0 = cy * h / kCanvasH, y1 = std::max(y0 + 1, (cy + 1) * h / kCanvasH);
      double sum = 0;
      int n = 0;
      for (int y = y0; y < y1 && y < h; ++y) {
        size_t row = off + stride * static_cast<size_t>(topDown ? y : h - 1 - y);
        for (int x = x0; x < x1 && x < w; ++x) {
          size_t at = row + static_cast<size_t>(x) * static_cast<size_t>(bpp / 8);
          sum += std::max(bmp.u8(at + 2), bmp.u8(at + 1)) / 255.0;  // red = left, green = right
          ++n;
        }
      }
      px[static_cast<size_t>(cy * kCanvasW + cx)] = static_cast<uint8_t>(std::lround(15.0 * (n ? sum / n : 0)));
    }
  return encodeCanvas(px);
}

bool decodeBeepMap(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace packs::models;
  Blob b = stateBlob(evs);
  if (b.size() < 4 + 4 * BmTags) return false;
  auto t = [&](int tag) { return b.i32(4 + 4 * static_cast<size_t>(tag)); };
  begin(m, "gl.imagesynth");
  int srcW = 0;
  std::string canvas;
  size_t bm = b.find("BM", 4 + 4 * BmTags);
  if (bm != std::string::npos) canvas = bmpToCanvas(b.sub(bm, b.size() - bm), srcW);
  if (!canvas.empty()) {
    set(m, ImSource, SrcCanvas);
    m.state.data = makeImageData("", canvas);
  }
  // Frequency range in cents above the lowest row (keyboard tracked from C5).
  double low = 65.0, high = low * std::pow(2.0, clampd(t(BmRange), 24, 9600) / 1200.0);
  set(m, ImLow, low);
  set(m, ImHigh, clampd(high, 200, 20000));
  set(m, ImScale, std::clamp(t(BmScale), 0, 2));
  double pixel = bmPixel(t(BmLength)) * (srcW > 0 ? static_cast<double>(srcW) / kCanvasW : 1.0);
  set(m, ImPixel, clampd(pixel, 0.005, 1.0));
  set(m, ImUseBlue, t(BmUseBlue) ? 1 : 0);
  set(m, ImGrainy, t(BmGrainy) ? 1 : 0);
  set(m, ImLoop, t(BmLoop) ? 1 : 0);
  set(m, ImWiden, t(BmWiden) ? 1 : 0);
  set(m, ImKeyTrack, 1);
  m.exact = false;  // picture reduced to the 64 × 32 grey canvas
  linkParams(m, {{BmLength, ImPixel, [srcW](double n) { return clampd(bmPixel(n * 4096.0) * (srcW > 0 ? srcW / 64.0 : 1.0), 0.005, 1.0); }}},
             {{BmLength, 0, 4096}});
  return true;
}

}  // namespace

void registerSynthsA(std::map<std::string, PluginDecoder>& d) {
  add(d, "Fruity DX10", decodeDx10);
  add(d, "MiniSynth", decodeMiniSynth);
  add(d, "Ogun", decodeOgun);
  add(d, "Autogun", decodeOgun);
  add(d, "BeepMap", decodeBeepMap);
}

}  // namespace gl::flp::gen2
