// Gridline - J-192 tests: a stand-in for yt-dlp and ffmpeg, so Import from URL is tested without
// any network or the real tools. One program for every platform (no shell scripts).
//   yt-dlp mode (argument --ignore-config):
//     -J            prints the info JSON (single item, or a playlist)
//     otherwise     "downloads": writes <-o template with %(ext)s = webm> in 10 steps, printing
//                   "GLPROG <got> <total> NA NA" progress lines
//   ffmpeg mode (argument -i): writes a 32-bit float WAV (2 s of 440 Hz, or -t seconds) to the
//                   last argument, prints "Duration:" on stderr and -progress lines on stdout
//   --version / -version: a version line.
// GL_FAKE_SCENARIO picks a case: ok (default), playlist, error, drm, signin, slow, live, noaudio,
// convertfail, stall, slowdl (only the download is slow). GL_FAKE_LOG appends every argument list to a file.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string env(const char* n) {
  const char* v = std::getenv(n);
  return v ? v : "";
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void put32(std::ofstream& f, uint32_t v) {
  for (int i = 0; i < 4; ++i) f.put(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put16(std::ofstream& f, uint16_t v) {
  f.put(static_cast<char>(v & 0xFF));
  f.put(static_cast<char>(v >> 8));
}

bool writeWav(const std::string& path, double seconds) {
  const uint32_t sr = 44100, ch = 2;
  const uint32_t frames = static_cast<uint32_t>(seconds * sr);
  const uint32_t data = frames * ch * 4;
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write("RIFF", 4);
  put32(f, 36 + data);
  f.write("WAVEfmt ", 8);
  put32(f, 16);
  put16(f, 3);  // IEEE float
  put16(f, static_cast<uint16_t>(ch));
  put32(f, sr);
  put32(f, sr * ch * 4);
  put16(f, static_cast<uint16_t>(ch * 4));
  put16(f, 32);
  f.write("data", 4);
  put32(f, data);
  for (uint32_t i = 0; i < frames; ++i) {
    const float v = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 440.0 * i / sr));
    for (uint32_t c = 0; c < ch; ++c) f.write(reinterpret_cast<const char*>(&v), 4);
  }
  return static_cast<bool>(f);
}

