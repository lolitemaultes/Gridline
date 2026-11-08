// Gridline — one Gridline per user on Linux, the BSDs and Windows (K-121). See SingleInstance.h.
#include "SingleInstance.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include "Platform.h"

#if defined(_WIN32)
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace fs = std::filesystem;

namespace gl::single {

// ------------------------------------------------------------------ the conversation ----

namespace {

constexpr const char* kHello = "GRIDLINE 1 ";
constexpr const char* kOk = "OK\n";
constexpr int kServeMs = 3000;  // how long the server waits for one client's request

// A decimal number followed by '\n' at `at`; NeedMore when the line is not complete yet.
Parse readNumberLine(const std::string& in, size_t& at, size_t& value, size_t maxValue) {
  value = 0;
  size_t i = at, digits = 0;
  while (i < in.size() && in[i] >= '0' && in[i] <= '9') {
    if (++digits > 9) return Parse::Bad;
    value = value * 10 + static_cast<size_t>(in[i] - '0');
    ++i;
  }
  if (i == in.size()) return digits > 9 ? Parse::Bad : Parse::NeedMore;
  if (in[i] != '\n' || digits == 0 || value > maxValue) return Parse::Bad;
  at = i + 1;
  return Parse::Done;
}

using Clock = std::chrono::steady_clock;
int msLeft(Clock::time_point until) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
  return ms <= 0 ? 0 : static_cast<int>(ms);
}

}  // namespace

std::string encodeRequest(const std::vector<std::string>& files) {
  std::string out = "OPEN " + std::to_string(files.size()) + "\n";
  for (const std::string& f : files) out += std::to_string(f.size()) + "\n" + f;
  return out;
}

Parse decodeRequest(const std::string& in, std::vector<std::string>& files) {
  files.clear();
  const std::string head = "OPEN ";
  if (in.size() < head.size()) return in.compare(0, in.size(), head, 0, in.size()) == 0 ? Parse::NeedMore : Parse::Bad;
  if (in.compare(0, head.size(), head) != 0) return Parse::Bad;
  size_t at = head.size(), count = 0;
  if (const Parse p = readNumberLine(in, at, count, kMaxFiles); p != Parse::Done) return p;
  for (size_t k = 0; k < count; ++k) {
    size_t len = 0;
    if (const Parse p = readNumberLine(in, at, len, kMaxPathBytes); p != Parse::Done) return p;
    if (in.size() - at < len) return Parse::NeedMore;
    std::string f = in.substr(at, len);
    if (f.empty() || f.find('\0') != std::string::npos) return Parse::Bad;
    files.push_back(std::move(f));
    at += len;
  }
  return at == in.size() ? Parse::Done : Parse::Bad;
}

// ------------------------------------------------------------------ the endpoint ----

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}

// The user a process token belongs to, as a SID string ("" when unknown).
std::wstring sidOfToken(HANDLE token) {
  DWORD len = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &len);
  if (len == 0) return {};
  std::vector<unsigned char> buf(len);
  if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) return {};
  LPWSTR s = nullptr;
  std::wstring out;
  if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s)) {
    out = s;
    LocalFree(s);
  }
  return out;
}

std::wstring sidOfProcess(HANDLE process) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
  std::wstring s = sidOfToken(token);
  CloseHandle(token);
  return s;
}

std::wstring mySid() {
  static const std::wstring s = sidOfProcess(GetCurrentProcess());
  return s;
}

bool processIsMine(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return false;
  const std::wstring s = sidOfProcess(p);
  CloseHandle(p);
  return !s.empty() && s == mySid();
}

}  // namespace

std::string defaultEndpoint(const std::string&) {
  const std::wstring sid = mySid();
  if (sid.empty()) return {};
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  return "\\\\.\\pipe\\Gridline-" + narrow(sid) + "-" + std::to_string(session);
}

std::string testEndpoint(const std::string& value) { return value.empty() ? std::string() : "\\\\.\\pipe\\" + value; }

#else

namespace {

#if !defined(__APPLE__)  // (macOS leaves single instance to the system)
// A folder only this user can enter: owned by the user, no access for group or others.
bool privateDir(const std::string& d) {
  struct stat st{};
  return !d.empty() && ::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 077) == 0;
}

// The display this Gridline shows on (two sessions of one user each get their own Gridline).
std::string displayTag() {
  std::string d;
  if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) d = w;
  else if (const char* x = std::getenv("DISPLAY"); x && *x) d = x;
  if (const size_t slash = d.find_last_of('/'); slash != std::string::npos) d = d.substr(slash + 1);
  std::string out;
  for (char c : d) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
  return out.empty() ? std::string("default") : out.substr(0, 40);
}
#endif

