// Gridline - K-142 (0.7.8 follow-up): what another thread queues for the interface wakes its
// sleeping loop at once, and costs nothing while the loop is awake.
//  * uiwake (src/app/UiWake.h): a poke while awake calls no waker; a poke before the sleep stops it;
//    pokes while asleep wake it once; pokes from many threads racing the loop's sleeps are all taken
//    within one turn (none waits for the loop's timed wake); a real-time thread's poke never wakes
//    (no system call) and is found at the next timed look.
//  * the OSC server's network thread (OscServer::onQueued): a message wakes the loop, whose next
//    turn takes it.
//  * the AI assistant's HTTP server (HttpServer::watch): a client connecting, its request and its
//    reply each wake the loop; a request is answered with the loop taking turns only when woken;
//    stop() ends the watcher at once.
// The loop here is the app's wait (beginWait, a sleep the waker ends, endWait, takePending) with a
// long timed wake (2 s), so a missed wake shows as a slow turn.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "../src/app/UiWake.h"
#include "Check.h"
#include "core/McpTransport.h"
#include "core/Net.h"
#include "core/Osc.h"
#include "core/OscServer.h"

using namespace gl;
using namespace gl::mcp;
using Clock = std::chrono::steady_clock;

namespace {

double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// The waker: ends the loop's sleep (the app's SDL event / run loop signal).
std::mutex gMu;
std::condition_variable gCv;
bool gWoken = false;
std::atomic<int> gWakerCalls{0};
void waker() {
  gWakerCalls.fetch_add(1);
  std::lock_guard<std::mutex> lk(gMu);
  gWoken = true;
  gCv.notify_one();
}

// The interface loop: sleeps up to `nap` between turns; a turn runs `turn` (the app's frame taking
// what was queued). Counts turns, and the ones a timed wake brought.
struct Loop {
  double nap = 2.0;
  std::function<void()> turn;
  std::atomic<uint64_t> turns{0}, timed{0}, started{0};
  std::atomic<bool> stop{false};
  std::thread th;
  void start() {
    {
      std::lock_guard<std::mutex> lk(gMu);
      gWoken = false;  // (the wake that ended the loop before, if its thread left without taking it)
    }
    th = std::thread([this] {
      while (!stop.load()) {
        if (uiwake::beginWait()) {
          {
            std::unique_lock<std::mutex> lk(gMu);
            gCv.wait_for(lk, std::chrono::duration<double>(nap), [&] { return gWoken || stop.load(); });
            gWoken = false;
          }
          uiwake::endWait();
          if (!uiwake::takePending()) {
            if (stop.load()) break;
            timed.fetch_add(1);
            continue;  // (nothing queued: the timed look found nothing)
          }
        } else {
          uiwake::takePending();
        }
        started.fetch_add(1);
        if (turn) turn();
        turns.fetch_add(1);
      }
    });
  }
  void end() {
    stop = true;
    {
      std::lock_guard<std::mutex> lk(gMu);
      gWoken = true;
    }
    gCv.notify_one();
    th.join();
  }
};

void testAwake() {
  uiwake::setWaker(&waker);
  uiwake::takePending();
  const unsigned long long w0 = uiwake::wakes();
  const int c0 = gWakerCalls.load();
  uiwake::poke();
  expect(uiwake::wakes() == w0 && gWakerCalls.load() == c0, "a poke while the loop is awake wakes nothing (no system call)");
  expect(!uiwake::beginWait(), "a poke before the loop sleeps: it does not sleep");
  expect(uiwake::takePending() && !uiwake::takePending(), "the turn takes the poke, once");
  expect(uiwake::beginWait(), "nothing pending: the loop sleeps");
  uiwake::poke();
  uiwake::poke();
  uiwake::poke();
  expect(uiwake::wakes() == w0 + 1 && gWakerCalls.load() == c0 + 1, "pokes while asleep: the waker is called once for that sleep");
  uiwake::endWait();
  expect(uiwake::takePending(), "and the loop finds them pending");
  {
    std::lock_guard<std::mutex> lk(gMu);
    gWoken = false;
  }
}

void testLatency() {
  Loop loop;
  std::atomic<uint64_t> taken{0};
  loop.turn = [&] { taken.fetch_add(1); };
  loop.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  double worst = 0;
  bool all = true;
  std::mt19937 rng(7);
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::microseconds(200 + rng() % 5000));
    const uint64_t t0 = loop.turns.load();
    const auto s = Clock::now();
    uiwake::poke();
    while (loop.turns.load() == t0 && since(s) < 3.0) std::this_thread::yield();
    const double took = since(s);
    worst = std::max(worst, took);
    all = all && loop.turns.load() > t0;
  }
  loop.end();
  std::printf("# poke to turn: worst %.2f ms over 100 (the loop's timed wake: 2 s)\n", worst * 1000);
  expect(all && worst < 0.1, "a poke from another thread: the sleeping loop takes a turn at once (each of 100 within 100 ms)");
  expect(loop.timed.load() == 0, "no turn waited for the timed wake");
}

