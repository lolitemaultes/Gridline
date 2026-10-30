// Gridline - J-191: stem-splitter model catalogue, installation and the file-level job.
#include "StemModels.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include "../../core/Platform.h"
#include "../../core/Sha256.h"
#include "../Export.h"
#include "../Sample.h"

namespace gl::stems {

namespace fs = std::filesystem;

namespace {

bool fail(std::string* err, const std::string& msg) {
  if (err) *err = msg;
  return false;
}

const char* kHost = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/";

ModelFile official(const char* name, const char* sha, int64_t bytes) {
  ModelFile f;
  f.name = name;
  f.url = std::string(kHost) + name;
  f.sha256 = sha;
  f.bytes = bytes;
  return f;
}

std::vector<ModelInfo> build() {
  std::vector<ModelInfo> v;
  const std::vector<std::string> four = {"drums", "bass", "other", "vocals"};
  {
    ModelInfo m;
    m.id = "htdemucs";
    m.title = "Standard";
    m.summary = "Vocals, drums, bass and other. Good quality, the fastest.";
    m.sources = four;
    m.files = {official("955717e8-8726e21a.th", "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4", 84141911)};
    v.push_back(m);
  }
  {
    ModelInfo m;
    m.id = "htdemucs_ft";
    m.title = "High quality";
    m.summary = "Vocals, drums, bass and other with one tuned model per stem. About four times slower.";
    m.sources = four;
    m.files = {official("f7e0c4bc-ba3fe64a.th", "ba3fe64ae8ef66ac9a4857222ce48efbdc5eb3ad375cb79dd13debee5aaa4066", 84141271),
               official("d12395a8-e57c48e6.th", "e57c48e6b0e38af4f7118d7bd08c49f0a0c0edf7d09143bdd902ea0d237303e6", 84141271),
               official("92cfc3b6-ef3bcb9c.th", "ef3bcb9c8b40d14ae5d51b6db2587339cc12c6b77c0be151ce6d69002e087bf2", 84141271),
               official("04573f0d-f3cf25b2.th", "f3cf25b222c4eed7cd49dd8b2c9597d50c18bd154090f7b919cfa5f93cf22c49", 84141271)};
    m.weights = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    m.relativeTime = 4.0;
    v.push_back(m);
  }
  {
    ModelInfo m;
    m.id = "htdemucs_6s";
    m.title = "Six stems";
    m.summary = "Adds guitar and piano to vocals, drums, bass and other. Piano separation is rough.";
    m.sources = {"drums", "bass", "other", "vocals", "guitar", "piano"};
    m.files = {official("5c90dfd2-34c22ccb.th", "34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd", 54996327)};
    v.push_back(m);
  }
  // Tests and development: a small model file of the same family, used as is.
  if (const char* t = std::getenv("GRIDLINE_STEMS_TEST_MODEL"); t && *t) {
    ModelInfo m;
    m.id = "test";
    m.title = "Test model";
    m.summary = "A tiny model for automated tests.";
    m.sources = four;
    if (const char* s = std::getenv("GRIDLINE_STEMS_TEST_SOURCES"); s && *s) {
      m.sources.clear();
      std::string cur;
      for (const char* p = s;; ++p) {
        if (*p == ',' || *p == 0) {
          if (!cur.empty()) m.sources.push_back(cur);
          cur.clear();
          if (*p == 0) break;
        } else {
          cur.push_back(*p);
        }
      }
    }
    ModelFile f;
    f.name = fs::u8path(t).filename().u8string();
    f.path = t;
    m.files = {f};
    m.relativeTime = 0.01;
    v.push_back(m);
  }
  return v;
}

}  // namespace

const std::vector<ModelInfo>& catalogue() {
  static const std::vector<ModelInfo> v = build();
  return v;
}

const ModelInfo* findModel(const std::string& id) {
  for (const ModelInfo& m : catalogue())
    if (m.id == id) return &m;
  return nullptr;
}

const ModelInfo& defaultModel() { return catalogue().front(); }

std::string modelsDir() {
  if (const char* e = std::getenv("GRIDLINE_STEM_MODELS"); e && *e) return e;
  return (fs::u8path(platform::dataRoot()) / "gridline" / "stem-models").u8string();
}

std::string modelFilePath(const ModelFile& f) {
  if (!f.path.empty()) return f.path;
  return (fs::u8path(modelsDir()) / fs::u8path(f.name)).u8string();
}

bool modelInstalled(const ModelInfo& m) {
  for (const ModelFile& f : m.files) {
    std::error_code ec;
    const fs::path p = fs::u8path(modelFilePath(f));
    if (!fs::is_regular_file(p, ec)) return false;
    if (f.bytes > 0 && int64_t(fs::file_size(p, ec)) != f.bytes) return false;
  }
  return true;
}

int64_t modelDownloadBytes(const ModelInfo& m) {
  int64_t n = 0;
  for (const ModelFile& f : m.files) {
    std::error_code ec;
    const fs::path p = fs::u8path(modelFilePath(f));
    if (!fs::is_regular_file(p, ec) || (f.bytes > 0 && int64_t(fs::file_size(p, ec)) != f.bytes)) n += f.bytes;
  }
  return n;
}

const char* modelLicenceNote() {
  return "The stem models are Hybrid Transformer Demucs by Meta AI Research. Their code is MIT-licensed; the trained "
         "model files are provided by Meta for research use and are not part of Gridline. They are downloaded from "
         "Meta's own server (dl.fbaipublicfiles.com) only when you ask, checked against a checksum, and kept on this "
         "computer. Check that your use of the results suits your purpose.";
}

bool downloadModel(const ModelInfo& m, const Downloader& dl, const ByteProgress& progress, const std::atomic<bool>* cancel,
                   std::string* err) {
  std::error_code ec;
  fs::create_directories(fs::u8path(modelsDir()), ec);
  const int64_t total = modelDownloadBytes(m);
  int64_t done = 0;
  for (const ModelFile& f : m.files) {
    const fs::path dst = fs::u8path(modelFilePath(f));
    if (fs::is_regular_file(dst, ec) && (f.bytes <= 0 || int64_t(fs::file_size(dst, ec)) == f.bytes)) continue;
    if (f.url.rfind("https://", 0) != 0) return fail(err, "model " + m.id + " has no download address");
    const fs::path part = fs::u8path(dst.u8string() + ".part");
    const int64_t base = done;
    const bool ok = dl(f.url, part.u8string(), [&](int64_t got, int64_t) {
      if (progress) progress(base + got, total);
    }, cancel, err);
    if (!ok) {
      fs::remove(part, ec);
      if (cancel && cancel->load()) return fail(err, "cancelled");
      return false;
    }
    if (!f.sha256.empty() && sha256File(part.u8string()) != f.sha256) {
      fs::remove(part, ec);
      return fail(err, "the downloaded file " + f.name + " is damaged (checksum mismatch); try again");
    }
    fs::rename(part, dst, ec);
    if (ec) return fail(err, "cannot store " + dst.u8string() + ": " + ec.message());
    done += f.bytes;
    if (progress) progress(done, total);
  }
  return true;
}

bool importModelFile(const std::string& path, std::string* installed, std::string* err) {
  std::error_code ec;
  const fs::path src = fs::u8path(path);
  if (!fs::is_regular_file(src, ec)) return fail(err, "not a file: " + path);
  const int64_t size = int64_t(fs::file_size(src, ec));
  const ModelFile* match = nullptr;
  const ModelInfo* owner = nullptr;
  for (const ModelInfo& m : catalogue())
    for (const ModelFile& f : m.files)
      if (!f.sha256.empty() && f.bytes == size) {
        match = &f;
        owner = &m;
      }
  if (!match) return fail(err, "this file is not one of the supported model files");
  const std::string sum = sha256File(path);
  match = nullptr;
  for (const ModelInfo& m : catalogue())
    for (const ModelFile& f : m.files)
      if (f.sha256 == sum) {
        match = &f;
        owner = &m;
      }
  if (!match) return fail(err, "this file is not one of the supported model files (checksum)");
  fs::create_directories(fs::u8path(modelsDir()), ec);
  const fs::path dst = fs::u8path(modelFilePath(*match));
  const fs::path tmp = fs::u8path(dst.u8string() + ".part");
  fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
  if (!ec) fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return fail(err, "cannot copy the model file: " + ec.message());
  }
  if (installed) *installed = owner->id;
  return true;
}

