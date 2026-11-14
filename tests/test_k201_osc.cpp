// Gridline: OSC tests. The codec (round trips of every type, the spec's own byte layout, padding,
// bundles and time tags, patterns, SLIP, malformed packets, a few thousand random and mutated
// packets), the server on 127.0.0.1 (UDP, TCP with SLIP and with length prefixes, allowed hosts,
// a port in use), the address space on the test song (transport, mixer, channels, notes, plugin
// parameters by number and name, patterns, markers, address patterns), learning and running a
// link, feedback (rate limited, no echo), and a loopback run through real sockets end to end.
// "apphelper": the sender for the hidden app test (tests/k201_osc_app.cmake).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "core/Net.h"
#include "core/Osc.h"
#include "core/OscControl.h"
#include "core/OscServer.h"
#include "fixtures/TestSong.h"

using namespace gl;

namespace {

// ---------------------------------------------------------------- sockets ----

using RawSock = decltype(::socket(0, 0, 0));
RawSock raw(net::Sock s) { return static_cast<RawSock>(s); }

struct Udp {
  net::Sock s = net::kNoSock;
  int port = 0;
  Udp() {
    net::startup();
#if defined(_WIN32)
    s = static_cast<net::Sock>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#else
    s = ::socket(AF_INET, SOCK_DGRAM, 0);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001u);
    a.sin_port = 0;
    ::bind(raw(s), reinterpret_cast<sockaddr*>(&a), sizeof a);
    net::SockLen len = sizeof a;
    ::getsockname(raw(s), reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    net::setNonBlocking(s);
  }
  ~Udp() { net::closeSock(s); }
  void send(int to, const std::vector<uint8_t>& p) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001u);
    a.sin_port = htons(static_cast<uint16_t>(to));
    ::sendto(raw(s), reinterpret_cast<const char*>(p.data()), static_cast<int>(p.size()), 0,
             reinterpret_cast<sockaddr*>(&a), sizeof a);
  }
  // Messages received within `seconds`.
  std::vector<osc::Message> receive(double seconds) {
    std::vector<osc::Message> out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    std::vector<uint8_t> buf(70000);
    while (std::chrono::steady_clock::now() < end) {
      auto pe = net::pollEntry(s, POLLIN);
      if (net::pollSocks(&pe, 1, 20) <= 0) continue;
      const auto n = ::recv(raw(s), reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
      if (n <= 0) continue;
      std::vector<osc::Timed> t;
      if (osc::parse(buf.data(), static_cast<size_t>(n), t))
        for (auto& x : t) out.push_back(x.msg);
    }
    return out;
  }
};

