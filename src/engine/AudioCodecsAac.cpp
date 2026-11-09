// Gridline - K-51: MPEG-4 audio (.m4a / .mp4 with AAC, HE-AAC or Apple Lossless) and raw AAC
// (ADTS .aac) samples. Gridline has no decoder of its own for these: it asks the computer's.
//   macOS    AudioToolbox (ExtAudioFile): AAC, HE-AAC, ALAC.
//   Windows  Media Foundation (source reader): AAC, HE-AAC, ALAC (Windows 10 and later).
//   others   the FFmpeg program Gridline already runs for Import from URL (mediatools: the copy
//            inside the package, the one Gridline installed, or the system's), as a separate
//            process writing a temporary 32-bit float WAV. No FFmpeg library is linked.
// When the system decoder fails (an old Windows without the codec, a damaged file), FFmpeg is
// tried too. Every route decodes on the calling thread (the sample cache's workers, never the
// audio thread) and gives up when the decode is cancelled (DecodeCancelScope).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../core/MediaTools.h"
#include "../core/Platform.h"
#include "WavPack.h"
#include "../../third_party/dr_libs/dr_wav.h"  // (implemented in Sample.cpp)

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace gl {

namespace fs = std::filesystem;

namespace {

std::atomic<int> gRoute{static_cast<int>(AacRoute::Auto)};

bool fail(std::string* err, const std::string& why) {
  if (err) *err = why;
  return false;
}

#if defined(__APPLE__)
// The richest format the file offers (HE-AAC with its full rate and channels before the AAC
// core of half the rate), else the file's own data format.
bool appleSourceFormat(ExtAudioFileRef f, AudioStreamBasicDescription& out) {
  UInt32 sz = sizeof out;
  if (ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &sz, &out) != noErr) return false;
  AudioFileID af = nullptr;
  sz = sizeof af;
  if (ExtAudioFileGetProperty(f, kExtAudioFileProperty_AudioFile, &sz, &af) != noErr || !af) return true;
  UInt32 listSize = 0;
  if (AudioFileGetPropertyInfo(af, kAudioFilePropertyFormatList, &listSize, nullptr) != noErr || listSize < sizeof(AudioFormatListItem)) return true;
  std::vector<AudioFormatListItem> list(listSize / sizeof(AudioFormatListItem));
  listSize = static_cast<UInt32>(list.size() * sizeof(AudioFormatListItem));
  if (AudioFileGetProperty(af, kAudioFilePropertyFormatList, &listSize, list.data()) != noErr || listSize < sizeof(AudioFormatListItem)) return true;
  const AudioStreamBasicDescription& best = list[0].mASBD;
  if (best.mSampleRate > 0 && best.mChannelsPerFrame > 0) out = best;
  return true;
}

bool decodeSystem(const std::string& path, DecodedAudio& out, std::string* err) {
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false);
  if (!url) return fail(err, "bad path");
  ExtAudioFileRef f = nullptr;
  const OSStatus st = ExtAudioFileOpenURL(url, &f);
  CFRelease(url);
  if (st != noErr || !f) return fail(err, "the system cannot read this file (" + std::to_string(static_cast<long>(st)) + ")");
  struct Close {
    ExtAudioFileRef f;
    ~Close() { ExtAudioFileDispose(f); }
  } closer{f};
  AudioStreamBasicDescription src{};
  if (!appleSourceFormat(f, src) || src.mSampleRate <= 0 || src.mChannelsPerFrame == 0) return fail(err, "no audio format");
  const UInt32 ch = std::min<UInt32>(src.mChannelsPerFrame, 8);
  AudioStreamBasicDescription pcm{};
  pcm.mSampleRate = src.mSampleRate;
  pcm.mFormatID = kAudioFormatLinearPCM;
  pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  pcm.mBitsPerChannel = 32;
  pcm.mChannelsPerFrame = ch;
  pcm.mFramesPerPacket = 1;
  pcm.mBytesPerFrame = 4 * ch;
  pcm.mBytesPerPacket = 4 * ch;
  if (ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof pcm, &pcm) != noErr) return fail(err, "no decoder for this file");
  SInt64 length = 0;
  UInt32 sz = sizeof length;
  if (ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileLengthFrames, &sz, &length) == noErr && length > 0 && length < (int64_t{1} << 32))
    out.interleaved.reserve(static_cast<size_t>(length) * ch);
  constexpr UInt32 kFrames = 8192;
  std::vector<float> block(static_cast<size_t>(kFrames) * ch);
  for (;;) {
    if (decodeCancelled()) return fail(err, "cancelled");
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = ch;
    abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(block.size() * sizeof(float));
    abl.mBuffers[0].mData = block.data();
    UInt32 n = kFrames;
    if (ExtAudioFileRead(f, &n, &abl) != noErr) return fail(err, "decoding failed");
    if (n == 0) break;
    out.interleaved.insert(out.interleaved.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(n * ch));
  }
  out.channels = static_cast<int>(ch);
  out.sampleRate = static_cast<int>(src.mSampleRate + 0.5);
  out.bitsPerSample = 32;
  if (out.interleaved.empty()) return fail(err, "no audio in the file");
  return true;
}
#elif defined(_WIN32)
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

