// Gridline CLI — offline renderer.
//   gridline render <project.json> -o <out.wav> [--sr 44100] [--bits 16|24]
//   gridline info   <project.json>
//   gridline upgrade <project.json> [-o <out.json>]   rewrite in the current schema
//   gridline import-flp <project.flp> -o <out.json> [--fl-data DIR]   convert an FL Studio project
#include "GridlineVersion.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "engine/Render.h"
#include "engine/ExportJob.h"
#include "engine/ParamAudit.h"
#include "core/MidiFile.h"
#include "core/MidiImport.h"
#include "core/ProjectInterop.h"
#include <algorithm>
#include "core/Automation.h"
#include "core/Demo.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpCoverage.h"
#include "core/flp/FlpPresets.h"
#include "core/flp/FlpWrapper.h"
#include "core/Settings.h"
#include "host/HostedPlugins.h"
#include "host/net/Http.h"
#include "host/PluginCheck.h"
#include "host/Smoke.h"
#include "host/vst3/Vst3Scan.h"
#include "packs/seltmann/seltmann.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include <io.h>
namespace {
int rawWrite(int fd, const void* p, size_t n) { return _write(fd, p, static_cast<unsigned>(n)); }  // async-signal-safe write
}  // namespace
#else
#include <unistd.h>
namespace {
long rawWrite(int fd, const void* p, size_t n) { return ::write(fd, p, n); }  // async-signal-safe write
}  // namespace
#endif

int benchMain(int argc, char** argv);  // Bench.cpp
int flpToolsMain(int argc, char** argv);  // FlpTools.cpp (-1 = not one of its commands)
int mcpMain(int argc, char** argv);       // McpCli.cpp (SC-07)
int collabMain(int argc, char** argv);    // CollabCli.cpp (J-111)
int stemsCliMain(int argc, char** argv);  // StemsCli.cpp (J-191)

int suiteCliMain(int argc, char** argv);  // SuiteCli.cpp (J-131)

namespace {
void usage() {
  std::fprintf(stderr,
      "Gridline " GRIDLINE_VERSION ", music workstation command line\n"
      "usage:\n"
      "  gridline --version                          print the version\n"
      "  gridline --check-updates [--beta]           ask GitHub for a newer release (docs/updates.md)\n"
      "  gridline render <project.json> -o <out.wav|.flac|.mp3|.ogg|.m4a|folder/> [--sr N] [--bits 16|24|32]\n"
      "        [--format wav,flac,mp3,ogg,m4a,mid] [--scope song|pattern:N|range:BAR-BAR] [--loops N]\n"
      "        [--tail cut|leave|wrap] [--dither none|tpdf|shaped] [--channels stereo|mono|left|right]\n"
      "        [--normalize] [--normalize-lufs L] [--ceiling DB] [--stems] [--stems-master-fx] [--tracks] [--names PATTERN] [--midi] [--bitrate K]\n"
      "        [--interp 0-7] [--oversample 1|2|4] [--no-master-fx] [--no-insert-fx] [--no-poly-limit]\n"
      "  gridline export-midi <project.json> -o <out.mid> [--pattern N]\n"
      "  gridline import-midi <file.mid> -o <out.json>   new project from a MIDI file\n"
      "  gridline collect|bones <project.json> -o <folder>   copy the project + its samples (bones: + MIDI, presets)\n"
      "  gridline zip <project.json> -o <out.zip>          zipped project with its samples\n"
      "  gridline render-stats <project.json> [--max-seconds S]   QA: render in memory, print level / NaN / clipping\n"
      "  gridline info   <project.json>\n"
      "  gridline plugin-params [type]               built-in plugin catalog (params, ranges, defaults) as JSON\n"
      "  gridline suite-info [suite] [--json] [--excluded]   Gridline Suite members and their plugin ids\n"
      "  gridline suite status|install|remove [--json]   the Gridline Suite in your plug-in folders\n"
      "  gridline param-audit [--type T]... [--json F] [--md F] [--baseline F] [--threads N] [--no-combos] [--no-live] [--base k=v;..] [--probe]\n"
      "  gridline param-audit --relabel <baseline.tsv> <out.tsv>   apply the documented-inert list to a baseline\n"
      "                                                 does every parameter of every built-in plugin change the output?\n"
      "  gridline upgrade <project.json> [-o <out.json>]\n"
      "  gridline demo    <out.json>                 write the demo song\n"
      "  gridline import-flp <project.flp> -o <out.json> [--fl-data DIR]   convert an FL Studio project\n"
      "  gridline export-flp <project.json|.flp> -o <out.flp> [--no-origin] [--fl-data DIR]   write an FL Studio project\n"
      "  gridline export-flp-zip <project.json|.flp|.zip> -o <out.zip> [--fl-data DIR] [--factory-samples]   FL Studio zipped loop package\n"
      "  gridline flp-roundtrip <in.flp> [-o out.flp] [--no-origin] [--json report.json]   import, export, re-import, compare\n"
      "  gridline flp-encode-check [--plugin NAME]... [--max N] [--verbose] [files...]   plugin encoders on FL's shipped states\n"
      "  gridline bench <project.json|.flp> [--threads 1,4] [--repeat N] [--render-seconds S] [--no-render] [--json out]\n"
      "                                                 load / save / render timings, undo memory, audio hash\n"
      "  gridline import-fl-presets [--fl-data DIR] [--out DIR]   convert FL Studio's presets into your preset library\n"
      "  gridline --scan-plugin <format> <entry> [--out <file>]   describe a plugin (scanner child process)\n"
      "  gridline plugin-smoke <format> <entry> [--sandbox]  load, play and save/restore every class of a plugin\n"
      "                                                 (format vst3 | clap | vst2 | au; AU entry: type/subtype/manufacturer)\n"
      "  gridline vst3-smoke <bundle>                   same as plugin-smoke vst3 <bundle>\n"
      "  gridline flp-plugins <file.flp>...            third-party plugins in FL files and what they map to here\n"
      "  gridline coverage-json <out.json> [--fl-data DIR]   FL import coverage (plugins, presets, event ids) as JSON\n"
      "  gridline plugin-check [--all | <type | name>...] [--json FILE] [--timeout S] [--no-editor] [--format F]\n"
      "                                                 compatibility sweep, one child process per plugin class\n"
      "  gridline list-plugins [format]                 list plugins this computer has (from the plugin cache)\n"
      "  gridline scan-plugins [--force]                scan installed plugins into the app's plugin cache\n"
      "  gridline mcp <project.gridline.json|.flp> [--export-dir DIR] [--read-only]   MCP server for AI assistants on stdio\n"
      "  gridline mcp --attach                          MCP stdio bridge to the running app (docs/mcp.md)\n"
      "  gridline stems <audio> [--model ID] [--out DIR] [--stems vocals,drums,...]   split a song into stems\n"
      "  gridline stems --list | --download ID | --import FILE | --remove ID   stem models (docs/stems.md)\n"
      "  gridline seltmann-analyse [--json] <audio...>  Seltmann analysis + automatic chop per file (tempo, kind, slices)\n"
      "  gridline collab-run --folder DIR --name NAME (--new | --create FILE | --join CODEFILE) [--invite-out FILE]\n"
      "                      [--seconds N] [--edits host|guest] [--audio WAV] [--report FILE] [--stun HOST:PORT]\n"
      "                                                 headless collaborative session (docs/collaboration.md)\n"
      "  gridline collab-run --test-connection          NAT type, router port mapping and IPv6 of this network\n");
}
}  // namespace

