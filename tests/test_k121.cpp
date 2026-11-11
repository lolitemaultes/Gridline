// Gridline — K-121: single project extensions and one Gridline per user.
//   1. names: .gridline / .gridlinez are projects, the older .gridline.json / .gridline.zip still
//      are; new saves get the new names, Save as can still choose the older ones;
//   2. the places that write or look for project files: templates, backups, the browser, zipped
//      projects, collaborative project folders made by earlier versions;
//   3. the single-instance conversation: encoding, a server and a later launch on a private
//      endpoint, a stale socket after a crash, the socket's permissions.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "collab/Session.h"
#include "core/Backup.h"
#include "core/BrowserIndex.h"
#include "core/Platform.h"
#include "core/Project.h"
#include "core/ProjectArchive.h"
#include "core/ProjectFiles.h"
#include "core/ProjectInterop.h"
#include "core/SingleInstance.h"
#include "core/Templates.h"
#include "core/Zip.h"
#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace gl;
namespace fs = std::filesystem;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

std::string scratch;

std::vector<std::string> namesIn(const fs::path& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) out.push_back(e.path().filename().u8string());
  std::sort(out.begin(), out.end());
  return out;
}

// ---------------------------------------------------------------- names

void testNames() {
  expect(std::string(kProjectExt) == ".gridline" && std::string(kZippedProjectExt) == ".gridlinez", "the new extensions");
  for (const char* p : {"/a/Song.gridline", "/a/Song.GRIDLINE", "/a/Song.gridline.json", "Mix 2.Gridline.Json"})
    expect(isGridlineProjectName(p) && openKindOf(p) == OpenKind::GridlineProject && opensAsProject(p), std::string("project name: ") + p);
  for (const char* p : {"/a/Song.gridlinez", "/a/SONG.GridlineZ", "/a/Song.gridline.zip"})
    expect(isGridlineZipName(p) && openKindOf(p) == OpenKind::ZippedProject && opensAsProject(p), std::string("zipped project name: ") + p);
  // A name that is only the extension is a hidden file or folder, never a project.
  expect(!isGridlineProjectName("/home/u/.gridline") && openKindOf("/home/u/.gridline") == OpenKind::Unknown, ".gridline alone is no project");
  expect(!isGridlineZipName("/home/u/.gridlinez") && openKindOf("/home/u/.gridlinez") == OpenKind::Unknown, ".gridlinez alone is no project");
  expect(!isGridlineProjectName("/a/gridline") && !isGridlineProjectName("/a/x.gridline-collab") && !isGridlineProjectName("/a/x.gridlines"),
         "other names that contain gridline are no projects");
  expect(!isGridlineZipName("/a/Song.zip") && !isGridlineProjectName("/a/preset.json"), "plain .zip / .json are not Gridline's own names");

  // New saves: the new names; the older ones are a choice in Save as.
  expect(withSaveExtension("Song", SaveFormat::Gridline) == "Song.gridline", "default project name");
  expect(withSaveExtension("Song", SaveFormat::GridlineZip) == "Song.gridlinez", "default zipped name");
  expect(withSaveExtension("Song", SaveFormat::GridlineJson) == "Song.gridline.json", "Save as: older project name");
  expect(withSaveExtension("Song", SaveFormat::GridlineZipOld) == "Song.gridline.zip", "Save as: older zipped name");
  expect(withSaveExtension("Song.gridline.json", SaveFormat::Gridline) == "Song.gridline" &&
             withSaveExtension("Song.gridline", SaveFormat::GridlineJson) == "Song.gridline.json",
         "switching between the two names");
  expect(withSaveExtension("Song.gridlinez", SaveFormat::Gridline) == "Song.gridline", "zipped -> project name (no z left)");
  expect(saveFormatOf("x.gridline") == SaveFormat::Gridline && saveFormatOf("x.gridline.json") == SaveFormat::GridlineJson &&
             saveFormatOf("x.gridlinez") == SaveFormat::GridlineZip && saveFormatOf("x.gridline.zip") == SaveFormat::GridlineZipOld,
         "the format of each name");
  expect(isGridlineText(SaveFormat::Gridline) && isGridlineText(SaveFormat::GridlineJson) && !isGridlineText(SaveFormat::GridlineZip) &&
             isGridlineZip(SaveFormat::GridlineZip) && isGridlineZip(SaveFormat::GridlineZipOld) && !isGridlineZip(SaveFormat::FlZip),
         "text and zip groups");
  expect(projectStemOf("/a/Night Drive.gridline") == "Night Drive" && projectStemOf("/a/Night Drive.gridline.json") == "Night Drive" &&
             projectStemOf("/a/Night Drive.gridlinez") == "Night Drive" && projectStemOf("/a/Night Drive.gridline.zip") == "Night Drive",
         "the stem of every name");
  const auto& save = saveFileTypes();
  expect(save[0].exts == std::vector<std::string>{".gridline"}, "Save as starts with the new project name");
  expect(projectFileNameInZip("/a/S.gridlinez", "S") == "S.gridline" && projectFileNameInZip("/a/S.gridline.zip", "S") == "S.gridline.json" &&
             projectFileNameInZip("/a/S.zip", "S") == "S.gridline",
         "the project's name inside a zip follows the zip's name");

  // The browser lists all of them as projects.
  using browse::Kind;
  for (const char* p : {"/a/x.gridline", "/a/x.gridlinez", "/a/x.gridline.json", "/a/x.gridline.zip"})
    expect(browse::kindOf(p) == Kind::Project, std::string("browser: project ") + p);
  expect(browse::kindOf("/a/.gridline") != Kind::Project, "browser: a hidden .gridline file is no project");
}

