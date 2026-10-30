// Gridline CLI - J-191: `gridline stems`, the stem splitter from scripts.
//   gridline stems <audio> [--model ID] [--out DIR] [--stems vocals,drums,...] [--threads N] [--download]
//   gridline stems --list
//   gridline stems --download <ID>
//   gridline stems --import <model file>
//   gridline stems --remove <ID>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "engine/stems/StemModels.h"
#include "engine/stems/StemOps.h"
#include "host/net/Http.h"

namespace {

namespace st = gl::stems;

void stemsUsage() {
  std::fprintf(stderr,
               "usage:\n"
               "  gridline stems <audio> [--model ID] [--out DIR] [--stems vocals,drums,bass,other,guitar,piano]\n"
               "                 [--threads N] [--download]\n"
               "  gridline stems --list              models, their stems and whether they are downloaded\n"
               "  gridline stems --download <ID>     download a model from its authors' server\n"
               "  gridline stems --import <file>     install a model file you already have\n"
               "  gridline stems --remove <ID>       delete a downloaded model\n");
}

std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool download(const st::ModelInfo& m) {
  std::fprintf(stderr, "%s\n\n", st::modelLicenceNote());
  std::fprintf(stderr, "Downloading \"%s\" (%s, %.0f MB) from %s\n", m.title.c_str(), m.id.c_str(),
               double(st::modelDownloadBytes(m)) / 1e6, m.files.empty() ? "" : m.files[0].url.c_str());
  std::string err;
  int lastPct = -1;
  const bool ok = st::downloadModel(m, gl::http::download, [&](int64_t done, int64_t total) {
    const int pct = total > 0 ? int(100.0 * double(done) / double(total)) : 0;
    if (pct != lastPct) {
      lastPct = pct;
      std::fprintf(stderr, "\r  %3d%%", pct);
      std::fflush(stderr);
    }
  }, nullptr, &err);
  std::fprintf(stderr, "\n");
  if (!ok) std::fprintf(stderr, "download failed: %s\n", err.c_str());
  return ok;
}

}  // namespace

int stemsCliMain(int argc, char** argv) {
  if (argc < 3) {
    stemsUsage();
    return 2;
  }
  const std::string a2 = argv[2];
  if (a2 == "--list") {
    std::printf("models folder: %s\n", st::modelsDir().c_str());
    std::printf("matrix products: %s\n", st::gemmBackend());
    for (const st::ModelInfo& m : st::catalogue()) {
      std::string stems;
      for (const auto& s : m.sources) stems += (stems.empty() ? "" : ",") + s;
      int64_t bytes = 0;
      for (const auto& f : m.files) bytes += f.bytes;
      std::printf("%-12s %-13s %-10s %5.0f MB  %s%s\n", m.id.c_str(), m.title.c_str(),
                  st::modelInstalled(m) ? "installed" : "-", double(bytes) / 1e6, stems.c_str(),
                  &m == &st::defaultModel() ? "  (default)" : "");
    }
    return 0;
  }
  if (a2 == "--download" || a2 == "--remove") {
    if (argc < 4) {
      stemsUsage();
      return 2;
    }
    const st::ModelInfo* m = st::findModel(argv[3]);
    if (!m) {
      std::fprintf(stderr, "unknown model %s (see gridline stems --list)\n", argv[3]);
      return 2;
    }
    if (a2 == "--remove") {
      std::string err;
      if (!st::removeModel(*m, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
      }
      return 0;
    }
    return st::modelInstalled(*m) || download(*m) ? 0 : 1;
  }
  if (a2 == "--import") {
    if (argc < 4) {
      stemsUsage();
      return 2;
    }
    std::string id, err;
    if (!st::importModelFile(argv[3], &id, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    std::printf("installed a file of model %s\n", id.c_str());
    return 0;
  }
  st::ExtractRequest req;
  req.input = a2;
  req.modelId = st::defaultModel().id;
  bool allowDownload = false;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (a == "--model") req.modelId = next();
    else if (a == "--out" || a == "-o") req.outDir = next();
    else if (a == "--stems") req.keep = splitList(next());
    else if (a == "--threads") req.threads = std::atoi(next().c_str());
    else if (a == "--name") req.baseName = next();
    else if (a == "--download") allowDownload = true;
    else {
      std::fprintf(stderr, "unknown option %s\n", a.c_str());
      stemsUsage();
      return 2;
    }
  }
  const st::ModelInfo* m = st::findModel(req.modelId);
  if (!m) {
    std::fprintf(stderr, "unknown model %s (see gridline stems --list)\n", req.modelId.c_str());
    return 2;
  }
  for (const auto& k : req.keep)
    if (std::find(m->sources.begin(), m->sources.end(), k) == m->sources.end()) {
      std::fprintf(stderr, "model %s has no stem \"%s\"\n", m->id.c_str(), k.c_str());
      return 2;
    }
  if (!st::modelInstalled(*m)) {
    if (!allowDownload) {
      std::fprintf(stderr, "model %s is not downloaded; run `gridline stems --download %s` or add --download\n",
                   m->id.c_str(), m->id.c_str());
      return 1;
    }
    if (!download(*m)) return 1;
  }
  if (req.outDir.empty()) {
    const std::filesystem::path in = std::filesystem::u8path(req.input);
    req.outDir = (in.parent_path() / std::filesystem::u8path(in.stem().u8string() + " stems")).u8string();
  }
  const auto t0 = std::chrono::steady_clock::now();
  int lastPct = -1;
  std::vector<std::pair<std::string, std::string>> written;
  std::string err;
  const bool ok = st::extractStems(req, [&](double f) {
    const int pct = int(f * 100.0);
    if (pct != lastPct) {
      lastPct = pct;
      std::fprintf(stderr, "\r  separating %3d%%", pct);
      std::fflush(stderr);
    }
  }, nullptr, &written, &err);
  std::fprintf(stderr, "\n");
  if (!ok) {
    std::fprintf(stderr, "stems failed: %s\n", err.c_str());
    return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  for (const auto& w : written) std::printf("%s\t%s\n", w.first.c_str(), w.second.c_str());
  std::fprintf(stderr, "done in %.1f s (model %s, %s)\n", secs, m->id.c_str(), st::gemmBackend());
  return 0;
}
