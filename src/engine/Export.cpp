// Gridline — audio export. See Export.h.
#include "Export.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include "../packs/edison/edison.h"
#include "../core/Platform.h"

namespace fs = std::filesystem;

namespace gl {

const char* exportFormatName(ExportFormat f) {
  switch (f) {
    case ExportFormat::Wav: return "WAV";
    case ExportFormat::Flac: return "FLAC";
    case ExportFormat::Mp3: return "MP3";
    case ExportFormat::Ogg: return "Ogg Vorbis";
    case ExportFormat::M4a: return "AAC (.m4a)";
    default: return "?";
  }
}
const char* exportFormatExt(ExportFormat f) {
  switch (f) {
    case ExportFormat::Wav: return ".wav";
    case ExportFormat::Flac: return ".flac";
    case ExportFormat::Mp3: return ".mp3";
    case ExportFormat::Ogg: return ".ogg";
    case ExportFormat::M4a: return ".m4a";
    default: return "";
  }
}
bool exportFormatExternal(ExportFormat f) { return f == ExportFormat::Mp3 || f == ExportFormat::Ogg || f == ExportFormat::M4a; }

const char* ditherName(DitherMode d) {
  switch (d) {
    case DitherMode::None: return "Off";
    case DitherMode::Tpdf: return "Triangular (TPDF)";
    case DitherMode::NoiseShaped: return "Noise-shaped";
    default: return "?";
  }
}
const char* channelModeName(ChannelMode m) {
  switch (m) {
    case ChannelMode::Stereo: return "Stereo";
    case ChannelMode::MonoMerged: return "Mono (merged)";
    case ChannelMode::MonoLeft: return "Mono (left only)";
    case ChannelMode::MonoRight: return "Mono (right only)";
    default: return "?";
  }
}

AudioBuffer applyChannelMode(const AudioBuffer& in, ChannelMode m) {
  if (m == ChannelMode::Stereo || in.channels == 0) return in;
  AudioBuffer out(1, in.frames);
  const float* l = in.plane(0);
  const float* r = in.channels > 1 ? in.plane(1) : in.plane(0);
  float* o = out.plane(0);
  for (size_t i = 0; i < in.frames; ++i)
    o[i] = m == ChannelMode::MonoMerged ? 0.5f * (l[i] + r[i]) : m == ChannelMode::MonoLeft ? l[i] : r[i];
  return out;
}

double normalizeBuffer(AudioBuffer& buf, double targetDb) {
  const float peak = buf.peak();
  if (peak < 1e-9f) return 1.0;
  const double g = std::pow(10.0, targetDb / 20.0) / peak;
  for (float& v : buf.data) v = static_cast<float>(v * g);
  return g;
}

void quantizeBuffer(AudioBuffer& buf, int bits, DitherMode dither, uint32_t seed) {
  if (bits != 16 && bits != 24) return;
  const double full = static_cast<double>(int64_t{1} << (bits - 1));
  const double inv = 1.0 / full;
  // Error feedback filter for noise shaping (noise transfer
  // 1 - 1.623 z^-1 + 0.982 z^-2 - 0.109 z^-3: about -12 dB at DC, +11 dB at Nyquist).
  static constexpr double h[3] = {1.623, -0.982, 0.109};
  for (size_t c = 0; c < buf.channels; ++c) {
    uint32_t s = seed ^ (0x9E3779B9u * static_cast<uint32_t>(c + 1));
    if (!s) s = 1;
    auto uni = [&s]() {  // [0, 1)
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      return static_cast<double>(s) / 4294967296.0;
    };
    double e[3] = {0, 0, 0};
    float* x = buf.plane(c);
    for (size_t i = 0; i < buf.frames; ++i) {
      double u = static_cast<double>(x[i]) * full;
      if (dither == DitherMode::NoiseShaped) u -= h[0] * e[0] + h[1] * e[1] + h[2] * e[2];
      const double d = dither == DitherMode::None ? 0.0 : uni() - uni();
      double q = std::nearbyint(u + d);
      q = std::clamp(q, -full, full - 1);
      if (dither == DitherMode::NoiseShaped) {
        const double err = std::clamp(q - u, -4.0, 4.0);  // bounded (clipping must not blow up the loop)
        e[2] = e[1];
        e[1] = e[0];
        e[0] = err;
      }
      x[i] = static_cast<float>(q * inv);
    }
  }
}

// ---------------------------------------------------------------- WAV

namespace {

void put16(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
  put16(b, v & 0xFFFF);
  put16(b, v >> 16);
}
void putTag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }
void putF32(std::vector<uint8_t>& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put32(b, u);
}

bool fail(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& b, std::string* err) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return fail(err, "cannot write " + path);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return static_cast<bool>(f) || fail(err, "write failed: " + path);
}

}  // namespace