bool decodeSystem(const std::string& path, DecodedAudio& out, std::string* err) {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool coOwned = SUCCEEDED(co);  // (RPC_E_CHANGED_MODE: the thread already has COM)
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    if (coOwned) CoUninitialize();
    return fail(err, "Media Foundation is not available");
  }
  IMFSourceReader* reader = nullptr;
  IMFMediaType* want = nullptr;
  IMFMediaType* got = nullptr;
  bool ok = false;
  std::string why = "the system cannot read this file";
  const DWORD audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
  do {
    if (FAILED(MFCreateSourceReaderFromURL(widen(path).c_str(), nullptr, &reader)) || !reader) break;
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader->SetStreamSelection(audio, TRUE))) break;
    why = "no decoder for this file";
    if (FAILED(MFCreateMediaType(&want))) break;
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(audio, nullptr, want))) break;
    if (FAILED(reader->GetCurrentMediaType(audio, &got))) break;
    UINT32 ch = 0, rate = 0, bits = 0;
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    got->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    if (ch == 0 || rate == 0 || bits != 32) break;
    why = "decoding failed";
    bool good = true;
    for (;;) {
      if (decodeCancelled()) {
        why = "cancelled";
        good = false;
        break;
      }
      DWORD flags = 0;
      IMFSample* sample = nullptr;
      if (FAILED(reader->ReadSample(audio, 0, nullptr, &flags, nullptr, &sample))) {
        good = false;
        break;
      }
      if (sample) {
        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
          BYTE* p = nullptr;
          DWORD len = 0;
          if (SUCCEEDED(buf->Lock(&p, nullptr, &len)) && p) {
            const float* f = reinterpret_cast<const float*>(p);
            out.interleaved.insert(out.interleaved.end(), f, f + len / sizeof(float));
            buf->Unlock();
          }
          release(buf);
        }
        release(sample);
      }
      if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
        good = good && !(flags & MF_SOURCE_READERF_ERROR);
        break;
      }
    }
    if (!good) break;
    out.channels = static_cast<int>(ch);
    out.sampleRate = static_cast<int>(rate);
    out.bitsPerSample = 32;
    out.interleaved.resize(out.interleaved.size() - out.interleaved.size() % ch);
    ok = !out.interleaved.empty();
    if (!ok) why = "no audio in the file";
  } while (false);
  release(got);
  release(want);
  release(reader);
  MFShutdown();
  if (coOwned) CoUninitialize();
  return ok ? true : fail(err, why);
}
#else
bool decodeSystem(const std::string&, DecodedAudio&, std::string* err) { return fail(err, "no system decoder"); }
#endif

// ---- MPEG-4 gapless information (the FFmpeg route trims with it itself) ----
uint64_t be(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}

// Calls f(type, body, bodySize) for every box in [p, end); f returns false to stop.
template <class F>
void eachBox(const uint8_t* p, const uint8_t* end, F f) {
  while (end - p >= 8) {
    uint64_t size = be(p, 4);
    size_t header = 8;
    if (size == 1) {
      if (end - p < 16) return;
      size = be(p + 8, 8);
      header = 16;
    } else if (size == 0) {
      size = static_cast<uint64_t>(end - p);
    }
    if (size < header || size > static_cast<uint64_t>(end - p)) return;
    if (!f(p + 4, p + header, static_cast<size_t>(size - header))) return;
    p += size;
  }
}

bool isType(const uint8_t* t, const char* want) { return std::memcmp(t, want, 4) == 0; }

