// Gridline — "synths" pack, J-33 group "samplekeys": Sample Keys catalogue
// entry and the sound set loader (the reference's ".wti" sets read in place).
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include "../../core/flp/Flp.h"
#include "../../engine/WavPack.h"
#include "../Packs.h"
#include "samplekeys.h"

namespace gl::packs::synths {

namespace skeys {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

using Ini = std::map<std::string, std::map<std::string, std::string>>;
Ini readIni(const fs::path& p) {
  Ini ini;
  std::ifstream in(p, std::ios::binary);
  std::string line, sec;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';') continue;
    if (line[0] == '[') {
      const size_t e = line.find(']');
      sec = lower(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = lower(trim(line.substr(0, eq)));
    if (!ini[sec].count(k)) ini[sec][k] = trim(line.substr(eq + 1));  // the first entry wins
  }
  return ini;
}
const std::string* get(const Ini& ini, const std::string& sec, const std::string& key) {
  auto s = ini.find(sec);
  if (s == ini.end()) return nullptr;
  auto k = s->second.find(key);
  if (k == s->second.end() || k->second.empty()) return nullptr;
  return &k->second;
}
bool getInt(const Ini& ini, const std::string& sec, const std::string& key, long long& out) {
  const std::string* v = get(ini, sec, key);
  if (!v) return false;
  char* e = nullptr;
  out = std::strtoll(v->c_str(), &e, 10);
  return e != v->c_str();
}
bool truthy(const std::string* v, bool def) { return v ? lower(*v) == "true" || *v == "1" : def; }

fs::path wavDir() {
  const std::string data = flp::defaultFactoryData();
  if (data.empty()) return {};
  return fs::path(data) / "Plugins/Fruity/Generators/FL Keys/Wavtables";
}

// Case-insensitive file lookup inside `dir` (the sets name files loosely).
fs::path findFile(const fs::path& dir, const std::string& rel) {
  fs::path p = dir / rel;
  std::error_code ec;
  if (fs::exists(p, ec)) return p;
  fs::path cur = dir;
  for (const auto& part : fs::path(rel)) {
    bool found = false;
    for (const auto& e : fs::directory_iterator(cur, ec))
      if (lower(e.path().filename().string()) == lower(part.string())) {
        cur = e.path(), found = true;
        break;
      }
    if (!found) return {};
  }
  return cur;
}

std::vector<uint8_t> readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct Wav {
  int channels = 1;
  std::vector<int16_t> pcm;  // interleaved
  long long loopStart = -1, loopEnd = -1;
};

uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

bool readWav(const fs::path& p, Wav& w) {
  const std::vector<uint8_t> d = readAll(p);
  if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
  int fmt = 0, ch = 1, bits = 16;
  const uint8_t* data = nullptr;
  size_t dataLen = 0;
  for (size_t i = 12; i + 8 <= d.size();) {
    const uint32_t n = u32(&d[i + 4]);
    const uint8_t* b = &d[i + 8];
    if (i + 8 + n > d.size()) break;
    if (std::memcmp(&d[i], "fmt ", 4) == 0 && n >= 16) fmt = u16(b), ch = std::max(1, static_cast<int>(u16(b + 2))), bits = u16(b + 14);
    else if (std::memcmp(&d[i], "data", 4) == 0) data = b, dataLen = n;
    else if (std::memcmp(&d[i], "smpl", 4) == 0 && n >= 36 + 24 && u32(b + 28) > 0) w.loopStart = u32(b + 36 + 8), w.loopEnd = u32(b + 36 + 12);
    i += 8 + n + (n & 1);
  }
  if (!data) return false;
  w.channels = std::min(ch, 2);
  if (fmt == 0x674F || fmt == 0x6750 || fmt == 0x6751) {  // Ogg Vorbis in WAV: decoded, then held as 16-bit
    DecodedAudio a;
    if (!decodeOggVorbis(data, dataLen, a, nullptr) || a.channels < 1) return false;
    w.channels = std::min(a.channels, 2);
    const size_t frames = a.frames();
    w.pcm.resize(frames * static_cast<size_t>(w.channels));
    for (size_t f = 0; f < frames; ++f)
      for (int c = 0; c < w.channels; ++c) {
        const float v = a.interleaved[f * static_cast<size_t>(a.channels) + static_cast<size_t>(c)];
        w.pcm[f * static_cast<size_t>(w.channels) + static_cast<size_t>(c)] = static_cast<int16_t>(std::clamp(std::lround(v * 32768.0f), -32768L, 32767L));
      }
    return true;
  }
  const int bps = bits / 8;
  if ((fmt != 1 && fmt != 3 && fmt != 0xFFFE) || bps < 1) return false;
  const size_t frames = dataLen / static_cast<size_t>(bps * ch);
  w.pcm.resize(frames * static_cast<size_t>(w.channels));
  for (size_t f = 0; f < frames; ++f)
    for (int c = 0; c < w.channels; ++c) {
      const uint8_t* s = data + (f * static_cast<size_t>(ch) + static_cast<size_t>(c)) * static_cast<size_t>(bps);
      int v = 0;
      if (fmt == 3 && bits == 32) {
        float x;
        std::memcpy(&x, s, 4);
        v = static_cast<int>(std::clamp(std::lround(x * 32768.0f), -32768L, 32767L));
      } else if (bits == 8) {
        v = (s[0] - 128) << 8;
      } else if (bits == 16) {
        v = static_cast<int16_t>(u16(s));
      } else if (bits == 24) {
        v = static_cast<int16_t>(s[1] | s[2] << 8);
      } else if (bits == 32) {
        v = static_cast<int16_t>(s[2] | s[3] << 8);
      }
      w.pcm[f * static_cast<size_t>(w.channels) + static_cast<size_t>(c)] = static_cast<int16_t>(v);
    }
  return true;
}

struct SetFile {
  std::string name;
  fs::path path;
};

const std::vector<SetFile>& setFiles() {
  static const std::vector<SetFile> files = [] {
    std::vector<SetFile> v;
    const fs::path dir = wavDir();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return v;
    std::string def;
    if (const std::string* f = get(readIni(dir / "default.wti"), "default", "file")) def = lower(*f);
    std::vector<fs::path> wtis;
    for (const auto& e : fs::directory_iterator(dir, ec))
      if (lower(e.path().extension().string()) == ".wti" && lower(e.path().filename().string()) != "default.wti") wtis.push_back(e.path());
    std::sort(wtis.begin(), wtis.end());
    for (const auto& p : wtis) {
      SetFile s;
      s.path = p;
      const Ini ini = readIni(p);
      const std::string* n = get(ini, "info", "name");
      s.name = n ? *n : p.stem().string();
      if (lower(p.filename().string()) == def) v.insert(v.begin(), s);
      else v.push_back(s);
    }
    return v;
  }();
  return files;
}

std::shared_ptr<KeySet> load(const SetFile& f) {
  const Ini ini = readIni(f.path);
  auto k = std::make_shared<KeySet>();
  k->name = f.name;
  long long v = 0;
  const fs::path dir = f.path.parent_path();
  std::string sub;
  if (const std::string* p = get(ini, "info", "path")) sub = *p;
  if (const std::string* r = get(ini, "info", "samplerate")) k->rate = std::atof(r->c_str());
  if (k->rate <= 0) k->rate = 44100;
  if (const std::string* r = get(ini, "info", "velocityfactor")) k->velocityFactor = std::atof(r->c_str());
  k->releaseHigh = truthy(get(ini, "info", "releasehighnotes"), false);
  const bool canLoop = truthy(get(ini, "info", "canloop"), true);
  const bool raw = get(ini, "info", "filetype") && lower(*get(ini, "info", "filetype")) == "raw";
  long long count = 0;
  if (!getInt(ini, "info", "samples", count) || count < 1) return nullptr;
  k->layers = 1;
  if (getInt(ini, "velocitylayers", "count", v) && v > 0) k->layers = static_cast<int>(std::min<long long>(v, 128));
  for (int l = 0; l < k->layers; ++l) k->threshold[l] = getInt(ini, "velocitylayers", std::to_string(l), v) ? static_cast<int>(v) : 128;
  k->threshold[0] = 0;
  // Raw data file shared by all samples.
  std::vector<int16_t> rawData;
  if (raw) {
    const std::string* sf = get(ini, "info", "samplefile");
    if (!sf) return nullptr;
    const fs::path p = findFile(dir, sub.empty() ? *sf : (fs::path(sub) / *sf).string());
    if (p.empty()) return nullptr;
    const std::vector<uint8_t> b = readAll(p);
    rawData.resize(b.size() / 2);
    for (size_t i = 0; i < rawData.size(); ++i) rawData[i] = static_cast<int16_t>(u16(&b[2 * i]));
    k->channels = 1;
  }
  std::map<std::string, Wav> wavs;
  int root = 60, high = 999;
  for (long long i = 0; i < count; ++i) {
    const std::string sec = "sample" + std::to_string(i);
    if (getInt(ini, sec, "root", v)) root = static_cast<int>(v);
    if (getInt(ini, sec, "high", v)) high = static_cast<int>(v);
    KeySample s;
    s.root = std::clamp(root, 0, 127), s.high = std::clamp(high, 0, 999);
    const int16_t* src = nullptr;
    long long total = 0;
    int ch = 1;
    long long pos = 0, end = 0, loop = 0;
    getInt(ini, sec, "pos", pos);
    getInt(ini, sec, "end", end);
    const bool haveLoop = getInt(ini, sec, "loop", loop);
    if (raw) {
      src = rawData.data(), total = static_cast<long long>(rawData.size());
      if (!haveLoop) loop = 0;
    } else {
      const std::string* nm = get(ini, sec, "name");
      if (!nm) return nullptr;
      auto it = wavs.find(*nm);
      if (it == wavs.end()) {
        Wav w;
        const fs::path p = findFile(dir, sub.empty() ? *nm : (fs::path(sub) / *nm).string());
        if (p.empty() || !readWav(p, w)) return nullptr;
        it = wavs.emplace(*nm, std::move(w)).first;
      }
      const Wav& w = it->second;
      ch = w.channels;
      src = w.pcm.data(), total = static_cast<long long>(w.pcm.size()) / ch;
      if (end <= 0) end = w.loopEnd >= 0 ? w.loopEnd : total - 1;
      if (!haveLoop) loop = w.loopStart >= 0 && w.loopEnd >= w.loopStart ? w.loopEnd - w.loopStart + 1 : end - pos + 1;
    }
    if (k->samples.empty()) k->channels = ch;
    if (total <= 0) return nullptr;
    pos = std::clamp(pos, 0LL, total - 1);
    end = std::clamp(std::max(end, pos), pos, total - 1);
    loop = std::clamp(loop, 0LL, total - pos);
    s.loops = canLoop && truthy(get(ini, sec, "canloop"), true) && loop > 0 && loop <= end - pos + 1;
    s.last = static_cast<int>(end - pos);
    s.loop = s.loops ? static_cast<int>(loop) : s.last;
    s.start = k->data.size() / static_cast<size_t>(k->channels);
    auto put = [&](long long frame) {
      for (int c = 0; c < k->channels; ++c) k->data.push_back(src[frame * ch + std::min(c, ch - 1)]);
    };
    for (long long fr = pos; fr <= end; ++fr) put(fr);
    put(std::clamp(pos + (s.last + 1 - s.loop), pos, end));  // guard frame
    k->samples.push_back(s);
  }
  return k;
}

}  // namespace

const std::vector<std::string>& keySetNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (const auto& f : setFiles()) v.push_back(f.name);
    return v;
  }();
  return names;
}