// ---------------------------------------------------------------- files

void testOpenBoth() {
  const std::string dir = scratch + "/open ü";
  fs::create_directories(fs::u8path(dir));
  Song s = makeEmptySong();
  s.info.title = "Both";
  std::string err;
  const std::string a = dir + "/New.gridline", b = dir + "/Old.gridline.json";
  expect(saveProject(a, s, &err) && saveProject(b, s, &err), "both names save (" + err + ")");
  Song x, y;
  expect(loadProject(a, x, &err) && x.info.title == "Both", "a .gridline project opens (" + err + ")");
  expect(loadProject(b, y, &err) && y.info.title == "Both", "a .gridline.json project opens (" + err + ")");

  // Zipped projects under both names open (unpacked).
  const std::string z1 = dir + "/Z new.gridlinez", z2 = dir + "/Z old.gridline.zip";
  expect(exportZippedProject(s, z1, "Z new").error.empty() && exportZippedProject(s, z2, "Z old").error.empty(), "both zipped names write");
  std::string p1, p2;
  expect(unpackZippedProject(z1, dir + "/u1", p1, &err) && fs::u8path(p1).filename().u8string() == "Z new.gridline", "new zip holds a .gridline");
  expect(unpackZippedProject(z2, dir + "/u2", p2, &err) && fs::u8path(p2).filename().u8string() == "Z old.gridline.json",
         "older zip holds a .gridline.json (earlier versions open it)");
  // A zip made by an earlier version and a zip with both kinds: the project is found either way.
  std::vector<ZipEntry> entries = {{"Song.gridline.json", 10}, {"Samples/kick.wav", 10}};
  expect(archive::pickProject(entries, "Song").names == std::vector<std::string>{"Song.gridline.json"}, "an earlier version's zip");
  entries = {{"Song.gridline", 10}, {"other.json", 10}};
  expect(archive::pickProject(entries, "Song").names == std::vector<std::string>{"Song.gridline"}, "a .gridline is the project, not other .json");

  // Project data files and the folder export write the new name.
  CollectReport r = exportProjectDataFiles(s, dir + "/data", "Data");
  expect(r.error.empty() && fs::exists(fs::u8path(dir + "/data/Data.gridline")), "project data files: <name>.gridline");
}