bool fitsSocketPath(const std::string& p) { return !p.empty() && p.size() < sizeof(sockaddr_un{}.sun_path); }

}  // namespace

std::string defaultEndpoint(const std::string& configDir) {
#if defined(__APPLE__)
  (void)configDir;
  return {};  // macOS: the system opens files in the running Gridline itself
#else
  const std::string name = "gridline-" + displayTag() + ".sock";
  std::vector<std::string> dirs;
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  const char* flatpak = std::getenv("FLATPAK_ID");
  // Flatpak: each instance has its own runtime folder; app/<id> inside it is shared by all of them.
  if (rt && *rt && flatpak && *flatpak) dirs.push_back((fs::path(rt) / "app" / flatpak).string());
  if (rt && *rt) dirs.push_back(rt);
  if (!configDir.empty()) {
    std::error_code ec;
    fs::create_directories(fs::u8path(configDir), ec);
    dirs.push_back(configDir);  // (the socket itself is 0600 and checked for its owner)
  }
  for (size_t i = 0; i < dirs.size(); ++i) {
    const bool fallback = !configDir.empty() && i + 1 == dirs.size();
    if (!fallback && !privateDir(dirs[i])) continue;
    const std::string p = (fs::path(dirs[i]) / name).string();
    if (fitsSocketPath(p)) return p;
  }
  return {};
#endif
}

std::string testEndpoint(const std::string& value) { return value; }

#endif

// ------------------------------------------------------------------ POSIX ----

#if !defined(_WIN32)

namespace {

void closeFd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

void setFlags(int fd) {
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
#if defined(SO_NOSIGPIPE)
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

bool socketAddress(const std::string& path, sockaddr_un& a) {
  std::memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  if (!fitsSocketPath(path)) return false;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  return true;
}

// The user id of the process at the other end, -1 when unknown.
long peerUid(int fd) {
#if defined(SO_PEERCRED) && defined(__linux__)
  struct ucred cr{};
  socklen_t len = sizeof cr;
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0) return static_cast<long>(cr.uid);
  return -1;
#else
  uid_t uid = 0;
  gid_t gid = 0;
  if (::getpeereid(fd, &uid, &gid) == 0) return static_cast<long>(uid);
  return -1;
#endif
}

enum class Conn { Ok, Nobody, Error };
// A connection to `path` within `ms`: Nobody when no socket is there or nothing listens on it.
Conn connectTo(const std::string& path, int ms, int& out) {
  out = -1;
  sockaddr_un a{};
  if (!socketAddress(path, a)) return Conn::Error;
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return Conn::Error;
  setFlags(fd);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    const int e = errno;
    if (e == ENOENT || e == ECONNREFUSED || e == ENOTSOCK) {
      closeFd(fd);
      return Conn::Nobody;
    }
    if (e != EINPROGRESS && e != EAGAIN) {
      closeFd(fd);
      return Conn::Error;
    }
    pollfd p{fd, POLLOUT, 0};
    int soErr = 0;
    socklen_t len = sizeof soErr;
    if (::poll(&p, 1, ms) != 1 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) != 0 || soErr != 0) {
      closeFd(fd);
      return soErr == ECONNREFUSED ? Conn::Nobody : Conn::Error;
    }
  }
  out = fd;
  return Conn::Ok;
}

bool sendAll(int fd, const std::string& data, Clock::time_point until) {
  size_t done = 0;
  while (done < data.size()) {
#if defined(MSG_NOSIGNAL)
    const ssize_t n = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd, data.data() + done, data.size() - done, 0);
#endif
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, msLeft(until)) != 1) return false;
      continue;
    }
    return false;
  }
  return true;
}

// Reads what has arrived into `buf`; false at the end of the stream, an error or the deadline.
bool recvSome(int fd, std::string& buf, Clock::time_point until) {
  for (;;) {
    char tmp[4096];
    const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n > 0) {
      buf.append(tmp, static_cast<size_t>(n));
      return true;
    }
    if (n == 0) return false;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
    pollfd p{fd, POLLIN, 0};
    const int ms = msLeft(until);
    if (ms <= 0 || ::poll(&p, 1, ms) != 1) return false;
  }
}

bool recvLine(int fd, std::string& line, Clock::time_point until) {
  std::string buf;
  while (buf.find('\n') == std::string::npos) {
    if (buf.size() > 256 || !recvSome(fd, buf, until)) return false;
  }
  line = buf.substr(0, buf.find('\n') + 1);
  return true;
}

}  // namespace

