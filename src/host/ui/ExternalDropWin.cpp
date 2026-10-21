// Gridline — J-75 on Windows: the main window as an OLE drop target (IDropTarget), so drags
// from Explorer, browsers and sample stores show where they would land while they hover
// (SDL2 alone reports files only when they are dropped). Files (CF_HDROP) and http(s) links
// (the URL formats browsers put on the drag) become xdrop events like on macOS; links download
// into the drop folder (URLDownloadToFile) once dropped.
#if defined(_WIN32)
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "../../core/ExternalDrop.h"

namespace fs = std::filesystem;

namespace gl::xdrop::native {

namespace {

std::string narrow(const wchar_t* w, int len = -1) {
  if (!w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}
std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// The dragged things, in order of preference: files, else web links to audio / MIDI.
std::vector<Item> readData(IDataObject* d) {
  std::vector<Item> out;
  if (!d) return out;
  FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM m{};
  if (SUCCEEDED(d->GetData(&fmt, &m))) {
    if (auto drop = static_cast<HDROP>(GlobalLock(m.hGlobal))) {
      const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        p.resize(len);
        Item it;
        it.path = narrow(p.c_str());
        it.name = fs::path(p).filename().u8string();
        std::error_code ec;
        it.dir = fs::is_directory(fs::path(p), ec);
        out.push_back(it);
      }
      GlobalUnlock(m.hGlobal);
    }
    ReleaseStgMedium(&m);
    if (!out.empty()) return out;
  }
  // Links: "UniformResourceLocatorW" (browsers), else plain text holding a URL.
  const CLIPFORMAT urlW = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocatorW"));
  for (CLIPFORMAT cf : {urlW, static_cast<CLIPFORMAT>(CF_UNICODETEXT)}) {
    FORMATETC f{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM s{};
    if (FAILED(d->GetData(&f, &s))) continue;
    std::string text;
    if (auto w = static_cast<const wchar_t*>(GlobalLock(s.hGlobal))) {
      text = narrow(w);
      GlobalUnlock(s.hGlobal);
    }
    ReleaseStgMedium(&s);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t end = text.find_first_of("\r\n", pos);
      if (end == std::string::npos) end = text.size();
      const std::string line = text.substr(pos, end - pos);
      pos = end + 1;
      if (const std::string p = pathFromFileUrl(line); !p.empty()) {
        Item it;
        it.path = p;
        it.name = fs::u8path(p).filename().u8string();
        out.push_back(it);
      } else if (isWebUrl(line) && browse::listable(browse::kindOf(fileNameForUrl(line)))) {
        Item it;
        it.url = line;
        it.name = fileNameForUrl(line);
        it.pending = true;
        out.push_back(it);
      }
    }
    if (!out.empty()) break;
  }
  return out;
}

Mods modsNow() {
  Mods m;
  m.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  m.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  m.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  return m;
}

class DropTarget final : public IDropTarget {
 public:
  explicit DropTarget(HWND w) : wnd_(w) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *obj = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *obj = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL pt, DWORD* effect) override {
    items_ = readData(d);
    setCancelled(false);
    if (items_.empty()) {
      *effect = DROPEFFECT_NONE;
      return S_OK;
    }
    Event e = at(EventType::Enter, pt);
    e.items = items_;
    post(e);
    *effect = DROPEFFECT_COPY;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
    if (items_.empty()) {
      *effect = DROPEFFECT_NONE;
      return S_OK;
    }
    post(at(EventType::Move, pt));
    *effect = hoverAccepted() && !cancelled() ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    if (!items_.empty()) post(Event{EventType::Leave});
    items_.clear();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL pt, DWORD* effect) override {
    std::vector<Item> items = readData(d);
    if (items.empty() || cancelled()) {
      if (!items_.empty()) post(Event{EventType::Leave});
      items_.clear();
      *effect = DROPEFFECT_NONE;
      return S_OK;
    }
    Event e = at(EventType::Drop, pt);
    e.items = items;
    e.token = newToken();
    for (size_t i = 0; i < items.size(); ++i)
      if (!items[i].url.empty()) download(e.token, i, items[i].url);
    post(e);
    items_.clear();
    *effect = DROPEFFECT_COPY;
    return S_OK;
  }

 private:
  Event at(EventType t, POINTL pt) const {
    POINT p{pt.x, pt.y};
    ScreenToClient(wnd_, &p);
    Event e;
    e.type = t;
    e.x = static_cast<float>(p.x);
    e.y = static_cast<float>(p.y);
    e.mods = modsNow();
    e.hasMods = true;
    return e;
  }
  HWND wnd_;
  std::atomic<ULONG> refs_{1};
  std::vector<Item> items_;
};

}  // namespace

bool install(void* nativeWindow) {
  HWND w = static_cast<HWND>(nativeWindow);
  if (!w) return false;
  const HRESULT ole = OleInitialize(nullptr);  // the UI thread is single-threaded (STA)
  if (FAILED(ole)) return false;
  auto* t = new DropTarget(w);
  const bool ok = SUCCEEDED(RegisterDragDrop(w, t));
  t->Release();  // the registration holds its own reference
  return ok;
}

void poll() {}

void download(uint64_t token, size_t index, const std::string& url) {
  std::thread([token, index, url] {
    const std::string dest = uniquePath(dropFolder(), fileNameForUrl(url));
    Event e;
    e.token = token;
    e.index = index;
    if (SUCCEEDED(URLDownloadToFileW(nullptr, widen(url).c_str(), widen(dest).c_str(), 0, nullptr))) {
      e.type = EventType::Arrived;
      e.path = dest;
    } else {
      e.type = EventType::Failed;
      e.error = "download failed";
      std::error_code ec;
      fs::remove(fs::u8path(dest), ec);
    }
    post(e);
  }).detach();
}

}  // namespace gl::xdrop::native
#endif
