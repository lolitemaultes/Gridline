// Gridline — J-75: drags from other applications inside the interface (ExternalDragUi.h).
#include "ExternalDragUi.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "App.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::xdragui {

namespace fs = std::filesystem;
using browse::Kind;

namespace {

enum class Phase { Idle, Hover, DropNext, Deliver, Cancelled };

struct State {
  Phase phase = Phase::Idle;
  std::vector<xdrop::Item> items;
  ImVec2 pos{-FLT_MAX, -FLT_MAX};
  int hoverFrames = 0;
  bool consumed = false;
  bool dropAfter = false;  // the drop frame is the next one
  uint64_t token = 0;
  bool shift = false, alt = false;  // modifier keys fed into ImGui (window system knows them)
  bool fedShift = false, fedAlt = false;
} st;

struct Deferred {
  uint64_t token = 0;
  std::vector<xdrop::Item> items;
  std::vector<int> status;  // 0 waiting, 1 here, 2 failed
  std::vector<double> progress;
  browse::DropTarget target = browse::DropTarget::Workspace;
  browserui::DropPlace place;
  xdrop::Mods mods;
  double started = 0;
};
std::vector<Deferred> gDeferred;
std::vector<xdrop::Event> gEarly;  // arrivals before their drop was deferred

std::vector<xdrop::Item> gSdlItems;  // SDL drop in progress
bool gSdlCollecting = false;

const ImGuiID kExternId = ImHashStr("#SourceExtern");

double nowSeconds() { return ImGui::GetTime(); }

void feedMods(bool shift, bool alt) {
  ImGuiIO& io = ImGui::GetIO();
  if (shift != st.fedShift) io.AddKeyEvent(ImGuiMod_Shift, shift);
  if (alt != st.fedAlt) io.AddKeyEvent(ImGuiMod_Alt, alt);
  st.fedShift = shift;
  st.fedAlt = alt;
}

void reset(Phase to) {
  feedMods(false, false);
  st.phase = to;
  st.items.clear();
  st.hoverFrames = 0;
  st.consumed = false;
  st.dropAfter = false;
  xdrop::setHoverAccepted(false);
}

void cancel(App& a, bool byUser) {
  ImGuiContext& g = *GImGui;
  if (g.DragDropActive && g.DragDropPayload.SourceId == kExternId) ImGui::ClearDragDrop();
  reset(byUser ? Phase::Cancelled : Phase::Idle);
  if (byUser) {
    xdrop::setCancelled(true);
    a.notify(i18n::tr("Drop cancelled"));
  } else {
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
}

std::string fmt(const char* f, int n, const char* s) {
  char buf[256];
  std::snprintf(buf, sizeof buf, f, n, s);
  return buf;
}

// What the drag shows next to the pointer.
void preview() {
  const size_t n = st.items.size();
  const xdrop::Item& first = st.items.front();
  const Kind k = xdrop::kindOf(first);
  const ui::Icon icon = k == Kind::Audio                      ? ui::Icon::Wave
                        : k == Kind::Midi                     ? ui::Icon::Piano
                        : k == Kind::Folder                   ? ui::Icon::Folder
                        : k == Kind::Project || k == Kind::Flp ? ui::Icon::Song
                                                               : ui::Icon::File;
  std::string text = n == 1 ? xdrop::displayName(first) : fmt(i18n::tr("%d files: %s"), static_cast<int>(n), xdrop::displayName(first).c_str());
  if (n > 1) text += ", …";
  ui::DragPreview(icon, text.c_str(), k == Kind::Audio ? theme::rgb(theme::role::kPaint) : 0);
}

void submitSource() {
  if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) return;
  const xdrop::Item& first = st.items.front();
  if (st.items.size() == 1 && !first.pending && !first.dir) {
    // One file on disk travels like a browser file: every sample slot takes it.
    const char* type = browse::kindOf(first.path) == Kind::Audio ? "GL_FILE" : "GL_ITEM";
    ImGui::SetDragDropPayload(type, first.path.data(), first.path.size(), ImGuiCond_Always);
  } else {
    std::string names;
    for (const xdrop::Item& it : st.items) names += xdrop::displayName(it) + "\n";
    ImGui::SetDragDropPayload("GL_FILES", names.data(), names.size(), ImGuiCond_Always);
  }
  preview();
  ImGui::EndDragDropSource();
}

void finishDeferred(App& a, Deferred& d) {
  std::vector<xdrop::Item> ready;
  std::vector<std::string> failed;
  for (size_t i = 0; i < d.items.size(); ++i) {
    if (d.status[i] == 2) {
      failed.push_back(xdrop::displayName(d.items[i]));
      continue;
    }
    ready.push_back(d.items[i]);
  }
  if (!failed.empty()) a.notify(std::string(i18n::tr("Could not receive: ")) + failed.front() + (failed.size() > 1 ? " (+" + std::to_string(failed.size() - 1) + ")" : ""), true);
  if (ready.empty()) return;
  if (d.target == browse::DropTarget::Workspace) browserui::openFiles(a, ready);
  else browserui::performItems(a, ready, d.target, d.place, d.mods);
}

void apply(const xdrop::Event& e, Deferred& d) {
  if (e.index >= d.items.size()) return;
  xdrop::Item& it = d.items[e.index];
  if (e.type == xdrop::EventType::Arrived) {
    it.path = e.path;
    it.name = fs::path(e.path).filename().string();
    it.pending = false;
    it.guessed = false;
    it.dir = fs::is_directory(e.path);
    d.status[e.index] = 1;
  } else if (e.type == xdrop::EventType::Failed) {
    d.status[e.index] = 2;
    if (!e.error.empty()) it.name += " (" + e.error + ")";
  } else {
    d.progress[e.index] = e.progress;
  }
}

bool routeArrival(const xdrop::Event& e) {
  for (Deferred& d : gDeferred)
    if (d.token == e.token) {
      apply(e, d);
      return true;
    }
  return false;
}

}  // namespace

