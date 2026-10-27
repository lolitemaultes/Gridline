// Gridline - J-141: the welcome launcher (see LauncherUi.h, core/Launcher.h).
#include "LauncherUi.h"
#include "GridlineVersion.h"
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/Demo.h"
#include "../core/I18n.h"
#include "../core/Launcher.h"
#include "../core/Project.h"
#include "../core/ProjectFiles.h"
#include "../core/Recents.h"
#include "../host/net/Http.h"
#include "App.h"
#include "DevSignal.h"
#include "Keys.h"
#include "SmoothScrollUi.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::launcherui {

namespace {

namespace fs = std::filesystem;
using i18n::tr;
using launcher::Rect;

int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ sketch store
// Sketches by key (a file path, or "@demo" / "@starter" / "@current"). The UI thread owns
// `map_`; one worker reads cached sketches, makes missing ones from project / template
// files and writes the cache, handing results back through `done_`.
enum class SState { Pending, Ready, None };
struct SEntry {
  SState st = SState::Pending;
  preview::Sketch k;
  double readyAt = 0;
};

class Store {
 public:
  ~Store() { stop(); }
  void setDir(const std::string& d) {
    std::lock_guard<std::mutex> lk(m_);
    dir_ = d;
  }
  void request(const std::string& key) {
    if (map_.count(key)) return;
    map_[key] = SEntry{};
    push({key, false, {}});
  }
  void put(const std::string& key, preview::Sketch k, double now, bool writeCache) {
    SEntry& e = map_[key];
    if (e.st != SState::Ready) e.readyAt = now;
    e.st = SState::Ready;
    if (writeCache) push({key, true, k});
    e.k = std::move(k);
  }
  const SEntry* get(const std::string& key) const {
    const auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  void poll(double now) {
    std::vector<Result> got;
    {
      std::lock_guard<std::mutex> lk(m_);
      got.swap(done_);
    }
    for (Result& r : got) {
      SEntry& e = map_[r.key];
      e.st = r.ok ? SState::Ready : SState::None;
      e.k = std::move(r.k);
      e.readyAt = now;
    }
  }
  // Files may have changed since they were read: look again (the cache stamp decides).
  void refreshFiles() {
    for (auto it = map_.begin(); it != map_.end();)
      if (it->first.rfind("@", 0) != 0 && it->second.st != SState::Pending) it = map_.erase(it);
      else ++it;
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
  }

 private:
  struct Job {
    std::string key;
    bool store = false;
    preview::Sketch k;
  };
  struct Result {
    std::string key;
    bool ok = false;
    preview::Sketch k;
  };
  void push(Job j) {
    {
      std::lock_guard<std::mutex> lk(m_);
      if (stop_) return;
      jobs_.push_back(std::move(j));
      if (!th_.joinable()) th_ = std::thread([this] { run(); });
    }
    cv_.notify_one();
  }
  void run() {
    for (;;) {
      Job j;
      std::string dir;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        j = std::move(jobs_.front());
        jobs_.pop_front();
        dir = dir_;
      }
      if (j.store) {
        if (!dir.empty()) preview::storeCached(dir, j.key, std::move(j.k));
        continue;
      }
      Result r;
      r.key = j.key;
      try {
        if (j.key == "@demo") {
          r.k = preview::make(makeDemoTrack());
          r.ok = true;
        } else if (!dir.empty() && preview::loadCached(dir, j.key, r.k)) {
          r.ok = true;
        } else if (openKindOf(j.key) == OpenKind::GridlineProject) {
          Song s;
          if (loadProject(j.key, s)) {
            r.k = preview::make(s);
            r.ok = true;
            if (!dir.empty()) preview::storeCached(dir, j.key, r.k);
          }
        }
      } catch (...) {
        r.ok = false;
      }
      std::lock_guard<std::mutex> lk(m_);
      done_.push_back(std::move(r));
    }
  }
  std::map<std::string, SEntry> map_;
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::vector<Result> done_;
  std::thread th_;
  bool stop_ = false;
  std::string dir_;
};

Store& store() {
  static Store* s = new Store();  // stopped explicitly (shutdown), never destroyed at exit
  return *s;
}

// ------------------------------------------------------------------ tiles
enum class TileKind { New, Current, Recent, Template, Demo, Starter };
struct Tile {
  TileKind kind = TileKind::Recent;
  std::string key;     // recent: path; template: path
  std::string name, meta, tip;
  std::string sketch;  // sketch key ("" = none)
  bool missing = false, open = false;
  bool fav = false;  // J-148: a favourite project (heart badge)
};

Tile tileOf(TileKind k) {
  Tile t;
  t.kind = k;
  return t;
}

struct UiState {
  int view = 0;  // 0 recent, 1 templates
  char search[128] = {};
  int focus = 0;         // tile index (keyboard)
  bool keyNav = false;   // focus ring shown (after keyboard use)
  bool focusSearch = false;
  bool scrollToFocus = false;
  std::map<ImGuiID, float> hover;
  double shownAt = -1;
  int shownFrame = 0;
  bool wasVisible = false;
  Tile menu;              // the tile whose menu is open
  bool openMenu = false;
  bool menuShown = false;  // the tile menu was drawn last frame (dev report)
  std::map<std::string, std::pair<bool, double>> exists;  // path -> (exists, checked at)
  std::vector<Tile> tiles;  // last frame's tiles (dev report)
};
UiState& ui_() {
  static UiState s;
  return s;
}

bool fileExists(const std::string& path, double now) {
  auto& m = ui_().exists;
  auto it = m.find(path);
  if (it == m.end() || now - it->second.second > 2.0) {
    m[path] = {recents::exists(path), now};
    it = m.find(path);
  }
  return it->second.first;
}

struct Colors {
  ImU32 bg, rail, railEdge, card, cardHover, thumb, line, lineHover, text, dim, faint, accent, danger, logo, fill, fillHover;
  ImU32 heart;  // J-148: the favourite heart (a warm rose that reads on both themes)
};
Colors colors() {
  const auto& P = theme::pal();
  Colors c{};
  if (P.dark) {
    c.bg = P.surface950;
    c.rail = theme::mix(P.surface950, P.ink, 0.22f);
    c.card = P.surface900;
    c.cardHover = theme::mix(P.surface900, P.surface800, 0.55f);
    c.thumb = P.field;
    c.fill = theme::mix(P.surface950, P.surface900, 0.6f);
    c.fillHover = P.surface900;
  } else {
    c.bg = P.surface900;
    c.rail = theme::mix(P.surface900, P.surface950, 0.5f);
    c.card = theme::mix(P.surface800, P.paper, 0.55f);
    c.cardHover = P.paper;
    c.thumb = theme::mix(P.well, P.paper, 0.3f);
    c.fill = theme::mix(P.surface900, P.surface800, 0.6f);
    c.fillHover = P.surface800;
  }
  c.railEdge = P.line;
  c.line = P.dark ? theme::mix(P.line, P.surface800, 0.35f) : P.line;
  c.lineHover = P.lineStrong;
  c.text = P.text;
  c.dim = P.textDim;
  c.faint = P.textFaint;
  c.accent = P.accent;
  c.danger = P.danger;
  c.logo = P.logo;
  c.heart = P.dark ? IM_COL32(240, 86, 110, 255) : IM_COL32(214, 48, 78, 255);
  return c;
}

float ease(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Hover / press easing per item (snaps with Interface animations off).
float animate(ImGuiID id, bool on) {
  float& v = ui_().hover[id];
  const float target = on ? 1.0f : 0.0f;
  if (!scrollui::animations()) return v = target;
  const float k = std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
  v += (target - v) * k;
  if (std::fabs(v - target) < 0.01f) v = target;
  return v;
}

std::string ageText(int64_t then, int64_t now) {
  const recents::Age g = recents::age(then, now);
  char b[64];
  switch (g.unit) {
    case recents::AgeUnit::None: return "";
    case recents::AgeUnit::JustNow: return tr("Just now");
    case recents::AgeUnit::Minutes: std::snprintf(b, sizeof b, tr("%d min ago"), g.count); return b;
    case recents::AgeUnit::Hours: std::snprintf(b, sizeof b, tr("%d h ago"), g.count); return b;
    case recents::AgeUnit::Yesterday: return tr("Yesterday");
    case recents::AgeUnit::Days: std::snprintf(b, sizeof b, tr("%d days ago"), g.count); return b;
    case recents::AgeUnit::Date: {
      static const char* months[] = {i18n::N_("Jan"), i18n::N_("Feb"), i18n::N_("Mar"), i18n::N_("Apr"), i18n::N_("May"), i18n::N_("Jun"),
                                     i18n::N_("Jul"), i18n::N_("Aug"), i18n::N_("Sep"), i18n::N_("Oct"), i18n::N_("Nov"), i18n::N_("Dec")};
      const std::time_t t = static_cast<std::time_t>(then);
      std::tm tmv{};
#ifdef _WIN32
      localtime_s(&tmv, &t);
#else
      localtime_r(&t, &tmv);
#endif
      std::snprintf(b, sizeof b, "%d %s %d", tmv.tm_mday, tr(months[std::clamp(tmv.tm_mon, 0, 11)]), tmv.tm_year + 1900);
      return b;
    }
  }
  return "";
}

std::string tempoText(double bpm) {
  char b[32];
  if (std::fabs(bpm - std::round(bpm)) < 0.005) std::snprintf(b, sizeof b, tr("%d BPM"), static_cast<int>(std::lround(bpm)));
  else std::snprintf(b, sizeof b, tr("%.1f BPM"), bpm);
  return b;
}

std::string facts(const preview::Sketch& k) {
  std::string s = tempoText(k.tempo);
  char b[32];
  if (k.kind == preview::Kind::Arrangement && k.bars > 0) {
    std::snprintf(b, sizeof b, k.bars == 1 ? tr("%d bar") : tr("%d bars"), k.bars);
    s += std::string("  \xC2\xB7  ") + b;
  } else if (k.kind == preview::Kind::Pattern) {
    s += std::string("  \xC2\xB7  ") + tr("Pattern");
  }
  return s;
}

const char* revealLabel() {
#if defined(__APPLE__)
  return i18n::N_("Show in Finder");
#elif defined(_WIN32)
  return i18n::N_("Show in Explorer");
#else
  return i18n::N_("Show in Files");
#endif
}

bool matches(const std::string& name, const char* q) {
  if (!q || !*q) return true;
  std::string a = name, b = q;
  std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return a.find(b) != std::string::npos;
}

std::string displayName(const std::string& path) {
  std::string n = projectStemOf(path);
  if (n.empty()) n = fs::path(path).filename().string();
  return n;
}

// The tiles of the current view (sections as nullopt-like markers: kind Template, key "#<category>").
void buildTiles(App& a, std::vector<Tile>& tiles, std::vector<launcher::Item>& items, std::vector<std::string>& sections) {
  UiState& S = ui_();
  const int64_t now = unixNow();
  const std::string openFile = a.home.projectOpen() ? a.projectFile() : std::string();
  auto add = [&](Tile t) {
    if (!matches(t.name, S.search)) return;
    tiles.push_back(std::move(t));
    items.push_back(launcher::Item::Tile);
  };
  auto section = [&](const std::string& name) {
    sections.push_back(name);
    items.push_back(launcher::Item::Section);
    tiles.push_back(Tile{TileKind::Template, "#" + name, name, "", "", "", false, false});
  };
  if (S.view == 0) {
    Tile nt;
    nt.kind = TileKind::New;
    nt.name = tr("New project");
    const std::string def = a.settings.getString("general.defaultTemplate");
    const TemplateInfo* ti = def.empty() ? nullptr : a.findTemplate(def);
    nt.meta = ti ? ti->name : std::string(tr("Starter kit"));
    if (!S.search[0]) {
      tiles.push_back(nt);
      items.push_back(launcher::Item::Tile);
    }
    // The open project, when it is not in the list (never saved, or saved elsewhere).
    const bool listed = !openFile.empty() && std::find(a.recentFiles.begin(), a.recentFiles.end(), openFile) != a.recentFiles.end();
    if (a.home.projectOpen() && !listed) {
      Tile c;
      c.kind = TileKind::Current;
      c.name = openFile.empty() ? (a.doc.song().info.title.empty() ? std::string(tr("Untitled project")) : a.doc.song().info.title) : displayName(openFile);
      c.meta = openFile.empty() ? tr("Not saved yet") : tr("Open now");
      c.sketch = "@current";
      c.open = true;
      add(c);
    }
    for (const std::string& p : a.recentFiles) {
      Tile t;
      t.kind = TileKind::Recent;
      t.key = p;
      t.name = displayName(p);
      t.tip = p;
      t.missing = !fileExists(p, a.now);
      t.open = !openFile.empty() && p == openFile;
      t.sketch = t.open ? "@current" : p;
      t.fav = a.isFavouriteProject(p);
      add(t);
    }
    if (a.recentFiles.empty() && !a.home.projectOpen() && !S.search[0]) {
      Tile d;
      d.kind = TileKind::Demo;
      d.name = tr("Demo song");
      d.meta = tr("Hear what Gridline can do");
      d.sketch = "@demo";
      add(d);
    }
    // Meta lines (age and facts) once sketches are known.
    for (Tile& t : tiles) {
      if (t.kind != TileKind::Recent) continue;
      std::string age = ageText(a.recentOpenedAt(t.key), now);
      if (t.missing) {
        t.meta = tr("Not found");
        continue;
      }
      std::string f;
      if (const SEntry* e = store().get(t.sketch); e && e->st == SState::Ready) f = facts(e->k);
      else if (openKindOf(t.key) == OpenKind::FlProject) f = tr("FL Studio project");
      else if (openKindOf(t.key) == OpenKind::ZippedProject) f = tr("Zipped project");
      t.meta = age.empty() ? f : f.empty() ? age : age + "  \xC2\xB7  " + f;
    }
  } else {
    section(tr("Start"));
    {
      Tile s;
      s.kind = TileKind::Starter;
      s.name = tr("Starter kit");
      s.meta = tr("Four drums and a synth, ready to play");
      s.sketch = "@starter";
      add(s);
      Tile d;
      d.kind = TileKind::Demo;
      d.name = tr("Demo song");
      d.meta = tr("Hear what Gridline can do");
      d.sketch = "@demo";
      add(d);
    }
    std::string cat;
    for (const TemplateInfo& t : a.templates()) {
      if (!matches(t.name, S.search)) continue;
      if (t.category != cat) {
        cat = t.category;
        section(cat);
      }
      Tile x;
      x.kind = TileKind::Template;
      x.key = t.path;
      x.name = t.name;
      x.meta = t.description;
      if (const size_t dot = x.meta.find(". "); dot != std::string::npos) x.meta = x.meta.substr(0, dot + 1);
      x.sketch = t.path;
      x.tip = t.description;
      add(x);
    }
    // Drop sections left empty by the search.
    for (size_t i = 0; i < items.size();) {
      const bool sec = items[i] == launcher::Item::Section;
      const bool empty = sec && (i + 1 == items.size() || items[i + 1] == launcher::Item::Section);
      if (empty) {
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
        tiles.erase(tiles.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
  }
}

// ------------------------------------------------------------------ drawing helpers
void drawSketch(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const preview::Sketch& k, float alpha) {
  const auto& P = theme::pal();
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  const float padX = std::max(10.0f, w * 0.06f), padY = std::max(10.0f, h * 0.14f);
  const ImVec2 a(p0.x + padX, p0.y + padY), b(p1.x - padX, p1.y - padY);
  const float iw = b.x - a.x, ih = b.y - a.y;
  if (iw <= 4 || ih <= 4) return;
  const ImU32 grid = theme::withAlpha(P.dark ? P.surface800 : P.lineStrong, 0.35f * alpha);
  if (k.kind == preview::Kind::Arrangement || k.kind == preview::Kind::Empty) {
    // Bar lines: a few, evenly spaced (every 4 / 8 / 16 bars).
    const int bars = std::max(1, k.bars);
    int every = 1;
    while (bars / every > 16) every *= 2;
    for (int i = every; i < bars; i += every) {
      const float x = std::round(a.x + iw * static_cast<float>(i) / static_cast<float>(bars)) + 0.5f;
      dl->AddLine(ImVec2(x, p0.y + 6), ImVec2(x, p1.y - 6), grid);
    }
  }
  if (k.kind == preview::Kind::Empty || k.blocks.empty()) {  // nothing yet: an idle step row, quiet
    const float cw = iw / 16.0f, rh = std::min(cw * 1.5f, ih * 0.3f), y = a.y + (ih - rh) * 0.5f, gap = std::max(1.0f, cw * 0.18f);
    for (int c = 0; c < 16; ++c) {
      const float x = a.x + cw * static_cast<float>(c);
      const ImU32 cc = theme::withAlpha((c / 4) % 2 ? (P.dark ? P.surface700 : P.stepOffAlt) : (P.dark ? P.surface800 : P.stepOff), 0.45f * alpha);
      dl->AddRectFilled(ImVec2(x + gap * 0.5f, y), ImVec2(x + cw - gap * 0.5f, y + rh), cc, 1.5f);
    }
    return;
  }
  const int rows = std::max(1, k.rows);
  if (k.kind == preview::Kind::Rack) {
    // Channel rack: a coloured name chip per channel and a row of idle steps.
    const float rh = std::min(ih / static_cast<float>(rows), std::max(8.0f, h * 0.13f));
    const float top = a.y + (ih - rh * static_cast<float>(rows)) * 0.5f;
    const float chipW = std::round(iw * 0.22f), gap = std::max(1.0f, rh * 0.18f);
    const float cw = (iw - chipW - 6) / 16.0f;
    const ImU32 off = theme::withAlpha(P.dark ? P.surface800 : P.stepOff, 0.55f * alpha);
    for (const preview::Block& bl : k.blocks) {
      const float y = top + rh * static_cast<float>(bl.row);
      dl->AddRectFilled(ImVec2(a.x, y + gap * 0.5f), ImVec2(a.x + chipW, y + rh - gap * 0.5f), theme::content(bl.rgb, 0.85f * alpha), 2.0f);
      for (int c = 0; c < 16; ++c) {
        const float x = a.x + chipW + 6 + cw * static_cast<float>(c);
        const ImU32 cc = (c / 4) % 2 ? theme::withAlpha(P.dark ? P.surface700 : P.stepOffAlt, 0.5f * alpha) : off;
        dl->AddRectFilled(ImVec2(x + gap * 0.5f, y + gap * 0.5f), ImVec2(x + cw - gap * 0.5f, y + rh - gap * 0.5f), cc, 1.5f);
      }
    }
    return;
  }
  if (k.kind == preview::Kind::Mixer) {
    // Mixer: a strip per named insert, its colour on top, a fader cap.
    const float sw = std::min(iw / static_cast<float>(rows), 26.0f);
    const float left = a.x + (iw - sw * static_cast<float>(rows)) * 0.5f;
    const ImU32 slot = theme::withAlpha(P.dark ? P.surface800 : P.stepOff, 0.6f * alpha);
    for (const preview::Block& bl : k.blocks) {
      const float x = left + sw * static_cast<float>(bl.row) + sw * 0.12f, x1 = x + sw * 0.76f;
      dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x1, b.y), slot, 2.0f);
      dl->AddRectFilled(ImVec2(x, a.y), ImVec2(x1, a.y + std::max(3.0f, ih * 0.08f)), theme::content(bl.rgb, 0.9f * alpha), 2.0f,
                        ImDrawFlags_RoundCornersTop);
      const float fy = a.y + ih * 0.42f;
      dl->AddRectFilled(ImVec2(x + 1, fy - 2), ImVec2(x1 - 1, fy + 2), theme::withAlpha(P.dark ? P.text : P.textDim, 0.7f * alpha), 1.0f);
    }
    return;
  }
  if (k.kind == preview::Kind::Pattern && k.cols > 0) {
    // Step grid: every step faint, lit steps in their channel colour.
    const int cols = k.cols;
    const float cw = iw / static_cast<float>(cols);
    const float rh = std::min(ih / static_cast<float>(rows), cw * 1.6f);
    const float top = a.y + (ih - rh * static_cast<float>(rows)) * 0.5f;
    const float gap = std::max(1.0f, std::min(cw, rh) * 0.18f);
    const ImU32 off = theme::withAlpha(P.dark ? P.surface800 : P.stepOff, 0.55f * alpha);
    for (int r = 0; r < rows; ++r)
      for (int c = 0; c < cols; ++c) {
        const float x = a.x + cw * static_cast<float>(c), y = top + rh * static_cast<float>(r);
        dl->AddRectFilled(ImVec2(x + gap * 0.5f, y + gap * 0.5f), ImVec2(x + cw - gap * 0.5f, y + rh - gap * 0.5f), off, 1.5f);
      }
    for (const preview::Block& bl : k.blocks) {
      const float x = a.x + iw * bl.x0, y = top + rh * static_cast<float>(bl.row);
      dl->AddRectFilled(ImVec2(x + gap * 0.5f, y + gap * 0.5f), ImVec2(x + cw - gap * 0.5f, y + rh - gap * 0.5f),
                        theme::content(bl.rgb, (bl.muted ? 0.35f : 0.95f) * alpha), 1.5f);
    }
    return;
  }
  // Arrangement (or piano-roll notes): rows of rounded blocks.
  const float rh = std::min(ih / static_cast<float>(rows), std::max(6.0f, h * 0.16f));
  const float top = a.y + (ih - rh * static_cast<float>(rows)) * 0.5f;
  const float gapY = std::clamp(rh * 0.22f, 1.0f, 4.0f);
  for (const preview::Block& bl : k.blocks) {
    float x0 = a.x + iw * bl.x0, x1 = a.x + iw * bl.x1;
    if (x1 - x0 < 2.0f) x1 = x0 + 2.0f;
    const float y = top + rh * static_cast<float>(bl.row);
    const ImU32 col = theme::content(bl.rgb, (bl.muted ? 0.3f : 0.85f) * alpha);
    dl->AddRectFilled(ImVec2(x0 + 0.5f, y + gapY * 0.5f), ImVec2(x1 - 0.5f, y + rh - gapY * 0.5f), col, std::min(3.0f, rh * 0.25f));
  }
}

void textAt(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* s, float maxW) {
  theme::textEllipsis(dl, f, size, ImVec2(std::round(p.x), std::round(p.y)), maxW, col, s);
}

ImVec2 measure(ImFont* f, float size, const char* s) { return f ? f->CalcTextSizeA(size, FLT_MAX, 0, s) : ImVec2(0, size); }

// A rail row: icon, label, a right-hand note (shortcut or count). Returns true when clicked.
bool railRow(App& a, const char* id, const Rect& r, ui::Icon icon, const char* label, const char* note, bool selected) {
  const Colors C = colors();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(r.w, r.h));
  const float t = animate(ImGui::GetItemID(), ImGui::IsItemHovered());
  const ImVec2 a0(r.x, r.y + 2), a1(r.r(), r.b() - 2);
  if (selected) dl->AddRectFilled(a0, a1, C.fillHover, theme::kRadiusLg);
  else if (t > 0) dl->AddRectFilled(a0, a1, theme::withAlpha(C.fillHover, t), theme::kRadiusLg);
  if (selected) dl->AddRectFilled(ImVec2(r.x, r.y + 9), ImVec2(r.x + 2, r.b() - 9), C.accent, 1.0f);
  const ImU32 tc = selected || t > 0.5f ? C.text : theme::mix(C.dim, C.text, t);
  ui::drawIcon(dl, icon, ImVec2(r.x + 18, r.y + r.h * 0.5f), 15, tc);
  float noteW = 0;
  if (note && *note) {
    noteW = measure(a.fontMono, theme::kFontLabel, note).x;
    dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(r.r() - 12 - noteW), std::round(r.y + (r.h - theme::kFontLabel) * 0.5f)), C.faint, note);
  }
  textAt(dl, selected ? a.fontBold : a.fontRegular, theme::kFontBody, ImVec2(r.x + 36, r.y + (r.h - theme::kFontBody) * 0.5f - 1), tc, label,
         r.w - 36 - 12 - (noteW > 0 ? noteW + 10 : 0));
  return clicked;
}

