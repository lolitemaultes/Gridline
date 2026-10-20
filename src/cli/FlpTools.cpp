// Gridline CLI — FL Studio project export and round-trip tools (J-34).
//   gridline export-flp <project.json|.flp> -o <out.flp> [--no-origin] [--fl-data DIR]
//   gridline export-flp-zip <project.json|.flp|.zip> -o <out.zip> [--no-origin] [--fl-data DIR] [--factory-samples]
//   gridline flp-roundtrip <in.flp> [-o <exported.flp>] [--no-origin] [--fl-data DIR] [--json <report.json>]
//   gridline flp-encode-check [--plugin NAME]... [--max N] [--verbose] [--fl-data DIR] [files...]
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "core/Automation.h"
#include "core/Project.h"
#include "engine/Render.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpCoverage.h"
#include "core/flp/FlpEncode.h"
#include "core/flp/FlpExport.h"
#include "core/flp/FlpPackage.h"
#include "core/ProjectInterop.h"

namespace {

int encodeCheckMain(int argc, char** argv) {
  gl::flp::enc::CheckOptions o;
  std::string data;
  std::vector<std::string> files;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plugin") && i + 1 < argc) o.plugins.push_back(argv[++i]);
    else if (!std::strcmp(argv[i], "--max") && i + 1 < argc) o.maxPerPlugin = static_cast<size_t>(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "--verbose")) o.verbose = true;
    else if (!std::strcmp(argv[i], "--fl-data") && i + 1 < argc) data = argv[++i];
    else files.push_back(argv[i]);
  }
  if (data.empty()) data = gl::flp::defaultFactoryData();
  if (files.empty()) {
    if (data.empty()) {
      std::fprintf(stderr, "error: FL Studio data folder not found (use --fl-data)\n");
      return 1;
    }
    gl::flp::CoverageFiles cf = gl::flp::coverageFiles(data);
    for (auto* v : {&cf.projects, &cf.presets, &cf.database}) files.insert(files.end(), v->begin(), v->end());
  }
  gl::flp::enc::EncodeContext c;
  c.factoryData = data;
  std::fputs(gl::flp::enc::encodeCheck(files, o, c).c_str(), stdout);
  return 0;
}

bool loadAny(const std::string& path, gl::Song& song, const std::string& data, std::string* err) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (ext == ".zip") {  // zipped project / FL Studio zipped loop package: unpacked beside the archive
    const std::filesystem::path zp(path);
    std::string proj;
    if (!gl::unpackZippedProject(path, (zp.parent_path() / zp.stem()).string(), proj, err)) return false;
    if (!loadAny(proj, song, data, err)) return false;
    gl::resolveRelativePaths(song, std::filesystem::path(proj).parent_path().string());
    return true;
  }
  if (ext == ".flp") {
    gl::flp::Report rep;
    return gl::importFlp(path, song, rep, err, data);
  }
  return gl::loadProject(path, song, err);
}

