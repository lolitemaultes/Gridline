// Gridline — FLP import, second wave: Slicex, Fruity Slicer, Fruity Granulizer, Wave Traveller.
//
// Layouts (worked out from FL Studio's supplied projects and presets; see
// analysis/demos/instruments_wave2.md for the full notes):
//   Slicex          0x60-byte header (version, knob integers at 4·(1+tag)),
//                   byte 0x5F = 1 when the sample is referenced by path only,
//                   Pascal path at 0x60, then (embedded) u32 length + RIFF/WAVE
//                   with the slice points in its "cue " chunk; the settings
//                   block that follows holds the region count at +0x18C.
//   Fruity Slicer   u32 version, f32 beats, f32 tempo, 5 × i32, Pascal path,
//                   i32 slice count, slices {Pascal name, i32 start frame,
//                   i32 end, f32, u8}, then u8, u8 base note, …
//   Granulizer      u32 version, then one i32 per knob (tag order); the
//                   sample is the channel's own sample (event 196).
//   Wave Traveller  i32 1000, header size, magic, i32, Pascal-like path
//                   (i32 length), then per key {2000, size, key, f32 length,
//                   3 × f32, f32 start, f32 end, i32, mute toggles, position
//                   spline, volume spline}, ended by 3000.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include "../../engine/Sample.h"
#include "../../packs/samplers/samplers.h"
#include "../PluginParams.h"
#include "FlpGen2.h"

namespace gl::flp::gen2 {

namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ paths
bool exists(const std::string& p) {
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}

// FL sample path → local file. Handles %FLStudioFactoryData%, %FLStudioData%
// (the factory "Data" folder) and absolute paths from other machines that
// point into an FL installation (…\Data\Patches\…, …/Resources/FL/…).
std::string resolveFl(const Context& c, std::string raw) {
  if (raw.empty()) return raw;
  for (char& ch : raw)
    if (ch == '\\') ch = '/';
  const std::string dataTag = "%FLStudioData%";
  std::string p = raw.rfind(dataTag, 0) == 0 ? c.factoryData + "/Data" + raw.substr(dataTag.size()) : c.resolvePath(raw);
  if (exists(p) || c.factoryData.empty()) return p;
  for (const char* key : {"/Resources/FL/", "/Data/Patches/"}) {
    size_t at = raw.find(key);
    if (at == std::string::npos) continue;
    std::string tail = raw.substr(at + std::string(key).size());
    std::string q = std::string(key) == "/Resources/FL/" ? c.factoryData + "/" + tail : c.factoryData + "/Data/Patches/" + tail;
    if (exists(q)) return q;
  }
  return p;
}

bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    auto c = static_cast<unsigned char>(s[i]);
    size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
    if (n == 9 || i + n >= s.size() + (n ? 0 : 1)) return false;
    for (size_t k = 1; k <= n; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n + 1;
  }
  return true;
}

// Raw bytes → UTF-8 (kept as is when already UTF-8, else read as Latin-1).
std::string textBytes(const Blob& b, size_t at, size_t n) {
  std::string raw;
  for (size_t i = 0; i < n && b.has(at + i, 1); ++i) raw += static_cast<char>(b.u8(at + i));
  if (validUtf8(raw)) return raw;
  std::string s;
  for (char c : raw) {
    auto ch = static_cast<unsigned char>(c);
    if (ch < 0x80) {
      s += c;
    } else {
      s += static_cast<char>(0xC0 | (ch >> 6));
      s += static_cast<char>(0x80 | (ch & 0x3F));
    }
  }
  return s;
}

// Length-prefixed (one byte) string.
std::string pascal(const Blob& b, size_t at, size_t* next) {
  size_t n = b.u8(at);
  if (next) *next = at + 1 + n;
  return textBytes(b, at + 1, n);
}