// ------------------------------------------------------------------ frame

void pump(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  {
    std::string dir = a.projectPath.empty() ? (fs::path(a.configDir()) / "Dropped").string()
                                            : (fs::path(a.projectPath).parent_path() / "Dropped").string();
    xdrop::setDropFolder(dir);
  }
  xdrop::native::poll();
  for (const xdrop::Event& e : xdrop::take()) {
    switch (e.type) {
      case xdrop::EventType::Enter:
        reset(e.items.empty() ? Phase::Idle : Phase::Hover);
        xdrop::setCancelled(false);
        st.items = e.items;
        st.pos = ImVec2(e.x, e.y);
        io.AddMousePosEvent(e.x, e.y);
        if (e.hasMods && st.phase == Phase::Hover) feedMods(e.mods.shift, e.mods.alt);
        break;
      case xdrop::EventType::Move:
        if (st.phase == Phase::Hover || st.phase == Phase::DropNext) {
          st.pos = ImVec2(e.x, e.y);
          io.AddMousePosEvent(e.x, e.y);
          if (e.hasMods) feedMods(e.mods.shift, e.mods.alt);
        }
        break;
      case xdrop::EventType::Leave:
        if (st.phase == Phase::Hover) cancel(a, false);
        else if (st.phase == Phase::Cancelled) reset(Phase::Idle);
        break;
      case xdrop::EventType::Drop:
        if (st.phase == Phase::Cancelled || e.items.empty()) {
          reset(Phase::Idle);
          break;
        }
        st.items = e.items;
        st.token = e.token;
        st.pos = ImVec2(e.x, e.y);
        io.AddMousePosEvent(e.x, e.y);
        if (e.hasMods) feedMods(e.mods.shift, e.mods.alt);
        // One more hover frame at the drop point, then the drop frame.
        st.phase = Phase::DropNext;
        break;
      case xdrop::EventType::Arrived:
      case xdrop::EventType::Failed:
      case xdrop::EventType::Progress:
        if (!routeArrival(e) && e.type != xdrop::EventType::Progress) gEarly.push_back(e);
        break;
    }
  }
  // Drops whose files are all here (or failed), or that waited too long.
  for (size_t k = 0; k < gDeferred.size();) {
    Deferred& d = gDeferred[k];
    const bool done = std::none_of(d.status.begin(), d.status.end(), [](int s) { return s == 0; });
    if (!done && nowSeconds() - d.started > 180.0) {
      for (int& s : d.status)
        if (s == 0) s = 2;
      continue;
    }
    if (!done) {
      ++k;
      continue;
    }
    Deferred finished = std::move(d);
    gDeferred.erase(gDeferred.begin() + static_cast<long>(k));
    finishDeferred(a, finished);
  }
}

