// Gridline — FLP import: FL sample-based instruments.
//   DirectWave → Multisampler ("gl.multisampler"): the program's zones.
//   FLEX       → the closest Gridline instrument for the loaded preset.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "../../packs/samplers/samplers.h"
#include "../../packs/synths/synths.h"
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

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
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
enum FlexKind { FxKeys, FxElectricKeys, FxBell, FxPad, FxStrings, FxBass, FxBrass, FxPluck, FxLead, FxOther };

FlexKind classify(const std::string& preset, const std::string& library) {
  // Words of the preset name (then the library name), matched by prefix.
  std::vector<std::string> words;
  std::string w;
  for (char ch : lower(preset + " " + library) + " ") {
    if (std::isalnum(static_cast<unsigned char>(ch))) {
      w += ch;
    } else if (!w.empty()) {
      words.push_back(w);
      w.clear();
    }
  }
  auto has = [&](std::initializer_list<const char*> ws) {
    for (const auto& word : words)
      for (const char* p : ws)
        if (word.rfind(p, 0) == 0) return true;
    return false;
  };
  auto is = [&](const char* x) { return std::find(words.begin(), words.end(), x) != words.end(); };
  if (has({"rhodes", "epiano", "electric", "wurli"}) || is("ep")) return FxElectricKeys;
  if (has({"piano", "keys", "grand", "upright", "clav"})) return FxKeys;
  if (has({"bell", "chime", "glock", "celest", "marimba", "vibra", "musicbox", "mallet", "tink", "kalimba", "xylo"})) return FxBell;
  if (has({"string", "violin", "viola", "cello", "orchestr", "ensemble"})) return FxStrings;
  if (has({"pad", "atmos", "choir", "texture", "ambient", "drone", "swell"})) return FxPad;
  if (has({"bass", "sub"})) return FxBass;
  if (has({"brass", "horn", "trumpet", "trombone", "sax"})) return FxBrass;
  if (has({"pluck", "harp", "guitar", "pizz", "koto"})) return FxPluck;
  if (has({"lead", "synth", "saw", "square"})) return FxLead;
  return FxOther;
}

PluginState presetOf(const std::string& type, const std::string& preset) {
  if (const PluginInfo* info = findPlugin(type))
    for (size_t i = 0; i < info->presets.size(); ++i)
      if (info->presets[i].name == preset) return presetState(*info, i);
  return base(type);
}