bool writeWavFile(const std::string& path, const AudioBuffer& buf, int sampleRate, int bits, const WavMeta& meta, std::string* err) {
  if (buf.channels == 0) return fail(err, "no audio");
  if (bits != 16 && bits != 24 && bits != 32) bits = 24;
  const bool flt = bits == 32;
  const uint32_t chans = static_cast<uint32_t>(buf.channels);
  const uint32_t bps = static_cast<uint32_t>(bits / 8), align = bps * chans;
  const uint64_t dataBytes = static_cast<uint64_t>(buf.frames) * align;
  if (dataBytes > 0xFFFFFF00ull) return fail(err, "audio too long for a WAV file (4 GB)");
  std::vector<uint8_t> b;
  b.reserve(static_cast<size_t>(dataBytes) + 1024);
  putTag(b, "RIFF");
  put32(b, 0);
  putTag(b, "WAVE");
  putTag(b, "fmt ");
  put32(b, 16);
  put16(b, flt ? 3 : 1);
  put16(b, chans);
  put32(b, static_cast<uint32_t>(sampleRate));
  put32(b, static_cast<uint32_t>(sampleRate) * align);
  put16(b, align);
  put16(b, static_cast<uint32_t>(bits));
  if (meta.tempo > 0) {  // tempo / beats for loop-aware hosts
    putTag(b, "acid");
    put32(b, 24);
    put32(b, meta.loop ? 0x0u : 0x1u);  // bit 0: one-shot
    put16(b, 60);
    put16(b, 0x8000);
    putF32(b, 0.0f);
    put32(b, static_cast<uint32_t>(std::max(0, meta.beats)));
    put16(b, static_cast<uint32_t>(meta.meterDen));
    put16(b, static_cast<uint32_t>(meta.meterNum));
    putF32(b, static_cast<float>(meta.tempo));
  }
  putTag(b, "data");
  put32(b, static_cast<uint32_t>(dataBytes));
  for (size_t i = 0; i < buf.frames; ++i)
    for (uint32_t ch = 0; ch < chans; ++ch) {
      const float x = buf.at(ch, i);
      if (flt) {
        putF32(b, x);
      } else {
        const int32_t q = quantizedInt(x, bits);
        b.push_back(static_cast<uint8_t>(q));
        b.push_back(static_cast<uint8_t>(q >> 8));
        if (bits == 24) b.push_back(static_cast<uint8_t>(q >> 16));
      }
    }
  if (dataBytes & 1) b.push_back(0);
  if (!meta.markers.empty()) {
    putTag(b, "cue ");
    put32(b, static_cast<uint32_t>(4 + 24 * meta.markers.size()));
    put32(b, static_cast<uint32_t>(meta.markers.size()));
    uint32_t id = 1;
    for (const auto& m : meta.markers) {
      put32(b, id++);
      put32(b, static_cast<uint32_t>(std::max<int64_t>(0, m.frame)));
      putTag(b, "data");
      put32(b, 0);
      put32(b, 0);
      put32(b, static_cast<uint32_t>(std::max<int64_t>(0, m.frame)));
    }
    std::vector<uint8_t> l;
    putTag(l, "adtl");
    id = 1;
    for (const auto& m : meta.markers) {
      putTag(l, "labl");
      const uint32_t sz = static_cast<uint32_t>(4 + m.name.size() + 1);
      put32(l, sz);
      put32(l, id++);
      l.insert(l.end(), m.name.begin(), m.name.end());
      l.push_back(0);
      if (sz & 1) l.push_back(0);
    }
    putTag(b, "LIST");
    put32(b, static_cast<uint32_t>(l.size()));
    b.insert(b.end(), l.begin(), l.end());
  }
  if (meta.loopEnd > meta.loopStart && meta.loopStart >= 0) {
    putTag(b, "smpl");
    put32(b, 36 + 24);
    put32(b, 0);
    put32(b, 0);
    put32(b, static_cast<uint32_t>(1e9 / std::max(1, sampleRate)));
    put32(b, 60);
    for (int k = 0; k < 3; ++k) put32(b, 0);
    put32(b, 1);
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    put32(b, static_cast<uint32_t>(meta.loopStart));
    put32(b, static_cast<uint32_t>(meta.loopEnd - 1));
    put32(b, 0);
    put32(b, 0);
  }
  const uint32_t riff = static_cast<uint32_t>(b.size() - 8);
  b[4] = static_cast<uint8_t>(riff);
  b[5] = static_cast<uint8_t>(riff >> 8);
  b[6] = static_cast<uint8_t>(riff >> 16);
  b[7] = static_cast<uint8_t>(riff >> 24);
  return writeBytes(path, b, err);
}

