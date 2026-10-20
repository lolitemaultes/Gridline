// Gridline — FLP import, second wave: Fruity Keyboard Controller, Fruity Voltage
// Controller and Patcher (instrument version; the effect version shares the
// FL name and is decoded here too).
#include <algorithm>
#include <cmath>
#include "../../packs/modulation/modulation.h"
#include "../../packs/modulation/modulation_dsp.h"
#include "FlpFx2.h"
#include "FlpGen2.h"

namespace gl::flp::gen2 {

namespace mod = gl::packs::modulation;

namespace {

// ------------------------------------------------------- Keyboard Controller
// State (version 2): u32 version, i32 attack, i32 release (smoothing, 0..256),
// f32 value[132] (one per key 0..131, 0..1), i32 zone low, i32 zone high
// (the selected key zone, editor only), i32 0, u8 flag. Older states (presets
// saved by FL 4) have no version word and fewer keys; the 13-byte tail is the same.
constexpr size_t kKcTail = 13;

double kcTime(int32_t v) {  // smoothing 0..256 → seconds (approximate curve)
  double n = std::clamp(v / 256.0, 0.0, 1.0);
  return std::max(0.001, n * n * 2.0);
}

bool decodeKeyboardController(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = stateBlob(evs);
  begin(m, "gl.keyctl");
  if (b.size() < kKcTail + 8) return true;  // empty state: defaults
  size_t keysAt = 0, keysEnd = b.size() - kKcTail;
  int32_t atk = 0, rel = 0;
  if (b.u32(0) == 2 && b.size() >= 12 + kKcTail) {
    atk = b.i32(4), rel = b.i32(8), keysAt = 12;
  } else {
    atk = b.i32(0), rel = b.i32(4), keysAt = 8;
  }
  std::vector<float> keys(128);
  for (int k = 0; k < 128; ++k) keys[static_cast<size_t>(k)] = static_cast<float>(k / 127.0);
  for (size_t k = 0; k < 128 && keysAt + 4 * k + 4 <= keysEnd; ++k) {
    float v = b.f32(keysAt + 4 * k);
    keys[k] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
  }
  m.state.data = mod::formatKeyValues(keys);
  set(m, mod::KcAttack, kcTime(atk));
  set(m, mod::KcRelease, kcTime(rel));
  m.exact = atk == 0 && rel == 0;  // instant smoothing maps 1:1; other times follow an approximate curve
  linkParams(m, {{0, mod::KcAttack, [](double n) { return kcTime(static_cast<int32_t>(n * 256)); }},
                 {1, mod::KcRelease, [](double n) { return kcTime(static_cast<int32_t>(n * 256)); }}},
             {{0, 0, 256}, {1, 0, 256}});
  return true;
}

// -------------------------------------------------------- Voltage Controller
// State: u32 version (1, 2), i32 offset[8] (L/R of main out, out 2, 3, 4;
// 0..65536, 32768 = 0 V), i32 scale[8] (0..65536), u8 source[8], u8 enabled,
// 22 bytes of per-output options (polarity / colour, zero in every supplied
// state), i32 clock resolution (PPQ) at 0x63; version 2 adds i32 pulse length
// (samples) and f64 pitch scaling factor (1 = 1 V/oct, 1.2 = 1.2 V/oct).
// Sources: 0 pitch, 1 gate, 2 trigger, 3 velocity, 4 volume, 5 panning,
// 6 mod X, 7 mod Y, 8 clock, 9 none.
int cvSource(int fl, bool& exact) {
  switch (fl) {
    case 0: return mod::CvsPitch;
    case 1: return mod::CvsGate;
    case 2: return mod::CvsTrigger;
    case 3: return mod::CvsVelocity;
    case 8: return mod::CvsClock;
    case 9: return mod::CvsOff;
    default: exact = false; return mod::CvsVelocity;  // volume / pan / mod X / mod Y: nearest per-note value
  }
}

bool decodeVoltageController(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = stateBlob(evs);
  begin(m, "gl.cvctl");
  if (b.size() < 0x4d) return true;
  bool exact = true;
  set(m, mod::CvLeftOfs, (b.i32(4) - 32768.0) / 32768.0);
  set(m, mod::CvRightOfs, (b.i32(8) - 32768.0) / 32768.0);
  set(m, mod::CvLeftScale, knob(b.i32(0x24)));
  set(m, mod::CvRightScale, knob(b.i32(0x28)));
  set(m, mod::CvLeftSrc, cvSource(b.u8(0x44), exact));
  set(m, mod::CvRightSrc, cvSource(b.u8(0x45), exact));
  set(m, mod::CvEnable, b.u8(0x4c) ? 1 : 0);
  if (b.has(0x63, 4)) {
    static const int ppqs[] = {2, 4, 8, 12, 16, 24, 32, 48, 64, 96};
    int ppq = b.i32(0x63), best = 5;
    for (int i = 0; i < 10; ++i)
      if (std::llabs(int64_t{ppqs[i]} - ppq) < std::llabs(int64_t{ppqs[best]} - ppq)) best = i;
    set(m, mod::CvClockPpq, best);
  }
  if (b.has(0x67, 4) && b.i32(0x67) > 0) set(m, mod::CvPulse, b.i32(0x67) / 44100.0);
  if (b.has(0x6b, 8)) set(m, mod::CvPitchScale, b.f64(0x6b) > 1.1 ? 1 : 0);
  // Outputs 2-4 (extra stereo outputs) and the per-output polarity / voltage
  // range options have no Gridline counterpart: the main output is imported.
  m.exact = false;
  (void)exact;
  auto ofs = [](double n) { return n * 2.0 - 1.0; };
  linkParams(m, {{0, mod::CvLeftOfs, ofs}, {1, mod::CvRightOfs, ofs}, {8, mod::CvLeftScale}, {9, mod::CvRightScale}},
             {{0, 0, 65536}, {1, 0, 65536}, {8, 0, 65536}, {9, 0, 65536}});
  return true;
}

// ------------------------------------------------------------------- Patcher
// State: u32 version (3, 4), then chunks: u32 id, u64 length, payload. Chunks
// with id 0 are containers (their payload is again a chunk list). A hosted
// plugin is a container holding 5000 (node header), 5004 (plugin: a DLL path
// in version 3, the plugin name in UTF-16 in version 4), 5008 (display
// name), 5010 (short name), 5005 (the plugin's own state, as event 213 of a
// channel / slot), 5007 (node options), 5009 (ports), 5001 (end). 1300 =
// wires between ports, 1400/1402 = the patcher's own ports, 1600 = editor
// positions.
struct Chunk {
  uint32_t id = 0;
  Blob data;
};

std::vector<Chunk> chunks(Blob b) {
  std::vector<Chunk> out;
  size_t p = 0;
  while (b.has(p, 12)) {
    uint32_t id = b.u32(p);
    uint64_t n = static_cast<uint64_t>(b.u32(p + 4)) | (static_cast<uint64_t>(b.u32(p + 8)) << 32);
    if (n > b.size() - p - 12) break;
    out.push_back({id, b.sub(p + 12, static_cast<size_t>(n))});
    p += 12 + static_cast<size_t>(n);
  }
  return out;
}

bool isChunkList(Blob b) {
  if (b.size() < 12) return false;
  size_t p = 0;
  while (b.has(p, 12)) {
    uint64_t n = static_cast<uint64_t>(b.u32(p + 4)) | (static_cast<uint64_t>(b.u32(p + 8)) << 32);
    if (n > b.size() - p - 12) return false;
    p += 12 + static_cast<size_t>(n);
  }
  return p == b.size();
}

struct HostedPlugin {
  std::string name, display;
  Blob state;
};

std::string chunkText(Blob b, bool utf16) {
  if (!utf16) return b.ascii(0, b.size());
  return b.utf16(0, b.size() / 2);
}

// Plugin name from a version-3 DLL path: the folder holding the DLL.
std::string nameFromPath(const std::string& path) {
  std::string p = path;
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  size_t slash = p.rfind('/');
  if (slash == std::string::npos) return p;
  std::string dir = p.substr(0, slash);
  size_t s2 = dir.rfind('/');
  return s2 == std::string::npos ? dir : dir.substr(s2 + 1);
}

void collectNodes(Blob b, bool utf16, std::vector<HostedPlugin>& out, std::vector<std::string>& ports, int depth) {
  if (depth > 4) return;
  for (const Chunk& c : chunks(b)) {
    if (c.id == 1402) ports.push_back(chunkText(c.data, utf16));
    if (c.id != 0 || !isChunkList(c.data)) continue;
    HostedPlugin h;
    bool node = false, hasState = false;
    for (const Chunk& k : chunks(c.data)) {
      if (k.id == 5004) h.name = chunkText(k.data, utf16), node = true;
      else if (k.id == 5008) h.display = chunkText(k.data, utf16);
      else if (k.id == 5005) h.state = k.data, hasState = true;
    }
    if (node) {
      if (!utf16 || h.name.find('\\') != std::string::npos || h.name.find('/') != std::string::npos) h.name = nameFromPath(h.name);
      if (hasState) out.push_back(h);
    } else {
      collectNodes(c.data, utf16, out, ports, depth + 1);
    }
  }
}

bool decodePatcher(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  if (fx2::decodePatcherV2(c, evs, m)) return true;  // J-41: the patch as wired
  Blob b = stateBlob(evs);
  if (b.size() < 16) {
    begin(m, "gl.patcher");
    return true;
  }
  bool utf16 = b.u32(0) >= 4;
  std::vector<HostedPlugin> hosted;
  std::vector<std::string> ports;
  collectNodes(b.sub(4, b.size() - 4), utf16, hosted, ports, 0);
  // FL uses one name for both versions: the instrument has a "Notes" input.
  bool instrument = std::find(ports.begin(), ports.end(), "Notes") != ports.end();
  if (!instrument && std::find(ports.begin(), ports.end(), "Audio input") == ports.end()) instrument = true;
  begin(m, instrument ? "gl.patcher" : "gl.patcherfx");
  mod::PatchGraph g;
  std::vector<mod::PatchNode> gens, fxs;
  bool exact = true, missingGen = false;
  for (const HostedPlugin& h : hosted) {
    const PluginDecoder* dec = findDecoder(h.name);
    bool ok = false;
    if (dec) {
      // The hosted state is laid out like a channel's / slot's event 213.
      std::vector<uint8_t> nameBytes;
      for (char ch : h.name) nameBytes.push_back(static_cast<uint8_t>(ch)), nameBytes.push_back(0);
      nameBytes.push_back(0), nameBytes.push_back(0);
      std::vector<Event> inner(2);
      inner[0].id = 201, inner[0].data = nameBytes.data(), inner[0].size = static_cast<uint32_t>(nameBytes.size());
      inner[1].id = 213, inner[1].data = h.state.p, inner[1].size = static_cast<uint32_t>(h.state.size());
      PluginMapping im;
      im.flName = h.name;
      if ((*dec)(c, inner, im) && !im.glType.empty()) {
        if (im.state.type.empty()) im.state.type = im.glType;
        normalizePluginState(im.state);
        const PluginInfo* info = findPlugin(im.state.type);
        if (info && im.state.type != "gl.patcher" && im.state.type != "gl.patcherfx" && info->kind != PluginKind::NoteFx) {
          mod::PatchNode n;
          n.plugin = im.state;
          (info->kind == PluginKind::Generator ? gens : fxs).push_back(n);
          exact = exact && im.exact;
          ok = true;
        }
      }
    }
    if (!ok) {
      exact = false;
      c.report.warn("Patcher: hosted plugin \"" + h.name + "\" has no Gridline equivalent (skipped)");
      // A hosted instrument that cannot be decoded still has to sound.
      static const char* const kFx[] = {"Fruity", "Control Surface", "Maximus", "Soundgoodizer", "Gross Beat", "Edison", "Pitcher"};
      bool looksFx = false;
      for (const char* p : kFx) looksFx = looksFx || h.name.rfind(p, 0) == 0;
      if (!looksFx) missingGen = true;
    }
  }
  if (!instrument) gens.clear(), missingGen = false;  // the effect version hosts effects only
  if (gens.empty() && missingGen) {
    mod::PatchNode n;
    n.plugin = defaultPluginState("gl.synth");
    gens.push_back(n);
  }
  for (auto& n : gens)
    if (g.nodes.size() < static_cast<size_t>(mod::kPatchNodes)) g.nodes.push_back(n);
  for (auto& n : fxs)
    if (g.nodes.size() < static_cast<size_t>(mod::kPatchNodes)) g.nodes.push_back(n);
  if (gens.size() + fxs.size() > static_cast<size_t>(mod::kPatchNodes)) exact = false;
  // The patcher's wiring is not replayed: instruments are layered, effects
  // run in series in the order FL stores them.
  m.exact = false;
  (void)exact;
  m.state.data = mod::serializePatchGraph(g);  // never the host's default graph
  return true;
}

}  // namespace

void registerControllers(std::map<std::string, PluginDecoder>& d) {
  add(d, "Fruity Keyboard Controller", decodeKeyboardController);
  add(d, "Fruity Voltage Controller", decodeVoltageController);
  add(d, "Patcher", decodePatcher);
}

}  // namespace gl::flp::gen2