void openTile(App& a, const Tile& t) {
  switch (t.kind) {
    case TileKind::New: a.guardUnsaved([&a]() { a.newProject(); }); break;
    case TileKind::Current: a.home.back(a.now); break;
    case TileKind::Recent: {
      if (t.open) {
        a.home.back(a.now);
        break;
      }
      if (t.missing) {
        a.notify("Cannot find " + t.name, true, t.key);
        break;
      }
      const std::string path = t.key;
      a.guardUnsaved([&a, path]() { a.openProject(path); });
      break;
    }
    case TileKind::Template: {
      const std::string path = t.key;
      a.guardUnsaved([&a, path]() { a.newFromTemplate(path); });
      break;
    }
    case TileKind::Demo: a.guardUnsaved([&a]() { a.newFromTemplate("demo"); }); break;
    case TileKind::Starter: a.guardUnsaved([&a]() { a.newFromTemplate("starter"); }); break;
  }
}

void removeTile(App& a, const Tile& t) {
  if (t.kind != TileKind::Recent) return;
  if (a.removeRecent(t.key)) a.notify("Removed from recent projects: " + t.name);
}

// J-148: favourite on / off (the tile menu, the tile's heart button, the F key).
void toggleFavourite(App& a, const Tile& t) {
  if (t.kind != TileKind::Recent) return;
  a.setFavouriteProject(t.key, !t.fav);
}