// -------------------------------------------------------------- RIFF/WAVE
struct WavInfo {
  bool ok = false;
  uint64_t frames = 0;              // from the data chunk (0 = unknown)
  std::vector<uint32_t> cues;       // cue point sample offsets, ascending
  std::map<uint32_t, uint32_t> regionLen;  // cue offset → region length ("LIST adtl" ltxt)
};

WavInfo wavInfo(const Blob& b) {
  WavInfo w;
  if (b.size() < 12 || b.find("RIFF") != 0 || b.find("WAVE", 8) != 8) return w;
  size_t end = std::min<size_t>(b.size(), static_cast<size_t>(b.u32(4)) + 8);
  uint32_t blockAlign = 0, dataSize = 0, factFrames = 0, tag = 0;
  std::map<uint32_t, uint32_t> cueIds, ltxt;  // cue id → offset, cue id → region length
  bool haveData = false;
  for (size_t q = 12; q + 8 <= end;) {
    uint32_t len = b.u32(q + 4);
    const uint8_t* id = b.p + q;
    if (std::memcmp(id, "fmt ", 4) == 0) {
      tag = b.u16(q + 8);
      blockAlign = b.u16(q + 8 + 12);
    } else if (std::memcmp(id, "fact", 4) == 0) {
      factFrames = b.u32(q + 8);
    } else if (std::memcmp(id, "data", 4) == 0) {
      dataSize = len;
      haveData = true;
    } else if (std::memcmp(id, "cue ", 4) == 0) {
      uint32_t n = b.u32(q + 8);
      for (uint32_t k = 0; k < n && k < 4096 && b.has(q + 12 + 24 * k, 24); ++k) {
        w.cues.push_back(b.u32(q + 12 + 24 * k + 20));
        cueIds[b.u32(q + 12 + 24 * k)] = b.u32(q + 12 + 24 * k + 20);
      }
    } else if (std::memcmp(id, "LIST", 4) == 0 && b.has(q + 8, 4) && std::memcmp(b.p + q + 8, "adtl", 4) == 0) {
      // Region lengths: "ltxt" {u32 cue id, u32 sample length, purpose, …}.
      size_t e = std::min(end, q + 8 + len);
      for (size_t r = q + 12; r + 8 <= e;) {
        uint32_t sl = b.u32(r + 4);
        if (std::memcmp(b.p + r, "ltxt", 4) == 0 && sl >= 8) ltxt[b.u32(r + 8)] = b.u32(r + 12);
        if (sl > e - r - 8) break;
        r += 8 + sl + (sl & 1);
      }
    }
    if (len > end - q - 8) break;
    q += 8 + len + (len & 1);
  }
  for (const auto& [cid, length] : ltxt)
    if (auto it = cueIds.find(cid); it != cueIds.end() && length > 0) w.regionLen[it->second] = length;
  std::sort(w.cues.begin(), w.cues.end());
  w.cues.erase(std::unique(w.cues.begin(), w.cues.end()), w.cues.end());
  // PCM / float: data size ÷ block size; compressed formats (ADPCM, Vorbis in
  // WAV, …): the "fact" frame count, else unknown (decoded later).
  bool linear = tag == 1 || tag == 3 || tag == 0xFFFE;
  if (haveData && linear && blockAlign > 0) w.frames = dataSize / blockAlign;
  else if (!linear && factFrames > 0) w.frames = factFrames;
  w.ok = true;
  return w;
}

std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Frames of a sample file (WAV header, else decoded through the sample cache).
uint64_t sampleFrames(const std::string& path, const WavInfo& w) {
  if (w.frames > 0) return w.frames;
  SamplePtr s = sampleCache().get(path);
  return s ? s->frames() : 0;
}

// Folder for samples stored inside plugin states (written once, reused).
std::string embeddedDir() {
  if (const char* e = std::getenv("GRIDLINE_CACHE_DIR"); e && *e) return std::string(e) + "/flp-samples";
  if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/gridline/flp-samples";
  if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/gridline/flp-samples";
  return (fs::temp_directory_path() / "gridline-flp-samples").string();
}

