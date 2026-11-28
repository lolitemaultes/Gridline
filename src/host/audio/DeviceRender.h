// Gridline: shared realtime render callback signature for non-interleaved audio device backends
// (ASIO on Windows, K-6; JACK / PipeWire on Linux, K-357).
#pragma once

namespace gl::audio {

// Called on the audio driver's realtime thread for every buffer period:
//   `in[k]`  = engine input channel k (`frames` floats, non-interleaved), for 0 <= k < numIn;
//   `out[k]` = device output channel k (`frames` floats, non-interleaved), for 0 <= k < numOut.
// Must remain strictly realtime-safe: no heap allocations, no locks, no I/O.
using RenderFn = void (*)(void* user, const float* const* in, int numIn, float* const* out, int numOut, int frames);

}  // namespace gl::audio
