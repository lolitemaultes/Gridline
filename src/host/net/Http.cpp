// Gridline - J-89: HTTPS requests for the update check (see Http.h).
// Windows: WinHTTP. Linux / BSD: the curl or wget program. macOS: HttpMac.mm.
#include "Http.h"
#include "../../core/ThreadName.h"
#include "GridlineVersion.h"
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include "../../core/Platform.h"
#include "../../core/Settings.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <winhttp.h>
#elif !defined(__APPLE__)
#include <unistd.h>
#endif

namespace gl::http {

namespace fs = std::filesystem;

namespace detail {
bool loopbackTest(const std::string& url) {
  const char* t = std::getenv("GRIDLINE_UPDATE_TEST_SERVER");
  if (!t || std::strncmp(t, "http://127.0.0.1:", 17) != 0) return false;
  const std::string base = std::string(t) + "/";
  return url.rfind(base, 0) == 0;
}
bool httpsOnly(const std::string& url) { return url.rfind("https://", 0) == 0 || loopbackTest(url); }
}  // namespace detail

update::CondFetcher condFetcher() {
  return [](const std::string& url, const std::string& etag, int timeoutSeconds) { return getConditional(url, etag, timeoutSeconds); };
}

int checkCommand(bool beta) {
  update::CheckResult r;
  r.current = GRIDLINE_VERSION;
  if (update::disabledByEnvironment()) {
    r.status = update::Status::Disabled;
  } else {
    Settings st;
    st.load(Settings::defaultPath());
    r = update::check(fetcher(), GRIDLINE_VERSION, beta || st.getBool("update.beta"));
  }
  std::printf("%s\n", update::describe(r).c_str());
  if (r.status == update::Status::Available) {
    if (const update::Asset* a = update::pickAsset(r.release, update::currentTarget())) std::printf("Download: %s\n", a->url.c_str());
    const std::string notes = update::notesExcerpt(r.release.body, 600);
    if (!notes.empty()) std::printf("\n%s\n", notes.c_str());
  }
  std::fflush(stdout);
  switch (r.status) {
    case update::Status::Available: return 2;
    case update::Status::UpToDate:
    case update::Status::NoRelease:
    case update::Status::Disabled: return 0;
    default: return 1;
  }
}

update::Fetcher fetcher() {
  return [](const std::string& url) { return get(url); };
}

update::Downloader downloader() {
  return [](const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
            const std::atomic<bool>* cancel, std::string* err) { return download(url, path, progress, cancel, err); };
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

bool offlineError(DWORD e) {
  return e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT || e == ERROR_WINHTTP_TIMEOUT ||
         e == ERROR_WINHTTP_CONNECTION_ERROR;
}

// One GET; the body goes to `sink` (false = stop). Status in `status`. `extra`: more request
// headers ("Name: value\r\n" each); `etag` receives the answer's ETag.
bool request(const std::string& url, int timeoutMs, bool api, const std::function<bool(const char*, size_t, int64_t)>& sink, int& status,
             std::string& err, bool& offline, const std::string& extra = std::string(), std::string* etag = nullptr) {
  status = 0;
  offline = false;
  if (!detail::httpsOnly(url)) {
    err = "not an https:// address";
    return false;
  }
  std::wstring wurl = widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2047;
  wchar_t extra[2048] = {};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 2047;
  const bool plain = detail::loopbackTest(url);  // tests' own local server only
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTPS && !(plain && uc.nScheme == INTERNET_SCHEME_HTTP))) {
    err = "invalid address";
    return false;
  }
  const std::wstring ua = widen(update::userAgent());
  Handle session, connection, req;
  session.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h) session.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h) {
    err = "WinHTTP is not available";
    return false;
  }
  WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
  connection.h = WinHttpConnect(session.h, host, uc.nPort, 0);
  const std::wstring object = std::wstring(path) + extra;
  if (connection.h)
    req.h = WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!req.h) {
    offline = offlineError(GetLastError());
    err = "could not open the connection";
    return false;
  }
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;  // redirects stay on HTTPS
  WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
  const std::wstring headerText =
      widen((api ? std::string("Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n") : std::string()) + extra);
  if (!WinHttpSendRequest(req.h, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
                          headerText.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.h, nullptr)) {
    const DWORD e = GetLastError();
    offline = offlineError(e);
    err = offline ? "no connection" : "request failed (WinHTTP error " + std::to_string(e) + ")";
    return false;
  }
  DWORD code = 0, len = sizeof code;
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                      WINHTTP_NO_HEADER_INDEX);
  status = static_cast<int>(code);
  if (etag) {
    wchar_t tag[512] = {};
    DWORD tagLen = sizeof tag;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX, tag, &tagLen, WINHTTP_NO_HEADER_INDEX)) *etag = narrow(tag);
  }
  int64_t total = -1;
  DWORD clen = 0;
  len = sizeof clen;
  if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &clen, &len,
                          WINHTTP_NO_HEADER_INDEX))
    total = static_cast<int64_t>(clen);
  std::string buf(64 * 1024, '\0');
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req.h, &avail)) {
      const DWORD e = GetLastError();
      offline = offlineError(e);
      err = "the connection broke off";
      return false;
    }
    if (avail == 0) break;
    DWORD got = 0;
    if (!WinHttpReadData(req.h, buf.data(), std::min<DWORD>(avail, static_cast<DWORD>(buf.size())), &got)) {
      err = "the connection broke off";
      return false;
    }
    if (got == 0) break;
    if (!sink(buf.data(), got, total)) {
      err = "cancelled";
      return false;
    }
  }
  return true;
}

}  // namespace