void feedPointer() {
  if (active() && st.pos.x > -FLT_MAX) ImGui::GetIO().AddMousePosEvent(st.pos.x, st.pos.y);
}

void beginFrame(App& a) {
  if (st.phase == Phase::Hover || st.phase == Phase::DropNext) {
    if (st.phase == Phase::Hover && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      cancel(a, true);
      return;
    }
    submitSource();
    ++st.hoverFrames;
    if (st.phase == Phase::DropNext) st.dropAfter = true;
    // No window took the drag last frame: what dropping here would do.
    if (GImGui->DragDropAcceptIdPrev == 0 && !st.items.empty()) {
      const xdrop::Plan p = browserui::planFor(a, st.items, browse::DropTarget::Workspace, browserui::DropPlace{}, browserui::currentMods());
      if (!p.label.empty()) a.setHint(p.label);
      xdrop::setHoverAccepted(p.accepted());
    } else {
      xdrop::setHoverAccepted(true);
    }
  }
  if (!gDeferred.empty() && a.hint.empty()) {
    int waitingN = 0;
    double prog = -1;
    std::string name;
    for (const Deferred& d : gDeferred)
      for (size_t i = 0; i < d.items.size(); ++i)
        if (d.status[i] == 0) {
          ++waitingN;
          if (name.empty()) name = xdrop::displayName(d.items[i]), prog = d.progress[i];
        }
    std::string h = waitingN == 1 ? std::string(i18n::tr("Receiving ")) + name : fmt(i18n::tr("Receiving %d files (%s …)"), waitingN, name.c_str());
    if (prog >= 0) h += " " + std::to_string(static_cast<int>(prog * 100)) + "%";
    a.setHint(h);
  }
}

void endFrame(App& a) {
  if (st.phase == Phase::Deliver) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    const bool delivered = st.consumed || (pl && pl->Delivery);
    if (GImGui->DragDropActive) ImGui::ClearDragDrop();
    const std::vector<xdrop::Item> items = st.items;
    if (!delivered) browserui::openFiles(a, items);  // dropped where no window takes it
    reset(Phase::Idle);
    return;
  }
  if (st.dropAfter) {
    st.dropAfter = false;
    st.phase = Phase::Deliver;
  }
}

bool active() { return st.phase == Phase::Hover || st.phase == Phase::DropNext || st.phase == Phase::Deliver; }
const std::vector<xdrop::Item>& items() { return st.items; }
bool ownsPayload(const ImGuiPayload* pl) { return pl && active() && pl->SourceId == kExternId; }
void markConsumed() { st.consumed = true; }
size_t waiting() { return gDeferred.size(); }

void defer(App& a, const std::vector<xdrop::Item>& items, browse::DropTarget target, const browserui::DropPlace& place, const xdrop::Mods& mods) {
  Deferred d;
  d.token = st.token;
  d.items = items;
  d.target = target;
  d.place = place;
  d.mods = mods;
  d.started = nowSeconds();
  d.progress.assign(items.size(), -1.0);
  int n = 0;
  for (const xdrop::Item& it : items) {
    d.status.push_back(it.pending ? 0 : 1);
    n += it.pending;
  }
  for (size_t k = 0; k < gEarly.size();) {
    if (gEarly[k].token == d.token) {
      apply(gEarly[k], d);
      gEarly.erase(gEarly.begin() + static_cast<long>(k));
    } else {
      ++k;
    }
  }
  gDeferred.push_back(std::move(d));
  a.notify(n == 1 ? std::string(i18n::tr("Receiving the dropped file…")) : fmt(i18n::tr("Receiving %d dropped files…"), n, ""));
}

// ------------------------------------------------------------------- SDL

