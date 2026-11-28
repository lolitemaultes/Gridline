// Gridline — the program the network probe plugin starts (tests/clap/NetProbe.cpp): sends one TCP
// message to 127.0.0.1. Usage: <port> <message>. Exit code 0 when it arrived, 1 otherwise.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(std::atoi(argv[1])));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const size_t n = std::strlen(argv[2]);
  const bool ok = ::connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 && ::send(fd, argv[2], n, 0) == static_cast<ssize_t>(n);
  ::close(fd);
  return ok ? 0 : 1;
}