update::HttpResult getConditional(const std::string& url, const std::string& etag, int timeoutSeconds) {
  update::HttpResult r;
  std::string body;
  const bool ok = request(url, timeoutSeconds * 1000, true,
                          [&body](const char* p, size_t n, int64_t) {
                            if (body.size() + n > (20u << 20)) return false;  // far beyond any release list
                            body.append(p, n);
                            return true;
                          },
                          r.status, r.error, r.offline, etag.empty() ? std::string() : "If-None-Match: " + etag + "\r\n", &r.etag);
  if (ok) r.body = std::move(body);
  else r.status = 0;  // the error says what went wrong
  return r;
}

update::HttpResult get(const std::string& url, int timeoutSeconds) { return getConditional(url, std::string(), timeoutSeconds); }

bool downloadContinue(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
                      const std::atomic<bool>* cancel, int stallSeconds, std::string* err) {
  for (int pass = 0; pass < 2; ++pass) {
    std::error_code ec;
    int64_t have = 0;
    if (fs::exists(fs::u8path(path), ec)) have = static_cast<int64_t>(fs::file_size(fs::u8path(path), ec));
    if (ec) have = 0;
    std::ofstream out;
    int status = 0;
    bool offline = false, opened = false;
    int64_t received = have;
    std::string e;
    const bool ok = request(url, std::max(1, stallSeconds) * 1000, false,
                            [&](const char* p, size_t n, int64_t total) {
                              if (cancel && cancel->load()) return false;
                              if (!opened) {
                                if (status == 206) out.open(fs::u8path(path), std::ios::binary | std::ios::app);
                                else if (status == 200) out.open(fs::u8path(path), std::ios::binary | std::ios::trunc), received = 0;
                                else return false;
                                opened = true;
                              }
                              out.write(p, static_cast<std::streamsize>(n));
                              received += static_cast<int64_t>(n);
                              if (progress) progress(received, total >= 0 ? total + (status == 206 ? have : 0) : -1);
                              return static_cast<bool>(out);
                            },
                            status, e, offline, have > 0 ? "Range: bytes=" + std::to_string(have) + "-\r\n" : std::string());
    const bool wrote = !opened || static_cast<bool>(out);
    out.close();
    if (cancel && cancel->load()) {
      if (err) *err = "cancelled";
      return false;
    }
    if (status == 416) return true;  // already whole; the caller checks it
    if (ok && wrote && (status == 200 || status == 206)) return true;
    if (!wrote) {
      if (err) *err = "could not write the file (disk full?)";
      return false;
    }
    if (status && status != 200 && status != 206) {
      if (err) *err = "the server refused the download (HTTP " + std::to_string(status) + ")";
      return false;
    }
    if (err) *err = offline ? "no connection" : e.empty() ? std::string("the connection broke off") : e;
    if (pass == 0 && !offline) continue;
    return false;
  }
  return false;
}

