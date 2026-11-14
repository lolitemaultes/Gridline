// Gridline: the Open / Save dialog's model (core/FileDialogModel): mount parsing, places,
// breadcrumbs (POSIX, Windows drive roots, UNC shares), Tab completion, sorting, favourites,
// recent and last folders saved in the settings and read back, the volume watcher's worker, and
// listings with sizes, times and hidden files.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "core/FileDialogModel.h"
#include "core/RemoteFs.h"
#include "core/Settings.h"

using namespace gl;
using namespace gl::filedlg;
namespace fs = std::filesystem;

namespace {

const Volume* find(const std::vector<Volume>& v, const std::string& path) {
  for (const auto& x : v)
    if (x.path == path) return &x;
  return nullptr;
}

std::vector<std::string> names(const std::vector<Entry>& v) {
  std::vector<std::string> out;
  for (const auto& e : v) out.push_back(e.name);
  return out;
}

Entry file(const char* n, int64_t size, int64_t t) {
  Entry e;
  e.name = n;
  e.size = size;
  e.mtime = t;
  e.kind = kindOf(n, false);
  return e;
}
Entry dir(const char* n) {
  Entry e;
  e.name = n;
  e.isDir = true;
  e.kind = FileKind::Folder;
  return e;
}

std::vector<Volume> fakeVolumes() { return {{"Disk", "/", VolumeKind::System, 10, 20}, {"Stick", "/media/x/Stick", VolumeKind::Removable, 1, 2}}; }

}  // namespace

