#include "Net.h"
#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>

namespace gl::net {

#if defined(_WIN32)

bool startup() {
  static const bool ok = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  return ok;
}

Sock tcpSocket() {
  startup();
  SOCKET s = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  return s == INVALID_SOCKET ? kNoSock : static_cast<Sock>(s);
}
void closeSock(Sock s) {
  if (s != kNoSock) ::closesocket(static_cast<SOCKET>(s));
}
void setNonBlocking(Sock s) {
  u_long one = 1;
  ::ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &one);
}
void setNoInherit(Sock s) { SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0); }
void setNoSigPipe(Sock) {}
void setTimeouts(Sock s, int seconds) {
  const DWORD ms = static_cast<DWORD>(seconds) * 1000u;
  ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
  ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}
long sendSome(Sock s, const char* p, size_t n) {
  return ::send(static_cast<SOCKET>(s), p, static_cast<int>(n > INT_MAX ? INT_MAX : n), 0);
}
long recvSome(Sock s, char* p, size_t n) {
  return ::recv(static_cast<SOCKET>(s), p, static_cast<int>(n > INT_MAX ? INT_MAX : n), 0);
}
bool wouldBlock() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS;
}
int pollSocks(PollFd* fds, size_t n, int timeoutMs) { return ::WSAPoll(fds, static_cast<ULONG>(n), timeoutMs); }
std::string lastError() {
  const int e = WSAGetLastError();
  char buf[256] = {};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(e), 0, buf, sizeof buf, nullptr);
  std::string s = buf;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
  return s.empty() ? "socket error " + std::to_string(e) : s;
}

#else

bool startup() { return true; }

Sock tcpSocket() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return kNoSock;
  setNoInherit(fd);
  return fd;
}
void closeSock(Sock s) {
  if (s != kNoSock) ::close(static_cast<int>(s));
}
void setNonBlocking(Sock s) {
  const int fd = static_cast<int>(s);
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
void setNoInherit(Sock s) { ::fcntl(static_cast<int>(s), F_SETFD, FD_CLOEXEC); }
void setNoSigPipe(Sock s) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)s;
#endif
}
void setTimeouts(Sock s, int seconds) {
  timeval tv{};
  tv.tv_sec = seconds;
  ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
long sendSome(Sock s, const char* p, size_t n) {
#ifdef MSG_NOSIGNAL
  constexpr int flags = MSG_NOSIGNAL;
#else
  constexpr int flags = 0;
#endif
  for (;;) {
    const auto r = ::send(static_cast<int>(s), p, n, flags);
    if (r < 0 && errno == EINTR) continue;
    return static_cast<long>(r);
  }
}
long recvSome(Sock s, char* p, size_t n) {
  for (;;) {
    const auto r = ::recv(static_cast<int>(s), p, n, 0);
    if (r < 0 && errno == EINTR) continue;
    return static_cast<long>(r);
  }
}
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
int pollSocks(PollFd* fds, size_t n, int timeoutMs) { return ::poll(fds, static_cast<nfds_t>(n), timeoutMs); }
std::string lastError() { return std::strerror(errno); }

#endif

}  // namespace gl::net
