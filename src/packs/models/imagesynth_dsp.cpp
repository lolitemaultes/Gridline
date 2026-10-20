// Gridline — Image Synth: reads a picture as a spectrogram. Every pixel row is
// a sine at a fixed frequency (log / linear / harmonic scale between Low and
// High); columns are played left to right at Pixel time each. Red drives the
// left channel, green the right, blue (optional) both.
#include <array>
#include <cmath>
#include "models_rt.h"

namespace gl::packs::models {

namespace {

using namespace dsp;
constexpr int kCtl = 32;
constexpr int kVoices = 12;

struct ImgView {
  const float* r = nullptr;
  const float* g = nullptr;
  const float* b = nullptr;
  int w = 0, h = 0;
  const void* key = nullptr;
};

class ImageGen final : public Generator {
 public:
  ImageGen() : voices_(new Voice[kVoices]) {
    (void)sineTable();
    for (int i = 0; i < kImgPatterns; ++i) patterns_[static_cast<size_t>(i)] = builtinImage(i);
  }
  bool active() const override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, held = false, ending = false;
    uint32_t id = 0;
    double pitch = 60, pos = 0;
    float vel = 1;
    uint64_t age = 0;
    Adsr env;
    uint32_t phL[kImgMaxRows], phR[kImgMaxRows], incL[kImgMaxRows], incR[kImgMaxRows];
    float aL[kImgMaxRows], aR[kImgMaxRows], dL[kImgMaxRows], dR[kImgMaxRows];
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    int src = prmi(s, ImSource, 0);
    ImgView v;
    auto fromAux = [&](size_t i) {
      const Sample* a = aux.sample(i).get();
      const Sample* b = aux.sample(i + 1).get();
      if (!a || !b || a->sampleRate < 1) return false;
      int w = static_cast<int>(a->sampleRate);
      int h = static_cast<int>(a->frames() / static_cast<size_t>(w));
      if (w < 1 || h < 1) return false;
      v = ImgView{a->left.data(), a->right.data(), b->left.data(), w, h, a};
      return true;
    };
    bool ok = (src == SrcFile && fromAux(0)) || (src == SrcCanvas && fromAux(2));
    if (!ok) {
      const Image& im = *patterns_[static_cast<size_t>(std::clamp(prmi(s, ImPattern, 0), 0, kImgPatterns - 1))];
      v = ImgView{im.r.data(), im.g.data(), im.b.data(), im.w, im.h, &im};
    }
    v.h = std::min(v.h, kImgMaxRows);
    bool changed = v.key != img_.key || v.w != img_.w || v.h != img_.h;
    img_ = v;  // always refresh: the aux data may have been rebuilt at the same address
    if (changed) {
      double mx = 0;  // loudest column (sum of squares) sets the normalisation
      for (int x = 0; x < img_.w; ++x) {
        double e = 0;
        for (int y = 0; y < img_.h; ++y) {
          size_t i = static_cast<size_t>(y * img_.w + x);
          e += img_.r[i] * img_.r[i] + img_.g[i] * img_.g[i] + img_.b[i] * img_.b[i];
        }
        mx = std::max(mx, e);
      }
      norm_ = static_cast<float>(0.45 / std::max(0.7, std::sqrt(mx)));
    }
    low_ = prmf(s, ImLow, 110);
    high_ = std::max(low_ * 1.01, static_cast<double>(prmf(s, ImHigh, 7000)));
    scale_ = prmi(s, ImScale, 0);
    pixel_ = std::max(0.002, static_cast<double>(prmf(s, ImPixel, 0.06f)));
    start_ = prmf(s, ImStart, 0);
    loop_ = prmf(s, ImLoop, 0) >= 0.5f;
    reverse_ = prmf(s, ImReverse, 0) >= 0.5f;
    grainy_ = prmf(s, ImGrainy, 0) >= 0.5f;
    widen_ = prmf(s, ImWiden, 0) >= 0.5f;
    blue_ = prmf(s, ImUseBlue, 0) >= 0.5f;
    keyTrack_ = prmf(s, ImKeyTrack, 1) >= 0.5f;
    gamma_ = prmf(s, ImGamma, 1);
    maxVoices_ = std::clamp(prmi(s, ImVoices, 6), 1, kVoices);
    vol_ = prmf(s, ImVolume, 0.8f);
    for (int i = 0; i < kVoices; ++i) voices_[i].env.set(sr_, prmf(s, ImAttack, 0.01f), 0.1, 1.0, prmf(s, ImRelease, 0.3f));
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    Voice* pick = nullptr;
    for (int i = 0; i < maxVoices_; ++i)
      if (!voices_[i].on) {
        pick = &voices_[i];
        break;
      }
    if (!pick)
      for (int i = 0; i < maxVoices_; ++i)
        if (!pick || voices_[i].age < pick->age) pick = &voices_[i];
    Voice& v = *pick;
    v.on = v.held = true;
    v.ending = false;
    v.id = id;
    v.pitch = pitch;
    v.vel = vel;
    v.age = ++clock_;
    v.pos = (reverse_ ? 1.0 - start_ : start_) * img_.w;
    if (reverse_) v.pos = std::max(0.0, v.pos - 1e-6);
    v.env.level = 0;
    v.env.gate(true);
    for (int r = 0; r < kImgMaxRows; ++r) {
      uint32_t h = static_cast<uint32_t>(r * 2654435761u + 12345u);
      v.phL[r] = h;
      v.phR[r] = h * 747796405u + 2891336453u;
      v.aL[r] = v.aR[r] = v.dL[r] = v.dR[r] = 0;
    }
    voiceControl(v, false);
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on && voices_[i].held && voices_[i].id == id) {
        voices_[i].held = false;
        voices_[i].env.gate(false);
      }
  }
  void onAllOff() override {
    for (int i = 0; i < kVoices; ++i) {
      voices_[i].held = false;
      voices_[i].env.gate(false);
    }
  }
  void onKill() override {
    for (int i = 0; i < kVoices; ++i) voices_[i].on = false;
  }