int main() {
  // ---- mounts ----
  const std::string mounts =
      "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
      "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
      "udev /dev devtmpfs rw,nosuid,relatime 0 0\n"
      "tmpfs /run tmpfs rw,nosuid,nodev,noexec 0 0\n"
      "/dev/nvme0n1p2 / ext4 rw,relatime,errors=remount-ro 0 0\n"
      "/dev/nvme0n1p1 /boot/efi vfat rw,relatime 0 0\n"
      "/dev/loop3 /snap/core22/1380 squashfs ro,nodev,relatime 0 0\n"
      "/dev/loop4 /snap/firefox/4173 squashfs ro,nodev,relatime 0 0\n"
      "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n"
      "overlay /var/lib/docker/overlay2/abc/merged overlay rw,relatime 0 0\n"
      "cgroup2 /sys/fs/cgroup cgroup2 rw 0 0\n"
      "/dev/nvme0n1p3 /home ext4 rw,relatime 0 0\n"
      "/dev/sda1 /media/me/USB\\040STICK vfat rw,nosuid,nodev,uid=1000 0 0\n"
      "/dev/sdb1 /run/media/me/Backup exfat rw,nosuid,nodev 0 0\n"
      "/dev/sr0 /media/me/AUDIO_CD iso9660 ro,nosuid,nodev 0 0\n"
      "/dev/sdc1 /mnt/data btrfs rw,relatime 0 0\n"
      "/dev/sdc1 /mnt/data btrfs rw,relatime 0 0\n"
      "server:/export /mnt/nas nfs4 rw,relatime 0 0\n"
      "//host/share /mnt/smb cifs rw,relatime 0 0\n"
      "portal /run/user/1000/doc fuse.portal rw,nosuid,nodev 0 0\n"
      "gvfsd-fuse /run/user/1000/gvfs fuse.gvfsd-fuse rw 0 0\n"
      "tmpfs /mnt/ram tmpfs rw 0 0\n"
      "/dev/sdd1 /home/me/.local/share/thing ext4 rw 0 0\n";
  const std::vector<Volume> v = parseLinuxMounts(mounts, "me");
  for (const auto& x : v) std::printf("# volume %s %s %d\n", x.name.c_str(), x.path.c_str(), static_cast<int>(x.kind));
  expect(find(v, "/") && find(v, "/")->kind == VolumeKind::System && v.front().path == "/", "the root is kept, first, as the system volume");
  expect(find(v, "/home") != nullptr, "a separate data partition is kept");
  expect(find(v, "/media/me/USB STICK") && find(v, "/media/me/USB STICK")->kind == VolumeKind::Removable &&
             find(v, "/media/me/USB STICK")->name == "USB STICK",
         "a USB stick under /media/<user>: removable, its name unescaped");
  expect(find(v, "/run/media/me/Backup") && find(v, "/run/media/me/Backup")->kind == VolumeKind::Removable, "/run/media/<user> is removable");
  expect(find(v, "/media/me/AUDIO_CD") && find(v, "/media/me/AUDIO_CD")->kind == VolumeKind::Optical, "an optical disc");
  expect(find(v, "/mnt/data") && find(v, "/mnt/data")->kind == VolumeKind::Internal, "/mnt entries are kept");
  expect(find(v, "/mnt/nas") && find(v, "/mnt/nas")->kind == VolumeKind::Network, "an NFS share is a network drive");
  expect(find(v, "/mnt/smb") && find(v, "/mnt/smb")->kind == VolumeKind::Network, "an SMB share is a network drive");
  int dataCount = 0;
  for (const auto& x : v) dataCount += x.path == "/mnt/data" ? 1 : 0;
  expect(dataCount == 1, "a mount listed twice shows once");
  bool pseudo = false;
  for (const auto& x : v)
    if (x.path == "/sys" || x.path == "/proc" || x.path == "/dev" || x.path == "/run" || x.path == "/tmp" || x.path.find("/snap") == 0 ||
        x.path.find("/var/") == 0 || x.path.find("/sys/") == 0 || x.path == "/boot/efi" || x.path.find("/run/user") == 0 || x.path == "/mnt/ram" ||
        x.path.find("/.") != std::string::npos)
      pseudo = true;
  expect(!pseudo, "proc, sys, tmpfs, snap images, overlay, cgroup, boot, portals and hidden helper mounts are dropped");
  expect(v.size() == 8, "exactly the eight real volumes");
  expect(parseLinuxMounts("", "me").empty() && parseLinuxMounts("garbage\n\n", "").empty(), "empty or broken text gives no volumes");
  VolumeKind k;
  expect(!keepMount("/dev/loop0", "/media/me/img", "ext4", &k), "a loop image is no drive");
  expect(keepMount("/dev/da0s1", "/media/usb", "msdosfs", &k) && k == VolumeKind::Removable, "BSD: a FAT stick under /media");
  expect(keepMount("/dev/ada0p2", "/", "ufs", &k) && k == VolumeKind::System, "BSD: the UFS root");
  expect(!keepMount("devfs", "/dev", "devfs", &k) && !keepMount("procfs", "/proc", "procfs", &k), "BSD: devfs and procfs dropped");
  expect(keepMount("overlay", "/", "overlay", &k) && k == VolumeKind::System, "an overlay root (container, live session) is still the file system");
  expect(keepMount("tmpfs", "/", "tmpfs", &k) && k == VolumeKind::System, "a tmpfs root (sandbox) is still the file system");

  // ---- places ----
  const std::string userDirs = "# written by xdg-user-dirs-update\nXDG_DESKTOP_DIR=\"$HOME/Schreibtisch\"\nXDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n"
                               "XDG_MUSIC_DIR=\"/data/Musik\"\nXDG_DOCUMENTS_DIR=\"$HOME/\"\n";
  expect(xdgUserDir(userDirs, "DESKTOP", "/home/me") == "/home/me/Schreibtisch", "XDG: a translated desktop folder");
  expect(xdgUserDir(userDirs, "MUSIC", "/home/me") == "/data/Musik", "XDG: an absolute music folder");
  expect(xdgUserDir(userDirs, "DOCUMENTS", "/home/me") == "", "XDG: a folder set to home counts as not set");
  const auto pl = standardPlaces("/home/me", userDirs);
  expect(pl.size() == 5 && pl[0].path == "/home/me" && pl[1].path == "/home/me/Schreibtisch" && pl[2].path == fs::path("/home/me/Documents").u8string() &&
             pl[3].path == "/data/Musik",
         "Home, Desktop, Documents, Music, Downloads (XDG names where set)");
  expect(standardPlaces("", "").empty(), "no home: no places");

  // ---- breadcrumbs ----
  {
    auto c = splitBreadcrumb("/home/me/Music", false);
    expect(c.size() == 4 && c[0].label == "/" && c[0].path == "/" && c[1].path == "/home" && c[3].label == "Music" && c[3].path == "/home/me/Music",
           "POSIX path: root, home, me, Music");
    c = splitBreadcrumb("/", false);
    expect(c.size() == 1 && c[0].path == "/", "the root alone");
    c = splitBreadcrumb("/a//b/./c/", false);
    expect(c.size() == 4 && c[3].path == "/a/b/c", "doubled separators and dots are skipped");
    c = splitBreadcrumb("C:\\Users\\me\\Music", true);
    expect(c.size() == 4 && c[0].label == "C:" && c[0].path == "C:\\" && c[1].path == "C:\\Users" && c[3].path == "C:\\Users\\me\\Music",
           "Windows: the drive root, then the folders");
    c = splitBreadcrumb("d:/", true);
    expect(c.size() == 1 && c[0].label == "D:" && c[0].path == "D:\\", "Windows: a bare drive root (forward slash, lower case)");
    c = splitBreadcrumb("\\\\server\\share\\Songs\\2026", true);
    expect(c.size() == 3 && c[0].label == "\\\\server\\share" && c[0].path == "\\\\server\\share\\" && c[1].path == "\\\\server\\share\\Songs" &&
               c[2].path == "\\\\server\\share\\Songs\\2026",
           "UNC: the share is one part");
    c = splitBreadcrumb("\\\\server\\share", true);
    expect(c.size() == 1 && c[0].path == "\\\\server\\share\\", "UNC: the share root alone");
  }

  // ---- Tab completion ----
  {
    const std::vector<std::string> n = {"Music/", "Musicals.txt", "Movies/", "notes.txt", ".hidden/"};
    expect(completePath("/home/me/Mo", n, false) == "/home/me/Movies/", "one folder: completed with its separator");
    expect(completePath("/home/me/mu", n, false) == "/home/me/Music", "two matches: their common start (case ignored)");
    expect(completePath("/home/me/no", n, false) == "/home/me/notes.txt", "one file: completed");
    expect(completePath("/home/me/zz", n, false) == "/home/me/zz", "no match: unchanged");
    expect(completePath("/home/me/.h", n, false) == "/home/me/.hidden/", "a typed dot reaches hidden names");
    expect(completePath("C:\\Users\\me\\Mo", n, true) == "C:\\Users\\me\\Movies\\", "Windows separators");
    std::string f, r;
    splitTyped("/a/b/mu", false, &f, &r);
    expect(f == "/a/b/" && r == "mu", "a typed path splits into folder and rest");
  }

  // ---- sorting ----
  {
    std::vector<Entry> e = {file("Take 10.wav", 300, 5), dir("zeta"), file("take 2.wav", 100, 9), file("Song.gridline", 200, 1), dir("Alpha"),
                            file("beat.mid", 50, 7)};
    sortEntries(e, SortKey::Name, false);
    expect(names(e) == std::vector<std::string>{"Alpha", "zeta", "beat.mid", "Song.gridline", "take 2.wav", "Take 10.wav"},
           "by name: folders first, case ignored, numbers in natural order");
    sortEntries(e, SortKey::Name, true);
    expect(names(e) == std::vector<std::string>{"zeta", "Alpha", "Take 10.wav", "take 2.wav", "Song.gridline", "beat.mid"}, "by name, reversed: folders still first");
    sortEntries(e, SortKey::Size, false);
    expect(names(e) == std::vector<std::string>{"Alpha", "zeta", "beat.mid", "take 2.wav", "Song.gridline", "Take 10.wav"}, "by size");
    sortEntries(e, SortKey::Modified, true);
    expect(names(e) == std::vector<std::string>{"zeta", "Alpha", "take 2.wav", "beat.mid", "Take 10.wav", "Song.gridline"}, "by date, newest first");
    sortEntries(e, SortKey::Kind, false);
    expect(names(e) == std::vector<std::string>{"Alpha", "zeta", "take 2.wav", "Take 10.wav", "beat.mid", "Song.gridline"}, "by kind (Audio, MIDI, Project)");
    expect(naturalLess("a2", "a10") && !naturalLess("a10", "a2") && naturalLess("a", "a1") && naturalLess("A", "a"), "natural order");
    expect(kindOf("x.gridlinez", false) == FileKind::Project && kindOf("x.FLP", false) == FileKind::Project && kindOf("x.WAV", false) == FileKind::Audio &&
               kindOf("x.mid", false) == FileKind::Midi && kindOf("x.fst", false) == FileKind::Preset && kindOf("x.txt", false) == FileKind::Other &&
               kindOf("x.wav", true) == FileKind::Folder,
           "kinds by name");
    expect(std::string(kindLabel(FileKind::Other, "notes.txt")) == "TXT" && std::string(kindLabel(FileKind::Other, "README")) == "File", "other files by extension");
    expect(sizeText(12) == "12 bytes" && sizeText(1536) == "1.5 KB" && sizeText(5 * 1024 * 1024) == "5.0 MB" && sizeText(-1).empty(), "sizes");
  }

  // ---- favourites, recent and last folders through the settings file ----
  {
    const fs::path dirT = fs::temp_directory_path() / ("gl_k213_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dirT);
    const std::string file = (dirT / "settings.json").u8string();
    std::vector<std::string> fav;
    expect(setPinned(fav, "/home/me/Music", true) && setPinned(fav, "/mnt/data/Songs", true) && !setPinned(fav, "/home/me/Music", true), "pin twice: once");
    std::vector<std::string> recent;
    for (int i = 0; i < 12; ++i) pushRecent(recent, "/r/" + std::to_string(i));
    pushRecent(recent, "/r/5");
    expect(recent.size() == 8 && recent[0] == "/r/5" && recent[1] == "/r/11", "recent: newest first, no doubles, at most eight");
    std::string last = setLastFolder("", "Open project", "/home/me/Songs");
    last = setLastFolder(last, "Load sample", "/data/Samples");
    last = setLastFolder(last, "Open project", "/home/me/Songs 2");
    {
      Settings s;
      expect(s.set("files.dialogFavourites", encodeList(fav)) && s.set("files.dialogRecent", encodeList(recent)) && s.set("files.dialogLast", last) &&
                 s.set("files.dialogHidden", true) && s.set("files.dialogSort", 3) && s.set("files.dialogW", 1000.0),
             "the dialog's settings are registered");
      expect(s.save(file), "saved");
    }
    Settings s2;
    expect(s2.load(file), "loaded");
    const auto fav2 = decodeList(s2.getString("files.dialogFavourites"));
    expect(fav2 == fav, "favourites read back as saved");
    expect(decodeList(s2.getString("files.dialogRecent")) == recent, "recent folders read back");
    expect(lastFolder(s2.getString("files.dialogLast"), "Open project") == "/home/me/Songs 2" &&
               lastFolder(s2.getString("files.dialogLast"), "Load sample") == "/data/Samples" && lastFolder(s2.getString("files.dialogLast"), "Load") == "",
           "the last folder per dialog");
    expect(s2.getBool("files.dialogHidden") && s2.getNumber("files.dialogSort") == 3 && s2.getNumber("files.dialogW") == 1000.0, "hidden, sort and size kept");
    std::vector<std::string> fav3 = fav2;
    expect(setPinned(fav3, "/home/me/Music", false) && fav3 == std::vector<std::string>{"/mnt/data/Songs"} && !contains(fav3, "/home/me/Music"), "unpin");
    expect(decodeList("a\r\n\nb\na\n") == std::vector<std::string>{"a", "b"}, "decode: blank lines, CR and doubles dropped");

    // ---- listings with details and hidden files ----
    fs::create_directories(dirT / "sub");
    { std::ofstream(dirT / "a.wav") << "1234567890"; }
    { std::ofstream(dirT / ".secret") << "x"; }
    auto l = remotefs::listNow(dirT.u8string(), remotefs::kListDetails);
    bool sawHidden = false, sized = false, dated = false;
    for (const auto& e : l.entries) {
      sawHidden |= e.hidden;
      if (e.name == "a.wav") sized = e.size == 10, dated = e.mtime > 1600000000;
    }
    expect(l.ok && !sawHidden && sized && dated, "details: size and time; hidden names left out by default");
    l = remotefs::listNow(dirT.u8string(), remotefs::kListHidden);
    sawHidden = false;
    for (const auto& e : l.entries) sawHidden |= e.hidden && e.name == ".secret";
    expect(sawHidden, "kListHidden: hidden names listed and marked");
    auto plain = remotefs::listNow(dirT.u8string());
    bool unsized = true;
    for (const auto& e : plain.entries) unsized &= e.size == -1 && e.mtime == 0 && !e.hidden;
    expect(plain.entries.size() == 3 && unsized, "plain listing unchanged: no hidden names, no stat");  // settings.json, sub, a.wav
    fs::remove_all(dirT);
  }

  // ---- the volume watcher reads on a worker and never waits ----
  {
    VolumeWatch::setReaderForTests(&fakeVolumes);
    VolumeWatch w;
    auto first = w.poll(0.0);
    for (int i = 0; i < 200 && !w.poll(10.0); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto got = w.poll(10.0);
    expect(got && got->size() == 2 && (*got)[1].kind == VolumeKind::Removable, "the worker's list arrives");
    expect(w.reads() == 1, "within maxAge: no second read");
    VolumeWatch::setReaderForTests(nullptr);
    (void)first;
    const auto real = detectVolumes();
    bool root = false;
    for (const auto& x : real) root |= x.kind == VolumeKind::System;
    for (const auto& x : real) std::printf("# this computer: %s at %s, kind %d, free %lld\n", x.name.c_str(), x.path.c_str(), static_cast<int>(x.kind), static_cast<long long>(x.freeBytes));
    expect(root, "this computer: the system volume is found");
  }
  return finish();
}
