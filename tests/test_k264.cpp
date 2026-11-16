// Gridline tests - K-264: the MCP server's listen address (core/McpListen.h) and the endpoint's
// safety checks beyond loopback (core/McpTransport.h).
//  * loopback stays the default and binds 127.0.0.1; the other modes bind as chosen;
//  * peers: loopback only by default; private ranges for WSL / VMs and a specific address; any for all;
//  * Host: loopback names always; this computer's own IP literals beyond loopback; never a host name;
//  * the token is required at every address; five wrong tokens from one address block it a minute;
//  * the client host for the connect command follows the choice.
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/McpListen.h"
#include "core/ProjectScript.h"
#include "core/McpTransport.h"

using namespace gl;
using namespace gl::mcp::listen;

int main() {
  uint32_t ip = 0;
  expect(parseIpv4("192.168.1.20", ip) && ip == 0xC0A80114u && ipv4Text(ip) == "192.168.1.20", "IPv4 text and back");
  expect(!parseIpv4("192.168.1", ip) && !parseIpv4("1.2.3.4.5", ip) && !parseIpv4("256.1.1.1", ip) && !parseIpv4("evil.example", ip) &&
             !parseIpv4("1.2.3.4 ", ip),
         "only exact a.b.c.d literals");
  expect(modeFromId("") == Mode::Loopback && modeFromId("junk") == Mode::Loopback && modeFromId(modeId(Mode::All)) == Mode::All,
         "unknown settings mean this computer only");

  Policy def;
  expect(def.mode == Mode::Loopback && bindAddress(def) == 0x7F000001u, "default: 127.0.0.1");
  Policy vms{Mode::LoopbackAndVms, 0}, one{Mode::Address, 0xC0A80114u}, all{Mode::All, 0};
  expect(bindAddress(vms) == 0 && bindAddress(all) == 0 && bindAddress(one) == 0xC0A80114u, "the other modes bind as chosen");

  const uint32_t lo = 0x7F000001u, wsl = 0xAC1A0001u /* 172.26.0.1 */, lan = 0xC0A80005u, pub = 0x08080808u, cgnat = 0x64400001u;
  expect(peerAllowed(def, lo) && !peerAllowed(def, wsl) && !peerAllowed(def, lan), "default: this computer only");
  expect(peerAllowed(vms, wsl) && peerAllowed(vms, lan) && peerAllowed(vms, cgnat) && !peerAllowed(vms, pub), "WSL and VMs: private ranges");
  expect(peerAllowed(one, lan) && !peerAllowed(one, pub), "a specific address: private ranges too");
  expect(peerAllowed(all, pub), "all networks: any address");
  expect(!isPrivate(0xAC200001u) && isPrivate(0xAC1F0001u), "172.16/12 ends at 172.31");

  const std::vector<uint32_t> own = {lo, lan};
  expect(hostAllowed(def, "127.0.0.1:9000", own) && hostAllowed(def, "localhost", own) && hostAllowed(def, "[::1]:9000", own), "loopback hosts");
  expect(!hostAllowed(def, "192.168.0.5:9000", own), "default: this computer's LAN address is not a valid host");
  expect(hostAllowed(vms, "192.168.0.5:9000", own), "beyond loopback: this computer's own address");
  expect(!hostAllowed(vms, "192.168.0.6:9000", own), "another computer's address is refused");
  expect(!hostAllowed(all, "attacker.example:9000", own) && !hostAllowed(all, "gridline.local", own), "a host name is never accepted (DNS rebinding)");

  expect(clientHost(def, own) == "127.0.0.1" && clientHost(one, own) == "192.168.1.20" && clientHost(vms, own) == "192.168.0.5",
         "the connect command names the address to use");
  expect(clientHost(all, {lo}) == "127.0.0.1", "no LAN address: loopback");

  AuthThrottle th;
  for (int i = 0; i < AuthThrottle::kMaxFails - 1; ++i) th.failed(lan, 10);
  expect(!th.blocked(lan, 10), "four wrong tokens: still allowed");
  th.failed(lan, 11);
  expect(th.blocked(lan, 12) && !th.blocked(wsl, 12), "five wrong tokens block that address only");
  expect(!th.blocked(lan, 11 + AuthThrottle::kBlock + 1), "the block ends after a minute");
  for (int i = 0; i < 3; ++i) th.failed(wsl, 0);
  th.succeeded(wsl);
  for (int i = 0; i < 3; ++i) th.failed(wsl, 1);
  expect(!th.blocked(wsl, 1), "a right token resets the count");

  // the endpoint: a request beyond loopback without the token is refused; Host checks follow the policy
  Document d;
  script::SimpleProjectHost host(d);
  mcp::Environment env(d, host);
  mcp::HttpEndpoint ep(env, "secret-token");
  ep.setListen(vms, own);
  mcp::HttpServer::Request r;
  r.method = "POST", r.path = "/mcp", r.peer = lan;
  r.headers["host"] = "192.168.0.5:9000";
  r.body = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
  expect(ep.handle(r).status == 401, "beyond loopback the token is still required");
  r.headers["host"] = "evil.example:9000";
  r.headers["authorization"] = "Bearer secret-token";
  expect(ep.handle(r).status == 403, "a host name is refused even with the token");
  r.headers["host"] = "192.168.0.5:9000";
  expect(ep.handle(r).status != 401 && ep.handle(r).status != 403, "this computer's address with the token is served");
  r.headers["authorization"] = "Bearer wrong";
  int last = 0;
  for (int i = 0; i < 6; ++i) last = ep.handle(r).status;
  expect(last == 429, "repeated wrong tokens: 429 for that address");
  mcp::HttpEndpoint ep2(env, "secret-token");  // the default endpoint keeps the loopback rules
  r.headers["authorization"] = "Bearer secret-token";
  expect(ep2.handle(r).status == 403, "default endpoint: a LAN host header is refused");
  return finish();
}