bool keySetInstalled(const std::string& name) {
  for (const auto& f : setFiles())
    if (lower(f.name) == lower(name)) return true;
  return false;
}

std::shared_ptr<const KeySet> keySet(const std::string& name) {
  static std::mutex m;
  static std::map<std::string, std::shared_ptr<const KeySet>> cache;
  const auto& files = setFiles();
  if (files.empty()) return nullptr;
  const SetFile* f = &files[0];
  for (const auto& s : files)
    if (lower(s.name) == lower(name)) f = &s;
  std::lock_guard<std::mutex> lock(m);
  auto it = cache.find(f->name);
  if (it != cache.end()) return it->second;
  std::shared_ptr<const KeySet> k = load(*f);
  cache[f->name] = k;
  return k;
}

}  // namespace skeys

PluginInfo sampleKeysInfo() {
  using namespace skeys;
  static const char* names[SkCount] = {"Decay",  "Release", "Hardness", "Velocity > hardness", "Muffle",  "Velocity > muffle",
                                       "Velocity sensitivity", "Stereo", "Tune", "Detune", "Stretch", "Treble",
                                       "Pan / tremolo", "LFO rate", "Overdrive"};
  std::vector<ParamDesc> q;
  for (int i = 0; i < SkCount; ++i)
    q.push_back(ParamDesc{names[i], "", static_cast<double>(kSkMin[i]), static_cast<double>(kSkMax[i]), static_cast<double>(kSkDefault[i]),
                          ParamKind::Continuous, ParamScale::Linear, {}});
  std::vector<PluginInfo::Preset> presets = {
      {"Default", {}},
      {"Bright", {{SkHardness, 400.f}, {SkMuffle, 1000.f}, {SkTreble, 300.f}}},
      {"Tremolo", {{SkPanTrem, 500.f}, {SkLfoRate, 42000.f}}},
  };
  PluginInfo p{"gl.samplekeys", "Sample Keys", "Synth", PluginKind::Generator, std::move(q), false, std::move(presets)};
  p.editorWidth = 620;
  return p;
}

}  // namespace gl::packs::synths
