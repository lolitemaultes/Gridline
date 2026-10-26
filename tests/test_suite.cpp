// Gridline tests — the Gridline Suite (J-131): Gridline's own instruments and
// effects built as standalone plugins, loaded through Gridline's own plugin
// host (CLAP, VST3 and, on macOS, Audio Unit) the way any other host loads
// them. Checks the identity scheme (stable, unique, no collision with the
// Gridline / Gridline FX plugins), then for every member: scan (name, vendor,
// category, buses, parameters), instantiation, sound (a note for instruments,
// finite and plausible output for effects), host automation, Mix, sidechain,
// state save / restore and its compatibility with Gridline's own plugin
// state, and a few editors opened offscreen.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/PluginInfo.h"
#include "core/PluginSuite.h"
#include "core/Project.h"
#include "engine/AudioEngine.h"
#include "engine/Effects.h"
#include "engine/Generators.h"
#include "engine/Program.h"
#include "host/Backend.h"
#include "host/EditorLayout.h"
#include "host/HostedPlugins.h"
#include "host/PluginScan.h"
#include "host/sandbox/Sandbox.h"
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include "host/Dylib.h"
#endif

namespace fs = std::filesystem;
using namespace gl;

namespace {

const std::string kClap = GRIDLINE_SUITE_CLAP;
const std::string kVst3 = GRIDLINE_SUITE_VST3;
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;
constexpr uint32_t kMixId = 0x00100000, kBypassId = 0x00100001;

// Members that make no sound from a note in their default state (they play a
// sample or an image the user loads) and effects that output silence or
// nothing like their input by design.
const std::set<std::string> kNeedsMaterial = {"gl.sampler",      "gl.slicer",     "gl.granular", "gl.samplekeys",
                                              "gl.multisampler", "gl.waveprogram", "gl.scratch",  "gl.grainscan",
                                              "gl.splinescratch", "gl.seltmann",  "gl.imagesynth", "gl.pads"};
const std::set<std::string> kSilentFx = {"gl.mute", "gl.vocoder"};
// Effects whose DSP reads the sidechain (SideIo::scL or a sidechain input parameter).
const std::set<std::string> kSidechainFx = {"gl.comp", "gl.limiter", "gl.vocoder", "gl.scduck",
                                            "gl.hall", "gl.tideline", "gl.transplit", "gl.hitloop"};

float peak(const std::vector<float>& v, size_t from = 0) {
  float p = 0;
  for (size_t i = from; i < v.size(); ++i) p = std::max(p, std::fabs(v[i]));
  return p;
}
bool finite(const std::vector<float>& v) {
  return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}

std::string clapData(const std::string& s) { return "clap1:" + host::base64Encode(s); }
std::string vst3Data(const std::string& s) { return "vst3s1:" + host::base64Encode(s) + ":"; }

// The suite state inside a format's PluginState::data.
std::string innerState(const std::string& format, const std::string& data) {
  const size_t a = data.find(':');
  if (a == std::string::npos) return {};
  size_t b = data.find(':', a + 1);
  if (b == std::string::npos) b = data.size();
  std::string out;
  if (!host::base64Decode(data.substr(a + 1, b - a - 1), out)) return {};
#if defined(__APPLE__)
  if (format == "au") {
    CFDataRef d = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(out.data()), static_cast<CFIndex>(out.size()));
    CFPropertyListRef p = CFPropertyListCreateWithData(nullptr, d, kCFPropertyListImmutable, nullptr, nullptr);
    CFRelease(d);
    std::string s;
    if (p && CFGetTypeID(p) == CFDictionaryGetTypeID()) {
      CFTypeRef v = CFDictionaryGetValue(static_cast<CFDictionaryRef>(p), CFSTR(kAUPresetDataKey));
      if (v && CFGetTypeID(v) == CFDataGetTypeID())
        s.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(static_cast<CFDataRef>(v))),
                 static_cast<size_t>(CFDataGetLength(static_cast<CFDataRef>(v))));
    }
    if (p) CFRelease(p);
    return s;
  }
#else
  (void)format;
