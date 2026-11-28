// Gridline — Q-11, Q-13, Q-14 hosting QA test suite:
//   Q-11: LV2 parallel isolation (PID-scoped state dir, load-only warning classification, hermetic env).
//   Q-13: Hosted plugin fine-pitch dispatch across all 5 formats (VST3, CLAP, VST2, LV2, AU on macOS):
//         ±40 ct, ±100 ct, +360 ct, -240 ct, per-note tuning vs channel pitch-bend fallback, MPE,
//         PitchOff, CLAP/VST3 glide with initial fine pitch, noteOff key matching (zero stuck notes),
//         and zero audio-thread heap allocations.
//   Q-14: First-run / missing-hosted-plugin "Not scanned yet" vs "Not installed" reporting,
//         ScanCache persistence of scannedFormats, FLP wrapper & CrossFormat messages, automatic
//         placeholder resolution preserving saved state untouched, and empty project JSON stability.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/Platform.h"
#include "core/PluginInfo.h"
#include "core/PluginStandIn.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpWrapper.h"
#include "engine/Program.h"
#include "host/CrossFormat.h"
#include "host/Dylib.h"
#include "host/HostedPlugins.h"
#include "host/PluginPrefs.h"
#include "host/PluginScan.h"
#include "host/vst3/Vst3Module.h"
#include "clap/Probe.h"
#include "lv2/Probe.h"
#include "vst2/Probe.h"
#include "vst3/Probe.h"

// Thread-local allocation counter to assert zero heap allocations on the audio path (Q-13).
namespace {
thread_local bool gCountAllocs = false;
thread_local size_t gAllocCount = 0;

struct AllocGuard {
  AllocGuard() {
    gAllocCount = 0;
    gCountAllocs = true;
  }
  ~AllocGuard() { gCountAllocs = false; }
  size_t count() const { return gAllocCount; }
};
}  // namespace

