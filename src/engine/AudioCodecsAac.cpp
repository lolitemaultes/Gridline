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

// The FFmpeg program: the first audio stream as 32-bit float WAV into a temporary file.
bool decodeProgram(const std::string& path, DecodedAudio& out, std::string* err) {
  const mediatools::Tool ff = mediatools::locateFfmpeg();
  if (!ff.found()) return fail(err, "this format needs FFmpeg, which was not found");
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
  for (const char* a : {"-nostdin", "-y", "-hide_banner", "-loglevel", "error", "-i"}) args.push_back(a);
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