// The moov box's body, found by walking the file's top-level boxes (it may sit at the end).
std::vector<uint8_t> readMoov(const std::string& path) {
  std::ifstream is(fs::u8path(path), std::ios::binary);
  if (!is) return {};
  is.seekg(0, std::ios::end);
  const uint64_t fileSize = static_cast<uint64_t>(is.tellg());
  uint64_t at = 0;
  for (int guard = 0; guard < 4096 && at + 8 <= fileSize; ++guard) {
    uint8_t h[16];
    is.seekg(static_cast<std::streamoff>(at));
    if (!is.read(reinterpret_cast<char*>(h), 8)) return {};
    uint64_t size = be(h, 4);
    uint64_t header = 8;
    if (size == 1) {
      if (!is.read(reinterpret_cast<char*>(h + 8), 8)) return {};
      size = be(h + 8, 8);
      header = 16;
    } else if (size == 0) {
      size = fileSize - at;
    }
    if (size < header || size > fileSize - at) return {};
    if (isType(h + 4, "moov")) {
      const uint64_t body = size - header;
      if (body > (uint64_t{64} << 20)) return {};
      std::vector<uint8_t> v(static_cast<size_t>(body));
      if (!is.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(body))) return {};
      return v;
    }
    at += size;
  }
  return {};
}

// "iTunSMPB": " 00000000 PPPPPPPP QQQQQQQQ LLLLLLLLLLLLLLLL ..." (hex: priming, padding, length).
bool parseSmpb(const uint8_t* moov, size_t n, Mp4Gapless& g) {
  static const char kName[] = "iTunSMPB";
  const uint8_t* end = moov + n;
  const uint8_t* hit = std::search(moov, end, kName, kName + 8);
  if (hit == end) return false;
  static const char kData[] = "data";
  const uint8_t* d = std::search(hit + 8, end, kData, kData + 4);
  if (d == end || d - moov < 4 || end - d < 12) return false;
  const uint64_t boxSize = be(d - 4, 4);
  if (boxSize < 16 || boxSize - 4 > static_cast<uint64_t>(end - d)) return false;
  const std::string text(reinterpret_cast<const char*>(d + 12), static_cast<size_t>(boxSize - 16));
  uint64_t f[4] = {0, 0, 0, 0};
  int k = 0;
  size_t i = 0;
  while (k < 4 && i < text.size()) {
    while (i < text.size() && !std::isxdigit(static_cast<unsigned char>(text[i]))) ++i;
    size_t j = i;
    uint64_t v = 0;
    while (j < text.size() && std::isxdigit(static_cast<unsigned char>(text[j])) && j - i < 16) {
      const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[j])));
      v = (v << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
      ++j;
    }
    if (j == i) break;
    f[k++] = v;
    i = j;
  }
  if (k < 4 || f[3] == 0) return false;
  g.priming = f[1];
  g.padding = f[2];
  g.length = f[3];
  g.fromTag = true;
  return g.found = true;
}

}  // namespace

