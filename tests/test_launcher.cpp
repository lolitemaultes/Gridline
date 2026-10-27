// Gridline - J-141: the welcome launcher's logic: the start-up plan for every way Gridline
// starts, the launcher state machine (logo, back, open, close), the recent list (order,
// cap, duplicates, removal, missing files, persistence, ages), project sketches and their
// cache (stamps, invalidation), and a layout audit at many window sizes and interface scales.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Launcher.h"
#include "core/Project.h"
#include "core/ProjectPreview.h"
#include "core/Recents.h"
#include "core/Song.h"
#include "core/UiScale.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void testStartupPlan() {
  using launcher::StartAction;
  launcher::StartupInputs in;
  // First launch on a machine: the demo song, the launcher waits behind it.
  in.freshInstall = true;
  in.demoShown = false;
  auto p = launcher::planStartup(in);
  expect(p.action == StartAction::Demo && !p.showLauncher && p.markDemoShown, "fresh install: the demo song opens, no launcher, demo marked shown");
  // Fresh install started with a file: the demo still loads first (the file opens over it, as before).
  in.plainLaunch = false;
  p = launcher::planStartup(in);
  expect(p.action == StartAction::Demo && !p.showLauncher, "fresh install with a file: demo then the file, no launcher");
  // Later plain launches: the launcher.
  in = {};
  in.demoShown = true;
  p = launcher::planStartup(in);
  expect(p.showLauncher && p.action == StartAction::None && !p.markDemoShown, "plain launch: the launcher, nothing else opens");
  // Settings that existed before the demo flag (not fresh): the launcher, flag set.
  in.demoShown = false;
  p = launcher::planStartup(in);
  expect(p.showLauncher && p.markDemoShown && p.action == StartAction::None, "older settings: launcher, demo flag set without the demo");
  // Launcher off: the old On startup modes.
  in = {};
  in.demoShown = true;
  in.launcherOn = false;
  p = launcher::planStartup(in);
  expect(!p.showLauncher && p.action == StartAction::None, "launcher off, template mode, built-in starter: keep the starter project");
  in.defaultTemplate = true;
  expect(launcher::planStartup(in).action == StartAction::Template, "launcher off, default template set: the template");
  in.startupMode = "empty";
  expect(launcher::planStartup(in).action == StartAction::Empty, "launcher off, empty mode: an empty project");
  in.startupMode = "last";
  in.lastExists = true;
  expect(launcher::planStartup(in).action == StartAction::Last, "launcher off, last mode: the last project");
  in.lastExists = false;
  expect(launcher::planStartup(in).action != StartAction::Last, "last mode with the last project missing: not opened");
  // Files or options on the command line: never the launcher, the old modes run.
  in = {};
  in.demoShown = true;
  in.plainLaunch = false;
  p = launcher::planStartup(in);
  expect(!p.showLauncher, "started with files / options: no launcher");
  in.startupMode = "last";
  in.lastExists = true;
  expect(launcher::planStartup(in).action == StartAction::Last && !launcher::planStartup(in).showLauncher,
         "started with files, last mode: the old behaviour, no launcher");
  // --show launcher forces it, even with the setting off and options present.
  in = {};
  in.demoShown = true;
  in.plainLaunch = false;
  in.launcherOn = false;
  in.forceLauncher = true;
  expect(launcher::planStartup(in).showLauncher, "--show launcher: shown whatever the setting");
}

void testStateMachine() {
  launcher::State s;
  expect(!s.visible() && s.projectOpen(), "initial: hidden, the constructor's project open");
  s.showAtStartup(1.0);
  expect(s.visible() && !s.projectOpen(), "start-up: shown with no project open");
  expect(!s.back(1.1) && s.visible(), "back with no project open: refused, still shown");
  s.projectOpened(2.0);
  expect(!s.visible() && s.projectOpen() && s.changedAt() == 2.0, "a project opened: hidden, open, change stamped");
  s.showHome(3.0);
  expect(s.visible() && s.projectOpen(), "logo: shown, the project kept open");
  expect(s.back(4.0) && !s.visible() && s.projectOpen(), "back: the project view again");
  s.showHome(5.0);
  s.projectOpened(6.0);
  expect(!s.visible() && s.projectOpen(), "opening another project from the launcher: hidden with it open");
  s.showHome(7.0);
  s.projectClosed(8.0);
  expect(s.visible() && !s.projectOpen() && !s.back(8.1), "close project: shown, nothing open, no way back");
  s.projectOpened(9.0);
  s.projectOpened(10.0);
  expect(s.changedAt() == 9.0, "no visibility change: stamp unchanged");
}

