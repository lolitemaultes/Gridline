// Gridline - K-357: JACK / PipeWire audio driver & named ports tests (all platforms, no daemon).
// Uses FakeServer implementing the full jack::Api C function table to verify:
//  1. Port planning, naming, clamping and name sanitization (client names, port aliases, track names)
//  2. Client open/activate, name collision reporting, port register/unregister on pool resize
//  3. Track add/remove/rename updating input port aliases & pretty-name metadata off the audio thread
//  4. Per-track audio input buffer routing & monitoring through AudioEngine
//  5. Extra stereo output pairs (mixer track taps + direct-only outputs)
//  6. Dynamic period / chunking invariance (4096 frames, 37 frames, mid-stream buffer size change)
//  7. Zero heap allocations on the realtime process callback across 1000 consecutive cycles
//  8. Server shutdown injection, graceful SDL fallback reason strings, and RetryWorker recovery
//  9. Connection save/restore round-trip, reopen persistence, and late-arriving peer reconnection
// 10. Project JSON serialization round-trip and byte-identical empty/untouched projects
// 11. Collaboration isolation (LocalState masking, keepSessionState, sharedSongDigest equality)
// 12. Auto-connect & natural physical port ordering (outputs on, inputs off by default)
// 13. Capture + playback latency reporting and PDC added on top
// 14. Sample rate change, xrun counting, freewheel mode, and runtime loadApi missing-path error
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "Check.h"
#include "collab/LocalState.h"
#include "core/Json.h"
#include "core/Project.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "host/audio/JackApi.h"
#include "host/audio/JackCore.h"
#include "host/audio/JackFake.h"
#include "host/audio/JackHost.h"

// ---- realtime allocation counter (same pattern as test_j109.cpp) ----
namespace {
thread_local bool gCountAllocs = false;
std::atomic<long> gAllocCount{0};
}  // namespace

void* operator new(std::size_t n) {
  if (gCountAllocs) gAllocCount.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCountAllocs) gAllocCount.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
using namespace gl::jack;