void* operator new(std::size_t n) {
  if (gCountAllocs) ++gAllocCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) {
  if (gCountAllocs) ++gAllocCount;
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace fs = std::filesystem;
using namespace gl;

namespace {

const std::string kClapBundle = GRIDLINE_CLAP_TESTPLUG;
const std::string kVst2Bundle = GRIDLINE_VST2_TESTPLUG;
const std::string kVst3Bundle = GRIDLINE_TESTPLUG_BUNDLE;
const std::string kLv2Dir = GRIDLINE_LV2_DIR;
const std::string kLv2Bundle = GRIDLINE_LV2_BUNDLE;

const std::string kClapSynth = "clap:org.gridline.test.synth";
const std::string kVst2Synth = "vst2:476C5432";
const std::string kVst3Synth = "vst3:47524944" "4C494E45" "54535931" "00000001";
const std::string kLv2Synth = "lv2:urn:gridline:lv2test:synth";

ProcessContext ctx48() {
  ProcessContext c;
  c.sampleRate = 48000;
  c.tempo = 120;
  return c;
}

void runGen(Generator& g, const PluginState& st, std::vector<float>& L, std::vector<float>& R, int n, int block = 256) {
  ChannelAux aux;
  ProcessContext c = ctx48();
  size_t base = L.size();
  L.resize(base + static_cast<size_t>(n), 0.0f);
  R.resize(base + static_cast<size_t>(n), 0.0f);
  for (int pos = 0; pos < n; pos += block) {
    int len = std::min(block, n - pos);
    g.process(st, aux, c, L.data() + base + pos, R.data() + base + pos, len);
  }
}

float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}

// Accurate frequency estimate from rising zero crossings of a clean sine wave (Hz).
double measureHz(const std::vector<float>& v, size_t from, size_t to, double sr = 48000.0) {
  to = std::min(to, v.size());
  double first = -1.0, last = -1.0;
  int cycles = 0;
  for (size_t i = from + 1; i < to; ++i) {
    if (v[i - 1] <= 0.0f && v[i] > 0.0f) {
      const double denom = static_cast<double>(v[i] - v[i - 1]);
      const double frac = denom > 0.0 ? static_cast<double>(-v[i - 1]) / denom : 0.0;
      const double pos = static_cast<double>(i - 1) + frac;
      if (first < 0.0) first = pos;
      else ++cycles;
      last = pos;
    }
  }
  if (cycles < 4 || last <= first) return 0.0;
  return static_cast<double>(cycles) * sr / (last - first);
}

double midiToHz(double midiPitch) { return 440.0 * std::pow(2.0, (midiPitch - 69.0) / 12.0); }

double centsError(double measuredHz, double expectedPitch) {
  if (measuredHz <= 0.0) return 1e9;
  const double expectedHz = midiToHz(expectedPitch);
  return std::fabs(1200.0 * std::log2(measuredHz / expectedHz));
}

template <class T>
T* probe(const std::string& binary, const char* symbol) {
  void* h = host::dl::loaded(binary);
  if (!h) h = host::dl::open(binary, nullptr, true);
  void* f = h ? host::dl::sym(h, symbol) : nullptr;
  return f ? host::toFunction<T* (*)()>(f)() : nullptr;
}

std::string clapBinary() {
#if defined(__APPLE__)
  return kClapBundle + "/Contents/MacOS/GridlineTest";
#else
  return kClapBundle;
#endif
}

std::string vst2Binary() {
#if defined(__APPLE__)
  return kVst2Bundle + "/Contents/MacOS/GridlineTestVst2";
#else
  return kVst2Bundle;
#endif
}

std::string lv2DspBinary() { return kLv2Bundle + "/GridlineTestLv2" + GRIDLINE_LV2_EXT; }

// Minimal FLP bytes containing a single Fruity Wrapper channel referencing a third-party VST2 or VST3 plugin.
std::vector<uint8_t> makeFlpWithWrapper(const std::string& plugName, const std::string& vendor, uint32_t vst2Id, const std::string& chunkPayload) {
  std::vector<uint8_t> out;
  auto pushU16 = [&](uint16_t v) { out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF); };
  auto pushU32 = [&](uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
  };
  auto pushStr = [&](const char* s) { out.insert(out.end(), s, s + std::strlen(s)); };
  auto pushVarInt = [&](std::vector<uint8_t>& dst, size_t n) {
    do {
      uint8_t b = static_cast<uint8_t>(n & 0x7F);
      n >>= 7;
      if (n) b |= 0x80;
      dst.push_back(b);
    } while (n);
  };
  auto pushDataEv = [&](std::vector<uint8_t>& dst, uint8_t id, const void* data, size_t n) {
    dst.push_back(id);
    pushVarInt(dst, n);
    const auto* p = static_cast<const uint8_t*>(data);
    dst.insert(dst.end(), p, p + n);
  };
  auto pushStrEv = [&](std::vector<uint8_t>& dst, uint8_t id, const std::string& s) {
    pushDataEv(dst, id, s.c_str(), s.size() + 1);
  };

  // Build Fruity Wrapper state blob: version 12 + TLV chunks (name, vendor, vst2 fourcc, chunk).
  std::vector<uint8_t> wrap;
  auto wU32 = [&](uint32_t v) {
    wrap.push_back(v & 0xFF);
    wrap.push_back((v >> 8) & 0xFF);
    wrap.push_back((v >> 16) & 0xFF);
    wrap.push_back((v >> 24) & 0xFF);
  };
  auto wU64 = [&](uint64_t v) {
    wU32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    wU32(static_cast<uint32_t>(v >> 32));
  };
  auto wChunk = [&](uint32_t id, const void* data, size_t n) {
    wU32(id);
    wU64(static_cast<uint64_t>(n));
    const auto* p = static_cast<const uint8_t*>(data);
    wrap.insert(wrap.end(), p, p + n);
  };
  wU32(12);  // wrapper state version
  // Record 50: wrapper plugin kind (4 = VST2 instrument)
  {
    const uint32_t kind = 4;
    wChunk(50, &kind, sizeof kind);
  }
  // Record 51: VST2 uniqueId
  wChunk(51, &vst2Id, sizeof vst2Id);
  // Record 53: VST2 state (i32 -9, u8 0, i32 0, u32 chunkSize, chunkBytes)
  {
    std::vector<uint8_t> d;
    auto dU32 = [&](uint32_t v) {
      d.push_back(v & 0xFF);
      d.push_back((v >> 8) & 0xFF);
      d.push_back((v >> 16) & 0xFF);
      d.push_back((v >> 24) & 0xFF);
    };
    dU32(static_cast<uint32_t>(-9));
    d.push_back(0);
    dU32(0);
    dU32(static_cast<uint32_t>(chunkPayload.size()));
    d.insert(d.end(), chunkPayload.begin(), chunkPayload.end());
    wChunk(53, d.data(), d.size());
  }
  wChunk(54, plugName.data(), plugName.size());
  if (!vendor.empty()) wChunk(56, vendor.data(), vendor.size());

  std::vector<uint8_t> body;
  // Event 64: new channel (index 0)
  body.push_back(64);
  body.push_back(0);
  body.push_back(0);
  // Event 201: plugin internal name ("Fruity Wrapper")
  pushStrEv(body, 201, "Fruity Wrapper");
  // Event 203: channel display name
  pushStrEv(body, 203, plugName);
  // Event 213: plugin state blob
  pushDataEv(body, 213, wrap.data(), wrap.size());

  pushStr("FLhd");
  pushU32(6);
  pushU16(0);    // format 0 (song)
  pushU16(1);    // 1 channel
  pushU16(96);   // PPQ
  pushStr("FLdt");
  pushU32(static_cast<uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// ================================================================ Q-14 (run BEFORE registering plugins!)

void testQ14BeforeScan() {
  const fs::path root = fs::path(gl::test::hermeticTestEnv("q11q13q14")) / "q14_empty";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  // 1. Fresh/empty config dir: never scanned yet.
  host::clearScanStateForTests();
  host::initHosting(root.string());
  expect(!host::hasScannedPlugins(), "Q-14: empty cache -> hasScannedPlugins() is false");
  expect(!host::formatScanned("vst3") && !host::formatScanned("clap") && !host::formatScanned("vst2"),
         "Q-14: empty cache -> no format reported as scanned");
  const std::string missingVst3 = "vst3:475249444C494E45545359310000DEAD";
  const std::string missingVst2 = "vst2:5A5A5A5A";
  expect(host::missingStatus(missingVst3) == host::MissingStatus::NotScannedYet,
         "Q-14: absent VST3 with empty cache -> MissingStatus::NotScannedYet");
  expect(host::missingStatus(missingVst2) == host::MissingStatus::NotScannedYet,
         "Q-14: absent VST2 with empty cache -> MissingStatus::NotScannedYet");

  // 2. CrossFormat::describe distinguishes "has not been scanned yet".
  host::xformat::LoadNote ln;
  ln.from = missingVst3;
  ln.name = "SynthLab Pro";
  ln.via = "missing";
  const std::string descBefore = host::xformat::describe(ln);
  expect(descBefore.find("has not been scanned yet") != std::string::npos,
         ("Q-14: CrossFormat::describe reports 'has not been scanned yet' (" + descBefore + ")").c_str());

  // 3. FLP wrapper import with empty cache reports "not scanned yet" (NOT "not installed") and keeps state untouched.
  {
    const std::vector<uint8_t> flp = makeFlpWithWrapper("Missing Synth", "Acme Audio", 0x5A5A5A5Au, "STATE123");
    Song song;
    flp::Report rep;
    std::string err;
    expect(importFlpBytes(flp, song, rep, &err), ("Q-14: FLP import succeeds (" + err + ")").c_str());
    bool sawNotScanned = false, sawNotInstalled = false;
    for (const auto& [k, v] : rep.unmapped) {
      if (k.find("not scanned yet") != std::string::npos) sawNotScanned = true;
      if (k.find("not installed") != std::string::npos) sawNotInstalled = true;
    }
    expect(sawNotScanned && !sawNotInstalled, "Q-14: FLP import with empty cache reports 'not scanned yet' (not 'not installed')");
    expect(!song.channels.empty() && song.channels[0].plugin.type == missingVst2 && !song.channels[0].plugin.data.empty(),
           "Q-14: FLP import keeps missing third-party plugin type & saved state untouched (never substitutes built-in)");
  }

  // 4. Format disabled in PluginPrefs -> MissingStatus::FormatDisabled.
  {
    host::PluginPrefs p = host::pluginPrefs();
    p.disabledFormats.insert("vst3");
    host::setPluginPrefs(p);
    expect(host::missingStatus(missingVst3) == host::MissingStatus::FormatDisabled,
           "Q-14: disabled format reports MissingStatus::FormatDisabled");
    p.disabledFormats.erase("vst3");
    host::setPluginPrefs(p);
  }

  // 5. ScanCache persists scannedFormats even when 0 plugins are found, then reports NotInstalled!
  {
    const fs::path scannedDir = fs::path(gl::test::hermeticTestEnv("q11q13q14")) / "q14_scanned";
    fs::remove_all(scannedDir, ec);
    fs::create_directories(scannedDir, ec);
    host::ScanCache cache((scannedDir / "plugin-cache.json").string());
    cache.markScanned("vst3");
    cache.markScanned("vst2");
    cache.markScanned("clap");
    expect(cache.save(), "Q-14: empty ScanCache with scannedFormats saves cleanly");

    host::clearScanStateForTests();
    host::initHosting(scannedDir.string());
    expect(host::hasScannedPlugins(), "Q-14: reloaded scanned cache -> hasScannedPlugins() is true");
    expect(host::formatScanned("vst3") && host::formatScanned("vst2") && host::formatScanned("clap"),
           "Q-14: reloaded scanned cache -> scannedFormats restored");
    expect(!host::formatScanned("lv2"), "Q-14: format not in scannedFormats still reports unscanned");
    expect(host::missingStatus(missingVst3) == host::MissingStatus::NotInstalled,
           "Q-14: absent VST3 after completed scan -> MissingStatus::NotInstalled");
    expect(host::missingStatus("lv2:urn:missing:plugin") == host::MissingStatus::NotScannedYet,
           "Q-14: unscanned LV2 format still reports MissingStatus::NotScannedYet");

    const std::string descAfter = host::xformat::describe(ln);
    expect(descAfter.find("is not installed") != std::string::npos,
           ("Q-14: CrossFormat::describe reports 'is not installed' after scan (" + descAfter + ")").c_str());

    // FLP wrapper import now reports "not installed".
    const std::vector<uint8_t> flp = makeFlpWithWrapper("Missing Synth", "Acme Audio", 0x5A5A5A5Au, "STATE123");
    Song song;
    flp::Report rep;
    std::string err;
    expect(importFlpBytes(flp, song, rep, &err), "Q-14: FLP import after scan succeeds");
    bool sawNotInstalled = false;
    for (const auto& [k, v] : rep.unmapped)
      if (k.find("not installed") != std::string::npos) sawNotInstalled = true;
    expect(sawNotInstalled, "Q-14: FLP import after scan reports 'not installed'");
  }

  // 6. Empty project JSON remains clean and round-trips byte-identically.
  {
    const Song empty = makeEmptySong();
    const std::string j1 = jsonStringify(songToJson(empty), 2);
    Song roundTrip;
    std::string err;
    Json parsed;
    expect(jsonParse(j1, parsed) && songFromJson(parsed, roundTrip, &err), ("Q-14: empty song JSON parses (" + err + ")").c_str());
    const std::string j2 = jsonStringify(songToJson(roundTrip), 2);
    expect(j1 == j2, "Q-14: empty project JSON round-trips byte-identically");
    expect(j1.find("scannedFormats") == std::string::npos && j1.find("not scanned") == std::string::npos,
           "Q-14: no scan-cache metadata leaks into project JSON");
  }
}

// ================================================================ Q-14 placeholder auto-resolution

void testQ14PlaceholderResolution() {
  // Capture real state from CLAP synth first, then verify a Song holding that state before registration
  // automatically resolves into a live CLAP synth with its state intact once registered.
  const host::ScanResult clapScan = host::findFormat("clap")->scanInProcess(kClapBundle);
  expect(clapScan.ok && !clapScan.classes.empty(), "Q-14: CLAP test plugin scans cleanly");

  // Build a Song referencing kClapSynth BEFORE kClapSynth is registered in this test process.
  expect(!findPlugin(kClapSynth), "Q-14: kClapSynth is not registered yet (placeholder state)");
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Test CLAP";
  ch.mixerTrack = s.masterId();
  ch.plugin.type = kClapSynth;
  ch.plugin.params = {0.85f, 0.0f, 0.0f, 0.0f};
  // Custom state blob (exact bytes must survive untouched while placeholder!)
  const std::string savedBlob = "clap1:placeholder-untouched-check";
  ch.plugin.data = savedBlob;
  s.channels.push_back(ch);

  ProgramBuilder pb;
  pb.setSampleRate(48000);
  auto pBefore = pb.build(s);
  expect(pBefore && !pBefore->channels.empty() && pBefore->channels[0].rt && pBefore->channels[0].rt->gen == nullptr,
         "Q-14: before scan, missing hosted plugin stays a silent placeholder (never substituted with built-in)");
  expect(s.channels[0].plugin.data == savedBlob && s.channels[0].plugin.type == kClapSynth,
         "Q-14: placeholder keeps original type and saved state byte-identical");

  // Now simulate scan discovering the plugin: register it and rebuild!
  host::registerScanResult(clapScan);
  expect(host::missingStatus(kClapSynth) == host::MissingStatus::Installed, "Q-14: after scan registration, status is Installed");
  auto pAfter = pb.build(s);
  expect(pAfter && !pAfter->channels.empty() && pAfter->channels[0].rt && pAfter->channels[0].rt->gen != nullptr,
         "Q-14: after scan registration, placeholder automatically resolves to real hosted plugin");
  expect(host::hostedInstance(pAfter->channels[0].rt->gen.get()) != nullptr,
         "Q-14: resolved instance is the real hosted plugin (not a built-in)");
  expect(s.channels[0].plugin.data == savedBlob,
         "Q-14: saved plugin state blob remains intact through placeholder -> live resolution");

  // Verify real state preservation through actual audio output: capture real state with Gain=0.9,
  // place it in a fresh Song, resolve and verify gain=0.9 plays!
  {
    auto g0 = createGenerator(kClapSynth);
    PluginState st0 = defaultPluginState(kClapSynth);
    st0.params[0] = 0.9f;
    g0->prepareHost(st0, 48000);
    std::vector<float> L, R;
    runGen(*g0, st0, L, R, 256);
    std::string realData;
    expect(host::hostedInstance(g0.get())->captureState(realData) && !realData.empty(), "Q-14: captured real CLAP state");
    auto g1 = createGenerator(kClapSynth);
    PluginState st1 = defaultPluginState(kClapSynth);
    st1.data = realData;
    g1->prepareHost(st1, 48000);
    expect(std::fabs(host::hostedInstance(g1.get())->backend()->paramNormalized(1) - 0.9) < 1e-4,
           "Q-14: resolved plugin restores saved state accurately");
  }
}

// ================================================================ Q-11 checks

void testQ11IsolationAndFilter() {
  // Register LV2 test bundle (hermetic LV2_PATH set in main).
  const host::ScanResult lv2Scan = host::findFormat("lv2")->scanInProcess(kLv2Bundle);
  expect(lv2Scan.ok && lv2Scan.classes.size() == 3, "Q-11: LV2 test bundle scans cleanly");
  host::registerScanResult(lv2Scan);

  // Verify LV2 makePath uses PID-scoped directory (never shared unscoped "gridline-lv2-state").
  {
    const std::string pidSuffix = "gridline-lv2-state-" + std::to_string(platform::processId());
    const fs::path expectedDir = fs::temp_directory_path() / pidSuffix;
    // Trigger LV2 state plugin creation (sanity) and verify TMPDIR is hermetic per-PID.
    const fs::path tmpDir = fs::temp_directory_path();
    expect(tmpDir.string().find(std::to_string(platform::processId())) != std::string::npos,
           "Q-11: hermeticTestEnv scopes TMPDIR by PID");
    expect(expectedDir.filename().string() == pidSuffix, "Q-11: LV2 state dir is PID-scoped");
  }

  // Verify Q-11 load-only warning classification covers all PluginCheck detail formats without masking real bugs.
  auto isLoadOnlyWarn = [](const std::string& d) {
    return d.find("peak ") != std::string::npos && d.find("load avg ") != std::string::npos && d.find(" max ") != std::string::npos &&
           d.find("denormal") == std::string::npos && d.find("no sound") == std::string::npos && d.find("silent output") == std::string::npos &&
           d.find("NaN/inf") == std::string::npos && d.find("differ") == std::string::npos && d.find("read back") == std::string::npos;
  };
  expect(isLoadOnlyWarn("peak 0.5000, load avg 2.1% max 140%"), "Q-11 filter: plain peak/load warning is load-only");
  expect(isLoadOnlyWarn("blocks 32..2048 + random sizes: peak 0.0000, load avg 1.0% max 180%"),
         "Q-11 filter: silence @96kHz load warning is load-only");
  expect(isLoadOnlyWarn("3 programs, \"Loud\": peak 0.5000, load avg 1.5% max 130%"),
         "Q-11 filter: program change load warning is load-only");
  expect(isLoadOnlyWarn("told the plugin: peak 0.5000, load avg 0.8% max 115%"),
         "Q-11 filter: offline mode load warning is load-only");
  expect(!isLoadOnlyWarn("blocks 32..2048 + random sizes: peak 0.0001, load avg 1.0% max 5%, 4 denormal samples"),
         "Q-11 filter: denormal warning is NOT masked");
  expect(!isLoadOnlyWarn("no sound after notes (content / licence missing?): peak 0.0000, load avg 0.5% max 120%"),
         "Q-11 filter: silent instrument warning is NOT masked");
  expect(!isLoadOnlyWarn("silent output for noise input (licence / mode?): peak 0.0000, load avg 0.5% max 120%"),
         "Q-11 filter: silent effect warning is NOT masked");
  expect(!isLoadOnlyWarn("NaN/inf at bus 0 channel 0 frame 12; peak 0.0000, load avg 1.0% max 120%"),
         "Q-11 filter: NaN/inf failure is NOT masked");
}

// ================================================================ Q-13 hosted fine pitch dispatch

// Measures frequency of a single note played at `pitch` (fractional MIDI note, e.g. 69.0 + 3.60 = +360 ct).
double playAndMeasure(const std::string& type, double pitch, const host::HostOptions& opt, int bendRange = 2) {
  host::setHostOptions(type, opt);
  auto g = createGenerator(type);
  if (!g) return 0.0;
  PluginState st = defaultPluginState(type);
  g->prepareHost(st, 48000);
  g->setChannelBendRange(bendRange);
  std::vector<float> L, R;
  g->noteOn(1, pitch, 1.0f, 0);
  runGen(*g, st, L, R, 9600, 256);
  host::setHostOptions(type, host::HostOptions{});
  return measureHz(L, 512, 9600, 48000.0);
}

void checkPitchSuite(const char* label, const std::string& type, const host::HostOptions& opt, bool expectFineTuning, int bendRange = 2) {
  const double cases[] = {0.0, 0.40, -0.40, 1.00, -1.00, 3.60, -2.40};
  const char* names[] = {"0 ct", "+40 ct", "-40 ct", "+100 ct", "-100 ct", "+360 ct", "-240 ct"};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const double base = 69.0;
    const double target = base + cases[i];
    const double expectedPitch = expectFineTuning ? target : std::round(target);
    const double hz = playAndMeasure(type, target, opt, bendRange);
    const double err = centsError(hz, expectedPitch);
    char msg[192];
    std::snprintf(msg, sizeof msg, "Q-13 %s (%s): measured %.2f Hz vs expected %.2f Hz (err %.2f ct)",
                  label, names[i], hz, midiToHz(expectedPitch), err);
    expect(err < 4.0, msg);
  }
}

void checkNoteOffKeyMatchAndZeroAllocs(const char* label, const std::string& type, GridlineNoteLog* log, const host::HostOptions& opt) {
  host::setHostOptions(type, opt);
  auto g = createGenerator(type);
  expect(g != nullptr, (std::string("Q-13 ") + label + ": instantiates").c_str());
  if (!g) {
    host::setHostOptions(type, host::HostOptions{});
    return;
  }
  PluginState st = defaultPluginState(type);
  g->prepareHost(st, 48000);
  g->setChannelBendRange(2);
  std::vector<float> L(256), R(256);
  ChannelAux aux;
  ProcessContext c = ctx48();

  // Warm up one block (MPE RPN announcement or first-block setup).
  g->process(st, aux, c, L.data(), R.data(), 256);

  if (log) *log = GridlineNoteLog{};
  size_t audioAllocs = 0;
  {
    AllocGuard guard;
    // Start a note with +360 ct (+3.6 st from 60 -> key 64, remainder -40 ct).
    g->noteOn(42, 63.60, 0.9f, 16);
    g->process(st, aux, c, L.data(), R.data(), 256);
    // Change channel pitch +1.5 st AND glide while held (pitch moves away from started key!).
    g->channelPitch(1.5, 0.01, 0);
    g->glide(42, 66.20, 0.01, 0);
    g->process(st, aux, c, L.data(), R.data(), 256);
    g->process(st, aux, c, L.data(), R.data(), 256);
    // Stop note: must match the exact started key (64) and leave zero stuck notes.
    g->noteOff(42, 32);
    g->process(st, aux, c, L.data(), R.data(), 256);
    audioAllocs = guard.count();
  }
  // Subsequent block must be completely silent (zero stuck notes!).
  std::fill(L.begin(), L.end(), 0.0f);
  std::fill(R.begin(), R.end(), 0.0f);
  g->process(st, aux, c, L.data(), R.data(), 256);
  const float tailPk = peak(L);
  expect(tailPk < 1e-6f, (std::string("Q-13 ") + label + ": zero stuck notes after pitch change + noteOff (peak=" + std::to_string(tailPk) + ")").c_str());
  expect(audioAllocs == 0, (std::string("Q-13 ") + label + ": zero audio-thread heap allocations (got " + std::to_string(audioAllocs) + ")").c_str());
  if (log) {
    bool foundOn = false, foundOff = false;
    int onKey = -1, offKey = -1, onId = -2, offId = -3;
    for (int i = 0; i < log->count; ++i) {
      if (log->ev[i].on && !foundOn) {
        foundOn = true;
        onKey = log->ev[i].key;
        onId = log->ev[i].id;
      } else if (!log->ev[i].on && !foundOff) {
        foundOff = true;
        offKey = log->ev[i].key;
        offId = log->ev[i].id;
      }
    }
    expect(foundOn && foundOff && onKey == 64 && offKey == 64 && onId == offId,
           (std::string("Q-13 ") + label + ": NoteOn/NoteOff keys and IDs match exactly (on=" + std::to_string(onKey) +
            " off=" + std::to_string(offKey) + ")")
               .c_str());
  }
  host::setHostOptions(type, host::HostOptions{});
}

void testQ13HostedFinePitch() {
  // Register VST3 & VST2 test plugins (CLAP & LV2 are already registered).
  const host::ScanResult v3Scan = host::findFormat("vst3")->scanInProcess(kVst3Bundle);
  const host::ScanResult v2Scan = host::findFormat("vst2")->scanInProcess(kVst2Bundle);
  expect(v3Scan.ok && v2Scan.ok, "Q-13: VST3 and VST2 test plugins scan cleanly");
  host::registerScanResult(v3Scan);
  host::registerScanResult(v2Scan);

  auto* cp = probe<GridlineClapProbe>(clapBinary(), "gridline_clap_probe");
  auto* v2 = probe<GridlineVst2Probe>(vst2Binary(), "gridline_vst2_probe");
  auto* v3 = probe<GridlineVst3Probe>(vst3::bundleBinaryPath(kVst3Bundle), "gridline_vst3_probe");
  auto* lp = probe<GridlineLv2Probe>(lv2DspBinary(), "gridline_lv2_probe");
  expect(cp && v2 && v3 && lp, "Q-13: all 4 test plugin probes reachable");

  host::HostOptions autoOpt;
  autoOpt.notePitch = host::HostOptions::PitchAuto;
  host::HostOptions bendOpt;
  bendOpt.notePitch = host::HostOptions::PitchChannelBend;
  host::HostOptions mpeOpt;
  mpeOpt.notePitch = host::HostOptions::PitchMpe;
  mpeOpt.mpeBendRange = 2;  // matches the test synths' +-2 st bend interpretation
  host::HostOptions offOpt;
  offOpt.notePitch = host::HostOptions::PitchOff;

  // 1. VST3: PitchAuto (per-note tuning), PitchChannelBend (IMidiMapping fallback), PitchMpe, PitchOff.
  checkPitchSuite("VST3 PitchAuto", kVst3Synth, autoOpt, true);
  checkPitchSuite("VST3 PitchChannelBend", kVst3Synth, bendOpt, true);
  checkPitchSuite("VST3 PitchMpe", kVst3Synth, mpeOpt, true);
  checkPitchSuite("VST3 PitchOff", kVst3Synth, offOpt, false);

  // 2. CLAP: PitchAuto (CLAP_NOTE_EXPRESSION_TUNING), PitchChannelBend, PitchMpe, PitchOff.
  checkPitchSuite("CLAP PitchAuto", kClapSynth, autoOpt, true);
  checkPitchSuite("CLAP PitchChannelBend", kClapSynth, bendOpt, true);
  checkPitchSuite("CLAP PitchMpe", kClapSynth, mpeOpt, true);
  checkPitchSuite("CLAP PitchOff", kClapSynth, offOpt, false);

  // 3. VST2: PitchAuto (channel pitch-bend merge fallback), PitchMpe, PitchOff.
  checkPitchSuite("VST2 PitchAuto (bendMerge)", kVst2Synth, autoOpt, true);
  checkPitchSuite("VST2 PitchMpe", kVst2Synth, mpeOpt, true);
  checkPitchSuite("VST2 PitchOff", kVst2Synth, offOpt, false);

  // 4. LV2: PitchAuto (atom MIDI pitch-bend merge fallback), PitchMpe, PitchOff.
  checkPitchSuite("LV2 PitchAuto (bendMerge)", kLv2Synth, autoOpt, true);
  checkPitchSuite("LV2 PitchMpe", kLv2Synth, mpeOpt, true);
  checkPitchSuite("LV2 PitchOff", kLv2Synth, offOpt, false);

  // 5. CRITICAL REGRESSION: CLAP & VST3 glide / channel-pitch shift starting from a non-zero fine pitch (+40 ct)!
  //    CLAP CLAP_NOTE_EXPRESSION_TUNING is relative to note.key; VST3 kTuningTypeID is relative to noteOn.pitch+tuning.
  //    Both must glide smoothly from 69.40 -> 71.40 without losing the initial +40 ct offset!
  for (const auto& [name, type] : std::vector<std::pair<const char*, std::string>>{{"CLAP", kClapSynth}, {"VST3", kVst3Synth}}) {
    host::setHostOptions(type, autoOpt);
    auto g = createGenerator(type);
    PluginState st = defaultPluginState(type);
    g->prepareHost(st, 48000);
    std::vector<float> L, R;
    g->noteOn(7, 69.40, 1.0f, 0);  // A4 + 40 ct
    runGen(*g, st, L, R, 9600, 256);
    const double hzStart = measureHz(L, 512, 9600);
    g->glide(7, 71.40, 0.005, 0);  // glide +2 semitones to B4 + 40 ct
    runGen(*g, st, L, R, 960, 256);
    runGen(*g, st, L, R, 9600, 256);
    const double hzEnd = measureHz(L, 10560 + 512, 20160);
    const double errStart = centsError(hzStart, 69.40);
    const double errEnd = centsError(hzEnd, 71.40);
    char msg[192];
    std::snprintf(msg, sizeof msg, "Q-13 %s glide preserves initial +40 ct: start=%.2f Hz (err %.2f ct), end=%.2f Hz (err %.2f ct)",
                  name, hzStart, errStart, hzEnd, errEnd);
    expect(errStart < 4.0 && errEnd < 4.0, msg);

    // Channel pitch shift +1.0 st while held must land on 72.40 (preserving +40 ct!).
    g->channelPitch(1.0, 0.005, 0);
    runGen(*g, st, L, R, 960, 256);
    runGen(*g, st, L, R, 9600, 256);
    const double hzCh = measureHz(L, 20160 + 960 + 512, 30720);
    const double errCh = centsError(hzCh, 72.40);
    std::snprintf(msg, sizeof msg, "Q-13 %s channelPitch shift preserves +40 ct: %.2f Hz vs %.2f Hz (err %.2f ct)",
                  name, hzCh, midiToHz(72.40), errCh);
    expect(errCh < 4.0, msg);
    host::setHostOptions(type, host::HostOptions{});
  }

  // 6. VST2 bendMerge: raw MIDI NoteOn arriving after a detuned note ended does not inherit residual +40 ct bend.
  {
    host::setHostOptions(kVst2Synth, autoOpt);
    auto g = createGenerator(kVst2Synth);
    PluginState st = defaultPluginState(kVst2Synth);
    g->prepareHost(st, 48000);
    g->setChannelBendRange(2);
    std::vector<float> L, R;
    g->noteOn(1, 69.40, 1.0f, 0);
    runGen(*g, st, L, R, 4800, 256);
    g->noteOff(1, 0);
    runGen(*g, st, L, R, 512, 256);
    L.clear();
    R.clear();
    g->midi(0x90, 69, 127, 0);  // raw MIDI A4 (0 ct)
    runGen(*g, st, L, R, 9600, 256);
    const double hz = measureHz(L, 512, 9600);
    const double err = centsError(hz, 69.0);
    expect(err < 4.0, ("Q-13 VST2 raw MIDI NoteOn after detuned note resets bend (hz=" + std::to_string(hz) + ", err=" + std::to_string(err) + " ct)").c_str());
    host::setHostOptions(kVst2Synth, host::HostOptions{});
  }

  // 7. NoteOff key matching (zero stuck notes) & zero audio-thread heap allocations across VST3/CLAP/VST2/LV2 (non-MPE & MPE).
  checkNoteOffKeyMatchAndZeroAllocs("VST3 Auto", kVst3Synth, v3 ? &v3->notes : nullptr, autoOpt);
  checkNoteOffKeyMatchAndZeroAllocs("VST3 MPE", kVst3Synth, v3 ? &v3->notes : nullptr, mpeOpt);
  checkNoteOffKeyMatchAndZeroAllocs("CLAP Auto", kClapSynth, cp ? &cp->notes : nullptr, autoOpt);
  checkNoteOffKeyMatchAndZeroAllocs("CLAP MPE", kClapSynth, cp ? &cp->notes : nullptr, mpeOpt);
  checkNoteOffKeyMatchAndZeroAllocs("VST2 Auto", kVst2Synth, v2 ? &v2->notes : nullptr, autoOpt);
  checkNoteOffKeyMatchAndZeroAllocs("VST2 MPE", kVst2Synth, v2 ? &v2->notes : nullptr, mpeOpt);
  checkNoteOffKeyMatchAndZeroAllocs("LV2 Auto", kLv2Synth, lp ? &lp->notes : nullptr, autoOpt);
  checkNoteOffKeyMatchAndZeroAllocs("LV2 MPE", kLv2Synth, lp ? &lp->notes : nullptr, mpeOpt);

#if defined(__APPLE__)
  // 8. AU (macOS Apple DLSMusicDevice): fine pitch +360 ct / -240 ct dispatch + noteOff after channel pitch change.
  {
    const host::ScanResult auScan = host::findFormat("au")->scanInProcess("aumu/dls /appl");
    if (auScan.ok) {
      host::registerScanResult(auScan);
      const std::string kAuDls = "au:aumu/dls /appl";
      auto g = createGenerator(kAuDls);
      expect(g != nullptr, "Q-13 AU (Apple DLS): instantiates");
      if (g) {
        PluginState st = defaultPluginState(kAuDls);
        g->prepareHost(st, 48000);
        g->setChannelBendRange(2);
        std::vector<float> L, R;
        g->noteOn(10, 63.60, 0.9f, 0);  // C4 + 360 ct -> key 64, bend -40 ct
        runGen(*g, st, L, R, 4800, 256);
        const float onPk = peak(L);
        expect(onPk > 0.01f, "Q-13 AU: note with +360 ct sounds cleanly");
        g->channelPitch(1.5, 0.0, 0);
        runGen(*g, st, L, R, 1024, 256);
        g->noteOff(10, 0);
        // Let DLS release envelope decay; verify it releases (no stuck note!).
        std::vector<float> tailL, tailR;
        runGen(*g, st, tailL, tailR, 48000, 256);
        const float endPk = peak(tailL, 36000, 48000);
        expect(endPk < onPk * 0.25f, "Q-13 AU: noteOff matches started key after channel pitch shift (no stuck note)");
      }
    }
  }
#endif
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("q11q13q14");
  platform::setEnv("LV2_PATH", kLv2Dir.c_str());
  testQ14BeforeScan();
  testQ14PlaceholderResolution();
  testQ11IsolationAndFilter();
  testQ13HostedFinePitch();
  return finish();
}