// ---------------------------------------------------------------- external encoders

namespace {

std::mutex& overrideMu() {
  static std::mutex m;
  return m;
}
std::map<std::string, std::string>& overrides() {
  static std::map<std::string, std::string> m;
  return m;
}

std::string findProgram(const std::string& name) {
  {
    std::lock_guard<std::mutex> lk(overrideMu());
    auto it = overrides().find(name);
    if (it != overrides().end()) return it->second;
  }
  std::vector<std::string> dirs;
#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  if (const char* p = std::getenv("PATH")) {
    std::string s = p;
    size_t a = 0;
    while (a <= s.size()) {
      size_t b = s.find(sep, a);
      if (b == std::string::npos) b = s.size();
      if (b > a) dirs.push_back(s.substr(a, b - a));
      a = b + 1;
    }
  }
#if defined(_WIN32)
  for (const auto& d : dirs) {
    std::error_code ec;
    for (const char* ext : {".exe", ".com", ""}) {
      const fs::path f = fs::path(d) / (name + ext);
      if (fs::is_regular_file(f, ec)) return f.string();
    }
  }
#else
  // Apps started from a desktop launcher often get a minimal PATH.
  for (const char* d : {"/usr/local/bin", "/opt/homebrew/bin", "/usr/pkg/bin", "/usr/bin", "/bin", "/opt/local/bin"}) dirs.push_back(d);
  for (const auto& d : dirs) {
    const std::string f = d + "/" + name;
    if (::access(f.c_str(), X_OK) == 0) return f;
  }
#endif
  return "";
}

// Programs that can make a format, in order of preference.
std::vector<std::string> candidates(ExportFormat f) {
  switch (f) {
    case ExportFormat::Mp3: return {"ffmpeg", "lame"};
    case ExportFormat::Ogg: return {"ffmpeg", "oggenc"};
    case ExportFormat::M4a: return {"ffmpeg"};
    default: return {};
  }
}

int run(const std::vector<std::string>& args) {
  platform::Process child;
  if (args.empty() || !child.start(args[0], args)) return -1;
  child.wait();
  return child.status().crashed ? -1 : child.status().code;
}

}  // namespace

void setEncoderOverride(const std::string& program, const std::string& path) {
  std::lock_guard<std::mutex> lk(overrideMu());
  overrides()[program] = path;
}

std::string encoderFor(ExportFormat f) {
  for (const auto& p : candidates(f))
    if (!findProgram(p).empty()) return p;
  return "";
}

std::string encoderStatus(ExportFormat f) {
  if (!exportFormatExternal(f)) return "built in";
  const std::string p = encoderFor(f);
  if (!p.empty()) return p + " (" + findProgram(p) + ")";
  std::string names;
  for (const auto& c : candidates(f)) names += (names.empty() ? "" : " or ") + c;
  return "not installed: needs " + names;
}