void testTemplatesAndBackups() {
  const std::string root = scratch + "/templates";
  Song s = makeEmptySong();
  std::string path, err;
  // An older template in the same folder, then the same one saved again.
  fs::create_directories(fs::u8path(root + "/Mine"));
  expect(saveProject(root + "/Mine/Groove.gridline.json", s, &err) && saveProject(root + "/Mine/Old one.gridline.json", s, &err), "older templates");
  auto list = scanTemplates({{root, true}});
  expect(list.size() == 2, "older templates list (" + std::to_string(list.size()) + ")");
  expect(saveTemplate(root, "Mine", "Groove", "", s, &path, &err) && fs::u8path(path).filename().u8string() == "Groove.gridline",
         "a template is saved as .gridline (" + err + ")");
  expect(!fs::exists(fs::u8path(root + "/Mine/Groove.gridline.json")), "saving it again replaced its older name");
  list = scanTemplates({{root, true}});
  bool newOne = false;
  for (const auto& t : list) newOne = newOne || (t.name == "Groove" && fs::u8path(t.path).filename().u8string() == "Groove.gridline");
  expect(list.size() == 2 && newOne, "the template list has the new file and the older one");
  // Both names in one folder (copied by hand): one entry, the new name.
  expect(saveProject(root + "/Mine/Groove.gridline.json", s, &err), "an older copy beside it");
  list = scanTemplates({{root, true}});
  size_t grooves = 0;
  for (const auto& t : list)
    if (t.name == "Groove") {
      ++grooves;
      newOne = fs::u8path(t.path).filename().u8string() == "Groove.gridline";
    }
  expect(grooves == 1 && newOne, "one Groove, the new file");

  BackupPolicy pol;
  pol.globalFolder = scratch + "/backups";
  BackupManager bm(pol);
  const std::string proj = scratch + "/Song.gridline.json";
  fs::create_directories(fs::u8path(pol.globalFolder));
  expect(saveProject(pol.globalFolder + "/Song (backup 2020-01-01 10-00-00).gridline.json", s, &err), "an older backup");
  const std::string f = bm.write(s, proj, 1700000000, &err);
  expect(!f.empty() && f.size() > 9 && f.compare(f.size() - 10, 10, ").gridline") == 0, "a backup is named .gridline: " + f);
  expect(bm.list(proj).size() == 2 && bm.list(scratch + "/Song.gridline").size() == 2, "backups of both names are listed, oldest first");
}

void testCollabFolders() {
  using collab::Session;
  const fs::path dir = fs::u8path(scratch + "/collab é");
  fs::create_directories(dir);
  Song s = makeEmptySong();
  std::string err;
  // A folder made by an earlier version: its file is found, then renamed to the new name.
  expect(saveProject((dir / "Night Drive.gridline.json").u8string(), s, &err), "an earlier version's project file");
  expect(fs::u8path(Session::projectFileOf(dir.u8string())).filename().u8string() == "Night Drive.gridline.json", "found by its older name");
  const std::string got = Session::renameProjectFileIn(dir.u8string(), "Night Drive.gridline.json", "Night Drive", &err);
  expect(got == "Night Drive.gridline" && namesIn(dir) == std::vector<std::string>{"Night Drive.gridline"}, "renamed to the new name: " + got);
  expect(fs::u8path(Session::projectFileOf(dir.u8string())).filename().u8string() == "Night Drive.gridline", "found by its new name");
  expect(std::string(collab::kProjectFileExt) == ".gridline", "collaborative project files are .gridline");
}

// ---------------------------------------------------------------- one Gridline

