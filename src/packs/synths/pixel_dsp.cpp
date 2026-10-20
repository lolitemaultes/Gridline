// Gridline — "synths" pack, J-33 group "pixel": Pixel Tone (gl.pixeltone)
// runtime, following the reference instrument:
//   column length n = round(256·(33^(L/4096) − 1)) + 64 samples at 44.1 kHz
//   (scaled to the sample rate); levels ramp linearly across each column from
//   the previous column's values; row levels pixel / (255·√rows), red left,
//   green right; blue (optional) sets a row's pitch weight 256 − B;
//   frequencies (integer maths, pitch P in cents from 523.2511 Hz at the
//   reference C5): log rows span R cents around the note, linear rows span
//   1.5 R Hz, harmonic rows are multiples of the note two octaves down with a
//   soft ceiling 1102.5·(11^(R/9600) − 1) Hz; sine partials from a 16384-point
//   table without interpolation; grainy multiplies by one sine cycle per
//   column; widen gives the right channel its own random phase.
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "../../engine/ChannelIns.h"
#include "../Packs.h"
#include "pixel.h"

namespace gl::packs::synths {

std::string pixelToneDefaultData();  // pixel_info.cpp

namespace {

using namespace pixel;

constexpr int kVoices = 32;
constexpr double kC5 = 523.2511;

struct PixelAux final : AuxData {
  Picture pic;
};

const float* sineTable() {
  static const std::vector<float> t = [] {
    std::vector<float> v(16384);
    for (int i = 0; i < 16384; ++i) v[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * M_PI * i / 16384.0));
    return v;
  }();
  return t.data();
}

class PixelToneGen final : public Generator {
 public:
  PixelToneGen() { sine_ = sineTable(); }
  bool active() const override {
    for (const auto& v : v_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Row {
    uint32_t th = 0, phR = 0, inc = 0;
    float cL = 0, cR = 0, dL = 0, dR = 0, aL = 0, aR = 0;
    int w = 256;
  };
  struct Voice {
    bool on = false, released = false;
    uint32_t id = 0;
    double pitch = 60;
    float vel = 1, fade = 1;
    int column = -1, left = 0;
    uint32_t gamma = 0;
    uint64_t age = 0;
    Row row[kMaxRows];
    InsVoice ins;  // channel instrument envelopes / filter (J-33)
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    aux_ = dynamic_cast<const PixelAux*>(aux.custom.get());
    ins_ = aux.ins;
    tempo_ = ctx.tempo, ppq_ = ctx.ppq, tick0_ = ctx.tick, blockPos_ = 0;
    auto p = [&](int i, double d) { return i < static_cast<int>(s.params.size()) ? static_cast<double>(s.params[static_cast<size_t>(i)]) : d; };
    range_ = static_cast<int>(std::lround(std::clamp(p(PxRange, 7200), 24.0, 9600.0)));
    spp_ = std::max(1, static_cast<int>(sr_ * columnSamples44(static_cast<int>(std::lround(std::clamp(p(PxLength, 2048), 0.0, 4096.0)))) / 44100.0));
    scale_ = static_cast<int>(std::lround(std::clamp(p(PxScale, 0), 0.0, 2.0)));
    blue_ = p(PxUseBlue, 0) >= 0.5, grainy_ = p(PxGrainy, 0) >= 0.5, loop_ = p(PxLoop, 1) >= 0.5, widen_ = p(PxWiden, 0) >= 0.5;
  }

  uint32_t rnd() { return seed_ = seed_ * 1664525u + 1013904223u; }

  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    Voice* v = nullptr;
    for (auto& c : v_)
      if (!c.on) {
        v = &c;
        break;
      }
    if (!v) {
      for (auto& c : v_)
        if (c.released && (!v || c.age < v->age)) v = &c;
      if (!v) {
        v = &v_[0];
        for (auto& c : v_)
          if (c.age < v->age) v = &c;
      }
    }
    v->on = true, v->released = false, v->id = id, v->pitch = pitch, v->vel = velocity, v->fade = 1;
    v->column = -1, v->left = 0, v->gamma = 0, v->age = ++clock_;
    v->ins.start(ins_.get(), sr_, tempo_, ppq_, 0, 0, tick0_ + blockPos_ * tempo_ * ppq_ / (60.0 * sr_));
    for (auto& r : v->row) {
      r.th = (rnd() >> 8) * 256u;
      r.phR = widen_ ? (rnd() >> 8) * 256u : 0u;
      r.cL = r.cR = r.dL = r.dR = r.aL = r.aR = 0;
    }
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : v_)
      if (v.on && v.id == id && !v.released) v.released = true, v.ins.release();
  }
  void onAllOff() override {
    for (auto& v : v_)
      if (!v.released) v.released = true, v.ins.release();
  }
  void onKill() override {
    for (auto& v : v_) v.on = false;
  }

