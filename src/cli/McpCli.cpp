// Gridline CLI — MCP server (SC-07).
//   gridline mcp <project.gridline.json|.flp> [--export-dir DIR] [--read-only] [--fl-data DIR]
//       stdio MCP server on a project file, headless (see docs/mcp.md)
//   gridline mcp --attach [--endpoint FILE]
//       stdio bridge to the running app's local MCP endpoint
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include "core/Mcp.h"
#include "core/McpTransport.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/ProjectScript.h"
#include "core/Settings.h"
#include "core/flp/Flp.h"

namespace fs = std::filesystem;

int mcpMain(int argc, char** argv) {
  std::string project, exportDir, flData, endpoint;
  bool readOnly = false, attach = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--attach") attach = true;
    else if (a == "--read-only") readOnly = true;
    else if (a == "--export-dir" && i + 1 < argc) exportDir = argv[++i];
    else if (a == "--fl-data" && i + 1 < argc) flData = argv[++i];
    else if (a == "--endpoint" && i + 1 < argc) endpoint = argv[++i];
    else if (!a.empty() && a[0] != '-' && project.empty()) project = a;
    else {
      std::fprintf(stderr, "gridline mcp: unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (!attach && project.empty()) {
    std::fprintf(stderr,
                 "usage: gridline mcp <project.gridline.json|.flp> [--export-dir DIR] [--read-only] [--fl-data DIR]\n"
                 "       gridline mcp --attach [--endpoint FILE]\n");
    return 2;
  }
  // The protocol owns stdout: everything else printed by the process goes to stderr.
  std::fflush(stdout);
  const int protoFd = ::dup(STDOUT_FILENO);
  ::dup2(STDERR_FILENO, STDOUT_FILENO);
  std::FILE* out = protoFd >= 0 ? ::fdopen(protoFd, "w") : nullptr;
  if (!out) {
    std::fprintf(stderr, "gridline mcp: cannot open the output stream\n");
    return 1;
  }
  if (attach) {
    const int rc = gl::mcp::attachBridge(stdin, out, endpoint.empty() ? gl::mcp::defaultEndpointFile() : endpoint);
    std::fclose(out);
    return rc;
  }

  std::error_code ec;
  const fs::path path = fs::absolute(project, ec);
  std::string ext = path.extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  gl::Song song;
  std::string err, savePath;
  if (fs::exists(path, ec)) {
    if (ext == ".flp") {
      gl::flp::Report rep;
      if (!gl::importFlp(path.string(), song, rep, &err, flData)) {
        std::fprintf(stderr, "gridline mcp: %s\n", err.c_str());
        return 1;
      }
    } else if (!gl::loadProject(path.string(), song, &err)) {
      std::fprintf(stderr, "gridline mcp: %s\n", err.c_str());
      return 1;
    } else {
      savePath = path.string();
    }
  } else if (ext == ".json") {  // a new project, written on save_project
    song = gl::makeEmptySong();
    std::string stem = path.filename().string();
    stem = stem.substr(0, stem.find('.'));
    song.info.title = stem.empty() ? "Untitled" : stem;
    savePath = path.string();
    std::fprintf(stderr, "gridline mcp: %s does not exist yet: new project (save_project writes it)\n", path.string().c_str());
  } else {
    std::fprintf(stderr, "gridline mcp: %s not found\n", path.string().c_str());
    return 1;
  }

  gl::Settings settings;
  settings.load(gl::Settings::defaultPath());
  gl::Document doc(std::move(song));
  gl::script::SimpleProjectHost host(doc);
  gl::mcp::Environment env(doc, host);
  env.projectPath = savePath.empty() ? path.string() : savePath;
  env.exportDir = exportDir.empty() ? std::string() : fs::absolute(exportDir, ec).string();
  env.presetRoot = gl::presetRoot(fs::path(gl::Settings::defaultPath()).parent_path().string());
  env.factoryData = flData.empty() ? settings.getString("import.flData") : flData;
  env.allowEdits = !readOnly;
  env.allowScripts = settings.getBool("scripts.enabled");
  env.editsOffMessage = "This server was started with --read-only.";
  env.scriptLimits.seconds = std::clamp(settings.getNumber("scripts.maxSeconds"), 0.05, 30.0);
  env.scriptLimits.instructions = static_cast<uint64_t>(std::clamp(settings.getNumber("scripts.maxMillionInstructions"), 1.0, 2000.0) * 1e6);
  env.onActivity = [](const gl::mcp::Activity& a) {
    std::fprintf(stderr, "[gridline mcp] %s%s: %s\n", a.tool.c_str(), a.ok ? "" : " (failed)", a.summary.c_str());
  };
  std::fprintf(stderr, "gridline mcp: serving %s on stdio%s\n", env.projectPath.c_str(), readOnly ? " (read-only)" : "");
  gl::mcp::serveStdio(env, stdin, out);
  if (doc.modified()) std::fprintf(stderr, "gridline mcp: unsaved changes were discarded\n");
  std::fclose(out);
  return 0;
}
