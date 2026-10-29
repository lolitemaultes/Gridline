// Gridline — FLP import, second wave: Slicex, Fruity Slicer, Fruity Granulizer, Wave Traveller.
//
// Layouts (worked out from FL Studio's supplied projects and presets; see
// own format notes):
//   Slicex          0x60-byte header (version, knob integers at 4·(1+tag)),
//                   byte 0x5F = 1 when the sample is referenced by path only,
//                   Pascal path at 0x60 (J-176: 0xFF + u32 length from 255
//                   bytes on), then (embedded) u32 length + RIFF/WAVE
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
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include "../../engine/Sample.h"
#include "../../engine/SlicexEngine.h"
#include "../../packs/samplers/samplers.h"
#include "../PluginParams.h"
#include "FlpGen2.h"
#include "../Platform.h"

namespace gl::flp::gen2 {

namespace {

namespace fs = std::filesystem;
namespace sx = gl::slicex;

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

// Length-prefixed string: one length byte, or (J-176) 0xFF and a u32 length
// for strings of 255 bytes or more.
std::string pascal(const Blob& b, size_t at, size_t* next) {
  size_t n = b.u8(at), body = at + 1;
  if (n == 0xFF && b.has(at + 1, 4)) n = b.u32(at + 1), body = at + 5;
  if (!b.has(body, n)) n = b.size() > body ? b.size() - body : 0;
  if (next) *next = body + n;
  return textBytes(b, body, n);
}

// -------------------------------------------------------------- RIFF/WAVE
// A RIFF/WAVE file and its length in frames (PCM / float: data size ÷ block
// size; compressed formats (ADPCM, Vorbis in WAV, …): the "fact" count, else
// 0 = unknown, decoded later). Regions: sx::readWavRegions.
struct WavInfo {
  bool ok = false;
  uint64_t frames = 0;
};

WavInfo wavInfo(const Blob& b) {
  const sx::WavRegions r = sx::readWavRegions(b.p, b.size());
  return WavInfo{r.riff, r.frames};
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
  // $XDG_CACHE_HOME, %LOCALAPPDATA% (Windows) or ~/.cache
  if (const std::string c = gl::platform::cacheRoot(); !c.empty()) return c + "/gridline/flp-samples";
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

// States the full parser does not take (partial / unusual layouts): the
// pre-J-41 reading (sample, cues, master knobs, region fields).
bool decodeSlicexClassic(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
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
  // Regions: one per cue point (the settings block that follows repeats the
  // count at +0x18C; a sample without cues is one region). J-166: read as the
  // plugin reads them (sx::readWavRegions); the classic engine plays its
  // markers in time order, so the regions are put in that order with their
  // lengths and keys.
  std::vector<sx::Region> regs;
  if (!riff.empty()) regs = sx::readWavRegions(riff.p, riff.size()).regions;
  else if (!path.empty()) {
    std::vector<uint8_t> bytes = readAll(path);
    regs = sx::readWavRegions(bytes.data(), bytes.size()).regions;
  }
  std::stable_sort(regs.begin(), regs.end(), [](const sx::Region& a, const sx::Region& b) { return a.start < b.start; });
  if (s.u32(p + 0x18C) != regs.size() && !regs.empty())
    c.report.warn("Slicex: region count differs from the sample's cue points");
  std::vector<uint32_t> starts;
  for (const auto& r : regs) starts.push_back(static_cast<uint32_t>(r.start));
  setMarkers(m, starts, w.frames);
  // Region end points ("ltxt" lengths) where a region stops before the next one; keys.
  for (size_t k = 0; k < regs.size() && k < static_cast<size_t>(kMaxSlices); ++k) {
    set(m, slParam2(static_cast<int>(k), SliceKey), regs[k].key);
    if (w.frames == 0 || regs[k].length < 0) continue;
    const uint64_t endF = static_cast<uint64_t>(regs[k].start + regs[k].length);
    const uint64_t next = k + 1 < regs.size() ? static_cast<uint64_t>(regs[k + 1].start) : w.frames;
    if (endF < next) set(m, slParam2(static_cast<int>(k), SliceEnd), static_cast<double>(endF) / static_cast<double>(w.frames));
  }
  // Master level 0..16000 (12800 = unity), master pitch −1200..1200 cents,
  // modulation X / Y 0..512 (256 = centre).
  int32_t level = s.i32(0x0C), cents = s.i32(0x18);
  // Master level law (J-33): (11^(v/12800) − 1)/10 (12800 = unity).
  set(m, SlVolume, std::clamp((std::pow(11.0, level / 12800.0) - 1.0) / 10.0, 0.0, 1.0));
  set(m, SlPitch, std::clamp(cents / 100.0, -24.0, 24.0));
  set(m, SlModX, std::clamp(s.i32(0x1C) / 512.0, 0.0, 1.0));
  set(m, SlModY, std::clamp(s.i32(0x20) / 512.0, 0.0, 1.0));
  set(m, SlBaseNote, 60);
  set(m, SlDeclick, 1);
  // Per-region settings (17 × i32 from S + 0x190): cut group [0], pan [2] ±6400, volume [3] 0..16000 (8000 =
  // unity), pan [4] ±6400, cutoff [6] / resonance [7] 0..256 (128 = neutral);
  // the articulator amounts / groups keep their defaults in every shipped file.
  bool regionsDefault = true;
  const uint32_t nReg = s.u32(p + 0x18C);
  for (uint32_t k = 0; k < nReg && k < static_cast<uint32_t>(kMaxSlices) && s.has(p + 0x190 + 68 * k, 68); ++k) {
    const size_t r = p + 0x190 + 68 * static_cast<size_t>(k);
    const int vol = s.i32(r + 12), pan = s.i32(r + 8), cut = s.i32(r + 24), res = s.i32(r + 28);
    const int ik = static_cast<int>(k);
    // Region volume law (J-33): (3^(v/8000) − 1)/2 (8000 = unity, 16000 = ×4).
    const double g = (std::pow(3.0, vol / 8000.0) - 1.0) / 2.0;
    set(m, slParam(ik, SliceGain), g > 0 ? std::clamp(20.0 * std::log10(g), -24.0, 12.0) : -24.0);
    set(m, slParam(ik, SlicePan), std::clamp(pan / 6400.0, -1.0, 1.0));
    // The region filter only runs when its articulator group's filter mode is
    // on; every shipped group has it off, so the region cutoff stays neutral.
    (void)cut, (void)res;
    set(m, slParam(ik, SlicePitch), std::clamp(s.i32(r + 40) / 100.0, -12.0, 12.0));
    set(m, slParam2(ik, SliceCut), std::clamp(s.i32(r + 0), 0, 99));
    set(m, slParam2(ik, SliceDelay), std::clamp(s.i32(r + 52) / 12800.0 * 0.5, 0.0, 1.0));  // beats → s at 120 BPM
    static const int kDefault[17] = {0, 0, 0, 8000, 0, 12800, 128, 128, 0, 12800, 0, 0, 12800, 0, 0, 0, 12800};
    for (int f = 0; f < 17; ++f)
      if (f != 0 && f != 2 && f != 3 && f != 10 && f != 13 && s.i32(r + 4 * static_cast<size_t>(f)) != kDefault[f]) regionsDefault = false;
  }
  if (!regionsDefault) c.report.warn("Slicex: region articulator / delay settings are not imported");
  linkParams(m, {{2, SlVolume, [](double n) { return std::clamp((std::pow(11.0, n * 16000.0 / 12800.0) - 1.0) / 10.0, 0.0, 1.0); }},
                 {5, SlPitch, [](double n) { return (-1200.0 + 2400.0 * n) / 100.0; }}},
             {{2, 0, 16000}, {5, -1200, 1200}});
  if (path.empty()) c.report.warn("Slicex: no sample found");
  m.exact = false;  // articulators, per-region envelopes and layering are not imported
  return true;
}


// ------------------------------------------------------------------ Slicex
// Full state (J-41, behaviour study notes Part A): parsed
// whole into the Slicex region model (engine/SlicexEngine.h) that the
// slicer's region engine (SlEngine = 1) plays; the per-region gain / pan /
// pitch / cut group / delay / end and the master knobs are also parameters.

// J-166: regions of a deck exactly as its audio holds them (sx::readWavRegions:
// cue list order, cue positions, "ltxt" lengths / "labl" names by cue id,
// "tlst" keys); one region over the whole sample when there are none. The
// state's region records follow the same order.
void deckRegions(sx::Deck& d, const sx::WavRegions& w) {
  std::vector<sx::Region> regs = w.regions;
  if (regs.empty()) regs.push_back(sx::Region{});
  for (size_t k = 0; k < regs.size() && k < d.regions.size(); ++k) regs[k].rec = d.regions[k].rec;
  d.regions = regs;
}

bool decodeSlicex(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob s = stateBlob(evs);
  if (s.size() < 0x62) return false;
  begin(m, "gl.slicer");
  sx::Model md;
  std::array<std::vector<uint8_t>, 2> riff;
  const bool parsed = sx::parseState(s.p, s.size(), md, riff);
  if (!parsed) return decodeSlicexClassic(c, evs, m);
  bool exact = true;
  std::string path;
  WavInfo wa;
  std::array<size_t, 2> records = {md.decks[0].regions.size(), md.decks[1].regions.size()};
  for (int d = 0; d < 2; ++d) {
    sx::Deck& dk = md.decks[static_cast<size_t>(d)];
    const std::string raw = textBytes(Blob(reinterpret_cast<const uint8_t*>(dk.rawPath.data()), dk.rawPath.size()), 0, dk.rawPath.size());
    WavInfo w;
    sx::WavRegions wr;
    std::string file;
    const auto& rf = riff[static_cast<size_t>(d)];
    if (!rf.empty()) {
      w = wavInfo(Blob(rf.data(), rf.size()));
      if (w.ok && w.frames > 0) {
        file = storeEmbedded(Blob(rf.data(), rf.size()), raw.empty() ? "slicex" : raw);
        wr = sx::readWavRegions(rf.data(), rf.size());
      } else {
        dk.riff = rf;  // an empty deck: kept inline
      }
    }
    if (file.empty() && !raw.empty() && dk.riff.empty()) {
      file = resolveFl(c, raw);
      std::vector<uint8_t> bytes = readAll(file);
      w = wavInfo(Blob(bytes.data(), bytes.size()));
      w.frames = sampleFrames(file, w);
      wr = sx::readWavRegions(bytes.data(), bytes.size());
    }
    dk.file = file;
    if (d == 0) path = file, wa = w;
    if (d == 1 && w.frames > 0) exact = false;  // deck B audio: played, layering by note colour not verified
    deckRegions(dk, wr);
    if (dk.regions.size() != records[static_cast<size_t>(d)] && !(records[static_cast<size_t>(d)] == 1 && wr.regions.empty()))
      c.report.warn("Slicex: region count differs from the sample's cue points");
  }
  // Features whose behaviour is not verified: song-time-synced layering,
  // mapping curves, speed / sample-start articulators.
  if (md.layering() == 7) exact = false;
  for (const auto& g : md.groups)
    for (int t = 0; t < sx::kTargets; ++t) {
      const auto& a = g.arts[static_cast<size_t>(t)];
      const double neutral = (t == sx::TVol || t == sx::TSpeed) ? 1.0 : 0.5;
      for (const auto& mp : a.maps)
        for (const auto& pt : mp.pts)
          if (pt.value != neutral) exact = false;
      if ((t == sx::TSpeed || t == sx::TStart) && (a.env.on() || a.lfo.on())) exact = false;
    }
  // J-176: regions past the 64 slice parameters play from their own records
  // (engine: gain / pan / pitch / cut / delay of the model), so they stay exact.
  m.state.data = path + "\n" + sx::writeText(md);
  set(m, SlEngine, 1);
  // Markers (region starts) for the slicer view, and region end points.
  std::vector<uint32_t> starts;
  for (const auto& r : md.decks[0].regions) starts.push_back(static_cast<uint32_t>(r.start));
  setMarkers(m, starts, wa.frames);
  if (wa.frames > 0)
    for (size_t k = 0; k < md.decks[0].regions.size() && k < static_cast<size_t>(kMaxSlices); ++k) {
      const auto& r = md.decks[0].regions[k];
      if (r.length < 0) continue;
      const uint64_t endF = static_cast<uint64_t>(r.start + r.length);
      const uint64_t next = k + 1 < md.decks[0].regions.size() ? static_cast<uint64_t>(md.decks[0].regions[k + 1].start) : wa.frames;
      if (endF < next) set(m, slParam2(static_cast<int>(k), SliceEnd), static_cast<double>(endF) / static_cast<double>(wa.frames));
    }
  // Master level (11^(v/12800) − 1)/10, pitch (cents), mod X / Y (0..512).
  const double master = sx::masterGain(md.knobs[2]);
  set(m, SlMaster, master);
  set(m, SlVolume, std::clamp(master, 0.0, 1.0));
  set(m, SlPitch, md.knobs[5] / 100.0);
  set(m, SlModX, md.knobs[6] / 512.0);
  set(m, SlModY, md.knobs[7] / 512.0);
  if (master > 2.0 || std::abs(md.knobs[5]) > 2400 || md.knobs[6] < 0 || md.knobs[6] > 512 || md.knobs[7] < 0 || md.knobs[7] > 512) exact = false;
  set(m, SlBaseNote, 60);
  set(m, SlDeclick, 1);
  // Per-region parameters (deck A).
  for (size_t k = 0; k < md.decks[0].regions.size() && k < static_cast<size_t>(kMaxSlices); ++k) {
    const auto& rec = md.decks[0].regions[k].rec;
    const int ik = static_cast<int>(k);
    set(m, slParam2(ik, SliceKey), md.decks[0].regions[k].key);  // J-166: explicit key (−1 = automatic)
    const double g = sx::regionGain(rec[sx::RVolume]);
    const double db = g > 0 ? 20.0 * std::log10(g) : -24.0;
    if (db > 12.0 || (g > 0 && db < -24.0)) exact = false;
    set(m, slParam(ik, SliceGain), std::clamp(db, -24.0, 12.0));
    set(m, slParam(ik, SlicePan), std::clamp(rec[sx::RPan] / 6400.0, -1.0, 1.0));
    set(m, slParam(ik, SlicePitch), std::clamp(rec[sx::RPitch] / 100.0, -24.0, 24.0));
    set(m, slParam2(ik, SliceCut), std::clamp(rec[sx::RCut], 0, 99));
    set(m, slParam2(ik, SliceDelay), std::clamp(rec[sx::RDelay] / 12800.0 * 0.5, 0.0, 1.0));  // beats → s at 120 BPM
    if (rec[sx::ROutput] != 0) exact = false;  // per-region mixer outputs are not routed
  }
  linkParams(m, {{2, SlMaster, [](double n) { return sx::masterGain(static_cast<int32_t>(std::lround(n * 16000.0))); }},
                 {5, SlPitch, [](double n) { return (-1200.0 + 2400.0 * n) / 100.0; }},
                 {6, SlModX, [](double n) { return std::round(n * 512.0) / 512.0; }},
                 {7, SlModY, [](double n) { return std::round(n * 512.0) / 512.0; }}},
             {{2, 0, 16000}, {5, -1200, 1200}, {6, 0, 512}, {7, 0, 512}});
  // An empty deck plays nothing, as in FL: still exact.
  if (path.empty() && md.decks[0].riff.empty()) c.report.warn("Slicex: no sample found");
  m.exact = exact;
  return true;
}

// ----------------------------------------------------------- Fruity Slicer
// u32 version, f32 beats, f32 tempo, i32 pitch (cents), i32 stretch amount
// (ratio 2^(v/10000)), i32 stretch method, i32 fade in (ms), i32 fade out
// (ms), Pascal path, i32 slice count, slices {Pascal name, i32 start frame,
// i32 end (-1 = next), f32 (-1), u8 reversed}, then the tail: u8, i32 base
// note, u8, i32 sample rate, u8, u8 declick, u8 (v16: + settings block).
// J-41: the processing (reverse, declick, stretch, pitch, fades) is the
// slicer's engine 2; exact when nothing is stretched or pitch-shifted (the
// stretch itself is Gridline's own).
bool decodeFruitySlicer(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  using namespace pp;
  Blob s = stateBlob(evs);
  if (s.size() < 37) return false;
  begin(m, "gl.slicer");
  const uint32_t version = s.u32(0);
  size_t p = 0;
  std::string rawPath = pascal(s, 32, &p);
  int32_t count = s.i32(p);
  p += 4;
  struct Rec {
    int32_t start, end;
    float f;
    uint8_t rev;
  };
  std::vector<Rec> recs;
  bool parsed = count >= 0;
  for (int32_t k = 0; k < count && k < 4096; ++k) {
    if (!s.has(p, 1)) {
      parsed = false;
      break;
    }
    size_t q = 0;
    pascal(s, p, &q);  // slice name
    if (!s.has(q, 13)) {
      parsed = false;
      break;
    }
    recs.push_back({s.i32(q), s.i32(q + 4), s.f32(q + 8), s.u8(q + 12)});
    p = q + 13;
  }
  const size_t tail = p;
  int baseNote = s.has(tail + 1, 1) ? s.u8(tail + 1) : 60;
  const bool haveTail = parsed && s.has(tail, 13);
  const bool declick = haveTail ? s.u8(tail + 11) != 0 : true;
  std::string path = resolveFl(c, rawPath);
  m.state.data = path;
  uint64_t frames = 0;
  if (!path.empty()) {
    std::vector<uint8_t> bytes = readAll(path);
    frames = sampleFrames(path, wavInfo(Blob(bytes.data(), bytes.size())));
  }
  std::vector<uint32_t> starts;
  bool sorted = true;
  for (size_t k = 0; k < recs.size(); ++k) {
    if (recs[k].start >= 0) starts.push_back(static_cast<uint32_t>(recs[k].start));
    if (k > 0 && recs[k].start <= recs[k - 1].start) sorted = false;
  }
  std::sort(starts.begin(), starts.end());
  setMarkers(m, starts, frames);
  set(m, SlBaseNote, baseNote > 0 && baseNote < 128 ? baseNote : 60);
  set(m, SlVolume, 1.0);
  const int32_t pitch = s.i32(12), fit = s.i32(16), method = s.i32(20), fin = s.i32(24), fout = s.i32(28);
  // Processed engine: pitch / stretch / fades / declick offline, nothing at play time.
  set(m, SlEngine, SlEngineProcessed);
  set(m, SlProcPitch, std::clamp(pitch, -1200, 1200));
  set(m, SlProcFit, std::clamp(fit, -20000, 20000));
  set(m, SlProcMethod, std::clamp(method, 0, 7));
  set(m, SlProcFadeIn, std::clamp(fin, 0, 1000));
  set(m, SlProcFadeOut, std::clamp(fout, 0, 1000));
  set(m, SlProcDeclick, declick ? 1 : 0);
  set(m, SlDeclick, 0);
  set(m, SlAttack, 0.0005);
  bool anyRev = false;
  for (size_t k = 0; k < recs.size() && k < static_cast<size_t>(kMaxSlices); ++k)
    if (recs[k].rev) set(m, slParam(static_cast<int>(k), SliceReverse), 1), anyRev = true;
  bool plain = true;  // every record: end at the next slice, f32 -1
  for (const Rec& r : recs) plain = plain && r.end == -1 && r.f == -1.0f && r.rev <= 1;
  m.exact = parsed && haveTail && (version == 15 || version == 16) && (frames > 0 || (rawPath.empty() && recs.empty())) && fit == 0 && pitch == 0 && method >= 0 && method <= 7 &&
            fin >= 0 && fin <= 1000 && fout >= 0 && fout <= 1000 && recs.size() <= static_cast<size_t>(kMaxSlices) && plain &&
            (sorted || (!anyRev && recs.size() <= 1)) && (recs.empty() || recs[0].start >= 0) && starts.size() == recs.size() &&
            (starts.empty() || starts.back() < frames);
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
  double speed = ws == 0 ? 0.0 : (ws < 0 ? -1.0 : 1.0) * std::exp2((std::fabs(static_cast<double>(ws)) - gs) / 200.0);
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