bool decodeFlex(Context& c, const std::vector<Event>& events, PluginMapping& m) {
  Bytes b = stateOf(events);
  if (!b.has(0, 16)) return false;
  std::string library, preset;
  float p[64];
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
        const int n = std::min(64, q.data.i32(4));
        for (int k = 0; k < n && q.data.has(8 + static_cast<size_t>(k) * 4, 4); ++k)
          p[k] = std::clamp(q.data.f32(8 + static_cast<size_t>(k) * 4), 0.0f, 1.0f);
        haveParams = n > 0;
      }
  }
  if (preset.empty() && !haveParams) return false;
  const FlexKind kind = classify(preset, library);
  const double pitchSt = (p[39] - 0.5) * 48.0;                   // semitones
  const double attackMul = std::pow(8.0, (p[0] - 0.5) * 2.0);     // envelope offsets around the preset
  const double releaseMul = std::pow(8.0, (p[4] - 0.5) * 2.0);
  const double cutoffOct = (p[18] - 0.5) * 6.0 + (p[40] > 0.5f ? (p[21] - 1.0) * 6.0 : 0.0);
  const bool dark = lower(preset).find("dark") != std::string::npos;
  const double reverb = p[38] > 0.5f ? p[30] : 0.0, delay = p[37] > 0.5f ? p[25] : 0.0;
  PluginState s;
  std::string what;
  ParamTable t;
  if (kind == FxKeys || kind == FxElectricKeys) {
    using namespace gl::pp;
    s = base("gl.keys");
    setp(s, KyModel, kind == FxElectricKeys ? 0 : 2);
    double muffle = 9000.0 * std::pow(2.0, cutoffOct) * (dark ? 0.6 : 1.0);
    setp(s, KyMuffle, clampd(muffle, 500, 20000));
    setp(s, KyHardness, dark ? 0.3 : 0.5);
    setp(s, KyRelease, clampd(0.25 * releaseMul, 0.02, 3));
    setp(s, KyTremDepth, 0.0);
    setp(s, KyTune, 0.0);
    setp(s, KyVolume, 0.8);
    what = "Keys";
  } else if (kind == FxBell) {
    using namespace gl::pp;
    s = presetOf("gl.fm", "Bell");
    setp(s, fmParam(0, FmRelease), clampd(1.2 * releaseMul, 0.005, 10));
    setp(s, fmParam(0, FmAttack), clampd(0.002 * attackMul, 0.001, 5));
    setp(s, FmOctave, std::clamp(std::round(pitchSt / 12.0), -3.0, 3.0));
    what = "FM Synth (bell)";
  } else {
    const char* pre = kind == FxPad      ? "Velvet Pad"
                      : kind == FxStrings ? "PWM Strings"
                      : kind == FxBass    ? "Fingered Bass"
                      : kind == FxBrass   ? "Brass Section"
                      : kind == FxPluck   ? "Glass Pluck"
                      : kind == FxLead    ? "Trance Stack"
                                          : "";
    s = *pre ? presetOf("gl.analog", pre) : base("gl.analog");
    auto get = [&](int i, double d) { return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : d; };
    setp(s, sy::AnTranspose, clampd(get(sy::AnTranspose, 0) + pitchSt, -24, 24));
    setp(s, sy::AnAmpA, clampd(get(sy::AnAmpA, 0.003) * attackMul, 0.001, 10));
    setp(s, sy::AnAmpR, clampd(get(sy::AnAmpR, 0.25) * releaseMul, 0.005, 15));
    setp(s, sy::AnCutoff, clampd(get(sy::AnCutoff, 4000) * std::pow(2.0, cutoffOct), 20, 20000));
    if (reverb > 0) setp(s, sy::AnFxDrive + sy::FxRevOn, 1), setp(s, sy::AnFxDrive + sy::FxRevMix, clampd(reverb * 0.5, 0, 1));
    if (delay > 0) setp(s, sy::AnFxDrive + sy::FxDlyOn, 1), setp(s, sy::AnFxDrive + sy::FxDlyMix, clampd(delay * 0.5, 0, 1));
    what = std::string("Analog") + (*pre ? std::string(" (") + pre + ")" : "");
    t.fn(39, sy::AnTranspose, [](double n) { return (n - 0.5) * 48.0; });
    t.range(39, -2400, 2400);  // pitch control, cents
  }
  // Keep the FLEX selection and every control value with the approximation
  // (the target instruments do not use PluginState::data), so nothing of the
  // original state is lost and a later FLEX-capable importer can re-map it.
  {
    std::string keep = "flex 1\npack " + library + "\npreset " + preset + "\nv";
    char num[24];
    for (int k = 0; k < 45; ++k) {
      std::snprintf(num, sizeof num, " %.6g", static_cast<double>(p[k]));
      keep += num;
    }
    s.data = keep + "\n";
  }
  m.glType = s.type;
  m.state = s;
  m.exact = false;
  // The pack content (zones and samples) ships access-protected; Gridline does
  // not unlock it, so FLEX presets play on the closest built-in instrument.
  note(c, "FLEX", "preset \"" + preset + "\" (" + library + ") is protected sample content (not converted); approximated by " + what +
                      " with the decoded pitch / envelope / filter / FX offsets; pack / preset / 45 controls kept in the state");
  t.install(m);
  return true;
}

}  // namespace

void registerSamplerDecoders(std::map<std::string, PluginDecoder>& d) {
  d["DirectWave"] = decodeDirectWave;
  d["FLEX"] = decodeFlex;
}

}  // namespace gl::flp::gen
