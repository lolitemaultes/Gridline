// Gridline — flush-to-zero for the threads that run DSP (J-25). Denormal
// floats (filter and reverb tails decaying towards silence) cost ~100x per
// operation on x86; FTZ/DAZ turns them into zeros. Results differ from IEEE
// only below ~1e-38 (inaudible); every DSP thread sets the same mode, so
// renders stay identical across thread counts and block sizes.
#pragma once
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#define GL_DENORMALS_X86 1
#endif

namespace gl {

// Sets FTZ + DAZ on the calling thread (cheap; idempotent).
inline void flushDenormals() {
#if defined(GL_DENORMALS_X86)
  const unsigned csr = _mm_getcsr();
  const unsigned want = csr | 0x8040u;  // FTZ (bit 15) | DAZ (bit 6)
  if (csr != want) _mm_setcsr(want);
#elif defined(__aarch64__)
  unsigned long fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  if (!(fpcr & (1ul << 24))) __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (1ul << 24)));  // FZ
#endif
}

// Restores the caller's mode on scope exit (offline renders on a UI thread).
class ScopedFlushDenormals {
 public:
  ScopedFlushDenormals() {
#if defined(GL_DENORMALS_X86)
    saved_ = _mm_getcsr();
#elif defined(__aarch64__)
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved_));
#endif
    flushDenormals();
  }
  ~ScopedFlushDenormals() {
#if defined(GL_DENORMALS_X86)
    _mm_setcsr(static_cast<unsigned>(saved_));
#elif defined(__aarch64__)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(saved_));
#endif
  }
  ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
  ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

 private:
  unsigned long saved_ = 0;
};

}  // namespace gl