void testRace() {
  // Four threads poke at random moments while the loop sleeps and wakes; after each poke the poking
  // thread waits for a turn that began after it (and pokes no more meanwhile). A poke lost in the race
  // would wait for the timed wake (2 s).
  Loop loop;
  loop.turn = [] { std::this_thread::sleep_for(std::chrono::microseconds(50)); };
  loop.start();
  std::atomic<double> worst{0};
  std::atomic<bool> lost{false};
  std::vector<std::thread> pokers;
  for (int t = 0; t < 4; ++t)
    pokers.emplace_back([&, t] {
      std::mt19937 rng(100 + t);
      for (int i = 0; i < 500; ++i) {
        if (rng() % 2) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
        // a turn started after the poke: the one after those started before it, finished
        const uint64_t s0 = loop.started.load();
        const auto s = Clock::now();
        uiwake::poke();
        while (loop.turns.load() < s0 + 1 && since(s) < 3.0) std::this_thread::yield();
        const double took = since(s);
        if (loop.turns.load() < s0 + 1) lost = true;
        double w = worst.load();
        while (took > w && !worst.compare_exchange_weak(w, took)) {
        }
      }
    });
  for (auto& p : pokers) p.join();
  loop.end();
  std::printf("# racing pokes: %llu turns, worst wait %.2f ms\n", static_cast<unsigned long long>(loop.turns.load()), worst.load() * 1000);
  expect(!lost.load() && worst.load() < 0.5, "pokes from 4 threads racing the loop's sleeps: every one taken, none waits for the timed wake");
}