Mp4Gapless readMp4Gapless(const std::string& path) {
  Mp4Gapless g;
  const std::vector<uint8_t> moov = readMoov(path);
  if (moov.empty()) return g;
  const uint8_t* b = moov.data();
  const uint8_t* e = b + moov.size();
  uint32_t movieScale = 0;
  bool haveEdit = false;
  uint64_t editStart = 0, editLength = 0;
  bool audioDone = false;
  eachBox(b, e, [&](const uint8_t* t, const uint8_t* body, size_t n) {
    if (isType(t, "mvhd") && n >= 24) movieScale = static_cast<uint32_t>(be(body + (body[0] == 1 ? 20 : 12), 4));
    if (!isType(t, "trak") || audioDone) return true;
    bool sound = false, edit = false;
    uint32_t mediaScale = 0;
    uint64_t start = 0, length = 0;
    eachBox(body, body + n, [&](const uint8_t* t2, const uint8_t* b2, size_t n2) {
      if (isType(t2, "mdia")) {
        eachBox(b2, b2 + n2, [&](const uint8_t* t3, const uint8_t* b3, size_t n3) {
          if (isType(t3, "hdlr") && n3 >= 12) sound = isType(b3 + 8, "soun");
          if (isType(t3, "mdhd") && n3 >= 24) mediaScale = static_cast<uint32_t>(be(b3 + (b3[0] == 1 ? 20 : 12), 4));
          return true;
        });
      } else if (isType(t2, "edts")) {
        eachBox(b2, b2 + n2, [&](const uint8_t* t3, const uint8_t* b3, size_t n3) {
          if (!isType(t3, "elst") || n3 < 8) return true;
          const bool v1 = b3[0] == 1;
          const size_t entry = v1 ? 20 : 12;
          const uint64_t count = be(b3 + 4, 4);
          // The first edit that plays media (an empty edit, media time -1, only delays it).
          for (uint64_t k = 0; k < count && 8 + (k + 1) * entry <= n3; ++k) {
            const uint8_t* p = b3 + 8 + k * entry;
            const uint64_t dur = be(p, v1 ? 8 : 4);
            const uint64_t mt = be(p + (v1 ? 8 : 4), v1 ? 8 : 4);
            const bool empty = v1 ? mt == ~uint64_t{0} : mt == 0xFFFFFFFFu;
            if (empty) continue;
            start = mt, length = dur, edit = true;
            break;
          }
          return true;
        });
      }
      return true;
    });
    if (!sound) return true;
    audioDone = true;
    g.timescale = mediaScale;
    if (edit) haveEdit = true, editStart = start, editLength = length;
    return true;
  });
  // The iTunes tag first: it states priming and length outright (an edit list made from it
  // says the same); else the edit list, whose length is in the movie's timescale.
  if (parseSmpb(b, moov.size(), g)) return g;
  if (haveEdit && movieScale > 0 && g.timescale > 0 && editLength > 0) {
    g.found = true;
    g.priming = editStart;
    g.length = static_cast<uint64_t>(static_cast<long double>(editLength) * g.timescale / movieScale + 0.5L);
  }
  return g;
}