int exportMain(int argc, char** argv) {
  if (argc < 3) return 2;
  std::string in = argv[2], out, data;
  gl::flp::ExportOptions opt;
  for (int i = 3; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--no-origin")) opt.useOrigin = false;
    else if (!std::strcmp(argv[i], "--fl-data") && i + 1 < argc) data = argv[++i];
  }
  if (out.empty()) {
    std::fprintf(stderr, "usage: gridline export-flp <project.json|.flp> -o <out.flp> [--no-origin] [--fl-data DIR]\n");
    return 2;
  }
  opt.factoryData = data.empty() ? gl::flp::defaultFactoryData() : data;
  gl::Song song;
  std::string err;
  if (!loadAny(in, song, opt.factoryData, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  gl::flp::ExportReport rep;
  if (!gl::flp::exportFlpFile(song, out, rep, opt, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  std::printf("wrote %s\n", out.c_str());
  std::fputs(gl::flp::reportText(rep).c_str(), stdout);
  return 0;
}

// FL Studio zipped loop package (J-52): the .flp export-flp writes plus the
// files the project uses, beside it in one .zip.
int exportZipMain(int argc, char** argv) {
  if (argc < 3) return 2;
  std::string in = argv[2], out, data;
  gl::flp::PackageOptions opt;
  for (int i = 3; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--no-origin")) opt.flp.useOrigin = false;
    else if (!std::strcmp(argv[i], "--fl-data") && i + 1 < argc) data = argv[++i];
    else if (!std::strcmp(argv[i], "--factory-samples")) opt.includeFactorySamples = true;
  }
  if (out.empty()) {
    std::fprintf(stderr, "usage: gridline export-flp-zip <project.json|.flp|.zip> -o <out.zip> [--no-origin] [--fl-data DIR] [--factory-samples]\n");
    return 2;
  }
  opt.flp.factoryData = data.empty() ? gl::flp::defaultFactoryData() : data;
  gl::Song song;
  std::string err;
  if (!loadAny(in, song, opt.flp.factoryData, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  std::string name = std::filesystem::path(out).stem().string();
  gl::flp::PackageReport rep;
  if (!gl::flp::exportFlpZip(song, out, name, opt, rep, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  std::printf("wrote %s: %s + %zu file(s), %d sample path(s) made relative\n", out.c_str(), rep.projectEntry.c_str(), rep.files.size(),
              rep.rewritten);
  for (const auto& m : rep.missing) std::printf("missing: %s\n", m.c_str());
  std::fputs(gl::flp::reportText(rep.flp).c_str(), stdout);
  return 0;
}

int roundtripMain(int argc, char** argv) {
  if (argc < 3) return 2;
  std::string in = argv[2], out, data, json;
  double renderSeconds = 0;
  gl::flp::ExportOptions opt;
  for (int i = 3; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--render") && i + 1 < argc) renderSeconds = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!std::strcmp(argv[i], "--no-origin")) opt.useOrigin = false;
    else if (!std::strcmp(argv[i], "--fl-data") && i + 1 < argc) data = argv[++i];
    else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
  }
  opt.factoryData = data.empty() ? gl::flp::defaultFactoryData() : data;
  gl::flp::RoundTrip rt;
  std::string err;
  if (!gl::flp::roundTrip(in, opt, rt, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }
  if (!out.empty()) {
    FILE* f = std::fopen(out.c_str(), "wb");
    if (f) {
      std::fwrite(rt.exported.data(), 1, rt.exported.size(), f);
      std::fclose(f);
    }
  }
  std::fputs(gl::flp::roundTripText(rt).c_str(), stdout);
  if (renderSeconds > 0) {
    // Renders the first seconds of both songs and compares them sample by sample.
    auto render = [&](gl::Song song) {
      gl::TempoMap tm;
      tm.build(song);
      const auto lim = static_cast<gl::Tick>(tm.tickAtSeconds(renderSeconds));
      std::vector<gl::Clip> kept;
      for (auto c : song.playlist)
        if (c.start < lim) {
          c.length = std::min<gl::Tick>(song.clipLength(c), lim - c.start);
          kept.push_back(c);
        }
      song.playlist = std::move(kept);
      gl::RenderOptions ro;
      ro.tail = song.ppq * 2;
      return gl::renderSong(song, ro);
    };
    gl::AudioBuffer a = render(rt.original), b = render(rt.reimported);
    double maxDiff = 0, peak = 0;
    const size_t n = std::min(a.data.size(), b.data.size());
    for (size_t i = 0; i < n; ++i) {
      maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(a.data[i]) - b.data[i]));
      peak = std::max(peak, std::fabs(static_cast<double>(a.data[i])));
    }
    std::printf("render: frames %zu / %zu, peak %.5f, max difference %.3g%s\n", a.frames, b.frames, peak, maxDiff,
                a.frames == b.frames && maxDiff == 0 ? " (identical)" : "");
  }
  if (!json.empty()) {
    FILE* f = std::fopen(json.c_str(), "wb");
    if (f) {
      const std::string j = gl::flp::roundTripJson(rt);
      std::fwrite(j.data(), 1, j.size(), f);
      std::fclose(f);
    }
  }
  return 0;
}

}  // namespace

// Returns -1 when argv is not one of these commands.
int flpToolsMain(int argc, char** argv) {
  if (argc < 2) return -1;
  const std::string cmd = argv[1];
  if (cmd == "flp-encode-check") return encodeCheckMain(argc, argv);
  if (cmd == "export-flp") return exportMain(argc, argv);
  if (cmd == "export-flp-zip") return exportZipMain(argc, argv);
  if (cmd == "flp-roundtrip") return roundtripMain(argc, argv);
  return -1;
}