  float pix(const float* c, int x, int y) const { return c[static_cast<size_t>(y * img_.w + x)]; }

  void voiceControl(Voice& v, bool advance) {
    const int W = img_.w, H = img_.h;
    if (advance) {
      double step = kCtl / (pixel_ * sr_);
      v.pos += reverse_ ? -step : step;
      if (v.pos >= W || v.pos < 0) {
        if (loop_) {
          v.pos = std::fmod(std::fmod(v.pos, W) + W, static_cast<double>(W));
        } else if (!v.ending) {
          v.ending = true;
          v.held = false;
          v.env.gate(false);
          v.pos = std::clamp(v.pos, 0.0, W - 1e-6);
        } else {
          v.pos = std::clamp(v.pos, 0.0, W - 1e-6);
        }
      }
    }
    int c0 = std::clamp(static_cast<int>(v.pos), 0, W - 1);
    int c1 = loop_ ? (c0 + (reverse_ ? W - 1 : 1)) % W : std::clamp(c0 + (reverse_ ? -1 : 1), 0, W - 1);
    float fr = grainy_ ? 0.0f : static_cast<float>(reverse_ ? std::ceil(v.pos) - v.pos : v.pos - std::floor(v.pos));
    double ratio = keyTrack_ ? std::pow(2.0, (v.pitch - 60.0) / 12.0) : 1.0;
    float g = norm_ * vol_ * (0.3f + 0.7f * v.vel);
    for (int y = 0; y < H; ++y) {
      double hz = imageRowHz(y, H, scale_, low_, high_, ratio);
      v.incL[y] = phaseInc(hz, sr_);
      v.incR[y] = widen_ ? phaseInc(hz * 1.0035, sr_) : v.incL[y];
      float r = pix(img_.r, c0, y) + (pix(img_.r, c1, y) - pix(img_.r, c0, y)) * fr;
      float gr = pix(img_.g, c0, y) + (pix(img_.g, c1, y) - pix(img_.g, c0, y)) * fr;
      float b = blue_ ? pix(img_.b, c0, y) + (pix(img_.b, c1, y) - pix(img_.b, c0, y)) * fr : 0.0f;
      float tl = r + b, tr = gr + b;
      if (gamma_ != 1.0f) {
        tl = std::pow(std::min(tl, 2.0f), gamma_);
        tr = std::pow(std::min(tr, 2.0f), gamma_);
      }
      if (grainy_) {
        float n = 0.25f + 1.5f * (0.5f + 0.5f * noise_.next());
        tl *= n;
        tr *= n;
      }
      if (v.incL[y] == 0) tl = tr = 0;
      v.dL[y] = (tl * g - v.aL[y]) / kCtl;
      v.dR[y] = (tr * g - v.aR[y]) / kCtl;
    }
  }