namespace {

// Trims the raw FFmpeg decode (priming and end padding kept) to what the file states.
void trimGapless(DecodedAudio& out, const Mp4Gapless& g) {
  const uint64_t frames = out.frames();
  if (!g.found || frames == 0 || out.channels <= 0) return;
  // Output samples per stated sample: 1, or 2 for HE-AAC (the tag counts the AAC core rate).
  double scale = 1.0;
  if (g.fromTag) {
    const uint64_t sum = g.priming + g.length + g.padding;
    const double r = sum ? static_cast<double>(frames) / static_cast<double>(sum) : 1.0;
    if (r > 1.75 && r < 2.25) scale = 2.0;
    else if (!(r > 0.85 && r < 1.15) && g.timescale > 0) scale = static_cast<double>(out.sampleRate) / g.timescale;
  } else if (g.timescale > 0) {
    scale = static_cast<double>(out.sampleRate) / g.timescale;
  }
  const uint64_t start = static_cast<uint64_t>(static_cast<double>(g.priming) * scale + 0.5);
  uint64_t keep = static_cast<uint64_t>(static_cast<double>(g.length) * scale + 0.5);
  if (start >= frames) return;  // nonsense: keep everything
  keep = std::min(keep, frames - start);
  if (keep == 0) return;
  const size_t ch = static_cast<size_t>(out.channels);
  out.interleaved.erase(out.interleaved.begin(), out.interleaved.begin() + static_cast<std::ptrdiff_t>(start * ch));
  out.interleaved.resize(static_cast<size_t>(keep) * ch);
}

bool isMp4File(const std::string& path) {
  std::ifstream is(fs::u8path(path), std::ios::binary);
  uint8_t h[12] = {};
  return is.read(reinterpret_cast<char*>(h), 12) && std::memcmp(h + 4, "ftyp", 4) == 0;
}

// One FFmpeg run: the first audio stream as 32-bit float WAV into a temporary file. `raw`:
// keep the encoder priming and end padding (no edit list, no automatic skipping).
bool runProgram(const mediatools::Tool& ff, const std::string& path, bool raw, DecodedAudio& out, std::string* err) {
  static std::atomic<unsigned> counter{0};
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec);
  if (ec || dir.empty()) dir = fs::u8path(path).parent_path();
  const fs::path tmp = dir / ("gridline-decode-" + std::to_string(platform::processId()) + "-" + std::to_string(counter.fetch_add(1)) + ".wav");
  struct Remove {
    fs::path p;
    ~Remove() {
      std::error_code e;
      fs::remove(p, e);
    }
  } cleanup{tmp};
  std::vector<std::string> args = ff.argv();
  for (const char* a : {"-nostdin", "-y", "-hide_banner", "-loglevel", "error"}) args.push_back(a);
  if (raw)
    for (const char* a : {"-ignore_editlist", "1", "-flags2", "+skip_manual"}) args.push_back(a);
  args.push_back("-i");
  args.push_back(path);
  for (const char* a : {"-map", "0:a:0", "-vn", "-c:a", "pcm_f32le", "-rf64", "auto", "-f", "wav"}) args.push_back(a);
  args.push_back(tmp.u8string());
  platform::Process child;
  if (!child.start(args[0], args)) return fail(err, "FFmpeg could not start");
  while (!child.wait(0.02)) {
    if (decodeCancelled()) {
      child.kill();
      child.wait();
      return fail(err, "cancelled");
    }
  }
  if (child.status().crashed || child.status().code != 0) return fail(err, "FFmpeg could not decode this file");
  std::vector<uint8_t> bytes;
  {
    std::ifstream is(tmp, std::ios::binary);
    if (is) bytes.assign(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
  }
  unsigned int ch = 0, sr = 0;
  drwav_uint64 n = 0;
  float* data = bytes.empty() ? nullptr : drwav_open_memory_and_read_pcm_frames_f32(bytes.data(), bytes.size(), &ch, &sr, &n, nullptr);
  if (!data) return fail(err, "FFmpeg wrote no audio");
  out.channels = static_cast<int>(ch);
  out.sampleRate = static_cast<int>(sr);
  out.bitsPerSample = 32;
  out.interleaved.assign(data, data + static_cast<size_t>(n) * ch);
  drwav_free(data, nullptr);
  if (ch == 0 || sr == 0 || out.interleaved.empty()) return fail(err, "no audio in the file");
  return true;
}

// The FFmpeg program. An MPEG-4 file that states its priming and length (iTunSMPB or an edit
// list) is decoded raw and trimmed here, to the sample, the same on every FFmpeg version (they
// differ in which of the two they apply and none trims the end padding). Raw AAC (ADTS) says
// neither, so it is decoded as FFmpeg gives it, like the system decoders do.
bool decodeProgram(const std::string& path, DecodedAudio& out, std::string* err) {
  const mediatools::Tool ff = mediatools::locateFfmpeg();
  if (!ff.found()) return fail(err, "this format needs FFmpeg, which was not found");
  const Mp4Gapless g = isMp4File(path) ? readMp4Gapless(path) : Mp4Gapless{};
  if (g.found) {
    if (runProgram(ff, path, true, out, err)) {
      trimGapless(out, g);
      return true;
    }
    if (decodeCancelled()) return false;
    out = DecodedAudio{};  // an FFmpeg without the raw options: as it decodes by itself
  }
  return runProgram(ff, path, false, out, err);
}

}  // namespace

bool looksLikeAacFile(const uint8_t* d, size_t n) {
  if (n >= 12 && std::memcmp(d + 4, "ftyp", 4) == 0) return true;      // MPEG-4 file (.m4a, .mp4, .m4b, .3gp)
  if (n >= 4 && std::memcmp(d, "ADIF", 4) == 0) return true;           // raw AAC with an ADIF header
  return n >= 7 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0;              // ADTS (layer 0: never an MPEG-1/2 audio frame)
}

bool isAacExtension(const std::string& lowerExt) {
  return lowerExt == ".m4a" || lowerExt == ".aac" || lowerExt == ".m4b" || lowerExt == ".mp4" || lowerExt == ".adts";
}

void setAacRouteForTests(AacRoute r) { gRoute.store(static_cast<int>(r)); }

bool decodeAacFile(const std::string& path, DecodedAudio& out, std::string* err) {
  out = DecodedAudio{};
  const auto route = static_cast<AacRoute>(gRoute.load());
  std::string why;
  if (route != AacRoute::Program) {
    if (decodeSystem(path, out, &why)) return true;
    if (decodeCancelled() || route == AacRoute::System) return fail(err, why);
  }
  out = DecodedAudio{};
  std::string programWhy;
  if (decodeProgram(path, out, &programWhy)) return true;
  out = DecodedAudio{};
#if defined(__APPLE__) || defined(_WIN32)
  return fail(err, route == AacRoute::Program ? programWhy : why);
#else
  return fail(err, programWhy);
#endif
}

}  // namespace gl