void testRecents(const std::string& dir) {
  recents::List l;
  l.touch("/a.gridline.json", 100);
  l.touch("/b.gridline.json", 200);
  l.touch("/c.gridline.json", 300);
  expect(l.paths.size() == 3 && l.paths[0] == "/c.gridline.json" && l.paths[2] == "/a.gridline.json", "most recent first");
  l.touch("/a.gridline.json", 400);
  expect(l.paths.size() == 3 && l.paths[0] == "/a.gridline.json" && l.paths[1] == "/c.gridline.json", "reopening moves to the front, no duplicate");
  expect(l.openedAt("/a.gridline.json") == 400 && l.openedAt("/b.gridline.json") == 200, "opened times kept per path");
  for (int i = 0; i < 40; ++i) l.touch("/p" + std::to_string(i), 1000 + i);
  expect(l.paths.size() == recents::kMaxRecent && l.paths[0] == "/p39", "capped at the maximum, newest kept");
  expect(l.openedAt("/a.gridline.json") == 0, "dropped paths lose their times");
  expect(l.remove("/p39") && !l.remove("/p39") && l.paths[0] == "/p38", "remove: once, the next moves up");
  // Persistence (and tolerant loading).
  expect(recents::save(dir, l), "saved");
  recents::List r = recents::load(dir);
  expect(r.paths == l.paths && r.openedAt("/p38") == 1038, "loaded back in order with times");
  {
    std::ofstream f(fs::path(dir) / "recent.txt");
    f << "/x\n\n/y\r\n/x\n/z\n";
  }
  r = recents::load(dir);
  expect(r.paths.size() == 3 && r.paths[0] == "/x" && r.paths[1] == "/y" && r.paths[2] == "/z", "blank lines, CRs and duplicates dropped");
  expect(r.openedAt("/x") == 0, "times of paths not in the list are ignored");
  // Missing files.
  const std::string real = (fs::path(dir) / "real.gridline.json").string();
  { std::ofstream f(real); f << "{}"; }
  recents::List m;
  m.touch((fs::path(dir) / "gone.gridline.json").string(), 1);
  m.touch(real, 2);
  m.touch((fs::path(dir) / "gone2.gridline.json").string(), 3);
  expect(recents::exists(real) && !recents::exists((fs::path(dir) / "gone.gridline.json").string()), "exists");
  expect(m.pruneMissing([](const std::string& p) { return recents::exists(p); }) == 2 && m.paths.size() == 1 && m.paths[0] == real,
         "remove missing: only the files on disk stay");
  // Ages.
  using recents::AgeUnit;
  expect(recents::age(0, 1000).unit == AgeUnit::None, "unknown time: no age");
  expect(recents::age(1000, 1030).unit == AgeUnit::JustNow, "just now");
  expect(recents::age(1000, 1000 + 5 * 60).unit == AgeUnit::Minutes && recents::age(1000, 1000 + 5 * 60).count == 5, "5 min");
  expect(recents::age(1000, 1000 + 3 * 3600).count == 3 && recents::age(1000, 1000 + 3 * 3600).unit == AgeUnit::Hours, "3 h");
  expect(recents::age(1000, 1000 + 30 * 3600).unit == AgeUnit::Yesterday, "yesterday");
  expect(recents::age(1000, 1000 + 4 * 86400).unit == AgeUnit::Days && recents::age(1000, 1000 + 4 * 86400).count == 4, "4 days");
  expect(recents::age(1000, 1000 + 30 * 86400).unit == AgeUnit::Date, "older: a date");
  expect(recents::age(2000, 1000).unit == AgeUnit::JustNow, "clock skew: just now");
}

