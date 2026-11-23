// Gridline: the system audio names Gridline's streams (K-357 step zero). Headless: sets SDL hints
// only (no SDL_Init, no device, no window), then checks main.cpp applies them in the right order.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "Check.h"
#include "app/AudioNames.h"

namespace {

bool hintIs(const char* hint, const char* want) {
  const char* v = SDL_GetHint(hint);
  return v && std::strcmp(v, want) == 0;
}

std::string readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A function body in `src`: from `sig` to the next closing brace at the start of a line.
std::string body(const std::string& src, const std::string& sig) {
  const size_t a = src.find(sig);
  if (a == std::string::npos) return {};
  const size_t b = src.find("\n}\n", a);
  return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

size_t countOpens(const std::string& s) {
  size_t n = 0;
  for (size_t p = s.find("SDL_OpenAudioDevice("); p != std::string::npos; p = s.find("SDL_OpenAudioDevice(", p + 1)) ++n;
  return n;
}

}  // namespace

int main() {
  using namespace gl;
  // An SDL_AUDIO_DEVICE_* variable in the environment wins over the hints (by design); clear them here.
#ifdef _WIN32
  _putenv_s("SDL_AUDIO_DEVICE_APP_NAME", "");
  _putenv_s("SDL_AUDIO_DEVICE_STREAM_NAME", "");
  _putenv_s("SDL_APP_NAME", "");
#else
  unsetenv("SDL_AUDIO_DEVICE_APP_NAME");
  unsetenv("SDL_AUDIO_DEVICE_STREAM_NAME");
  unsetenv("SDL_APP_NAME");
#endif

  audionames::applyAppName();
#ifdef SDL_HINT_AUDIO_DEVICE_APP_NAME
  expect(hintIs(SDL_HINT_AUDIO_DEVICE_APP_NAME, "Gridline"), "audio app name is Gridline");
#endif
#ifdef SDL_HINT_APP_NAME
  expect(hintIs(SDL_HINT_APP_NAME, "Gridline"), "general app name is Gridline");
#endif
#ifdef SDL_HINT_AUDIO_DEVICE_STREAM_NAME
  audionames::applyStreamName(false);
  expect(hintIs(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Playback"), "output stream is named Playback");
  audionames::applyStreamName(true);
  expect(hintIs(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Recording"), "input stream is named Recording");
  audionames::applyStreamName(false);
  expect(hintIs(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Playback"), "reopening the output names it Playback again");
#endif

  // main.cpp: the app name before SDL_Init, a stream name before every device open.
  const std::string src = readFile("src/app/main.cpp");
  expect(!src.empty(), "main.cpp readable (run from the source dir)");
  const size_t app = src.find("audionames::applyAppName();");
  const size_t init = src.find("SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO");
  expect(app != std::string::npos && init != std::string::npos && app < init, "app name set before SDL_Init");
  const std::string out = body(src, "void openAudio(App& a)");
  const std::string in = body(src, "void openAudioInput(App& a");
  expect(!out.empty() && !in.empty(), "openAudio and openAudioInput found");
  const size_t outName = out.find("audionames::applyStreamName(false);");
  const size_t outOpen = out.find("SDL_OpenAudioDevice(");
  expect(outName != std::string::npos && outOpen != std::string::npos && outName < outOpen,
         "output stream named before the first output open");
  const size_t inName = in.find("audionames::applyStreamName(true);");
  const size_t inOpen = in.find("SDL_OpenAudioDevice(");
  expect(inName != std::string::npos && inOpen != std::string::npos && inName < inOpen,
         "input stream named before the input open");
  // Every open in main.cpp sits in one of those two functions: a new open site must name its stream.
  const size_t opens = countOpens(src);
  expect(opens > 0 && opens == countOpens(out) + countOpens(in), "no device opens outside the two named paths");
  return finish();
}
