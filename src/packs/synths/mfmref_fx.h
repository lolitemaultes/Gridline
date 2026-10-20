// Gridline — "synths" pack: Matrix FM reference model, effects and kernels.
//
// Effects section of the reference model (feedback delays, modulated
// multi-line chorus, figure-of-eight plate reverb, RBJ biquad EQ bands, 2x
// half-band decimators) and the per-block DSP kernels the voices use (gain
// ramps, state-variable / two-pole / ladder / allpass stages, table shaper,
// 2x oversampling), at the precision the model was measured with. Buffers
// are sized in setup() (UI thread); process calls never allocate. Internal to mfmref.
#pragma once
#include <cstdint>
#include <vector>

namespace gl::packs::synths::mfmref::fx {

// Gain ramp with a clamped slope: start / increment for n frames, cur becomes the end value.
void ramp(float target, float maxStep, float& inc, float& cur, float& start, int n);

struct Delay {
  bool on = false;
  int mode = 0;            // 0 straight, 1 swapped output, else ping-pong
  bool serial = false;     // output feeds the effects bus (next delay / reverb)
  int ofsL = 0, ofsR = 0;  // input write offsets (frames)
  float fbTarget = 0, fbCur = 0, vol = 1;
  std::vector<float> buf;  // frames * 2
  int frames = 0, rd = 0, wr = 0;
  void setup(const uint8_t raw[23], int sr, int blockFrames, double tempo, float lawVol);
  void process(const float* fx, bool writeInput, float* out, int n);
};

struct Biquad {
  int type = 0;
  double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
  void design(double A, double alpha, double s, double c);
  void process(float* buf, int n);  // stereo interleaved, in place
};

struct EqBand {
  int countdown = -1;  // frames left before a flat band switches off (-1 = never)
  Biquad bq;
  void setup(const int32_t raw[4], int sr, int tailFrames);
};

struct Chorus {
  struct Line {
    float d0 = 0, gL = 1, gR = 0, base = 0;
    int32_t phase = 0, phase0 = 0, inc = 0;
    int chan = 0;
    float D = 0, F = 0, E = 0;
  };
  bool on = false;
  std::vector<Line> lines;
  std::vector<float> buf;  // frames * 2
  int frames = 0, rd = 0, wr = 0, delayFrames = 0;
  void setup(int order, float depth, float speed, float delay, float spread, float cross, float vol, int sr, int blockFrames);
  void write(const float* fx, int n);
  void read(float* out, int n);
  void idle(int n);
};

struct Reverb {
  bool on = false;
  float wet = 0;
  float lp = 0, hpS1 = 0, hpS2 = 0, hpC = 0, g = 0, cAp1 = 0, cAp2 = 0, cIn = 0, lpC = 1, dampC = 1;
  float damp[2] = {}, shIn[2] = {}, shOut[2] = {}, shC = 0, shH = 0;
  std::vector<float> line[13];
  int len[13] = {}, pos[13] = {}, tapA[7] = {}, tapB[7] = {};
  void setup(const uint8_t raw[38], int sr, double tempo);
  void process(const float* mono, float* out, int n);
};

// Cascade of 2x decimators (two allpass branches of three sections).
struct Decimator {
  struct Stage {
    double s[12][2] = {};
  };
  std::vector<Stage> st;
  void init(int stages) { st.assign(static_cast<size_t>(stages > 0 ? stages : 0), Stage{}); }
  // Decimates interleaved stereo in place through stages hi .. lo (n frames in, n >> (hi - lo + 1) out).
  void run(float* buf, int& n, int hi, int lo);
};

// ----------------------------------------------------------------- kernels
// dst = src * g, g ramping in four lanes (16-frame unrolled), scalar tail on lane 0.
void rampMulLanes(const float* src, float* dst, float start, float inc, int n);
// dst += src * g, g ramping (extended accumulator).
void rampAdd(const float* src, float* dst, float start, float inc, int n);
// Gain stages: set dst / add to dst.
void mixSet(float gain, float inc, const float* src, float* dst, int n);
void mixAdd(float gain, float inc, const float* src, float* dst, int n);
// State-variable stage: low out in place (lo may alias in), band / high to bp / hp.
void svf(double* st, float f0, float q0, float finc, float qinc, bool ramp, const float* in, float* lo, float* bp, float* hp, int n);
// Damped two-pole: low (or high) out.
void twoPole(double* st, float f0, float q0, float finc, float qinc, bool high, const float* in, float* out, int n);
// Four one-pole ladder (z[0..3] stage states, z[4] feedback sum); drive 0 = linear.
void ladder(float* z, double p, float k, double drive, float gain, const float* in, float* out, int n);
// Mono biquad with coefficients of a Biquad; state x1 x2 y1 y2 at st[0], st[2], st[4], st[6].
void biquadMono(double* st, const Biquad& q, const float* in, float* out, int n);
// Allpass chain with feedback: 2 * count first-order sections.
void allpassChain(float* s, int count, float a, float fb, float& yState, float* buf, int n);
// Table shaper (tbl points at entry 0; tbl[-1] and tbl[L + 1] are guards).
void shaper(const float* in, float* out, int n, const int16_t* tbl, int L, float inGain, bool bipolar, bool interp);
// 2x oversampling of a buffer (sample doubling up, pair average down).
void upsample2(float* buf, int n);
void downsample2(float* buf, int n);
// Delphi-style power (integral exponents by repeated multiplication).
double powerD(double b, double e);
float powerF(float b, float e);

}  // namespace gl::packs::synths::mfmref::fx