#endif
  return out;
}

#if defined(__APPLE__)
std::string auData(const std::string& state, const suite::Member& m) {
  CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  auto num = [&](const char* key, SInt32 v) {
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
    CFDictionarySetValue(d, k, n);
    CFRelease(n);
    CFRelease(k);
  };
  num(kAUPresetVersionKey, 0);
  num(kAUPresetTypeKey, static_cast<SInt32>(m.auType));
  num(kAUPresetSubtypeKey, static_cast<SInt32>(m.auSubtype));
  num(kAUPresetManufacturerKey, static_cast<SInt32>(suite::kAuManufacturer));
  CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(state.data()), static_cast<CFIndex>(state.size()));
  CFDictionarySetValue(d, CFSTR(kAUPresetDataKey), data);
  CFDictionarySetValue(d, CFSTR(kAUPresetNameKey), CFSTR("Untitled"));
  CFRelease(data);
  CFDataRef bytes = CFPropertyListCreateData(nullptr, d, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
  CFRelease(d);
  std::string out(reinterpret_cast<const char*>(CFDataGetBytePtr(bytes)), static_cast<size_t>(CFDataGetLength(bytes)));
  CFRelease(bytes);
  return "au1:" + host::base64Encode(out);
}

// Registers every unit of "Gridline Suite.component" in this process
// (nothing is installed): Gridline's AU host then finds them like installed ones.
bool registerAu() {
  const std::string bin = std::string(GRIDLINE_SUITE_AU) + "/Contents/MacOS/Gridline Suite";
  void* h = host::dl::open(bin, nullptr, true);
  auto factory = h ? host::toFunction<AudioComponentFactoryFunction>(host::dl::sym(h, "GridlineSuiteAUFactory")) : nullptr;
  if (!factory) return false;
  bool ok = true;
  for (const suite::Member& m : suite::members("gridline")) {
    const AudioComponentDescription d{m.auType, m.auSubtype, suite::kAuManufacturer, 0, 0};
    CFStringRef name = CFStringCreateWithCString(nullptr, ("Gridline: " + m.name).c_str(), kCFStringEncodingUTF8);
    ok = AudioComponentRegister(&d, name, 0x00000700, factory) != nullptr && ok;
    CFRelease(name);
  }
  return ok;
}
#endif

std::string auKey(const suite::Member& m) {
  return suite::fourcc(m.auType) + "/" + suite::fourcc(m.auSubtype) + "/" + suite::fourcc(suite::kAuManufacturer);
}
std::string vst3Key(const suite::Member& m) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08X%08X%08X%08X", m.vst3Uid[0], m.vst3Uid[1], m.vst3Uid[2], m.vst3Uid[3]);
  return buf;
}

// ---------------------------------------------------------------- identities