int main(int argc, char** argv) {
  // Child processes: plugin scanner (--scan-plugin / --scan-vst3) and the
  // plugin sandbox (--plugin-sandbox).
  if (int rc = gl::host::childMain(argc, argv); rc >= 0) return rc;
  if (argc >= 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "version") == 0)) {
    std::printf("Gridline " GRIDLINE_VERSION "\n");
    return 0;
  }
  if (argc >= 2 && std::strcmp(argv[1], "--check-updates") == 0)  // J-89
    return gl::http::checkCommand(argc >= 3 && std::strcmp(argv[2], "--beta") == 0);
  if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "help") == 0)) {
    usage();
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "bench") return benchMain(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "mcp") return mcpMain(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "collab-run") return collabMain(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "stems") return stemsCliMain(argc, argv);  // J-191
  if (argc >= 2 && std::string(argv[1]) == "seltmann-analyse") return gl::packs::seltmann::cliMain(argc, argv);  // J-107
  if (argc >= 2 && (std::string(argv[1]) == "suite-info" || std::string(argv[1]) == "suite")) return suiteCliMain(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "plugin-params") {
    // Built-in plugin catalog as JSON (type, kind, params with range / default /
    // choices): tools that write projects by hand (tools/make_ui_sounds.py) fill
    // full parameter vectors from it.
    gl::Json all = gl::Json::makeArray();
    for (const auto& p : gl::pluginCatalog()) {
      if (argc >= 3 && p.type != argv[2]) continue;
      gl::Json jp = gl::Json::makeObject();
      jp.object["type"] = gl::Json::makeString(p.type);
      jp.object["name"] = gl::Json::makeString(p.name);
      jp.object["kind"] = gl::Json::makeString(p.kind == gl::PluginKind::Generator ? "generator" : "effect");
      gl::Json params = gl::Json::makeArray();
      for (const auto& d : p.params) {
        gl::Json jd = gl::Json::makeObject();
        jd.object["name"] = gl::Json::makeString(d.name);
        jd.object["unit"] = gl::Json::makeString(d.unit);
        jd.object["min"] = gl::Json::makeNumber(d.min);
        jd.object["max"] = gl::Json::makeNumber(d.max);
        jd.object["def"] = gl::Json::makeNumber(d.def);
        if (!d.choices.empty()) {
          gl::Json ch = gl::Json::makeArray();
          for (const auto& c : d.choices) ch.array.push_back(gl::Json::makeString(c));
          jd.object["choices"] = std::move(ch);
        }
        params.array.push_back(std::move(jd));
      }
      jp.object["params"] = std::move(params);
      all.array.push_back(std::move(jp));
    }
    std::printf("%s\n", gl::jsonStringify(all, 1).c_str());
    return 0;
  }
  if (argc >= 5 && std::string(argv[1]) == "param-audit" && std::string(argv[2]) == "--relabel") {
    // J-92: re-apply the documented-inert list to a baseline written earlier
    // (rows without effect whose parameter is now documented become "inert").
    std::ifstream in(argv[3], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string out, line;
    int changed = 0;
    while (std::getline(ss, line)) {
      std::vector<std::string> f;
      size_t a = 0;
      for (size_t b; (b = line.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
      f.push_back(line.substr(a));
      if (f.size() >= 6 && f[3] == "no-effect" && gl::paramaudit::inertReason(f[0], f[2])) {
        f[3] = std::strncmp(gl::paramaudit::inertReason(f[0], f[2]), "VERIFIED", 8) == 0 ? "verified" : "inert";
        f.resize(6), f.push_back(gl::paramaudit::inertReason(f[0], f[2]));
        ++changed;
        line.clear();
        for (size_t i = 0; i < f.size(); ++i) line += (i ? "\t" : "") + f[i];
      }
      out += line + "\n";
    }
    std::ofstream(argv[4], std::ios::binary) << out;
    std::printf("%d rows relabelled as inert\n", changed);
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "param-audit") {  // J-92 (engine/ParamAudit.h)
    gl::paramaudit::Options opt;
    std::vector<std::string> types;
    std::string json, md, base;
    int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--type" && i + 1 < argc) types.push_back(argv[++i]);
      else if (a == "--json" && i + 1 < argc) json = argv[++i];
      else if (a == "--md" && i + 1 < argc) md = argv[++i];
      else if (a == "--baseline" && i + 1 < argc) base = argv[++i];
      else if (a == "--threads" && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
      else if (a == "--no-combos") opt.combos = false;
      else if (a == "--no-live") opt.liveness = false;
      else if (a == "--probe") opt.probeOnly = true;
      else if (a == "--base" && i + 1 < argc) {  // "k=v,k=v": audit on top of these values
        for (const auto& row : gl::paramaudit::parseBaseline(std::string("x\t0\tx\tx\t0\t") + argv[++i]))
          for (const auto& c : row.condition) opt.base.push_back(c);
      }
      else {
        std::fprintf(stderr, "unknown option for param-audit: %s\n", a.c_str());
        return 2;
      }
    }
    std::signal(SIGSEGV, [](int sig) {  // which plugin crashed the audit
      const char msg[] = "param-audit: crashed while auditing:\n";
      (void)!rawWrite(2, msg, sizeof msg - 1);
      gl::paramaudit::writeActive(2);
      std::signal(sig, SIG_DFL);
      std::raise(sig);
    });
    size_t shown = 0;
    auto reports = gl::paramaudit::auditAll(
        opt,
        [&](const gl::PluginInfo& p) {
          if (types.empty()) return true;
          for (const auto& t : types)
            if (p.type == t || (t.size() > 1 && t.back() == '*' && p.type.rfind(t.substr(0, t.size() - 1), 0) == 0)) return true;  // "gl.fx*" = prefix
          return false;
        },
        threads,
        [&](const gl::paramaudit::PluginReport& r) {
          int c[7] = {};
          for (const auto& q : r.params) ++c[static_cast<int>(q.status)];
          std::fprintf(stderr, "[%zu] %-22s %3zu params: %d working, %d combination, %d no effect, %d routing, %d broken, %d inert, %d verified (%.1f s)\n", ++shown,
                       r.type.c_str(), r.params.size(), c[0], c[1], c[2], c[3], c[4], c[5], c[6], r.seconds);
        });
    if (reports.empty()) {
      std::fprintf(stderr, "no built-in plugin matches\n");
      return 1;
    }
    auto write = [](const std::string& path, const std::string& text) {
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        return true;
      }
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      return false;
    };
    const std::string mdText = gl::paramaudit::toMarkdown(reports);
    if (!json.empty()) write(json, gl::jsonStringify(gl::paramaudit::toJson(reports), 1));
    if (!base.empty()) write(base, gl::paramaudit::toBaseline(reports));
    if (!md.empty()) write(md, mdText);
    else if (json.empty() && base.empty()) std::printf("%s", mdText.c_str());
    int bad = 0;
    for (const auto& r : reports)
      for (const auto& q : r.params) bad += q.status == gl::paramaudit::Status::Broken || !q.roundTrip || (q.liveTested && !q.live);
    return bad ? 1 : 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "vst3-smoke") gl::vst3::smokeMain(argv[2]);
  if (argc >= 4 && std::string(argv[1]) == "plugin-smoke")
    gl::host::smokeMain(argv[2], argv[3], argc >= 5 && std::string(argv[4]) == "--sandbox");
  // Hosted plugins known from the app's scan cache (projects may use them).
  // J-171: also for the FL tools, which then import and export installed
  // plugins (e.g. Serum 2) as the app does instead of as missing ones.
  if (argc >= 2 && std::string(argv[1]) != "scan-plugins")
    gl::host::initHosting(std::filesystem::path(gl::Settings::defaultPath()).parent_path().string());
  if (int r = flpToolsMain(argc, argv); r >= 0) return r;  // export-flp, flp-roundtrip, flp-encode-check (FlpTools.cpp)
  if (argc >= 2 && std::string(argv[1]) == "plugin-check") return gl::host::pluginCheckMain(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "list-plugins") {
    for (const auto& c : gl::host::hostedClasses()) {
      if (argc >= 3 && c->format != argv[2]) continue;
      std::printf("%-5s %-11s %-36s %-24s %s\n", c->formatLabel(), c->instrument ? "instrument" : "effect", c->name.c_str(), c->vendor.c_str(),
                  c->type.c_str());
    }
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "scan-plugins") {  // fill the app's plugin cache now
    std::string dir = std::filesystem::path(gl::Settings::defaultPath()).parent_path().string();
    gl::host::initHosting(dir);
    gl::host::startScan(gl::host::selfExecutablePath(), argc >= 3 && std::string(argv[2]) == "--force");
    gl::host::stopScan();
    auto st = gl::host::scanStatus();
    std::printf("%d plugins scanned, %d hosted classes, %d failed (cache: %s/plugin-cache.json)\n", st.done, st.classes, st.failed, dir.c_str());
    for (const auto& [b, why] : gl::host::blacklist()) std::printf("  failed: %s: %s\n", b.c_str(), why.c_str());
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "import-fl-presets") {
    std::string data, out;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--fl-data") == 0 && i + 1 < argc) data = argv[++i];
      else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
      else {  // never write into the real library on a mistyped option
        std::fprintf(stderr, "unknown option for import-fl-presets: %s\n", argv[i]);
        usage();
        return 2;
      }
    }
    if (out.empty()) out = gl::presetRoot(std::filesystem::path(gl::Settings::defaultPath()).parent_path().string());
    std::printf("converting FL Studio presets into %s\n", out.c_str());
    auto rep = gl::flp::importPresetLibrary(data, out, [](size_t done, size_t total, const std::string&) {
      if (done % 250 == 0 || done == total) std::fprintf(stderr, "\r  %zu / %zu", done, total);
      return true;
    });
    std::fprintf(stderr, "\n");
    if (!rep.error.empty()) {
      std::fprintf(stderr, "error: %s\n", rep.error.c_str());
      return 1;
    }
    std::printf("presets found      %zu\nplugin presets     %zu (%zu exact, %zu FLEX stand-ins)\neffect chains      %zu\nautomation shapes  %zu\nnot converted      %zu\n",
                rep.total, rep.converted, rep.exact, rep.standins, rep.chains, rep.shapes, rep.failed);
    if (!rep.unmappedPlugins.empty()) {
      std::printf("no Gridline equivalent (presets skipped):\n");
      for (const auto& [k, v] : rep.unmappedPlugins) std::printf("  %-34s %d\n", k.c_str(), v);
    }
    for (const auto& [k, v] : rep.errors) std::printf("  error: %s (x%d)\n", k.c_str(), v);
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "flp-plugins") {
    // gridline flp-plugins <file.flp|.fst>...: third-party plugins ("Fruity
    // Wrapper") in FL files and the installed plugin each maps to.
    int total = 0, byId = 0, byName = 0, missing = 0, empty = 0, builtin = 0;
    for (int i = 2; i < argc; ++i) {
      gl::flp::File f;
      std::string err;
      if (!gl::flp::readFile(argv[i], f, &err)) {
        std::fprintf(stderr, "%s: %s\n", argv[i], err.c_str());
        continue;
      }
      gl::flp::Sections sec = gl::flp::split(f);
      std::vector<const std::vector<gl::flp::Event>*> lists;
      for (const auto& [k, evs] : sec.channels) lists.push_back(&evs);
      for (const auto& ins : sec.inserts)
        for (const auto& sl : ins.slots) lists.push_back(&sl.events);
      if (sec.channels.empty() && sec.inserts.empty()) lists.push_back(&f.events);  // presets
      for (const auto* evs : lists) {
        const gl::flp::Event* nm = gl::flp::find(*evs, 201);
        if (!nm || gl::flp::findDecoder(nm->text()) != gl::flp::findDecoder("Fruity Wrapper")) continue;
        const gl::flp::Event* st = gl::flp::find(*evs, 213);
        gl::flp::WrapperInfo w;
        if (!st || !gl::flp::parseWrapper(st->data, st->size, w)) {
          ++empty;
          continue;
        }
        ++total;
        gl::flp::WrapperMatch m = gl::flp::matchWrapper(w);
        std::string how = m.type.empty() ? "MISSING" : m.byId ? "installed, by id" : m.stateKept ? "installed, by name" : "installed, by name, other format";
        std::string to = m.type;
        if (m.type.empty()) {  // what the importer does instead (built-in equivalent or placeholder)
          gl::Song song;
          gl::flp::Report rep;
          gl::flp::Context ctx(f, song, rep);
          gl::flp::PluginMapping pm;
          if ((*gl::flp::findDecoder("Fruity Wrapper"))(ctx, *evs, pm) && pm.glType.rfind("gl.", 0) == 0) {
            how = "not installed: FL plugin -> built-in";
            to = pm.glType;
            ++builtin;
          } else {
            how = "not installed: placeholder";
            ++missing;
          }
        } else {
          (m.byId ? byId : byName)++;
        }
        std::printf("%-5s %-34s %-24s -> %s %s\n   in %s\n", w.formatName(), w.name.c_str(), w.savedType().c_str(), how.c_str(),
                    to.c_str(), argv[i]);
      }
    }
    std::printf("%d wrapped plugins: %d installed (by id), %d installed (by name), %d FL plugins -> built-in, %d placeholders; %d empty wrappers\n",
                total, byId, byName, builtin, missing, empty);
    return 0;
  }
  if (argc >= 3 && std::string(argv[1]) == "coverage-json") {
    // gridline coverage-json <out.json> [--fl-data DIR]: decodes every plugin
    // in FL's shipped projects, preset library and plugin database and counts
    // every project event id (for the coverage report).
    std::string data;
    for (int i = 3; i + 1 < argc; ++i)
      if (std::strcmp(argv[i], "--fl-data") == 0) data = argv[++i];
    if (data.empty()) data = gl::flp::defaultFactoryData();
    if (data.empty()) {
      std::fprintf(stderr, "error: FL Studio data folder not found (use --fl-data)\n");
      return 1;
    }
    gl::flp::CoverageFiles files = gl::flp::coverageFiles(data);
    gl::flp::CoverageTally t;
    size_t done = 0, total = files.projects.size() + files.presets.size() + files.database.size();
    auto run = [&](const std::vector<std::string>& v, gl::flp::FileKind k) {
      for (const auto& f : v) {
        gl::flp::tallyFile(f, k, t, data);
        if (++done % 500 == 0 || done == total) std::fprintf(stderr, "\r  %zu / %zu", done, total);
      }
    };
    run(files.projects, gl::flp::FileKind::Project);
    run(files.presets, gl::flp::FileKind::Preset);
    run(files.database, gl::flp::FileKind::Database);
    std::fprintf(stderr, "\n");
    FILE* fh = std::fopen(argv[2], "wb");
    if (!fh) {
      std::fprintf(stderr, "error: cannot write %s\n", argv[2]);
      return 1;
    }
    std::string js = gl::flp::coverageJson(t);
    std::fwrite(js.data(), 1, js.size(), fh);
    std::fclose(fh);
    std::printf("%d projects, %d presets, %d plugin database entries, %d unreadable -> %s\n", t.projects, t.presets, t.database,
                t.unreadable, argv[2]);
    return 0;
  }
  if (argc < 3) { usage(); return 2; }
  std::string cmd = argv[1];
  std::string project = argv[2];
  if (cmd == "demo") {  // gridline demo <out.json>
    std::string err;
    if (!gl::saveProject(project, gl::makeDemoTrack(), &err)) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 1;
    }
    std::printf("wrote %s\n", project.c_str());
    return 0;
  }

  if (cmd == "import-midi") {  // gridline import-midi <file.mid> -o <out.json>
    std::string out;
    for (int i = 3; i < argc; ++i)
      if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    if (out.empty()) { usage(); return 2; }
    gl::MidiFileData m;
    std::string err;
    if (!gl::readMidiFile(project, m, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    gl::Song song = gl::makeEmptySong();
    song.patterns.clear();
    const std::string name = std::filesystem::path(project).stem().string();
    song.info.title = name;
    gl::MidiImportResult r = gl::importMidiIntoSong(song, m, name);
    if (!r.error.empty()) { std::fprintf(stderr, "error: %s\n", r.error.c_str()); return 1; }
    if (!gl::saveProject(out, song, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    std::printf("wrote %s: %zu channels, %d notes, %d tempo changes, %d automation lanes\n", out.c_str(), r.channels.size(), r.notes,
                r.tempoChanges, r.automationLanes);
    return 0;
  }
  if (cmd == "import-flp") {
    std::string out, data;
    for (int i = 3; i + 1 < argc; ++i) {
      if (std::strcmp(argv[i], "-o") == 0) out = argv[++i];
      else if (std::strcmp(argv[i], "--fl-data") == 0) data = argv[++i];
    }
    if (out.empty()) { usage(); return 2; }
    gl::Song song;
    gl::flp::Report rep;
    std::string err;
    if (!gl::importFlp(project, song, rep, &err, data)) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 1;
    }
    if (!gl::saveProject(out, song, &err)) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 1;
    }
    std::printf("wrote %s\n", out.c_str());
    for (const auto& [k, v] : rep.counts) std::printf("  %-34s %d\n", k.c_str(), v);
    if (!rep.mapped.empty()) {  // J-94: FL plugin → Gridline plugin, by display name
      std::printf("  plugins (FL Studio \xE2\x86\x92 Gridline):\n");
      for (const auto& [k, v] : rep.mapped) std::printf("   %s (%d)\n", k.c_str(), v);
    }
    if (!rep.unmapped.empty()) {
      std::printf("  plugins without a mapped state:\n");
      for (const auto& [k, v] : rep.unmapped) std::printf("    %-32s x%d\n", k.c_str(), v);
    }
    for (const auto& w : rep.warnings) std::printf("  warning: %s\n", w.c_str());
    return 0;
  }

  gl::Song song;
  std::string err;
  if (!gl::loadProject(project, song, &err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  if (cmd == "info") {
    std::printf("title:    %s\n", song.info.title.c_str());
    std::printf("tempo:    %.3f BPM  (%d/%d)  PPQ %d\n", song.tempo, song.timeSigNum,
                song.timeSigDen, song.ppq);
    std::printf("channels: %zu\n", song.channels.size());
    for (size_t i = 0; i < song.channels.size(); ++i) {
      const auto& c = song.channels[i];
      std::printf("  [%zu] #%u %-20s %-11s gain=%.2f pan=%+.2f%s\n", i,
                  c.id, c.name.c_str(), c.plugin.type.c_str(), c.gain, c.pan,
                  c.muted ? " (muted)" : "");
    }
    std::printf("patterns: %zu\n", song.patterns.size());
    size_t audioClips = 0, autoClips = 0;
    for (const auto& c : song.playlist) {
      audioClips += c.kind == gl::ClipKind::Audio;
      autoClips += c.kind == gl::ClipKind::Automation;
    }
    std::printf("clips:    %zu (%zu audio, %zu automation)\n", song.playlist.size(), audioClips, autoClips);
    std::printf("markers:  %zu\n", song.markers.size());
    gl::TempoMap tm;
    tm.build(song);  // follows tempo automation
    std::printf("length:   %lld ticks (%.2f s)\n",
                static_cast<long long>(song.lengthTicks()),
                tm.secondsAt(static_cast<double>(song.lengthTicks())));
    return 0;
  }

  if (cmd == "upgrade") {
    std::string out = project;
    for (int i = 3; i < argc; ++i)
      if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    if (!gl::saveProject(out, song, &err)) {
      std::fprintf(stderr, "error: %s\n", err.c_str());
      return 1;
    }
    std::printf("wrote %s (schema %d)\n", out.c_str(), gl::kProjectSchemaVersion);
    return 0;
  }

  if (cmd == "render-stats") {
    // QA sweeps (J-21): render in memory (optionally only the first S seconds)
    // and report non-finite samples, level, clipping and silence; no file.
    for (int i = 3; i + 1 < argc; ++i)
      if (!std::strcmp(argv[i], "--max-seconds")) {
        gl::TempoMap tm;
        tm.build(song);
        const auto lim = static_cast<gl::Tick>(tm.tickAtSeconds(std::atof(argv[i + 1])));
        std::vector<gl::Clip> kept;
        for (auto c : song.playlist)
          if (c.start < lim) {
            c.length = std::min<gl::Tick>(song.clipLength(c), lim - c.start);
            kept.push_back(c);
          }
        song.playlist = std::move(kept);
      }
    gl::RenderOptions opt;
    opt.tail = song.ppq * 2;
    gl::AudioBuffer buf = gl::renderSong(song, opt);
    if (buf.frames == 0)  // nothing arranged (e.g. FL's render tests): play the first pattern with notes
      for (const auto& p : song.patterns) {
        bool notes = false;
        for (const auto& [ch, ns] : p.notes) notes = notes || !ns.empty();
        for (const auto& [ch, st] : p.steps)
          for (uint8_t v : st) notes = notes || v != 0;
        if (!notes) continue;
        opt.patternMode = true;
        opt.pattern = p.id;
        buf = gl::renderSong(song, opt);
        std::printf("mode: pattern \"%s\"\n", p.name.c_str());
        break;
      }
    if (buf.frames == 0) { std::fprintf(stderr, "error: empty song\n"); return 1; }
    size_t nonFinite = 0, clipped = 0, silent = 0;
    double sum = 0.0, pk = 0.0;
    const size_t ch = buf.channels ? buf.channels : 1;
    for (size_t f = 0; f < buf.frames; ++f) {
      double fp = 0.0;
      for (size_t c = 0; c < ch; ++c) {
        float v = buf.data[c * buf.frames + f];  // plane-major
        if (!std::isfinite(v)) { ++nonFinite; continue; }
        double a = std::fabs(static_cast<double>(v));
        sum += a * a;
        fp = std::max(fp, a);
        clipped += a > 1.0;
      }
      pk = std::max(pk, fp);
      silent += fp < 1e-5;
    }
    std::printf("stats: frames=%zu nonfinite=%zu peak=%.4f rms=%.5f clipped=%zu silentFrac=%.3f\n", buf.frames, nonFinite, pk,
                std::sqrt(sum / static_cast<double>(buf.frames * ch)), clipped,
                static_cast<double>(silent) / static_cast<double>(buf.frames));
    // J-32 loudness sweep: integrated loudness (BS.1770 K-weighting, 400 ms
    // blocks, absolute + relative gates), the peak's time and the longest
    // silent gap between the first and the last sound (sudden dropouts).
    {
      const double fs = opt.sampleRate;
      auto biquad = [](double b0, double b1, double b2, double a0, double a1, double a2) {
        return std::array<double, 5>{b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
      };
      const double pi = 3.14159265358979323846;
      double w = 2 * pi * 1681.974450955533 / fs, A = std::pow(10.0, 3.999843853973347 / 40.0), al = std::sin(w) / (2 * 0.7071752369554196);
      const auto shelf = biquad(A * ((A + 1) + (A - 1) * std::cos(w) + 2 * std::sqrt(A) * al), -2 * A * ((A - 1) + (A + 1) * std::cos(w)),
                                A * ((A + 1) + (A - 1) * std::cos(w) - 2 * std::sqrt(A) * al), (A + 1) - (A - 1) * std::cos(w) + 2 * std::sqrt(A) * al,
                                2 * ((A - 1) - (A + 1) * std::cos(w)), (A + 1) - (A - 1) * std::cos(w) - 2 * std::sqrt(A) * al);
      w = 2 * pi * 38.13547087602444 / fs, al = std::sin(w) / (2 * 0.5003270373238773);
      const auto hp = biquad((1 + std::cos(w)) / 2, -(1 + std::cos(w)), (1 + std::cos(w)) / 2, 1 + al, -2 * std::cos(w), 1 - al);
      std::vector<double> power(buf.frames, 0.0);
      for (size_t c = 0; c < std::min<size_t>(ch, 2); ++c) {
        double s1[4] = {}, s2[4] = {};
        for (size_t f = 0; f < buf.frames; ++f) {
          double x = buf.data[c * buf.frames + f];
          if (!std::isfinite(x)) x = 0;
          double y = shelf[0] * x + shelf[1] * s1[0] + shelf[2] * s1[1] - shelf[3] * s1[2] - shelf[4] * s1[3];
          s1[1] = s1[0], s1[0] = x, s1[3] = s1[2], s1[2] = y;
          double z = hp[0] * y + hp[1] * s2[0] + hp[2] * s2[1] - hp[3] * s2[2] - hp[4] * s2[3];
          s2[1] = s2[0], s2[0] = y, s2[3] = s2[2], s2[2] = z;
          power[f] += z * z;
        }
      }
      const size_t blk = static_cast<size_t>(0.4 * fs), hop = blk / 4;
      std::vector<double> blocks;
      if (blk > 0)
        for (size_t s = 0; s + blk <= buf.frames; s += hop) {
          double e = 0;
          for (size_t f = s; f < s + blk; ++f) e += power[f];
          blocks.push_back(e / static_cast<double>(blk));
        }
      auto lufsOf = [](double ms) { return -0.691 + 10.0 * std::log10(std::max(ms, 1e-30)); };
      auto gated = [&](double gate) {
        double acc = 0;
        size_t n = 0;
        for (double b : blocks)
          if (lufsOf(b) > gate) acc += b, ++n;
        return n ? acc / static_cast<double>(n) : 0.0;
      };
      double lufs = -120;
      if (const double a = gated(-70.0); a > 0) {
        const double r = gated(lufsOf(a) - 10.0);
        lufs = r > 0 ? lufsOf(r) : lufsOf(a);
      }
      size_t first = buf.frames, last = 0, run = 0, gap = 0, peakAt = 0;
      double pmax = 0;
      for (size_t f = 0; f < buf.frames; ++f) {
        double fp = 0;
        for (size_t c = 0; c < ch; ++c) {
          const float v = buf.data[c * buf.frames + f];
          if (std::isfinite(v)) fp = std::max(fp, static_cast<double>(std::fabs(v)));
        }
        if (fp > pmax) pmax = fp, peakAt = f;
        if (fp >= 1e-4) {
          if (first == buf.frames) first = f;
          last = f;
          if (first < f) gap = std::max(gap, run);
          run = 0;
        } else {
          ++run;
        }
      }
      std::printf("loudness: lufs=%.2f peakDb=%.2f peakAt=%.2f maxGap=%.2f firstSound=%.2f lastSound=%.2f seconds=%.2f\n", lufs,
                  20.0 * std::log10(std::max(pk, 1e-12)), static_cast<double>(peakAt) / fs, static_cast<double>(gap) / fs,
                  first == buf.frames ? -1.0 : static_cast<double>(first) / fs, static_cast<double>(last) / fs,
                  static_cast<double>(buf.frames) / fs);
    }
    return 0;
  }

  if (cmd == "render") {
    // gridline render <project> -o <out.wav|.flac|.mp3|.ogg|.m4a|folder/> [options]
    std::string out, scope = "song", formats;
    gl::ExportSettings es;
    es.render.sampleRate = 44100;
    es.wavBits = 16;
    es.render.tail = gl::TailMode::Leave;
    es.render.maxTailSeconds = 10;
    for (int i = 3; i < argc; ++i) {
      auto arg = [&](const char* n) { return !std::strcmp(argv[i], n) && i + 1 < argc; };
      if (arg("-o")) out = argv[++i];
      else if (arg("--sr")) es.render.sampleRate = std::atoi(argv[++i]);
      else if (arg("--bits")) es.wavBits = es.flacBits = std::atoi(argv[++i]);
      else if (arg("--format")) formats = argv[++i];
      else if (arg("--scope")) scope = argv[++i];
      else if (arg("--loops")) es.render.loops = std::atoi(argv[++i]);
      else if (arg("--tail")) {
        std::string t = argv[++i];
        es.render.tail = t == "cut" ? gl::TailMode::Cut : t == "wrap" ? gl::TailMode::Wrap : gl::TailMode::Leave;
      } else if (arg("--dither")) {
        std::string d = argv[++i];
        es.dither = d == "none" ? gl::DitherMode::None : d == "shaped" ? gl::DitherMode::NoiseShaped : gl::DitherMode::Tpdf;
      } else if (arg("--channels")) {
        std::string c = argv[++i];
        es.channels = c == "mono" ? gl::ChannelMode::MonoMerged : c == "left" ? gl::ChannelMode::MonoLeft : c == "right" ? gl::ChannelMode::MonoRight : gl::ChannelMode::Stereo;
      } else if (arg("--bitrate")) es.mp3Bitrate = es.oggBitrate = es.m4aBitrate = std::atoi(argv[++i]);
      else if (arg("--interp")) es.render.interp = static_cast<gl::RenderInterp>(std::clamp(std::atoi(argv[++i]), 0, 7));
      else if (arg("--oversample")) es.render.oversample = std::atoi(argv[++i]);
      else if (arg("--names")) es.partPattern = argv[++i];
      else if (!std::strcmp(argv[i], "--normalize")) es.normalize = true;
      else if (arg("--normalize-lufs")) {  // RN-09: loudness target (LUFS), peak ceiling -1 dBFS unless --ceiling
        es.normalize = es.normalizeLoudness = true;
        es.normalizeLufs = std::atof(argv[++i]);
        if (es.normalizeDb >= 0) es.normalizeDb = -1.0;
      } else if (arg("--ceiling")) es.normalizeDb = std::min(0.0, std::atof(argv[++i]));
      else if (!std::strcmp(argv[i], "--stems")) es.splitMixer = true;
      else if (!std::strcmp(argv[i], "--stems-master-fx")) es.stemsMasterFx = true;
      else if (!std::strcmp(argv[i], "--tracks")) es.splitPlaylist = true;
      else if (!std::strcmp(argv[i], "--midi")) es.midi = true;
      else if (!std::strcmp(argv[i], "--no-master-fx")) es.render.masterEffects = false;
      else if (!std::strcmp(argv[i], "--no-insert-fx")) es.render.insertEffects = false;
      else if (!std::strcmp(argv[i], "--no-poly-limit")) es.render.polyphonyLimits = false;
    }
    if (out.empty()) { usage(); return 2; }
    // Scope: song | pattern:<n> (1-based) | range:<from>-<to> (bars, 1-based, inclusive start).
    if (scope.rfind("pattern", 0) == 0) {
      int n = scope.size() > 8 ? std::atoi(scope.c_str() + 8) : 1;
      if (n < 1 || n > static_cast<int>(song.patterns.size())) { std::fprintf(stderr, "error: no pattern %d\n", n); return 2; }
      es.render.scope = gl::RenderScope::Pattern;
      es.render.pattern = song.patterns[static_cast<size_t>(n - 1)].id;
    } else if (scope.rfind("range:", 0) == 0) {
      int a = 1, b = 1;
      if (std::sscanf(scope.c_str() + 6, "%d-%d", &a, &b) != 2 || b <= a) { std::fprintf(stderr, "error: --scope range:<bar>-<bar>\n"); return 2; }
      es.render.scope = gl::RenderScope::Range;
      es.render.start = (a - 1) * song.barTicks();
      es.render.end = (b - 1) * song.barTicks();
    }
    std::filesystem::path op(out);
    const bool folder = out.back() == '/' || std::filesystem::is_directory(op);
    es.folder = folder ? op.string() : (op.has_parent_path() ? op.parent_path().string() : ".");
    es.baseName = folder ? song.info.title : op.stem().string();
    es.projectTitle = song.info.title;
    es.formats.clear();
    std::string fl = formats.empty() ? (folder ? std::string("wav") : op.extension().string().substr(op.extension().string().empty() ? 0 : 1)) : formats;
    for (size_t a = 0; a <= fl.size();) {
      size_t b = fl.find(',', a);
      if (b == std::string::npos) b = fl.size();
      std::string f = fl.substr(a, b - a);
      for (int k = 0; k < static_cast<int>(gl::ExportFormat::Count); ++k)
        if (f == gl::exportFormatExt(static_cast<gl::ExportFormat>(k)) + 1) es.formats.push_back(static_cast<gl::ExportFormat>(k));
      if (f == "mid" || f == "midi") es.midi = true;
      a = b + 1;
    }
    if (es.formats.empty() && !es.midi) { std::fprintf(stderr, "error: unknown format '%s'\n", fl.c_str()); return 2; }
    for (auto f : es.formats)
      if (gl::exportFormatExternal(f) && gl::encoderFor(f).empty()) {
        std::fprintf(stderr, "error: %s export: %s\n", gl::exportFormatName(f), gl::encoderStatus(f).c_str());
        return 1;
      }
    gl::ExportOutcome res = gl::runExport(song, es, [](double p, const std::string&) {
      std::fprintf(stderr, "\r  rendering %3d%%", static_cast<int>(p * 100));
    });
    std::fprintf(stderr, "\n");
    for (const auto& e : res.errors) std::fprintf(stderr, "error: %s\n", e.c_str());
    if (res.peak > 1.0f && !es.normalize)
      std::fprintf(stderr, "warning: peak %.3f exceeds 0 dBFS — integer formats clip (re-run with --normalize)\n", res.peak);
    for (const auto& f : res.files) std::printf("wrote %s\n", f.c_str());
    std::printf("%lld frames, %.2f s at %d Hz, peak %.3f, %.1f LUFS%s\n", static_cast<long long>(res.frames),
                static_cast<double>(res.frames) / es.render.sampleRate, es.render.sampleRate, res.peak, res.lufs,
                res.ceilingLimited ? " (held back by the peak ceiling)" : "");
    return res.errors.empty() && !res.files.empty() ? 0 : 1;
  }

  if (cmd == "export-midi") {
    std::string out;
    int pattern = 0;
    for (int i = 3; i < argc; ++i) {
      if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
      else if (!std::strcmp(argv[i], "--pattern") && i + 1 < argc) pattern = std::atoi(argv[++i]);
    }
    if (out.empty()) { usage(); return 2; }
    gl::MidiFileData m = pattern > 0 && pattern <= static_cast<int>(song.patterns.size())
                             ? gl::patternToMidi(song, song.patterns[static_cast<size_t>(pattern - 1)].id)
                             : gl::songToMidi(song);
    if (m.tracks.empty() || !gl::writeMidiFile(out, m, &err)) {
      std::fprintf(stderr, "error: %s\n", m.tracks.empty() ? "no notes" : err.c_str());
      return 1;
    }
    std::printf("wrote %s (%zu tracks)\n", out.c_str(), m.tracks.size());
    return 0;
  }

  if (cmd == "collect" || cmd == "zip" || cmd == "bones") {
    std::string out;
    for (int i = 3; i < argc; ++i)
      if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    if (out.empty()) { usage(); return 2; }
    const std::string name = gl::safeFileName(song.info.title.empty() ? "Project" : song.info.title);
    gl::CollectReport rep;
    if (cmd == "zip") rep = gl::exportZippedProject(song, out, name);
    else if (cmd == "bones") rep = gl::exportProjectBones(song, out, name).files;
    else rep = gl::exportProjectDataFiles(song, out, name);
    for (const auto& m : rep.missing) std::fprintf(stderr, "missing: %s\n", m.c_str());
    if (!rep.error.empty()) { std::fprintf(stderr, "error: %s\n", rep.error.c_str()); return 1; }
    std::printf("wrote %s (%d files copied, %zu missing)\n", out.c_str(), rep.copied + rep.reused, rep.missing.size());
    return 0;
  }

  usage();
  return 2;
}
