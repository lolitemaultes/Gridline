// Gridline - J-191: numeric building blocks of the stem splitter: a small worker pool, one
// matrix-multiply routine everything heavy is lowered to, an FFT and element-wise helpers.
// Runs on background threads only (never the audio thread); allocations are fine here.
#pragma once
#include <atomic>
#include <complex>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace gl::stems {

// Fixed set of worker threads; run() splits n jobs over them and the calling thread and
// returns when all are done. Not re-entrant (one run() at a time per pool).
class Pool {
 public:
  explicit Pool(int threads);  // <= 0: hardware threads
  ~Pool();
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  int size() const { return int(workers_.size()) + 1; }
  void run(int n, const std::function<void(int)>& fn);

 private:
  void loop();
  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_, doneCv_;
  const std::function<void(int)>* fn_ = nullptr;
  int n_ = 0;
  std::atomic<int> next_{0};
  int busy_ = 0;
  unsigned gen_ = 0;
  bool quit_ = false;
};

// C = alpha * op(A) * op(B) + beta * C, row-major; op(X) = X or its transpose. M x N result,
// K inner. beta == 0 never reads C. Uses the system BLAS where Gridline has one (Accelerate on
// macOS) and Gridline's own blocked kernel elsewhere (or when GRIDLINE_STEMS_GEMM=own).
void gemm(Pool& pool, bool transA, bool transB, int M, int N, int K, float alpha, const float* A, int lda,
          const float* B, int ldb, float beta, float* C, int ldc);
// Gridline's own kernel (always available; tests compare the two).
void gemmOwn(Pool& pool, bool transA, bool transB, int M, int N, int K, float alpha, const float* A, int lda,
             const float* B, int ldb, float beta, float* C, int ldc);
// Single-threaded product for use inside Pool tasks.
void gemmSerial(bool transA, bool transB, int M, int N, int K, float alpha, const float* A, int lda, const float* B, int ldb,
                float beta, float* C, int ldc);
// Name of the kernel gemm() uses ("accelerate" / "own").
const char* gemmBackend();

// In-place complex FFT of a power-of-two size (forward: e^{-i...}); `inverse` does not scale.
class Fft {
 public:
  explicit Fft(int n);
  int size() const { return n_; }
  void run(std::complex<float>* x, bool inverse) const;

 private:
  int n_;
  std::vector<int> rev_;
  std::vector<std::complex<float>> tw_;
};

float gelu(float x);  // exact (erf) form, as PyTorch's default
void geluInPlace(float* x, size_t n);
float sigmoid(float x);

}  // namespace gl::stems