void tileMenu(App& a) {
  UiState& S = ui_();
  if (S.openMenu) {
    ImGui::OpenPopup("##launcherTileMenu");
    S.openMenu = false;
  }
  S.menuShown = ImGui::BeginPopup("##launcherTileMenu");
  if (!S.menuShown) return;
  const Tile& t = S.menu;
  if (t.kind == TileKind::Recent || t.kind == TileKind::Current) {
    if (ImGui::MenuItem(t.open ? tr("Back to project") : tr("Open"), nullptr, false, !t.missing)) openTile(a, t);
    if (t.kind == TileKind::Recent) {
      if (ImGui::MenuItem(t.fav ? tr("Remove from favourites") : tr("Add to favourites"))) toggleFavourite(a, t);
      ImGui::Separator();
      if (ImGui::MenuItem(tr(revealLabel()), nullptr, false, !t.missing)) http::reveal(t.key);
      if (ImGui::MenuItem(tr("Copy path"))) ImGui::SetClipboardText(t.key.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem(tr("Remove from list"), nullptr)) removeTile(a, t);
    }
    if (t.open) {
      ImGui::Separator();
      if (ImGui::MenuItem(tr("Close project"))) a.closeProject();
    }
  } else {
    if (ImGui::MenuItem(tr("Create"))) openTile(a, t);
    if (t.kind == TileKind::Template && ImGui::MenuItem(tr(revealLabel()))) http::reveal(t.key);
  }
  ImGui::EndPopup();
}