// Writes an embedded RIFF to the cache (named by content hash); "" on failure.
std::string storeEmbedded(const Blob& riff, const std::string& hintName) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (size_t i = 0; i < riff.size(); ++i) h = (h ^ riff.p[i]) * 1099511628211ull;
  std::string stem = fs::path(hintName).stem().string();
  std::string clean;
  for (char ch : stem) clean += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') ? ch : '_';
  if (clean.empty()) clean = "slice";
  char hex[20];
  std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
  std::string dir = embeddedDir();
  std::string path = dir + "/" + clean.substr(0, 40) + "-" + hex + ".wav";
  std::error_code ec;
  if (fs::is_regular_file(path, ec) && fs::file_size(path, ec) == riff.size()) return path;
  fs::create_directories(dir, ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return "";
  f.write(reinterpret_cast<const char*>(riff.p), static_cast<std::streamsize>(riff.size()));
  return f ? path : "";
}

// Slice starts (frames) → gl.slicer manual markers.
void setMarkers(PluginMapping& m, const std::vector<uint32_t>& starts, uint64_t frames) {
  using namespace pp;
  set(m, SlMode, SlModeManual);
  int k = 0;
  for (uint32_t s : starts) {
    if (k >= kMaxSlices) break;
    double pos = frames > 0 ? static_cast<double>(s) / static_cast<double>(frames) : 0.0;
    if (pos < 0 || pos >= 1) continue;
    set(m, slMarker(k++), pos);
  }
  if (k == 0) set(m, slMarker(0), 0.0);
}