void testRealtime() {
  Loop loop;
  loop.nap = 0.25;  // (the app's hidden look: kHiddenPoll)
  loop.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const int c0 = gWakerCalls.load();
  const uint64_t t0 = loop.turns.load();
  std::atomic<bool> done{false};
  const auto s = Clock::now();
  std::thread rt([&] {
    uiwake::markRealtimeThread();
    uiwake::poke();
    done = true;
  });
  rt.join();
  while (loop.turns.load() == t0 && since(s) < 2.0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const double took = since(s);
  loop.end();
  expect(done.load() && gWakerCalls.load() == c0, "a poke from a real-time thread never calls the waker (no system call)");
  expect(loop.turns.load() > t0 && took < 0.4, "and the sleeping loop takes it at its next timed look");
}

void testOsc() {
  OscServer server, sender;
  std::atomic<int> queued{0};
  server.onQueued = [&] {
    queued.fetch_add(1);
    uiwake::poke();
  };
  OscServerConfig cfg;
  cfg.port = 0;
  std::string err;
  const bool up = server.start(cfg, &err) && sender.start(cfg, &err);
  expect(up, "OSC: two servers on 127.0.0.1");
  if (!up) return;
  std::mutex mu;
  std::vector<std::pair<std::string, double>> got;
  Clock::time_point sentAt;
  Loop loop;
  loop.turn = [&] {
    std::vector<OscIncoming> in;
    server.poll(in);
    std::lock_guard<std::mutex> lk(mu);
    for (const auto& m : in) got.push_back({m.msg.address, since(sentAt)});
  };
  loop.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  double worst = 0;
  bool all = true;
  for (int i = 0; i < 20; ++i) {
    osc::Message m;
    m.address = "/probe/" + std::to_string(i);
    m.args.push_back(osc::Arg::Float(0.5f));
    {
      std::lock_guard<std::mutex> lk(mu);
      sentAt = Clock::now();
    }
    sender.sendTo(0x7F000001u, server.port(), osc::encode(m));
    const auto s = Clock::now();
    bool seen = false;
    while (!seen && since(s) < 3.0) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      std::lock_guard<std::mutex> lk(mu);
      for (const auto& g : got)
        if (g.first == m.address) seen = true, worst = std::max(worst, g.second);
    }
    all = all && seen;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  loop.end();
  server.stop();
  sender.stop();
  std::printf("# OSC message to the turn that took it: worst %.2f ms over 20\n", worst * 1000);
  expect(queued.load() >= 20, "OSC: the network thread calls its hook for each message queued");
  expect(all && worst < 0.1, "OSC: a message wakes the sleeping loop, which takes it in its next turn (each within 100 ms)");
  expect(loop.timed.load() == 0, "OSC: no turn waited for the timed wake");
}

void testHttp() {
  HttpServer server;
  std::string err;
  const bool up = server.start(0, &err);
  expect(up, "HTTP: the server listens on 127.0.0.1");
  if (!up) return;
  expect(server.watch([] { uiwake::poke(); }), "HTTP: the watcher starts");
  std::atomic<int> handled{0};
  Loop loop;
  loop.turn = [&] {
    server.poll([&](const HttpServer::Request& rq) {
      handled.fetch_add(1);
      HttpServer::Response r;
      r.body = "{\"echo\":\"" + rq.path + "\"}";
      return r;
    });
  };
  loop.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const uint64_t turns0 = loop.started.load();
  double worst = 0;
  bool all = true;
  for (int i = 0; i < 10; ++i) {
    HttpReply reply;
    const auto s = Clock::now();
    const bool ok = httpRequest(server.port(), "POST", "/p" + std::to_string(i), {}, "{}", reply, &err, 10);
    const double took = since(s);
    worst = std::max(worst, took);
    all = all && ok && reply.status == 200 && reply.body.find("/p" + std::to_string(i)) != std::string::npos;
  }
  const uint64_t turns = loop.started.load() - turns0;  // (begun: see the idle check below)
  std::printf("# HTTP request answered: worst %.2f ms over 10, %llu turns, %llu knocks\n", worst * 1000,
              static_cast<unsigned long long>(turns), static_cast<unsigned long long>(server.knocks()));
  expect(all && handled.load() == 10, "HTTP: every request answered with the loop taking turns only when woken");
  expect(worst < 0.3, "HTTP: each within 300 ms (the loop's timed wake: 2 s)");
  expect(loop.timed.load() == 0 && server.knocks() >= 10, "HTTP: the watcher woke the loop for them (no timed wake)");
  // Idle: the watcher sleeps, the loop takes no turn. Counted by the turns begun: the client has its
  // reply while the turn that sent it is still ending (closing the connection, telling the watcher),
  // so that turn may count as ended only after this looks (Linux runs the client at once).
  const uint64_t k0 = server.knocks(), t0 = loop.started.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const uint64_t k1 = server.knocks(), t1 = loop.started.load();
  if (k1 != k0 || t1 != t0)
    std::printf("# idle: %llu knocks, %llu turns begun\n", static_cast<unsigned long long>(k1 - k0), static_cast<unsigned long long>(t1 - t0));
  expect(k1 == k0 && t1 == t0, "HTTP: no client, no knock, no turn");
  const auto s = Clock::now();
  loop.end();
  server.stop();
  expect(since(s) < 1.0, "HTTP: stop ends the watcher at once");
  expect(!server.running(), "HTTP: stopped");
}

}  // namespace

int main() {
  net::startup();
  testAwake();
  testLatency();
  testRace();
  testRealtime();
  testOsc();
  testHttp();
  return finish();
}
