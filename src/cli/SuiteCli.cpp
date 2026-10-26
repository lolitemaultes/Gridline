// Gridline — command line: the Gridline Suite (J-131).
//   gridline suite-info [suite] [--json] [--excluded]      members and plugin ids
//   gridline suite-info <suite> --plist <out> --version N --factory F --name N --id ID --exe E --min M
//                                                          (build: the AU bundle's Info.plist)
//   gridline suite status|install|remove [--json]          the suite in this user's plug-in folders
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "core/Json.h"
#include "core/PluginSuite.h"
#include "core/SuiteInstall.h"

namespace {

std::string arg(int argc, char** argv, const char* name, const char* def = "") {
  for (int i = 2; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  return def;
}
bool flag(int argc, char** argv, const char* name) {
  for (int i = 2; i < argc; ++i)
    if (std::strcmp(argv[i], name) == 0) return true;
  return false;
}

std::string plistEscape(const std::string& s) {
  std::string o;
  for (char c : s) o += c == '&' ? std::string("&amp;") : c == '<' ? std::string("&lt;") : std::string(1, c);
  return o;
}

int writePlist(const std::string& suiteId, int argc, char** argv) {
  const std::string out = arg(argc, argv, "--plist");
  const std::string name = arg(argc, argv, "--name", "Gridline Suite");
  const std::string id = arg(argc, argv, "--id", "org.gridline.GridlineSuite.component");
  const std::string exe = arg(argc, argv, "--exe", name.c_str());
  const std::string ver = arg(argc, argv, "--version", "0.0.0");
  const std::string minOs = arg(argc, argv, "--min", "11.0");
  const std::string factory = arg(argc, argv, "--factory", "GridlineSuiteAUFactory");
  const uint32_t auVer = static_cast<uint32_t>(std::strtoul(arg(argc, argv, "--au-version", "0").c_str(), nullptr, 10));
  std::string p = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                  "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n";
  auto kv = [&](const char* k, const std::string& v) { p += std::string("<key>") + k + "</key><string>" + plistEscape(v) + "</string>\n"; };
  kv("CFBundleDevelopmentRegion", "English");
  kv("CFBundleName", name);
  kv("CFBundleDisplayName", name);
  kv("CFBundleExecutable", exe);
  kv("CFBundleIdentifier", id);
  kv("CFBundleIconFile", "Gridline");
  kv("CFBundleInfoDictionaryVersion", "6.0");
  kv("CFBundlePackageType", "BNDL");
  kv("CFBundleSignature", "????");
  kv("CFBundleShortVersionString", ver);
  kv("CFBundleVersion", ver);
  kv("LSMinimumSystemVersion", minOs);
  p += "<key>NSHighResolutionCapable</key><true/>\n";
  p += gl::suite::auComponentsPlist(suiteId, auVer, factory);
  p += "</dict></plist>\n";
  std::ofstream f(out, std::ios::binary);
  f << p;
  return f.good() ? 0 : 1;
}

void printInstall(const gl::suite::InstallStatus& st, bool json) {
  if (json) {
    gl::Json j = gl::Json::makeObject();
    j.object["suite"] = gl::Json::makeString(st.suite);
    j.object["shipped"] = gl::Json::makeString(st.shippedVersion);
    j.object["installed"] = gl::Json::makeString(st.installedVersion);
    j.object["state"] = gl::Json::makeString(gl::suite::stateName(st.state));
    gl::Json b = gl::Json::makeArray();
    for (const auto& x : st.bundles) {
      gl::Json o = gl::Json::makeObject();
      o.object["format"] = gl::Json::makeString(x.format);
      o.object["source"] = gl::Json::makeString(x.source);
      o.object["destination"] = gl::Json::makeString(x.dest);
      b.array.push_back(std::move(o));
    }
    j.object["bundles"] = std::move(b);
    std::printf("%s\n", gl::jsonStringify(j, 1).c_str());
    return;
  }
  std::printf("%s: %s\n", st.suite.c_str(), gl::suite::stateName(st.state));
  if (!st.shippedVersion.empty()) std::printf("  shipped with this Gridline: %s\n", st.shippedVersion.c_str());
  if (!st.installedVersion.empty()) std::printf("  installed: %s\n", st.installedVersion.c_str());
  for (const auto& x : st.bundles) std::printf("  %-5s %s\n        -> %s\n", x.format.c_str(), x.source.c_str(), x.dest.c_str());
}

}  // namespace

int suiteCliMain(int argc, char** argv) {
  const std::string cmd = argv[1];
  if (cmd == "suite-info") {
    std::string suiteId = argc >= 3 && argv[2][0] != '-' ? argv[2] : "gridline";
    if (!gl::suite::findSuite(suiteId)) {
      std::fprintf(stderr, "unknown suite %s\n", suiteId.c_str());
      return 2;
    }
    if (!arg(argc, argv, "--plist").empty()) return writePlist(suiteId, argc, argv);
    if (flag(argc, argv, "--excluded")) {
      for (const auto& p : gl::pluginCatalog())
        if (const std::string r = gl::suite::exclusionReason(p); !r.empty()) std::printf("%-16s %-24s %s\n", p.type.c_str(), p.name.c_str(), r.c_str());
      return 0;
    }
    const bool json = flag(argc, argv, "--json");
    gl::Json all = gl::Json::makeArray();
    for (const auto& m : gl::suite::members(suiteId)) {
      if (!json) {
        std::printf("%-10s %-22s %-18s %-34s %s %s/%s %-20s %s%s\n", m.instrument ? "instrument" : "effect", m.name.c_str(), m.type.c_str(),
                    m.clapId.c_str(), gl::suite::vst3UidText(m.vst3Uid).c_str(), gl::suite::fourcc(m.auType).c_str(),
                    gl::suite::fourcc(m.auSubtype).c_str(), m.vst3Category.c_str(), m.sidechain ? "sidechain " : "",
                    m.midiInput ? "midi" : "");
        continue;
      }
      gl::Json o = gl::Json::makeObject();
      o.object["type"] = gl::Json::makeString(m.type);
      o.object["name"] = gl::Json::makeString(m.name);
      o.object["kind"] = gl::Json::makeString(m.instrument ? "instrument" : "effect");
      o.object["category"] = gl::Json::makeString(m.info->category);
      o.object["clap"] = gl::Json::makeString(m.clapId);
      o.object["vst3"] = gl::Json::makeString(gl::suite::vst3UidText(m.vst3Uid));
      o.object["au"] = gl::Json::makeString(gl::suite::fourcc(m.auType) + "/" + gl::suite::fourcc(m.auSubtype) + "/GrLn");
      o.object["vst3Category"] = gl::Json::makeString(m.vst3Category);
      o.object["params"] = gl::Json::makeNumber(static_cast<double>(m.info->params.size()));
      o.object["presets"] = gl::Json::makeNumber(static_cast<double>(m.info->presets.size()));
      gl::Json b;
      b.type = gl::Json::Type::Bool;
      b.boolean = m.sidechain;
      o.object["sidechain"] = b;
      all.array.push_back(std::move(o));
    }
    if (json) std::printf("%s\n", gl::jsonStringify(all, 1).c_str());
    return 0;
  }
  // gridline suite status|install|remove
  const std::string what = argc >= 3 ? argv[2] : "status";
  const bool json = flag(argc, argv, "--json");
  // Every suite the build makes (Gridline Suite, Gridline Suite Mini), one after the other.
  if (what != "status" && what != "install" && what != "remove") {
    std::fprintf(stderr, "usage: gridline suite status|install|remove [--json]\n");
    return 2;
  }
  int rc = 0;
  for (const gl::suite::Suite& su : gl::suite::suites()) {
    gl::suite::InstallStatus st = gl::suite::installStatus(su.id);
    if (what == "status") {
      printInstall(st, json);
      continue;
    }
    std::string err;
    if (what == "install" && !gl::suite::install(st, &err)) {
      std::fprintf(stderr, "could not install the %s: %s\n", su.name, err.c_str());
      rc = 1;
      continue;
    }
    if (what == "remove" && !gl::suite::remove(st, &err)) {
      std::fprintf(stderr, "could not remove the %s: %s\n", su.name, err.c_str());
      rc = 1;
      continue;
    }
    printInstall(gl::suite::installStatus(su.id), json);
  }
  return rc;
}