// ------------------------------------------------------------------ Slicex
bool decodeSlicex(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob s = stateBlob(evs);
  if (s.size() < 0x62) return false;
  begin(m, "gl.slicer");
  bool external = s.u8(0x5F) == 1;
  size_t p = 0;
  std::string rawPath = pascal(s, 0x60, &p);
  Blob riff;
  if (!external) {
    uint32_t len = s.u32(p);
    if (s.has(p + 4, len)) riff = s.sub(p + 4, len);
    p += 4 + static_cast<size_t>(len);
  }
  // Sample: the embedded copy (what Slicex plays, possibly edited) wins.
  WavInfo w;
  std::string path;
  if (!riff.empty()) {
    w = wavInfo(riff);
    if (w.ok && w.frames > 0) path = storeEmbedded(riff, rawPath.empty() ? "slicex" : rawPath);
    if (path.empty()) w = {};
  }
  if (path.empty() && !rawPath.empty()) {
    path = resolveFl(c, rawPath);
    std::vector<uint8_t> bytes = readAll(path);
    w = wavInfo(Blob(bytes.data(), bytes.size()));
    w.frames = sampleFrames(path, w);
  }
  m.state.data = path;
  // Regions: one per cue point, in sample order (the settings block that
  // follows repeats the count at +0x18C; a sample without cues is one region).
  if (s.u32(p + 0x18C) != w.cues.size() && !w.cues.empty())
    c.report.warn("Slicex: region count differs from the sample's cue points");
  setMarkers(m, w.cues, w.frames);
  // Region end points ("ltxt" lengths) where a region stops before the next one.
  if (w.frames > 0)
    for (size_t k = 0; k < w.cues.size() && k < static_cast<size_t>(kMaxSlices); ++k) {
      auto it = w.regionLen.find(w.cues[k]);
      if (it == w.regionLen.end()) continue;
      const uint64_t endF = static_cast<uint64_t>(w.cues[k]) + it->second;
      const uint64_t next = k + 1 < w.cues.size() ? w.cues[k + 1] : w.frames;
      if (endF < next) set(m, slParam2(static_cast<int>(k), SliceEnd), static_cast<double>(endF) / static_cast<double>(w.frames));
    }
  // Master level 0..16000 (12800 = unity), master pitch −1200..1200 cents,
  // modulation X / Y 0..512 (256 = centre).
  int32_t level = s.i32(0x0C), cents = s.i32(0x18);
  set(m, SlVolume, std::clamp(level / 12800.0, 0.0, 1.0));
  set(m, SlPitch, std::clamp(cents / 100.0, -24.0, 24.0));
  set(m, SlModX, std::clamp(s.i32(0x1C) / 512.0, 0.0, 1.0));
  set(m, SlModY, std::clamp(s.i32(0x20) / 512.0, 0.0, 1.0));
  set(m, SlBaseNote, 60);
  set(m, SlDeclick, 1);
  // Per-region settings (17 × i32 from S + 0x190): volume [3] 0..16000 (8000 =
  // unity), pan [4] ±6400, cutoff [6] / resonance [7] 0..256 (128 = neutral);
  // the articulator amounts / groups keep their defaults in every shipped file.
  bool regionsDefault = true;
  const uint32_t nReg = s.u32(p + 0x18C);
  for (uint32_t k = 0; k < nReg && k < static_cast<uint32_t>(kMaxSlices) && s.has(p + 0x190 + 68 * k, 68); ++k) {
    const size_t r = p + 0x190 + 68 * static_cast<size_t>(k);
    const int vol = s.i32(r + 12), pan = s.i32(r + 16), cut = s.i32(r + 24), res = s.i32(r + 28);
    const int ik = static_cast<int>(k);
    set(m, slParam(ik, SliceGain), vol > 0 ? std::clamp(20.0 * std::log10(vol / 8000.0), -24.0, 12.0) : -24.0);
    set(m, slParam(ik, SlicePan), std::clamp(pan / 6400.0, -1.0, 1.0));
    set(m, slParam(ik, SliceCutoff), std::clamp((cut - 128) / 128.0, -1.0, 1.0));
    set(m, slParam2(ik, SliceReso), std::clamp((res - 128) / 128.0, -1.0, 1.0));
    static const int kDefault[17] = {0, 0, 0, 8000, 0, 12800, 128, 128, 0, 12800, 0, 0, 12800, 0, 0, 0, 12800};
    for (int f = 0; f < 17; ++f)
      if (f != 3 && f != 4 && f != 6 && f != 7 && s.i32(r + 4 * static_cast<size_t>(f)) != kDefault[f]) regionsDefault = false;
  }
  if (!regionsDefault) c.report.warn("Slicex: region articulator / delay settings are not imported");
  linkParams(m, {{2, SlVolume, [](double n) { return std::min(1.0, n * 16000.0 / 12800.0); }},
                 {5, SlPitch, [](double n) { return (-1200.0 + 2400.0 * n) / 100.0; }}},
             {{2, 0, 16000}, {5, -1200, 1200}});
  if (path.empty()) c.report.warn("Slicex: no sample found");
  m.exact = false;  // articulators, per-region envelopes and layering are not imported
  return true;
}

// ----------------------------------------------------------- Fruity Slicer
bool decodeFruitySlicer(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob s = stateBlob(evs);
  if (s.size() < 37) return false;
  begin(m, "gl.slicer");
  size_t p = 0;
  std::string rawPath = pascal(s, 32, &p);
  int32_t count = s.i32(p);
  p += 4;
  std::vector<uint32_t> starts;
  for (int32_t k = 0; k < count && k < 4096 && s.has(p, 1); ++k) {
    size_t q = 0;
    pascal(s, p, &q);  // slice name
    int32_t st = s.i32(q);
    if (st >= 0) starts.push_back(static_cast<uint32_t>(st));
    p = q + 13;
  }
  int baseNote = s.has(p + 1, 1) ? s.u8(p + 1) : 60;
  std::string path = resolveFl(c, rawPath);
  m.state.data = path;
  std::sort(starts.begin(), starts.end());
  uint64_t frames = 0;
  if (!path.empty()) {
    std::vector<uint8_t> bytes = readAll(path);
    frames = sampleFrames(path, wavInfo(Blob(bytes.data(), bytes.size())));
  }
  setMarkers(m, starts, frames);
  set(m, SlBaseNote, baseNote > 0 && baseNote < 128 ? baseNote : 60);
  set(m, SlVolume, 1.0);
  set(m, SlPitch, std::clamp(s.i32(12) / 100.0, -24.0, 24.0));
  set(m, SlDeclick, 1);
  m.exact = false;  // fit-to-tempo stretch, fades and per-slice reverse are not imported
  return true;
}

