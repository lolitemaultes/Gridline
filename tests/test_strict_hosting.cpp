// Gridline — K-211: hosting instruments that are picky about their host, as hardware
// emulations often are (no audio input, several stereo outputs with the extra ones switched off
// by default, the main output not on bus 0, latency, silence while they boot, MIDI-only notes).
// Uses the strict in-tree test plugins only (tests/vst3/StrictPlugin.cpp, tests/clap/StrictPlugin.cpp):
// every call out of order and every buffer that does not match the negotiated layout is a
// violation the probe reports, and the plugin then stays silent.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "common/StrictProbe.h"
#include "core/Document.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Render.h"
#include "host/Dylib.h"
#include "host/HostedPlugins.h"
#include "host/PluginScan.h"
#include "host/vst3/Vst3Module.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

using namespace gl;

namespace {

const std::string kVst3Bundle = GRIDLINE_STRICT_VST3;
const std::string kClapBundle = GRIDLINE_STRICT_CLAP;
const std::string kVst3Strict = "vst3:47524944" "4C494E45" "53545231" "00000001";
const std::string kVst3Mid = "vst3:47524944" "4C494E45" "53545231" "00000002";
const std::string kClapStrict = "clap:org.gridline.strict.synth";
const std::string kClapMid = "clap:org.gridline.strict.mid";
constexpr int kWarmup = 2048, kLatency = 64;

GridlineStrictProbe* probeIn(const std::string& binary) {
  void* h = host::dl::loaded(binary);
  if (!h) h = host::dl::open(binary, nullptr, true);
  void* f = h ? host::dl::sym(h, "gridline_strict_probe") : nullptr;
  return f ? host::toFunction<GridlineStrictProbe* (*)()>(f)() : nullptr;
}
std::string clapBinary() {
#if defined(__APPLE__)
  return kClapBundle + "/Contents/MacOS/GridlineStrict";
#else
  return kClapBundle;
#endif
}

std::string violations(uint32_t v) {
  static const char* const kNames[] = {"layout while active", "bus while active", "setup while active", "active without setup",
                                       "process inactive", "process not started", "block too long", "buffers do not match layout",
                                       "inputs passed", "deactivated while processing", "events to an inactive bus", "wrong thread"};
  std::string s;
  for (int i = 0; i < 12; ++i)
    if (v & (1u << i)) s += std::string(s.empty() ? "" : ", ") + kNames[i];
  return s.empty() ? "none" : s;
}

float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
  float p = 0;
  for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}
int firstSound(const std::vector<float>& v) {
  for (size_t i = 0; i < v.size(); ++i)
    if (std::fabs(v[i]) > 1e-6f) return static_cast<int>(i);
  return -1;
}

// n frames in blocks of `block` (the last one shorter); the extra outputs collected too.
void render(Generator& g, const PluginState& st, std::vector<float>& L, std::vector<std::vector<float>>& aux, int n, int block) {
  ChannelAux ca;
  ProcessContext c;
  c.sampleRate = 48000;
  c.tempo = 120;
  std::vector<float> R(static_cast<size_t>(AudioEngine::kMaxBlock));
  std::vector<float> l(static_cast<size_t>(AudioEngine::kMaxBlock));
  aux.resize(static_cast<size_t>(g.auxOutputCount()));
  for (int pos = 0; pos < n; pos += block) {
    const int len = std::min(block, n - pos);
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(R.begin(), R.end(), 0.0f);
    g.process(st, ca, c, l.data(), R.data(), len);
    L.insert(L.end(), l.begin(), l.begin() + len);
    for (int k = 0; k < g.auxOutputCount(); ++k) {
      const float *aL = nullptr, *aR = nullptr;
      if (g.auxOutput(k, aL, aR) && aL) aux[static_cast<size_t>(k)].insert(aux[static_cast<size_t>(k)].end(), aL, aL + len);
    }
  }
}

void scan(const char* format, const std::string& bundle, const char* label) {
  const host::Format* f = host::findFormat(format);
  host::ScanResult r = f ? f->scanInProcess(bundle) : host::ScanResult{};
  expect(r.ok && r.classes.size() == 2, (std::string(label) + ": the strict test bundle scans, 2 instruments " + r.error).c_str());
  bool shape = r.classes.size() == 2;
  for (const auto& c : r.classes)
    shape = shape && c.instrument && c.inputChannels == 0 && c.outputChannels == 2 && c.auxOutputs.size() == 2 && c.latency == kLatency;
  expect(shape, (std::string(label) + ": instrument, no input, stereo main output, 2 extra outputs, latency 64").c_str());
  for (const auto& c : r.classes)
    if (c.name == "Gridline Strict Synth Mid")
      expect(c.auxOutputs.size() == 2 && c.auxOutputs[0] == "Out 1+2" && c.auxOutputs[1] == "Out 5+6",
             (std::string(label) + ": main output in the middle, the outputs around it are the extra ones").c_str());
  host::registerScanResult(r);
}