std::string after(const std::vector<std::string>& a, const char* key) {
  for (size_t i = 0; i + 1 < a.size(); ++i)
    if (a[i] == key) return a[i + 1];
  return "";
}
bool has(const std::vector<std::string>& a, const char* key) {
  for (const auto& x : a)
    if (x == key) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);
  const std::string sc = env("GL_FAKE_SCENARIO").empty() ? "ok" : env("GL_FAKE_SCENARIO");
  if (const std::string log = env("GL_FAKE_LOG"); !log.empty()) {
    std::ofstream f(log, std::ios::app);
    for (const auto& x : a) f << "[" << x << "]";
    f << "\n";
  }
  if (has(a, "--version")) {
    std::printf("2099.01.01\n");
    return 0;
  }
  if (has(a, "-version")) {
    std::printf("ffmpeg version fake-1 Copyright (c) the fake\n");
    return 0;
  }
  if (has(a, "--ignore-config")) {  // yt-dlp
    const std::string url = a.empty() ? "" : a.back();
    if (has(a, "-J")) {
      if (sc == "slow") sleepMs(30000);
      if (sc == "error") {
        std::fprintf(stderr, "WARNING: something minor\nERROR: [generic] Unsupported URL: %s\n", url.c_str());
        return 1;
      }
      if (sc == "drm") {
        std::fprintf(stderr, "ERROR: [vendor] x1: This video is DRM protected\n");
        return 1;
      }
      if (sc == "live") {
        std::printf("{\"id\": \"l1\", \"title\": \"Live now\", \"is_live\": true, \"extractor_key\": \"Youtube\", \"webpage_url\": \"%s\"}\n",
                    url.c_str());
        return 0;
      }
      if (sc == "playlist") {
        std::printf(
            "{\"_type\": \"playlist\", \"id\": \"PL1\", \"title\": \"Fake Playlist\", \"uploader\": \"List Owner\", "
            "\"extractor_key\": \"YoutubeTab\", \"webpage_url\": \"%s\", \"entries\": ["
            "{\"_type\": \"url\", \"id\": \"a1\", \"url\": \"https://example.com/watch?v=a1\", \"title\": \"First Song\", \"duration\": 2.0},"
            "{\"_type\": \"url\", \"id\": \"a2\", \"url\": \"https://example.com/watch?v=a2\", \"title\": \"Second Song\", \"duration\": 2.0, "
            "\"channel\": \"Other Channel\"},"
            "{\"_type\": \"url\", \"id\": \"a3\", \"url\": \"https://example.com/watch?v=a3\", \"title\": \"Third / Song: Live?\"}]}\n",
            url.c_str());
        return 0;
      }
      std::printf(
          "{\"id\": \"abc123\", \"title\": \"Fake Song: Take/One\", \"uploader\": \"Test Uploader\", \"extractor_key\": \"Youtube\", "
          "\"extractor\": \"youtube\", \"duration\": 2.0, \"webpage_url\": \"%s\", \"formats\": [{\"format_id\": \"251\"}]}\n",
          url.c_str());
      return 0;
    }
    // download
    if (sc == "signin") {
      std::fprintf(stderr, "ERROR: [youtube] abc123: Sign in to confirm your age. This video may be inappropriate for some users.\n");
      return 1;
    }
    std::string out = after(a, "-o");
    const size_t at = out.find("%(ext)s");
    if (at != std::string::npos) out.replace(at, 7, "webm");
    const int steps = 10, total = 10 * 4096;
    std::ofstream part(out + ".part", std::ios::binary);
    for (int i = 1; i <= steps; ++i) {
      std::string chunk(4096, static_cast<char>('a' + i));
      part.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
      part.flush();
      std::printf("[download] GLPROG %d %d NA NA\n", i * 4096, total);
      std::fflush(stdout);
      if (sc == "stall" && i == 3) sleepMs(60000);
      sleepMs(sc == "slow" || sc == "slowdl" ? 1000 : 20);
    }
    part.close();
    if (sc == "noaudio") {
      std::remove((out + ".part").c_str());
      return 0;
    }
    std::rename((out + ".part").c_str(), out.c_str());
    return 0;
  }
  if (has(a, "-i")) {  // ffmpeg
    const std::string in = after(a, "-i");
    std::ifstream probe(in, std::ios::binary);
    if (!probe) {
      std::fprintf(stderr, "%s: No such file or directory\n", in.c_str());
      return 1;
    }
    if (sc == "convertfail") {
      std::fprintf(stderr, "Input #0, matroska,webm, from '%s':\n  Duration: 00:00:02.00, start: 0.000000\n", in.c_str());
      std::fprintf(stderr, "Stream map '0:a:0' matches no streams.\n");
      return 1;
    }
    std::fprintf(stderr, "Input #0, matroska,webm, from '%s':\n  Duration: 00:00:02.00, start: 0.000000, bitrate: 1 kb/s\n", in.c_str());
    double secs = 2.0;
    if (const std::string t = after(a, "-t"); !t.empty()) secs = std::min(secs, std::atof(t.c_str()));
    std::printf("out_time_us=%lld\nprogress=continue\n", static_cast<long long>(secs * 500000));
    std::fflush(stdout);
    if (!writeWav(a.back(), secs)) return 1;
    std::printf("out_time_us=%lld\nout_time=00:00:%09.6f\nprogress=end\n", static_cast<long long>(secs * 1000000), secs);
    return 0;
  }
  std::fprintf(stderr, "fake tool: unknown arguments\n");
  return 2;
}