void testIdentities() {
  const auto& ms = suite::members("gridline");
  expect(ms.size() >= 100, "suite: Gridline Suite has its members");
  std::set<std::string> clap, vst3, au, names;
  int inst = 0, fx = 0;
  bool shapes = true, stable = true, sidechain = true;
  for (const auto& m : ms) {
    clap.insert(m.clapId);
    vst3.insert(vst3Key(m));
    au.insert(auKey(m));
    names.insert(m.name);
    (m.instrument ? inst : fx)++;
    const std::string stem = m.type.substr(3);
    const std::string sub = suite::fourcc(m.auSubtype);
    shapes = shapes && m.type.rfind("gl.", 0) == 0 && m.clapId == "org.gridline.suite." + stem && m.vst3Uid[0] == suite::kVst3Uid0 &&
             m.vst3Uid[1] == suite::kVst3Uid1 && m.vst3Uid[2] == suite::kVst3Uid2 && sub.size() == 4 && sub[0] >= 'a' && sub[0] <= 'z' &&
             std::all_of(sub.begin(), sub.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
    stable = stable && m.vst3Uid[3] == suite::fnv1a32(m.type);
    const uint32_t want = m.instrument ? 0x61756D75u /* aumu */ : m.midiInput ? 0x61756D66u /* aumf */ : 0x61756678u /* aufx */;
    shapes = shapes && m.auType == want;
    sidechain = sidechain && (m.sidechain == (kSidechainFx.count(m.type) > 0));
    if (m.sidechain != (kSidechainFx.count(m.type) > 0)) std::printf("# sidechain mismatch: %s\n", m.type.c_str());
  }
  std::printf("# suite: %d instruments, %d effects\n", inst, fx);
  expect(clap.size() == ms.size() && vst3.size() == ms.size() && au.size() == ms.size() && names.size() == ms.size(),
         "suite: CLAP ids, VST3 class ids, AU codes and names are unique");
  expect(shapes, "suite: ids follow the documented scheme (org.gridline.suite.<name>, GRIDLINESUIT + hash, GrLn lower-case subtype)");
  expect(stable, "suite: the VST3 / AU identity is the FNV-1a hash of the type alone");
  expect(sidechain, "suite: sidechain members are the effects whose DSP reads the sidechain");
  // Gridline's own plugins: CLAP org.gridline.gridline(-fx), VST3 GRIDLINEPLUG 1/2, AU GrIn / GrFx.
  bool clash = false;
  for (const auto& m : ms) {
    clash = clash || m.clapId == "org.gridline.gridline" || m.clapId == "org.gridline.gridline-fx" || m.vst3Uid[2] == 0x504C5547u ||
            m.auSubtype == 0x4772496Eu || m.auSubtype == 0x47724678u;
  }
  expect(!clash, "suite: no identity collides with Gridline / Gridline FX");
  // Lookups.
  bool lookups = true;
  for (const auto& m : ms)
    lookups = lookups && suite::byClapId(m.clapId) == &m && suite::byVst3Uid(m.vst3Uid) == &m && suite::byAu(m.auType, m.auSubtype) == &m &&
              suite::findMember(m.type) == &m;
  expect(lookups, "suite: lookups by CLAP id, VST3 uid, AU code and type");
  // Every built-in plugin is either a member or excluded with a reason.
  bool accounted = true;
  for (const PluginInfo& p : pluginCatalog()) {
    if (p.type.rfind("gl.", 0) != 0) continue;
    const bool member = suite::findMember(p.type) != nullptr;
    accounted = accounted && (member != !suite::exclusionReason(p).empty());
  }
  expect(accounted, "suite: every built-in plugin is a member or excluded with a reason");
}

// ---------------------------------------------------------------- one format

struct FormatRun {
  std::string format;
  std::string (*key)(const suite::Member&);
  std::string (*wrap)(const std::string&, const suite::Member&);
};

std::string clapKey(const suite::Member& m) { return m.clapId; }
std::string clapWrap(const std::string& s, const suite::Member&) { return clapData(s); }
std::string vst3Wrap(const std::string& s, const suite::Member&) { return vst3Data(s); }

const host::ScannedClass* findClass(const std::vector<host::ScanResult>& scans, const std::string& uid) {
  for (const auto& r : scans)
    for (const auto& c : r.classes)
      if (c.uid == uid) return &c;
  return nullptr;
}

int hostedIndex(host::HostedInstance* hi, uint32_t id) {
  if (!hi) return -1;
  const auto& ps = hi->hostedClass().params;
  for (size_t k = 0; k < ps.size(); ++k)
    if (ps[k].id == id) return static_cast<int>(k);
  return -1;
}

struct Inst {
  std::unique_ptr<Generator> gen;
  std::unique_ptr<Effect> fx;
  PluginState st;
  host::HostedInstance* hi = nullptr;
};

Inst make(const std::string& type, bool instrument, const std::string& data = "") {
  Inst i;
  i.st = defaultPluginState(type);
  i.st.data = data;
  if (instrument) {
    i.gen = createGenerator(type);
    if (i.gen) i.gen->prepareHost(i.st, kSr);
    i.hi = i.gen ? host::hostedInstance(i.gen.get()) : nullptr;
  } else {
    i.fx = createEffect(type);
    if (i.fx) i.fx->prepareHost(i.st, kSr);
    i.hi = i.fx ? host::hostedInstance(i.fx.get()) : nullptr;
  }
  return i;
}

ProcessContext context() {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = 120;
  c.ppq = 96;
  return c;
}

std::vector<float> renderNote(Inst& i, int frames) {
  ProcessContext ctx = context();
  ChannelAux none;
  std::vector<float> L, R;
  // Hosts run their main thread between loading a plugin and playing it: a
  // few idle calls (pack instruments such as Plasma build their tables there).
  for (int k = 0; k < 20; ++k) {
    std::vector<float> l(kBlock, 0.f), r(kBlock, 0.f);
    i.gen->process(i.st, none, ctx, l.data(), r.data(), kBlock);
    host::UiUpdate u;
    i.hi->pumpUi(u);
  }
  i.gen->noteOn(1, 60, 1.0f, 0);
  for (int pos = 0; pos < frames; pos += kBlock) {
    std::vector<float> l(kBlock, 0.f), r(kBlock, 0.f);
    if (pos == frames / 2) i.gen->noteOff(1, 0);
    i.gen->process(i.st, none, ctx, l.data(), r.data(), kBlock);
    L.insert(L.end(), l.begin(), l.end());
    R.insert(R.end(), r.begin(), r.end());
  }
  L.insert(L.end(), R.begin(), R.end());
  return L;
}

// The same note through Gridline's own engine (the built-in generator in this process).
float nativePeak(const std::string& type) {
  Song s = makeEmptySong();
  Document doc(s);
  const ObjectId ch = doc.addChannel("Voice", type);
  AudioEngine eng(kSr);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  eng.setProgram(b.build(doc.song()));
  eng.noteOn(ch, 60, 1.0f);
  std::vector<float> L(16384), R(16384);
  for (int pos = 0; pos < 16384; pos += kBlock) eng.process(L.data() + pos, R.data() + pos, kBlock);
  const float p = std::max(peak(L), peak(R));
  std::printf("# %s in Gridline: peak %g\n", type.c_str(), static_cast<double>(p));
  return p;
}

std::vector<float> renderFx(Inst& i, int frames, float scGain = 0.f) {
  ProcessContext ctx = context();
  std::vector<float> L(static_cast<size_t>(frames)), R(static_cast<size_t>(frames)), sc(kBlock);
  for (int k = 0; k < frames; ++k) {
    L[static_cast<size_t>(k)] = 0.5f * std::sin(static_cast<float>(k) * 0.03f);
    R[static_cast<size_t>(k)] = 0.5f * std::sin(static_cast<float>(k) * 0.031f);
  }
  for (int pos = 0; pos < frames; pos += kBlock) {
    if (scGain > 0) {
      for (int k = 0; k < kBlock; ++k) sc[static_cast<size_t>(k)] = scGain * std::sin(static_cast<float>(pos + k) * 0.007f);
      SideIo io;
      io.scL = io.scR = sc.data();
      i.fx->setSideIo(io);
    }
    i.fx->processAux(i.st, ChannelAux{}, ctx, L.data() + pos, R.data() + pos, kBlock);
  }
  L.insert(L.end(), R.begin(), R.end());
  return L;
}

void testFormat(const FormatRun& fr, const std::string& bundle, bool full) {
  const auto& ms = suite::members("gridline");
  const std::string tag = "suite " + fr.format + ": ";
  auto name = [&](const std::string& s) { return tag + s; };
  const auto t0 = std::chrono::steady_clock::now();

  std::vector<host::ScanResult> scans;
  if (fr.format == "au") {
    for (const auto& m : ms) scans.push_back(host::findFormat("au")->scanInProcess(auKey(m)));
  } else {
    scans.push_back(host::findFormat(fr.format)->scanInProcess(bundle));
  }
  size_t classes = 0;
  bool scanOk = true;
  for (const auto& r : scans) {
    scanOk = scanOk && r.ok;
    classes += r.classes.size();
    if (!r.ok) std::printf("# %s scan error: %s\n", fr.format.c_str(), r.error.c_str());
  }
  expect(scanOk && classes == ms.size(), name("scan finds every member").c_str());
  for (const auto& r : scans) host::registerScanResult(r);

  int described = 0, silentInst = 0, badFx = 0, roundTrips = 0, compatible = 0, automated = 0, instantiated = 0;
  std::vector<std::string> problems;
  for (const auto& m : ms) {
    const std::string key = fr.key(m);
    const host::ScannedClass* c = findClass(scans, key);
    if (!c) {
      problems.push_back(m.type + " not scanned");
      continue;
    }
    // Description: name, vendor, buses, parameters (plugin's own, + Mix and Bypass for effects).
    const size_t nParams = m.info->params.size();
    bool desc = c->name == m.name && c->vendor == "Gridline" && c->instrument == m.instrument && c->outputChannels == 2;
    if (m.instrument)
      desc = desc && c->eventInput;
    else
      desc = desc && c->inputChannels == 2 && (c->sidechainChannels == 2) == m.sidechain && c->eventInput == m.midiInput;
    // Effects: Mix and Bypass (Gridline's VST3 scanner leaves the bypass
    // parameter out of the list, as hosts show it as their own switch).
    desc = desc && c->params.size() == nParams + (m.instrument ? 0 : fr.format == "vst3" ? 1 : 2);
    for (size_t k = 0; desc && k < nParams; ++k) desc = c->params[k].id == k && c->params[k].title == m.info->params[k].name;
    if (fr.format == "clap" && !m.instrument) {
      const bool hasBypass = std::any_of(c->params.begin(), c->params.end(), [](const host::ScannedParam& p) {
        return p.id == kBypassId && (p.flags & host::kParamBypass);
      });
      desc = desc && hasBypass;
    }
    if (desc)
      ++described;
    else
      problems.push_back(m.type + " description (name '" + c->name + "', vendor '" + c->vendor + "', params " + std::to_string(c->params.size()) +
                         ", in " + std::to_string(c->inputChannels) + ", sidechain " + std::to_string(c->sidechainChannels) + ", events " +
                         std::to_string(c->eventInput) + ")");
    if (!full && &m != &ms.front() && m.type != "gl.comp" && m.type != "gl.scduck" && m.type != "gl.reverb" && m.type != "gl.fm") continue;

    if (const char* only = std::getenv("GRIDLINE_SUITE_ONLY"))  // debugging one member
      if (m.type != only) continue;
    const std::string pluginType = fr.format + ":" + key;
    Inst a = make(pluginType, m.instrument);
    if (!a.hi) {
      problems.push_back(m.type + " does not instantiate");
      continue;
    }
    if (!m.instrument && fr.format != "au" && !a.hi->backend()->hasBypass()) problems.push_back(m.type + " has no bypass parameter");
    else ++instantiated;
    // Sound.
    std::vector<float> out = m.instrument ? renderNote(a, 16384) : renderFx(a, 8192);
    const float p = peak(out);
    if (m.instrument) {
      // A member silent in its default state must be silent in Gridline too.
      const bool ok = finite(out) && (p > 1e-4f || kNeedsMaterial.count(m.type) || nativePeak(m.type) <= 1e-4f);
      if (!ok) ++silentInst, problems.push_back(m.type + " note: peak " + std::to_string(p));
    } else {
      const bool ok = finite(out) && p < 16.f && (p > 1e-3f || kSilentFx.count(m.type));
      if (!ok) ++badFx, problems.push_back(m.type + " output: peak " + std::to_string(p));
    }
    if (full) std::printf("# %-6s %-18s peak %.4f\n", fr.format.c_str(), m.type.c_str(), static_cast<double>(p));

    // State: capture, restore into a new instance, capture again (identical).
    std::string d1, d2;
    if (a.hi->captureState(d1) && !d1.empty()) {
      Inst b = make(pluginType, m.instrument, d1);
      if (b.hi && b.hi->captureState(d2) && d1 == d2) ++roundTrips;
      else problems.push_back(m.type + " state round trip");
    } else {
      problems.push_back(m.type + " state capture");
    }
    // Compatibility with Gridline's own state: a PluginState as Gridline saves
    // it (a parameter changed) loads into the suite plugin and comes back in
    // the suite state's "plugin" part.
    if (nParams > 0) {
      PluginState gs = defaultPluginState(m.type);
      size_t k = 0;
      while (k < nParams && m.info->params[k].kind != ParamKind::Continuous) ++k;
      if (k == nParams) k = 0;
      const ParamDesc& pd = m.info->params[k];
      const float want = static_cast<float>(pd.min + 0.25 * (pd.max - pd.min));
      if (k < gs.params.size()) gs.params[k] = pd.kind == ParamKind::Continuous ? want : static_cast<float>(pd.min);
      const std::string data = fr.wrap(jsonStringify(pluginStateToJson(gs), 0), m);
      Inst g = make(pluginType, m.instrument, data);
      std::string back;
      Json j;
      if (g.hi && g.hi->captureState(back) && jsonParse(innerState(fr.format, back), j) && j.find("plugin")) {
        const PluginState rs = pluginStateFromJson(*j.find("plugin"));
        const Json* f = j.find("format");
        if (f && f->str == "gridline-suite" && rs.type == m.type && k < rs.params.size() && std::fabs(rs.params[k] - gs.params[k]) < 1e-4f)
          ++compatible;
        else
          problems.push_back(m.type + " Gridline state not taken");
      } else {
        problems.push_back(m.type + " Gridline state");
      }
    } else {
      ++compatible;
    }
    // Host automation: the first continuous parameter follows the host.
    if (nParams > 0) {
      size_t k = 0;
      while (k < nParams && m.info->params[k].kind != ParamKind::Continuous) ++k;
      const int hk = k < nParams ? hostedIndex(a.hi, static_cast<uint32_t>(k)) : -1;
      if (hk >= 0 && static_cast<size_t>(hk) < a.st.params.size()) {
        a.st.params[static_cast<size_t>(hk)] = 0.123f;
        (void)(m.instrument ? renderNote(a, 2048) : renderFx(a, 2048));
        host::UiUpdate u;
        a.hi->pumpUi(u);
        const double v = a.hi->backend()->paramNormalized(static_cast<uint32_t>(k));
        if (std::fabs(v - 0.123) < 2e-3) ++automated;
        else problems.push_back(m.type + " automation: " + std::to_string(v));
      } else {
        ++automated;
      }
    } else {
      ++automated;
    }
  }
  const size_t expectN = full ? ms.size() : 5;
  expect(described == static_cast<int>(ms.size()), name("every member: name, vendor Gridline, buses and named parameters").c_str());
  expect(instantiated == static_cast<int>(expectN), name("members instantiate").c_str());
  expect(silentInst == 0, name("instruments: a note makes sound (sample players: finite)").c_str());
  expect(badFx == 0, name("effects: finite, plausible output").c_str());
  expect(roundTrips == static_cast<int>(expectN), name("state round trip: identical").c_str());
  expect(compatible == static_cast<int>(expectN), name("Gridline's plugin state loads into the suite plugin").c_str());
  expect(automated == static_cast<int>(expectN), name("host automation reaches the plugin's parameter").c_str());
  for (const auto& s : problems) std::printf("# %s problem: %s\n", fr.format.c_str(), s.c_str());
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("# %s: %zu members, %.1f s\n", fr.format.c_str(), expectN, secs);
}

// Mix and sidechain through the host (CLAP).
void testMixAndSidechain() {
  const suite::Member* duck = suite::findMember("gl.scduck");
  const suite::Member* dist = suite::findMember("gl.dist");
  expect(duck && dist && duck->sidechain && !dist->sidechain, "suite: ducker has a sidechain, distortion has none");
  if (!duck || !dist) return;
  {
    Inst a = make("clap:" + dist->clapId, false);
    Inst b = make("clap:" + dist->clapId, false);
    const int mix = hostedIndex(b.hi, kMixId);
    expect(mix >= 0, "suite: effects expose Mix");
    if (mix >= 0) b.st.params[static_cast<size_t>(mix)] = 0.0f;
    std::vector<float> wet = renderFx(a, 8192), dry = renderFx(b, 8192);
    float err = 0;
    for (int k = 0; k < 8192; ++k) err = std::max(err, std::fabs(dry[static_cast<size_t>(k)] - 0.5f * std::sin(static_cast<float>(k) * 0.03f)));
    std::printf("# mix 0: max difference from the input %g\n", static_cast<double>(err));
    expect(err < 1e-3f && peak(wet) > 0.01f, "suite: Mix 0 passes the input through unchanged");
  }
  {
    Inst a = make("clap:" + duck->clapId, false);
    Inst b = make("clap:" + duck->clapId, false);
    const std::vector<float> plain = renderFx(a, 16384), ducked = renderFx(b, 16384, 0.9f);
    const float pp = peak(plain, 8192), pd = peak(ducked, 8192);
    std::printf("# sidechain: peak without %g, with a loud sidechain %g\n", static_cast<double>(pp), static_cast<double>(pd));
    expect(pd < pp * 0.9f, "suite: the sidechain input reaches the ducker");
  }
}

// Editors open offscreen (Gridline's own editor code, sized from the plugin).
void testEditors(const std::string& format, const std::vector<std::string>& keys) {
  for (const std::string& key : keys) {
    const bool instrument = key.find("aumu") != std::string::npos || (format == "clap" && suite::byClapId(key) && suite::byClapId(key)->instrument);
    Inst i = make(format + ":" + key, instrument);
    std::unique_ptr<host::Editor> e = i.hi ? i.hi->backend()->createEditor() : nullptr;
    expect(e != nullptr, ("suite " + format + ": " + key + " has an editor").c_str());
    void* parent = e ? host::offscreenEditorParent(1280, 900) : nullptr;
    if (e && !parent) {  // no offscreen parent window on this platform (macOS only)
      std::printf("# %s editor %s: no offscreen parent here, not opened\n", format.c_str(), key.c_str());
      continue;
    }
    int resizes = 0;
    if (e) {  // like a host: the window takes the size the editor asks for
      host::Editor* ep = e.get();
      e->requestResize = [ep, &resizes](int rw, int rh) {
        ++resizes;
        ep->setSize(rw, rh);
        return true;
      };
    }
    const bool attached = parent && e->attach(parent);
    int w = 0, h = 0;
    for (int k = 0, n = std::getenv("GRIDLINE_PLUGIN_SHOT") ? 200 : 40; k < n; ++k) {  // captures: the view saves its 60th frame
      if (e) e->idle();
      host::pumpOffscreenEvents(0.01);
    }
    const bool sized = e && e->size(w, h) && w >= 200 && h >= 120;
    std::printf("# %s editor %s: %dx%d resizable %d, %d resize requests\n", format.c_str(), key.c_str(), w, h, e ? int(e->canResize()) : 0, resizes);
    expect(attached && sized && e->canResize(), ("suite " + format + ": editor of " + key + " opens and resizes").c_str());
    e.reset();
    host::closeOffscreenEditorParent();
    host::pumpOffscreenEvents(0.02);
  }
}

}  // namespace

int main(int argc, char** argv) {
  test::hermeticTestEnv("suite");
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps every line printed so far
  // "--quick": every member is scanned, a few are played (the full run plays all of them in every format).
  const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
  testIdentities();
  testFormat({"clap", clapKey, clapWrap}, kClap, true);
  testFormat({"vst3", vst3Key, vst3Wrap}, kVst3, !quick);
  testMixAndSidechain();
  testEditors("clap", {"org.gridline.suite.synth", "org.gridline.suite.comp", "org.gridline.suite.reverb", "org.gridline.suite.plasma", "org.gridline.suite.eq"});
#if defined(__APPLE__)
  expect(registerAu(), "suite au: Gridline Suite.component's units registered in this process (not installed)");
  testFormat({"au", auKey, auData}, GRIDLINE_SUITE_AU, !quick);
  testEditors("au", {"aumu/kcxv/GrLn", "aufx/pyjy/GrLn"});
#endif
  return finish();
}