// One class through the instrument path the engine uses: prepare, notes, odd blocks, a sample
// rate change, the extra outputs, and the end of the instance.
void exercise(const std::string& type, GridlineStrictProbe* p, const char* label) {
  const std::string L0 = label;
  if (!p) {
    expect(false, (L0 + ": probe reachable").c_str());
    return;
  }
  p->reset();
  auto g = createGenerator(type);
  expect(g != nullptr, (L0 + ": instantiates " + host::lastError()).c_str());
  if (!g) return;
  PluginState st = defaultPluginState(type);
  g->prepareHost(st, 48000);
  expect(p->violations == 0 && p->sampleRate == 48000 && p->setupMaxBlock >= 512,
         (L0 + ": set up at 48 kHz before activation, nothing out of order (" + violations(p->violations) + ")").c_str());
  expect(g->latencySamples() == kLatency, (L0 + ": its latency reaches the engine").c_str());
  expect(g->auxOutputCount() == 2, (L0 + ": both extra outputs offered").c_str());

  // A note at once: silent while the plugin boots, then the note (after its latency).
  std::vector<float> L;
  std::vector<std::vector<float>> aux;
  g->noteOn(1, 69.0, 1.0f, 0);
  render(*g, st, L, aux, 8192, 256);
  const int first = firstSound(L);
  expect(first >= kWarmup && first < kWarmup + 64 && peak(L) > 0.4f,
         (L0 + ": sound after the plugin's boot, from the main output (first at " + std::to_string(first) + ")").c_str());
  expect(aux.size() == 2 && std::fabs(peak(aux[0]) - 0.25f) < 0.02f && std::fabs(peak(aux[1]) - 0.125f) < 0.02f,
         (L0 + ": the extra outputs carry their own signals").c_str());
  expect(p->violations == 0 && p->notesOn == 1, (L0 + ": the note arrived, nothing out of order (" + violations(p->violations) + ")").c_str());
  g->noteOff(1, 0);
  render(*g, st, L, aux, 1024, 256);

  // Blocks of every size: 1 frame, odd sizes, the engine's largest; note events split blocks.
  L.clear();
  for (int block : {1, 7, 333, AudioEngine::kMaxBlock}) {
    g->noteOn(2, 72.0, 1.0f, 3);
    render(*g, st, L, aux, std::max(600, block * 2), block);
    g->noteOff(2, 5);
    render(*g, st, L, aux, std::max(300, block), block);
  }
  expect(p->violations == 0 && p->minBlock >= 1 && p->maxBlock <= p->setupMaxBlock && peak(L) > 0.4f,
         (L0 + ": every block size plays, none longer than set up (" + violations(p->violations) + ")").c_str());

  // Another sample rate: deactivated, set up again, active again.
  const int starts = p->starts;
  g->prepareHost(st, 44100);
  L.clear();
  g->noteOn(3, 69.0, 1.0f, 0);
  render(*g, st, L, aux, 8192, 512);
  expect(p->sampleRate == 44100 && p->starts > starts && peak(L) > 0.4f && p->violations == 0,
         (L0 + ": a new sample rate re-activates in order and plays (" + violations(p->violations) + ")").c_str());
  g.reset();
  expect(p->violations == 0, (L0 + ": the instance ends in order (" + violations(p->violations) + ")").c_str());
}

// The same in a song render (the engine's own blocks, the extra outputs routed).
void renderSongWith(const std::string& type, GridlineStrictProbe* p, const char* label) {
  if (!p) return;
  p->reset();
  Document d(makeEmptySong());
  d.setParam(songParam(SongParam::Tempo), 120.0, false);
  ObjectId pat = d.song().patterns[0].id;
  ObjectId ch = d.addChannel("strict", type);
  d.addNote(pat, ch, {0, 192, 69, 127});
  d.addClip(pat, 0, 0);
  RenderOptions o;
  o.blockSize = 256;
  AudioBuffer out = renderSong(d.song(), o);
  expect(out.peak() > 0.1f && p->violations == 0,
         (std::string(label) + ": a song with it renders with sound (" + violations(p->violations) + ")").c_str());
}

// The probe catches a host that gets it wrong (so the checks above mean something): activated
// before setup, processed before setProcessing, buffers missing for an active bus.
void wrongHost(GridlineStrictProbe* p) {
  if (!p) return;
  p->reset();
  auto mod = vst3::Module::shared(kVst3Bundle, nullptr);
  auto comp = mod ? mod->create<Steinberg::Vst::IComponent>(kVst3Strict.substr(5)) : vst3::Ptr<Steinberg::Vst::IComponent>();
  auto proc = comp ? vst3::Ptr<Steinberg::Vst::IAudioProcessor>::query<Steinberg::Vst::IAudioProcessor>(comp.get())
                   : vst3::Ptr<Steinberg::Vst::IAudioProcessor>();
  expect(comp && proc, "strict probe check: the raw component loads");
  if (!comp || !proc) return;
  comp->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);
  comp->setActive(true);  // no setupProcessing first
  Steinberg::Vst::ProcessData d{};
  d.numSamples = 64;
  d.numOutputs = 0;  // the active main bus has no buffers
  proc->process(d);  // and setProcessing(true) never came
  const uint32_t want = kStrictActiveWithoutSetup | kStrictProcessNotStarted | kStrictLayout | kStrictBlockTooLong;
  expect((p->violations & want) == want, ("strict probe check: a host out of order is caught (" + violations(p->violations) + ")").c_str());
  comp->setActive(false);
  comp->terminate();
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("strict");
  scan("vst3", kVst3Bundle, "VST3");
  scan("clap", kClapBundle, "CLAP");
  GridlineStrictProbe* v3 = probeIn(vst3::bundleBinaryPath(kVst3Bundle));
  GridlineStrictProbe* cp = probeIn(clapBinary());
  exercise(kVst3Strict, v3, "VST3 strict (main on bus 0)");
  exercise(kVst3Mid, v3, "VST3 strict (main on bus 1)");
  exercise(kClapStrict, cp, "CLAP strict (MIDI notes only)");
  exercise(kClapMid, cp, "CLAP strict (main on port 1)");
  renderSongWith(kVst3Mid, v3, "VST3 strict");
  renderSongWith(kClapStrict, cp, "CLAP strict");
  wrongHost(v3);
  return finish();
}