Song arrangementSong() {
  Document d(makeEmptySong());
  const ObjectId kick = d.addChannel("Kick", "gl.kick");
  const ObjectId p1 = d.song().patterns[0].id;
  const ObjectId p2 = d.addPattern("Pattern 2", 2);
  d.editPattern(p1, "", [&](Pattern& p) {
    p.color = 0x123456;
    p.steps[kick] = std::vector<uint8_t>(16, 0);
    p.steps[kick][0] = 100;
    p.steps[kick][8] = 100;
  });
  const Tick bar = d.song().barTicks();
  d.addClip(p1, 0, 0);
  d.addClip(p1, 0, bar * 4);
  d.addClip(p2, 3, bar * 2, bar * 6);  // ends at bar 8
  d.addClip(p2, 12, bar);
  return d.song();
}

void testSketch(const std::string& dir) {
  Song s = arrangementSong();
  s.tempo = 128;
  const preview::Sketch k = preview::make(s);
  expect(k.kind == preview::Kind::Arrangement, "arrangement sketch");
  expect(k.rows == 3, "rows = tracks in use (0, 3, 12 fold to 3 rows)");
  expect(k.blocks.size() == 4 && k.clips == 4, "a block per clip");
  expect(k.bars == 8, "length in bars from the last clip end");
  expect(std::fabs(k.seconds - 8 * 4 * 60.0 / 128.0) < 1e-6, "seconds at the song tempo");
  bool order = true, inRange = true;
  for (const auto& b : k.blocks) {
    inRange = inRange && b.x0 >= 0 && b.x1 <= 1 && b.x0 <= b.x1 && b.row >= 0 && b.row < k.rows;
  }
  for (size_t i = 1; i < k.blocks.size(); ++i) order = order && k.blocks[i - 1].x0 <= k.blocks[i].x0;
  expect(inRange && order, "blocks normalised and in time order");
  expect(k.blocks[0].rgb == 0x123456, "blocks carry the pattern colour");
  // Pattern only: steps.
  Song ps = arrangementSong();
  ps.playlist.clear();
  const preview::Sketch kp = preview::make(ps);
  expect(kp.kind == preview::Kind::Pattern && kp.cols == 16 && kp.rows == 1 && kp.blocks.size() == 2, "empty playlist: the first pattern's steps");
  expect(preview::make(makeEmptySong()).kind == preview::Kind::Empty, "empty song: empty sketch");
  // Many tracks fold into the last row.
  Document many(makeEmptySong());
  for (int t = 0; t < 30; ++t) many.addClip(many.song().patterns[0].id, t, 0);
  const preview::Sketch km = preview::make(many.song());
  int maxRow = 0;
  for (const auto& b : km.blocks) maxRow = std::max(maxRow, b.row);
  expect(km.rows == preview::kMaxRows && maxRow == preview::kMaxRows - 1, "30 tracks: capped rows");
  // JSON round trip.
  preview::Sketch back;
  expect(preview::fromJson(preview::toJson(k), back), "json parses");
  expect(back.kind == k.kind && back.rows == k.rows && back.bars == k.bars && back.blocks.size() == k.blocks.size() && back.tempo == 128,
         "json round trip keeps the facts");
  expect(std::fabs(back.blocks[2].x1 - k.blocks[2].x1) < 1e-3f && back.blocks[0].rgb == k.blocks[0].rgb, "json round trip keeps blocks");
  Json bad = preview::toJson(k);
  bad.object["version"] = Json::makeNumber(99);
  expect(!preview::fromJson(bad, back), "another version is not read");
  // Cache keys.
  expect(preview::pathKey("/a") != preview::pathKey("/b") && preview::pathKey("/a").size() == 16, "path keys: 16 hex digits, distinct");
  // Cache with stamps.
  const std::string cache = (fs::path(dir) / "sketches").string();
  const std::string proj = (fs::path(dir) / "Song one.gridline.json").string();
  expect(saveProject(proj, s), "project written");
  preview::Sketch got;
  expect(!preview::loadCached(cache, proj, got), "no cache yet");
  expect(preview::storeCached(cache, proj, k), "cache stored");
  expect(preview::loadCached(cache, proj, got) && got.blocks.size() == k.blocks.size(), "cache read back while the file is unchanged");
  Song s2 = s;
  s2.playlist.pop_back();
  s2.info.title = "A longer title that changes the size";
  expect(saveProject(proj, s2), "project changed");
  expect(!preview::loadCached(cache, proj, got), "a changed project makes its cached sketch stale");
  expect(!preview::storeCached(cache, (fs::path(dir) / "missing.gridline.json").string(), k), "no sketch for a missing file");
  expect(!preview::loadCached(cache, (fs::path(dir) / "missing.gridline.json").string(), got), "missing file: no cached sketch");
  // A loaded project sketches like the song it came from.
  Song loaded;
  expect(loadProject(proj, loaded) && preview::make(loaded).clips == 3, "sketch from a loaded project");
}

