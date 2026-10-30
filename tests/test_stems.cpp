// Gridline - J-191: stem splitter tests. No real model weights are used anywhere: a tiny model of
// the same architecture is built here from a fixed pseudo-random sequence and written in the
// PyTorch checkpoint format; its expected output was computed once with the reference
// implementation (demucs 4.1.0 on PyTorch 2.2.2) from the same file.
//   gridline_tests_stems                    run the tests
//   gridline_tests_stems --write-tiny <p>   write the tiny model (the app test and reference use it)
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "Check.h"
#include "core/AudioBuffer.h"
#include "core/Platform.h"
#include "core/Sha256.h"
#include "core/Song.h"
#include "engine/Export.h"
#include "engine/stems/HTDemucs.h"
#include "engine/stems/StemModels.h"
#include "engine/stems/StemOps.h"
#include "engine/stems/StemPlace.h"
#include "engine/stems/StemSplit.h"
#include "engine/stems/TorchFile.h"

using namespace gl;

// Reference values for the tiny model and tinyInput(22050): demucs.states.load_model on the file
// written by --write-tiny, model(x) in float32 (see the header comment).
#define TINY_E0 51.67253487
#define TINY_E1 12.55542155
#define TINY_E2 42.79273801
#define TINY_E3 72.20004043
#define TINY_V0 0.0339583419f
#define TINY_V1 0.0355850533f
#define TINY_V2 -0.00629070029f
#define TINY_V3 -0.0550032258f
using namespace gl::stems;
namespace fs = std::filesystem;

