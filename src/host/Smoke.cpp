#include "Smoke.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "HostedPlugins.h"
#include "PluginPrefs.h"

namespace gl::host {

namespace {
struct Stats {
  float peak = 0;
  bool finite = true;
};
void measure(const std::vector<float>& v, Stats& s) {
  for (float x : v) {
    if (!std::isfinite(x)) s.finite = false;
    s.peak = std::max(s.peak, std::fabs(x));
  }
}
}  // namespace

void smokeMain(const std::string& format, const std::string& entry, bool sandbox) {
  const Format* fmt = findFormat(format);
  if (!fmt) {
    std::printf("unknown plugin format \"%s\"\n", format.c_str());
    std::_Exit(1);
  }
  auto t0 = std::chrono::steady_clock::now();
  ScanResult r = fmt->scanInProcess(entry);
  double scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("%s  %s\nscan    %s (%.0f ms)%s%s\n", fmt->label, entry.c_str(), r.ok ? "ok" : "FAILED", scanMs, r.error.empty() ? "" : "  ",
              r.error.c_str());
  if (!r.ok) std::_Exit(1);
  registerScanResult(r);
  int failed = 0;
  const double sr = 48000;
  const int block = 512;
  for (const auto& c : r.classes) {
    const std::string type = r.format + ":" + c.uid;
    if (sandbox) setRunInSandbox(type, true);
    auto cls = hostedClass(type);
    std::printf("\nclass   %s  [%s]  %s  vendor \"%s\" v%s%s\n        type %s\n", c.name.c_str(), c.subCategories.c_str(),
                c.instrument ? "instrument" : "effect", c.vendor.c_str(), c.version.c_str(), sandbox ? "  (sandboxed)" : "", type.c_str());
    std::printf("        io: in %d, out %d, sidechain %d, extra outs %zu, events %s, latency %d, params %zu (%zu exposed), programs %zu\n",
                c.inputChannels, c.outputChannels, c.sidechainChannels, c.auxOutputs.size(), c.eventInput ? "yes" : "no", c.latency,
                c.params.size(), cls ? cls->params.size() : 0, c.programs.size());
    PluginState st = defaultPluginState(type);
    ProcessContext ctx;
    ctx.sampleRate = sr;
    ctx.tempo = 120;
    ctx.playing = true;
    std::vector<float> L(block), R(block);
    Stats silent, sound, bent;
    std::string data, data2;
    bool ok = true;
    HostedInstance* hi = nullptr;
    auto t1 = std::chrono::steady_clock::now();
    std::unique_ptr<Generator> g, g2;
    std::unique_ptr<Effect> fx, fx2;
    if (c.instrument) {
      g = createGenerator(type);
      if (!g) {
        std::printf("        FAILED to instantiate: %s\n", lastError().c_str());
        ++failed;
        continue;
      }
      g->prepareHost(st, sr);
      ChannelAux aux;
      for (int b = 0; b < 20; ++b) {  // silence
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        g->process(st, aux, ctx, L.data(), R.data(), block);
        measure(L, silent);
        measure(R, silent);
      }
      g->noteOn(1, 60, 0.9f, 17);
      for (int b = 0; b < 94; ++b) {  // ~1 s, pitch bend + mod wheel at ~0.25 s, note off after ~0.5 s
        if (b == 23) {
          g->midi(0xE0, 0x00, 0x60, 0);
          g->midi(0xB0, 1, 100, 0);
        }
        if (b == 47) g->noteOff(1, 3);
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        g->process(st, aux, ctx, L.data(), R.data(), block);
        measure(L, b < 23 ? sound : bent);
        measure(R, b < 23 ? sound : bent);
        ctx.tick += block / sr * 2.0 * ctx.ppq;
      }
      g->midi(0xE0, 0x00, 0x40, 0);
      hi = hostedInstance(g.get());
    } else {
      fx = createEffect(type);
      if (!fx) {
        std::printf("        FAILED to instantiate: %s\n", lastError().c_str());
        ++failed;
        continue;
      }
      fx->prepareHost(st, sr);
      for (int b = 0; b < 20; ++b) {
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        fx->processAux(st, ChannelAux{}, ctx, L.data(), R.data(), block);
        measure(L, silent);
        measure(R, silent);
      }
      double ph = 0;
      for (int b = 0; b < 94; ++b) {
        for (int i = 0; i < block; ++i) {
          L[static_cast<size_t>(i)] = R[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(ph));
          ph += 2 * M_PI * 220 / sr;
        }
        fx->processAux(st, ChannelAux{}, ctx, L.data(), R.data(), block);
        measure(L, sound);
        measure(R, sound);
      }
      hi = hostedInstance(fx.get());
    }
    UiUpdate u;
    hi->pumpUi(u);
    if (hi->crashed()) {
      std::printf("        CRASHED: %s\n", hi->crashReason().c_str());
      ++failed;
      continue;
    }
    ok = hi->captureState(data);
    PluginState st2 = st;
    st2.data = data;
    HostedInstance* h2 = nullptr;
    if (c.instrument) {
      g2 = createGenerator(type);
      if (g2) {
        g2->prepareHost(st2, sr);
        h2 = hostedInstance(g2.get());
      }
    } else {
      fx2 = createEffect(type);
      if (fx2) {
        fx2->prepareHost(st2, sr);
        h2 = hostedInstance(fx2.get());
      }
    }
    if (h2) h2->captureState(data2);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    const bool same = !data.empty() && data == data2;
    std::printf("        silence: peak %.6f%s · %s: peak %.4f%s", silent.peak, silent.finite ? "" : " (NON-FINITE)",
                c.instrument ? "note C4" : "220 Hz tone", sound.peak, sound.finite ? "" : " (NON-FINITE)");
    if (c.instrument) std::printf(" · after pitch bend + CC1: peak %.4f%s", bent.peak, bent.finite ? "" : " (NON-FINITE)");
    std::printf("\n        state: %zu B (%s…), restore → %s  (%.0f ms)\n", data.size(), data.substr(0, data.find(':')).c_str(),
                same ? "identical" : (h2 ? "differs" : "NOT RESTORED"), ms);
    if (hi->programCount() > 0) {
      std::printf("        programs: %d (", hi->programCount());
      for (int k = 0; k < std::min(4, hi->programCount()); ++k) std::printf("%s\"%s\"", k ? ", " : "", hi->programName(k).c_str());
      std::printf("%s)", hi->programCount() > 4 ? ", …" : "");
      const bool sel = hi->selectProgram(std::min(1, hi->programCount() - 1));
      UiUpdate u2;
      hi->pumpUi(u2);
      std::printf(" select #2 → %s\n", sel ? (u2.readback ? "ok, parameters read back" : "ok") : "FAILED");
    }
    if (!ok || !silent.finite || !sound.finite || !bent.finite) ++failed;
  }
  std::printf("\n%s\n", failed ? "SOME CLASSES FAILED" : "all classes passed");
  std::fflush(stdout);
  std::_Exit(failed ? 1 : 0);  // skip plugin unload / static destructors
}

}  // namespace gl::host
