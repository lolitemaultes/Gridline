// Gridline - J-191: Hybrid Transformer Demucs inference (see HTDemucs.h).
#include "HTDemucs.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gl::stems {

namespace {

// GRIDLINE_STEMS_PROFILE=1: time per stage on stderr after each segment (development aid).
struct Prof {
  bool on = std::getenv("GRIDLINE_STEMS_PROFILE") != nullptr;
  std::map<std::string, double> t;
  std::mutex m;
  void add(const std::string& k, double s) {
    std::lock_guard<std::mutex> lk(m);
    t[k] += s;
  }
  void dump() {
    std::lock_guard<std::mutex> lk(m);
    for (auto& kv : t) std::fprintf(stderr, "  %-14s %7.3f s\n", kv.first.c_str(), kv.second);
    t.clear();
  }
};
Prof& prof() {
  static Prof p;
  return p;
}
struct Scope {
  const char* name;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit Scope(const char* n) : name(n) {}
  ~Scope() {
    if (prof().on) prof().add(name, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
};

constexpr float kEps = 1e-5f;

bool fail(std::string* err, const std::string& msg) {
  if (err) *err = msg;
  return false;
}

// Splits [0, n) into chunks over the pool.
template <class F>
void parallelFor(Pool& pool, size_t n, F fn, size_t minChunk = 4096) {
  if (n == 0) return;
  const size_t workers = size_t(pool.size());
  size_t chunks = std::min(n / std::max<size_t>(1, minChunk) + 1, workers * 4);
  chunks = std::max<size_t>(1, std::min(chunks, n));
  const size_t per = (n + chunks - 1) / chunks;
  pool.run(int(chunks), [&](int c) {
    const size_t lo = size_t(c) * per, hi = std::min(n, lo + per);
    if (lo < hi) fn(lo, hi);
  });
}

void geluPar(Pool& pool, std::vector<float>& x) {
  Scope prof_("gelu");
  float* p = x.data();
  parallelFor(pool, x.size(), [p](size_t lo, size_t hi) { geluInPlace(p + lo, hi - lo); });
}

// GroupNorm with one group, applied separately to every row h of a (c, h, w) stack: statistics
// over (c, w) of that row; per-channel affine. (DConv on the spectrogram branch folds the
// frequency axis into the batch, so each frequency row is its own group.)
void groupNormRows(Pool& pool, float* x, int c, int h, int w, const std::vector<float>& g, const std::vector<float>& b) {
  Scope prof_("groupnorm");
  pool.run(h, [&](int row) {
    double s = 0, s2 = 0;
    for (int ch = 0; ch < c; ++ch) {
      const float* p = x + (size_t(ch) * size_t(h) + size_t(row)) * size_t(w);
      for (int t = 0; t < w; ++t) s += p[t];
    }
    const double n = double(c) * double(w);
    const double mean = s / n;
    for (int ch = 0; ch < c; ++ch) {
      const float* p = x + (size_t(ch) * size_t(h) + size_t(row)) * size_t(w);
      for (int t = 0; t < w; ++t) {
        const double d = p[t] - mean;
        s2 += d * d;
      }
    }
    const float inv = float(1.0 / std::sqrt(s2 / n + double(kEps)));
    const float m = float(mean);
    for (int ch = 0; ch < c; ++ch) {
      float* p = x + (size_t(ch) * size_t(h) + size_t(row)) * size_t(w);
      const float gg = g[size_t(ch)] * inv, bb = b[size_t(ch)];
      for (int t = 0; t < w; ++t) p[t] = (p[t] - m) * gg + bb;
    }
  });
}

// GLU over the channel axis: (2c, n) -> (c, n).
std::vector<float> glu(Pool& pool, const std::vector<float>& x, int c, size_t n) {
  Scope prof_("glu");
  std::vector<float> out(size_t(c) * n);
  const float* a = x.data();
  const float* g = x.data() + size_t(c) * n;
  float* o = out.data();
  parallelFor(pool, size_t(c) * n, [&](size_t lo, size_t hi) {
    for (size_t i = lo; i < hi; ++i) o[i] = a[i] * sigmoid(g[i]);
  });
  return out;
}

// LayerNorm over the last axis of (n, d) tokens.
void layerNorm(Pool& pool, const float* x, float* out, int n, int d, const std::vector<float>& g, const std::vector<float>& b) {
  Scope prof_("layernorm");
  parallelFor(
      pool, size_t(n),
      [&](size_t lo, size_t hi) {
        for (size_t r = lo; r < hi; ++r) {
          const float* p = x + r * size_t(d);
          float* o = out + r * size_t(d);
          double s = 0, s2 = 0;
          for (int k = 0; k < d; ++k) s += p[k];
          const double mean = s / d;
          for (int k = 0; k < d; ++k) {
            const double v = p[k] - mean;
            s2 += v * v;
          }
          const float inv = float(1.0 / std::sqrt(s2 / d + double(kEps)));
          const float m = float(mean);
          for (int k = 0; k < d; ++k) o[k] = (p[k] - m) * inv * g[size_t(k)] + b[size_t(k)];
        }
      },
      16);
}

// y = x W^T + b for tokens x (n, in), W (out, in).
std::vector<float> linear(Pool& pool, const float* x, int n, int in, const std::vector<float>& w, const std::vector<float>& b,
                          int out) {
  Scope prof_("linear");
  std::vector<float> y(size_t(n) * size_t(out));
  gemm(pool, false, true, n, out, in, 1.f, x, in, w.data(), in, 0.f, y.data(), out);
  if (!b.empty()) {
    float* p = y.data();
    parallelFor(pool, size_t(n), [&](size_t lo, size_t hi) {
      for (size_t r = lo; r < hi; ++r)
        for (int k = 0; k < out; ++k) p[r * size_t(out) + size_t(k)] += b[size_t(k)];
    }, 64);
  }
  return y;
}

int getInt(const PyRef& kw, const char* name, int def) {
  PyRef v = kw->get(name);
  return v && v->isNumber() ? int(v->number()) : def;
}
double getNum(const PyRef& kw, const char* name, double def) {
  PyRef v = kw->get(name);
  return v && v->isNumber() ? v->number() : def;
}
bool isTrue(const PyRef& kw, const char* name, bool def) {
  PyRef v = kw->get(name);
  if (!v || v->kind == PyValue::Kind::None) return def;
  return v->isNumber() ? v->number() != 0 : def;
}

}  // namespace

// ---------------------------------------------------------------- configuration

bool demucsConfigFrom(const PyRef& kw, DemucsConfig& c, std::string* err) {
  if (!kw || kw->kind != PyValue::Kind::Dict) return fail(err, "model has no configuration");
  PyRef src = kw->get("sources");
  if (!src || (src->kind != PyValue::Kind::List && src->kind != PyValue::Kind::Tuple) || src->items.empty())
    return fail(err, "model lists no sources");
  c.sources.clear();
  for (const PyRef& s : src->items) c.sources.push_back(s ? s->s : std::string());
  c.audioChannels = getInt(kw, "audio_channels", 2);
  c.samplerate = getInt(kw, "samplerate", 44100);
  c.segment = getNum(kw, "segment", 10.0);
  c.channels = getInt(kw, "channels", 48);
  c.growth = getInt(kw, "growth", 2);
  c.nfft = getInt(kw, "nfft", 4096);
  c.depth = getInt(kw, "depth", 4);
  c.kernelSize = getInt(kw, "kernel_size", 8);
  c.stride = getInt(kw, "stride", 4);
  c.context = getInt(kw, "context", 1);
  c.contextEnc = getInt(kw, "context_enc", 0);
  c.dconvMode = getInt(kw, "dconv_mode", 1);
  c.dconvDepth = getInt(kw, "dconv_depth", 2);
  c.dconvComp = getInt(kw, "dconv_comp", 8);
  c.freqEmb = getNum(kw, "freq_emb", 0.2);
  c.embScale = getNum(kw, "emb_scale", 10.0);
  c.bottomChannels = getInt(kw, "bottom_channels", 0);
  c.tLayers = getInt(kw, "t_layers", 5);
  c.tHeads = getInt(kw, "t_heads", 8);
  c.tHiddenScale = getNum(kw, "t_hidden_scale", 4.0);
  c.tMaxPeriod = getNum(kw, "t_max_period", 10000.0);
  c.tWeightPosEmbed = getNum(kw, "t_weight_pos_embed", 1.0);
  c.tCrossFirst = isTrue(kw, "t_cross_first", false);

  auto unsupported = [&](const std::string& what) { return fail(err, "unsupported model option: " + what); };
  if (PyRef v = kw->get("channels_time"); v && v->kind != PyValue::Kind::None) return unsupported("channels_time");
  if (getInt(kw, "wiener_iters", 0) != 0) return unsupported("wiener_iters");
  if (!isTrue(kw, "cac", true)) return unsupported("cac off");
  if (!isTrue(kw, "rewrite", true)) return unsupported("rewrite off");
  if (PyRef v = kw->get("multi_freqs"); v && !v->items.empty()) return unsupported("multi_freqs");
  if (getInt(kw, "norm_starts", 4) < c.depth) return unsupported("norm_starts");
  if (PyRef v = kw->get("t_emb"); v && v->s != "sin") return unsupported("t_emb " + v->s);
  if (!isTrue(kw, "t_norm_in", true) || isTrue(kw, "t_norm_in_group", false) || isTrue(kw, "t_group_norm", false))
    return unsupported("transformer input norm");
  if (!isTrue(kw, "t_norm_first", true) || !isTrue(kw, "t_norm_out", true)) return unsupported("transformer norm order");
  if (!isTrue(kw, "t_layer_scale", true) || !isTrue(kw, "t_gelu", true)) return unsupported("transformer layer type");
  if (isTrue(kw, "t_sparse_self_attn", false) || isTrue(kw, "t_sparse_cross_attn", false)) return unsupported("sparse attention");
  if (getInt(kw, "t_sin_random_shift", 0) != 0) return unsupported("t_sin_random_shift");
  if (!isTrue(kw, "use_train_segment", true)) return unsupported("use_train_segment off");
  if (c.audioChannels < 1 || c.audioChannels > 8 || c.depth < 1 || c.depth > 8 || c.nfft < 16 || (c.nfft & (c.nfft - 1)) ||
      c.stride < 1 || c.kernelSize < c.stride || c.segment <= 0 || c.segment > 120 || c.samplerate < 8000 ||
      c.samplerate > 384000 || c.tHeads < 1)
    return fail(err, "model configuration out of range");
  int freqs = c.nfft / 2;
  for (int i = 0; i < c.depth; ++i) {
    if (freqs <= c.kernelSize) return unsupported("frequency branch merges into the time branch");
    freqs /= c.stride;
  }
  return true;
}

// ---------------------------------------------------------------- weights

bool HTDemucs::take(std::map<std::string, Tensor>& st, const std::string& name, std::vector<int64_t> shape,
                    std::vector<float>& out, std::string* err) const {
  auto it = st.find(name);
  if (it == st.end()) return fail(err, "model weight missing: " + name);
  if (!shape.empty() && it->second.shape != shape) return fail(err, "model weight has an unexpected shape: " + name);
  out = std::move(it->second.data);
  st.erase(it);
  return true;
}

bool HTDemucs::takeConv(std::map<std::string, Tensor>& st, const std::string& name, Conv& c, std::string* err) const {
  auto it = st.find(name + ".weight");
  if (it == st.end()) return fail(err, "model weight missing: " + name + ".weight");
  const std::vector<int64_t> s = it->second.shape;
  if (s.size() < 2 || s.size() > 4) return fail(err, "model weight has an unexpected shape: " + name);
  c.cout = int(s[0]);
  c.cin = int(s[1]);
  c.kh = s.size() == 4 ? int(s[2]) : 1;
  c.kw = s.size() == 4 ? int(s[3]) : (s.size() == 3 ? int(s[2]) : 1);
  if (!take(st, name + ".weight", s, c.w, err)) return false;
  if (st.count(name + ".bias")) {
    // conv-transpose biases have the second dimension's size
    auto b = st.find(name + ".bias");
    c.b = std::move(b->second.data);
    st.erase(b);
  }
  return true;
}

bool HTDemucs::takeNorm(std::map<std::string, Tensor>& st, const std::string& name, int n, Norm& out, std::string* err) const {
  return take(st, name + ".weight", {n}, out.w, err) && take(st, name + ".bias", {n}, out.b, err);
}

bool HTDemucs::takeDConv(std::map<std::string, Tensor>& st, const std::string& prefix, int channels,
                         std::vector<DConvLayer>& out, std::string* err) const {
  out.clear();
  if (!st.count(prefix + ".layers.0.0.weight")) return true;
  for (int d = 0;; ++d) {
    const std::string p = prefix + ".layers." + std::to_string(d);
    if (!st.count(p + ".0.weight")) break;
    DConvLayer l;
    l.dilation = 1 << d;
    if (!takeConv(st, p + ".0", l.c1, err) || !takeConv(st, p + ".3", l.c2, err)) return false;
    if (l.c1.cin != channels || l.c2.cout != 2 * channels || l.c2.cin != l.c1.cout || l.c1.kw != 3)
      return fail(err, "model residual branch has an unexpected shape: " + p);
    if (!takeNorm(st, p + ".1", l.c1.cout, l.n1, err) || !takeNorm(st, p + ".4", 2 * channels, l.n2, err)) return false;
    if (!take(st, p + ".6.scale", {channels}, l.scale, err)) return false;
    out.push_back(std::move(l));
  }
  return true;
}

bool HTDemucs::load(const std::string& path, std::string* err) {
  TorchFile f;
  if (!f.open(path, err)) return false;
  const PyRef& root = f.root();
  PyRef klass = root ? root->get("klass") : nullptr;
  if (!klass || klass->kind != PyValue::Kind::Global || klass->s != "demucs.htdemucs HTDemucs")
    return fail(err, "not a Hybrid Transformer Demucs model");
  DemucsConfig cfg;
  if (!demucsConfigFrom(root->get("kwargs"), cfg, err)) return false;
  std::map<std::string, Tensor> state;
  if (!f.loadAll(root->get("state"), state, err)) return false;
  return init(cfg, std::move(state), err);
}

bool HTDemucs::init(const DemucsConfig& cfg, std::map<std::string, Tensor> st, std::string* err) {
  cfg_ = cfg;
  enc_.clear();
  tenc_.clear();
  dec_.clear();
  tdec_.clear();
  layers_.clear();
  layersT_.clear();
  const int depth = cfg.depth;
  for (int i = 0; i < depth; ++i) {
    for (int branch = 0; branch < 2; ++branch) {
      const bool freq = branch == 0;
      EncLayer e;
      e.freq = freq;
      e.stride = cfg.stride;
      e.pad = cfg.kernelSize / 4;
      e.ctx = cfg.contextEnc;
      const std::string p = std::string(freq ? "encoder." : "tencoder.") + std::to_string(i);
      if (!takeConv(st, p + ".conv", e.conv, err) || !takeConv(st, p + ".rewrite", e.rewrite, err)) return false;
      if ((freq && (e.conv.kh != cfg.kernelSize || e.conv.kw != 1)) || (!freq && e.conv.kw != cfg.kernelSize) ||
          e.rewrite.cin != e.conv.cout || e.rewrite.cout != 2 * e.conv.cout)
        return fail(err, "model encoder has an unexpected shape: " + p);
      if (!takeDConv(st, p + ".dconv", e.conv.cout, e.dconv, err)) return false;
      (freq ? enc_ : tenc_).push_back(std::move(e));
    }
  }
  for (int j = 0; j < depth; ++j) {
    for (int branch = 0; branch < 2; ++branch) {
      const bool freq = branch == 0;
      DecLayer d;
      d.freq = freq;
      d.stride = cfg.stride;
      d.pad = cfg.kernelSize / 4;
      d.ctx = cfg.context;
      d.last = j == depth - 1;
      const std::string p = std::string(freq ? "decoder." : "tdecoder.") + std::to_string(j);
      if (!takeConv(st, p + ".conv_tr", d.convTr, err) || !takeConv(st, p + ".rewrite", d.rewrite, err)) return false;
      // conv_tr weight: (cin, cout, k[, 1]); takeConv read it as (cout=cin, cin=cout)
      std::swap(d.convTr.cin, d.convTr.cout);
      if (freq && d.convTr.kw != 1) return fail(err, "model decoder has an unexpected shape: " + p);
      if (!freq) d.convTr.kh = d.convTr.kw;
      if (d.convTr.kh != cfg.kernelSize || d.rewrite.cin != d.convTr.cin || d.rewrite.cout != 2 * d.convTr.cin ||
          int(d.convTr.b.size()) != d.convTr.cout)
        return fail(err, "model decoder has an unexpected shape: " + p);
      if (!takeDConv(st, p + ".dconv", d.convTr.cin, d.dconv, err)) return false;
      (freq ? dec_ : tdec_).push_back(std::move(d));
    }
  }
  if (cfg.freqEmb > 0) {
    auto it = st.find("freq_emb.embedding.weight");
    if (it == st.end() || it->second.shape.size() != 2) return fail(err, "model frequency embedding missing");
    freqEmbFreqs_ = int(it->second.shape[0]);
    if (int(it->second.shape[1]) != enc_[0].conv.cout) return fail(err, "model frequency embedding has an unexpected shape");
    freqEmb_ = std::move(it->second.data);
    st.erase(it);
    for (float& v : freqEmb_) v *= float(cfg.embScale * cfg.freqEmb);
  }
  const int bottom = enc_.back().conv.cout;
  tDim_ = bottom;
  if (cfg.bottomChannels > 0) {
    if (!takeConv(st, "channel_upsampler", up_, err) || !takeConv(st, "channel_downsampler", down_, err) ||
        !takeConv(st, "channel_upsampler_t", upT_, err) || !takeConv(st, "channel_downsampler_t", downT_, err))
      return false;
    tDim_ = cfg.bottomChannels;
    if (up_.cin != bottom || up_.cout != tDim_ || down_.cout != bottom || upT_.cin != tenc_.back().conv.cout)
      return fail(err, "model bottleneck has an unexpected shape");
  }
  if (tenc_.back().conv.cout != bottom) return fail(err, "model branches have different widths");
  if (cfg.tLayers > 0) {
    if (tDim_ % cfg.tHeads != 0 || tDim_ % 4 != 0) return fail(err, "model transformer width not divisible");
    if (!takeNorm(st, "crosstransformer.norm_in", tDim_, normIn_, err) ||
        !takeNorm(st, "crosstransformer.norm_in_t", tDim_, normInT_, err))
      return false;
    const int hidden = int(tDim_ * cfg.tHiddenScale);
    const int parity = cfg.tCrossFirst ? 1 : 0;
    for (int i = 0; i < cfg.tLayers; ++i)
      for (int branch = 0; branch < 2; ++branch) {
        TLayer l;
        l.cross = (i % 2) != parity;
        const std::string p = std::string(branch == 0 ? "crosstransformer.layers." : "crosstransformer.layers_t.") + std::to_string(i);
        const std::string a = p + (l.cross ? ".cross_attn" : ".self_attn");
        if (!take(st, a + ".in_proj_weight", {3 * tDim_, tDim_}, l.attn.inW, err) ||
            !take(st, a + ".in_proj_bias", {3 * tDim_}, l.attn.inB, err) ||
            !take(st, a + ".out_proj.weight", {tDim_, tDim_}, l.attn.outW, err) ||
            !take(st, a + ".out_proj.bias", {tDim_}, l.attn.outB, err) ||
            !takeConv(st, p + ".linear1", l.lin1, err) || !takeConv(st, p + ".linear2", l.lin2, err) ||
            !takeNorm(st, p + ".norm1", tDim_, l.norm1, err) || !takeNorm(st, p + ".norm2", tDim_, l.norm2, err) ||
            (l.cross && !takeNorm(st, p + ".norm3", tDim_, l.norm3, err)) ||
            !takeNorm(st, p + ".norm_out", tDim_, l.normOut, err) ||
            !take(st, p + ".gamma_1.scale", {tDim_}, l.gamma1, err) || !take(st, p + ".gamma_2.scale", {tDim_}, l.gamma2, err))
          return false;
        if (l.lin1.cout != hidden || l.lin1.cin != tDim_ || l.lin2.cout != tDim_ || l.lin2.cin != hidden)
          return fail(err, "model transformer has an unexpected shape: " + p);
        (branch == 0 ? layers_ : layersT_).push_back(std::move(l));
      }
  }
  // decoder chain widths
  const int outFreq = int(cfg.sources.size()) * cfg.audioChannels * 2, outTime = int(cfg.sources.size()) * cfg.audioChannels;
  if (dec_.back().convTr.cout != outFreq || tdec_.back().convTr.cout != outTime || enc_[0].conv.cin != cfg.audioChannels * 2 ||
      tenc_[0].conv.cin != cfg.audioChannels)
    return fail(err, "model input or output width does not match its sources");
  return true;
}

// ---------------------------------------------------------------- convolutions

std::vector<float> conv2d(Pool& pool, const float* x, int cin, int h, int w, const float* weight, const float* bias,
                          int cout, int kh, int kw, int sh, int sw, int ph, int pw, int dh, int dw, int& ho, int& wo) {
  Scope prof_("conv2d");
  ho = (h + 2 * ph - dh * (kh - 1) - 1) / sh + 1;
  wo = (w + 2 * pw - dw * (kw - 1) - 1) / sw + 1;
  const size_t P = size_t(ho) * size_t(wo);
  std::vector<float> out(size_t(cout) * P);
  if (kh == 1 && kw == 1 && sh == 1 && sw == 1 && ph == 0 && pw == 0) {
    gemm(pool, false, false, cout, int(P), cin, 1.f, weight, cin, x, int(P), 0.f, out.data(), int(P));
  } else {
    const int kd = cin * kh * kw;
    const size_t budget = size_t(1) << 23;  // floats in the column buffer (32 MB)
    const size_t tile = std::max<size_t>(64, std::min(P, budget / size_t(kd)));
    std::vector<float> cols(size_t(kd) * tile);
    for (size_t p0 = 0; p0 < P; p0 += tile) {
      const size_t np = std::min(tile, P - p0);
      float* cp = cols.data();
      pool.run(kd, [&](int r) {
        const int ci = r / (kh * kw), ky = (r / kw) % kh, kx = r % kw;
        const float* plane = x + size_t(ci) * size_t(h) * size_t(w);
        float* dst = cp + size_t(r) * np;
        int oy = int(p0 / size_t(wo)), ox = int(p0 % size_t(wo));
        for (size_t q = 0; q < np; ++q) {
          const int iy = oy * sh - ph + ky * dh, ix = ox * sw - pw + kx * dw;
          dst[q] = (iy >= 0 && iy < h && ix >= 0 && ix < w) ? plane[size_t(iy) * size_t(w) + size_t(ix)] : 0.f;
          if (++ox == wo) {
            ox = 0;
            ++oy;
          }
        }
      });
      gemm(pool, false, false, cout, int(np), kd, 1.f, weight, kd, cp, int(np), 0.f, out.data() + p0, int(P));
    }
  }
  if (bias) {
    float* o = out.data();
    pool.run(cout, [&](int c) {
      float* p = o + size_t(c) * P;
      const float b = bias[c];
      for (size_t i = 0; i < P; ++i) p[i] += b;
    });
  }
  return out;
}

std::vector<float> convTranspose(Pool& pool, const float* x, int cin, int h, int w, const float* weight,
                                 const float* bias, int cout, int k, int s, bool axisH, int& ho, int& wo) {
  Scope prof_("convtr");
  ho = axisH ? (h - 1) * s + k : h;
  wo = axisH ? w : (w - 1) * s + k;
  const size_t P = size_t(h) * size_t(w), Po = size_t(ho) * size_t(wo);
  std::vector<float> out(size_t(cout) * Po, 0.f);
  const int rows = cout * k;
  const size_t budget = size_t(1) << 23;
  const size_t tile = std::max<size_t>(64, std::min(P, budget / size_t(rows)));
  std::vector<float> buf(size_t(rows) * tile);
  for (size_t p0 = 0; p0 < P; p0 += tile) {
    const size_t np = std::min(tile, P - p0);
    gemm(pool, true, false, rows, int(np), cin, 1.f, weight, rows, x + p0, int(P), 0.f, buf.data(), int(np));
    float* o = out.data();
    const float* bp = buf.data();
    pool.run(cout, [&](int co) {
      float* plane = o + size_t(co) * Po;
      for (int kk = 0; kk < k; ++kk) {
        const float* src = bp + (size_t(co) * size_t(k) + size_t(kk)) * np;
        int y = int(p0 / size_t(w)), xw = int(p0 % size_t(w));
        for (size_t q = 0; q < np; ++q) {
          if (axisH)
            plane[size_t(y * s + kk) * size_t(wo) + size_t(xw)] += src[q];
          else
            plane[size_t(y) * size_t(wo) + size_t(xw * s + kk)] += src[q];
          if (++xw == w) {
            xw = 0;
            ++y;
          }
        }
      }
    });
  }
  if (bias) {
    float* o = out.data();
    pool.run(cout, [&](int c) {
      float* p = o + size_t(c) * Po;
      for (size_t i = 0; i < Po; ++i) p[i] += bias[c];
    });
  }
  return out;
}

// ---------------------------------------------------------------- spectrogram

namespace {
// Reflect padding (no edge repeat) of one channel.
std::vector<float> reflectPad(const float* x, int n, int left, int right) {
  std::vector<float> out(size_t(n + left + right));
  for (int i = 0; i < n + left + right; ++i) {
    int j = i - left;
    if (n == 1) {
      j = 0;
    } else {
      const int period = 2 * (n - 1);
      j = ((j % period) + period) % period;
      if (j >= n) j = period - j;
    }
    out[size_t(i)] = x[j];
  }
  return out;
}

std::vector<float> hann(int n) {
  std::vector<float> w(static_cast<size_t>(n));
  const double pi = 3.14159265358979323846;
  for (int i = 0; i < n; ++i) w[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * pi * i / n));
  return w;
}
}  // namespace

std::vector<std::complex<float>> demucsSpec(const float* x, int channels, int length, int nfft, int& frames) {
  const int hl = nfft / 4;
  const int le = (length + hl - 1) / hl;
  const int pad = hl / 2 * 3;
  const int bins = nfft / 2;
  frames = le;
  std::vector<std::complex<float>> z(size_t(channels) * size_t(bins) * size_t(le));
  const std::vector<float> win = hann(nfft);
  const float norm = float(1.0 / std::sqrt(double(nfft)));
  Fft fft(nfft);
  std::vector<std::complex<float>> buf(static_cast<size_t>(nfft));
  for (int c = 0; c < channels; ++c) {
    const std::vector<float> a = reflectPad(x + size_t(c) * size_t(length), length, pad, pad + le * hl - length);
    const std::vector<float> b = reflectPad(a.data(), int(a.size()), nfft / 2, nfft / 2);  // stft center
    for (int f = 0; f < le; ++f) {
      const int start = (f + 2) * hl;  // frames 2 .. 2 + le of the full STFT
      for (int n = 0; n < nfft; ++n) buf[size_t(n)] = std::complex<float>(b[size_t(start + n)] * win[size_t(n)], 0.f);
      fft.run(buf.data(), false);
      for (int k = 0; k < bins; ++k) z[(size_t(c) * size_t(bins) + size_t(k)) * size_t(le) + size_t(f)] = buf[size_t(k)] * norm;
    }
  }
  return z;
}

std::vector<float> demucsISpec(const std::complex<float>* z, int channels, int nfft, int frames, int length) {
  const int hl = nfft / 4;
  const int bins = nfft / 2;
  const int pad = hl / 2 * 3;
  const int leOut = hl * ((length + hl - 1) / hl) + 2 * pad;
  const int nFrames = frames + 4;  // two empty frames each side
  const int total = nfft + hl * (nFrames - 1);
  const std::vector<float> win = hann(nfft);
  std::vector<float> env(size_t(total), 0.f);
  for (int f = 0; f < nFrames; ++f)
    for (int n = 0; n < nfft; ++n) env[size_t(f * hl + n)] += win[size_t(n)] * win[size_t(n)];
  Fft fft(nfft);
  std::vector<std::complex<float>> buf(static_cast<size_t>(nfft));
  const float scale = float(std::sqrt(double(nfft)) / double(nfft));
  std::vector<float> out(size_t(channels) * size_t(length));
  std::vector<float> y(static_cast<size_t>(total));
  for (int c = 0; c < channels; ++c) {
    std::fill(y.begin(), y.end(), 0.f);
    for (int f = 0; f < frames; ++f) {
      // spectrum with the (zero) Nyquist bin restored; Hermitian extension
      for (int k = 0; k < bins; ++k) buf[size_t(k)] = z[(size_t(c) * size_t(bins) + size_t(k)) * size_t(frames) + size_t(f)];
      buf[0] = std::complex<float>(buf[0].real(), 0.f);
      buf[size_t(bins)] = 0.f;
      for (int k = 1; k < bins; ++k) buf[size_t(nfft - k)] = std::conj(buf[size_t(k)]);
      fft.run(buf.data(), true);
      const int start = (f + 2) * hl;
      for (int n = 0; n < nfft; ++n) y[size_t(start + n)] += buf[size_t(n)].real() * scale * win[size_t(n)];
    }
    // centre (nfft/2), then the reference's own padding
    float* o = out.data() + size_t(c) * size_t(length);
    for (int i = 0; i < length; ++i) {
      const size_t idx = size_t(nfft / 2 + pad + i);
      const float e = env[idx];
      o[i] = e > 1e-11f ? y[idx] / e : 0.f;
    }
    (void)leOut;
  }
  return out;
}

// ---------------------------------------------------------------- layers

void HTDemucs::runDConv(Pool& pool, const std::vector<DConvLayer>& layers, std::vector<float>& x, int c, int h, int w) const {
  for (const DConvLayer& l : layers) {
    int ho = 0, wo = 0;
    std::vector<float> y = conv2d(pool, x.data(), c, h, w, l.c1.w.data(), l.c1.b.data(), l.c1.cout, 1, 3, 1, 1, 0,
                                  l.dilation, 1, l.dilation, ho, wo);
    groupNormRows(pool, y.data(), l.c1.cout, h, w, l.n1.w, l.n1.b);
    geluPar(pool, y);
    std::vector<float> y2 = conv2d(pool, y.data(), l.c1.cout, h, w, l.c2.w.data(), l.c2.b.data(), l.c2.cout, 1, 1, 1, 1, 0, 0,
                                   1, 1, ho, wo);
    groupNormRows(pool, y2.data(), l.c2.cout, h, w, l.n2.w, l.n2.b);
    const size_t plane = size_t(h) * size_t(w);
    const float* a = y2.data();
    const float* g = y2.data() + size_t(c) * plane;
    float* xp = x.data();
    pool.run(c, [&](int ch) {
      const float s = l.scale[size_t(ch)];
      const size_t off = size_t(ch) * plane;
      for (size_t i = 0; i < plane; ++i) xp[off + i] += s * a[off + i] * sigmoid(g[off + i]);
    });
  }
}

void HTDemucs::runEncoder(Pool& pool, const EncLayer& e, std::vector<float>& x, int& c, int& h, int& w) const {
  if (!e.freq && w % e.stride != 0) {
    const int nw = w + (e.stride - w % e.stride);
    std::vector<float> p(size_t(c) * size_t(h) * size_t(nw), 0.f);
    for (int ch = 0; ch < c; ++ch)
      for (int r = 0; r < h; ++r)
        std::memcpy(&p[(size_t(ch) * size_t(h) + size_t(r)) * size_t(nw)], &x[(size_t(ch) * size_t(h) + size_t(r)) * size_t(w)],
                    sizeof(float) * size_t(w));
    x.swap(p);
    w = nw;
  }
  int ho = 0, wo = 0;
  std::vector<float> y;
  if (e.freq)
    y = conv2d(pool, x.data(), c, h, w, e.conv.w.data(), e.conv.b.data(), e.conv.cout, e.conv.kh, 1, e.stride, 1, e.pad, 0, 1, 1,
               ho, wo);
  else
    y = conv2d(pool, x.data(), c, h, w, e.conv.w.data(), e.conv.b.data(), e.conv.cout, 1, e.conv.kw, 1, e.stride, 0, e.pad, 1, 1,
               ho, wo);
  geluPar(pool, y);
  c = e.conv.cout;
  h = ho;
  w = wo;
  runDConv(pool, e.dconv, y, c, h, w);
  const int k = e.rewrite.kw;
  std::vector<float> z = conv2d(pool, y.data(), c, h, w, e.rewrite.w.data(), e.rewrite.b.data(), e.rewrite.cout, e.rewrite.kh, k,
                                1, 1, e.rewrite.kh / 2, k / 2, 1, 1, ho, wo);
  x = glu(pool, z, c, size_t(h) * size_t(w));
}

void HTDemucs::runDecoder(Pool& pool, const DecLayer& d, std::vector<float>& x, const std::vector<float>& skip, int& c, int& h,
                          int& w, int length) const {
  for (size_t i = 0; i < x.size(); ++i) x[i] += skip[i];
  int ho = 0, wo = 0;
  std::vector<float> z = conv2d(pool, x.data(), c, h, w, d.rewrite.w.data(), d.rewrite.b.data(), d.rewrite.cout, d.rewrite.kh,
                                d.rewrite.kw, 1, 1, d.rewrite.kh / 2, d.rewrite.kw / 2, 1, 1, ho, wo);
  std::vector<float> y = glu(pool, z, c, size_t(h) * size_t(w));
  runDConv(pool, d.dconv, y, c, h, w);
  std::vector<float> t = convTranspose(pool, y.data(), c, h, w, d.convTr.w.data(), d.convTr.b.data(), d.convTr.cout, d.convTr.kh,
                                       d.stride, d.freq, ho, wo);
  const int co = d.convTr.cout;
  if (d.freq) {
    const int nh = ho - 2 * d.pad;
    x.assign(size_t(co) * size_t(nh) * size_t(wo), 0.f);
    for (int ch = 0; ch < co; ++ch)
      std::memcpy(&x[size_t(ch) * size_t(nh) * size_t(wo)], &t[(size_t(ch) * size_t(ho) + size_t(d.pad)) * size_t(wo)],
                  sizeof(float) * size_t(nh) * size_t(wo));
    h = nh;
    w = wo;
  } else {
    x.assign(size_t(co) * size_t(length), 0.f);
    for (int ch = 0; ch < co; ++ch)
      std::memcpy(&x[size_t(ch) * size_t(length)], &t[size_t(ch) * size_t(wo) + size_t(d.pad)], sizeof(float) * size_t(length));
    h = 1;
    w = length;
  }
  c = co;
  if (!d.last) geluPar(pool, x);
}

void HTDemucs::runTLayer(Pool& pool, const TLayer& l, std::vector<float>& x, int n, const std::vector<float>* kv, int m) const {
  Scope prof_("tlayer");
  const int D = tDim_, H = cfg_.tHeads, hd = D / H;
  std::vector<float> q(size_t(n) * size_t(D));
  layerNorm(pool, x.data(), q.data(), n, D, l.norm1.w, l.norm1.b);
  std::vector<float> kn;
  const float* kvp = q.data();
  int mm = n;
  if (l.cross) {
    kn.resize(size_t(m) * size_t(D));
    layerNorm(pool, kv->data(), kn.data(), m, D, l.norm2.w, l.norm2.b);
    kvp = kn.data();
    mm = m;
  }
  // projections
  std::vector<float> Q(size_t(n) * size_t(D)), K(size_t(mm) * size_t(D)), V(size_t(mm) * size_t(D));
  gemm(pool, false, true, n, D, D, 1.f, q.data(), D, l.attn.inW.data(), D, 0.f, Q.data(), D);
  gemm(pool, false, true, mm, D, D, 1.f, kvp, D, l.attn.inW.data() + size_t(D) * size_t(D), D, 0.f, K.data(), D);
  gemm(pool, false, true, mm, D, D, 1.f, kvp, D, l.attn.inW.data() + 2 * size_t(D) * size_t(D), D, 0.f, V.data(), D);
  {
    float* Qp = Q.data();
    float* Kp = K.data();
    float* Vp = V.data();
    const float* b = l.attn.inB.data();
    parallelFor(pool, size_t(n), [&](size_t lo, size_t hi) {
      for (size_t r = lo; r < hi; ++r)
        for (int k = 0; k < D; ++k) Qp[r * size_t(D) + size_t(k)] += b[k];
    }, 64);
    parallelFor(pool, size_t(mm), [&](size_t lo, size_t hi) {
      for (size_t r = lo; r < hi; ++r)
        for (int k = 0; k < D; ++k) {
          Kp[r * size_t(D) + size_t(k)] += b[D + k];
          Vp[r * size_t(D) + size_t(k)] += b[2 * D + k];
        }
    }, 64);
  }
  std::vector<float> O(size_t(n) * size_t(D));
  {
    // Attention by blocks of query rows, every (head, block) pair an independent task: scores,
    // softmax and the weighted sum stay in one thread's cache.
    Scope profA_("attention");
    const float scale = 1.f / std::sqrt(float(hd));
    const int rb = 64;
    const int nb = (n + rb - 1) / rb;
    const float* Qp = Q.data();
    const float* Kp = K.data();
    const float* Vp = V.data();
    float* Op = O.data();
    pool.run(H * nb, [&](int task) {
      const int hh = task / nb, r0 = (task % nb) * rb, nr = std::min(rb, n - r0);
      std::vector<float> Sb(size_t(nr) * size_t(mm));
      gemmSerial(false, true, nr, mm, hd, scale, Qp + size_t(r0) * size_t(D) + size_t(hh * hd), D, Kp + hh * hd, D, 0.f,
                 Sb.data(), mm);
      for (int r = 0; r < nr; ++r) {
        float* row = Sb.data() + size_t(r) * size_t(mm);
        float mx = row[0];
        for (int j = 1; j < mm; ++j) mx = std::max(mx, row[j]);
        double sum = 0;
        for (int j = 0; j < mm; ++j) {
          row[j] = std::exp(row[j] - mx);
          sum += row[j];
        }
        const float inv = float(1.0 / sum);
        for (int j = 0; j < mm; ++j) row[j] *= inv;
      }
      gemmSerial(false, false, nr, hd, mm, 1.f, Sb.data(), mm, Vp + hh * hd, D, 0.f,
                 Op + size_t(r0) * size_t(D) + size_t(hh * hd), D);
    });
  }
  std::vector<float> att = linear(pool, O.data(), n, D, l.attn.outW, l.attn.outB, D);
  {
    float* xp = x.data();
    const float* ap = att.data();
    parallelFor(pool, size_t(n), [&](size_t lo, size_t hi) {
      for (size_t r = lo; r < hi; ++r)
        for (int k = 0; k < D; ++k) xp[r * size_t(D) + size_t(k)] += l.gamma1[size_t(k)] * ap[r * size_t(D) + size_t(k)];
    }, 64);
  }
  // feed-forward
  layerNorm(pool, x.data(), q.data(), n, D, l.cross ? l.norm3.w : l.norm2.w, l.cross ? l.norm3.b : l.norm2.b);
  std::vector<float> hdn = linear(pool, q.data(), n, D, l.lin1.w, l.lin1.b, l.lin1.cout);
  geluPar(pool, hdn);
  std::vector<float> ff = linear(pool, hdn.data(), n, l.lin1.cout, l.lin2.w, l.lin2.b, D);
  double s = 0;
  {
    float* xp = x.data();
    const float* fp = ff.data();
    for (size_t r = 0; r < size_t(n); ++r)
      for (int k = 0; k < D; ++k) {
        float& v = xp[r * size_t(D) + size_t(k)];
        v += l.gamma2[size_t(k)] * fp[r * size_t(D) + size_t(k)];
        s += v;
      }
  }
  // GroupNorm(1) over all tokens and channels
  const double cnt = double(n) * double(D);
  const double mean = s / cnt;
  double s2 = 0;
  for (float v : x) s2 += (v - mean) * (v - mean);
  const float inv = float(1.0 / std::sqrt(s2 / cnt + double(kEps)));
  const float mf = float(mean);
  float* xp = x.data();
  parallelFor(pool, size_t(n), [&](size_t lo, size_t hi) {
    for (size_t r = lo; r < hi; ++r)
      for (int k = 0; k < D; ++k) {
        float& v = xp[r * size_t(D) + size_t(k)];
        v = (v - mf) * inv * l.normOut.w[size_t(k)] + l.normOut.b[size_t(k)];
      }
  }, 64);
}

void HTDemucs::runTransformer(Pool& pool, std::vector<float>& x, int fr, int t1, std::vector<float>& xt, int t2) const {
  Scope prof_("transformer");
  const int D = tDim_;
  const int n = fr * t1;
  // tokens: index t * fr + f
  std::vector<float> X(size_t(n) * size_t(D)), Xt(size_t(t2) * size_t(D));
  for (int c = 0; c < D; ++c)
    for (int f = 0; f < fr; ++f)
      for (int t = 0; t < t1; ++t) X[(size_t(t) * size_t(fr) + size_t(f)) * size_t(D) + size_t(c)] = x[(size_t(c) * size_t(fr) + size_t(f)) * size_t(t1) + size_t(t)];
  for (int c = 0; c < D; ++c)
    for (int t = 0; t < t2; ++t) Xt[size_t(t) * size_t(D) + size_t(c)] = xt[size_t(c) * size_t(t2) + size_t(t)];
  layerNorm(pool, X.data(), X.data(), n, D, normIn_.w, normIn_.b);
  layerNorm(pool, Xt.data(), Xt.data(), t2, D, normInT_.w, normInT_.b);
  const double maxp = cfg_.tMaxPeriod;
  const float wpe = float(cfg_.tWeightPosEmbed);
  {  // 2-D sinusoidal embedding (width = time, height = frequency)
    const int half = D / 2;
    std::vector<float> div(size_t(half / 2));
    for (int i = 0; i < half / 2; ++i) div[size_t(i)] = float(std::exp(double(2 * i) * -(std::log(maxp) / double(half))));
    for (int t = 0; t < t1; ++t)
      for (int f = 0; f < fr; ++f) {
        float* row = &X[(size_t(t) * size_t(fr) + size_t(f)) * size_t(D)];
        for (int i = 0; i < half / 2; ++i) {
          const float aw = float(t) * div[size_t(i)], ah = float(f) * div[size_t(i)];
          row[2 * i] += wpe * std::sin(aw);
          row[2 * i + 1] += wpe * std::cos(aw);
          row[half + 2 * i] += wpe * std::sin(ah);
          row[half + 2 * i + 1] += wpe * std::cos(ah);
        }
      }
  }
  {  // 1-D sinusoidal embedding: [cos, sin]
    const int half = D / 2;
    for (int t = 0; t < t2; ++t) {
      float* row = &Xt[size_t(t) * size_t(D)];
      for (int i = 0; i < half; ++i) {
        const float ph = float(double(t) / std::pow(maxp, double(i) / double(half - 1)));
        row[i] += wpe * std::cos(ph);
        row[half + i] += wpe * std::sin(ph);
      }
    }
  }
  for (size_t i = 0; i < layers_.size(); ++i) {
    if (!layers_[i].cross) {
      runTLayer(pool, layers_[i], X, n, nullptr, 0);
      runTLayer(pool, layersT_[i], Xt, t2, nullptr, 0);
    } else {
      const std::vector<float> old = X;
      runTLayer(pool, layers_[i], X, n, &Xt, t2);
      runTLayer(pool, layersT_[i], Xt, t2, &old, n);
    }
  }
  for (int c = 0; c < D; ++c)
    for (int f = 0; f < fr; ++f)
      for (int t = 0; t < t1; ++t) x[(size_t(c) * size_t(fr) + size_t(f)) * size_t(t1) + size_t(t)] = X[(size_t(t) * size_t(fr) + size_t(f)) * size_t(D) + size_t(c)];
  for (int c = 0; c < D; ++c)
    for (int t = 0; t < t2; ++t) xt[size_t(c) * size_t(t2) + size_t(t)] = Xt[size_t(t) * size_t(D) + size_t(c)];
}

// ---------------------------------------------------------------- forward

std::vector<float> HTDemucs::forward(Pool& pool, const float* mixIn, int length) const {
  const int C = cfg_.audioChannels;
  const int S = int(cfg_.sources.size());
  const int Ls = cfg_.segmentLength();
  length = std::min(length, Ls);
  std::vector<float> mix(size_t(C) * size_t(Ls), 0.f);
  for (int c = 0; c < C; ++c) std::memcpy(&mix[size_t(c) * size_t(Ls)], mixIn + size_t(c) * size_t(length), sizeof(float) * size_t(length));

  Scope profF_("forward");
  int T = 0;
  const std::vector<std::complex<float>> z = demucsSpec(mix.data(), C, Ls, cfg_.nfft, T);
  const int F = cfg_.nfft / 2;
  // complex as channels: (c, re/im)
  std::vector<float> x(size_t(2 * C) * size_t(F) * size_t(T));
  for (int c = 0; c < C; ++c)
    for (size_t i = 0; i < size_t(F) * size_t(T); ++i) {
      x[size_t(2 * c) * size_t(F) * size_t(T) + i] = z[size_t(c) * size_t(F) * size_t(T) + i].real();
      x[size_t(2 * c + 1) * size_t(F) * size_t(T) + i] = z[size_t(c) * size_t(F) * size_t(T) + i].imag();
    }
  auto meanStd = [](const std::vector<float>& v, double& mean, double& sd) {
    double s = 0;
    for (float a : v) s += a;
    mean = s / double(v.size());
    double s2 = 0;
    for (float a : v) s2 += (a - mean) * (a - mean);
    sd = std::sqrt(s2 / double(v.size() > 1 ? v.size() - 1 : 1));  // unbiased, as torch.std
  };
  double mean = 0, sd = 0, meanT = 0, sdT = 0;
  meanStd(x, mean, sd);
  for (float& v : x) v = float((v - mean) / (1e-5 + sd));
  std::vector<float> xt = mix;
  meanStd(xt, meanT, sdT);
  for (float& v : xt) v = float((v - meanT) / (1e-5 + sdT));

  std::vector<std::vector<float>> saved, savedT;
  std::vector<int> lengthsT;
  int c = 2 * C, h = F, w = T;
  int ct = C, ht = 1, wt = Ls;
  for (int idx = 0; idx < cfg_.depth; ++idx) {
    lengthsT.push_back(wt);
    runEncoder(pool, tenc_[size_t(idx)], xt, ct, ht, wt);
    savedT.push_back(xt);
    runEncoder(pool, enc_[size_t(idx)], x, c, h, w);
    if (idx == 0 && !freqEmb_.empty()) {
      const int fr = std::min(h, freqEmbFreqs_);
      for (int ch = 0; ch < c; ++ch)
        for (int f = 0; f < fr; ++f) {
          const float e = freqEmb_[size_t(f) * size_t(c) + size_t(ch)];
          float* p = &x[(size_t(ch) * size_t(h) + size_t(f)) * size_t(w)];
          for (int t = 0; t < w; ++t) p[t] += e;
        }
    }
    saved.push_back(x);
  }
  if (!layers_.empty()) {
    int ho = 0, wo = 0;
    if (cfg_.bottomChannels > 0) {
      x = conv2d(pool, x.data(), c, 1, h * w, up_.w.data(), up_.b.data(), up_.cout, 1, 1, 1, 1, 0, 0, 1, 1, ho, wo);
      xt = conv2d(pool, xt.data(), ct, 1, wt, upT_.w.data(), upT_.b.data(), upT_.cout, 1, 1, 1, 1, 0, 0, 1, 1, ho, wo);
    }
    runTransformer(pool, x, h, w, xt, wt);
    if (cfg_.bottomChannels > 0) {
      x = conv2d(pool, x.data(), tDim_, 1, h * w, down_.w.data(), down_.b.data(), down_.cout, 1, 1, 1, 1, 0, 0, 1, 1, ho, wo);
      xt = conv2d(pool, xt.data(), tDim_, 1, wt, downT_.w.data(), downT_.b.data(), downT_.cout, 1, 1, 1, 1, 0, 0, 1, 1, ho, wo);
    }
  }
  for (int idx = 0; idx < cfg_.depth; ++idx) {
    runDecoder(pool, dec_[size_t(idx)], x, saved.back(), c, h, w, 0);
    saved.pop_back();
    const int lt = lengthsT.back();
    lengthsT.pop_back();
    runDecoder(pool, tdec_[size_t(idx)], xt, savedT.back(), ct, ht, wt, lt);
    savedT.pop_back();
  }
  // frequency branch: (S, C, re/im, F, T) -> complex spectrogram -> waveform
  std::vector<std::complex<float>> zo(size_t(S * C) * size_t(F) * size_t(T));
  const size_t plane = size_t(F) * size_t(T);
  for (int sc = 0; sc < S * C; ++sc)
    for (size_t i = 0; i < plane; ++i)
      zo[size_t(sc) * plane + i] = std::complex<float>(float(x[size_t(2 * sc) * plane + i] * sd + mean),
                                                       float(x[size_t(2 * sc + 1) * plane + i] * sd + mean));
  std::vector<float> wave(size_t(S * C) * size_t(Ls));
  {
    std::vector<std::vector<float>> parts(size_t(S * C));
    pool.run(S * C, [&](int sc) { parts[size_t(sc)] = demucsISpec(zo.data() + size_t(sc) * plane, 1, cfg_.nfft, T, Ls); });
    for (int sc = 0; sc < S * C; ++sc) std::memcpy(&wave[size_t(sc) * size_t(Ls)], parts[size_t(sc)].data(), sizeof(float) * size_t(Ls));
  }
  std::vector<float> out(size_t(S * C) * size_t(length));
  for (int sc = 0; sc < S * C; ++sc)
    for (int i = 0; i < length; ++i)
      out[size_t(sc) * size_t(length) + size_t(i)] =
          wave[size_t(sc) * size_t(Ls) + size_t(i)] + float(xt[size_t(sc) * size_t(Ls) + size_t(i)] * sdT + meanT);
  if (prof().on) prof().dump();
  return out;
}

}  // namespace gl::stems