// One tile at screen position `p` (size from the layout). Returns true when activated.
bool drawTile(App& a, int index, const Tile& t, ImVec2 p, const launcher::Layout& L, bool focused) {
  UiState& S = ui_();
  const Colors C = colors();
  const auto& P = theme::pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p1(p.x + L.tileW, p.y + L.tileH);
  ImGui::PushID(index);
  ImGui::SetCursorScreenPos(p);
  ImGui::SetNextItemAllowOverlap();
  bool activated = ImGui::InvisibleButton("##tile", ImVec2(L.tileW, L.tileH));
  const bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
  const bool held = ImGui::IsItemActive();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && t.kind != TileKind::New) {
    S.menu = t;
    S.openMenu = true;
  }
  if (hov && !t.tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay | ImGuiHoveredFlags_AllowWhenOverlappedByItem)) {
    if (ImGui::BeginTooltip()) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
      ImGui::TextUnformatted(t.tip.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
  }
  const float ht = animate(ImGui::GetID("##hov"), hov || (focused && S.keyNav));
  const float dim = t.missing ? 0.55f : 1.0f;
  const float r = theme::kRadiusLg + 2;
  // Card.
  dl->AddRectFilled(p, p1, theme::mix(C.card, C.cardHover, ht), r);
  const ImVec2 t0(p.x + 1, p.y + 1), t1(p1.x - 1, p.y + L.thumbH);
  // Thumbnail.
  if (t.kind == TileKind::New) {
    dl->AddRectFilled(t0, t1, theme::mix(C.thumb, C.cardHover, 0.35f + 0.25f * ht), r - 1, ImDrawFlags_RoundCornersTop);
    const float ms = std::round(std::clamp(L.thumbH * 0.34f, 28.0f, 72.0f));
    const ImVec2 mc((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f);
    theme::drawMark(dl, ImVec2(std::round(mc.x - ms * 0.5f), std::round(mc.y - ms * 0.5f)), ms, C.logo, theme::mix(C.thumb, C.cardHover, 0.35f + 0.25f * ht),
                    0.82f + 0.18f * ht);
  } else {
    dl->AddRectFilled(t0, t1, theme::withAlpha(C.thumb, dim), r - 1, ImDrawFlags_RoundCornersTop);
    const SEntry* e = t.sketch.empty() ? nullptr : store().get(t.sketch);
    if (e && e->st == SState::Ready) {
      float fa = scrollui::animations() ? ease(static_cast<float>((a.now - e->readyAt) / 0.25)) : 1.0f;
      if (ImGui::GetFrameCount() - S.shownFrame > 14) fa = std::max(fa, 0.999f);
      dl->PushClipRect(t0, t1, true);
      drawSketch(dl, t0, t1, e->k, fa * dim);
      dl->PopClipRect();
    } else if (t.missing || (e && e->st == SState::None)) {
      const char* label = t.missing ? tr("Not found") : openKindOf(t.key) == OpenKind::FlProject ? "FLP" : openKindOf(t.key) == OpenKind::ZippedProject ? "ZIP" : "";
      if (*label) {
        const ImVec2 ts = measure(a.fontMono, theme::kFontLabel, label);
        dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round((t0.x + t1.x - ts.x) * 0.5f), std::round((t0.y + t1.y - ts.y) * 0.5f)),
                    t.missing ? theme::withAlpha(C.danger, 0.9f) : C.faint, label);
      }
    }
  }
  dl->AddLine(ImVec2(t0.x, t1.y), ImVec2(t1.x, t1.y), theme::withAlpha(C.line, 0.8f));
  // Caption (the open project: an "OPEN" pill at the end of the name line).
  const float cx = p.x + 14;
  float cw = L.tileW - 28;
  const float nameY = p.y + L.thumbH + 13;
  if (t.open) {
    const char* b = tr("OPEN");
    const ImVec2 bs = measure(a.fontMono, 10, b);
    const ImVec2 b1(p1.x - 12, nameY + 16), b0(b1.x - bs.x - 14, nameY - 1);
    dl->AddRectFilled(b0, b1, C.accent, 8.5f);
    dl->AddText(a.fontMono, 10, ImVec2(std::round(b0.x + 7), std::round(b0.y + (17 - bs.y) * 0.5f)), theme::contrastText(C.accent), b);
    cw -= bs.x + 22;
  }
  textAt(dl, a.fontBold, theme::kFontBody, ImVec2(cx, nameY), theme::withAlpha(C.text, dim), t.name.c_str(), cw);
  if (t.kind == TileKind::New) {
    const char* sc = keys::label("file.new");
    const float sw = sc && *sc ? measure(a.fontMono, theme::kFontLabel, sc).x : 0;
    textAt(dl, a.fontRegular, theme::kFontCaption, ImVec2(cx, nameY + 21), C.dim, t.meta.c_str(), cw - (sw > 0 ? sw + 8 : 0));
    if (sw > 0) dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(p1.x - 14 - sw), std::round(nameY + 22)), C.faint, sc);
  } else {
    textAt(dl, a.fontRegular, theme::kFontCaption, ImVec2(cx, nameY + 21), t.missing ? theme::withAlpha(C.danger, 0.9f) : C.dim, t.meta.c_str(), L.tileW - 28);
  }
  // Outline: quiet line; hover a stronger one; the open project in the accent; keyboard focus a 2 px ring.
  ImU32 edge = theme::mix(C.line, C.lineHover, ht);
  if (t.open) edge = theme::withAlpha(C.accent, 0.75f + 0.25f * ht);
  dl->AddRect(p, p1, edge, r, 0, t.open ? 1.5f : 1.0f);
  if (focused && S.keyNav) dl->AddRect(ImVec2(p.x - 3, p.y - 3), ImVec2(p1.x + 3, p1.y + 3), C.accent, r + 3, 0, 2.0f);
  if (held) dl->AddRectFilled(p, p1, theme::withAlpha(P.ink, P.dark ? 0.10f : 0.04f), r);
  // J-148: a favourite wears a heart badge in the thumbnail's top-left corner.
  if (t.fav) {
    const float bd = 26;
    const ImVec2 h0(t0.x + 8, t0.y + 8);
    dl->AddRectFilled(h0, ImVec2(h0.x + bd, h0.y + bd), theme::withAlpha(C.card, 0.92f), bd * 0.5f);
    ui::drawIcon(dl, ui::Icon::HeartFilled, ImVec2(h0.x + bd * 0.5f, h0.y + bd * 0.5f + 0.5f), 14, C.heart);
  }
  // Hover: the tile's buttons, top right (not on the new-project tile): a heart (favourite,
  // recent projects only) and the menu. They stay while pressed: the tile reports no hover
  // while one of its buttons is the active item, and a button that is not submitted on the
  // frame the mouse is released never reports the click.
  const ImGuiID moreId = ImGui::GetID("##more"), favId = ImGui::GetID("##fav");
  const ImGuiID activeId = ImGui::GetActiveID();
  const bool menuOpen = ImGui::IsPopupOpen("##launcherTileMenu") && S.menu.key == t.key && S.menu.kind == t.kind;
  if (t.kind != TileKind::New && (hov || menuOpen || activeId == moreId || activeId == favId)) {
    const float bs = 26;
    const ImVec2 m0(t1.x - bs - 8, t0.y + 8);
    ImGui::SetCursorScreenPos(m0);
    if (ImGui::InvisibleButton("##more", ImVec2(bs, bs))) {
      S.menu = t;
      S.openMenu = true;
    }
    const bool mh = ImGui::IsItemHovered();
    if (mh) ImGui::SetTooltip("%s", tr("More"));
    dl->AddRectFilled(m0, ImVec2(m0.x + bs, m0.y + bs), theme::withAlpha(mh ? C.cardHover : C.card, 0.92f), theme::kRadius + 1);
    ui::drawIcon(dl, ui::Icon::More, ImVec2(m0.x + bs * 0.5f, m0.y + bs * 0.5f), 14, mh ? C.text : C.dim);
    bool fh = false;
    if (t.kind == TileKind::Recent && !t.fav) {
      const ImVec2 f0(m0.x - bs - 6, m0.y);
      ImGui::SetCursorScreenPos(f0);
      if (ImGui::InvisibleButton("##fav", ImVec2(bs, bs))) toggleFavourite(a, t);
      fh = ImGui::IsItemHovered();
      if (fh) ImGui::SetTooltip("%s", tr("Add to favourites"));
      dl->AddRectFilled(f0, ImVec2(f0.x + bs, f0.y + bs), theme::withAlpha(fh ? C.cardHover : C.card, 0.92f), theme::kRadius + 1);
      ui::drawIcon(dl, fh ? ui::Icon::HeartFilled : ui::Icon::Heart, ImVec2(f0.x + bs * 0.5f, f0.y + bs * 0.5f + 0.5f), 14,
                   fh ? C.heart : C.dim);
    }
    if (mh || fh) activated = false;
  }
  // The heart badge doubles as the "remove from favourites" button.
  if (t.fav && t.kind == TileKind::Recent) {
    const float bd = 26;
    ImGui::SetCursorScreenPos(ImVec2(t0.x + 8, t0.y + 8));
    if (ImGui::InvisibleButton("##unfav", ImVec2(bd, bd))) toggleFavourite(a, t);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tr("Remove from favourites"));
      activated = false;
    }
  }
  ImGui::PopID();
  return activated;
}

}  // namespace