bool removeModel(const ModelInfo& m, std::string* err) {
  for (const ModelFile& f : m.files) {
    if (!f.path.empty()) continue;  // test models are not Gridline's to delete
    // files shared with another model stay
    bool shared = false;
    for (const ModelInfo& o : catalogue())
      if (o.id != m.id && modelInstalled(o))
        for (const ModelFile& g : o.files) shared = shared || g.name == f.name;
    if (shared) continue;
    std::error_code ec;
    fs::remove(fs::u8path(modelFilePath(f)), ec);
    if (ec) return fail(err, ec.message());
  }
  return true;
}

std::string stemTitle(const std::string& s) {
  if (s.empty()) return s;
  std::string t = s;
  t[0] = char(std::toupper(static_cast<unsigned char>(t[0])));
  return t;
}

bool extractStems(const ExtractRequest& req, const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                  std::vector<std::pair<std::string, std::string>>* written, std::string* err) {
  const ModelInfo* m = findModel(req.modelId);
  if (!m) return fail(err, "unknown model " + req.modelId);
  if (!modelInstalled(*m)) return fail(err, "the model \"" + m->title + "\" is not downloaded yet");
  std::string lerr;
  SamplePtr smp;
  {
    DecodeCancelScope scope(cancel);
    smp = loadSample(req.input, &lerr);
  }
  if (!smp) return fail(err, cancel && cancel->load() ? std::string("cancelled") : "cannot read " + req.input + ": " + lerr);
  AudioBuffer in;
  const Sample& sm = *smp;
  const bool mono = sm.mono();
  const double rate = sm.sampleRate;
  in.resize(mono ? 1 : 2, sm.frames());
  std::copy(sm.left.begin(), sm.left.end(), in.plane(0));
  if (!mono) std::copy(sm.right.data(), sm.right.data() + sm.frames(), in.plane(1));
  smp.reset();
  SplitRequest sr;
  for (const ModelFile& f : m->files) sr.modelFiles.push_back(modelFilePath(f));
  sr.weights = m->weights;
  sr.threads = req.threads;
  SplitResult res;
  if (!separate(in, rate, sr, res, progress, cancel, err)) return false;
  std::error_code ec;
  fs::create_directories(fs::u8path(req.outDir), ec);
  std::string base = req.baseName.empty() ? fs::u8path(req.input).stem().u8string() : req.baseName;
  for (char& c : base)  // a channel name used as a file name: no separators or reserved characters
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) c = '_';
  while (!base.empty() && (base.back() == '.' || base.back() == ' ')) base.pop_back();
  if (base.empty()) base = "Stems";
  for (size_t s = 0; s < res.sources.size(); ++s) {
    const std::string& id = res.sources[s];
    if (!req.keep.empty() && std::find(req.keep.begin(), req.keep.end(), id) == req.keep.end()) continue;
    fs::path p = fs::u8path(req.outDir) / fs::u8path(base + " - " + stemTitle(id) + ".wav");
    for (int k = 2; fs::exists(p, ec) && k < 10000; ++k)
      p = fs::u8path(req.outDir) / fs::u8path(base + " - " + stemTitle(id) + " " + std::to_string(k) + ".wav");
    AudioFileOptions o;
    o.bits = 32;
    if (!writeAudioFile(p.u8string(), res.stems[s], int(std::lround(rate)), o, err)) return false;
    if (written) written->emplace_back(id, p.u8string());
  }
  return true;
}

}  // namespace gl::stems
