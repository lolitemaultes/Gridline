// Gridline CLI — `gridline bench`: load / save / render timings of a project (J-25).
//   gridline bench <project.json|.flp> [--threads 1,4] [--repeat 5] [--render-seconds S] [--json out.json]
// Prints medians of every stage plus a hash of the rendered audio (bit-exact checks
// of optimisations: equal hashes = identical output).
#include "../core/Platform.h"
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "core/Json.h"
#include "core/Project.h"
#include "core/Undo.h"
#include "core/Document.h"
#include "core/flp/Flp.h"
#include "engine/Render.h"
#include "engine/Sample.h"

namespace {

double nowSec() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
double cpuSec() { return gl::platform::cpuSeconds(); }  // whole process (all threads)
double peakRssMb() { return gl::platform::peakRssMb(); }
// Bytes currently allocated on the heap (0 where unsupported).
size_t heapInUse() {
#if defined(__APPLE__)
  malloc_statistics_t st{};
  malloc_zone_statistics(nullptr, &st);
  return st.size_in_use;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  return mallinfo2().uordblks;
#else
  return 0;
#endif
}
double median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}
uint64_t hashAudio(const gl::AudioBuffer& b) {  // FNV-1a over the float bits
  uint64_t h = 1469598103934665603ull;
  for (float v : b.data) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    for (int k = 0; k < 4; ++k) h = (h ^ ((u >> (8 * k)) & 0xFF)) * 1099511628211ull;
  }
  return h;
}

struct Stage {
  const char* name;
  std::vector<double> wall, cpu;
};

}  // namespace

