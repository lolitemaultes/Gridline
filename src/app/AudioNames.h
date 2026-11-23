// Gridline: the names the system audio shows for Gridline's streams (K-357 step zero).
// PipeWire and PulseAudio (through SDL) list an application name and a stream name in their
// patchbays and volume controls; without these hints SDL reports its generic name. The app name
// is read when the audio subsystem starts, so applyAppName() runs before SDL_Init; the stream
// name is read when a device opens, so applyStreamName() runs right before each
// SDL_OpenAudioDevice. Other backends (Core Audio, WASAPI, the dummy driver) ignore them.
// Hints set here have normal priority: an SDL_AUDIO_DEVICE_* environment variable still wins.
#pragma once

#include <SDL.h>

namespace gl::audionames {

inline constexpr const char* kApp = "Gridline";
inline constexpr const char* kPlayback = "Playback";
inline constexpr const char* kRecording = "Recording";

inline void applyAppName() {
#ifdef SDL_HINT_APP_NAME
  SDL_SetHint(SDL_HINT_APP_NAME, kApp);  // SDL 2.0.18+: the general fallback name
#endif
#ifdef SDL_HINT_AUDIO_DEVICE_APP_NAME
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, kApp);
#endif
}

inline void applyStreamName(bool capture) {
#ifdef SDL_HINT_AUDIO_DEVICE_STREAM_NAME
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, capture ? kRecording : kPlayback);
#else
  (void)capture;
#endif
}

}  // namespace gl::audionames