bool download(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
              const std::atomic<bool>* cancel, std::string* err) {
  std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  int64_t received = 0;
  int status = 0;
  bool offline = false;
  std::string e;
  const bool ok = request(url, 60 * 1000, false,
                          [&](const char* p, size_t n, int64_t total) {
                            if (cancel && cancel->load()) return false;
                            if (status != 200) return false;
                            out.write(p, static_cast<std::streamsize>(n));
                            received += static_cast<int64_t>(n);
                            if (progress) progress(received, total);
                            return static_cast<bool>(out);
                          },
                          status, e, offline);
  out.close();
  if (!ok || status != 200 || !out) {
    if (err) *err = status && status != 200 ? "HTTP " + std::to_string(status) : e.empty() ? std::string("write failed") : e;
    return false;
  }
  return true;
}

std::string downloadsFolder() {
  PWSTR p = nullptr;
  std::string dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &p)) && p) dir = narrow(p);
  if (p) CoTaskMemFree(p);
  if (dir.empty()) dir = (fs::u8path(platform::homeDir()) / "Downloads").u8string();
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  return dir;
}

bool reveal(const std::string& path) {
  const std::wstring args = L"/select,\"" + widen(fs::u8path(path).make_preferred().u8string()) + L"\"";
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

#elif !defined(__APPLE__)

// ---------------------------------------------------------------- Linux / BSD: curl or wget

namespace {

std::string findTool(const char* name) {
  std::string dirs = std::getenv("PATH") ? std::getenv("PATH") : "";
  dirs += ":/usr/bin:/bin:/usr/local/bin:/usr/pkg/bin";
  std::stringstream ss(dirs);
  for (std::string d; std::getline(ss, d, ':');) {
    if (d.empty()) continue;
    const std::string p = d + "/" + name;
    if (::access(p.c_str(), X_OK) == 0) return p;
  }
  return {};
}

std::string readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// The status of the last response in a header dump / wget log ("HTTP/1.1 200 OK").
int lastStatus(const std::string& text) {
  int status = 0;
  std::stringstream ss(text);
  for (std::string line; std::getline(ss, line);) {
    const size_t at = line.find("HTTP/");
    if (at == std::string::npos || line.find_first_not_of(" \t") != at) continue;
    const size_t sp = line.find(' ', at);
    if (sp != std::string::npos) status = std::atoi(line.c_str() + sp + 1);
  }
  return status;
}

fs::path scratchBase() {
  static std::atomic<int> counter{0};
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec);
  if (ec) dir = "/tmp";
  return dir / ("gridline-http-" + std::to_string(platform::processId()) + "-" + std::to_string(counter++));
}

// Runs a tool until it exits, `seconds` pass (killed) or `cancel` is set (killed); `tick`
// runs every 100 ms while waiting. Exit code, or -1 when killed / not started.
int run(const std::string& exe, const std::vector<std::string>& args, double seconds, const std::atomic<bool>* cancel,
        const std::function<void()>& tick) {
  platform::Process p;
  if (!p.start(exe, args)) return -1;
  const auto t0 = std::chrono::steady_clock::now();
  while (!p.wait(0.1)) {
    if (tick) tick();
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if ((cancel && cancel->load()) || (seconds > 0 && el > seconds)) {
      p.kill();
      p.wait(5);
      return -1;
    }
  }
  return p.status().crashed ? -1 : p.status().code;
}

// The ETag of the last response in a header dump ("" when none).
std::string lastEtag(const std::string& text) {
  std::string etag;
  std::stringstream ss(text);
  for (std::string line; std::getline(ss, line);) {
    if (line.rfind("HTTP/", 0) == 0) etag.clear();
    std::string low = line.substr(0, 5);
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == "etag:") {
      std::string v = line.substr(5);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
      while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
      etag = v;
    }
  }
  return etag;
}

// curl's allowed protocols: HTTPS, and plain HTTP only for tests' own local server.
std::string protos(const std::string& url) { return detail::loopbackTest(url) ? "=http,https" : "=https"; }

}  // namespace