// -------------------------------------------------------- Fruity Granulizer
bool decodeGranulizer(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob s = stateBlob(evs);
  if (s.size() < 44) return false;
  begin(m, "gl.granular");
  auto knobAt = [&](int tag, int32_t def) { return s.has(static_cast<size_t>(4 * (1 + tag)), 4) ? s.i32(static_cast<size_t>(4 * (1 + tag))) : def; };
  if (const Event* e = find(evs, 196)) m.state.data = resolveFl(c, e->text());
  int32_t att = knobAt(0, 204), hold = knobAt(1, 50), gs = knobAt(2, 611), ws = knobAt(3, 611);
  int32_t fxDepth = knobAt(4, 0), fxSpeed = knobAt(5, 180), rnd = knobAt(6, 0), stereo = knobAt(7, 0);
  int32_t loop = knobAt(8, 0), start = knobAt(10, 0), transHold = knobAt(11, 228), trig = knobAt(12, 0);
  // Grain spacing is logarithmic: 200 units per octave (twice-faster presets
  // differ by ~200 between grain and wave spacing).
  double spacing = 0.00483 * std::exp2(std::clamp(gs, 125, 1000) / 200.0);
  set(m, GrDensity, std::clamp(1.0 / spacing, 2.0, 120.0));
  set(m, GrSize, std::clamp(spacing * 2.0, 0.01, 0.5));
  set(m, GrShape, std::clamp(att / std::max(1.0, 2.0 * att + hold), 0.02, 0.5));
  double speed = ws == 0 ? 0.0 : (ws < 0 ? -1.0 : 1.0) * std::exp2((std::abs(ws) - gs) / 200.0);
  set(m, GrSpeed, std::clamp(speed, -2.0, 2.0));
  set(m, GrFreeze, 0);
  set(m, GrPosition, std::clamp(start / 4096.0, 0.0, 1.0));
  set(m, GrSpread, 0.02 + 0.3 * std::clamp(rnd / 256.0, 0.0, 1.0));
  set(m, GrJitter, std::clamp(rnd / 256.0, 0.0, 1.0));
  set(m, GrPitchRand, 0);
  set(m, GrStereo, std::clamp(stereo / 64.0, 0.0, 1.0));
  set(m, GrLoop, loop != 0);
  set(m, GrKeyMode, trig <= 0 ? 0 : trig == 1 ? 1 : 2);
  set(m, GrTransientHold, std::clamp(transHold / 1000.0, 0.0, 2.0));
  set(m, GrWobble, 2.0 * std::clamp(fxDepth / 384.0, 0.0, 1.0));
  set(m, GrWobbleRate, std::clamp(fxSpeed / 20.0, 0.1, 20.0));
  set(m, GrAttack, 0.003);
  set(m, GrRelease, 0.1);
  set(m, GrVolume, 0.8);
  set(m, GrBaseNote, 60);
  linkParams(m, {{7, GrStereo}, {10, GrPosition}, {6, GrJitter}, {8, GrLoop}}, {{7, 0, 64}, {10, 0, 4096}, {6, 0, 256}, {8, 0, 1}});
  m.exact = false;  // grain timing curves are approximations of the FL engine
  return true;
}

