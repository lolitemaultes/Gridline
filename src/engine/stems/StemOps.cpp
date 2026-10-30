// Gridline - J-191: stem splitter building blocks (see StemOps.h).
#include "StemOps.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#define GL_STEMS_HAVE_ACCELERATE 1
#endif

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
#include <immintrin.h>
#define GL_STEMS_X86_AVX2 1
#endif

namespace gl::stems {

// ---------------------------------------------------------------- pool

Pool::Pool(int threads) {
  if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));
  threads = std::min(threads, 64);
  for (int t = 1; t < threads; ++t) workers_.emplace_back([this] { loop(); });
}

Pool::~Pool() {
  {
    std::lock_guard<std::mutex> lk(m_);
    quit_ = true;
  }
  cv_.notify_all();
  for (auto& w : workers_) w.join();
}

void Pool::loop() {
  unsigned seen = 0;
  for (;;) {
    const std::function<void(int)>* fn;
    int n;
    {
      std::unique_lock<std::mutex> lk(m_);
      cv_.wait(lk, [&] { return quit_ || gen_ != seen; });
      if (quit_) return;
      seen = gen_;
      fn = fn_;
      n = n_;
    }
    for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) (*fn)(i);
    {
      std::lock_guard<std::mutex> lk(m_);
      if (--busy_ == 0) doneCv_.notify_all();
    }
  }
}

void Pool::run(int n, const std::function<void(int)>& fn) {
  if (n <= 0) return;
  if (workers_.empty() || n == 1) {
    for (int i = 0; i < n; ++i) fn(i);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    fn_ = &fn;
    n_ = n;
    next_.store(0);
    busy_ = int(workers_.size());
    ++gen_;
  }
  cv_.notify_all();
  for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) fn(i);
  std::unique_lock<std::mutex> lk(m_);
  doneCv_.wait(lk, [&] { return busy_ == 0; });
}

// ---------------------------------------------------------------- gemm

namespace {

constexpr int kNR = 16;
constexpr int kKC = 256;
constexpr int kMC = 96;   // multiple of 4 and 6
constexpr int kNC = 256;  // multiple of kNR

// acc[MR][16] = sum_k a[k][i] * b[k][j]
void kernelGeneric4(int kc, const float* a, const float* b, float* acc) {
  float c[4][kNR];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < kNR; ++j) c[i][j] = 0.f;
  for (int k = 0; k < kc; ++k) {
    const float* bk = b + k * kNR;
    const float* ak = a + k * 4;
    for (int i = 0; i < 4; ++i) {
      const float ai = ak[i];
      for (int j = 0; j < kNR; ++j) c[i][j] += ai * bk[j];
    }
  }
  std::memcpy(acc, c, sizeof(c));
}

#if defined(GL_STEMS_X86_AVX2)
__attribute__((target("avx2,fma"))) void kernelAvx2_6(int kc, const float* a, const float* b, float* acc) {
  __m256 c00 = _mm256_setzero_ps(), c01 = _mm256_setzero_ps();
  __m256 c10 = _mm256_setzero_ps(), c11 = _mm256_setzero_ps();
  __m256 c20 = _mm256_setzero_ps(), c21 = _mm256_setzero_ps();
  __m256 c30 = _mm256_setzero_ps(), c31 = _mm256_setzero_ps();
  __m256 c40 = _mm256_setzero_ps(), c41 = _mm256_setzero_ps();
  __m256 c50 = _mm256_setzero_ps(), c51 = _mm256_setzero_ps();
  for (int k = 0; k < kc; ++k) {
    const __m256 b0 = _mm256_loadu_ps(b + k * kNR);
    const __m256 b1 = _mm256_loadu_ps(b + k * kNR + 8);
    const float* ak = a + k * 6;
    __m256 x = _mm256_broadcast_ss(ak + 0);
    c00 = _mm256_fmadd_ps(x, b0, c00);
    c01 = _mm256_fmadd_ps(x, b1, c01);
    x = _mm256_broadcast_ss(ak + 1);
    c10 = _mm256_fmadd_ps(x, b0, c10);
    c11 = _mm256_fmadd_ps(x, b1, c11);
    x = _mm256_broadcast_ss(ak + 2);
    c20 = _mm256_fmadd_ps(x, b0, c20);
    c21 = _mm256_fmadd_ps(x, b1, c21);
    x = _mm256_broadcast_ss(ak + 3);
    c30 = _mm256_fmadd_ps(x, b0, c30);
    c31 = _mm256_fmadd_ps(x, b1, c31);
    x = _mm256_broadcast_ss(ak + 4);
    c40 = _mm256_fmadd_ps(x, b0, c40);
    c41 = _mm256_fmadd_ps(x, b1, c41);
    x = _mm256_broadcast_ss(ak + 5);
    c50 = _mm256_fmadd_ps(x, b0, c50);
    c51 = _mm256_fmadd_ps(x, b1, c51);
  }
  _mm256_storeu_ps(acc + 0, c00);
  _mm256_storeu_ps(acc + 8, c01);
  _mm256_storeu_ps(acc + 16, c10);
  _mm256_storeu_ps(acc + 24, c11);
  _mm256_storeu_ps(acc + 32, c20);
  _mm256_storeu_ps(acc + 40, c21);
  _mm256_storeu_ps(acc + 48, c30);
  _mm256_storeu_ps(acc + 56, c31);
  _mm256_storeu_ps(acc + 64, c40);
  _mm256_storeu_ps(acc + 72, c41);
  _mm256_storeu_ps(acc + 80, c50);
  _mm256_storeu_ps(acc + 88, c51);
}