update::HttpResult getConditional(const std::string& url, const std::string& etag, int timeoutSeconds) {
  update::HttpResult r;
  if (!detail::httpsOnly(url)) {
    r.error = "not an https:// address";
    return r;
  }
  const std::string curl = findTool("curl"), wget = curl.empty() ? findTool("wget") : std::string();
  if (curl.empty() && wget.empty()) {
    r.error = "neither curl nor wget is installed";
    return r;
  }
  const fs::path base = scratchBase();
  const fs::path body = base.string() + ".body", head = base.string() + ".head";
  const std::string ua = update::userAgent(), t = std::to_string(timeoutSeconds);
  const std::string connect = std::to_string(std::min(10, std::max(1, timeoutSeconds)));
  int code = 0;
  if (!curl.empty()) {
    std::vector<std::string> args = {"curl", "-sS", "-L", "--proto", protos(url), "--proto-redir", protos(url), "--max-redirs", "5",
                                     "--connect-timeout", connect, "--max-time", t, "--max-filesize", "20000000", "-A", ua, "-H",
                                     "Accept: application/vnd.github+json", "-H", "X-GitHub-Api-Version: 2022-11-28"};
    if (!etag.empty()) args.insert(args.end(), {"-H", "If-None-Match: " + etag});
    args.insert(args.end(), {"-D", head.string(), "-o", body.string(), url});
    code = run(curl, args, timeoutSeconds + 5, nullptr, nullptr);
    // 6 host not resolved, 7 no connection, 28 timeout, 35 TLS handshake, 52 / 56 connection lost
    r.offline = code == 6 || code == 7 || code == 28 || code == 35 || code == 52 || code == 56 || code < 0;
    if (code != 0) r.error = r.offline ? "no connection" : "curl exited with " + std::to_string(code);
  } else {
    std::vector<std::string> args = {"wget", "-S", "--https-only", "--tries=1", "--max-redirect=5", "--timeout=" + t, "-U", ua,
                                     "--header=Accept: application/vnd.github+json", "--header=X-GitHub-Api-Version: 2022-11-28",
                                     "--content-on-error"};
    if (detail::loopbackTest(url)) args.erase(args.begin() + 2);
    if (!etag.empty()) args.push_back("--header=If-None-Match: " + etag);
    args.insert(args.end(), {"-O", body.string(), "-o", head.string(), url});
    code = run(wget, args, timeoutSeconds + 5, nullptr, nullptr);
    r.offline = code == 4 || code < 0;  // 4: network failure; 8: the server answered with an error status
    if (code != 0 && code != 8) r.error = r.offline ? "no connection" : "wget exited with " + std::to_string(code);
  }
  const std::string headers = readAll(head);
  r.status = lastStatus(headers);
  if (r.status) {
    r.offline = false;
    r.error.clear();
    r.body = readAll(body);
    r.etag = lastEtag(headers);
  }
  std::error_code ec;
  fs::remove(body, ec);
  fs::remove(head, ec);
  return r;
}

update::HttpResult get(const std::string& url, int timeoutSeconds) { return getConditional(url, std::string(), timeoutSeconds); }

bool downloadContinue(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
                      const std::atomic<bool>* cancel, int stallSeconds, std::string* err) {
  if (!detail::httpsOnly(url)) {
    if (err) *err = "not an https:// address";
    return false;
  }
  const std::string curl = findTool("curl"), wget = curl.empty() ? findTool("wget") : std::string();
  if (curl.empty() && wget.empty()) {
    if (err) *err = "neither curl nor wget is installed";
    return false;
  }
  const std::string ua = update::userAgent(), stall = std::to_string(std::max(1, stallSeconds));
  auto tick = [&]() {
    std::error_code ec;
    const auto n = fs::file_size(fs::u8path(path), ec);
    if (!ec && progress) progress(static_cast<int64_t>(n), -1);
  };
  for (int pass = 0; pass < 2; ++pass) {
    std::error_code ec;
    const bool partial = fs::exists(fs::u8path(path), ec) && fs::file_size(fs::u8path(path), ec) > 0;
    int code = -1;
    std::string status;
    if (!curl.empty()) {
      const fs::path out = scratchBase().string() + ".code";
      std::vector<std::string> args = {"curl", "-sS", "-f", "-L", "--proto", protos(url), "--proto-redir", protos(url), "--max-redirs", "5",
                                       "--connect-timeout", "15", "--speed-limit", "1", "--speed-time", stall, "-A", ua, "-w", "%{http_code}"};
      if (partial) args.insert(args.end(), {"-C", "-"});
      args.insert(args.end(), {"-o", path, url});
      platform::Process p;
      p.redirectOutput(out.string(), std::string());
      if (p.start(curl, args)) {
        bool killed = false;
        while (!p.wait(0.1)) {
          tick();
          if (cancel && cancel->load()) {
            p.kill();
            p.wait(5);
            killed = true;
            break;
          }
        }
        if (!killed) code = p.status().crashed ? -1 : p.status().code;
      }
      status = readAll(out);
      fs::remove(out, ec);
    } else {
      std::vector<std::string> args = {"wget", "-q", "--https-only", "--tries=1", "--max-redirect=5", "--timeout=" + stall, "-U", ua};
      if (detail::loopbackTest(url)) args.erase(args.begin() + 2);
      if (partial) args.push_back("-c");
      args.insert(args.end(), {"-O", path, url});
      code = run(wget, args, 0, cancel, tick);
    }
    tick();
    if (cancel && cancel->load()) {
      if (err) *err = "cancelled";
      return false;
    }
    if (code == 0) return true;
    // 416: the part is already whole (the caller checks it); 33: the server cannot resume.
    if (status == "416") return true;
    if (code == 33 && pass == 0) {
      fs::remove(fs::u8path(path), ec);
      continue;
    }
    if (err) {
      if (code == 28) *err = "stalled";
      else if (code == 18) *err = "the connection broke off";
      else if (code == 6 || code == 7 || code == 35 || code == 52 || code == 56) *err = "no connection";
      else if (code == 22 || code == 8) *err = "the server refused the download (HTTP " + (status.empty() ? std::string("?") : status) + ")";
      else if (code == 23) *err = "could not write the file (disk full?)";
      else *err = "the download failed";
    }
    return false;
  }
  if (err) *err = "the download failed";
  return false;
}