  void columnStart(Voice& v) {
    const Picture* pic = aux_ ? &aux_->pic : nullptr;
    const int N = pic ? std::min(pic->h, kMaxRows) : 0, W = pic ? pic->w : 0;
    ++v.column;
    bool silent = W == 0;
    if (v.column >= W) {
      if (loop_ && W > 0) v.column = 0;
      else silent = true;
    }
    const float k = N > 0 ? static_cast<float>(1.0 / (255.0 * std::sqrt(static_cast<double>(N)))) : 0.f;
    const int M = std::max(1, N - 1);
    const int64_t Wd = 512ll * M;
    const int P = static_cast<int>(std::lround((v.pitch - 60.0) * 100.0));
    const int Q = static_cast<int>(sr_ / 4);
    int f0 = 0, fmax = Q;
    if (scale_ != ScLog) {
      const double cents = scale_ == ScLinear ? P : P - 2400;
      f0 = std::min(Q, static_cast<int>(std::lround(kC5 * std::exp2(cents / 1200.0))));
      fmax = f0 > 0 ? (Q / f0) * f0 : Q;
    }
    const int Rl = (3 * range_) / 2;
    const int Fc = static_cast<int>(1102.5 * (std::pow(11.0, range_ / 9600.0) - 1.0));
    const float inv = 1.f / static_cast<float>(spp_);
    for (int i = 0; i < N; ++i) {
      Row& r = v.row[i];
      if (silent) r.aL = r.aR = 0;
      else {
        const uint8_t* px = &pic->rgb[(static_cast<size_t>(i) * static_cast<size_t>(W) + static_cast<size_t>(v.column)) * 3];
        r.aL = px[0] * k, r.aR = px[1] * k;
        r.w = blue_ ? 256 - px[2] : 256;
      }
      if (scale_ == ScLog) {
        int c = P - static_cast<int>((static_cast<int64_t>(2 * i - M) * (static_cast<int64_t>(r.w) * range_)) / Wd);
        c = std::clamp(c, -4800, 4800);
        r.inc = static_cast<uint32_t>(std::llround(4294967296.0 * kC5 / sr_ * std::exp2(c / 1200.0)));
      } else {
        int64_t f;
        if (scale_ == ScLinear) f = f0 - (static_cast<int64_t>(2 * i - M) * (static_cast<int64_t>(r.w) * Rl)) / Wd;
        else f = static_cast<int64_t>(N - i) * f0 * (9 - r.w / 32);
        f = std::clamp<int64_t>(f, 40, std::max(40, fmax));
        r.inc = static_cast<uint32_t>((static_cast<uint64_t>(f) << 32) / static_cast<uint64_t>(sr_));
        if (scale_ == ScHarmonics && f > Fc) {
          const float g = std::max(0.f, 1.f - static_cast<float>(f - Fc) / 480.f);
          r.aL *= g, r.aR *= g;
        }
      }
      r.dL = (r.aL - r.cL) * inv;
      r.dR = (r.aR - r.cR) * inv;
    }
    v.left = spp_;
  }

  void render(float* L, float* R, int n) override {
    blockPos_ += n;
    const bool volEnv = ins_ && ins_->volumeEnvelope();
    const Picture* pic = aux_ ? &aux_->pic : nullptr;
    const int N = pic ? std::min(pic->h, kMaxRows) : 0;
    const int W = pic ? pic->w : 0;
    const uint32_t gInc = static_cast<uint32_t>(4294967296.0 / spp_);
    const float fadeStep = static_cast<float>(1.0 / (0.005 * sr_));
    for (auto& v : v_) {
      if (!v.on) continue;
      int i = 0;
      while (i < n && v.on) {
        if (v.left <= 0) {
          if (!loop_ && v.column >= W) {  // one silent column after the last: done
            v.on = false;
            break;
          }
          columnStart(v);
        }
        const int len = std::min(n - i, v.left);
        for (int q = 0; q < len; ++q) {
          float sl = 0, sr = 0;
          for (int k = 0; k < N; ++k) {
            Row& r = v.row[k];
            sl += sine_[r.th >> 18] * r.cL;
            sr += sine_[(r.th + r.phR) >> 18] * r.cR;
            r.cL += r.dL, r.cR += r.dR;
            r.th += r.inc;
          }
          if (grainy_) {
            const float g = sine_[v.gamma >> 18];
            sl *= g, sr *= g;
            v.gamma += gInc;
          }
          float gain = v.vel;
          if (v.released && volEnv) {  // the channel volume envelope ends the voice
            if (v.ins.done()) {
              v.on = false;
              break;
            }
          } else if (v.released) {
            v.fade -= fadeStep;
            if (v.fade <= 0) {
              v.on = false;
              break;
            }
            gain *= v.fade;
          }
          sl *= gain, sr *= gain;
          if (v.ins.active()) v.ins.process(sl, sr);
          L[i + q] += sl;
          R[i + q] += sr;
        }
        v.left -= len;
        i += len;
      }
    }
  }

 private:
  double sr_ = 44100;
  const PixelAux* aux_ = nullptr;
  const float* sine_ = nullptr;
  int range_ = 7200, spp_ = 1279, scale_ = 0;
  bool blue_ = false, grainy_ = false, loop_ = true, widen_ = false;
  uint32_t seed_ = 0x2545F491u;
  uint64_t clock_ = 0;
  Voice v_[kVoices];
  std::shared_ptr<const InsSettings> ins_;
  double tempo_ = 120, tick0_ = 0;
  int ppq_ = 96;
  int64_t blockPos_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makePixelTone() { return std::make_unique<PixelToneGen>(); }

bool preparePixelAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != "gl.pixeltone") return false;
  aux = ChannelAux{};
  auto a = std::make_shared<PixelAux>();
  if (!parsePicture(state.data, a->pic)) parsePicture(pixelToneDefaultData(), a->pic);
  aux.custom = a;
  return true;
}

}  // namespace gl::packs::synths
