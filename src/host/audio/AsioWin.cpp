// Gridline: the Windows side of the ASIO backend (K-6): the installed driver list (registry)
// and loading a driver through COM. Other platforms get an empty list.
// Licence notes: AsioAbi.h and docs/asio.md (Gridline's own declarations, no SDK files).
#include "AsioHost.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#endif

namespace gl::asio {

#ifdef _WIN32

// The structures' binary layout (4-byte packing, 32-bit long) as drivers expect it.
static_assert(sizeof(abi::Long) == 4, "the driver ABI's long is 32 bits");
static_assert(sizeof(abi::ChannelInfo) == 52, "ChannelInfo layout");
static_assert(sizeof(abi::BufferInfo) == 8 + 2 * sizeof(void*), "BufferInfo layout");
static_assert(sizeof(abi::Callbacks) == 4 * sizeof(void*), "Callbacks layout");

namespace {

std::string utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

// The driver's COM interface: IUnknown, then the driver methods in their binary order.
struct IDriverCom : public IUnknown {
  virtual abi::Bool init(void* sysHandle) = 0;
  virtual void getDriverName(char* name) = 0;  // 32 bytes
  virtual abi::Long getDriverVersion() = 0;
  virtual void getErrorMessage(char* text) = 0;  // 124 bytes
  virtual abi::Error start() = 0;
  virtual abi::Error stop() = 0;
  virtual abi::Error getChannels(abi::Long* in, abi::Long* out) = 0;
  virtual abi::Error getLatencies(abi::Long* in, abi::Long* out) = 0;
  virtual abi::Error getBufferSize(abi::Long* minSize, abi::Long* maxSize, abi::Long* preferred, abi::Long* granularity) = 0;
  virtual abi::Error canSampleRate(abi::SampleRate rate) = 0;
  virtual abi::Error getSampleRate(abi::SampleRate* rate) = 0;
  virtual abi::Error setSampleRate(abi::SampleRate rate) = 0;
  virtual abi::Error getClockSources(void* clocks, abi::Long* count) = 0;
  virtual abi::Error setClockSource(abi::Long reference) = 0;
  virtual abi::Error getSamplePosition(abi::Samples64* pos, abi::Samples64* stamp) = 0;
  virtual abi::Error getChannelInfo(abi::ChannelInfo* info) = 0;
  virtual abi::Error createBuffers(abi::BufferInfo* infos, abi::Long count, abi::Long frames, abi::Callbacks* cb) = 0;
  virtual abi::Error disposeBuffers() = 0;
  virtual abi::Error controlPanel() = 0;
  virtual abi::Error future(abi::Long selector, void* opt) = 0;
  virtual abi::Error outputReady() = 0;
};

class ComDriver : public Driver {
 public:
  explicit ComDriver(IDriverCom* p) : p_(p) {}
  ~ComDriver() override {
    if (p_) p_->Release();
  }
  ComDriver(const ComDriver&) = delete;
  ComDriver& operator=(const ComDriver&) = delete;
  bool init(void* sysHandle) override { return p_->init(sysHandle) != 0; }
  std::string errorMessage() override {
    char buf[256] = {};  // the driver writes up to 124 bytes
    p_->getErrorMessage(buf);
    buf[sizeof buf - 1] = 0;
    return buf;
  }
  abi::Error start() override { return p_->start(); }
  abi::Error stop() override { return p_->stop(); }
  abi::Error getChannels(abi::Long* in, abi::Long* out) override { return p_->getChannels(in, out); }
  abi::Error getLatencies(abi::Long* in, abi::Long* out) override { return p_->getLatencies(in, out); }
  abi::Error getBufferSize(abi::Long* a, abi::Long* b, abi::Long* c, abi::Long* d) override { return p_->getBufferSize(a, b, c, d); }
  abi::Error canSampleRate(abi::SampleRate r) override { return p_->canSampleRate(r); }
  abi::Error getSampleRate(abi::SampleRate* r) override { return p_->getSampleRate(r); }
  abi::Error setSampleRate(abi::SampleRate r) override { return p_->setSampleRate(r); }
  abi::Error getChannelInfo(abi::ChannelInfo* i) override { return p_->getChannelInfo(i); }
  abi::Error createBuffers(abi::BufferInfo* i, abi::Long n, abi::Long f, abi::Callbacks* cb) override { return p_->createBuffers(i, n, f, cb); }
  abi::Error disposeBuffers() override { return p_->disposeBuffers(); }
  abi::Error controlPanel() override { return p_->controlPanel(); }
  abi::Error outputReady() override { return p_->outputReady(); }

 private:
  IDriverCom* p_ = nullptr;
};

}  // namespace

std::vector<DriverEntry> listInstalledDrivers() {
  std::vector<DriverEntry> v;
  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return v;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256];
    DWORD len = 256;
    const LONG r = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
    if (r == ERROR_NO_MORE_ITEMS) break;
    if (r != ERROR_SUCCESS) continue;
    wchar_t clsid[64] = {};
    DWORD bytes = sizeof clsid;
    if (RegGetValueW(root, name, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &bytes) != ERROR_SUCCESS) continue;
    DriverEntry e;
    e.name = utf8(name);
    e.clsid = utf8(clsid);
    if (!e.name.empty() && !e.clsid.empty()) v.push_back(std::move(e));
  }
  RegCloseKey(root);
  return v;
}

std::unique_ptr<Driver> loadDriver(const DriverEntry& e, std::string* err) {
  if (e.simulated) return std::make_unique<SimDriver>(simSpecFor(e.name));
  // The UI thread's apartment (SDL may have entered it already; a different mode is kept).
  static const bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  (void)comInit;
  CLSID id{};
  std::wstring w = wide(e.clsid);
  if (w.empty() || CLSIDFromString(w.data(), &id) != NOERROR) {
    if (err) *err = "The ASIO driver \"" + e.name + "\" has an invalid registry entry.";
    return nullptr;
  }
  void* p = nullptr;
  // ASIO drivers use their class id as the interface id too.
  const HRESULT hr = CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, &p);
  if (FAILED(hr) || !p) {
    char buf[64];
    std::snprintf(buf, sizeof buf, " (error 0x%08lx)", static_cast<unsigned long>(hr));
    if (err) *err = "The ASIO driver \"" + e.name + "\" could not be loaded" + buf + ". A 32-bit-only driver cannot load into 64-bit Gridline.";
    return nullptr;
  }
  return std::make_unique<ComDriver>(static_cast<IDriverCom*>(p));
}

#else  // not Windows: no installed drivers; only the simulated ones load

std::vector<DriverEntry> listInstalledDrivers() { return {}; }

std::unique_ptr<Driver> loadDriver(const DriverEntry& e, std::string* err) {
  if (e.simulated) return std::make_unique<SimDriver>(simSpecFor(e.name));
  if (err) *err = "ASIO drivers exist only on Windows.";
  return nullptr;
}

#endif

}  // namespace gl::asio
