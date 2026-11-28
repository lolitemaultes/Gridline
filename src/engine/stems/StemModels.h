// Gridline - J-191: the stem-splitter models Gridline can use, where they live and how they get
// there. Gridline ships no model weights: each model is downloaded on the user's request from
// its authors' own server (or imported from a file the user already has) and checked against a
// SHA-256 checksum before use. Nothing is sent anywhere and no network is used unless asked.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "StemSplit.h"

namespace gl {
struct Sample;
}

namespace gl::stems {

struct ModelFile {
  std::string name;    // file name in the models folder
  std::string url;     // official download address
  std::string sha256;  // expected checksum (lower-case hex); empty: not checked (test models)
  int64_t bytes = 0;
  std::string path;    // explicit location (test models); empty: modelsDir()/name
};

struct ModelInfo {
  std::string id;       // "htdemucs", "htdemucs_ft", "htdemucs_6s"
  std::string title;    // UI name (English; the UI translates)
  std::string summary;  // one line
  std::vector<std::string> sources;  // the model's output order
  std::vector<ModelFile> files;      // one per bag member
  std::vector<std::vector<float>> weights;
  double relativeTime = 1.0;  // compute cost relative to the standard model
};

const std::vector<ModelInfo>& catalogue();
const ModelInfo* findModel(const std::string& id);
const ModelInfo& defaultModel();

// $GRIDLINE_STEM_MODELS, else <data folder>/gridline/stem-models (created on demand).
std::string modelsDir();
std::string modelFilePath(const ModelFile& f);
bool modelInstalled(const ModelInfo& m);
int64_t modelDownloadBytes(const ModelInfo& m);  // still to download

// Licence and origin statement shown before downloading (English; the UI translates).
const char* modelLicenceNote();

// Downloads the missing files of `m` (HTTPS, official host only), verifying each against its
// checksum; complete files are kept when a later one fails or is cancelled, so a new attempt
// continues where it stopped. progress(bytesDone, bytesTotal).
using ByteProgress = std::function<void(int64_t, int64_t)>;
using Downloader = std::function<bool(const std::string& url, const std::string& path, const ByteProgress&,
                                      const std::atomic<bool>* cancel, std::string* err)>;
bool downloadModel(const ModelInfo& m, const Downloader& dl, const ByteProgress& progress, const std::atomic<bool>* cancel,
                   std::string* err);
// Installs a model file the user already has (e.g. copied from another computer): accepted only
// when its checksum matches one of the catalogue's files. `installed` gets the matching model id.
bool importModelFile(const std::string& path, std::string* installed, std::string* err);
bool removeModel(const ModelInfo& m, std::string* err);

// Splits one audio file and writes the chosen stems as 32-bit float WAV files into `outDir`
// ("<baseName> - <Stem>.wav", never overwriting). `keep` lists source ids (empty: all).
// `written` gets (source id, path) pairs in the model's order.
// K-447: only a part of the audio is split when `end` > `start` (seconds of the audio, rounded
// to the nearest frame): the stems then hold that part alone, its first frame first, and nothing
// outside it is read or separated. `sound` (when set) is the audio itself, as a clip plays it
// (core/ClipSource: the channel's sample processing applied); `input` still names its file (the
// stems' names and their Track Receipt origin).
struct ExtractRequest {
  std::string input;
  std::string modelId;
  std::vector<std::string> keep;
  std::string outDir;
  std::string baseName;
  int threads = 0;
  std::shared_ptr<const Sample> sound;
  double start = 0, end = 0;
};
// The frames [from, to) of an `frames` long audio at `rate` that a request's start / end take.
void extractRange(const ExtractRequest& req, double rate, int64_t frames, int64_t& from, int64_t& to);
bool extractStems(const ExtractRequest& req, const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                  std::vector<std::pair<std::string, std::string>>* written, std::string* err);

// "vocals" -> "Vocals"
std::string stemTitle(const std::string& source);

}  // namespace gl::stems