// ---- layout audit ----
int auditFails = 0;
void layoutCheck(bool ok, const char* what, float w, float h, float sc, const char* view) {
  if (ok) return;
  if (++auditFails <= 12) std::printf("# layout: %s at %.0fx%.0f scale %.2f (%s)\n", what, w, h, sc, view);
}

void auditOne(float W, float H, float sc, const std::vector<launcher::Item>& items, bool back, const char* view) {
  launcher::LayoutIn in;
  in.width = W;
  in.height = H;
  in.items = items;
  in.hasBack = back;
  in.titleW = 200;
  in.backW = 170;
  in.brandW = 118;
  const launcher::Layout L = launcher::layout(in);
  const launcher::Rect win{0, 0, W, H};
  // Rail rows inside the rail, apart.
  for (size_t i = 0; i < L.railRows.size(); ++i) {
    layoutCheck(L.railRows[i].second.inside(L.railRect), "rail row outside the rail", W, H, sc, view);
    for (size_t j = i + 1; j < L.railRows.size(); ++j)
      layoutCheck(!L.railRows[i].second.overlaps(L.railRows[j].second), "rail rows overlap", W, H, sc, view);
  }
  if (L.rail) layoutCheck(L.railRect.inside(win), "rail outside the window", W, H, sc, view);
  // Header pieces inside the window, apart, clear of the rail and the icons.
  std::vector<launcher::Rect> head = {L.title};
  if (L.search.w > 0) head.push_back(L.search);
  if (L.back.w > 0) head.push_back(L.back);
  layoutCheck(!back || L.back.w >= 80, "back button missing or too small", W, H, sc, view);
  layoutCheck(L.title.w >= 100, "no room for the title", W, H, sc, view);
  for (size_t i = 0; i < head.size(); ++i) {
    layoutCheck(head[i].inside(win), "header piece outside the window", W, H, sc, view);
    if (L.rail) layoutCheck(!head[i].overlaps(L.railRect), "header piece over the rail", W, H, sc, view);
    for (size_t j = i + 1; j < head.size(); ++j) layoutCheck(!head[i].overlaps(head[j]), "header pieces overlap", W, H, sc, view);
    for (const auto& ic : L.headerIcons) layoutCheck(!head[i].overlaps(ic), "header piece under an icon", W, H, sc, view);
  }
  for (size_t i = 0; i < L.headerIcons.size(); ++i) {
    layoutCheck(L.headerIcons[i].inside(win), "icon outside the window", W, H, sc, view);
    layoutCheck(!L.headerIcons[i].overlaps(L.brand), "icon over the brand", W, H, sc, view);
    for (size_t j = i + 1; j < L.headerIcons.size(); ++j) layoutCheck(!L.headerIcons[i].overlaps(L.headerIcons[j]), "icons overlap", W, H, sc, view);
  }
  // Grid below the header, inside the window.
  layoutCheck(L.grid.inside(win) && L.grid.y >= L.title.b(), "grid outside the window or under the header", W, H, sc, view);
  if (L.rail) layoutCheck(!L.grid.overlaps(L.railRect), "grid over the rail", W, H, sc, view);
  layoutCheck(L.grid.h >= L.tileH * 0.75f || H < 420, "grid shows less than one tile", W, H, sc, view);
  // Tiles: big enough, caption room, inside the grid's width, apart.
  layoutCheck(L.tileW >= std::min(launcher::kTileMin, L.grid.w) - 0.5f && L.tileW <= launcher::kTileMax, "tile width out of range", W, H, sc, view);
  layoutCheck(L.tileH - L.thumbH >= 60 && L.thumbH >= L.tileW * 0.55f, "tile caption / thumbnail proportions", W, H, sc, view);
  layoutCheck(L.items.size() == items.size(), "an item has no rectangle", W, H, sc, view);
  for (size_t i = 0; i < L.items.size(); ++i) {
    layoutCheck(L.items[i].x >= 0 && L.items[i].r() <= L.grid.w + 0.5f, "item wider than the grid", W, H, sc, view);
    for (size_t j = i + 1; j < L.items.size(); ++j) layoutCheck(!L.items[i].overlaps(L.items[j]), "items overlap", W, H, sc, view);
  }
  if (!L.items.empty()) layoutCheck(L.contentH >= L.items.back().b() - 0.5f, "content height short of the last item", W, H, sc, view);
  if (L.contentH > L.grid.h)  // scrolling: the scrollbar never covers a tile
    for (size_t i = 0; i < L.items.size(); ++i)
      if (items[i] == launcher::Item::Tile) layoutCheck(L.items[i].r() <= L.grid.w - in.scrollbar - 0.5f, "a tile under the scrollbar", W, H, sc, view);
}