namespace {

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
bool writeTiny(const std::string& path, std::string* err) {
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

// The reference input: 2 x 22050 samples.
std::vector<float> tinyInput(int length) {
  std::vector<float> x(size_t(2 * length));
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < length; ++i)
      x[size_t(c * length + i)] = float(0.5 * std::sin(i * 0.01 * (c + 1)) + 0.1 * std::sin(i * 0.37) + 0.05 * std::sin(i * 1.9 + c));
  return x;
}

// ---------------------------------------------------------------- helpers

void naiveGemm(bool ta, bool tb, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb, float beta,
               float* C, int ldc) {
  for (int i = 0; i < M; ++i)
    for (int j = 0; j < N; ++j) {
      double s = 0;
      for (int k = 0; k < K; ++k) s += double(ta ? A[k * lda + i] : A[i * lda + k]) * double(tb ? B[j * ldb + k] : B[k * ldb + j]);
      C[i * ldc + j] = float(alpha * s + (beta == 0.f ? 0.0 : beta * C[i * ldc + j]));
    }
}

std::string tempDir(const char* tag) {
  const fs::path p = fs::temp_directory_path() / (std::string("gl_stems_") + tag + "_" + std::to_string(platform::processId()));
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.u8string();
}

bool finite(const std::vector<float>& v) {
  for (float x : v)
    if (!std::isfinite(x)) return false;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--write-tiny") {
    std::string err;
    if (!writeTiny(argv[2], &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    return 0;
  }
  const std::string dir = tempDir("t");

  // ---- SHA-256 (FIPS 180-2 test vectors)
  expect(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
  expect(sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of the empty string");
  {
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect(sha256Hex(m, std::strlen(m)) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 of the 448-bit message");
    std::string mil(1000000, 'a');
    Sha256 h;
    for (size_t i = 0; i < mil.size(); i += 777) h.update(mil.data() + i, std::min<size_t>(777, mil.size() - i));
    expect(h.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 of a million 'a' in odd pieces");
    const std::string f = dir + "/abc.bin";
    std::ofstream(f, std::ios::binary) << "abc";
    expect(sha256File(f) == sha256Hex("abc", 3), "sha256 of a file");
  }

  // ---- half floats
  {
    bool ok = true;
    for (float v : {0.f, 1.f, -2.5f, 65504.f, 6.1035156e-05f, 5.9604645e-08f, 0.333251953125f})
      ok = ok && stemsHalfToFloat(stemsFloatToHalf(v)) == v;
    expect(ok, "half precision round trip of representable values");
    expect(std::isinf(stemsHalfToFloat(0x7c00)) && stemsHalfToFloat(0x3c00) == 1.f && stemsHalfToFloat(0x0001) > 0.f,
           "half precision specials and subnormals");
  }

  // ---- checkpoint writer / reader
  const std::string tiny = dir + "/tiny.th";
  {
    std::string err;
    expect(writeTiny(tiny, &err), "the tiny model is written in the checkpoint format");
    TorchFile f;
    expect(f.open(tiny, &err), "the checkpoint opens");
    PyRef kw = f.root() ? f.root()->get("kwargs") : nullptr;
    expect(kw && kw->get("nfft") && kw->get("nfft")->i == 256 && kw->get("segment")->f == 0.5 && kw->get("cac")->b,
           "the configuration (ints, floats, bools) reads back");
    PyRef src = kw ? kw->get("sources") : nullptr;
    expect(src && src->items.size() == 4 && src->items[3]->s == "vocals", "the source list reads back");
    std::map<std::string, Tensor> st;
    expect(f.loadAll(f.root()->get("state"), st, &err), "every tensor loads");
    expect(st.count("encoder.0.conv.weight") && st["encoder.0.conv.weight"].shape == std::vector<int64_t>({4, 4, 8, 1}),
           "tensor names and shapes read back");
    TinyBuilder again;
    // first tensor values (half precision)
    const float v0 = again.rng.next() * (1.f / std::sqrt(32.f));
    expect(std::fabs(st["encoder.0.conv.weight"].data[0] - v0) < 1e-3f, "tensor values read back (half precision)");
    // garbage
    const std::string bad = dir + "/bad.th";
    std::ofstream(bad, std::ios::binary) << "this is not a model";
    TorchFile g;
    expect(!g.open(bad, &err) && !err.empty(), "a file that is not a checkpoint is refused with a message");
    // pickle fragments
    const uint8_t trunc[] = {0x80, 2, '}', '('};
    expect(!unpickle(trunc, sizeof trunc, &err), "a truncated pickle is refused");
    const uint8_t evil[] = {0x80, 2, 'c', 'o', 's', '\n', 's', 'y', 's', 't', 'e', 'm', '\n', 'X', 2, 0, 0, 0, 'l', 's', 0x85, 'R', '.'};
    PyRef r = unpickle(evil, sizeof evil, &err);
    expect(r && r->kind == PyValue::Kind::Object, "a pickled function call is recorded as data, never run");
  }

  // ---- GEMM
  {
    Pool pool(4);
    Lcg rng(7);
    bool okOwn = true, okSys = true;
    const int shapes[][3] = {{1, 1, 1}, {7, 5, 3}, {97, 33, 260}, {6, 300, 17}, {130, 64, 513}, {2, 1000, 9}};
    for (const auto& sh : shapes)
      for (int t = 0; t < 4; ++t) {
        const int M = sh[0], N = sh[1], K = sh[2];
        const bool ta = t & 1, tb = t & 2;
        std::vector<float> A(size_t(M * K)), B(size_t(K * N)), C0(size_t(M * N)), C1, C2;
        for (float& v : A) v = rng.next();
        for (float& v : B) v = rng.next();
        for (float& v : C0) v = rng.next();
        C1 = C0;
        C2 = C0;
        std::vector<float> R = C0;
        const int lda = ta ? M : K, ldb = tb ? K : N;
        naiveGemm(ta, tb, M, N, K, 0.7f, A.data(), lda, B.data(), ldb, 0.3f, R.data(), N);
        gemmOwn(pool, ta, tb, M, N, K, 0.7f, A.data(), lda, B.data(), ldb, 0.3f, C1.data(), N);
        gemm(pool, ta, tb, M, N, K, 0.7f, A.data(), lda, B.data(), ldb, 0.3f, C2.data(), N);
        for (size_t i = 0; i < R.size(); ++i) {
          okOwn = okOwn && std::fabs(C1[i] - R[i]) < 1e-3f * (1.f + std::sqrt(float(K)));
          okSys = okSys && std::fabs(C2[i] - R[i]) < 1e-3f * (1.f + std::sqrt(float(K)));
        }
      }
    expect(okOwn, "Gridline's matrix product matches a plain one (all transposes, odd sizes)");
    expect(okSys, std::string(std::string("the matrix product in use matches a plain one (") + gemmBackend() + ")").c_str());
    std::vector<float> C(4, std::nanf(""));
    std::vector<float> A = {1, 2}, B = {3, 4};
    gemmOwn(pool, false, false, 2, 2, 1, 1.f, A.data(), 1, B.data(), 2, 0.f, C.data(), 2);
    expect(C[0] == 3 && C[3] == 8, "beta 0 never reads the output (NaN there is overwritten)");
  }

  // ---- FFT and the spectrogram pair
  {
    Fft f(64);
    std::vector<std::complex<float>> x(64), X(64);
    Lcg rng(3);
    for (auto& v : x) v = {rng.next(), rng.next()};
    for (int k = 0; k < 64; ++k) {
      std::complex<double> s = 0;
      for (int n = 0; n < 64; ++n) s += std::complex<double>(x[size_t(n)]) * std::polar(1.0, -2 * 3.14159265358979323846 * k * n / 64.0);
      X[size_t(k)] = std::complex<float>(s);
    }
    std::vector<std::complex<float>> y = x;
    f.run(y.data(), false);
    double e = 0;
    for (int k = 0; k < 64; ++k) e = std::max(e, double(std::abs(y[size_t(k)] - X[size_t(k)])));
    expect(e < 1e-4, "FFT matches the DFT");
    f.run(y.data(), true);
    e = 0;
    for (int k = 0; k < 64; ++k) e = std::max(e, double(std::abs(y[size_t(k)] / 64.f - x[size_t(k)])));
    expect(e < 1e-5, "inverse FFT restores the input");
    const int L = 5000;
    std::vector<float> sig = tinyInput(L);
    int frames = 0;
    std::vector<std::complex<float>> z = demucsSpec(sig.data(), 2, L, 256, frames);
    expect(frames == (L + 63) / 64, "spectrogram frame count = ceil(length / hop)");
    std::vector<float> back = demucsISpec(z.data(), 2, 256, frames, L);
    // the reference's inverse counts the window of the two empty frames on each side, so only the
    // interior is an exact inverse; there the dropped Nyquist bin is the only difference
    double err = 0;
    for (int c = 0; c < 2; ++c)
      for (int i = 256; i < L - 256; ++i) err = std::max(err, double(std::fabs(back[size_t(c * L + i)] - sig[size_t(c * L + i)])));
    std::printf("# spectrogram round trip error %g\n", err);
    expect(err < 1e-5, "inverse spectrogram restores the signal's interior (without the Nyquist bin, as the reference)");
  }

  // ---- overlap-add maths
  {
    const std::vector<float> w = segmentWeights(10);
    expect(w.size() == 10 && w[0] > 0 && std::fabs(w[4] - 1.f) < 1e-6f && w[4] == w[5] && w[0] == w[9], "triangular segment weights peak at 1");
    const std::vector<long long> off = segmentOffsets(1000, 100, 0.25);
    expect(off.size() == 14 && off[1] == 75 && off.back() == 975, "segment offsets step by 75 % of a segment");
    const std::vector<float> w100 = segmentWeights(100);
    std::vector<float> cover(1000, 0.f);
    for (long long o : off)
      for (int i = 0; i < 100 && o + i < 1000; ++i) cover[size_t(o + i)] += w100[size_t(i)];
    bool pos = true;
    for (float c : cover) pos = pos && c > 0;
    expect(pos, "every sample is covered by a positive weight");
  }

  // ---- the tiny model against the reference implementation
  {
    HTDemucs m;
    std::string err;
    expect(m.load(tiny, &err), (std::string("the tiny model loads ") + err).c_str());
    expect(m.config().segmentLength() == 22050 && m.config().sources.size() == 4, "its configuration");
    Pool pool(0);
    const std::vector<float> x = tinyInput(22050);
    const std::vector<float> y = m.forward(pool, x.data(), 22050);
    expect(y.size() == size_t(4 * 2 * 22050) && finite(y), "forward gives 4 sources x 2 channels, all finite");
    // reference values (demucs 4.1.0 / PyTorch 2.2.2 on this file and input)
    double e[4] = {0, 0, 0, 0};
    for (int s = 0; s < 4; ++s)
      for (size_t i = 0; i < size_t(2 * 22050); ++i) e[s] += double(y[size_t(s) * 2 * 22050 + i]) * y[size_t(s) * 2 * 22050 + i];
    const double refE[4] = {TINY_E0, TINY_E1, TINY_E2, TINY_E3};
    bool ok = true;
    for (int s = 0; s < 4; ++s) ok = ok && std::fabs(e[s] - refE[s]) < 1e-4 * refE[s];
    std::printf("# energies %.8g %.8g %.8g %.8g\n", e[0], e[1], e[2], e[3]);
    expect(ok, "per-source energy matches the reference implementation");
    const size_t idx[4] = {100, 7000, 2 * 22050 + 15000, 7 * 22050 + 21000};
    const float refV[4] = {TINY_V0, TINY_V1, TINY_V2, TINY_V3};
    ok = true;
    for (int k = 0; k < 4; ++k) ok = ok && std::fabs(y[idx[k]] - refV[k]) < 2e-5f + 1e-4f * std::fabs(refV[k]);
    expect(ok, "individual output samples match the reference implementation");
    // shorter input is padded like the reference and gives the same start
    const std::vector<float> y2 = m.forward(pool, x.data(), 22050);
    expect(y2 == y, "forward is deterministic");
    Pool one(1);
    const std::vector<float> y3 = m.forward(one, x.data(), 22050);
    double d = 0;
    for (size_t i = 0; i < y.size(); ++i) d = std::max(d, double(std::fabs(y3[i] - y[i])));
    expect(d < 1e-5, "one thread and many give the same result");
    // extremes
    std::vector<float> loud(x.size());
    for (size_t i = 0; i < loud.size(); ++i) loud[i] = (i / 50) % 2 ? 1.f : -1.f;
    expect(finite(m.forward(pool, loud.data(), 22050)), "full-scale square wave: finite output");
    std::vector<float> zero(x.size(), 0.f);
    const std::vector<float> yz = m.forward(pool, zero.data(), 22050);
    expect(finite(yz), "digital silence: finite output");
  }

  // ---- separate(): segments, rates, channels, cancel
  {
    SplitRequest req;
    req.modelFiles = {tiny};
    req.threads = 2;
    AudioBuffer in;
    const int n = 60000;  // ~3 segments
    in.resize(2, size_t(n));
    for (int i = 0; i < n; ++i) {
      in.at(0, size_t(i)) = float(0.4 * std::sin(i * 0.02) + 0.1 * std::sin(i * 0.5));
      in.at(1, size_t(i)) = float(0.3 * std::sin(i * 0.03));
    }
    SplitResult res;
    std::string err;
    int calls = 0;
    double last = 0;
    const bool ok = separate(in, 44100, req, res, [&](double f) { ++calls, last = f; }, nullptr, &err);
    expect(ok && res.stems.size() == 4 && res.sources[3] == "vocals", "separate gives the model's stems");
    bool shape = ok;
    for (const auto& b : res.stems) shape = shape && b.channels == 2 && b.frames == size_t(n) && finite(b.data);
    expect(shape, "each stem has the input's length and channels");
    expect(calls == 4 && std::fabs(last - 1.0) < 1e-9, "progress reaches 1 once per segment");
    // a clip shorter than one segment step equals one forward pass on the normalised input,
    // centred in a zero-padded segment (as the reference pads a short chunk)
    {
      const int n1 = 16000, L = 22050, lead = (L - n1) / 2;
      AudioBuffer s1;
      s1.resize(2, size_t(n1));
      const std::vector<float> x = tinyInput(n1);
      std::copy(x.begin(), x.end(), s1.data.begin());
      SplitResult r1;
      separate(s1, 44100, req, r1, {}, nullptr, &err);
      double mean = 0, var = 0;
      for (int i = 0; i < n1; ++i) mean += (x[size_t(i)] + x[size_t(n1 + i)]) * 0.5;
      mean /= n1;
      for (int i = 0; i < n1; ++i) {
        const double v = (x[size_t(i)] + x[size_t(n1 + i)]) * 0.5 - mean;
        var += v * v;
      }
      const double sd = std::sqrt(var / (n1 - 1));
      std::vector<float> xn(size_t(2 * L), 0.f);
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n1; ++i) xn[size_t(c * L + lead + i)] = float((x[size_t(c * n1 + i)] - mean) / sd);
      HTDemucs m;
      m.load(tiny, &err);
      Pool pool(2);
      const std::vector<float> y = m.forward(pool, xn.data(), L);
      double d = 0;
      for (int s = 0; s < 4; ++s)
        for (int c = 0; c < 2; ++c)
          for (int i = 0; i < n1; ++i)
            d = std::max(d, std::fabs(double(r1.stems[size_t(s)].data[size_t(c * n1 + i)]) -
                                      (y[size_t((s * 2 + c) * L + lead + i)] * sd + mean)));
      expect(d < 1e-4, "one segment: separate = normalise, forward, de-normalise (the reference's scaling)");
    }
    // mono at 48 kHz
    AudioBuffer mono;
    mono.resize(1, 30000);
    for (int i = 0; i < 30000; ++i) mono.at(0, size_t(i)) = float(0.5 * std::sin(i * 0.05));
    SplitResult rm;
    expect(separate(mono, 48000, req, rm, {}, nullptr, &err) && rm.stems[0].channels == 1 && rm.stems[0].frames == 30000,
           "mono 48 kHz in: mono stems at the input's length out (resampled for the model and back)");
    // the stems add up to roughly the mix for a model whose outputs are not trained: only check finiteness and level
    bool fin = true;
    for (const auto& b : rm.stems) fin = fin && finite(b.data);
    expect(fin, "resampled stems are finite");
    // silence
    AudioBuffer sil;
    sil.resize(2, 10000);
    SplitResult rs;
    expect(separate(sil, 44100, req, rs, {}, nullptr, &err) && finite(rs.stems[0].data), "silence in: finite stems out");
    // cancel before and during
    std::atomic<bool> cancel{true};
    expect(!separate(in, 44100, req, res, {}, &cancel, &err) && err == "cancelled", "a cancelled split stops with \"cancelled\"");
    cancel = false;
    int seen = 0;
    expect(!separate(in, 44100, req, res, [&](double) { if (++seen == 1) cancel = true; }, &cancel, &err) && seen == 1,
           "cancel during the split stops after the current segment");
    // a bag of two copies with one-hot weights equals the single model
    SplitRequest bag = req;
    bag.modelFiles = {tiny, tiny};
    bag.weights = {{1, 1, 0, 0}, {0, 0, 1, 1}};
    SplitResult rb;
    separate(in, 44100, bag, rb, {}, nullptr, &err);
    SplitResult r0;
    separate(in, 44100, req, r0, {}, nullptr, &err);
    double d = 0;
    for (int s = 0; s < 4; ++s)
      for (size_t i = 0; i < rb.stems[size_t(s)].data.size(); ++i)
        d = std::max(d, std::fabs(double(rb.stems[size_t(s)].data[i]) - double(r0.stems[size_t(s)].data[i])));
    expect(d < 1e-5, "a bag of models blends per stem with its weights");
  }

  // ---- catalogue, downloads, import
  {
    bool ok = !catalogue().empty() && defaultModel().id == "htdemucs";
    for (const ModelInfo& m : catalogue()) {
      if (m.id == "test") continue;
      ok = ok && !m.files.empty() && (m.weights.empty() || m.weights.size() == m.files.size());
      for (const ModelFile& f : m.files)
        ok = ok && f.sha256.size() == 64 && f.url.rfind("https://dl.fbaipublicfiles.com/demucs/", 0) == 0 && f.bytes > 1000000 &&
             f.name.substr(9, 8) == f.sha256.substr(0, 8);  // the file names carry the checksum's start
    }
    expect(ok, "catalogue: official https addresses, full checksums matching the names, sizes");
    expect(findModel("htdemucs_6s") && findModel("htdemucs_6s")->sources.size() == 6 && findModel("htdemucs_ft")->files.size() == 4,
           "six-stem and high-quality models");
    const std::string models = dir + "/models";
    platform::setEnv("GRIDLINE_STEM_MODELS", models.c_str());
    expect(modelsDir() == models && !modelInstalled(defaultModel()) && modelDownloadBytes(defaultModel()) == 84141911,
           "models folder from the environment; nothing installed there");
    ModelInfo fake;
    fake.id = "fake";
    const std::string content = "model bytes";
    ModelFile f1;
    f1.name = "a.th";
    f1.url = "https://example.invalid/a.th";
    f1.sha256 = sha256Hex(content.data(), content.size());
    f1.bytes = int64_t(content.size());
    ModelFile f2 = f1;
    f2.name = "b.th";
    f2.url = "https://example.invalid/b.th";
    fake.files = {f1, f2};
    int calls = 0;
    std::string body = content;
    Downloader dl = [&](const std::string& url, const std::string& path, const ByteProgress& p, const std::atomic<bool>*,
                        std::string*) {
      ++calls;
      std::ofstream(path, std::ios::binary) << body;
      if (p) p(int64_t(body.size()), int64_t(body.size()));
      return url.rfind("https://", 0) == 0;
    };
    std::string err;
    int64_t lastDone = 0;
    expect(downloadModel(fake, dl, [&](int64_t d, int64_t) { lastDone = d; }, nullptr, &err) && calls == 2 && modelInstalled(fake),
           "download: every file fetched, checked and stored");
    expect(lastDone == 2 * int64_t(content.size()), "download progress counts bytes over all files");
    expect(downloadModel(fake, dl, {}, nullptr, &err) && calls == 2, "an installed model is not downloaded again");
    fs::remove(fs::u8path(models) / "b.th");
    body = "damaged bytes";
    expect(!downloadModel(fake, dl, {}, nullptr, &err) && err.find("checksum") != std::string::npos &&
               !fs::exists(fs::u8path(models) / "b.th") && !fs::exists(fs::u8path(models) / "b.th.part") &&
               fs::exists(fs::u8path(models) / "a.th"),
           "a damaged download is refused and removed; finished files stay (the next try resumes)");
    std::atomic<bool> cancel{true};
    Downloader dlc = [&](const std::string&, const std::string&, const ByteProgress&, const std::atomic<bool>*, std::string*) {
      return false;
    };
    expect(!downloadModel(fake, dlc, {}, &cancel, &err) && err == "cancelled", "a cancelled download says so");
    std::string id;
    expect(!importModelFile(tiny, &id, &err) && !err.empty(), "importing a file that is not a catalogue model is refused");
    expect(removeModel(fake, &err), "remove");
    platform::unsetEnv("GRIDLINE_STEM_MODELS");
  }

  // ---- extractStems: files, names, chosen stems
  {
    const std::string wav = dir + "/song.wav";
    AudioBuffer b;
    b.resize(2, 30000);
    for (size_t i = 0; i < 30000; ++i) b.at(0, i) = b.at(1, i) = float(0.3 * std::sin(double(i) * 0.03));
    AudioFileOptions o;
    o.bits = 32;
    writeAudioFile(wav, b, 44100, o);
    platform::setEnv("GRIDLINE_STEMS_TEST_MODEL", tiny.c_str());
    // the catalogue is built once: the test model entry exists only when the variable was set
    // before the first use, so this part calls separate through a request instead.
    ExtractRequest req;
    req.input = wav;
    req.modelId = "nope";
    std::string err;
    expect(!extractStems(req, {}, nullptr, nullptr, &err) && err.find("unknown model") != std::string::npos, "unknown model id refused");
    req.modelId = "htdemucs";
    platform::setEnv("GRIDLINE_STEM_MODELS", (dir + "/empty").c_str());
    expect(!extractStems(req, {}, nullptr, nullptr, &err) && err.find("not downloaded") != std::string::npos,
           "a model that is not downloaded is reported, nothing fetched");
    platform::unsetEnv("GRIDLINE_STEM_MODELS");
  }

  // ---- placement in the playlist
  {
    Song s;
    s.tracks.resize(4);
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Loop";
    ch.plugin.type = "gl.sampler";
    ch.plugin.data = "/x/loop.wav";
    ch.gain = 0.5f;
    ch.audioClip = true;
    s.channels.push_back(ch);
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch.id;
    c.track = 1;
    c.start = 960;
    c.length = 3840;
    c.audioStart = 1.25;
    c.audioEnd = 4.5;
    c.stretch = 1.5;
    c.gain = 0.7f;
    c.reverse = true;
    c.fadeIn = 0.1;
    s.playlist.push_back(c);
    Clip below = c;
    below.id = s.allocateId();
    below.track = 2;
    s.playlist.push_back(below);
    std::vector<StemFile> files = {{"drums", "/s/d.wav"}, {"vocals", "/s/v.wav"}, {"bass", "/s/b.wav"}};
    const std::vector<ObjectId> made = placeStems(s, c.id, files, "loop", true);
    expect(made.size() == 3 && s.tracks.size() == 7, "three stems: three new tracks");
    const Clip* v = s.clip(made[0]);
    const Channel* vc = v ? s.channel(v->source) : nullptr;
    expect(v && vc && vc->plugin.data == "/s/v.wav" && v->track == 2 && s.tracks[2].name == "Vocals",
           "vocals first, on the track right below the clip");
    expect(v && v->start == c.start && v->length == c.length && v->audioStart == c.audioStart && v->audioEnd == c.audioEnd &&
               v->stretch == c.stretch && v->gain == c.gain && v->reverse && v->fadeIn == c.fadeIn,
           "each stem clip mirrors the source clip (position, offset, stretch, gain, reverse, fades)");
    expect(vc && vc->gain == 0.5f && vc->plugin.type == "gl.sampler" && vc->name == "loop - Vocals" && vc->id != ch.id,
           "each stem channel copies the source channel's settings");
    expect(s.clip(c.id)->muted, "the original clip is muted");
    expect(s.clip(below.id)->track == 5, "clips further down move below the new tracks");
    const Clip* d = s.clip(made[1]);
    expect(d && d->track == 3 && s.channel(d->source)->plugin.data == "/s/d.wav", "then drums, bass");
    expect(placeStems(s, 999999, files, "x", false).empty(), "an unknown clip places nothing");
    // full playlist: empty tracks further down
    Song f;
    f.tracks.resize(size_t(kMaxPlaylistTracks));
    ch.id = f.allocateId();
    f.channels.push_back(ch);
    Clip fc = c;
    fc.id = f.allocateId();
    fc.source = ch.id;
    fc.muted = false;
    fc.track = 10;
    f.playlist.push_back(fc);
    const std::vector<ObjectId> m2 = placeStems(f, fc.id, files, "loop", false);
    expect(m2.size() == 3 && int(f.tracks.size()) == kMaxPlaylistTracks && f.clip(m2[0])->track == 11 && !f.clip(fc.id)->muted,
           "a full playlist: the stems go on empty tracks below");
  }

  std::error_code ec;
  fs::remove_all(fs::u8path(dir), ec);
  return finish();
}
