// K-51: M4A (AAC, Apple Lossless) and raw AAC (ADTS) samples load. The test makes its own
// files from a WAV tone it writes (afconvert on macOS, the FFmpeg program elsewhere; parts
// whose encoder is missing are skipped and say so), then decodes them through every route the
// platform has (its own decoder, the FFmpeg program) and compares length and level with the
// WAV. Also: files that cannot be decoded fail (the "can't load" state, not silence), a
// cancelled decode stops, the sample cache, and an FL project whose audio clip is an .m4a.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/MediaTools.h"
#include "core/Platform.h"
#include "core/Song.h"
#include "core/flp/Flp.h"
#include "engine/Export.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "engine/WavPack.h"

namespace fs = std::filesystem;
using namespace gl;

namespace {

constexpr int kRate = 44100;
constexpr size_t kFrames = 66150;  // 1.5 s

std::string gDir;

// Left: a rising sweep (200 Hz to 3.2 kHz, never periodic, so a shift of even one sample shows).
double toneL(size_t i) {
  const double t = static_cast<double>(i) / kRate;
  return 0.5 * std::sin(2 * 3.14159265358979 * (200.0 + 1000.0 * t) * t);
}
double toneR(size_t i) { return 0.3 * std::sin(2 * 3.14159265358979 * 660.0 * static_cast<double>(i) / kRate); }

bool run(const std::vector<std::string>& args) {
  platform::Process p;
  if (!p.start(args[0], args)) return false;
  p.wait();
  return !p.status().crashed && p.status().code == 0;
}

double rms(const std::vector<float>& v, size_t from, size_t to) {
  double e = 0;
  size_t n = 0;
  for (size_t i = from; i < to && i < v.size(); ++i, ++n) e += static_cast<double>(v[i]) * v[i];
  return n ? std::sqrt(e / static_cast<double>(n)) : 0.0;
}

struct Encoded {
  std::string path, what;
  bool lossless = false;
};

// The encoded test files this computer can make.
std::vector<Encoded> makeFiles(const std::string& wav16) {
  std::vector<Encoded> out;
  const auto add = [&](const std::string& name, const std::string& what, bool lossless, const std::vector<std::string>& args) {
    if (run(args) && fs::file_size(gDir + "/" + name) > 100) out.push_back({gDir + "/" + name, what, lossless});
    else std::printf("# could not make %s\n", what.c_str());
  };
#if defined(__APPLE__)
  const std::string af = "/usr/bin/afconvert";
  if (fs::exists(af)) {
    add("aac.m4a", "AAC in M4A (afconvert)", false, {af, "-f", "m4af", "-d", "aac", "-b", "192000", wav16, gDir + "/aac.m4a"});
    add("he.m4a", "HE-AAC in M4A (afconvert)", false, {af, "-f", "m4af", "-d", "aach", "-b", "64000", wav16, gDir + "/he.m4a"});
    add("alac.m4a", "ALAC in M4A (afconvert)", true, {af, "-f", "m4af", "-d", "alac", wav16, gDir + "/alac.m4a"});
    add("adts.aac", "AAC in ADTS (afconvert)", false, {af, "-f", "adts", "-d", "aac", "-b", "192000", wav16, gDir + "/adts.aac"});
  }
#endif
  const mediatools::Tool ff = mediatools::locateFfmpeg();
  if (ff.found()) {
    const auto ffArgs = [&](std::vector<std::string> tail) {
      std::vector<std::string> a = ff.argv();
      for (const char* s : {"-nostdin", "-y", "-hide_banner", "-loglevel", "error", "-i"}) a.push_back(s);
      a.push_back(wav16);
      a.insert(a.end(), tail.begin(), tail.end());
      return a;
    };
    add("ff-aac.m4a", "AAC in M4A (FFmpeg)", false, ffArgs({"-c:a", "aac", "-b:a", "192k", gDir + "/ff-aac.m4a"}));
    add("ff-alac.m4a", "ALAC in M4A (FFmpeg)", true, ffArgs({"-c:a", "alac", gDir + "/ff-alac.m4a"}));
    add("ff-adts.aac", "AAC in ADTS (FFmpeg)", false, ffArgs({"-c:a", "aac", "-b:a", "192k", "-f", "adts", gDir + "/ff-adts.aac"}));
  } else {
    std::printf("# FFmpeg not found: its files and its route are skipped\n");
  }
  return out;
}

void checkDecode(const Encoded& f, const std::vector<float>& refL, const std::vector<float>& refR, const char* route) {
  std::string err;
  SamplePtr s = loadSample(f.path, &err);
  const std::string tag = f.what + " via " + route;
  expect(s != nullptr, ("K-51: " + tag + " loads " + err).c_str());
  if (!s) return;
  const long diff = static_cast<long>(s->frames()) - static_cast<long>(kFrames);
  const double r0L = rms(refL, 4096, kFrames - 4096), r0R = rms(refR, 4096, kFrames - 4096);
  const double rL = rms(s->left, 4096, kFrames - 4096), rR = rms(s->right.vec(), 4096, kFrames - 4096);
  const double dbL = 20 * std::log10(rL / r0L), dbR = 20 * std::log10(rR / r0R);
  std::printf("# %s: %zu frames (%+ld), %.0f Hz, level %+.3f / %+.3f dB\n", tag.c_str(), s->frames(), diff, s->sampleRate, dbL, dbR);
  expect(s->sampleRate == kRate, ("K-51: " + tag + ": the sample rate").c_str());
  expect(!s->mono(), ("K-51: " + tag + ": both channels").c_str());
  if (f.lossless) {
    float worst = 0;
    for (size_t i = 0; i < std::min(s->frames(), kFrames); ++i)
      worst = std::max({worst, std::fabs(s->left[i] - refL[i]), std::fabs(s->right[i] - refR[i])});
    expect(diff == 0 && worst < 1e-6f, ("K-51: " + tag + ": every sample as in the WAV").c_str());
  } else {
    // AAC-LC in an MPEG-4 file states its priming and length (iTunSMPB or the edit list): every
    // route gives exactly the WAV's frames, starting on its first sample. ADTS states neither,
    // and HE-AAC is rebuilt from a half-rate core (its waveform is not the source's), so those
    // only roughly.
    const bool adts = f.path.size() > 4 && f.path.compare(f.path.size() - 4, 4, ".aac") == 0;
    const bool he = f.what.rfind("HE-", 0) == 0;
    if (adts || he) {
      expect(std::labs(diff) <= (adts || std::string(route) == "FFmpeg" ? 4096 : 1024), ("K-51: " + tag + ": the length matches the WAV").c_str());
    } else if (s->frames() > 12000) {
      expect(diff == 0, ("K-134: " + tag + ": the length is the WAV's, to the sample").c_str());
      // The best match of the sweep within +-2000 samples is at no shift, and close.
      int best = 0;
      double bestErr = 1e300, ref = 0;
      for (int lag = -2000; lag <= 2000; ++lag) {
        double e = 0;
        for (size_t i = 3000; i < 9000; ++i) {
          const double x = static_cast<double>(s->left[static_cast<size_t>(static_cast<long>(i) + lag)]) - refL[i];
          e += x * x;
        }
        if (e < bestErr) bestErr = e, best = lag;
      }
      for (size_t i = 3000; i < 9000; ++i) ref += static_cast<double>(refL[i]) * refL[i];
      const double snr = 10 * std::log10(ref / std::max(bestErr, 1e-30));
      std::printf("# %s: first samples shifted by %d, %.1f dB apart from the WAV\n", tag.c_str(), best, snr);
      expect(best == 0 && snr > 20, ("K-134: " + tag + ": the first samples line up with the WAV").c_str());
    }
    expect(std::fabs(dbL) < 0.5 && std::fabs(dbR) < 0.5, ("K-51: " + tag + ": the level matches the WAV").c_str());
  }
}

// FLP writer (the container helpers of test_arrangement).
struct Builder {
  std::vector<uint8_t> ev;
  void b(uint8_t id, uint8_t v) { ev.push_back(id), ev.push_back(v); }
  void w(uint8_t id, uint16_t v) { ev.push_back(id), ev.push_back(v & 0xFF), ev.push_back(v >> 8); }
  void d(uint8_t id, uint32_t v) {
    ev.push_back(id);
    for (int i = 0; i < 4; ++i) ev.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void data(uint8_t id, const std::vector<uint8_t>& p) {
    ev.push_back(id);
    size_t n = p.size();
    do {
      uint8_t x = n & 0x7F;
      n >>= 7;
      ev.push_back(static_cast<uint8_t>(x | (n ? 0x80 : 0)));
    } while (n);
    ev.insert(ev.end(), p.begin(), p.end());
  }
  void text(uint8_t id, const std::string& s) {
    std::vector<uint8_t> p;
    for (char ch : s) p.push_back(static_cast<uint8_t>(ch)), p.push_back(0);
    p.push_back(0), p.push_back(0);
    data(id, p);
  }
  std::vector<uint8_t> file(int ppq) const {
    std::vector<uint8_t> f = {'F', 'L', 'h', 'd', 6, 0, 0, 0, 0, 0, 1, 0, static_cast<uint8_t>(ppq), 0, 'F', 'L', 'd', 't'};
    uint32_t n = static_cast<uint32_t>(ev.size());
    for (int i = 0; i < 4; ++i) f.push_back(static_cast<uint8_t>(n >> (8 * i)));
    f.insert(f.end(), ev.begin(), ev.end());
    return f;
  }
};
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
}
void putF(std::vector<uint8_t>& v, size_t at, float x) {
  uint32_t b;
  std::memcpy(&b, &x, 4);
  put32(v, at, b);
}

// An FL project with one audio clip channel playing `sample` from the start of the playlist.
void testFlImport(const std::string& sample) {
  Builder B;
  B.text(199, "25.2.3.5164");
  B.d(156, 150000);
  B.w(65, 1);
  B.data(224, std::vector<uint8_t>{});
  B.w(64, 0), B.b(21, 4), B.text(203, "Tone");
  B.text(196, sample);
  B.w(99, 0);
  std::vector<uint8_t> it(80, 0);
  put32(it, 0, 0);
  it[4] = 0x00, it[5] = 0x50;  // 0x5000: channel item
  it[6] = 0, it[7] = 0;        // channel 0
  put32(it, 8, 384);           // 4 beats
  it[12] = static_cast<uint8_t>(499 & 0xFF), it[13] = static_cast<uint8_t>(499 >> 8);  // track 1
  it[18] = 0x40;
  putF(it, 24, -1.0f), putF(it, 28, -1.0f);
  putF(it, 52, 1.0f);
  B.data(233, it);
  const std::string path = gDir + "/m4a-clip.flp";
  {
    const auto bytes = B.file(96);
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  Song s;
  flp::Report rep;
  std::string err;
  const bool ok = importFlp(path, s, rep, &err, "/nonexistent");
  expect(ok, ("K-51: the FL project imports " + err).c_str());
  const Channel* tone = nullptr;
  for (const auto& c : s.channels)
    if (c.name == "Tone") tone = &c;
  expect(tone && tone->plugin.type == "gl.sampler" && fs::path(tone->plugin.data) == fs::path(sample), "K-51: FL import: the clip's channel keeps the .m4a");
  bool clip = false;
  for (const auto& c : s.playlist) clip = clip || c.kind == ClipKind::Audio;
  expect(clip, "K-51: FL import: the audio clip is on the playlist");
  RenderOptions o;
  const AudioBuffer out = renderSong(s, o);
  std::printf("# FL import render: %zu frames, peak %.3f\n", out.frames, static_cast<double>(out.peak()));
  expect(out.peak() > 0.05f, "K-51: FL import: the .m4a clip plays (not silence)");
}


// A minimal MPEG-4 file for the gapless reader: ftyp, mdat, then moov (at the end, as streamed
// files have it) with a video track first and the audio track second.
std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& body, bool large = false) {
  // The header is written into a sized vector by index (GCC 12 and 13 report a false
  // stringop-overflow for push_back into an empty vector here).
  const size_t head = large ? 16 : 8;
  const uint64_t n = body.size() + head;
  std::vector<uint8_t> v(head + body.size());
  const auto u32 = [&](size_t at, uint64_t x) {
    for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * (3 - i)));
  };
  u32(0, large ? 1 : n);
  for (size_t i = 0; i < 4; ++i) v[4 + i] = static_cast<uint8_t>(type[i]);
  if (large) {
    u32(8, n >> 32);
    u32(12, n & 0xFFFFFFFFu);
  }
  std::copy(body.begin(), body.end(), v.begin() + static_cast<std::ptrdiff_t>(head));
  return v;
}
std::vector<uint8_t> cat(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> v;
  for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
  return v;
}
std::vector<uint8_t> be32v(uint32_t x) { return {static_cast<uint8_t>(x >> 24), static_cast<uint8_t>(x >> 16), static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x)}; }
std::vector<uint8_t> be64v(uint64_t x) { return cat({be32v(static_cast<uint32_t>(x >> 32)), be32v(static_cast<uint32_t>(x))}); }
std::vector<uint8_t> fullHeader(uint8_t version) { return {version, 0, 0, 0}; }
std::vector<uint8_t> track(const char* handler, uint32_t mediaScale, const std::vector<uint8_t>& elst) {
  const auto mdhd = box("mdhd", cat({fullHeader(0), be32v(0), be32v(0), be32v(mediaScale), be32v(0), be32v(0)}));
  const auto hdlr = box("hdlr", cat({fullHeader(0), be32v(0), {static_cast<uint8_t>(handler[0]), static_cast<uint8_t>(handler[1]), static_cast<uint8_t>(handler[2]), static_cast<uint8_t>(handler[3])}, be32v(0), be32v(0), be32v(0), {0}}));
  std::vector<uint8_t> trak = box("mdia", cat({mdhd, hdlr}));
  if (!elst.empty()) trak = cat({box("edts", box("elst", elst)), trak});
  return box("trak", trak);
}
void writeBytes(const std::string& path, const std::vector<uint8_t>& v) {
  std::ofstream o(path, std::ios::binary);
  o.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size()));
}