void testLayoutAudit() {
  const std::pair<int, int> sizes[] = {{640, 400}, {800, 500}, {960, 600}, {1024, 768}, {1280, 800}, {1440, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
  const double scales[] = {0.75, 1.0, 1.25, 1.5, 2.0};
  using launcher::Item;
  std::vector<Item> none = {Item::Tile};
  std::vector<Item> few(4, Item::Tile);
  std::vector<Item> many(26, Item::Tile);
  std::vector<Item> tpl = {Item::Section, Item::Tile, Item::Tile, Item::Section, Item::Tile, Item::Tile, Item::Section, Item::Tile, Item::Section,
                           Item::Tile, Item::Tile, Item::Tile};
  int runs = 0;
  for (const auto& [w, h] : sizes)
    for (double sc : scales) {
      const float s = uiscale::effectiveScale(sc, false, 1.0f, w, h);
      const float W = static_cast<float>(w) / s, H = static_cast<float>(h) / s;
      for (bool back : {false, true}) {
        auditOne(W, H, s, none, back, "empty");
        auditOne(W, H, s, few, back, "few");
        auditOne(W, H, s, many, back, "many");
        auditOne(W, H, s, tpl, back, "templates");
        runs += 4;
      }
    }
  std::printf("# layout audit: %d layouts\n", runs);
  expect(auditFails == 0, "layout audit: every size and scale lays out cleanly");
  // Specific expectations.
  launcher::LayoutIn in;
  in.items = many;
  in.width = 1440;
  in.height = 900;
  launcher::Layout L = launcher::layout(in);
  expect(L.rail && L.cols >= 4, "1440x900: the rail and at least four columns");
  in.width = 3840;
  in.height = 2160;
  L = launcher::layout(in);
  expect(L.tileW <= launcher::kTileMax && L.cols >= 10, "4K: many columns, tiles never huge");
  in.width = 700;
  in.height = 480;
  L = launcher::layout(in);
  expect(!L.rail && L.headerIcons.size() == 6, "narrow window: the rail folds into header icons");
  in.items = {Item::Tile, Item::Tile, Item::Section, Item::Tile};
  in.width = 1440;
  L = launcher::layout(in);
  expect(L.items[2].y >= L.items[1].b() && L.items[3].y >= L.items[2].b() && L.items[3].x == 0, "a section starts a new row");
}

}  // namespace

int main() {
  const std::string dir = gl::test::hermeticTestEnv("launcher");
  testStartupPlan();
  testStateMachine();
  testRecents((fs::path(dir) / "recents").string());
  fs::create_directories(fs::path(dir) / "sketch");
  testSketch((fs::path(dir) / "sketch").string());
  testLayoutAudit();
  return finish();
}