// ------------------------------------------------------------------ public
void rememberSketch(App& a, const std::string& path, preview::Sketch k) {
  store().setDir(a.sketchCacheDir());
  store().put(path, std::move(k), a.now, true);
}

void setView(int view) { ui_().view = std::clamp(view, 0, 1); }

void shutdown() { store().stop(); }

void drawReturnFade(App& a) {
  UiState& S = ui_();
  if (S.wasVisible) {  // the launcher left (back, or a project opened from anywhere)
    S.wasVisible = false;
    devsig::forget("launcher-shown");
    devsig::reach("launcher-hidden");
  }
  const double since = a.now - a.home.changedAt();
  if (a.home.changedAt() < 0 || since > 0.2 || !scrollui::animations()) return;
  const float t = ease(static_cast<float>(since / 0.16));
  if (t >= 1.0f) return;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::GetForegroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), theme::withAlpha(colors().bg, 1.0f - t));
}

void devReport(App& a) {
  const UiState& S = ui_();
  std::printf("launcher visible=%d open=%d view=%d tiles=%d prompt=%d modified=%d project=%s title=%s\n", a.home.visible() ? 1 : 0,
              a.home.projectOpen() ? 1 : 0, S.view, static_cast<int>(S.tiles.size()), a.confirmDiscard || ImGui::GetTopMostPopupModal() ? 1 : 0, a.doc.modified() ? 1 : 0,
              fs::path(a.projectFile()).filename().string().c_str(), a.windowTitle().c_str());
  std::printf("launcher menu=%s\n", S.menuShown && a.home.visible() ? S.menu.name.c_str() : "-");  // J-148: the tile menu shown
  for (const Tile& t : S.tiles) {
    if (!t.key.empty() && t.key[0] == '#') continue;
    const SEntry* e = t.sketch.empty() ? nullptr : store().get(t.sketch);
    std::printf("tile kind=%d name=%s open=%d missing=%d fav=%d sketch=%s meta=%s\n", static_cast<int>(t.kind), t.name.c_str(), t.open ? 1 : 0,
                t.missing ? 1 : 0, t.fav ? 1 : 0, !e ? "none" : e->st == SState::Ready ? "ready" : e->st == SState::Pending ? "pending" : "failed", t.meta.c_str());
  }
  std::fflush(stdout);
}