void testGaplessReader() {
  const auto ftyp = box("ftyp", {'M', '4', 'A', ' ', 0, 0, 0, 0, 'M', '4', 'A', ' '});
  const auto mdat = box("mdat", std::vector<uint8_t>(3000, 0x11), true);
  const auto mvhd = box("mvhd", cat({fullHeader(0), be32v(0), be32v(0), be32v(1000), be32v(0), std::vector<uint8_t>(80, 0)}));
  // Version 1 edit list: an empty edit (a delay), then the media from sample 1024 for 1.5 s.
  const auto elst1 = cat({fullHeader(1), be32v(2), be64v(10), be64v(~uint64_t{0}), be32v(0x10000), be64v(1500), be64v(1024), be32v(0x10000)});
  const auto video = track("vide", 600, cat({fullHeader(0), be32v(1), be32v(1500), be32v(7), be32v(0x10000)}));
  {
    const std::string path = gDir + "/gapless-elst.m4a";
    writeBytes(path, cat({ftyp, mdat, box("moov", cat({mvhd, video, track("soun", 44100, elst1)}))}));
    const Mp4Gapless g = readMp4Gapless(path);
    std::printf("# edit list: found %d, priming %llu, length %llu, timescale %u\n", g.found, static_cast<unsigned long long>(g.priming), static_cast<unsigned long long>(g.length), g.timescale);
    expect(g.found && !g.fromTag && g.priming == 1024 && g.length == 66150 && g.timescale == 44100, "K-134: the edit list of the audio track (version 1, after an empty edit, moov at the end, a 64-bit box)");
  }
  {
    // The iTunes tag wins over an edit list, and counts in its own units.
    const std::string smpb = " 00000000 00000840 0000028D 0000000000008133 00000000 00000000";
    std::vector<uint8_t> data = cat({be32v(1), be32v(0)});
    data.insert(data.end(), smpb.begin(), smpb.end());
    const auto item = box("----", cat({box("mean", cat({be32v(0), {'c', 'o', 'm', '.', 'a', 'p', 'p', 'l', 'e', '.', 'i', 'T', 'u', 'n', 'e', 's'}})),
                                         box("name", cat({be32v(0), {'i', 'T', 'u', 'n', 'S', 'M', 'P', 'B'}})), box("data", data)}));
    const auto udta = box("udta", box("meta", cat({fullHeader(0), box("ilst", item)})));
    const std::string path = gDir + "/gapless-tag.m4a";
    writeBytes(path, cat({ftyp, box("moov", cat({mvhd, track("soun", 22050, elst1), udta})), mdat}));
    const Mp4Gapless g = readMp4Gapless(path);
    expect(g.found && g.fromTag && g.priming == 2112 && g.padding == 653 && g.length == 33075, "K-134: the iTunes gapless tag");
  }
  {
    // No edit list and no tag: nothing to trim. A damaged box size stops the reader cleanly.
    const std::string path = gDir + "/gapless-none.m4a";
    writeBytes(path, cat({ftyp, box("moov", cat({mvhd, track("soun", 44100, {})})), mdat}));
    expect(!readMp4Gapless(path).found, "K-134: a file that states no priming is not trimmed");
    std::vector<uint8_t> bad = cat({ftyp, box("moov", cat({mvhd, track("soun", 44100, elst1)}))});
    bad[ftyp.size()] = 0x7F;  // moov longer than the file
    writeBytes(path, bad);
    expect(!readMp4Gapless(path).found, "K-134: a damaged file states nothing");
    expect(!readMp4Gapless(gDir + "/no-such-file.m4a").found, "K-134: a missing file states nothing");
  }
}

}  // namespace

