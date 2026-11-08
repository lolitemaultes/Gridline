// Gridline — files the operating system asks Gridline to open. See OpenDocuments.h.
#include "OpenDocuments.h"
#include <cstring>
#include <filesystem>
#include <system_error>
#include "../core/ExternalDrop.h"
#include "App.h"
#include "BrowserUi.h"

namespace fs = std::filesystem;

namespace gl::opendocs {

namespace {

std::vector<std::string> gCollecting;  // files of the request that is still arriving
std::vector<std::string> gReady;       // files of complete requests
std::vector<std::string> gCommandLine;  // files main.cpp opens itself (passed over while starting)

// The same file however it is written (relative, "..", a link): compared by its full path.
std::string key(const std::string& path) {
  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs::u8path(path), ec);
  if (ec) p = fs::u8path(path).lexically_normal();
  return p.u8string();
}

}  // namespace

std::string requestPath(const std::string& raw) {
  if (raw.empty()) return "";
  if (raw.rfind("file:", 0) == 0) return xdrop::pathFromFileUrl(raw);
  if (raw.find("://") != std::string::npos) return "";  // another kind of link: not a file
  return raw;
}

void setCommandLine(int argc, char** argv) {
  gCommandLine.clear();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dev-os-open") {
      ++i;  // (the test's own request, not a command line file)
      continue;
    }
    if (!a.empty() && a[0] != '-') gCommandLine.push_back(key(a));
  }
}

void startupDone() { gCommandLine.clear(); }

bool take(const SDL_Event& ev) {
  if (ev.type != SDL_DROPBEGIN && ev.type != SDL_DROPFILE && ev.type != SDL_DROPTEXT && ev.type != SDL_DROPCOMPLETE) return false;
  if (ev.drop.windowID != 0) return false;  // a drop on a window: ExternalDragUi
  if (ev.type == SDL_DROPBEGIN) {
    gCollecting.clear();
  } else if (ev.type == SDL_DROPFILE || ev.type == SDL_DROPTEXT) {
    const std::string raw = ev.drop.file ? ev.drop.file : "";
    SDL_free(ev.drop.file);
    if (const std::string path = requestPath(raw); !path.empty()) {
      const std::string k = key(path);
      bool fromCommandLine = false;
      for (size_t i = 0; i < gCommandLine.size(); ++i)
        if (gCommandLine[i] == k) {
          gCommandLine.erase(gCommandLine.begin() + static_cast<long>(i));
          fromCommandLine = true;
          break;
        }
      if (!fromCommandLine) gCollecting.push_back(path);
    }
  } else {  // SDL_DROPCOMPLETE
    gReady.insert(gReady.end(), gCollecting.begin(), gCollecting.end());
    gCollecting.clear();
  }
  return true;
}

bool pending() { return !gReady.empty(); }

void queue(const std::vector<std::string>& files) { gReady.insert(gReady.end(), files.begin(), files.end()); }

std::vector<std::string> takeReady() {
  std::vector<std::string> out;
  out.swap(gReady);
  return out;
}

void open(App& a, const std::vector<std::string>& files) {
  std::vector<xdrop::Item> items;
  for (const std::string& f : files) {
    xdrop::Item it;
    it.path = f;
    it.name = fs::u8path(f).filename().u8string();
    std::error_code ec;
    it.dir = fs::is_directory(fs::u8path(f), ec);
    items.push_back(it);
  }
  if (!items.empty()) browserui::openFiles(a, items);
}

void post(const std::vector<std::string>& paths) {
  SDL_Event e;
  std::memset(&e, 0, sizeof e);
  e.type = SDL_DROPBEGIN;
  SDL_PushEvent(&e);
  for (const std::string& p : paths) {
    std::memset(&e, 0, sizeof e);
    e.type = SDL_DROPFILE;
    e.drop.file = SDL_strdup(p.c_str());
    SDL_PushEvent(&e);
  }
  std::memset(&e, 0, sizeof e);
  e.type = SDL_DROPCOMPLETE;
  SDL_PushEvent(&e);
}

}  // namespace gl::opendocs