struct Server::Impl {
  int listenFd = -1;
  int wakeR = -1, wakeW = -1;
  ino_t inode = 0;
  dev_t device = 0;
};

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

Server::Claim Server::claim(const std::string& endpoint, std::string* err) {
  endpoint_ = endpoint;
  if (endpoint.empty() || !fitsSocketPath(endpoint)) {
    if (err) *err = "no place for the socket";
    return Claim::Failed;
  }
  // One launch at a time looks and takes it (two double clicks at once).
  int lock = ::open((endpoint + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (lock >= 0) ::flock(lock, LOCK_EX);
  auto done = [&lock](Claim c) {
    if (lock >= 0) {
      ::flock(lock, LOCK_UN);
      ::close(lock);
    }
    return c;
  };
  int probe = -1;
  switch (connectTo(endpoint, 1000, probe)) {
    case Conn::Ok:
      closeFd(probe);
      return done(Claim::Busy);
    case Conn::Error:
      if (err) *err = std::string("cannot reach ") + endpoint + ": " + std::strerror(errno);
      return done(Claim::Failed);
    case Conn::Nobody: break;
  }
  struct stat st{};
  if (::lstat(endpoint.c_str(), &st) == 0) {  // left behind by a Gridline that crashed
    if (!S_ISSOCK(st.st_mode) || st.st_uid != ::getuid()) {
      if (err) *err = endpoint + " is not this user's socket";
      return done(Claim::Failed);
    }
    ::unlink(endpoint.c_str());
  }
  sockaddr_un a{};
  socketAddress(endpoint, a);
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = std::strerror(errno);
    return done(Claim::Failed);
  }
  setFlags(fd);
  const mode_t old = ::umask(077);
  const int b = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  const int bindErr = errno;
  ::umask(old);
  if (b != 0 || ::chmod(endpoint.c_str(), 0600) != 0 || ::listen(fd, 16) != 0) {
    if (err) *err = std::strerror(b != 0 ? bindErr : errno);
    closeFd(fd);
    return done(Claim::Failed);
  }
  if (::lstat(endpoint.c_str(), &st) == 0) {
    impl_->inode = st.st_ino;
    impl_->device = st.st_dev;
  }
  int pipeFds[2] = {-1, -1};
  if (::pipe(pipeFds) != 0) {
    ::unlink(endpoint.c_str());
    closeFd(fd);
    if (err) *err = std::strerror(errno);
    return done(Claim::Failed);
  }
  ::fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
  impl_->wakeR = pipeFds[0];
  impl_->wakeW = pipeFds[1];
  impl_->listenFd = fd;
  claimed_ = true;
  return done(Claim::Taken);
}

void Server::serve() {
  for (;;) {
    pollfd p[2] = {{impl_->listenFd, POLLIN, 0}, {impl_->wakeR, POLLIN, 0}};
    if (::poll(p, 2, -1) < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (p[1].revents) return;
    if (!(p[0].revents & POLLIN)) continue;
    int c = ::accept(impl_->listenFd, nullptr, nullptr);
    if (c < 0) continue;
    setFlags(c);
    const Clock::time_point until = Clock::now() + std::chrono::milliseconds(kServeMs);
    if (peerUid(c) == static_cast<long>(::getuid()) &&
        sendAll(c, kHello + std::to_string(platform::processId()) + "\n", until)) {
      std::string in;
      std::vector<std::string> files;
      Parse st = Parse::NeedMore;
      while (st == Parse::NeedMore && in.size() <= kMaxFiles * (kMaxPathBytes + 8) && recvSome(c, in, until)) st = decodeRequest(in, files);
      if (st == Parse::Done) {
        push(Request{files});
        sendAll(c, kOk, until);
      }
    }
    closeFd(c);
  }
}

void Server::stop() {
  if (impl_->wakeW >= 0) {
    const char b = 1;
    (void)!::write(impl_->wakeW, &b, 1);
  }
  if (thread_.joinable()) thread_.join();
  if (impl_->listenFd >= 0) {
    struct stat st{};
    if (::lstat(endpoint_.c_str(), &st) == 0 && st.st_ino == impl_->inode && st.st_dev == impl_->device) ::unlink(endpoint_.c_str());
  }
  closeFd(impl_->listenFd);
  closeFd(impl_->wakeR);
  closeFd(impl_->wakeW);
  claimed_ = false;
}

Handover handOver(const std::string& endpoint, const std::vector<std::string>& files, std::string* err, int timeoutMs) {
  const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeoutMs);
  int fd = -1;
  switch (connectTo(endpoint, timeoutMs, fd)) {
    case Conn::Nobody: return Handover::NoServer;
    case Conn::Error:
      if (err) *err = std::string("cannot connect: ") + std::strerror(errno);
      return Handover::Failed;
    case Conn::Ok: break;
  }
  auto fail = [&](const char* why) {
    if (err) *err = why;
    closeFd(fd);
    return Handover::Failed;
  };
  if (peerUid(fd) != static_cast<long>(::getuid())) return fail("the endpoint belongs to another user");
  std::string line;
  if (!recvLine(fd, line, until) || line.rfind(kHello, 0) != 0) return fail("no answer from the running Gridline");
  if (!sendAll(fd, encodeRequest(files), until)) return fail("could not send the files");
  if (!recvLine(fd, line, until) || line != kOk) return fail("the running Gridline did not take the files");
  closeFd(fd);
  return Handover::Sent;
}

#else  // ------------------------------------------------------------------ Windows ----

namespace {

// Overlapped I/O on `h` that waits at most `ms` (or until `stop` is set); false on failure.
bool finishIo(HANDLE h, OVERLAPPED& ov, BOOL started, HANDLE stop, DWORD ms, DWORD& n) {
  n = 0;
  if (!started && GetLastError() != ERROR_IO_PENDING) return false;
  HANDLE w[2] = {ov.hEvent, stop};
  const DWORD r = WaitForMultipleObjects(stop ? 2 : 1, w, FALSE, ms);
  if (r != WAIT_OBJECT_0) {
    CancelIo(h);
    GetOverlappedResult(h, &ov, &n, TRUE);
    return false;
  }
  return GetOverlappedResult(h, &ov, &n, FALSE) != 0;
}

DWORD msUntil(Clock::time_point until) { return static_cast<DWORD>(msLeft(until)); }

bool writeAllWin(HANDLE h, HANDLE ev, HANDLE stop, const std::string& data, Clock::time_point until) {
  size_t done = 0;
  while (done < data.size()) {
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ResetEvent(ev);
    DWORD n = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 64 * 1024));
    const BOOL ok = WriteFile(h, data.data() + done, chunk, nullptr, &ov);
    if (!finishIo(h, ov, ok, stop, msUntil(until), n) || n == 0) return false;
    done += n;
  }
  return true;
}