void draw(App& a) {
  UiState& S = ui_();
  const Colors C = colors();
  ImGuiIO& io = ImGui::GetIO();
  Store& st = store();
  st.setDir(a.sketchCacheDir());
  if (!S.wasVisible) {  // just shown: fresh file checks, the open song's sketch, focus on the first tile
    S.shownAt = a.now;
    S.shownFrame = ImGui::GetFrameCount();
    S.exists.clear();
    st.refreshFiles();
    S.focus = 0;
    S.keyNav = false;
    devsig::forget("launcher-hidden");
  }
  S.wasVisible = true;
  devsig::reach("launcher-shown");
  st.poll(a.now);
  if (a.home.projectOpen() && (!st.get("@current") || ImGui::GetFrameCount() % 30 == 0))
    st.put("@current", preview::make(a.doc.song()), st.get("@current") ? st.get("@current")->readyAt : a.now, false);
  if (!st.get("@starter")) st.put("@starter", preview::make(a.starterSong()), a.now, false);

  std::vector<Tile> tiles;
  std::vector<launcher::Item> items;
  std::vector<std::string> sections;
  buildTiles(a, tiles, items, sections);
  for (const Tile& t : tiles)
    if (!t.sketch.empty() && t.sketch != "@current" && t.sketch != "@starter" && !t.missing) st.request(t.sketch);

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(C.bg));
  ImGui::Begin("##launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = vp->WorkPos;
  if (ImGui::GetFrameCount() == S.shownFrame) ImGui::SetWindowFocus();  // keys go to the launcher at once

  // ---- layout ----
  const char* title = S.view == 0 ? tr("Recent projects") : tr("Templates");
  launcher::LayoutIn in;
  in.width = vp->WorkSize.x;
  in.height = vp->WorkSize.y;
  in.items = items;
  in.hasBack = a.home.projectOpen();
  in.titleW = measure(a.fontBold, 22, title).x + 40;  // + the list menu button
  in.backW = measure(a.fontRegular, theme::kFontBody, tr("Back to project")).x + 66;
  in.brandW = theme::lockupWidth(28, a.fontBold);
  in.actions = 3;
  in.views = 2;
  in.scrollbar = ImGui::GetStyle().ScrollbarSize;
  const launcher::Layout L = launcher::layout(in);
  auto scr = [&](const Rect& r) { return ImVec2(o.x + r.x, o.y + r.y); };

  // ---- rail / narrow header ----
  const int nRecent = static_cast<int>(a.recentFiles.size());
  const int nTemplates = static_cast<int>(a.templates().size()) + 2;
  char cntR[16], cntT[16];
  std::snprintf(cntR, sizeof cntR, "%d", nRecent);
  std::snprintf(cntT, sizeof cntT, "%d", nTemplates);
  if (L.rail) {
    dl->AddRectFilled(scr(L.railRect), ImVec2(o.x + L.railRect.r(), o.y + L.railRect.b()), C.rail);
    dl->AddLine(ImVec2(o.x + L.railRect.r() - 0.5f, o.y), ImVec2(o.x + L.railRect.r() - 0.5f, o.y + L.railRect.b()), C.railEdge);
    int action = 0, view = 0;
    for (const auto& [kind, r] : L.railRows) {
      Rect sr{o.x + r.x, o.y + r.y, r.w, r.h};
      switch (kind) {
        case launcher::RailRow::Brand: theme::drawLockup(dl, ImVec2(sr.x, sr.y), 28, C.logo, C.rail, a.fontBold); break;
        case launcher::RailRow::Action:
          if (action == 0 && railRow(a, "##new", sr, ui::Icon::Plus, tr("New project"), keys::label("file.new"), false)) openTile(a, tileOf(TileKind::New));
          if (action == 1 && railRow(a, "##open", sr, ui::Icon::Open, tr("Open..."), keys::label("file.open"), false)) a.openDialog();
          if (action == 2 && railRow(a, "##demo", sr, ui::Icon::SpeakerWaves, tr("Demo song"), "", false)) openTile(a, tileOf(TileKind::Demo));
          ++action;
          break;
        case launcher::RailRow::Label:
          dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(sr.x), std::round(sr.y + 3)), C.faint, tr("LIBRARY"));
          break;
        case launcher::RailRow::View:
          if (view == 0 && railRow(a, "##vrecent", sr, ui::Icon::ListBullets, tr("Recent"), cntR, S.view == 0)) S.view = 0, S.focus = 0;
          if (view == 1 && railRow(a, "##vtpl", sr, ui::Icon::Clone, tr("Templates"), cntT, S.view == 1)) S.view = 1, S.focus = 0;
          ++view;
          break;
        case launcher::RailRow::Settings:
          if (railRow(a, "##settings", sr, ui::Icon::Settings, tr("Settings"), "", false)) a.showSettings = true;
          break;
        case launcher::RailRow::Version:
          dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(sr.x), std::round(sr.y)), C.faint, "Gridline " GRIDLINE_VERSION);
          break;
      }
    }
  } else {
    const bool full = L.brand.w >= in.brandW;
    if (full) theme::drawLockup(dl, scr(L.brand), 26, C.logo, C.bg, a.fontBold);
    else theme::drawMark(dl, scr(L.brand), 26, C.logo, C.bg);
    struct B {
      const char* id;
      ui::Icon icon;
      const char* hint;
      bool active;
    };
    const B btn[] = {{"##nnew", ui::Icon::Plus, i18n::N_("New project"), false},
                     {"##nopen", ui::Icon::Open, i18n::N_("Open..."), false},
                     {"##ndemo", ui::Icon::SpeakerWaves, i18n::N_("Demo song"), false},
                     {"##nrecent", ui::Icon::ListBullets, i18n::N_("Recent projects"), S.view == 0},
                     {"##ntpl", ui::Icon::Clone, i18n::N_("Templates"), S.view == 1},
                     {"##nset", ui::Icon::Settings, i18n::N_("Settings"), false}};
    for (size_t i = 0; i < L.headerIcons.size() && i < 6; ++i) {
      ImGui::SetCursorScreenPos(scr(L.headerIcons[i]));
      const bool hit = ui::IconButton(btn[i].id, btn[i].icon, btn[i].active, L.headerIcons[i].w, btn[i].hint);
      ui::Tooltip(btn[i].hint);  // (no status bar here for the hint)
      if (hit) {
        if (i == 0) openTile(a, tileOf(TileKind::New));
        if (i == 1) a.openDialog();
        if (i == 2) openTile(a, tileOf(TileKind::Demo));
        if (i == 3) S.view = 0, S.focus = 0;
        if (i == 4) S.view = 1, S.focus = 0;
        if (i == 5) a.showSettings = true;
      }
    }
  }

  // ---- header: title, list menu, search, back ----
  {
    const ImVec2 tp = scr(L.title);
    const float ty = std::round(tp.y + (L.title.h - 22) * 0.5f);
    const float tw = measure(a.fontBold, 22, title).x;
    textAt(dl, a.fontBold, 22, ImVec2(tp.x, ty), C.text, title, L.title.w - 36);
    if (S.view == 0 && L.title.w >= tw + 36) {
      ImGui::SetCursorScreenPos(ImVec2(tp.x + tw + 8, std::round(tp.y + (L.title.h - 24) * 0.5f)));
      const bool hit = ui::IconButton("##listmenu", ui::Icon::More, ImGui::IsPopupOpen("##launcherListMenu"), 24, i18n::N_("Recent list"));
      ui::Tooltip("Recent list");
      if (hit) ImGui::OpenPopup("##launcherListMenu");
      if (ImGui::BeginPopup("##launcherListMenu")) {
        if (ImGui::MenuItem(tr("Remove missing projects"))) {
          const size_t n = a.removeMissingRecents();
          if (n) a.notify("Removed missing projects from the list");
        }
        // J-148: favourites stay when the list is cleared.
        const bool favs = a.favouriteProjectCount() > 0;
        if (ImGui::MenuItem(favs ? tr("Clear all but favourites") : tr("Clear the list"), nullptr, false, a.recentFiles.size() > a.favouriteProjectCount()))
          a.clearRecents();
        ImGui::EndPopup();
      }
    }
    if (L.search.w > 0) {
      const ImVec2 sp = scr(L.search);
      ImGui::SetCursorScreenPos(sp);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32, std::round((L.search.h - theme::kFontBody) * 0.5f)));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kRadiusLg + 2);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(C.fill));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, theme::vec(C.fillHover));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, theme::vec(C.fillHover));
      ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(C.line));
      ImGui::SetNextItemWidth(L.search.w);
      if (S.focusSearch) {
        ImGui::SetKeyboardFocusHere();
        S.focusSearch = false;
      }
      if (ImGui::InputTextWithHint("##launcherSearch", tr("Search"), S.search, sizeof S.search)) S.focus = 0;
      const bool searching = ImGui::IsItemActive();
      ImGui::PopStyleColor(4);
      ImGui::PopStyleVar(3);
      ui::drawIcon(dl, ui::Icon::Search, ImVec2(sp.x + 17, sp.y + L.search.h * 0.5f), 14, searching ? C.text : C.faint);
      if (searching) dl->AddRect(sp, ImVec2(sp.x + L.search.w, sp.y + L.search.h), C.accent, theme::kRadiusLg + 2, 0, 1.5f);
    }
    if (L.back.w > 0) {
      const ImVec2 bp = scr(L.back);
      ImGui::SetCursorScreenPos(bp);
      const bool clicked = ImGui::InvisibleButton("##back", ImVec2(L.back.w, L.back.h));
      const float t = animate(ImGui::GetItemID(), ImGui::IsItemHovered());
      const ImVec2 b1(bp.x + L.back.w, bp.y + L.back.h);
      dl->AddRectFilled(bp, b1, theme::mix(C.fill, C.fillHover, t), theme::kRadiusLg + 2);
      dl->AddRect(bp, b1, theme::mix(C.line, C.lineHover, t), theme::kRadiusLg + 2);
      ui::drawIcon(dl, ui::Icon::ChevronLeft, ImVec2(bp.x + 18, bp.y + L.back.h * 0.5f), 14, C.text);
      dl->AddCircleFilled(ImVec2(b1.x - 15, bp.y + L.back.h * 0.5f), 3.0f, C.accent);
      textAt(dl, a.fontRegular, theme::kFontBody, ImVec2(bp.x + 32, bp.y + (L.back.h - theme::kFontBody) * 0.5f - 1), C.text, tr("Back to project"),
             L.back.w - 32 - 26);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && ImGui::BeginTooltip()) {
        const std::string pf = a.projectFile();
        ImGui::TextUnformatted(pf.empty() ? a.doc.song().info.title.c_str() : pf.c_str());
        ImGui::EndTooltip();
      }
      if (clicked) a.home.back(a.now);
    }
  }

  // ---- grid ----
  int activate = -1;
  std::vector<int> tileIdx;  // item index of each tile (keyboard order)
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i] == launcher::Item::Tile) tileIdx.push_back(static_cast<int>(i));
  const int nTiles = static_cast<int>(tileIdx.size());
  S.focus = nTiles ? std::clamp(S.focus, 0, nTiles - 1) : 0;
  ImGui::SetCursorScreenPos(scr(L.grid));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  // A little room around the grid so focus rings and outlines are never clipped.
  const float ring = 4;
  ImGui::SetCursorScreenPos(ImVec2(o.x + L.grid.x - ring, o.y + L.grid.y - ring));
  if (ImGui::BeginChild("##launcherGrid", ImVec2(L.grid.w + 2 * ring, L.grid.h + 2 * ring), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
    ImDrawList* gdl = ImGui::GetWindowDrawList();
    const ImVec2 g0 = ImGui::GetCursorScreenPos();
    const ImVec2 base(g0.x + ring, g0.y + ring);
    int ti = 0;
    for (size_t i = 0; i < items.size(); ++i) {
      const Rect& r = L.items[i];
      const ImVec2 p(base.x + r.x, base.y + r.y);
      if (items[i] == launcher::Item::Section) {
        const std::string up = [&] {
          std::string s = tiles[i].name;
          for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          return s;
        }();
        gdl->AddText(a.fontMono, theme::kFontLabel, ImVec2(std::round(p.x + 2), std::round(p.y + r.h - 22)), C.faint, up.c_str());
        continue;
      }
      // Off-screen tiles are skipped (only their room counts).
      const float sy = ImGui::GetScrollY();
      const bool visible = r.b() + ring >= sy && r.y <= sy + L.grid.h + ring;
      if (visible && drawTile(a, ti, tiles[i], p, L, ti == S.focus)) activate = static_cast<int>(i);
      if (S.scrollToFocus && ti == S.focus) {
        if (r.y < sy) ImGui::SetScrollY(std::max(0.0f, r.y - 8));
        else if (r.b() + 2 * ring > sy + L.grid.h) ImGui::SetScrollY(r.b() + 2 * ring - L.grid.h + 8);
        S.scrollToFocus = false;
      }
      ++ti;
    }
    // Empty states.
    if (S.view == 0 && a.recentFiles.empty() && !S.search[0]) {
      const float y = base.y + L.contentH + 28;
      const char* l1 = tr("Your projects will appear here.");
      const char* l2 = tr("Start a new project, open one from disk, or begin from a template.");
      gdl->AddText(a.fontBold, theme::kFontBody, ImVec2(base.x + 2, y), C.dim, l1);
      gdl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(base.x + 2, y + 22), C.faint, l2);
    } else if (nTiles == 0 && S.search[0]) {
      gdl->AddText(a.fontRegular, theme::kFontBody, ImVec2(base.x + 2, base.y + 6), C.dim, tr("Nothing matches the search."));
    }
    ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + L.contentH + ring));
    ImGui::Dummy(ImVec2(1, 1));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  tileMenu(a);

  // ---- keyboard ----
  const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  ImGuiWindow* nav = GImGui->NavWindow;
  const bool mine = !nav || std::string(nav->RootWindow ? nav->RootWindow->Name : nav->Name).find("##launcher") != std::string::npos;
  if (!popup && mine && !a.fileDialog.open && !a.confirmDiscard) {
    if (!io.WantTextInput) {
      auto move = [&](int d) {
        if (!nTiles) return;
        if (S.keyNav) S.focus = std::clamp(S.focus + d, 0, nTiles - 1);
        S.keyNav = true;
        S.scrollToFocus = true;
      };
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move(1);
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move(-1);
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(L.cols);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(-L.cols);
      if (ImGui::IsKeyPressed(ImGuiKey_Home) && nTiles) S.focus = 0, S.keyNav = true, S.scrollToFocus = true;
      if (ImGui::IsKeyPressed(ImGuiKey_End) && nTiles) S.focus = nTiles - 1, S.keyNav = true, S.scrollToFocus = true;
      if (nTiles && S.keyNav && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || ImGui::IsKeyPressed(ImGuiKey_Space)))
        activate = tileIdx[static_cast<size_t>(S.focus)];
      if (nTiles && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) && S.keyNav)
        removeTile(a, tiles[static_cast<size_t>(tileIdx[static_cast<size_t>(S.focus)])]);
      if (nTiles && S.keyNav && (ImGui::IsKeyPressed(ImGuiKey_Menu) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10)))) {
        const Tile& t = tiles[static_cast<size_t>(tileIdx[static_cast<size_t>(S.focus)])];
        if (t.kind != TileKind::New) S.menu = t, S.openMenu = true;
      }
      // J-148: F toggles the focused project's favourite mark.
      if (nTiles && S.keyNav && io.KeyMods == ImGuiMod_None && ImGui::IsKeyPressed(ImGuiKey_F, false))
        toggleFavourite(a, tiles[static_cast<size_t>(tileIdx[static_cast<size_t>(S.focus)])]);
      if (ImGui::IsKeyPressed(ImGuiKey_Tab)) S.view = 1 - S.view, S.focus = 0;
      if (keys::pressed("file.new")) openTile(a, tileOf(TileKind::New));
      if (keys::pressed("file.open")) a.openDialog();
      if (ImGui::IsKeyPressed(ImGuiKey_F) && io.KeyMods == ImGuiMod_Ctrl) S.focusSearch = true;
      if (ImGui::IsKeyPressed(ImGuiKey_F) && io.KeyMods == ImGuiMod_Super) S.focusSearch = true;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (S.search[0]) S.search[0] = 0;
        else a.home.back(a.now);
      }
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      S.search[0] = 0;
      ImGui::ClearActiveID();
    }
  }
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) S.keyNav = false;
  if (activate >= 0) openTile(a, tiles[static_cast<size_t>(activate)]);

  ImGui::End();

  // ---- entrance: a short fade from the background (frames too: off-screen runs are fast) ----
  if (scrollui::animations()) {
    const float tt = static_cast<float>((a.now - S.shownAt) / 0.22);
    const float tf = static_cast<float>(ImGui::GetFrameCount() - S.shownFrame) / 14.0f;
    const float t = ease(std::max(tt, tf));
    if (t < 1.0f)
      ImGui::GetForegroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), theme::withAlpha(C.bg, 1.0f - t));
  }
  drawNotificationCards(a);
  S.tiles = std::move(tiles);
}

}  // namespace gl::launcherui
