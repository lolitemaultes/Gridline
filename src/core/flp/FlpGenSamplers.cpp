// Gridline — FLP import: FL sample-based instruments.
//   DirectWave → Multisampler ("gl.multisampler"): the program's zones.
//   FLEX       → "gl.flex": the FLEX state kept exactly + a Gridline stand-in (J-53).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../../packs/samplers/samplers.h"
#include "../../packs/synths/synths.h"
#include "../FlexStandin.h"
#include "../PluginParams.h"
#include "FlpGen.h"

namespace gl::flp::gen {

namespace {

namespace smp = gl::packs::samplers;
namespace sy = gl::packs::synths;

double clampd(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

// Records {dword id, qword size, payload} from `at` to the end of `b`.
struct Rec {
  uint32_t id;
  Bytes data;
};
std::vector<Rec> records(Bytes b, size_t at) {
  std::vector<Rec> out;
  while (b.has(at, 12)) {
    const uint32_t id = b.u32(at);
    const uint64_t size = b.u64(at + 4);
    if (size > b.n - at - 12) break;
    out.push_back({id, b.sub(at + 12, static_cast<size_t>(size))});
    at += 12 + static_cast<size_t>(size);
  }
  return out;
}

std::string text(Bytes b) {
  std::string s;
  for (size_t i = 0; i < b.n && b.p[i]; ++i) s += static_cast<char>(b.p[i]);
  return s;
}

// FL sample paths: %FLStudioFactoryData% / %FLStudioData% / relative; falls
// back to the file name next to the project when the path does not exist.
std::string samplePath(Context& c, std::string p) {
  for (char& ch : p)
    if (ch == '\\') ch = '/';
  const std::string tag = "%FLStudioData%";
  if (p.rfind(tag, 0) == 0) p = c.factoryData + "/Data" + p.substr(tag.size());
  p = c.resolvePath(p);
  std::error_code ec;
  if (!p.empty() && !std::filesystem::exists(p, ec) && !c.projectDir.empty()) {
    std::string alt = c.projectDir + "/" + std::filesystem::path(p).filename().string();
    if (std::filesystem::exists(alt, ec)) return alt;
  }
  return p;
}

// --------------------------------------------------------------- DirectWave
// State: dword version, then records. Record 1 = a program (128 of them; the
// empty ones are short). Inside a program: 0x64 header, 0x66 name, 0x67 file,
// program-level settings, then one record 3 per zone. Inside a zone:
//   500: byte root, low key, high key, low velocity, high velocity, 3 bytes,
//        float volume (0..1), float pan (0..1, centre 0.5), …
//   501: zone name; 502: sample path
//   503: dword frames, dword ?, dword channels, dword ?, float rate,
//        dword loop mode, dword loop start, dword loop end (frames)
bool decodeDirectWave(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 16)) return false;
  smp::ZoneMap map;
  std::string progName;
  int missing = 0;
  for (const Rec& top : records(b, 4)) {
    if (top.id != 1) continue;
    std::vector<Rec> prog = records(top.data, 0);
    std::vector<smp::Zone> zones;
    std::string name;
    for (const Rec& r : prog) {
      if (r.id == 0x66) name = text(r.data);
      if (r.id != 3) continue;
      smp::Zone z;
      bool ok = false;
      for (const Rec& f : records(r.data, 0)) {
        Bytes d = f.data;
        switch (f.id) {
          case 500:
            if (d.n >= 17) {
              z.root = std::clamp<int>(d.u8(0), 0, 127);
              z.lo = std::clamp<int>(d.u8(1), 0, 127);
              z.hi = std::clamp<int>(d.u8(2), z.lo, 127);
              z.vlo = std::clamp<int>(d.u8(3), 0, 127);
              z.vhi = std::clamp<int>(d.u8(4), z.vlo, 127);
              const double vol = d.f32(9), pan = d.f32(13);
              z.gain = static_cast<float>(vol > 0.001 ? clampd(20.0 * std::log10(vol), -60, 12) : -60.0);
              z.pan = static_cast<float>(clampd((pan - 0.5) * 2.0, -1, 1));
            }
            break;
          case 501: z.name = text(d); break;
          case 502:
            z.path = samplePath(c, text(d));
            ok = !z.path.empty();
            break;
          case 503:
            if (d.n >= 32) {
              const int mode = d.i32(20);
              const int64_t ls = d.i32(24), le = d.i32(28);
              if (mode > 0 && le > ls) {
                z.loop = mode == 3 ? smp::LoopBounce : smp::LoopForward;
                z.loopStart = ls;
                z.loopEnd = le;
              }
            }
            break;
          default: break;
        }
      }
      if (!ok) continue;
      std::error_code ec;
      if (!std::filesystem::exists(z.path, ec)) ++missing;
      zones.push_back(z);
    }
    if (!zones.empty()) {
      map.zones = std::move(zones);
      progName = name;
      break;  // the first program with zones is the one in use
    }
  }
  if (map.zones.empty()) return false;
  PluginState s = base("gl.multisampler");
  setp(s, smp::MsRoundRobin, smp::RrLayer);
  setp(s, smp::MsRelease, 0.45);
  setp(s, smp::MsVolume, 0.8);
  s.data = smp::serializeZoneMap(map);
  m.glType = "gl.multisampler";
  m.state = s;
  m.exact = false;
  char buf[256];
  std::snprintf(buf, sizeof buf, "program \"%s\": %zu zones (key / velocity ranges, roots, volume, pan, loops) → Multisampler; envelopes and modulation at defaults%s",
                progName.c_str(), map.zones.size(), missing ? ", some samples not found" : "");
  note(c, "DirectWave", buf);
  if (missing) c.report.warn("DirectWave: " + std::to_string(missing) + " sample file(s) not found");
  // Automation / recorded values: the zone amp envelope (tags 32..35: attack
  // 0..1000, decay / sustain / release 0..2000, sustain 1000 = full level)
  // drives the Multisampler's amp envelope (times: same position on a log scale).
  ParamTable t;
  t.exp(32, smp::MsAttack, 0.001, 10);
  t.exp(33, smp::MsDecay, 0.01, 20);
  t.fn(34, smp::MsSustain, [](double n) { return clampd(2.0 * n, 0, 1); });
  t.exp(35, smp::MsRelease, 0.005, 20);
  t.range(32, 0, 1000), t.range(33, 0, 2000), t.range(34, 0, 2000), t.range(35, 0, 2000);
  t.install(m);
  return true;
}

// --------------------------------------------------------------------- FLEX
// State: dword version, then records: 1 = preset identity (library name at 0,
// preset name at 0x401, ANSI), 2 = parameters (a record 0 holding dword 2,
// dword count, count floats 0..1 in control-tag order: 0..4 volume AHDSR,
// 5..9 filter AHDSR, 10..17 macros, 18 cutoff, 19 env amount, 20 resonance,
// 21..23 post filter, 24..29 delay, 30..34 reverb, 35/36 levels, 37 delay on,
// 38 reverb on, 39 pitch (±2400 cents), 40..44 options). The envelope and
// filter controls are offsets around 0.5 on top of the preset's own sound.
// J-53: the channel becomes "gl.flex" (core/FlexStandin.h): the 45 controls
// are kept exactly (parameters 0..44, identity automation) with the pack and
// preset names; Gridline plays a stand-in built from them, and the FL export
// writes the FLEX state back (the original sound in FL Studio).
bool decodeFlex(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 16)) return false;
  std::string library, preset;
  float p[flex::kControls];
  for (float& v : p) v = 0.5f;
  bool haveParams = false;
  for (const Rec& r : records(b, 4)) {
    if (r.id == 1) {
      library = text(r.data.sub(0, std::min<size_t>(r.data.n, 0x400)));
      if (r.data.n > 0x401) preset = text(r.data.sub(0x401, r.data.n - 0x401));
    }
    if (r.id == 2)
      for (const Rec& q : records(r.data, 0)) {
        if (q.id != 0 || !q.data.has(0, 8)) continue;
        const int n = std::min(flex::kControls, q.data.i32(4));
        for (int k = 0; k < n && q.data.has(8 + static_cast<size_t>(k) * 4, 4); ++k) {
          const float v = q.data.f32(8 + static_cast<size_t>(k) * 4);
          p[k] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.5f;
        }
        haveParams = n > 0;
      }
  }
  if (preset.empty() && !haveParams) return false;
  PluginState s = flex::makeState(library, preset, p);
  m.glType = s.type;
  m.state = s;
  m.exact = false;
  // Automation / remote control: FLEX parameter k is stand-in parameter k.
  m.param = [](int fl) { return fl >= 0 && fl < flex::kControls ? fl : -1; };
  m.value = [](int, double n) { return std::clamp(n, 0.0, 1.0); };
  std::vector<RawRange> ranges;
  for (int k = 0; k < flex::kControls; ++k)
    ranges.push_back(k == flex::Pitch ? RawRange{k, -2400, 2400} : k == flex::DlyTime ? RawRange{k, 1, 768} : RawRange{k, 0, 65536});
  setRawRanges(m, ranges);
  const int cat = flex::classify(library, preset);
  note(c, "FLEX", "preset \"" + preset + "\" (" + library + ") plays a Gridline stand-in: " + flex::catName(cat) + " on " +
                      flex::engineName(flex::engineOf(cat)) + "; the FLEX state (pack, preset, 45 controls) is kept and exports unchanged");
  return true;
}

}  // namespace

void registerSamplerDecoders(std::map<std::string, PluginDecoder>& d) {
  d["DirectWave"] = decodeDirectWave;
  d["FLEX"] = decodeFlex;
}

}  // namespace gl::flp::gen