int main() {
  gDir = gl::test::hermeticTestEnv("k51");
  expect(isAudioFile("x.m4a") && isAudioFile("x.aac") && isAudioFile("X.M4A"), "K-51: .m4a and .aac are audio files");
  {
    const uint8_t mp4[12] = {0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '};
    const uint8_t adts[7] = {0xFF, 0xF1, 0x50, 0x80, 0x02, 0x1F, 0xFC};
    const uint8_t mp3[7] = {0xFF, 0xFB, 0x90, 0x64, 0, 0, 0};
    expect(looksLikeAacFile(mp4, 12) && looksLikeAacFile(adts, 7) && !looksLikeAacFile(mp3, 7), "K-51: MPEG-4 and ADTS recognised, MP3 frames not");
  }
  // The reference tone, 16-bit (what the encoders read; ALAC keeps it exactly).
  AudioBuffer tone(2, kFrames);
  for (size_t i = 0; i < kFrames; ++i) tone.at(0, i) = static_cast<float>(toneL(i)), tone.at(1, i) = static_cast<float>(toneR(i));
  const std::string wav = gDir + "/tone.wav";
  std::string err;
  expect(writeWavFile(wav, tone, kRate, 16, {}, &err), ("K-51: the reference WAV is written " + err).c_str());
  SamplePtr ref = loadSample(wav, &err);
  expect(ref && ref->frames() == kFrames, "K-51: the reference WAV reads back");
  if (!ref) return finish();
  testGaplessReader();
  const std::vector<float> refL = ref->left, refR = ref->right.vec();

  const std::vector<Encoded> files = makeFiles(wav);
  const bool ffmpeg = mediatools::locateFfmpeg().found();
#if defined(__APPLE__) || defined(_WIN32)
  const bool system = true;
#else
  const bool system = false;
#endif
  // Without an encoder (no afconvert, no FFmpeg: most Linux and BSD systems) nothing can be made:
  // the decode checks are skipped (TAP skip), the checks of damaged files below still run.
#if defined(__APPLE__)
  const bool encoder = ffmpeg || fs::exists("/usr/bin/afconvert");
#else
  const bool encoder = ffmpeg;
#endif
  if (encoder) expect(!files.empty(), "K-51: test files were made (afconvert or FFmpeg)");
  else std::printf("ok %d - K-51: test files were made # SKIP no encoder here (afconvert or FFmpeg)\n", ++checks);
  for (const Encoded& f : files) {
    if (system) {
      setAacRouteForTests(AacRoute::System);
      checkDecode(f, refL, refR, "the system decoder");
    }
    if (ffmpeg) {
      setAacRouteForTests(AacRoute::Program);
      checkDecode(f, refL, refR, "FFmpeg");
    }
    setAacRouteForTests(AacRoute::Auto);
  }
  if (!files.empty()) {
    const std::string m4a = files[0].path;
    // The sample cache (browser preview, waveforms, channels) reads it too.
    SamplePtr cached = sampleCache().get(m4a);
    expect(cached && cached->frames() > kFrames / 2, "K-51: the sample cache loads an .m4a");
    // A cancelled decode stops with nothing.
    {
      std::atomic<bool> stop{true};
      DecodeCancelScope scope(&stop);
      std::string why;
      for (AacRoute r : {AacRoute::System, AacRoute::Program}) {
        if ((r == AacRoute::System && !system) || (r == AacRoute::Program && !ffmpeg)) continue;
        setAacRouteForTests(r);
        expect(loadSample(m4a, &why) == nullptr && why == "cancelled", "K-51: a cancelled decode gives up");
      }
      setAacRouteForTests(AacRoute::Auto);
    }
    testFlImport(m4a);
  }
  // A damaged file and a file that only claims the name: the "can't load" state, not silence.
  {
    std::vector<uint8_t> junk = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 0, 0, 0, 0, 'M', '4', 'A', ' ', 'i', 's', 'o', 'm'};
    for (int i = 0; i < 4000; ++i) junk.push_back(static_cast<uint8_t>((i * 37) ^ 0x5A));
    for (const char* name : {"broken.m4a", "broken.aac"}) {
      std::ofstream o(gDir + "/" + name, std::ios::binary);
      o.write(reinterpret_cast<const char*>(junk.data()), static_cast<std::streamsize>(junk.size()));
    }
    for (const char* name : {"broken.m4a", "broken.aac"}) {
      std::string why;
      SamplePtr s = loadSample(gDir + "/" + name, &why);
      std::printf("# %s: %s\n", name, why.c_str());
      expect(s == nullptr && !why.empty(), (std::string("K-51: ") + name + " fails with a reason").c_str());
    }
  }
#if !defined(__APPLE__) && !defined(_WIN32)
  // Without FFmpeg (an override to a program that is not there) an .m4a cannot load: it says so.
  if (!files.empty()) {
    platform::setEnv("GRIDLINE_FFMPEG", (gDir + "/no-ffmpeg-here").c_str());
    std::string why;
    expect(loadSample(files[0].path, &why) == nullptr && why.find("FFmpeg") != std::string::npos, "K-51: no FFmpeg: the file fails and says what it needs");
    platform::unsetEnv("GRIDLINE_FFMPEG");
  }
#endif
  return finish();
}