net::Sock tcpConnect(int port) {
  net::Sock s = net::tcpSocket();
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(0x7F000001u);
  a.sin_port = htons(static_cast<uint16_t>(port));
  if (::connect(raw(s), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    net::closeSock(s);
    return net::kNoSock;
  }
  return s;
}

void sendAll(net::Sock s, const std::vector<uint8_t>& b) {
  size_t off = 0;
  while (off < b.size()) {
    const long n = net::sendSome(s, reinterpret_cast<const char*>(b.data()) + off, b.size() - off);
    if (n <= 0) return;
    off += static_cast<size_t>(n);
  }
}

// Polls the server until `count` messages arrived (or 3 s pass).
std::vector<OscIncoming> await(OscServer& srv, size_t count, double seconds = 3.0) {
  std::vector<OscIncoming> got;
  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (got.size() < count && std::chrono::steady_clock::now() < end) {
    srv.poll(got);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return got;
}

osc::Message msg(const std::string& a, std::vector<osc::Arg> args = {}) {
  osc::Message m;
  m.address = a;
  m.args = std::move(args);
  return m;
}
osc::Arg F(double v) { return osc::Arg::Float(static_cast<float>(v)); }
osc::Arg I(int v) { return osc::Arg::Int(v); }

// ---------------------------------------------------------------- test host ----

struct TestHost : OscHost {
  Document d;
  bool isPlaying = false, rec = false, song = false;
  double pos = 0;
  Tick loopA = 0, loopB = 0;
  ObjectId pat = kNoId, sel = kNoId;
  std::vector<std::pair<int, int>> notes;  // pitch, velocity
  std::vector<RemoteAction> actions;
  int stops = 0;
  // Learn: a target waiting for an address (the remote control dialog listening).
  bool learning = false;
  ParamId learnTarget;
  explicit TestHost(Song s) : d(std::move(s)) {
    if (!d.song().patterns.empty()) pat = d.song().patterns[0].id;
  }
  Document& doc() override { return d; }
  bool playing() const override { return isPlaying; }
  void play() override { isPlaying = true; }
  void pause() override { isPlaying = false; }
  void stop() override {
    isPlaying = false;
    pos = 0;
    ++stops;
  }
  bool recording() const override { return rec; }
  void setRecording(bool on) override { rec = on; }
  bool songMode() const override { return song; }
  void setSongMode(bool s) override { song = s; }
  double position() const override { return pos; }
  void setPosition(double t) override { pos = t; }
  void setLoop(Tick a, Tick b) override { loopA = a, loopB = b; }
  ObjectId currentPattern() const override { return pat; }
  void selectPattern(ObjectId p) override { pat = p; }
  ObjectId selectedChannel() const override { return sel; }
  void note(ObjectId, int pitch, int vel) override { notes.push_back({pitch, vel}); }
  void action(RemoteAction a) override { actions.push_back(a); }
  bool learn(const std::string& address) override {
    if (!learning) return false;
    learning = false;
    ControllerLink l;
    l.source = RemoteSource::Osc;
    l.oscAddress = address;
    l.owner = learnTarget.owner;
    l.index = learnTarget.index;
    d.editSong("link controller", [&](Song& s) { s.links.push_back(l); });
    return true;
  }
};

double norm(const Document& d, ParamId p) {
  auto desc = describeParam(d.song(), p);
  auto v = getParam(d.song(), p);
  return desc && v ? desc->toNormalized(*v) : -1;
}

bool nearly(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

const osc::Message* find(const std::vector<osc::Message>& v, const std::string& a) {
  for (const auto& m : v)
    if (m.address == a) return &m;
  return nullptr;
}

// ---------------------------------------------------------------- codec ----

void testCodec() {
  osc::Message m = msg("/all/types");
  m.args = {osc::Arg::Int(-7), osc::Arg::Float(0.25f), osc::Arg::Str("hello"), osc::Arg::Blob({1, 2, 3, 4, 5}),
            osc::Arg::Int64(-1234567890123LL), osc::Arg::Double(3.5), osc::Arg::Time(0x0123456789ABCDEFull), osc::Arg::Bool(true),
            osc::Arg::Bool(false), osc::Arg::Nil(), osc::Arg::Impulse()};
  osc::Arg S;
  S.type = 'S';
  S.s = "sym";
  osc::Arg c;
  c.type = 'c';
  c.i = 'A';
  osc::Arg r;
  r.type = 'r';
  r.i = 0x11223344;
  m.args.push_back(S);
  m.args.push_back(c);
  m.args.push_back(r);
  const auto bytes = osc::encode(m);
  expect(bytes.size() % 4 == 0, "codec: an encoded message is a multiple of 4 bytes");
  std::vector<osc::Timed> out;
  expect(osc::parse(bytes, out) && out.size() == 1 && out[0].msg == m, "codec: every type round trips");
  expect(out.size() == 1 && out[0].msg.typeTags() == ",ifsbhdtTFNIScr", "codec: type tags kept in order");
  expect(out.size() == 1 && out[0].time == osc::kImmediately, "codec: a bare message is immediate");

  // The specification's example: "/oscillator/4/frequency" ,f 440.0.
  const uint8_t spec[] = {'/', 'o', 's', 'c', 'i', 'l', 'l', 'a', 't', 'o', 'r', '/', '4', '/', 'f', 'r', 'e', 'q', 'u', 'e', 'n', 'c', 'y', 0,
                          ',', 'f', 0, 0, 0x43, 0xdc, 0x00, 0x00};
  const auto ex = osc::encode(msg("/oscillator/4/frequency", {F(440.0)}));
  expect(ex.size() == sizeof spec && std::memcmp(ex.data(), spec, sizeof spec) == 0, "codec: matches the specification's byte layout");
  // Padding: a string of length 4k gets 4 NULs, others 1..3.
  for (size_t len = 1; len <= 9; ++len) {
    const auto b = osc::encode(msg("/" + std::string(len - 1, 'a')));
    const size_t addr = (len + 4) & ~size_t(3);
    if (b.size() != addr + 4) {
      expect(false, "codec: address padding");
      break;
    }
  }
  expect(true, "codec: address padding for lengths 1 to 9");
  // Old senders without type tags.
  {
    const uint8_t old[] = {'/', 'a', 'b', 0};
    std::vector<osc::Timed> o;
    expect(osc::parse(old, sizeof old, o) && o.size() == 1 && o[0].msg.args.empty(), "codec: a message without type tags has no arguments");
  }
  // Bundles: nested, time tags, an immediate inner bundle keeps the outer time.
  {
    const uint64_t t1 = osc::timeFromUnix(2000000000.25), t2 = osc::timeFromUnix(2000000001.5);
    std::vector<std::vector<uint8_t>> inner = {osc::encode(msg("/b", {I(2)}))};
    std::vector<std::vector<uint8_t>> parts = {osc::encode(msg("/a", {I(1)})), osc::encodeBundlePackets(t2, inner),
                                               osc::encodeBundlePackets(osc::kImmediately, {osc::encode(msg("/c"))})};
    const auto b = osc::encodeBundlePackets(t1, parts);
    std::vector<osc::Timed> o;
    expect(osc::parse(b, o) && o.size() == 3, "bundle: three messages from a nested bundle");
    expect(o.size() == 3 && o[0].msg.address == "/a" && o[0].time == t1, "bundle: the outer time tag");
    expect(o.size() == 3 && o[1].msg.address == "/b" && o[1].time == t2, "bundle: an inner bundle's own time tag");
    expect(o.size() == 3 && o[2].msg.address == "/c" && o[2].time == t1, "bundle: an immediate inner bundle keeps the outer time");
    expect(std::fabs(osc::unixFromTime(t1) - 2000000000.25) < 1e-6, "time: unix seconds round trip");
    const auto eb = osc::encodeBundle(t1, {msg("/x", {F(1)}), msg("/y")});
    std::vector<osc::Timed> o2;
    expect(osc::parse(eb, o2) && o2.size() == 2 && o2[1].msg.address == "/y", "bundle: encodeBundle round trips");
  }
  // Malformed packets are refused and leave nothing behind.
  {
    std::vector<osc::Timed> o = {osc::Timed{}};
    const auto good = osc::encode(msg("/a/b", {I(1), osc::Arg::Str("xyz")}));
    auto bad = [&](std::vector<uint8_t> p, const char* name) {
      std::string err;
      const bool ok = osc::parse(p, o, &err);
      expect(!ok && o.size() == 1 && !err.empty(), name);
    };
    bad(std::vector<uint8_t>(good.begin(), good.end() - 4), "malformed: truncated arguments");
    bad(std::vector<uint8_t>(good.begin(), good.begin() + 6), "malformed: size not a multiple of 4");
    bad({'a', 'b', 'c', 0}, "malformed: no '/' or '#'");
    bad({'/', 'a', 0, 0, ',', 'Q', 0, 0}, "malformed: unknown type tag");
    bad({'/', 'a', 'b', 'c'}, "malformed: unterminated address");
    bad({'/', 'a', 0, 0, ',', 'b', 0, 0, 0x7f, 0xff, 0xff, 0xff}, "malformed: blob larger than the packet");
    std::vector<uint8_t> b = osc::encodeBundle(1, {msg("/x")});
    b[16] = 0x7f;  // element size far beyond the bundle
    bad(b, "malformed: bundle element larger than the bundle");
    std::vector<uint8_t> deep = osc::encode(msg("/d"));
    for (int k = 0; k < osc::kMaxDepth + 1; ++k) deep = osc::encodeBundlePackets(1, {deep});
    bad(deep, "malformed: bundles nested too deep");
    bad({'#', 'b', 'u', 'n', 'd', 'l', 'e', 0}, "malformed: bundle without a time tag");
    bad({'/', 'a', 0, 0, ',', '[', 'i', 0, 0, 0, 0, 1}, "malformed: unbalanced array brackets");
  }
  // Arrays (OSC 1.1) are read as plain arguments.
  {
    const uint8_t arr[] = {'/', 'a', 0, 0, ',', '[', 'i', 'i', ']', 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 2};
    std::vector<osc::Timed> o;
    expect(osc::parse(arr, sizeof arr, o) && o.size() == 1 && o[0].msg.args.size() == 2 && o[0].msg.args[1].i == 2, "codec: array brackets flatten");
  }
}

void testPatternsAndSlip() {
  expect(osc::match("/mixer/*/volume", "/mixer/12/volume"), "pattern: * in a segment");
  expect(!osc::match("/mixer/*", "/mixer/1/volume"), "pattern: * does not cross '/'");
  expect(osc::match("/ch/?/mute", "/ch/3/mute") && !osc::match("/ch/?/mute", "/ch/13/mute"), "pattern: ? is one character");
  expect(osc::match("/ch/[1-3]", "/ch/2") && !osc::match("/ch/[1-3]", "/ch/4"), "pattern: [range]");
  expect(osc::match("/ch/[!1-3]", "/ch/4") && !osc::match("/ch/[!1-3]", "/ch/1"), "pattern: [!negated]");
  expect(osc::match("/x/{volume,pan}", "/x/pan") && !osc::match("/x/{volume,pan}", "/x/mute"), "pattern: {alternatives}");
  expect(osc::match("/a*b*c", "/aXXbYYc") && !osc::match("/a*b*c", "/aXXbYY"), "pattern: several stars");
  expect(osc::match("/plain", "/plain") && !osc::match("/plain", "/plainer"), "pattern: plain addresses compare whole");
  {
    const std::string hostile = "/" + std::string(3000, '*') + "x";
    const auto t0 = std::chrono::steady_clock::now();
    const bool r = osc::match(hostile, "/" + std::string(3000, 'a'));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    expect(!r && secs < 2.0, "pattern: a hostile pattern finishes quickly");
  }
  expect(osc::validAddress("/1/fader1") && !osc::validAddress("1/fader") && !osc::validAddress("/a b") && !osc::validAddress("/a,b"),
         "address: validity");

  const std::vector<uint8_t> p = {1, osc::kSlipEnd, 2, osc::kSlipEsc, 3, osc::kSlipEnd};
  const auto e = osc::slipEncode(p);
  osc::SlipDecoder dec;
  std::vector<std::vector<uint8_t>> frames;
  for (size_t k = 0; k < e.size(); ++k) dec.feed(&e[k], 1, frames);  // a byte at a time
  expect(frames.size() == 1 && frames[0] == p, "slip: escapes round trip, byte by byte");
  osc::SlipDecoder small(16);
  frames.clear();
  const auto big = osc::slipEncode(std::vector<uint8_t>(64, 7));
  small.feed(big.data(), big.size(), frames);
  const auto ok = osc::slipEncode({9, 9, 9, 9});
  small.feed(ok.data(), ok.size(), frames);
  expect(small.overflows() == 1 && frames.size() == 1 && frames[0].size() == 4, "slip: an oversized frame is dropped, the next one kept");
}

void testFuzz() {
  std::mt19937 rng(201);
  std::uniform_int_distribution<int> byte(0, 255);
  size_t parsed = 0, maxOut = 0;
  const std::vector<std::vector<uint8_t>> seeds = {
      osc::encode(msg("/mixer/1/volume", {F(0.5)})), osc::encodeBundle(5, {msg("/a", {I(1), osc::Arg::Str("s")}), msg("/b", {osc::Arg::Blob({1, 2})})}),
      osc::encode(msg("/x", {osc::Arg::Int64(1), osc::Arg::Double(2), osc::Arg::Bool(true)}))};
  for (int k = 0; k < 6000; ++k) {
    std::vector<uint8_t> p;
    if (k % 2 == 0) {  // random bytes, sometimes with a plausible start
      p.resize(static_cast<size_t>(4 * (1 + byte(rng) % 64)));
      for (auto& b : p) b = static_cast<uint8_t>(byte(rng));
      if (k % 4 == 0) p[0] = '/';
      if (k % 8 == 2 && p.size() >= 16) std::memcpy(p.data(), "#bundle\0", 8);
    } else {  // a valid packet with bytes flipped, or cut short
      p = seeds[static_cast<size_t>(k) % seeds.size()];
      const int flips = 1 + byte(rng) % 4;
      for (int f = 0; f < flips; ++f) p[static_cast<size_t>(byte(rng)) % p.size()] = static_cast<uint8_t>(byte(rng));
      if (k % 3 == 0) p.resize(p.size() - static_cast<size_t>(byte(rng)) % p.size());
    }
    std::vector<osc::Timed> out;
    if (osc::parse(p.data(), p.size(), out)) ++parsed;
    maxOut = std::max(maxOut, out.size());
    for (const auto& t : out) maxOut = std::max(maxOut, t.msg.args.size() / 64);
  }
  expect(maxOut <= osc::kMaxMessages, "fuzz: 6000 random and mutated packets, no crash, output bounded");
  std::printf("# fuzz: %zu of 6000 packets parsed\n", parsed);
  // A packet of only type tags: its argument count stays bounded.
  std::vector<uint8_t> tags = {'/', 'a', 0, 0, ','};
  tags.insert(tags.end(), 60000, 'T');
  tags.resize((tags.size() + 4) & ~size_t(3), 0);
  std::vector<osc::Timed> out;
  expect(!osc::parse(tags, out) && out.empty(), "fuzz: a packet of 60000 arguments is refused");
}

// ---------------------------------------------------------------- server ----

void testServer() {
  OscServer srv;
  OscServerConfig cfg;
  cfg.port = 0;
  cfg.tcp = true;
  std::string err;
  expect(srv.start(cfg, &err) && srv.port() > 0, "server: starts on 127.0.0.1, any free port, UDP and TCP");
  Udp c;
  c.send(srv.port(), osc::encode(msg("/hello", {I(5)})));
  auto got = await(srv, 1);
  expect(got.size() == 1 && got[0].msg.address == "/hello" && got[0].ip == 0x7F000001u && got[0].port == c.port && !got[0].tcp,
         "server: a UDP message arrives with its sender");
  c.send(srv.port(), std::vector<uint8_t>{1, 2, 3, 4});
  c.send(srv.port(), osc::encodeBundle(1, {msg("/b1"), msg("/b2")}));
  got = await(srv, 2);
  const auto st = srv.status();
  expect(got.size() == 2 && st.malformed == 1 && !st.lastProblem.empty(), "server: a bundle's messages arrive, a bad packet is counted");
  expect(st.lastAddress == "/b2" && st.received == 3, "server: activity (last address, count)");

  net::Sock t = tcpConnect(srv.port());
  expect(t != net::kNoSock, "server: a TCP client connects");
  if (t != net::kNoSock) {
    auto a = osc::slipEncode(osc::encode(msg("/tcp/slip", {F(0.5)})));
    auto b = osc::slipEncode(osc::encode(msg("/tcp/slip2")));
    a.insert(a.end(), b.begin(), b.end());
    sendAll(t, a);
    got = await(srv, 2);
    expect(got.size() == 2 && got[0].msg.address == "/tcp/slip" && got[0].tcp && got[1].msg.address == "/tcp/slip2", "server: TCP with SLIP");
    net::closeSock(t);
  }
  t = tcpConnect(srv.port());
  if (t != net::kNoSock) {
    const auto p = osc::encode(msg("/tcp/len", {I(3)}));
    std::vector<uint8_t> f = {0, 0, 0, static_cast<uint8_t>(p.size())};
    f.insert(f.end(), p.begin(), p.end());
    sendAll(t, std::vector<uint8_t>(f.begin(), f.begin() + 6));  // split across two writes
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    sendAll(t, std::vector<uint8_t>(f.begin() + 6, f.end()));
    got = await(srv, 1);
    expect(got.size() == 1 && got[0].msg.address == "/tcp/len", "server: TCP with a length prefix, split across writes");
    net::closeSock(t);
  }

  OscServer second;
  OscServerConfig same;
  same.port = srv.port();
  std::string e2;
  expect(!second.start(same, &e2) && e2.find("in use") != std::string::npos, "server: a port in use is reported");

  // Feedback out: from the server's port to a client.
  expect(srv.sendTo(0x7F000001u, c.port, osc::encode(msg("/fb", {F(1)}))), "server: sends a packet");
  auto fb = c.receive(0.5);
  expect(find(fb, "/fb") != nullptr, "server: the packet arrives");
  srv.stop();
  expect(!srv.running(), "server: stops");

  // Allowed hosts.
  std::string bad;
  expect(OscServer::validHostList("192.168.1.20, 10.0.0.0/8;172.16.0.0/12", &bad), "hosts: a list of addresses and ranges");
  expect(!OscServer::validHostList("192.168.1.300", &bad) && bad == "192.168.1.300", "hosts: a bad address is named");
  expect(!OscServer::validHostList("example.org", &bad), "hosts: names are not accepted");
  uint32_t ip = 0;
  OscServer::parseIPv4("10.1.2.3", ip);
  expect(OscServer::hostAllowed("10.0.0.0/8", ip) && !OscServer::hostAllowed("192.168.0.0/16", ip), "hosts: ranges");
  expect(OscServer::hostAllowed("192.168.0.0/16", 0x7F000001u), "hosts: this computer is always allowed");
  expect(OscServer::hostAllowed("", ip), "hosts: an empty list allows any host");
  OscServerConfig badCfg;
  badCfg.port = 0;
  badCfg.network = true;
  badCfg.allowedHosts = "nonsense";
  std::string e3;
  expect(!srv.start(badCfg, &e3) && e3.find("nonsense") != std::string::npos, "server: refuses to start with a bad hosts list");
}

// ---------------------------------------------------------------- address space ----

void testControl() {
  TestHost h(makeTestSong());
  OscControl c(h);
  const Song& s = h.d.song();
  using R = OscControl::Result;

  expect(c.handle(msg("/transport/play")) == R::Handled && h.isPlaying, "transport: play");
  c.handle(msg("/transport/play", {I(0)}));
  expect(!h.isPlaying, "transport: play 0 pauses");
  c.handle(msg("/transport/playpause", {F(1)}));
  c.handle(msg("/transport/playpause", {F(0)}));  // a release does nothing
  expect(h.isPlaying, "transport: playpause acts on the press only");
  c.handle(msg("/transport/stop", {F(1)}));
  expect(!h.isPlaying && h.stops == 1, "transport: stop");
  c.handle(msg("/transport/record", {I(1)}));
  expect(h.rec, "transport: record 1 arms");
  c.handle(msg("/transport/record/toggle", {I(1)}));
  c.handle(msg("/transport/record/toggle", {I(0)}));
  expect(!h.rec, "transport: record/toggle flips on the press");
  c.handle(msg("/transport/tempo", {F(133)}));
  expect(nearly(s.tempo, 133, 1e-3), "transport: tempo in BPM");
  c.handle(msg("/transport/position", {F(3)}));
  expect(nearly(h.pos, 2.0 * s.barTicks()), "transport: position in bars (bar 3 starts after two bars)");
  c.handle(msg("/transport/position/beats", {F(6)}));
  expect(nearly(h.pos, 6.0 * s.ppq), "transport: position in beats");
  c.handle(msg("/transport/position/seconds", {F(2)}));
  expect(nearly(h.pos, 2.0 * s.tempo / 60.0 * s.ppq), "transport: position in seconds");
  c.handle(msg("/transport/loop", {I(2), I(3)}));
  expect(h.loopA == s.barTicks() && h.loopB == 3 * s.barTicks(), "transport: loop bars 2 to 3");
  c.handle(msg("/transport/loop/off"));
  expect(h.loopA == 0 && h.loopB == 0, "transport: loop off");
  c.handle(msg("/transport/mode", {osc::Arg::Str("song")}));
  expect(h.song, "transport: mode song");

  const ObjectId t1 = s.mixer[1].id, t2 = s.mixer[2].id;
  expect(c.handle(msg("/mixer/1/volume", {F(0.25)})) == R::Handled && nearly(norm(h.d, mixerParam(t1, MixerParam::Volume)), 0.25), "mixer: volume");
  c.handle(msg("/mixer/1/pan", {F(0.75)}));
  expect(nearly(norm(h.d, mixerParam(t1, MixerParam::Pan)), 0.75), "mixer: pan");
  c.handle(msg("/mixer/[1-2]/mute", {I(1)}));
  expect(s.mixerTrack(t1)->muted && s.mixerTrack(t2)->muted && !s.mixer[3].muted, "mixer: mute through an address pattern");
  c.handle(msg("/mixer/1/mute"));
  expect(!s.mixerTrack(t1)->muted, "mixer: mute without a value toggles");
  c.handle(msg("/mixer/2/solo", {I(1)}));
  expect(s.mixerTrack(t2)->solo, "mixer: solo");
  c.handle(msg("/mixer/2/solo/toggle", {I(1)}));
  expect(!s.mixerTrack(t2)->solo, "mixer: solo/toggle");
  c.handle(msg("/mixer/2/send/5", {F(0.4)}));
  expect(nearly(norm(h.d, sendParam(t2, 5)), 0.4), "mixer: a send (made when missing)");
  const ObjectId t7 = s.mixer[7].id;
  c.handle(msg("/mixer/7/slot/2/bypass", {I(1)}));
  expect(!s.mixerTrack(t7)->slots[1].enabled, "mixer: slot bypass");
  c.handle(msg("/mixer/7/slot/2/bypass", {I(0)}));
  expect(s.mixerTrack(t7)->slots[1].enabled, "mixer: slot bypass off");
  c.handle(msg("/mixer/7/slot/2/mix", {F(0.5)}));
  expect(nearly(s.mixerTrack(t7)->slots[1].mix, 0.5), "mixer: slot mix");
  expect(c.handle(msg("/mixer/99/volume", {F(1)})) == R::Unknown, "mixer: a track that does not exist");

  const ObjectId ch1 = s.channels[0].id;
  c.handle(msg("/channel/1/volume", {F(0.5)}));
  expect(nearly(norm(h.d, channelParam(ch1, ChannelParam::Volume)), 0.5), "channel: volume");
  c.handle(msg("/channel/1/mute", {I(1)}));
  expect(s.channels[0].muted, "channel: mute");
  c.handle(msg("/channel/1/note", {I(60), I(100)}));
  c.handle(msg("/channel/1/note", {I(64), F(0.5)}));
  c.handle(msg("/channel/1/noteoff", {I(60)}));
  expect(h.notes.size() == 3 && h.notes[0] == std::make_pair(60, 100) && h.notes[1] == std::make_pair(64, 64) && h.notes[2] == std::make_pair(60, 0),
         "channel: note on, a 0..1 velocity, note off");
  expect(c.heldNotes() == 1, "channel: held notes are tracked");
  c.releaseNotes();
  expect(h.notes.back() == std::make_pair(64, 0) && c.heldNotes() == 0, "channel: held notes are released when OSC stops");
  h.sel = s.channels[2].id;
  c.handle(msg("/channel/selected/pan", {F(0.1)}));
  expect(nearly(norm(h.d, channelParam(s.channels[2].id, ChannelParam::Pan)), 0.1), "channel: the selected channel");

  // Plugin parameters: the bass synth (channel 6) by number and by name (continuous ones: 2 is "Osc 1 level",
  // 4 "Osc 2 level"); a mixer slot.
  const ObjectId bass = s.channels[5].id;
  c.handle(msg("/param/channel/6/0/2", {F(0.3)}));
  expect(nearly(norm(h.d, channelPluginParam(bass, 1)), 0.3, 0.01), "param: a channel's instrument parameter by number");
  const std::string pname = describeParam(s, channelPluginParam(bass, 3))->name;
  std::string spaced = pname;
  for (char& ch : spaced)
    if (ch == ' ') ch = '_';
  c.handle(msg("/param/channel/6/instrument/" + spaced, {F(0.6)}));
  expect(nearly(norm(h.d, channelPluginParam(bass, 3)), 0.6, 0.02), "param: by name (case and spaces ignored)");
  const ObjectId clapT = s.mixer[2].id;
  c.handle(msg("/param/mixer/2/1/1", {F(0.8)}));
  expect(nearly(norm(h.d, slotParam(clapT, 0, 0)), 0.8, 0.02), "param: a mixer slot's parameter");
  expect(c.handle(msg("/param/mixer/2/1/9999", {F(0.8)})) == R::Unknown, "param: a parameter that does not exist");

  // Patterns and markers.
  c.handle(msg("/pattern/select", {I(2)}));
  expect(h.pat == s.patterns[1].id, "pattern: select by number");
  c.handle(msg("/pattern/next", {I(1)}));
  expect(h.pat == s.patterns[2].id, "pattern: next");
  c.handle(msg("/pattern/select", {osc::Arg::Str(s.patterns[0].name)}));
  expect(h.pat == s.patterns[0].id, "pattern: select by name");
  h.d.editSong("markers", [](Song& x) {
    Marker a;
    a.pos = 4 * x.barTicks();
    a.name = "Drop";
    Marker b;
    b.pos = 8 * x.barTicks();
    b.name = "Break";
    x.markers = {a, b};
  });
  h.song = false;
  c.handle(msg("/marker/jump", {I(2)}));
  expect(nearly(h.pos, 8.0 * s.barTicks()) && h.song, "marker: jump by number (song mode)");
  c.handle(msg("/marker/jump", {osc::Arg::Str("drop")}));
  expect(nearly(h.pos, 4.0 * s.barTicks()), "marker: jump by name");
  c.handle(msg("/marker/next", {I(1)}));
  expect(nearly(h.pos, 8.0 * s.barTicks()), "marker: next");

  expect(c.handle(msg("/stage/scene/1")) == R::Reserved, "stage: reserved for the Stage view");
  expect(c.handle(msg("/nothing/here")) == R::Unknown, "unknown addresses are reported");
  expect(c.handle(msg("/transport/tempo", {osc::Arg::Str("fast")})) == R::BadArgs, "bad arguments are reported");

  // Learn, then the link runs.
  const ParamId target = channelPluginParam(bass, 1);
  h.learning = true;
  h.learnTarget = target;
  expect(c.handle(msg("/1/fader3", {F(0.1)})) == R::Learned, "learn: the next address is learned");
  expect(!s.links.empty() && s.links.back().source == RemoteSource::Osc && s.links.back().oscAddress == "/1/fader3", "learn: an OSC link is saved");
  expect(c.handle(msg("/1/fader3", {F(0.9)})) == R::Linked && nearly(norm(h.d, target), 0.9, 0.01), "link: moves the parameter");
  h.d.editSong("range", [](Song& x) {
    x.links.back().min = 0.2f;
    x.links.back().max = 0.6f;
    x.links.back().invert = true;
  });
  c.handle(msg("/1/fader3", {F(0.0)}));
  expect(nearly(norm(h.d, target), 0.6, 0.01), "link: range and invert like MIDI links");
  // An action link (button).
  h.d.editSong("act", [](Song& x) {
    ControllerLink l;
    l.source = RemoteSource::Osc;
    l.oscAddress = "/1/push1";
    l.action = RemoteAction::Metronome;
    x.links.push_back(l);
  });
  c.handle(msg("/1/push1", {F(1)}));
  c.handle(msg("/1/push1", {F(0)}));
  c.handle(msg("/1/push1"));
  expect(h.actions.size() == 2 && h.actions[0] == RemoteAction::Metronome, "link: an action fires on press and impulse, not release");

  // Links save with the project.
  {
    const std::string json = projectToText(h.d.song());
    Song back;
    std::string err;
    expect(loadProjectText(json, "osc.gridline", back, &err) && back.links == h.d.song().links, "link: OSC links save and load with the project");
  }

  // Feedback.
  std::vector<osc::Message> fb;
  c.resetFeedback();
  c.feedback(10.0, fb);
  expect(find(fb, "/transport/tempo") && nearly(find(fb, "/transport/tempo")->args[0].f, 133, 1e-3), "feedback: tempo");
  expect(find(fb, "/transport/play") && find(fb, "/transport/record") && find(fb, "/transport/position/bars"), "feedback: transport state and position");
  expect(find(fb, "/1/fader3") && nearly(find(fb, "/1/fader3")->args[0].f, 0.0, 0.02), "feedback: a linked value as the control's position");
  expect(find(fb, "/mixer/1/volume") && nearly(find(fb, "/mixer/1/volume")->args[0].f, 0.25, 1e-3), "feedback: a fixed address received before");
  fb.clear();
  c.feedback(10.001, fb);
  expect(fb.empty(), "feedback: nothing when nothing changed");
  h.d.setParam(mixerParam(t1, MixerParam::Volume), 0.3);
  c.feedback(10.002, fb);
  expect(fb.empty(), "feedback: rate limited");
  c.feedback(10.2, fb);
  expect(find(fb, "/mixer/1/volume") != nullptr, "feedback: a change made in Gridline goes out");
  fb.clear();
  c.handle(msg("/mixer/1/volume", {F(0.5)}));
  c.feedback(11.0, fb);
  expect(find(fb, "/mixer/1/volume") == nullptr, "feedback: a value set by OSC is not echoed");
  c.handle(msg("/gridline/ping"));
  fb.clear();
  c.feedback(11.01, fb);
  expect(find(fb, "/gridline/pong") != nullptr, "feedback: ping answers pong");
  expect(nearly(OscControl::linkPosition(h.d.song().links[h.d.song().links.size() - 2], 0.4), 0.5, 0.01), "feedback: inverse of range and invert");
}

// ---------------------------------------------------------------- loopback ----

void testLoopback() {
  TestHost h(makeTestSong());
  OscControl c(h);
  OscServer srv;
  OscServerConfig cfg;
  cfg.port = 0;
  std::string err;
  expect(srv.start(cfg, &err), "loopback: server on 127.0.0.1");
  Udp surface;
  auto pump = [&](size_t n) {
    for (const auto& in : await(srv, n)) c.handle(in.msg);
  };
  surface.send(srv.port(), osc::encode(msg("/transport/play", {I(1)})));
  surface.send(srv.port(), osc::encodeBundle(osc::kImmediately, {msg("/transport/tempo", {F(128)}), msg("/mixer/3/volume", {F(0.4)})}));
  surface.send(srv.port(), osc::encode(msg("/param/channel/6/0/2", {F(0.7)})));
  pump(4);
  const Song& s = h.d.song();
  expect(h.isPlaying, "loopback: transport plays");
  expect(nearly(s.tempo, 128, 1e-3), "loopback: tempo");
  expect(nearly(norm(h.d, mixerParam(s.mixer[3].id, MixerParam::Volume)), 0.4), "loopback: mixer volume");
  expect(nearly(norm(h.d, channelPluginParam(s.channels[5].id, 1)), 0.7, 0.01), "loopback: a plugin parameter");
  h.learning = true;
  h.learnTarget = channelParam(s.channels[1].id, ChannelParam::Volume);
  surface.send(srv.port(), osc::encode(msg("/surface/knob", {F(0.2)})));
  pump(1);
  surface.send(srv.port(), osc::encode(msg("/surface/knob", {F(0.65)})));
  pump(1);
  expect(nearly(norm(h.d, channelParam(s.channels[1].id, ChannelParam::Volume)), 0.65, 0.01), "loopback: a learned link drives the parameter");
  std::vector<osc::Message> fb;
  c.feedback(1.0, fb);
  for (const auto& m : fb) srv.sendTo(0x7F000001u, surface.port, osc::encode(m));
  const auto got = surface.receive(0.5);
  expect(find(got, "/transport/tempo") && nearly(find(got, "/transport/tempo")->args[0].f, 128, 1e-3) && find(got, "/transport/play"),
         "loopback: the surface receives feedback");
  srv.stop();
}

// ---------------------------------------------------------------- app helper ----

// The hidden app test's sender: waits for "osc: listening <port>" on stdin (the app's output),
// sends, waits for feedback, then passes the rest of the app's output through.
int appHelper() {
  std::string line;
  int port = 0;
  while (std::getline(std::cin, line)) {
    std::cout << line << "\n";
    if (line.rfind("osc: listening ", 0) == 0) {
      port = std::atoi(line.c_str() + 15);
      break;
    }
  }
  if (port <= 0) {
    std::cout << "helper: the app never listened\n";
    return 1;
  }
  Udp u;
  u.send(port, osc::encode(msg("/transport/tempo", {F(133)})));
  u.send(port, osc::encode(msg("/mixer/1/volume", {F(0.25)})));
  u.send(port, osc::encode(msg("/param/mixer/0/1/1", {F(0.5)})));
  u.send(port, osc::encode(msg("/channel/1/note", {I(60), I(100)})));
  u.send(port, osc::encode(msg("/channel/1/noteoff", {I(60)})));
  u.send(port, osc::encode(msg("/transport/play", {I(1)})));
  std::cout << "helper: sent 6\n";
  bool tempo = false, play = false;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!(tempo && play) && std::chrono::steady_clock::now() < end)
    for (const auto& m : u.receive(0.2)) {
      if (m.address == "/transport/tempo" && !m.args.empty() && nearly(m.args[0].f, 133, 1e-3)) tempo = true;
      if (m.address == "/transport/play" && !m.args.empty() && m.args[0].f > 0.5) play = true;
    }
  std::cout << "helper: feedback tempo " << (tempo ? "yes" : "no") << " play " << (play ? "yes" : "no") << "\n";
  u.send(port, osc::encode(msg("/gridline/ping")));  // the app counts it: the script's waitfor ends
  std::cout.flush();
  while (std::getline(std::cin, line)) std::cout << line << "\n";
  return tempo && play ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "apphelper") return appHelper();
  testCodec();
  testPatternsAndSlip();
  testFuzz();
  testServer();
  testControl();
  testLoopback();
  return finish();
}