bool cpuHasAvx2() {
  static const bool has = [] {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  }();
  return has;
}
#endif

using Kernel = void (*)(int, const float*, const float*, float*);

struct KernelChoice {
  Kernel fn;
  int mr;
};

KernelChoice pickKernel() {
#if defined(GL_STEMS_X86_AVX2)
  if (cpuHasAvx2()) return {kernelAvx2_6, 6};
#endif
  return {kernelGeneric4, 4};
}

}  // namespace

void gemmOwn(Pool& pool, bool transA, bool transB, int M, int N, int K, float alpha, const float* A, int lda,
             const float* B, int ldb, float beta, float* C, int ldc) {
  if (M <= 0 || N <= 0) return;
  if (K <= 0) {
    for (int i = 0; i < M; ++i)
      for (int j = 0; j < N; ++j) C[size_t(i) * size_t(ldc) + size_t(j)] = beta == 0.f ? 0.f : beta * C[size_t(i) * size_t(ldc) + size_t(j)];
    return;
  }
  const KernelChoice kc = pickKernel();
  const int MR = kc.mr;
  const int mBlocks = (M + kMC - 1) / kMC, nBlocks = (N + kNC - 1) / kNC;
  pool.run(mBlocks * nBlocks, [&](int task) {
    const int mb = task / nBlocks, nb = task % nBlocks;
    const int i0 = mb * kMC, mc = std::min(kMC, M - i0);
    const int j0 = nb * kNC, nc = std::min(kNC, N - j0);
    const int mPanels = (mc + MR - 1) / MR, nPanels = (nc + kNR - 1) / kNR;
    std::vector<float> pa(size_t(mPanels) * size_t(MR) * kKC), pb(size_t(nPanels) * kNR * kKC);
    float acc[6 * kNR];
    for (int p0 = 0; p0 < K; p0 += kKC) {
      const int kcur = std::min(kKC, K - p0);
      // pack B: panels of kNR columns, k-major
      for (int q = 0; q < nPanels; ++q) {
        float* dst = &pb[size_t(q) * kNR * kKC];
        for (int k = 0; k < kcur; ++k)
          for (int j = 0; j < kNR; ++j) {
            const int col = j0 + q * kNR + j;
            float v = 0.f;
            if (col < j0 + nc)
              v = transB ? B[size_t(col) * size_t(ldb) + size_t(p0 + k)] : B[size_t(p0 + k) * size_t(ldb) + size_t(col)];
            dst[k * kNR + j] = v;
          }
      }
      // pack A: panels of MR rows, k-major
      for (int q = 0; q < mPanels; ++q) {
        float* dst = &pa[size_t(q) * size_t(MR) * kKC];
        for (int k = 0; k < kcur; ++k)
          for (int i = 0; i < MR; ++i) {
            const int row = i0 + q * MR + i;
            float v = 0.f;
            if (row < i0 + mc)
              v = transA ? A[size_t(p0 + k) * size_t(lda) + size_t(row)] : A[size_t(row) * size_t(lda) + size_t(p0 + k)];
            dst[k * MR + i] = v;
          }
      }
      const bool first = p0 == 0;
      for (int qm = 0; qm < mPanels; ++qm)
        for (int qn = 0; qn < nPanels; ++qn) {
          kc.fn(kcur, &pa[size_t(qm) * size_t(MR) * kKC], &pb[size_t(qn) * kNR * kKC], acc);
          const int rows = std::min(MR, mc - qm * MR), cols = std::min(kNR, nc - qn * kNR);
          for (int i = 0; i < rows; ++i) {
            float* c = C + size_t(i0 + qm * MR + i) * size_t(ldc) + size_t(j0 + qn * kNR);
            const float* s = acc + i * kNR;
            if (!first) {
              for (int j = 0; j < cols; ++j) c[j] += alpha * s[j];
            } else if (beta == 0.f) {
              for (int j = 0; j < cols; ++j) c[j] = alpha * s[j];
            } else {
              for (int j = 0; j < cols; ++j) c[j] = alpha * s[j] + beta * c[j];
            }
          }
        }
    }
  });
}

