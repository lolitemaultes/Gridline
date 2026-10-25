// Gridline — FL Studio zipped loop package (J-52). See FlpPackage.h.
#include "FlpPackage.h"
#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <set>
#include "../../packs/samplers/dwave.h"
#include "../PluginStandIn.h"
#include "../ProjectInterop.h"
#include "FlpStandIn.h"
#include "../Zip.h"
#include "Flp.h"
#include "FlpWriter.h"

namespace fs = std::filesystem;

namespace gl::flp {

namespace {

std::string lowerStr(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string norm(const std::string& p) { return fs::path(p).lexically_normal().generic_string(); }

bool under(const std::string& path, const std::string& dir) {
  if (dir.empty()) return false;
  const std::string d = norm(dir), p = norm(path);
  return p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/';
}

void putVarint(Bytes& b, size_t n) {
  do {
    uint8_t byte = n & 0x7F;
    n >>= 7;
    if (n) byte |= 0x80;
    b.push_back(byte);
  } while (n);
}

}  // namespace

std::map<std::string, std::string> packageEntries(const Song& song, const std::string& projectEntry, const PackageOptions& opt,
                                                  std::vector<std::string>* missing) {
  std::map<std::string, std::string> out;
  std::set<std::string> names = {lowerStr(projectEntry)};
  for (const auto& p : referencedFiles(song)) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
      if (missing) missing->push_back(p);
      continue;
    }
    if (!opt.includeFactorySamples && under(p, opt.flp.factoryData)) continue;
    const std::string key = norm(p);
    if (out.count(key)) continue;
    const fs::path src(p);
    std::string n;
    for (int k = 1;; ++k) {
      n = k == 1 ? src.filename().string() : src.stem().string() + " (" + std::to_string(k) + ")" + src.extension().string();
      if (names.insert(lowerStr(n)).second) break;
    }
    out.emplace(key, n);
  }
  return out;
}

int rewriteSamplePaths(Bytes& flp, const std::map<std::string, std::string>& entries, const std::string& factoryData,
                       const std::string& projectDir) {
  File f;
  if (!readBytes(flp, f, nullptr)) return -1;
  const auto& d = f.bytes;
  // Start of the event data: "FLhd" u32 len, header, "FLdt" u32 size.
  const size_t hl = static_cast<size_t>(d[4] | (d[5] << 8) | (d[6] << 16) | (static_cast<size_t>(d[7]) << 24));
  const size_t dt = 8 + hl;  // "FLdt"
  const size_t first = dt + 8;
  const size_t end = std::min<size_t>(d.size(), first + getU32(d, dt + 4));
  Song tmp;
  Report r;
  Context c(f, tmp, r);
  c.factoryData = factoryData;
  c.projectDir = projectDir;
  Bytes body;
  body.reserve(end - first);
  int changed = 0;
  for (size_t i = 0; i < f.events.size(); ++i) {
    const Event& e = f.events[i];
    const size_t from = e.offset, to = i + 1 < f.events.size() ? f.events[i + 1].offset : end;
    if (e.id == 196) {
      auto it = entries.find(norm(c.resolvePath(e.text())));
      if (it != entries.end()) {
        const Bytes text = utf16z(it->second);
        body.push_back(196);
        putVarint(body, text.size());
        body.insert(body.end(), text.begin(), text.end());
        ++changed;
        continue;
      }
    }
    body.insert(body.end(), d.begin() + static_cast<std::ptrdiff_t>(from), d.begin() + static_cast<std::ptrdiff_t>(to));
  }
  if (!changed) return 0;
  Bytes out(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(first));
  putU32(out, dt + 4, static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(end), d.end());  // anything after the data chunk
  flp = std::move(out);
  return changed;
}

bool exportFlpZip(const Song& song, const std::string& zipPath, const std::string& name, const PackageOptions& opt,
                  PackageReport& rep, std::string* err) {
  rep = PackageReport{};
  rep.projectEntry = name + ".flp";
  // J-80: Gridline-only plugins → stand-ins; rendered samples go into the
  // package (rendered in a temporary folder, removed afterwards).
  Song work = song;
  standin::restoreAll(work);  // missing Serum / Kickstart: exported as the plugin itself (PluginStandIn.h)
  StandInResult sr;
  StandInOptions so;
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec) /
                       ("gridline-flp-" + std::to_string(std::hash<std::string>{}(zipPath + std::to_string(
                                                             std::chrono::steady_clock::now().time_since_epoch().count()))));
  struct Cleanup {
    fs::path p;
    ~Cleanup() {
      std::error_code e;
      if (!p.empty()) fs::remove_all(p, e);
    }
  } cleanup{opt.flp.renderInstruments ? tmp : fs::path()};
  if (opt.flp.renderInstruments) so.sampleDir = (tmp / name).string();
  so.progress = opt.flp.progress;
  so.cancel = opt.flp.cancel;
  if (!applyStandIns(work, opt.flp, so, sr)) {
    rep.flp.cancelled = sr.cancelled;
    if (err) *err = sr.cancelled ? "cancelled" : sr.error;
    return false;
  }
  auto entries = packageEntries(work, rep.projectEntry, opt, &rep.missing);
  {
    std::set<std::string> names = {lowerStr(rep.projectEntry)};
    for (const auto& [src, e] : entries) names.insert(lowerStr(e));
    std::map<std::string, std::string> rendered;  // rendered file → entry
    for (const auto& f : sr.files) {
      const std::string key = norm(f);
      if (entries.count(key)) {
        rendered[key] = entries[key];
        continue;
      }
      const fs::path src(f);
      std::string n;
      for (int k = 1;; ++k) {
        n = k == 1 ? src.filename().string() : src.stem().string() + " (" + std::to_string(k) + ")" + src.extension().string();
        if (names.insert(lowerStr(n)).second) break;
      }
      entries.emplace(key, n);
      rendered[key] = n;
    }
    // DirectWave keeps its zone paths in its own state: they name the files beside the project.
    namespace dwn = gl::packs::samplers::dw;
    for (auto& c : work.channels) {
      if (c.plugin.type != "gl.waveprogram" || !sr.find(c.id, -1)) continue;
      dwn::DwProgram p;
      if (!dwn::parseProgram(c.plugin.data, p)) continue;
      for (auto& z : p.zones)
        if (auto it = rendered.find(norm(z.path)); it != rendered.end()) z.path = it->second;
      c.plugin.data = dwn::formatProgram(p);
    }
  }
  Bytes flp;
  if (!exportFlpWithStandIns(work, sr, flp, rep.flp, opt.flp)) {
    if (err) *err = rep.flp.warnings.empty() ? "FL Studio export failed" : rep.flp.warnings.back();
    return false;
  }
  const int n = rewriteSamplePaths(flp, entries, opt.flp.factoryData, opt.flp.projectDir);
  if (n < 0) {
    if (err) *err = "the exported project could not be read back";
    return false;
  }
  rep.rewritten = n;
  ZipWriter z;
  if (!z.open(zipPath, err)) return false;
  if (!z.add(rep.projectEntry, flp)) {
    if (z.close(err) && err) *err = "cannot write " + zipPath;  // close() names the reason (J-78)
    return false;
  }
  for (const auto& [src, entry] : entries) {
    if (!z.addFile(entry, src, err)) return false;
    rep.files[src] = entry;
  }
  return z.close(err);
}

}  // namespace gl::flp