namespace {

// Realtime render state mirroring main.cpp's DeviceRender (fixed buffers, zero allocations).
struct TestRender {
  AudioEngine* eng = nullptr;
  const std::atomic<bool>* freewheel = nullptr;
  float inter[kMaxInputChannels * AudioEngine::kMaxBlock] = {};
  int extraCount = 0;
  int extraMixer[kMaxExtraPairs] = {};
  int extraOut[kMaxExtraPairs] = {};
};

void testRenderFn(void* user, const float* const* in, int nIn, float* const* out, int nOut, int frames) {
  auto* r = static_cast<TestRender*>(user);
  AudioEngine& eng = *r->eng;
  const int inClamped = std::min(nIn, kMaxInputChannels);
  const bool hasOut = nOut >= 2 && out && out[0] && out[1];
  for (int done = 0; done < frames;) {
    const int n = std::min(AudioEngine::kMaxBlock, frames - done);
    if (inClamped > 0 && in) {
      for (int c = 0; c < inClamped; ++c) {
        const float* src = in[c];
        if (src) {
          const float* s = src + done;
          for (int i = 0; i < n; ++i) r->inter[static_cast<size_t>(i) * static_cast<size_t>(inClamped) + static_cast<size_t>(c)] = s[i];
        }
      }
      eng.pushInput(r->inter, n);
    }
    if (hasOut) {
      float* tl[kMaxExtraPairs];
      float* tr[kMaxExtraPairs];
      int valid = 0;
      for (int k = 0; k < r->extraCount && k < kMaxExtraPairs; ++k) {
        const int idx = r->extraOut[k];
        if (idx >= 0 && idx + 1 < nOut && out[idx] && out[idx + 1]) {
          tl[valid] = out[idx] + done;
          tr[valid] = out[idx + 1] + done;
          ++valid;
        }
      }
      AudioEngine::OutputTaps taps;
      taps.count = valid;
      taps.mixer = r->extraMixer;
      taps.L = tl;
      taps.R = tr;
      eng.process(out[0] + done, out[1] + done, n, valid > 0 ? &taps : nullptr);
    }
    done += n;
  }
}

float peakOf(const std::vector<float>& v) {
  float p = 0.f;
  for (float x : v) p = std::max(p, std::fabs(x));
  return p;
}

bool containsStr(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// 1. Port planning, naming, clamping and sanitization.
void testPortPlanningAndSanitization() {
  const PortPlan def = planPorts(2, 0);
  expect(def.ins.size() == 2, "planPorts default: 2 inputs");
  expect(def.ins[0].name == "In 1" && def.ins[1].name == "In 2", "planPorts default input names");
  expect(def.outs.size() == 2, "planPorts default: 2 main outputs");
  expect(def.outs[0].name == "Out 1" && def.outs[0].alias == "Main L", "planPorts Out 1 -> Main L");
  expect(def.outs[1].name == "Out 2" && def.outs[1].alias == "Main R", "planPorts Out 2 -> Main R");

  const PortPlan zero = planPorts(0, 0);
  expect(zero.ins.empty() && zero.outs.size() == 2, "planPorts 0 inputs still keeps main stereo outputs");

  const PortPlan clamped = planPorts(99, 40);
  expect(static_cast<int>(clamped.ins.size()) == kMaxInputs, "planPorts clamps inputs to 16");
  expect(static_cast<int>(clamped.outs.size()) == 2 + 2 * kMaxExtraPairs, "planPorts clamps extra pairs to 15 (32 outs)");
  expect(clamped.outs[2].name == "Out 3" && clamped.outs[2].alias == "Pair 1 L", "planPorts extra pair 1 L");
  expect(clamped.outs[3].name == "Out 4" && clamped.outs[3].alias == "Pair 1 R", "planPorts extra pair 1 R");
  expect(clamped.outs.back().name == "Out 32" && clamped.outs.back().alias == "Pair 15 R", "planPorts extra pair 15 R");

  const PortPlan neg = planPorts(-5, -3);
  expect(neg.ins.empty() && neg.outs.size() == 2, "planPorts negative counts clamp to 0");

  // Sanitization: strips ':', '/', '\', control chars, trims & collapses spaces, falls back on empty.
  expect(sanitizeClientName("  Gridline:Main/Live\n\t ") == "Gridline Main Live", "sanitizeClientName removes : / ctrl");
  expect(sanitizeClientName(":::\r\n   ") == "Gridline", "sanitizeClientName falls back to Gridline");
  expect(sanitizeClientName(std::string(80, 'A')).size() == kMaxClientNameLen, "sanitizeClientName clamps length");
  expect(formatTrackAlias("In 1", "Lead:Vocal/1\n") == "In 1 (Lead Vocal 1)", "formatTrackAlias sanitizes track name");
  expect(formatTrackAlias("In 1", "   ") == "In 1", "formatTrackAlias blank track keeps base name");
  expect(formatTrackAlias("In 1", std::string(100, 'X')).size() <= kMaxPortNameLen, "formatTrackAlias respects max port len");

  // Input aliases follow first mixer track using each channel.
  const PortPlan p4 = planPorts(4, 0);
  const std::vector<TrackInputRef> refs = {{"Vocal", 0, true}, {"Guitar", 2, false}, {"Backup", 0, false}};
  const std::vector<std::string> al = planInputAliases(p4, refs);
  expect(al.size() == 4 && al[0] == "In 1 (Vocal)" && al[1] == "In 2 (Vocal)" && al[2] == "In 3 (Guitar)" && al[3] == "In 4",
         "planInputAliases maps stereo and mono tracks onto input port aliases");

  // PipeWire latency env & extra mixers.
  expect(pipewireLatencyEnv(0, 48000).empty(), "pipewireLatencyEnv(0) is empty");
  expect(pipewireLatencyEnv(256, 48000) == "256/48000", "pipewireLatencyEnv formats frames/rate");
  expect(pipewireLatencyEnv(128, 0) == "128/48000", "pipewireLatencyEnv defaults rate to 48000");

  const std::vector<int> mx = resolveExtraMixers(3, {{2, 0}, {5, 2}, {9, 8}, {0, 1}});
  expect(mx == std::vector<int>({2, -1, 5}), "resolveExtraMixers maps valid pairs and leaves unassigned as -1");

  // Status & connected peer formatting.
  StatusInfo si{"Gridline", true, false, 48000, 256, 2, 2, 128, 256, 0};
  const std::string stLine = formatStatusLine(si);
  expect(stLine.find("via PipeWire") != std::string::npos && stLine.find("48000 Hz") != std::string::npos &&
             stLine.find("256-frame buffer") != std::string::npos,
         "formatStatusLine includes PipeWire, rate and buffer");
  expect(formatLatencyLine(si).find("128 in / 256 out, 0 dropouts") != std::string::npos, "formatLatencyLine formats latency/xruns");
  expect(formatConnectedPeers({"Tone:out_1", "Tone:out_2", "Tone:out_1"}) == "Tone:out_1, Tone:out_2",
         "formatConnectedPeers deduplicates peers");
}

// 2. Open/activate, name collision, port register/unregister, track add/rename/remove aliases.
void testLifecycleAndTrackAliases() {
  FakeServer::Spec spec;
  spec.sampleRate = 48000;
  spec.bufferSize = 256;
  FakeServer srv(spec);
  const Api api = srv.api();

  Host h1, h2;
  Config cfg;
  cfg.inputs = 4;
  cfg.extraPairs = 1;
  cfg.clientName = "Gridline";
  std::string err;
  expect(h1.open(api, cfg, nullptr, nullptr, &err), "host 1 opens on fake server");
  expect(h1.clientName() == "Gridline", "first client gets exact name Gridline");
  expect(h1.activate(&err), "host 1 activates");

  const std::vector<std::string> ports1 = srv.portNames("Gridline:");
  expect(ports1.size() == 8, "registered 4 inputs + 2 main outputs + 2 extra outputs = 8 ports");
  expect(containsStr(ports1, "Gridline:In 1") && containsStr(ports1, "Gridline:In 4") &&
             containsStr(ports1, "Gridline:Out 1") && containsStr(ports1, "Gridline:Out 4"),
         "expected named ports registered");

  // Second instance: client name collision produces "Gridline-01".
  expect(h2.open(api, cfg, nullptr, nullptr, &err), "host 2 opens with colliding name");
  expect(h2.clientName() == "Gridline-01", "second client reports collided name Gridline-01");
  h2.close();
  expect(srv.portNames("Gridline-01:").empty(), "closing host 2 unregisters all of its ports");

  // Track add / rename / remove updates aliases and JACK pretty-name metadata off the audio thread.
  h1.updateInputAliases({{"Vocal", 0, true}, {"Synth", 2, false}});
  expect(containsStr(srv.portAliases("Gridline:In 1"), "In 1 (Vocal)"), "track add sets In 1 alias");
  expect(containsStr(srv.portAliases("Gridline:In 2"), "In 2 (Vocal)"), "stereo track add sets In 2 alias");
  expect(srv.portProperty("Gridline:In 1", kMetadataPrettyName) == "In 1 (Vocal)", "track add sets pretty-name metadata");
  expect(srv.portProperty("Gridline:In 3", kMetadataPrettyName) == "In 3 (Synth)", "mono track sets In 3 pretty-name");

  // Track rename -> old alias unset, new alias and pretty-name set.
  h1.updateInputAliases({{"Lead Vox", 0, true}, {"Synth", 2, false}});
  expect(!containsStr(srv.portAliases("Gridline:In 1"), "In 1 (Vocal)"), "track rename clears old alias");
  expect(containsStr(srv.portAliases("Gridline:In 1"), "In 1 (Lead Vox)"), "track rename sets new alias");
  expect(srv.portProperty("Gridline:In 1", kMetadataPrettyName) == "In 1 (Lead Vox)", "track rename updates pretty-name");

  // Track removed / unassigned -> alias reverts cleanly to base port name.
  h1.updateInputAliases({});
  expect(containsStr(srv.portAliases("Gridline:In 1"), "In 1") &&
             !containsStr(srv.portAliases("Gridline:In 1"), "In 1 (Lead Vox)"),
         "track removal resets alias back to In 1");
  expect(srv.portProperty("Gridline:In 1", kMetadataPrettyName) == "In 1", "track removal resets pretty-name");

  // Port pool resize (reopen with 2 inputs, 0 extra pairs) unregisters old ports and registers new set.
  cfg.inputs = 2;
  cfg.extraPairs = 0;
  expect(h1.open(api, cfg, nullptr, nullptr, &err) && h1.activate(&err), "reopen with smaller port pool succeeds");
  const std::vector<std::string> portsSmall = srv.portNames("Gridline:");
  expect(portsSmall.size() == 4 && !containsStr(portsSmall, "Gridline:In 3") && !containsStr(portsSmall, "Gridline:Out 3"),
         "port pool shrink unregisters removed input and extra output ports");

  h1.close();
  expect(srv.portNames("Gridline:").empty(), "close unregisters all Gridline ports");
}

// 3, 4, 5, 6: Per-track audio input routing, extra stereo outputs, chunking invariance, and 0 RT allocs.
void testAudioRoutingExtrasChunkingAndZeroAllocs() {
  FakeServer::Spec spec;
  spec.sampleRate = 48000;
  spec.bufferSize = 256;
  FakeServer srv(spec);
  srv.addPeerClient("Tone", {"out_1", "out_2", "out_3"});
  srv.setPeerSignal("Tone:out_1", [](float* out, Nframes n, uint64_t) {
    for (Nframes i = 0; i < n; ++i) out[i] = 0.25f;
  });
  srv.setPeerSignal("Tone:out_2", [](float* out, Nframes n, uint64_t) {
    for (Nframes i = 0; i < n; ++i) out[i] = -0.50f;
  });
  srv.setPeerSignal("Tone:out_3", [](float* out, Nframes n, uint64_t) {
    for (Nframes i = 0; i < n; ++i) out[i] = 0.40f;
  });

  // Minimal song:
  //   Insert 1: stereo input In 1 - In 2, Monitor On -> main mix (Out 1 / Out 2)
  //   Insert 2: mono input In 3, Monitor On, sent to extra pair 1 (Out 3 / Out 4) AND NOT to master
  Song song = makeEmptySong();
  song.mixer[1].name = "Vocal";
  song.mixer[1].input = 0;
  song.mixer[1].inputStereo = true;
  song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);

  song.mixer[2].name = "ClickAux";
  song.mixer[2].input = 2;
  song.mixer[2].inputStereo = false;
  song.mixer[2].monitor = static_cast<uint8_t>(InputMonitor::On);
  song.mixer[2].sends.clear();  // does not go to master: proves Out 3 / Out 4 carries Insert 2 alone!

  ProgramBuilder builder;
  builder.setSampleRate(48000.0);
  AudioEngine eng(48000.0);
  eng.setInputChannels(4);
  eng.setInputPrefill(0);
  eng.setProgram(builder.build(song));

  TestRender render;
  render.eng = &eng;
  render.extraCount = 2;      // Out 3-4 (pair 0 -> Insert 2), Out 5-6 (pair 1 -> direct only / silent)
  render.extraMixer[0] = 2;   // mixer index 2 -> pair 0 (Out 3 / Out 4)
  render.extraOut[0] = 2;
  render.extraMixer[1] = -1;  // unassigned -> pair 1 (Out 5 / Out 6)
  render.extraOut[1] = 4;

  const Api api = srv.api();
  Host host;
  Config cfg;
  cfg.inputs = 4;
  cfg.extraPairs = 2;
  std::string err;
  expect(host.open(api, cfg, testRenderFn, &render, &err) && host.activate(&err), "audio routing host opens and activates");
  render.freewheel = &host.freewheelFlag();

  host.autoConnect(true, false);
  expect(host.connect("In 1", "Tone:out_1"), "connect Tone:out_1 -> In 1");
  expect(host.connect("In 2", "Tone:out_2"), "connect Tone:out_2 -> In 2");
  expect(host.connect("In 3", "Tone:out_3"), "connect Tone:out_3 -> In 3");

  // Run a few cycles to let monitoring settle past any smoothing ramps.
  for (int i = 0; i < 4; ++i) srv.cycle(256);

  expect(eng.takeInputPeak(0) > 0.20f && eng.takeInputPeak(1) > 0.45f && eng.takeInputPeak(2) > 0.35f,
         "engine receives per-channel non-zero input signals from JACK peer");
  const std::vector<float> playL = srv.lastBuffer("system:playback_1", 256);
  const std::vector<float> playR = srv.lastBuffer("system:playback_2", 256);
  expect(peakOf(playL) > 0.15f && peakOf(playR) > 0.30f, "monitored Insert 1 reaches system:playback_1 and playback_2");
  expect(std::fabs(playR.back() / std::max(1e-6f, playL.back()) + 2.0f) < 0.05f, "stereo input L/R ratio preserved on Out 1/2");

  const std::vector<float> extra1L = srv.lastBuffer("Gridline:Out 3", 256);
  const std::vector<float> extra1R = srv.lastBuffer("Gridline:Out 4", 256);
  const std::vector<float> extra2L = srv.lastBuffer("Gridline:Out 5", 256);
  expect(peakOf(extra1L) > 0.25f && peakOf(extra1R) > 0.25f, "Insert 2 routed to extra stereo pair Out 3 / Out 4");
  expect(peakOf(extra2L) < 1e-6f, "unassigned extra stereo pair Out 5 / Out 6 stays silent");

  // Chunking invariance: 4096-frame cycle (4x kMaxBlock) and 37-frame prime cycle without reopen.
  srv.injectBufferSizeChange(4096);
  const int evBig = host.poll();
  expect((evBig & Host::BufferChanged) != 0 && host.bufferFrames() == 4096, "buffer size change to 4096 handled without reopen");
  srv.cycle(4096);
  expect(peakOf(srv.lastBuffer("system:playback_1", 4096)) > 0.15f, "4096-frame cycle produces clean output");

  srv.injectBufferSizeChange(37);
  const int evOdd = host.poll();
  expect((evOdd & Host::BufferChanged) != 0 && host.bufferFrames() == 37, "buffer size change to 37 handled without reopen");
  srv.cycle(37);
  expect(peakOf(srv.lastBuffer("system:playback_1", 37)) > 0.15f, "37-frame odd cycle produces clean output");

  // Return to 256 frames and verify ZERO heap allocations across 1000 realtime cycles!
  srv.injectBufferSizeChange(256);
  host.poll();
  srv.cycle(256);  // warm-up cycle
  gAllocCount.store(0, std::memory_order_relaxed);
  gCountAllocs = true;
  for (int i = 0; i < 1000; ++i) srv.cycle(256);
  gCountAllocs = false;
  const long rtAllocs = gAllocCount.load(std::memory_order_relaxed);
  expect(rtAllocs == 0, "0 heap allocations in JACK process callback across 1000 cycles");

  host.close();
}

// 7, 8, 12, 13, 14: Shutdown + SDL fallback reason + RetryWorker + links + auto-connect + latency + rate/xrun/freewheel.
void testLinksShutdownLatencyAndEvents() {
  FakeServer::Spec spec;
  spec.sampleRate = 48000;
  spec.bufferSize = 256;
  spec.captureLatency = 128;
  spec.playbackLatency = 256;
  FakeServer srv(spec);
  srv.addPeerClient("Tone", {"out_1", "out_2"});
  const Api api = srv.api();

  // Auto-connect: outputs to physical speakers ON, inputs OFF by default.
  Host host;
  Config cfg;
  cfg.inputs = 2;
  cfg.extraPairs = 0;
  std::string err;
  expect(host.open(api, cfg, nullptr, nullptr, &err) && host.activate(&err), "host opens for graph tests");
  host.autoConnect(true, false);
  expect(srv.isConnected("Gridline:Out 1", "system:playback_1") && srv.isConnected("Gridline:Out 2", "system:playback_2"),
         "autoConnect wires Out 1/2 to physical speakers");
  expect(!srv.isConnected("system:capture_1", "Gridline:In 1"), "autoConnect keeps physical inputs disconnected by default");

  // Latency reporting: connect physical capture_1 -> In 1 to check capture(128) + playback(256) = 384, plus PDC on port.
  expect(host.connect("In 1", "system:capture_1"), "connect system:capture_1 -> In 1");
  host.poll();
  expect(host.inputLatency() == 128 && host.outputLatency() == 256, "capture 128 and playback 256 read from server");
  expect(recordLatency({{128, 128}}, {{256, 256}}) == 384, "recordLatency = 128 + 256 = 384");
  host.setEngineOutputLatency(64);
  const LatencyRange out1Lat = srv.portLatency("Gridline:Out 1", kPlaybackLatency);
  expect(out1Lat.max == 320, "Out 1 reports engine PDC (64) added on top of playback latency (256 -> 320)");
  expect(host.outputLatency() == 256, "Host::outputLatency avoids double-counting engine PDC");

  // Natural physical port ordering.
  const std::vector<std::string> phys =
      pickPhysical({"system:playback_10", "system:playback_2", "system:playback_1"}, 2);
  expect(phys == std::vector<std::string>({"system:playback_1", "system:playback_2"}),
         "pickPhysical uses natural numeric order");

  // Links snapshot, reopen, restore, and late peer arrival.
  expect(host.disconnect("In 1", "system:capture_1"), "disconnect capture_1");
  expect(host.connect("In 1", "Tone:out_1"), "connect Tone:out_1 -> In 1");
  std::vector<Link> saved = host.snapshotLinks();
  saved.push_back({"In 2", "LateApp:out_1"});  // peer does not exist yet
  const std::string text = formatLinks(saved);
  expect(parseLinks(text) == saved, "parseLinks / formatLinks round-trip");

  // Reopen with 4 inputs (drops live links as real jack_deactivate does), then restore.
  cfg.inputs = 4;
  expect(host.open(api, cfg, nullptr, nullptr, &err) && host.activate(&err), "reopen with 4 inputs");
  expect(!srv.isConnected("Tone:out_1", "Gridline:In 1"), "reopen starts with fresh unconnected ports");
  const int restored1 = host.restoreLinks(saved);
  expect(restored1 >= 3 && srv.isConnected("Tone:out_1", "Gridline:In 1") &&
             srv.isConnected("Gridline:Out 1", "system:playback_1"),
         "restoreLinks reconnects existing peers");
  expect(!srv.isConnected("LateApp:out_1", "Gridline:In 2"), "absent peer link waits");

  // Peer app starts later -> ClientAppeared event -> restoreLinks connects it without dropping existing links.
  srv.addPeerClient("LateApp", {"out_1", "out_2"});
  const int evPeer = host.poll();
  expect((evPeer & Host::ClientAppeared) != 0, "adding peer client raises ClientAppeared event");
  const int restored2 = host.restoreLinks(saved);
  expect(restored2 == 1 && srv.isConnected("LateApp:out_1", "Gridline:In 2") &&
             srv.isConnected("Tone:out_1", "Gridline:In 1"),
         "late peer connects on arrival while keeping existing links");

  // Rate change, xrun and freewheel events.
  srv.injectRateChange(96000);
  const int evRate = host.poll();
  expect((evRate & Host::RateChanged) != 0 && host.sampleRate() == 96000, "sample rate change raises RateChanged");

  srv.injectXrun();
  expect((host.poll() & Host::Xrun) != 0 && host.xruns() == 1, "xrun increments counter");
  srv.injectFreewheel(true);
  expect((host.poll() & Host::Freewheel) != 0 && host.freewheeling(), "freewheel entering sets flag");
  srv.injectXrun();
  expect(host.xruns() == 1, "xruns ignored while freewheeling");
  srv.injectFreewheel(false);
  host.poll();
  expect(!host.freewheeling(), "freewheel exit clears flag");

  // Shutdown injection: poll reports Shutdown + reason, process never runs again, close is safe.
  const long long cyclesBefore = host.cycles();
  srv.injectShutdown(kStatusFailure, "PipeWire daemon stopped");
  const int evShut = host.poll();
  expect((evShut & Host::Shutdown) != 0 && !host.active(), "shutdown injection raises Shutdown and marks host inactive");
  expect(host.shutdownReason() == "PipeWire daemon stopped", "shutdown preserves exact reason string");
  srv.cycle(256);
  expect(host.cycles() == cyclesBefore, "process callback is never called after shutdown");
  host.close();  // must be completely safe after shutdown

  // Graceful fallback when server is stopped -> readable reason string.
  srv.setServerRunning(false);
  std::string downErr;
  expect(!host.open(api, cfg, nullptr, nullptr, &downErr), "open fails cleanly when server is down");
  expect(downErr.find("No JACK or PipeWire server is running") != std::string::npos,
         "fallback error string explains no server is running");

  // RetryWorker detects server coming back on background thread.
  RetryWorker retry;
  retry.start(&api, 50);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  expect(!retry.takeServerBack(), "RetryWorker stays quiet while server is down");
  srv.setServerRunning(true);
  bool back = false;
  for (int i = 0; i < 40 && !back; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    back = retry.takeServerBack();
  }
  retry.stop();
  expect(back, "RetryWorker detects server return off the UI thread");

  // Runtime loadApi with explicit missing path returns null + readable error (no crash on macOS or Linux).
  std::string libErr;
  const Api* badApi = loadApi("/nonexistent/gridline/path/libjack.so.0", &libErr);
  expect(badApi == nullptr && !libErr.empty(), "loadApi with missing explicit path returns null and readable error");
}

// 10 & 11: Project JSON serialization round-trip, byte-identical empty song, and collaboration isolation.
void testProjectAndCollaborationIsolation() {
  // Empty/untouched song must not emit "portLinks" at all (byte-identical to before K-357).
  const Song empty = makeEmptySong();
  const std::string emptyJson = projectToText(empty);
  expect(emptyJson.find("portLinks") == std::string::npos, "empty project JSON has no portLinks key (byte-identical)");

  // Round-trip of saved portLinks through project JSON.
  Song withLinks = empty;
  withLinks.portLinks = {{"In 1", "StreamApp:out_1"}, {"In 2", "StreamApp:out_2"}, {"Out 1", "system:playback_1"}};
  const std::string savedText = projectToText(withLinks);
  expect(savedText.find("portLinks") != std::string::npos, "non-empty portLinks written to project JSON");

  Song loaded;
  std::string err;
  expect(loadProjectText(savedText, "test.gridline", loaded, &err), "project with portLinks loads cleanly");
  expect(loaded.portLinks == withLinks.portLinks, "portLinks round-trip exact through project JSON");

  // Old project without portLinks loads with empty vector.
  Song loadedOld;
  expect(loadProjectText(emptyJson, "old.gridline", loadedOld, &err) && loadedOld.portLinks.empty(),
         "project without portLinks loads with empty portLinks");

  // Song::operator== ignores per-computer portLinks (no undo step, never dirties project).
  expect(withLinks == empty, "Song::operator== ignores per-computer portLinks");

  // Collaboration isolation: maskSessionJson strips portLinks, keepSessionState preserves local links,
  // and sharedSongDigest is identical regardless of local JACK graph.
  Json masked = songToJson(withLinks);
  collab::maskSessionJson(masked);
  expect(masked.find("portLinks") == nullptr, "collab::maskSessionJson strips portLinks");

  Song incoming = makeEmptySong();
  incoming.tempo = 128.0;
  incoming.portLinks = {{"In 1", "OtherPeer:out_1"}};
  collab::keepSessionState(incoming, withLinks);
  expect(incoming.portLinks == withLinks.portLinks, "collab::keepSessionState preserves this computer's portLinks");
  expect(incoming.tempo == 128.0, "collab::keepSessionState keeps incoming shared song changes");

  expect(collab::sharedSongDigest(withLinks) == collab::sharedSongDigest(empty),
         "collab::sharedSongDigest ignores local JACK portLinks");
}

}  // namespace

int main() {
  testPortPlanningAndSanitization();
  testLifecycleAndTrackAliases();
  testAudioRoutingExtrasChunkingAndZeroAllocs();
  testLinksShutdownLatencyAndEvents();
  testProjectAndCollaborationIsolation();
  return finish();
}