// ---------------------------------------------------------- Wave Traveller
bool decodeWaveTraveller(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace packs::samplers;
  Blob s = stateBlob(evs);
  if (s.size() < 20 || s.i32(0) != 1000) return false;
  begin(m, "gl.scratch");
  int32_t plen = s.i32(16);
  if (plen < 0 || !s.has(20, static_cast<size_t>(plen))) return false;
  std::string raw = textBytes(s, 20, static_cast<size_t>(plen));
  ScratchData d;
  d.path = resolveFl(c, raw);
  for (int k = 0; k < kMoves; ++k) d.moves[static_cast<size_t>(k)] = ScratchMove{{{0, 0, CurveLinear}, {1, 1, CurveLinear}}, 0};
  double length = 0;
  size_t p = 20 + static_cast<size_t>(plen);
  int parsed = 0;
  while (s.has(p, 48) && s.i32(p) == 2000) {
    int key = s.i32(p + 8);
    float len = s.f32(p + 12), st = s.f32(p + 28), en = s.f32(p + 32);
    int32_t nm = s.i32(p + 44);
    if (nm < 0 || nm > 4096) break;
    p += 48;
    std::vector<std::pair<int32_t, float>> mutes;
    for (int32_t k = 0; k < nm && s.has(p, 8); ++k, p += 8) mutes.emplace_back(s.i32(p), s.f32(p + 4));
    std::vector<ScratchPoint> pts[2];
    bool bad = false;
    for (auto& v : pts) {
      int32_t n = s.i32(p);
      p += 4;
      if (n < 0 || n > 4096 || !s.has(p, static_cast<size_t>(n) * 60)) {
        bad = true;
        break;
      }
      for (int32_t k = 0; k < n; ++k, p += 60) {
        ScratchPoint pt;
        pt.t = std::clamp(s.f32(p + 8), 0.f, 1.f);
        pt.y = std::clamp(s.f32(p + 12), 0.f, 1.f);
        pt.curve = s.i32(p + 48) == 2 ? CurveSmooth : CurveLinear;
        v.push_back(pt);
      }
    }
    if (bad) break;
    ++parsed;
    // Keys 60..71 (the C5 octave) become the twelve moves.
    if (key < 60 || key >= 60 + kMoves || pts[0].size() < 2) continue;
    ScratchMove mv;
    if (!(st >= 0 && st <= 1)) st = 0;
    if (!(en >= 0 && en <= 1)) en = 1;
    for (auto pt : pts[0]) {
      pt.y = st + pt.y * (en - st);
      mv.pts.push_back(pt);
    }
    if (mv.pts.size() > 64) mv.pts.resize(64);
    std::stable_sort(mv.pts.begin(), mv.pts.end(), [](const ScratchPoint& a, const ScratchPoint& b) { return a.t < b.t; });
    mv.pts.front().t = 0, mv.pts.back().t = 1;
    for (int step = 0; step < kMuteSteps; ++step) {  // toggles → muted steps
      float x = (step + 0.5f) / kMuteSteps;
      int on = 1;
      for (const auto& [state, at] : mutes)
        if (at <= x) on = state;
      if (!on) mv.mutes |= 1u << step;
    }
    d.moves[static_cast<size_t>(key - 60)] = mv;
    if (length <= 0 && std::isfinite(len) && len > 0) length = len;
  }
  if (parsed == 0) return false;
  m.state.data = serializeScratch(d);
  set(m, ScVolume, 0.8);
  set(m, ScStart, 0), set(m, ScEnd, 1);
  set(m, ScLength, std::clamp(length > 0 ? length : 4.0, 0.25, 16.0));
  set(m, ScSync, 1);
  set(m, ScKeyMode, KeyMoves);
  set(m, ScRoot, 60);
  set(m, ScPitchFollow, 0);
  set(m, ScMotion, MotionPosition);
  m.exact = false;  // volume splines and per-key lengths outside the C5 octave are not imported
  return true;
}

}  // namespace

void registerSlicers(std::map<std::string, PluginDecoder>& d) {
  add(d, "Slicex", decodeSlicex);
  add(d, "Fruity Slicer", decodeFruitySlicer);
  add(d, "Fruity slicer", decodeFruitySlicer);
  add(d, "Fruity Granulizer", decodeGranulizer);
  add(d, "Fruity granulizer", decodeGranulizer);
  add(d, "Wave Traveller", decodeWaveTraveller);
}

}  // namespace gl::flp::gen2