  void render(float* L, float* R, int n) override {
    const SineTable& st = sineTable();
    int pos = 0;
    while (pos < n) {
      if (ctr_ == 0) {
        for (int i = 0; i < kVoices; ++i)
          if (voices_[i].on) voiceControl(voices_[i], true);
        ctr_ = kCtl;
      }
      int len = std::min(ctr_, n - pos);
      const bool end = len == ctr_;
      for (int i = 0; i < kVoices; ++i) {
        Voice& v = voices_[i];
        if (!v.on) continue;
        float bl[kCtl] = {}, br[kCtl] = {};
        for (int y = 0; y < img_.h; ++y) {
          if (v.aL[y] == 0 && v.dL[y] == 0 && v.aR[y] == 0 && v.dR[y] == 0) {
            v.phL[y] += v.incL[y] * static_cast<uint32_t>(len);
            v.phR[y] += v.incR[y] * static_cast<uint32_t>(len);
            continue;
          }
          for (int j = 0; j < len; ++j) {
            if (widen_) {
              br[j] += st.at(v.phR[y]) * v.aR[y];
              v.phR[y] += v.incR[y];
              bl[j] += st.at(v.phL[y]) * v.aL[y];
            } else {
              float sv = st.at(v.phL[y]);
              bl[j] += sv * v.aL[y];
              br[j] += sv * v.aR[y];
            }
            v.phL[y] += v.incL[y];
            v.aL[y] += v.dL[y];
            v.aR[y] += v.dR[y];
          }
          // J-30: flushes and the voice end happen at the end of a control period,
          // not at the end of each host block.
          if (end && std::fabs(v.aL[y]) < 1e-9f) v.aL[y] = 0;
          if (end && std::fabs(v.aR[y]) < 1e-9f) v.aR[y] = 0;
        }
        for (int j = 0; j < len; ++j) {
          float e = static_cast<float>(v.env.next());
          L[pos + j] += bl[j] * e;
          R[pos + j] += br[j] * e;
        }
        if (end && !v.env.active()) v.on = false;
      }
      ctr_ -= len;
      pos += len;
    }
  }

 private:
  std::unique_ptr<Voice[]> voices_;
  std::array<ImagePtr, kImgPatterns> patterns_;
  ImgView img_;
  double sr_ = 44100, low_ = 110, high_ = 7000, pixel_ = 0.06, start_ = 0;
  int scale_ = 0, maxVoices_ = 6, ctr_ = 0;
  bool loop_ = false, reverse_ = false, grainy_ = false, widen_ = false, blue_ = false, keyTrack_ = true;
  float gamma_ = 1, vol_ = 0.8f, norm_ = 0.1f;
  Noise noise_;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeImageSynth() { return std::make_unique<ImageGen>(); }

}  // namespace gl::packs::models