void sdlDropBegin() {
  gSdlItems.clear();
  gSdlCollecting = true;
}

void sdlDropFile(const std::string& path) {
  xdrop::Item it;
  it.path = path;
  it.name = fs::path(path).filename().string();
  std::error_code ec;
  it.dir = fs::is_directory(path, ec);
  gSdlItems.push_back(it);
}

void sdlDropText(const std::string& text) {
  // text/uri-list: one link per line.
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find_first_of("\r\n", pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (line.empty() || line[0] == '#') continue;
    if (const std::string p = xdrop::pathFromFileUrl(line); !p.empty()) {
      sdlDropFile(p);
    } else if (xdrop::isWebUrl(line) && browse::listable(browse::kindOf(xdrop::fileNameForUrl(line)))) {
      xdrop::Item it;
      it.url = line;
      it.name = xdrop::fileNameForUrl(line);
      it.pending = true;
      gSdlItems.push_back(it);
    }
  }
}

void sdlDropComplete(float x, float y) {
  gSdlCollecting = false;
  if (gSdlItems.empty()) return;
  xdrop::Event e;
  e.type = xdrop::EventType::Enter;
  e.x = x;
  e.y = y;
  e.items = gSdlItems;
  xdrop::post(e);
  e.type = xdrop::EventType::Drop;
  e.token = xdrop::newToken();
  for (size_t i = 0; i < e.items.size(); ++i)
    if (!e.items[i].url.empty()) xdrop::native::download(e.token, i, e.items[i].url);
  xdrop::post(e);
  gSdlItems.clear();
}

// ------------------------------------------------------------------- dev

namespace {
std::vector<xdrop::Item> devItems(const std::vector<std::string>& files) {
  std::vector<xdrop::Item> out;
  for (const std::string& f : files) {
    xdrop::Item it;
    if (f.rfind("promise:", 0) == 0) {
      it.url = f.substr(8);  // the source copied on drop (dev only)
      it.name = fs::path(it.url).filename().string();
      it.pending = true;
    } else {
      it.path = f;
      it.name = fs::path(f).filename().string();
      std::error_code ec;
      it.dir = fs::is_directory(f, ec);
    }
    out.push_back(it);
  }
  return out;
}
std::vector<std::string> gDevFiles;
}  // namespace

void devEnter(float x, float y, const std::vector<std::string>& files) {
  gDevFiles = files;
  xdrop::Event e;
  e.type = xdrop::EventType::Enter;
  e.x = x;
  e.y = y;
  e.items = devItems(files);
  for (xdrop::Item& it : e.items)
    if (it.pending) it.url.clear();  // hover: not known yet
  xdrop::post(e);
}

void devMove(float x, float y) {
  xdrop::Event e;
  e.type = xdrop::EventType::Move;
  e.x = x;
  e.y = y;
  xdrop::post(e);
}

void devDrop(float x, float y, bool here) {
  xdrop::Event e;
  e.type = xdrop::EventType::Drop;
  e.x = here ? x : st.pos.x;
  e.y = here ? y : st.pos.y;
  e.token = xdrop::newToken();
  e.items = devItems(gDevFiles);
  const std::string folder = xdrop::dropFolder();
  for (size_t i = 0; i < e.items.size(); ++i) {
    if (!e.items[i].pending) continue;
    // A promise kept a little later, from another thread (like a source app).
    const std::string src = e.items[i].url;
    e.items[i].url.clear();
    const uint64_t token = e.token;
    std::thread([src, folder, token, i] {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      xdrop::Event r;
      r.token = token;
      r.index = i;
      const std::string dest = xdrop::uniquePath(folder, fs::path(src).filename().string());
      std::error_code ec;
      fs::copy_file(src, dest, ec);
      if (ec) {
        r.type = xdrop::EventType::Failed;
        r.error = ec.message();
      } else {
        r.type = xdrop::EventType::Arrived;
        r.path = dest;
      }
      xdrop::post(r);
    }).detach();
  }
  xdrop::post(e);
}

void devLeave() {
  xdrop::Event e;
  e.type = xdrop::EventType::Leave;
  xdrop::post(e);
}

}  // namespace gl::xdragui