bool readSomeWin(HANDLE h, HANDLE ev, HANDLE stop, std::string& buf, Clock::time_point until) {
  char tmp[4096];
  OVERLAPPED ov{};
  ov.hEvent = ev;
  ResetEvent(ev);
  DWORD n = 0;
  const BOOL ok = ReadFile(h, tmp, sizeof tmp, nullptr, &ov);
  if (!finishIo(h, ov, ok, stop, msUntil(until), n) || n == 0) return false;
  buf.append(tmp, n);
  return true;
}

bool readLineWin(HANDLE h, HANDLE ev, std::string& line, Clock::time_point until) {
  std::string buf;
  while (buf.find('\n') == std::string::npos)
    if (buf.size() > 256 || !readSomeWin(h, ev, nullptr, buf, until)) return false;
  line = buf.substr(0, buf.find('\n') + 1);
  return true;
}

}  // namespace

struct Server::Impl {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HANDLE stopEvent = nullptr;
  HANDLE ioEvent = nullptr;
};

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { stop(); }

Server::Claim Server::claim(const std::string& endpoint, std::string* err) {
  endpoint_ = endpoint;
  const std::wstring sid = mySid();
  if (endpoint.empty() || sid.empty()) {
    if (err) *err = "no pipe name";
    return Claim::Failed;
  }
  // Only this user may open the pipe (and with FIRST_PIPE_INSTANCE nobody can add an instance of
  // a name that is already taken).
  PSECURITY_DESCRIPTOR sd = nullptr;
  const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
    if (err) *err = "cannot make the pipe's access list";
    return Claim::Failed;
  }
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = sd;
  sa.bInheritHandle = FALSE;
  HANDLE h = CreateNamedPipeW(widen(endpoint).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, &sa);
  const DWORD e = GetLastError();
  LocalFree(sd);
  if (h == INVALID_HANDLE_VALUE) {
    if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY) return Claim::Busy;
    if (err) *err = "cannot create the pipe (" + std::to_string(e) + ")";
    return Claim::Failed;
  }
  impl_->pipe = h;
  impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  impl_->ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  claimed_ = true;
  return Claim::Taken;
}