void testConversation() {
  using namespace single;
  std::vector<std::string> files;
  const std::vector<std::string> in = {"/a/Song A.gridline", "C:\\Users\\someone\\Zoë\\Été mix 2.gridline.json", "/b/line\nbreak.xm"};
  const std::string enc = encodeRequest(in);
  expect(decodeRequest(enc, files) == Parse::Done && files == in, "a request round trip (spaces, accents, a line break)");
  expect(decodeRequest(encodeRequest({}), files) == Parse::Done && files.empty(), "a request without files");
  bool partial = true;
  for (size_t n = 0; n < enc.size(); ++n) partial = partial && decodeRequest(enc.substr(0, n), files) == Parse::NeedMore;
  expect(partial, "every cut of a request needs more");
  expect(decodeRequest("HELLO 1\n", files) == Parse::Bad, "another greeting is refused");
  expect(decodeRequest("OPEN 99999\n", files) == Parse::Bad, "too many files are refused");
  expect(decodeRequest("OPEN 1\n0\n", files) == Parse::Bad, "an empty path is refused");
  expect(decodeRequest("OPEN 1\n999999999\n", files) == Parse::Bad, "an overlong path is refused");
  expect(decodeRequest(enc + "x", files) == Parse::Bad, "bytes after the request are refused");
  expect(decodeRequest(std::string("OPEN 1\n3\na") + '\0' + "b", files) == Parse::Bad, "a path with a NUL is refused");
  // K-153: the later launch's activation token goes before OPEN; older requests (none) still parse.
  std::string token;
  const std::string withToken = encodeRequest(in, "gnome-shell/Gridline/1234-0-host_TIME55");
  expect(decodeRequest(withToken, files, &token) == Parse::Done && files == in && token == "gnome-shell/Gridline/1234-0-host_TIME55",
         "a request with an activation token round trips");
  expect(decodeRequest(enc, files, &token) == Parse::Done && token.empty(), "a request without a token has none");
  bool partialToken = true;
  for (size_t n = 0; n < withToken.size(); ++n) partialToken = partialToken && decodeRequest(withToken.substr(0, n), files, &token) == Parse::NeedMore;
  expect(partialToken, "every prefix of a request with a token needs more");
  expect(decodeRequest("ACTIVATE 3\na b" + encodeRequest({}), files, &token) == Parse::Bad, "a token with a space is refused");
  expect(decodeRequest("ACTIVATE 99999\n" + encodeRequest({}), files, &token) == Parse::Bad, "an overlong token is refused");
  expect(decodeRequest("ACTIVATE 1\nx", files, &token) == Parse::NeedMore, "a token alone needs the OPEN after it");
  expect(decodeRequest("ACTIVATE 1\nxCLOSE", files, &token) == Parse::Bad, "anything but OPEN after the token is refused");
  expect(decodeRequest(encodeRequest(in, ""), files) == Parse::Done, "an empty token is not sent at all");
}

#if defined(_WIN32)
std::string endpointFor(const std::string& tag) {
  return single::testEndpoint("gridline-test-k121-" + std::to_string(platform::processId()) + "-" + tag);
}
#else
std::string endpointFor(const std::string& tag) {
  // (a short folder: a socket path has to fit in about 100 bytes)
  static const std::string dir = [] {
    const std::string d = "/tmp/glk121-" + std::to_string(platform::processId());
    fs::create_directories(d);
    ::chmod(d.c_str(), 0700);
    return d;
  }();
  return single::testEndpoint(dir + "/" + tag + ".sock");
}
#endif