namespace {

bool encodeExternal(const std::string& path, const AudioBuffer& buf, int sampleRate, const AudioFileOptions& o, std::string* err) {
  const std::string prog = encoderFor(o.format);
  if (prog.empty()) return fail(err, std::string(exportFormatName(o.format)) + " export needs an encoder program: " + encoderStatus(o.format));
  const std::string exe = findProgram(prog);
  // Intermediate WAV next to the output (same disk), removed afterwards.
  const fs::path out(path);
  const std::string tmp = (out.parent_path() / (".gridline-encode-" + std::to_string(platform::processId()) + "-" + out.stem().string() + ".wav")).string();
  AudioBuffer pcm = buf;
  int bits = 32;
  if (prog != "ffmpeg") {  // lame / oggenc read integer PCM
    bits = prog == "lame" ? 16 : 24;
    quantizeBuffer(pcm, bits, o.dither, o.seed);
  }
  if (!writeWavFile(tmp, pcm, sampleRate, bits, {}, err)) return false;
  const std::string br = std::to_string(std::clamp(o.bitrate, 32, 512));
  std::vector<std::string> args;
  if (prog == "ffmpeg") {
    const char* codec = o.format == ExportFormat::Mp3 ? "libmp3lame" : o.format == ExportFormat::Ogg ? "libvorbis" : "aac";
    args = {exe, "-nostdin", "-y", "-hide_banner", "-loglevel", "error", "-i", tmp, "-c:a", codec, "-b:a", br + "k", path};
  } else if (prog == "lame") {
    args = {exe, "--quiet", "-b", br, tmp, path};
  } else {
    args = {exe, "-Q", "-b", br, "-o", path, tmp};
  }
  const int rc = run(args);
  std::error_code ec;
  fs::remove(tmp, ec);
  if (rc != 0) return fail(err, prog + " failed (exit " + std::to_string(rc) + ")");
  if (!fs::exists(path, ec) || fs::file_size(path, ec) == 0) return fail(err, prog + " wrote no file");
  return true;
}

}  // namespace

bool writeAudioFile(const std::string& path, const AudioBuffer& buf, int sampleRate, const AudioFileOptions& o, std::string* err) {
  if (buf.channels == 0) return fail(err, "no audio");
  switch (o.format) {
    case ExportFormat::Wav: {
      if (o.bits == 32) return writeWavFile(path, buf, sampleRate, 32, o.meta, err);
      AudioBuffer q = buf;
      quantizeBuffer(q, o.bits == 16 ? 16 : 24, o.dither, o.seed);
      return writeWavFile(path, q, sampleRate, o.bits == 16 ? 16 : 24, o.meta, err);
    }
    case ExportFormat::Flac: {
      const int bits = o.bits == 16 ? 16 : 24;
      AudioBuffer q = buf;
      quantizeBuffer(q, bits, o.dither, o.seed);
      packs::edison::Audio a = packs::edison::makeAudio(sampleRate, q.channels > 1 ? 2 : 1, static_cast<int64_t>(q.frames));
      std::copy(q.plane(0), q.plane(0) + q.frames, a.L.begin());
      std::copy(q.plane(q.channels > 1 ? 1 : 0), q.plane(q.channels > 1 ? 1 : 0) + q.frames, a.R.begin());
      return packs::edison::writeFlacFile(path, a, bits, err);
    }
    case ExportFormat::Mp3:
    case ExportFormat::Ogg:
    case ExportFormat::M4a: return encodeExternal(path, buf, sampleRate, o, err);
    default: return fail(err, "unknown format");
  }
}

// ---------------------------------------------------------------- names

std::string safeFileName(const std::string& s) {
  std::string o;
  for (char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    o += (u < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
             ? '_'
             : ch;
  }
  while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
  while (!o.empty() && o.front() == ' ') o.erase(o.begin());
  return o.empty() ? std::string("untitled") : o;
}

std::string expandNamePattern(const std::string& pattern, const std::map<char, std::string>& tokens) {
  std::string out;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] != '%' || i + 1 >= pattern.size()) {
      out += pattern[i];
      continue;
    }
    const char t = pattern[++i];
    if (t == '%') {
      out += '%';
      continue;
    }
    auto it = tokens.find(t);
    if (it != tokens.end()) {
      out += it->second;
    } else if (t == 'D') {
      std::time_t now = std::time(nullptr);
      char d[16];
      std::strftime(d, sizeof d, "%Y-%m-%d", std::localtime(&now));
      out += d;
    }
  }
  return safeFileName(out);
}

}  // namespace gl
