#include "Vst3Scan.h"
#include <algorithm>
#include "../Smoke.h"
#include "Vst3Plugin.h"

namespace gl::vst3 {

int32_t hostParamFlags(int32_t f) {
  int32_t h = 0;
  if (f & sv::ParameterInfo::kCanAutomate) h |= host::kParamAutomatable;
  if (f & sv::ParameterInfo::kIsReadOnly) h |= host::kParamReadOnly;
  if (f & sv::ParameterInfo::kIsHidden) h |= host::kParamHidden;
  if (f & sv::ParameterInfo::kIsBypass) h |= host::kParamBypass;
  if (f & sv::ParameterInfo::kIsProgramChange) h |= host::kParamProgramChange;
  return h;
}

int64_t bundleMtime(const std::string& bundle) {
  return std::max(host::pathMtime(bundle), host::pathMtime(bundleBinaryPath(bundle)));
}

ScanResult scanBundleInProcess(const std::string& bundle) {
  ScanResult r;
  r.format = "vst3";
  r.bundle = bundle;
  r.mtime = bundleMtime(bundle);
  std::string err;
  std::unique_ptr<Module> mod = Module::load(bundle, &err);
  if (!mod) {
    r.error = err;
    return r;
  }
  std::shared_ptr<Module> m(mod.release());
  for (const ClassInfo& ci : m->classes()) {
    if (!ci.isAudioModule()) continue;
    ScannedClass c;
    c.uid = ci.uid;
    c.name = ci.name;
    c.vendor = ci.vendor.empty() ? m->factoryVendor() : ci.vendor;
    c.version = ci.version;
    c.subCategories = ci.subCategories;
    c.sdkVersion = ci.sdkVersion;
    c.instrument = ci.isInstrument();
    std::string perr;
    if (auto p = Plugin::create(m, ci.uid, &perr)) {
      c.eventInput = p->hasEventInput();
      c.eventOutput = p->hasEventOutput();
      if (p->prepare(48000.0, 512)) {
        c.inputChannels = p->mainInputChannels();
        c.outputChannels = p->mainOutputChannels();
        c.sidechainChannels = p->sidechainChannels();
        c.latency = p->latency();
        for (int b : p->auxOutputBuses()) {
          std::string n = p->outputBusName(b);
          c.auxOutputs.push_back(n.empty() ? "Output " + std::to_string(c.auxOutputs.size() + 2) : n);
        }
      }
      // Instruments without a category: event input and no audio input.
      if (ci.subCategories.empty()) c.instrument = c.eventInput && c.inputChannels == 0;
      c.programs = p->programs();
      // Only parameters a host can use: visible, writable, and automatable
      // when the plugin flags any (wrappers add thousands of MIDI-controller
      // proxies that are not).
      const bool anyAutomatable = std::any_of(p->params().begin(), p->params().end(), [](const ParamInfo& q) { return q.automatable(); });
      for (const ParamInfo& pi : p->params()) {
        if (!pi.exposed() || (anyAutomatable && !pi.automatable())) continue;
        ScannedParam sp;
        sp.id = pi.id;
        sp.title = pi.title;
        sp.units = pi.units;
        sp.stepCount = pi.stepCount;
        sp.def = pi.def;
        sp.flags = hostParamFlags(pi.flags);
        if (pi.stepCount >= 1 && pi.stepCount <= 32)
          for (int k = 0; k <= pi.stepCount; ++k) sp.labels.push_back(p->paramText(pi.id, static_cast<double>(k) / pi.stepCount));
        c.params.push_back(std::move(sp));
      }
    } else {
      r.error += (r.error.empty() ? "" : "; ") + ci.name + ": " + perr;
      continue;
    }
    r.classes.push_back(std::move(c));
  }
  r.ok = !r.classes.empty();
  if (!r.ok && r.error.empty()) r.error = "no audio module classes";
  m->leakOnDestroy();
  return r;
}

ScanResult scanBundleIsolated(const std::string& exe, const std::string& bundle, double timeoutSeconds) {
  return host::scanIsolated(exe, "vst3", bundle, timeoutSeconds);
}

int scanMain(int argc, char** argv) { return host::childMain(argc, argv); }

void smokeMain(const std::string& bundle) { host::smokeMain("vst3", bundle); }

}  // namespace gl::vst3