#if defined(GL_STEMS_HAVE_ACCELERATE)
namespace {
bool forceOwn() {
  static const bool own = [] {
    const char* e = std::getenv("GRIDLINE_STEMS_GEMM");
    return e && std::string(e) == "own";
  }();
  return own;
}
}  // namespace
#endif

const char* gemmBackend() {
#if defined(GL_STEMS_HAVE_ACCELERATE)
  if (!forceOwn()) return "accelerate";
#endif
#if defined(GL_STEMS_X86_AVX2)
  if (cpuHasAvx2()) return "own-avx2";
#endif
  return "own";
}

void gemm(Pool& pool, bool transA, bool transB, int M, int N, int K, float alpha, const float* A, int lda,
          const float* B, int ldb, float beta, float* C, int ldc) {
#if defined(GL_STEMS_HAVE_ACCELERATE)
  if (!forceOwn()) {
    if (M <= 0 || N <= 0) return;
    // The classic CBLAS entry point: still shipped, works on every macOS Gridline supports (the
    // newer interface needs macOS 13.3).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    cblas_sgemm(CblasRowMajor, transA ? CblasTrans : CblasNoTrans, transB ? CblasTrans : CblasNoTrans, M, N, K, alpha, A,
                lda, B, ldb, beta, C, ldc);
#pragma clang diagnostic pop
    return;
  }
#endif
  gemmOwn(pool, transA, transB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
}

void gemmSerial(bool transA, bool transB, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb,
                float beta, float* C, int ldc) {
#if defined(GL_STEMS_HAVE_ACCELERATE) && defined(__aarch64__)
  // Apple silicon: Accelerate's matrix units beat any portable kernel even on one thread. Intel
  // Macs: Gridline's AVX2 kernel (Accelerate threads internally and oversubscribes the pool).
  if (!forceOwn()) {
    if (M <= 0 || N <= 0) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    cblas_sgemm(CblasRowMajor, transA ? CblasTrans : CblasNoTrans, transB ? CblasTrans : CblasNoTrans, M, N, K, alpha, A,
                lda, B, ldb, beta, C, ldc);
#pragma clang diagnostic pop
    return;
  }
#endif
  Pool serial(1);
  gemmOwn(serial, transA, transB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
}

// ---------------------------------------------------------------- fft

Fft::Fft(int n) : n_(n), rev_(size_t(n)), tw_(size_t(n / 2)) {
  int bits = 0;
  while ((1 << bits) < n) ++bits;
  for (int i = 0; i < n; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b)
      if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[size_t(i)] = r;
  }
  const double pi = 3.14159265358979323846;
  for (int k = 0; k < n / 2; ++k) {
    const double a = -2.0 * pi * double(k) / double(n);
    tw_[size_t(k)] = std::complex<float>(float(std::cos(a)), float(std::sin(a)));
  }
}

void Fft::run(std::complex<float>* x, bool inverse) const {
  const int n = n_;
  for (int i = 0; i < n; ++i)
    if (i < rev_[size_t(i)]) std::swap(x[i], x[rev_[size_t(i)]]);
  for (int len = 2; len <= n; len <<= 1) {
    const int half = len >> 1, step = n / len;
    for (int i = 0; i < n; i += len)
      for (int k = 0; k < half; ++k) {
        std::complex<float> w = tw_[size_t(k * step)];
        if (inverse) w = std::conj(w);
        const std::complex<float> u = x[i + k];
        const std::complex<float> v = x[i + k + half] * w;
        x[i + k] = u + v;
        x[i + k + half] = u - v;
      }
  }
}

// ---------------------------------------------------------------- element-wise

float gelu(float x) { return 0.5f * x * (1.f + std::erf(x * 0.70710678118654752f)); }

void geluInPlace(float* x, size_t n) {
  for (size_t i = 0; i < n; ++i) x[i] = gelu(x[i]);
}

float sigmoid(float x) { return 1.f / (1.f + std::exp(-x)); }

}  // namespace gl::stems