int benchMain(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: gridline bench <project.json|.flp> [--threads 1,4] [--repeat N] [--render-seconds S] [--json out]\n");
    return 2;
  }
  std::string path = argv[2], jsonOut;
  std::vector<int> threads{1};
  int repeat = 5;
  double renderSeconds = 0;  // 0 = whole song
  bool noRender = false;
  for (int i = 3; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) {
      threads.clear();
      std::stringstream ss(argv[++i]);
      for (std::string t; std::getline(ss, t, ',');) threads.push_back(std::max(1, std::atoi(t.c_str())));
    } else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) {
      repeat = std::max(1, std::atoi(argv[++i]));
    } else if (!std::strcmp(argv[i], "--render-seconds") && i + 1 < argc) {
      renderSeconds = std::atof(argv[++i]);
    } else if (!std::strcmp(argv[i], "--no-render")) {
      noRender = true;
    } else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) {
      jsonOut = argv[++i];
    }
  }
  const bool isFlp = path.size() > 4 && (path.compare(path.size() - 4, 4, ".flp") == 0 || path.compare(path.size() - 4, 4, ".FLP") == 0);
  std::vector<Stage> stages;
  auto stage = [&](const char* name) -> Stage& {
    for (auto& s : stages)
      if (!std::strcmp(s.name, name)) return s;
    stages.push_back({name, {}, {}});
    return stages.back();
  };
  auto timed = [&](const char* name, auto&& fn) {
    double w0 = nowSec(), c0 = cpuSec();
    fn();
    Stage& s = stage(name);
    s.wall.push_back((nowSec() - w0) * 1000.0);
    s.cpu.push_back((cpuSec() - c0) * 1000.0);
  };

  gl::Song song;
  std::string err, text;
  size_t jsonBytes = 0;
  for (int r = 0; r < repeat; ++r) {
    gl::Song s;
    bool ok = true;
    if (isFlp) {
      timed("import_flp", [&] {
        gl::flp::Report rep;
        ok = gl::importFlp(path, s, rep, &err);
      });
    } else {
      std::string raw;
      timed("read_file", [&] {
        std::ifstream is(path, std::ios::binary);
        std::stringstream ss;
        ss << is.rdbuf();
        raw = ss.str();
      });
      gl::Json j;
      timed("json_parse", [&] {
        size_t pos = 0;
        ok = gl::jsonParse(raw, j, &pos);
      });
      if (ok) timed("song_from_json", [&] { ok = gl::songFromJson(j, s, &err); });
    }
    if (!ok) {
      std::fprintf(stderr, "error: %s\n", err.empty() ? "cannot load" : err.c_str());
      return 1;
    }
    timed("save_serialize", [&] { text = gl::projectToText(s); });
    jsonBytes = text.size();
    if (r == 0) song = std::move(s);
  }

  // Undo memory: heap growth of 20 whole-song edits (snapshots) and of 20
  // channel edits on this song; and of one copy of the song for scale.
  long long undoSongBytes = 0, undoChannelBytes = 0, songBytes = 0;  // signed: allocator caches can shrink
  {
    size_t h0 = heapInUse();
    gl::Song copy = song;
    songBytes = static_cast<long long>(heapInUse()) - static_cast<long long>(h0);
    gl::Document doc(std::move(copy));
    h0 = heapInUse();
    for (int k = 0; k < 20; ++k) doc.editSong("bench edit", [k](gl::Song& x) { x.tempo = 100.0 + k; });
    undoSongBytes = static_cast<long long>(heapInUse()) - static_cast<long long>(h0);
    h0 = heapInUse();
    if (!song.channels.empty())
      for (int k = 0; k < 20; ++k)
        doc.editChannel(song.channels[0].id, "bench edit", [k](gl::Channel& c) { c.gain = 0.01f * static_cast<float>(k + 1); });
    undoChannelBytes = static_cast<long long>(heapInUse()) - static_cast<long long>(h0);
  }

  // Samples: first decode (cold cache) of everything the song references.
  // Samples: first decode (cold cache) of every channel sample, as program
  // building does it (parallel prefetch).
  timed("sample_decode_cold", [&] {
    std::vector<std::string> paths;
    for (const auto& c : song.channels)
      if (!c.plugin.data.empty() && gl::isAudioFile(c.plugin.data)) paths.push_back(c.plugin.data);
    gl::sampleCache().prefetch(paths);
  });

  struct RenderRow {
    int threads;
    std::vector<double> wall, cpu;
    uint64_t hash = 0;
    double seconds = 0;
    bool stable = true;
  };
  std::vector<RenderRow> renders;
  if (!noRender) {
    gl::Song rs = song;
    if (renderSeconds > 0) {  // cut the arrangement to the first S seconds (at the start tempo)
      const gl::Tick limit = static_cast<gl::Tick>(renderSeconds * rs.tempo / 60.0 * rs.ppq);
      rs.playlist.erase(std::remove_if(rs.playlist.begin(), rs.playlist.end(), [&](const gl::Clip& c) { return c.start >= limit; }),
                        rs.playlist.end());
      for (auto& c : rs.playlist)
        if (c.start + rs.clipLength(c) > limit) c.length = limit - c.start;
    }
    for (int t : threads) {
      RenderRow row{t, {}, {}, 0, 0, true};
      for (int r = 0; r < repeat; ++r) {
        gl::RenderOptions opt;
        opt.threads = t;
        opt.tail = rs.ppq * 2;
        double w0 = nowSec(), c0 = cpuSec();
        gl::AudioBuffer buf = gl::renderSong(rs, opt);
        row.wall.push_back((nowSec() - w0) * 1000.0);
        row.cpu.push_back((cpuSec() - c0) * 1000.0);
        uint64_t h = hashAudio(buf);
        if (r == 0) row.hash = h;
        else if (h != row.hash) row.stable = false;
        row.seconds = static_cast<double>(buf.frames) / opt.sampleRate;
      }
      renders.push_back(row);
    }
  }

  std::string out = "{\"project\": \"";
  for (char c : path) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "\", \"channels\": %zu, \"patterns\": %zu, \"clips\": %zu, \"automations\": %zu, \"mixer\": %zu, \"json_bytes\": %zu, "
                "\"song_heap_bytes\": %lld, \"undo_20_song_edits_bytes\": %lld, \"undo_20_channel_edits_bytes\": %lld, "
                "\"peak_rss_mb\": %.1f, \"repeat\": %d",
                song.channels.size(), song.patterns.size(), song.playlist.size(), song.automations.size(), song.mixer.size(), jsonBytes,
                songBytes, undoSongBytes, undoChannelBytes, peakRssMb(), repeat);
  out += buf;
  for (const auto& s : stages) {
    std::snprintf(buf, sizeof buf, ", \"%s_ms\": %.3f, \"%s_cpu_ms\": %.3f", s.name, median(s.wall), s.name, median(s.cpu));
    out += buf;
  }
  out += ", \"render\": [";
  for (size_t i = 0; i < renders.size(); ++i) {
    const auto& r = renders[i];
    std::snprintf(buf, sizeof buf,
                  "%s{\"threads\": %d, \"wall_ms\": %.2f, \"cpu_ms\": %.2f, \"audio_s\": %.3f, \"x_realtime\": %.2f, \"hash\": \"%016llx\", "
                  "\"deterministic\": %s}",
                  i ? ", " : "", r.threads, median(r.wall), median(r.cpu), r.seconds,
                  median(r.wall) > 0 ? r.seconds * 1000.0 / median(r.wall) : 0.0, static_cast<unsigned long long>(r.hash),
                  r.stable ? "true" : "false");
    out += buf;
  }
  out += "]}\n";
  std::fputs(out.c_str(), stdout);
  if (!jsonOut.empty()) {
    std::ofstream os(jsonOut, std::ios::app);
    os << out;
  }
  return 0;
}