bool download(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
              const std::atomic<bool>* cancel, std::string* err) {
  if (!detail::httpsOnly(url)) {
    if (err) *err = "not an https:// address";
    return false;
  }
  const std::string curl = findTool("curl"), wget = curl.empty() ? findTool("wget") : std::string();
  if (curl.empty() && wget.empty()) {
    if (err) *err = "neither curl nor wget is installed";
    return false;
  }
  const std::string ua = update::userAgent();
  auto tick = [&]() {
    std::error_code ec;
    const auto n = fs::file_size(fs::u8path(path), ec);
    if (!ec && progress) progress(static_cast<int64_t>(n), -1);
  };
  int code;
  if (!curl.empty())
    code = run(curl,
               {"curl", "-sS", "-f", "-L", "--proto", protos(url), "--proto-redir", protos(url), "--max-redirs", "5", "--connect-timeout", "15",
                "--speed-limit", "1", "--speed-time", "60", "-A", ua, "-o", path, url},
               0, cancel, tick);
  else
    code = run(wget, {"wget", "-q", "--https-only", "--tries=1", "--max-redirect=5", "--timeout=60", "-U", ua, "-O", path, url}, 0, cancel,
               tick);
  tick();
  if (code != 0) {
    if (err) *err = cancel && cancel->load() ? "cancelled" : code == 22 || code == 8 ? "the server refused the download" : "the download failed";
    return false;
  }
  return true;
}

std::string downloadsFolder() {
  const std::string home = platform::homeDir();
  std::string dir;
  if (const char* x = std::getenv("XDG_DOWNLOAD_DIR"); x && *x) dir = x;
  if (dir.empty()) {  // xdg-user-dirs: XDG_DOWNLOAD_DIR="$HOME/Downloads"
    const char* cfg = std::getenv("XDG_CONFIG_HOME");
    const fs::path f = (cfg && *cfg ? fs::path(cfg) : fs::path(home) / ".config") / "user-dirs.dirs";
    std::stringstream ss(readAll(f));
    for (std::string line; std::getline(ss, line);) {
      if (line.rfind("XDG_DOWNLOAD_DIR=", 0) != 0) continue;
      std::string v = line.substr(17);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
      dir = v;
    }
  }
  if (dir.empty()) dir = (fs::path(home) / "Downloads").string();
  std::error_code ec;
  fs::create_directories(dir, ec);
  return fs::is_directory(dir, ec) ? dir : home;
}

bool reveal(const std::string& path) {
  const std::string opener = findTool("xdg-open");
  if (opener.empty()) return false;
  const std::string folder = fs::path(path).parent_path().string();
  std::thread([opener, folder] { gl::threads::setName("gl-http-open");  // waited for, so no zombie is left behind
    platform::Process p;
    if (p.start(opener, {"xdg-open", folder})) p.wait(60);
  }).detach();
  return true;
}

#endif

}  // namespace gl::http