void testServer() {
  using namespace single;
  const std::string ep = endpointFor("a");
  expect(handOver(ep, {"/x.gridline"}, nullptr, 1000) == Handover::NoServer, "nobody listens yet");
  Server first;
  std::string err;
  expect(first.claim(ep, &err) == Server::Claim::Taken && first.listening(), "the first Gridline takes the endpoint (" + err + ")");
  first.start({});
#if !defined(_WIN32)
  struct stat st{};
  expect(::lstat(ep.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600, "the socket is private to the user (0600)");
#endif
  Server second;
  expect(second.claim(ep, &err) == Server::Claim::Busy && !second.listening(), "a second one finds it taken");
  const std::vector<std::string> files = {"/p/Song A.gridline", "/p/Été.gridlinez"};
#if !defined(_WIN32)
  platform::unsetEnv("XDG_ACTIVATION_TOKEN");  // (a test run started from a desktop launcher)
  platform::unsetEnv("DESKTOP_STARTUP_ID");
#endif
  expect(handOver(ep, files, &err) == Handover::Sent, "a later launch hands its files over (" + err + ")");
#if !defined(_WIN32)
  platform::setEnv("XDG_ACTIVATION_TOKEN", "token-from-the-desktop_1");  // K-153
#endif
  expect(handOver(ep, {}, &err) == Handover::Sent, "a launch without files hands over too");
#if !defined(_WIN32)
  platform::unsetEnv("XDG_ACTIVATION_TOKEN");
#endif
  std::vector<Request> got;
  for (int i = 0; i < 200 && got.size() < 2; ++i) {
    for (Request& r : first.take()) got.push_back(r);
    if (got.size() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  expect(got.size() == 2 && got[0].files == files && got[1].files.empty(), "the running one has both requests, in order");
  expect(got.size() == 2 && got[0].activation.empty(), "a launch without a token hands none over");
#if !defined(_WIN32)
  expect(got.size() == 2 && got[1].activation == "token-from-the-desktop_1", "K-153: the launch's activation token reaches the running Gridline");
#endif
  first.stop();
  expect(!first.listening(), "stopped");
#if !defined(_WIN32)
  expect(!fs::exists(ep), "the socket is gone after a clean exit");
#endif
  expect(handOver(ep, files, nullptr, 1000) == Handover::NoServer, "after it quit nobody listens");
  Server third;
  expect(third.claim(ep, &err) == Server::Claim::Taken, "the next launch takes its place (" + err + ")");
  third.stop();
}

#if !defined(_WIN32)
void testStaleSocket() {
  using namespace single;
  const std::string ep = endpointFor("stale");
  // What a crash leaves: a socket file that nothing listens on.
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::snprintf(a.sun_path, sizeof a.sun_path, "%s", ep.c_str());
  const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && ::listen(fd, 1) == 0;
  ::close(fd);
  expect(bound && fs::exists(ep), "a stale socket is there");
  expect(handOver(ep, {"/x.gridline"}, nullptr, 1000) == Handover::NoServer, "a stale socket answers nobody");
  Server s;
  std::string err;
  expect(s.claim(ep, &err) == Server::Claim::Taken, "the next launch takes the endpoint anyway (" + err + ")");
  s.start({});
  expect(handOver(ep, {"/y.gridline"}, &err) == Handover::Sent, "and serves it (" + err + ")");
  s.stop();
  // Something else at that path (not a socket) is never removed.
  { std::FILE* f = std::fopen(ep.c_str(), "w"); if (f) std::fclose(f); }
  Server t;
  expect(t.claim(ep, &err) == Server::Claim::Failed && fs::exists(ep), "a file that is no socket stays, and the launch runs on its own");
  fs::remove(ep);
}

void testDefaultEndpoint() {
  const std::string cfg = scratch + "/cfg";
#if defined(__APPLE__)
  expect(single::defaultEndpoint(cfg).empty(), "macOS: no endpoint (the system opens files in the running Gridline)");
#else
  const std::string rt = "/tmp/glk121rt-" + std::to_string(platform::processId());
  fs::create_directories(rt);
  ::chmod(rt.c_str(), 0700);
  platform::setEnv("XDG_RUNTIME_DIR", rt.c_str());
  platform::setEnv("WAYLAND_DISPLAY", "wayland-1");
  platform::unsetEnv("FLATPAK_ID");
  expect(single::defaultEndpoint(cfg) == rt + "/gridline-wayland-1.sock", "in the runtime folder, per display: " + single::defaultEndpoint(cfg));
  ::chmod(rt.c_str(), 0755);
  expect(single::defaultEndpoint(cfg) == (fs::path(cfg) / "gridline-wayland-1.sock").string(),
         "a runtime folder others can enter is not used: the config folder instead");
  fs::remove_all(rt);
#endif
}
#endif

}  // namespace

int main() {
  scratch = test::hermeticTestEnv("k121");
  testNames();
  testOpenBoth();
  testTemplatesAndBackups();
  testCollabFolders();
  testConversation();
  testServer();
#if !defined(_WIN32)
  testStaleSocket();
  testDefaultEndpoint();
  std::error_code ec;
  fs::remove_all("/tmp/glk121-" + std::to_string(platform::processId()), ec);
#endif
  return finish();
}
