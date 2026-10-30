// Gridline - J-191: inference for the Hybrid Transformer Demucs network (Rouard, Massa, Defossez,
// "Hybrid Transformers for Music Source Separation", ICASSP 2023; reference code MIT,
// github.com/facebookresearch/demucs). Gridline's own implementation of the published
// architecture: a spectrogram U-Net and a waveform U-Net joined by a cross-domain transformer.
// Weights are read at run time from the official checkpoint the user downloaded; none ship with
// Gridline. Configurations outside what the published models use are refused with a message.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "StemOps.h"
#include "TorchFile.h"

namespace gl::stems {

struct DemucsConfig {
  std::vector<std::string> sources;
  int audioChannels = 2;
  int samplerate = 44100;
  double segment = 7.8;  // seconds (the training length the model always runs at)
  int channels = 48, growth = 2, nfft = 4096, depth = 4;
  int kernelSize = 8, stride = 4, context = 1, contextEnc = 0;
  int dconvMode = 1, dconvDepth = 2, dconvComp = 8;
  double freqEmb = 0.2, embScale = 10.0;
  int bottomChannels = 0;
  int tLayers = 5, tHeads = 8;
  double tHiddenScale = 4.0, tMaxPeriod = 10000.0, tWeightPosEmbed = 1.0;
  bool tCrossFirst = false;
  int segmentLength() const { return int(segment * double(samplerate)); }
};

// Reads the configuration from a checkpoint's "kwargs"; false + message when unsupported.
bool demucsConfigFrom(const PyRef& kwargs, DemucsConfig& cfg, std::string* err);

class HTDemucs {
 public:
  // Loads a checkpoint (a dict with "klass" HTDemucs, "kwargs", "state").
  bool load(const std::string& path, std::string* err);
  // Builds the model from an already parsed configuration and state (tests).
  bool init(const DemucsConfig& cfg, std::map<std::string, Tensor> state, std::string* err);
  const DemucsConfig& config() const { return cfg_; }

  // One segment: `mix` holds audioChannels planes of `length` samples (length <=
  // config().segmentLength(); shorter input is zero-padded like the reference). Returns
  // sources x audioChannels planes of `length` samples, source-major.
  std::vector<float> forward(Pool& pool, const float* mix, int length) const;

 private:
  struct Conv {
    std::vector<float> w, b;
    int cout = 0, cin = 0, kh = 1, kw = 1;
  };
  struct Norm {
    std::vector<float> w, b;
  };
  struct DConvLayer {
    Conv c1, c2;
    Norm n1, n2;
    std::vector<float> scale;
    int dilation = 1;
  };
  struct EncLayer {
    Conv conv, rewrite;
    std::vector<DConvLayer> dconv;
    bool freq = true;
    int stride = 4, pad = 2, ctx = 0;
  };
  struct DecLayer {
    Conv convTr, rewrite;  // convTr: cin x cout x k (PyTorch layout)
    std::vector<DConvLayer> dconv;
    bool freq = true, last = false;
    int stride = 4, pad = 2, ctx = 1;
  };
  struct Attn {
    std::vector<float> inW, inB, outW, outB;
  };
  struct TLayer {
    bool cross = false;
    Attn attn;
    Conv lin1, lin2;  // weights (out, in)
    Norm norm1, norm2, norm3, normOut;
    std::vector<float> gamma1, gamma2;
  };

  bool take(std::map<std::string, Tensor>& st, const std::string& name, std::vector<int64_t> shape,
            std::vector<float>& out, std::string* err) const;
  bool takeConv(std::map<std::string, Tensor>& st, const std::string& name, Conv& c, std::string* err) const;
  bool takeNorm(std::map<std::string, Tensor>& st, const std::string& name, int n, Norm& out, std::string* err) const;
  bool takeDConv(std::map<std::string, Tensor>& st, const std::string& prefix, int channels,
                 std::vector<DConvLayer>& out, std::string* err) const;

  void runEncoder(Pool& pool, const EncLayer& e, std::vector<float>& x, int& c, int& h, int& w) const;
  void runDecoder(Pool& pool, const DecLayer& d, std::vector<float>& x, const std::vector<float>& skip, int& c, int& h,
                  int& w, int length) const;
  void runDConv(Pool& pool, const std::vector<DConvLayer>& layers, std::vector<float>& x, int c, int h, int w) const;
  void runTransformer(Pool& pool, std::vector<float>& x, int fr, int t1, std::vector<float>& xt, int t2) const;
  void runTLayer(Pool& pool, const TLayer& l, std::vector<float>& x, int n, const std::vector<float>* kv, int m) const;

  DemucsConfig cfg_;
  std::vector<EncLayer> enc_, tenc_;
  std::vector<DecLayer> dec_, tdec_;
  std::vector<float> freqEmb_;  // freqs x channels, already scaled
  int freqEmbFreqs_ = 0;
  Conv up_, down_, upT_, downT_;
  Norm normIn_, normInT_;
  std::vector<TLayer> layers_, layersT_;
  int tDim_ = 0;
};

// ---- building blocks shared with the tests ----
// 2-D convolution on a (cin, h, w) plane stack; weights (cout, cin, kh, kw); stride / padding /
// dilation per axis. Output (cout, ho, wo).
std::vector<float> conv2d(Pool& pool, const float* x, int cin, int h, int w, const float* weight, const float* bias,
                          int cout, int kh, int kw, int sh, int sw, int ph, int pw, int dh, int dw, int& ho, int& wo);
// Transposed convolution along one axis (axisH: along h, else along w), kernel k, stride s, no
// padding; weights (cin, cout, k). Output (cout, h', w').
std::vector<float> convTranspose(Pool& pool, const float* x, int cin, int h, int w, const float* weight,
                                 const float* bias, int cout, int k, int s, bool axisH, int& ho, int& wo);
// Spectrogram as the reference computes it (`_spec`): channels x (nfft/2 bins) x frames complex.
std::vector<std::complex<float>> demucsSpec(const float* x, int channels, int length, int nfft, int& frames);
// Inverse (`_ispec`): z = channels x (nfft/2) x frames; returns channels x length.
std::vector<float> demucsISpec(const std::complex<float>* z, int channels, int nfft, int frames, int length);

}  // namespace gl::stems
