// Gridline: K-16 / K-18: the browser's file operations (core/FileOps.h) and favourite plugins.
//   K-18: names checked; rename (sidecars along, collisions, a change of letter case), new folder,
//   move into a folder (into itself refused), the trash (a freedesktop-style trash in a test
//   folder, and the native one under a test HOME), a read-only folder refused; the open project's
//   references follow a rename or a folder move as one undo step that moves the files back on
//   undo and again on redo; the browser index and its tags follow.
//   K-16: favourites round trip through the settings file (a restart), favourites-first ordering.
// Every file operation runs in this test's own temp folder (Hermetic.h).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/Document.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/ProjectInterop.h"
#include "core/Song.h"
#include "host/PluginPrefs.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace gl;
namespace fs = std::filesystem;

namespace {

fs::path root;

void touch(const fs::path& p, const std::string& body = "x") {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << body;
}
bool has(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

void testNames() {
  expect(fileops::checkName("Kick 01").empty(), "a plain name is fine");
  expect(fileops::checkName("kick.wav").empty(), "a name with an extension is fine");
  expect(!fileops::checkName("").empty() && !fileops::checkName("   ").empty(), "empty names are refused");
  expect(!fileops::checkName("a/b").empty() && !fileops::checkName("a:b").empty() && !fileops::checkName("a?").empty(), "reserved characters are refused");
  expect(!fileops::checkName("..").empty() && !fileops::checkName(".hidden").empty(), "dot names are refused");
  expect(!fileops::checkName("CON").empty() && !fileops::checkName("com1.wav").empty(), "names Windows reserves are refused");
  expect(!fileops::checkName("name.").empty() && !fileops::checkName("name ").empty(), "a trailing dot or space is refused");
  expect(!fileops::checkName(std::string(300, 'a')).empty(), "too long is refused");
}

void testPaths() {
  expect(fileops::isInside("/a/b/c.wav", "/a/b") && fileops::isInside("/a/b", "/a/b"), "isInside: a file in a folder, the folder itself");
  expect(!fileops::isInside("/a/bc/d.wav", "/a/b"), "isInside: a sibling with the same prefix is not inside");
  std::string p = "/a/b/c.wav";
  expect(fileops::retargetPath(p, "/a/b/c.wav", "/a/b/d.wav") && p == "/a/b/d.wav", "retarget: the file itself");
  p = "/a/b/sub/c.wav";
  expect(fileops::retargetPath(p, "/a/b", "/a/x") && fs::path(p) == fs::path("/a/x/sub/c.wav"), "retarget: a file inside a moved folder");
  p = "/a/bc/c.wav";
  expect(!fileops::retargetPath(p, "/a/b", "/a/x") && p == "/a/bc/c.wav", "retarget: a sibling folder stays");
  p = "rel/c.wav";
  expect(!fileops::retargetPath(p, "rel/c.wav", "/x.wav"), "retarget: relative references are left alone");
}

void testRename() {
  const fs::path d = root / "rename";
  touch(d / "Snap.wav", "snap");
  touch(d / "Snap.wav.origin.json", "{}");
  touch(d / "Snap.wav.source.json", "{}");
  touch(d / "Tick.wav", "tick");
  auto r = fileops::rename((d / "Snap.wav").string(), "Kick.wav");
  expect(r.ok && has(d / "Kick.wav") && !has(d / "Snap.wav"), "rename: the file has its new name");
  expect(has(d / "Kick.wav.origin.json") && has(d / "Kick.wav.source.json") && !has(d / "Snap.wav.origin.json"), "rename: its sidecars follow");
  expect(r.moves.size() == 3 && r.moves[0].second == (d / "Kick.wav").string(), "rename: the moves list the file first, then the sidecars");
  r = fileops::rename((d / "Tick.wav").string(), "Kick.wav");
  expect(!r.ok && r.error.find("already exists") != std::string::npos && has(d / "Tick.wav"), "rename onto an existing name is refused, nothing moves");
  r = fileops::rename((d / "Tick.wav").string(), "a/b.wav");
  expect(!r.ok && has(d / "Tick.wav"), "rename to an invalid name is refused");
  r = fileops::rename((d / "Kick.wav").string(), "kick.wav");
  bool listed = false;
  for (const auto& e : fs::directory_iterator(d)) listed = listed || e.path().filename() == "kick.wav";
  expect(r.ok && listed, "a change of letter case only (case-insensitive file systems too)");
  r = fileops::rename((d / "nothing.wav").string(), "x.wav");
  expect(!r.ok && !r.error.empty(), "a missing file is reported");
  // Folders.
  touch(d / "Loops" / "a.wav");
  r = fileops::rename((d / "Loops").string(), "Grooves");
  expect(r.ok && has(d / "Grooves" / "a.wav") && !has(d / "Loops"), "rename a folder: its contents go with it");
}

void testMoveAndFolders() {
  const fs::path d = root / "move";
  touch(d / "a.wav");
  touch(d / "a.wav.origin.json", "{}");
  touch(d / "Sub" / "a.wav");
  auto f = fileops::makeFolder(d.string());
  expect(f.ok && fs::is_directory(f.path) && fs::path(f.path).filename() == "New folder", "new folder: \"New folder\"");
  auto f2 = fileops::makeFolder(d.string());
  expect(f2.ok && fs::path(f2.path).filename() == "New folder 2", "a second one: \"New folder 2\"");
  auto r = fileops::moveInto((d / "a.wav").string(), f.path);
  expect(r.ok && has(fs::path(f.path) / "a.wav") && has(fs::path(f.path) / "a.wav.origin.json") && !has(d / "a.wav"), "move into a folder, sidecar along");
  r = fileops::moveInto((fs::path(f.path) / "a.wav").string(), (d / "Sub").string());
  expect(!r.ok && r.error.find("already exists") != std::string::npos, "a move onto an existing name is refused");
  r = fileops::moveInto(f.path, (fs::path(f.path)).string());
  expect(!r.ok, "a folder cannot go into itself");
  fs::create_directories(fs::path(f.path) / "Inner");
  r = fileops::moveInto(f.path, (fs::path(f.path) / "Inner").string());
  expect(!r.ok, "... nor into one of its subfolders");
  r = fileops::moveInto((d / "Sub").string(), f2.path);
  expect(r.ok && has(fs::path(f2.path) / "Sub" / "a.wav"), "a folder moves into another folder");
  std::string err;
  expect(fileops::moveBack(r.moves, &err) && has(d / "Sub" / "a.wav"), "moveBack undoes it");
  expect(fileops::moveAgain(r.moves, &err) && has(fs::path(f2.path) / "Sub" / "a.wav"), "moveAgain redoes it");
}

void testTrash() {
  const fs::path d = root / "trash";
  touch(d / "gone.wav", "1");
  touch(d / "gone.wav.origin.json", "{}");
  touch(d / "again" / "gone.wav", "2");
  const fs::path t = root / "testtrash";
  auto r = fileops::moveToTrash((d / "gone.wav").string(), t.string());
  expect(r.ok && !has(d / "gone.wav") && !has(d / "gone.wav.origin.json"), "trash: the file and its sidecar leave the folder");
  expect(has(t / "files" / "gone.wav") && has(t / "files" / "gone.wav.origin.json"), "trash: they are in the trash's files");
#if !defined(_WIN32)
  expect(has(t / "info" / "gone.wav.trashinfo"), "trash: with a .trashinfo (freedesktop)");
  std::ifstream info(t / "info" / "gone.wav.trashinfo");
  std::string body((std::istreambuf_iterator<char>(info)), std::istreambuf_iterator<char>());
  expect(body.rfind("[Trash Info]\nPath=/", 0) == 0 && body.find("DeletionDate=") != std::string::npos, "trash: the .trashinfo names the place and date");
#endif
  r = fileops::moveToTrash((d / "again" / "gone.wav").string(), t.string());
  expect(r.ok && has(t / "files" / "gone 2.wav"), "trash: a second item of that name gets its own name");
  r = fileops::moveToTrash((d / "again").string(), t.string());
  expect(r.ok && !has(d / "again"), "trash: a folder");
#if !defined(_WIN32)
  // The native trash under a test HOME / XDG_DATA_HOME (never the user's).
  const fs::path home = root / "home";
  fs::create_directories(home);
  platform::setEnv("HOME", home.string().c_str());
  platform::setEnv("XDG_DATA_HOME", (home / "xdg").string().c_str());
  touch(d / "native.wav");
  r = fileops::moveToTrash((d / "native.wav").string());
#if defined(__APPLE__)
  expect(r.ok && has(home / ".Trash" / "native.wav"), "native trash: ~/.Trash (macOS)");
#else
  expect(r.ok && has(home / "xdg" / "Trash" / "files" / "native.wav"), "native trash: the freedesktop trash in XDG_DATA_HOME");
#endif
  // A read-only folder: nothing can be renamed in it.
  const fs::path ro = root / "readonly";
  touch(ro / "locked.wav");
  ::chmod(ro.string().c_str(), 0555);
  if (::access(ro.string().c_str(), W_OK) != 0) {  // (root ignores permissions)
    r = fileops::rename((ro / "locked.wav").string(), "free.wav");
    expect(!r.ok && has(ro / "locked.wav"), "a read-only folder: rename refused with a message");
    expect(!platform::writable(ro.string()), "platform::writable says so (the browser shows the reason)");
  }
  ::chmod(ro.string().c_str(), 0755);
#endif
}

Song songUsing(const std::string& sample, const std::string& loupeTake) {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Kick";
  c.plugin.type = "gl.sampler";
  c.plugin.data = sample;
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  if (s.mixer.size() > 1) {
    s.mixer[1].slots[0].plugin.type = "gl.edison";
    s.mixer[1].slots[0].plugin.data = "{\"path\": \"" + loupeTake + "\", \"v\": 1}";
  }
  return s;
}

void testProjectSide() {
  const fs::path d = root / "project";
  touch(d / "Drums" / "kick.wav");
  touch(d / "Edited audio" / "take.wav");
  Document doc;
  doc.reset(songUsing((d / "Drums" / "kick.wav").string(), (d / "Edited audio" / "take.wav").string()));
  expect(fileops::referencesTo(doc.song(), (d / "Drums").string()) == 1, "references into a folder are counted");
  // Rename the file: the channel follows, one undo step.
  auto r = fileops::rename((d / "Drums" / "kick.wav").string(), "Kick 01.wav");
  std::vector<std::string> heard;
  doc.execute(std::make_unique<fileops::FileMoveCommand>("Renamed", r.moves, std::vector<fileops::Move>{r.moves.front()},
                                                         [&heard](const std::string& from, const std::string& to) { heard.push_back(from + ">" + to); }));
  expect(doc.song().channels.back().plugin.data == (d / "Drums" / "Kick 01.wav").string(), "rename: the channel's sample follows");
  expect(doc.history().position() == 1 && doc.history().undoLabel() == "Renamed", "... one undo step");
  doc.undo();
  expect(doc.song().channels.back().plugin.data == (d / "Drums" / "kick.wav").string() && has(d / "Drums" / "kick.wav") &&
             !has(d / "Drums" / "Kick 01.wav"),
         "undo: the reference and the file go back");
  doc.redo();
  expect(doc.song().channels.back().plugin.data == (d / "Drums" / "Kick 01.wav").string() && has(d / "Drums" / "Kick 01.wav"),
         "redo: renamed again (file and reference)");
  expect(heard.size() == 2, "the index hears undo and redo");
  // Move a folder: the Loupe take inside its JSON state follows.
  touch(d / "Archive" / ".keep");
  r = fileops::moveInto((d / "Edited audio").string(), (d / "Archive").string());
  doc.execute(std::make_unique<fileops::FileMoveCommand>("Moved", r.moves, std::vector<fileops::Move>{r.moves.front()}));
  const auto refs = referencedFiles(doc.song());
  expect(std::find(refs.begin(), refs.end(), (d / "Archive" / "Edited audio" / "take.wav").string()) != refs.end(),
         "a moved folder: the references inside a plugin's JSON state follow");
  doc.undo();
  expect(has(d / "Edited audio" / "take.wav") && fileops::referencesTo(doc.song(), (d / "Edited audio").string()) == 1, "undo: folder and reference back");
}

void testIndex() {
  browse::Index ix;
  ix.setCacheFile((root / "index.json").string());
  const std::string a = (root / "ix" / "a.wav").string(), b = (root / "ix" / "sub" / "b.wav").string();
  ix.setTags(a, {"starred"});
  ix.setTags(b, {"kick"});
  ix.movePath(a, (root / "ix" / "renamed.wav").string());
  expect(ix.tagsOf(a).empty() && ix.tagsOf((root / "ix" / "renamed.wav").string()) == std::vector<std::string>{"starred"}, "index: a renamed file keeps its tags");
  ix.movePath((root / "ix" / "sub").string(), (root / "ix" / "moved").string());
  expect(ix.tagsOf((root / "ix" / "moved" / "b.wav").string()) == std::vector<std::string>{"kick"}, "index: entries inside a moved folder follow");
  ix.movePath((root / "ix" / "moved").string(), "");
  expect(ix.tagsOf((root / "ix" / "moved" / "b.wav").string()).empty(), "index: a trashed folder is forgotten");
}

void testFavourites() {
  const std::string cfg = (root / "cfg").string();
  fs::create_directories(cfg);
  host::loadPluginPrefs(cfg);
  expect(!host::isFavourite("gl.reverb"), "favourites: none at first");
  expect(host::setFavourite("gl.reverb", true) && host::setFavourite("vst3:ABCDEF", true), "favourites: a built-in and a hosted type");
  expect(!host::setFavourite("gl.reverb", true), "favourites: setting it again changes nothing");
  host::loadPluginPrefs((root / "empty").string());  // another config: nothing
  expect(!host::isFavourite("gl.reverb"), "favourites: another user's settings do not have them");
  host::loadPluginPrefs(cfg);  // a restart
  expect(host::isFavourite("gl.reverb") && host::isFavourite("vst3:ABCDEF"), "favourites: kept across a restart (settings round trip)");
  expect(host::setFavourite("vst3:ABCDEF", false) && !host::isFavourite("vst3:ABCDEF"), "favourites: removed");
  host::loadPluginPrefs(cfg);
  expect(!host::isFavourite("vst3:ABCDEF") && host::isFavourite("gl.reverb"), "favourites: the removal is saved too");
  // Picker order: favourites first, each part in its own order.
  std::vector<std::string> list = {"gl.chorus", "gl.delay", "gl.eq", "gl.reverb", "vst3:X"};
  const std::set<std::string> favs = {"gl.reverb", "gl.delay"};
  host::favouritesFirst(list, favs, [](const std::string& t) { return t; });
  expect((list == std::vector<std::string>{"gl.delay", "gl.reverb", "gl.chorus", "gl.eq", "vst3:X"}), "picker order: favourites first, both parts keep their order");
}

}  // namespace

int main() {
  root = fs::path(test::hermeticTestEnv("k18")) / "files";
  fs::create_directories(root);
  testNames();
  testPaths();
  testRename();
  testMoveAndFolders();
  testTrash();
  testProjectSide();
  testIndex();
  testFavourites();
  return finish();
}
