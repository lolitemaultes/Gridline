// Gridline - J-191: the tiny stem model the stem tests use (shared by test_stems.cpp and the
// stem part tests): a model of the real architecture built from a fixed pseudo-random sequence and
// written in the PyTorch checkpoint format. No real model weights anywhere.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "engine/stems/TorchFile.h"

namespace gl::stemtiny {

using namespace gl::stems;

// ---------------------------------------------------------------- tiny model

struct Lcg {
  uint64_t s;
  explicit Lcg(uint64_t seed) : s(seed) {}
  float next() {  // uniform in [-1, 1)
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return float(double(s >> 40) / double(1ULL << 24)) * 2.f - 1.f;
  }
};

struct TinyBuilder {
  Lcg rng{1234};
  std::vector<std::pair<std::string, Tensor>> state;
  void add(const std::string& name, std::vector<int64_t> shape, float scale, float offset = 0.f) {
    Tensor t;
    t.shape = std::move(shape);
    t.data.resize(size_t(t.numel()));
    for (float& v : t.data) v = offset + scale * rng.next();
    state.emplace_back(name, std::move(t));
  }
  void conv(const std::string& name, std::vector<int64_t> shape, bool transposed = false) {
    int64_t fanIn = 1;
    for (size_t i = 1; i < shape.size(); ++i) fanIn *= shape[i];
    if (transposed) fanIn = shape[0] * (shape.size() > 2 ? shape[2] : 1);
    const float sc = 1.f / std::sqrt(float(fanIn));
    const int64_t nb = transposed ? shape[1] : shape[0];
    add(name + ".weight", shape, sc);
    add(name + ".bias", {nb}, sc);
  }
  void norm(const std::string& name, int64_t n) {
    add(name + ".weight", {n}, 0.1f, 1.f);
    add(name + ".bias", {n}, 0.05f);
  }
  void dconv(const std::string& p, int64_t c, int64_t comp, bool freq) {
    (void)freq;
    const int64_t hidden = c / comp;
    for (int d = 0; d < 2; ++d) {
      const std::string q = p + ".layers." + std::to_string(d);
      conv(q + ".0", {hidden, c, 3});
      norm(q + ".1", hidden);
      conv(q + ".3", {2 * c, hidden, 1});
      norm(q + ".4", 2 * c);
      add(q + ".6.scale", {c}, 0.05f, 0.1f);
    }
  }
  void attn(const std::string& p, int64_t d) {
    add(p + ".in_proj_weight", {3 * d, d}, 1.f / std::sqrt(float(d)));
    add(p + ".in_proj_bias", {3 * d}, 0.05f);
    conv(p + ".out_proj", {d, d});
  }
  void tlayer(const std::string& p, int64_t d, int64_t hidden, bool cross) {
    attn(p + (cross ? ".cross_attn" : ".self_attn"), d);
    conv(p + ".linear1", {hidden, d});
    conv(p + ".linear2", {d, hidden});
    norm(p + ".norm1", d);
    norm(p + ".norm2", d);
    if (cross) norm(p + ".norm3", d);
    norm(p + ".norm_out", d);
    add(p + ".gamma_1.scale", {d}, 0.05f, 0.2f);
    add(p + ".gamma_2.scale", {d}, 0.05f, 0.2f);
  }
};

// channels 4, depth 2, nfft 256, segment 0.5 s, bottom 16, 2 transformer layers (self, cross).
inline bool writeTiny(const std::string& path, std::string* err) {
  TinyBuilder b;
  // encoders
  b.conv("encoder.0.conv", {4, 4, 8, 1});
  b.conv("encoder.0.rewrite", {8, 4, 1, 1});
  b.dconv("encoder.0.dconv", 4, 2, true);
  b.conv("encoder.1.conv", {8, 4, 8, 1});
  b.conv("encoder.1.rewrite", {16, 8, 1, 1});
  b.dconv("encoder.1.dconv", 8, 2, true);
  b.conv("tencoder.0.conv", {4, 2, 8});
  b.conv("tencoder.0.rewrite", {8, 4, 1});
  b.dconv("tencoder.0.dconv", 4, 2, false);
  b.conv("tencoder.1.conv", {8, 4, 8});
  b.conv("tencoder.1.rewrite", {16, 8, 1});
  b.dconv("tencoder.1.dconv", 8, 2, false);
  // decoders (0 = deepest)
  b.conv("decoder.0.conv_tr", {8, 4, 8, 1}, true);
  b.conv("decoder.0.rewrite", {16, 8, 3, 3});
  b.dconv("decoder.0.dconv", 8, 2, true);
  b.conv("decoder.1.conv_tr", {4, 16, 8, 1}, true);
  b.conv("decoder.1.rewrite", {8, 4, 3, 3});
  b.dconv("decoder.1.dconv", 4, 2, true);
  b.conv("tdecoder.0.conv_tr", {8, 4, 8}, true);
  b.conv("tdecoder.0.rewrite", {16, 8, 3});
  b.dconv("tdecoder.0.dconv", 8, 2, false);
  b.conv("tdecoder.1.conv_tr", {4, 8, 8}, true);
  b.conv("tdecoder.1.rewrite", {8, 4, 3});
  b.dconv("tdecoder.1.dconv", 4, 2, false);
  b.add("freq_emb.embedding.weight", {32, 4}, 0.1f);
  b.conv("channel_upsampler", {16, 8, 1});
  b.conv("channel_downsampler", {8, 16, 1});
  b.conv("channel_upsampler_t", {16, 8, 1});
  b.conv("channel_downsampler_t", {8, 16, 1});
  b.norm("crosstransformer.norm_in", 16);
  b.norm("crosstransformer.norm_in_t", 16);
  b.tlayer("crosstransformer.layers.0", 16, 32, false);
  b.tlayer("crosstransformer.layers.1", 16, 32, true);
  b.tlayer("crosstransformer.layers_t.0", 16, 32, false);
  b.tlayer("crosstransformer.layers_t.1", 16, 32, true);
  using K = KwValue;
  std::vector<std::pair<std::string, KwValue>> kw = {
      {"sources", K::strings({"drums", "bass", "other", "vocals"})},
      {"audio_channels", K::integer(2)},
      {"samplerate", K::integer(44100)},
      {"segment", K::real(0.5)},
      {"channels", K::integer(4)},
      {"growth", K::integer(2)},
      {"nfft", K::integer(256)},
      {"wiener_iters", K::integer(0)},
      {"end_iters", K::integer(0)},
      {"cac", K::boolean(true)},
      {"depth", K::integer(2)},
      {"rewrite", K::boolean(true)},
      {"freq_emb", K::real(0.2)},
      {"emb_scale", K::integer(10)},
      {"emb_smooth", K::boolean(true)},
      {"kernel_size", K::integer(8)},
      {"stride", K::integer(4)},
      {"time_stride", K::integer(2)},
      {"context", K::integer(1)},
      {"context_enc", K::integer(0)},
      {"norm_starts", K::integer(4)},
      {"norm_groups", K::integer(4)},
      {"dconv_mode", K::integer(3)},
      {"dconv_depth", K::integer(2)},
      {"dconv_comp", K::integer(2)},
      {"dconv_init", K::real(0.001)},
      {"bottom_channels", K::integer(16)},
      {"t_layers", K::integer(2)},
      {"t_hidden_scale", K::real(2.0)},
      {"t_heads", K::integer(2)},
      {"t_dropout", K::real(0.0)},
      {"t_layer_scale", K::boolean(true)},
      {"t_gelu", K::boolean(true)},
      {"t_emb", K::str("sin")},
      {"t_max_period", K::real(10000.0)},
      {"t_weight_pos_embed", K::real(1.0)},
      {"t_norm_in", K::boolean(true)},
      {"t_norm_in_group", K::boolean(false)},
      {"t_group_norm", K::boolean(false)},
      {"t_norm_first", K::boolean(true)},
      {"t_norm_out", K::boolean(true)},
      {"t_cross_first", K::boolean(false)},
      {"t_sparse_self_attn", K::boolean(false)},
      {"t_sparse_cross_attn", K::boolean(false)},
      {"rescale", K::real(0.1)},
  };
  return writeTorchCheckpoint(path, "demucs.htdemucs", "HTDemucs", kw, b.state, true, err);
}

}  // namespace gl::stemtiny