void Server::serve() {
  HANDLE h = impl_->pipe, stopEv = impl_->stopEvent, ev = impl_->ioEvent;
  for (;;) {
    if (WaitForSingleObject(stopEv, 0) == WAIT_OBJECT_0) return;
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ResetEvent(ev);
    BOOL connected = ConnectNamedPipe(h, &ov);
    if (!connected) {
      const DWORD e = GetLastError();
      if (e == ERROR_PIPE_CONNECTED) {
        connected = TRUE;
      } else if (e == ERROR_IO_PENDING) {
        HANDLE w[2] = {ev, stopEv};
        if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
          CancelIo(h);
          return;
        }
        DWORD n = 0;
        connected = GetOverlappedResult(h, &ov, &n, FALSE);
      }
    }
    if (connected) {
      const Clock::time_point until = Clock::now() + std::chrono::milliseconds(kServeMs);
      ULONG clientPid = 0;
      if (GetNamedPipeClientProcessId(h, &clientPid) && processIsMine(clientPid) &&
          writeAllWin(h, ev, stopEv, kHello + std::to_string(platform::processId()) + "\n", until)) {
        std::string in;
        std::vector<std::string> files;
        Parse st = Parse::NeedMore;
        while (st == Parse::NeedMore && in.size() <= kMaxFiles * (kMaxPathBytes + 8) && readSomeWin(h, ev, stopEv, in, until))
          st = decodeRequest(in, files);
        if (st == Parse::Done) {
          push(Request{files});
          if (writeAllWin(h, ev, stopEv, kOk, until)) {
            std::string rest;  // until the client has read the answer and closed its end
            readSomeWin(h, ev, stopEv, rest, Clock::now() + std::chrono::milliseconds(2000));
          }
        }
      }
    }
    DisconnectNamedPipe(h);
  }
}

void Server::stop() {
  if (impl_->stopEvent) SetEvent(impl_->stopEvent);
  if (thread_.joinable()) thread_.join();
  if (impl_->pipe != INVALID_HANDLE_VALUE) CloseHandle(impl_->pipe);
  if (impl_->stopEvent) CloseHandle(impl_->stopEvent);
  if (impl_->ioEvent) CloseHandle(impl_->ioEvent);
  impl_->pipe = INVALID_HANDLE_VALUE;
  impl_->stopEvent = impl_->ioEvent = nullptr;
  claimed_ = false;
}

Handover handOver(const std::string& endpoint, const std::vector<std::string>& files, std::string* err, int timeoutMs) {
  const Clock::time_point until = Clock::now() + std::chrono::milliseconds(timeoutMs);
  const std::wstring name = widen(endpoint);
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 3 && h == INVALID_HANDLE_VALUE; ++attempt) {
    h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h != INVALID_HANDLE_VALUE) break;
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) return Handover::NoServer;
    if (e != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), msUntil(until))) {
      if (err) *err = "cannot open the pipe (" + std::to_string(e) + ")";
      return Handover::Failed;
    }
  }
  if (h == INVALID_HANDLE_VALUE) return Handover::Failed;
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  auto finish = [&](Handover r, const char* why) {
    if (why && err) *err = why;
    CloseHandle(ev);
    CloseHandle(h);
    return r;
  };
  ULONG serverPid = 0;
  if (!ev || !GetNamedPipeServerProcessId(h, &serverPid) || !processIsMine(serverPid))
    return finish(Handover::Failed, "the pipe belongs to another user");
  std::string line;
  if (!readLineWin(h, ev, line, until) || line.rfind(kHello, 0) != 0) return finish(Handover::Failed, "no answer from the running Gridline");
  AllowSetForegroundWindow(serverPid);  // (the launch the user just made may bring the window forward)
  if (!writeAllWin(h, ev, nullptr, encodeRequest(files), until)) return finish(Handover::Failed, "could not send the files");
  if (!readLineWin(h, ev, line, until) || line != kOk) return finish(Handover::Failed, "the running Gridline did not take the files");
  return finish(Handover::Sent, nullptr);
}

#endif

// ------------------------------------------------------------------ both ----

void Server::start(std::function<void()> wake) {
  if (!claimed_ || thread_.joinable()) return;
  wake_ = std::move(wake);
  thread_ = std::thread([this] { serve(); });
}

void Server::push(Request r) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.push_back(std::move(r));
  }
  if (wake_) wake_();
}

std::vector<Request> Server::take() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Request> out;
  out.swap(queue_);
  return out;
}

}  // namespace gl::single
