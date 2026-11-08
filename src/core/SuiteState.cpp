// Gridline — the Gridline Suite plugin state with its files (K-110). See SuiteState.h.
#include "SuiteState.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include "../host/Backend.h"
#include "Platform.h"
#include "Project.h"
#include "ProjectInterop.h"
#include "Sha256.h"

namespace gl::suitestate {

namespace fs = std::filesystem;

namespace {

bool readAll(const std::string& path, std::string& out) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return !f.bad();
}

uint64_t fileSize(const std::string& path) {
  std::error_code ec;
  const auto n = fs::file_size(fs::u8path(path), ec);
  return ec ? UINT64_MAX : static_cast<uint64_t>(n);
}

// A file name that stays inside its folder.
std::string safeName(const std::string& name) {
  std::string n = fs::u8path(name).filename().u8string();
  std::string out;
  for (char c : n) out += (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) ? '_' : c;
  if (out.empty() || out == "." || out == "..") out = "file";
  return out;
}

// Writes `bytes` as <dir>/<hash>/<name> (or reuses an equal file there).
// A different file under that name (never expected: the folder is named by
// the content) moves to a longer folder name instead of being overwritten.
bool storeInCache(const std::string& cacheDir, const std::string& name, const std::string& bytes, const std::string& sha, std::string& outPath,
                  std::string* err) {
  const std::string fname = safeName(name);
  for (size_t len : {size_t(16), size_t(64)}) {
    const fs::path dir = fs::u8path(cacheDir) / sha.substr(0, std::min(len, sha.size()));
    const fs::path file = dir / fs::u8path(fname);
    std::error_code ec;
    if (fs::exists(file, ec)) {
      std::string have;
      if (fileSize(file.u8string()) == bytes.size() && readAll(file.u8string(), have) && have == bytes) {
        outPath = file.u8string();
        return true;
      }
      continue;  // not ours: never overwritten
    }
    fs::create_directories(dir, ec);
    if (ec) {
      if (err && err->empty()) *err = "cannot create " + dir.u8string();
      return false;
    }
    const fs::path tmp = dir / fs::u8path("." + fname + ".part");
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      if (!f) {
        if (err && err->empty()) *err = "cannot write " + tmp.u8string();
        fs::remove(tmp, ec);
        return false;
      }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
      fs::remove(tmp, ec);
      if (err && err->empty()) *err = "cannot write " + file.u8string();
      return false;
    }
    outPath = file.u8string();
    return true;
  }
  if (err && err->empty()) *err = "the cache holds another file named " + fname;
  return false;
}

}  // namespace

int embedFiles(const PluginState& st0, Json& j, size_t limit, EmbedReport* rep, const std::set<std::string>* only) {
  PluginState st = st0;
  std::vector<std::string> refs;
  forEachStateFileRef(st, [&](std::string& p) {
    if (std::find(refs.begin(), refs.end(), p) == refs.end()) refs.push_back(p);
  });
  Json files = Json::makeArray();
  size_t total = 0;
  int n = 0;
  for (const std::string& p : refs) {
    if (only && !only->count(p)) continue;
    const uint64_t size = fileSize(p);
    std::string bytes;
    if (size == UINT64_MAX || total + size > limit || !readAll(p, bytes)) {
      if (rep) rep->skipped.push_back(p);
      continue;
    }
    total += bytes.size();
    Json f = Json::makeObject();
    f.object["path"] = Json::makeString(p);
    f.object["name"] = Json::makeString(fs::u8path(p).filename().u8string());
    f.object["size"] = Json::makeNumber(static_cast<double>(bytes.size()));
    f.object["sha256"] = Json::makeString(sha256Hex(bytes.data(), bytes.size()));
    f.object["data"] = Json::makeString(host::base64Encode(bytes));
    files.array.push_back(std::move(f));
    ++n;
  }
  if (n > 0) j.object["files"] = std::move(files);
  if (rep) rep->files += n, rep->bytes += total;
  return n;
}

std::string stateText(const PluginState& st, bool embed, EmbedReport* rep) {
  Json j = Json::makeObject();
  j.object["format"] = Json::makeString(kFormat);
  j.object["version"] = Json::makeNumber(kVersion);
  j.object["plugin"] = pluginStateToJson(st);
  if (embed) embedFiles(st, j, kEmbedLimit, rep);
  return jsonStringify(j, 0);
}

int restoreFiles(PluginState& st, const Json& j, const std::string& cacheDir, std::set<std::string>* restored, std::string* err) {
  const Json* files = j.find("files");
  if (!files || !files->is(Json::Type::Array)) return 0;
  std::map<std::string, const Json*> byPath;
  for (const Json& f : files->array)
    if (f.is(Json::Type::Object) && !f.text("path").empty()) byPath[f.text("path")] = &f;
  std::map<std::string, std::string> done;  // reference -> path it now names
  int changed = 0;
  forEachStateFileRef(st, [&](std::string& p) {
    auto it = byPath.find(p);
    if (it == byPath.end()) return;
    const Json& f = *it->second;
    if (auto d = done.find(p); d != done.end()) {
      if (d->second != p) p = d->second, ++changed;
      if (restored) restored->insert(p);
      return;
    }
    const double size = f.num("size", -1);
    if (size >= 0 && fileSize(p) == static_cast<uint64_t>(size)) {  // the original is here
      done[p] = p;
      if (restored) restored->insert(p);
      return;
    }
    std::string bytes;
    if (!host::base64Decode(f.text("data"), bytes) || (size >= 0 && bytes.size() != static_cast<size_t>(size))) {
      if (err && err->empty()) *err = "damaged embedded file " + f.text("name");
      return;
    }
    std::string sha = sha256Hex(bytes.data(), bytes.size());
    if (!f.text("sha256").empty() && f.text("sha256") != sha) {
      if (err && err->empty()) *err = "damaged embedded file " + f.text("name");
      return;
    }
    std::string out;
    if (!storeInCache(cacheDir, f.text("name").empty() ? fs::u8path(p).filename().u8string() : f.text("name"), bytes, sha, out, err)) return;
    done[p] = out;
    p = out;
    ++changed;
    if (restored) restored->insert(out);
  });
  return changed;
}

std::string defaultCacheDir() { return (fs::u8path(platform::cacheRoot()) / "gridline" / "suite-files").u8string(); }

bool parse(const std::string& text, PluginState& st, Json* whole) {
  Json j;
  if (!jsonParse(text, j) || !j.is(Json::Type::Object) || j.text("format") != kFormat) return false;
  const Json* p = j.find("plugin");
  if (!p || !p->is(Json::Type::Object) || p->text("type").empty()) return false;
  st = pluginStateFromJson(*p);
  if (whole) *whole = std::move(j);
  return true;
}

}  // namespace gl::suitestate
