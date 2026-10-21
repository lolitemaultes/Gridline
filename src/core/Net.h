// Gridline: loopback TCP sockets on POSIX and Windows (Winsock 2), for the MCP server and its
// clients (McpTransport.cpp) and the tests. Sockets are kept as std::intptr_t (-1: none), which
// holds a POSIX descriptor and a Winsock SOCKET (INVALID_SOCKET is -1 as intptr_t).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace gl::net {

using Sock = std::intptr_t;
constexpr Sock kNoSock = -1;

#if defined(_WIN32)
using PollFd = WSAPOLLFD;
using SockLen = int;
#else
using PollFd = pollfd;
using SockLen = socklen_t;
#endif

// Winsock needs WSAStartup before the first call; every function here does it once.
bool startup();
Sock tcpSocket();
void closeSock(Sock s);
void setNonBlocking(Sock s);
void setNoInherit(Sock s);   // not passed on to child processes
void setNoSigPipe(Sock s);   // a write to a closed peer fails instead of raising SIGPIPE
void setTimeouts(Sock s, int seconds);  // blocking send / recv give up after this long
// send / recv: bytes moved, 0 = peer closed (recv), < 0 = error (see wouldBlock()).
long sendSome(Sock s, const char* p, size_t n);
long recvSome(Sock s, char* p, size_t n);
bool wouldBlock();  // the last call failed only because a non-blocking socket was not ready
int pollSocks(PollFd* fds, size_t n, int timeoutMs);
inline PollFd pollEntry(Sock s, short events) {
  PollFd p{};
#if defined(_WIN32)
  p.fd = static_cast<SOCKET>(s);
#else
  p.fd = static_cast<int>(s);
#endif
  p.events = events;
  return p;
}
inline Sock sockOf(const PollFd& p) { return static_cast<Sock>(p.fd); }
std::string lastError();  // text of the last socket error

}  // namespace gl::net
