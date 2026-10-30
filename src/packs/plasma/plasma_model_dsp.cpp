// Gridline — "plasma" pack: the data model kept in PluginState::data (JSON):
// custom wavetables, drawn LFO shapes and paths, velocity / note graphs, matrix
// curves, LFO point modulation, effect rack layout, arpeggiator and clip banks,
// voice-control sequence, macro names, tuning. Also the point-curve helpers,
// the factory LFO shapes and arpeggiator note orders.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include "../../core/Json.h"
#include "../../core/Platform.h"
#include "plasma.h"

namespace gl::packs::plasma {

// ------------------------------------------------------------------ curves
double bendCurve(double t, double c) {
  t = std::clamp(t, 0.0, 1.0);
  if (std::fabs(c) < 1e-6) return t;
  // exponential bend: c > 0 bows upwards (fast start), c < 0 downwards
  const double k = c * 8.0;
  return (1.0 - std::exp(-k * t)) / (1.0 - std::exp(-k));
}

double curveAt(const Curve& c, double x) {
  if (c.empty()) return std::clamp(x, 0.0, 1.0);
  if (x <= c.front().x) return c.front().y;
  for (size_t i = 1; i < c.size(); ++i) {
    const CurvePt& a = c[i - 1];
    const CurvePt& b = c[i];
    if (x <= b.x) {
      const double w = b.x - a.x;
      if (w <= 1e-9) return b.y;
      const double t = bendCurve((x - a.x) / w, b.c);
      return a.y + (b.y - a.y) * t;
    }
  }
  return c.back().y;
}

Curve linearCurve() { return {{0, 0, 0}, {1, 1, 0}}; }

Curve lfoFactoryShape(int shape) {
  switch (shape) {
    case 1: return {{0, 0, 0}, {0.5f, 1, 0}, {1, 0, 0}};                       // triangle
    case 2: return {{0, 0.5f, 0}, {0.25f, 1, 0.45f}, {0.5f, 0.5f, -0.45f}, {0.75f, 0, 0.45f}, {1, 0.5f, -0.45f}};  // sine-ish
    case 3: return {{0, 0, 0}, {1, 1, 0}};                                     // ramp up
    case 4: return {{0, 1, 0}, {1, 0, 0}};                                     // ramp down
    case 5: return {{0, 1, 0}, {0.5f, 1, 0}, {0.5f, 0, 0}, {1, 0, 0}};         // square
    case 6: return {{0, 1, 0}, {0.25f, 1, 0}, {0.25f, 0, 0}, {1, 0, 0}};       // pulse 25
    case 7: return {{0, 1, 0}, {1, 0, 0.7f}};                                  // exp down
    case 8: return {{0, 0, 0}, {1, 1, -0.7f}};                                 // exp up
    case 9: return {{0, 0, 0}, {0.5f, 1, 0.6f}, {1, 0, -0.6f}};                // bounce
    case 10: return {{0, 0, 0}, {0.25f, 0, 0}, {0.25f, 0.333f, 0}, {0.5f, 0.333f, 0}, {0.5f, 0.667f, 0}, {0.75f, 0.667f, 0}, {0.75f, 1, 0}, {1, 1, 0}};
    case 11: {
      Curve c;
      const float v[8] = {0.f, 0.57f, 0.29f, 1.f, 0.14f, 0.71f, 0.43f, 0.86f};
      for (int i = 0; i < 8; ++i) {
        c.push_back({i / 8.f, v[i], 0});
        c.push_back({(i + 1) / 8.f, v[i], 0});
      }
      return c;
    }
    case 12: {
      Curve c;
      for (int i = 0; i < 8; ++i) {
        c.push_back({i / 8.f, i / 7.f, 0});
        c.push_back({(i + 1) / 8.f, i / 7.f, 0});
      }
      return c;
    }
    case 13: return {{0, 0, 0}, {0.5f, 1, 0.5f}, {1, 0, -0.5f}};               // half sine
    case 14: return {{0, 0, 0}, {0.02f, 1, 0}, {1, 0, 0.75f}};                 // pump (sidechain-like)
    case 15: return {{0, 0, 0}, {0.15f, 1, -0.5f}, {1, 0, 0.5f}};
    case 16: return {{0, 1, 0}, {0.5f, 1, 0}, {0.5f, 0, 0}, {1, 0, 0}};        // gate 1/2
    case 17: return {{0, 1, 0}, {0.375f, 1, 0}, {0.375f, 0, 0}, {0.5f, 0, 0}, {0.5f, 1, 0}, {0.875f, 1, 0}, {0.875f, 0, 0}, {1, 0, 0}};
    case 18: {
      Curve c;
      const bool on[8] = {true, false, true, true, false, true, false, true};
      for (int i = 0; i < 8; ++i) {
        const float y = on[i] ? 1.f : 0.f;
        c.push_back({i / 8.f, y, 0});
        c.push_back({(i + 0.85f) / 8.f, y, 0});
        c.push_back({(i + 0.85f) / 8.f, 0, 0});
        c.push_back({(i + 1) / 8.f, 0, 0});
      }
      return c;
    }
    case 19: return {{0, 0.5f, 0}, {0.125f, 1, 0.5f}, {0.375f, 0, 0}, {0.625f, 1, 0}, {0.875f, 0, 0}, {1, 0.5f, -0.5f}};
    case 20: return {{0, 0, 0}, {0.8f, 1, -0.6f}, {1, 0, 0}};                  // shark fin
    case 21: return {{0, 0, 0}, {0.25f, 1, 0.5f}, {0.5f, 0, -0.5f}, {0.75f, 0.6f, 0.5f}, {1, 0, -0.5f}};
    case 22: return {{0, 0.3f, 0}, {0.2f, 0.9f, 0.4f}, {0.45f, 0.1f, -0.4f}, {0.7f, 0.7f, 0.4f}, {1, 0.3f, -0.4f}};
    case 23: return {{0, 0.6f, 0}, {0.25f, 0.6f, 0}, {0.25f, 0.1f, 0}, {0.5f, 0.1f, 0}, {0.5f, 0.9f, 0}, {0.75f, 0.9f, 0}, {0.75f, 0.35f, 0}, {1, 0.35f, 0}};
    default: return {{0, 0, 0}, {0.5f, 1, 0}, {1, 0, 0}};
  }
}

// ------------------------------------------------------------------ base64 (float frames as 16-bit)
namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64(const std::vector<uint8_t>& in) {
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) | in[i + 2];
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = static_cast<uint32_t>(in[i]) << 16;
    if (i + 1 < in.size()) v |= static_cast<uint32_t>(in[i + 1]) << 8;
    out += kB64[(v >> 18) & 63];
    out += kB64[(v >> 12) & 63];
    out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}
std::vector<uint8_t> unb64(const std::string& s) {
  std::vector<uint8_t> out;
  out.reserve(s.size() / 4 * 3);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else continue;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
    }
  }
  return out;
}
std::string framesToText(const std::vector<std::vector<float>>& frames) {
  std::vector<uint8_t> raw;
  raw.reserve(frames.size() * kFrameSize * 2);
  for (const auto& f : frames)
    for (int i = 0; i < kFrameSize; ++i) {
      const float x = i < static_cast<int>(f.size()) ? f[static_cast<size_t>(i)] : 0.f;
      const int v = static_cast<int>(std::lround(std::clamp(x, -1.f, 1.f) * 32767.f));
      const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(v));
      raw.push_back(static_cast<uint8_t>(u & 0xFF));
      raw.push_back(static_cast<uint8_t>(u >> 8));
    }
  return b64(raw);
}
std::vector<std::vector<float>> framesFromText(const std::string& s) {
  const auto raw = unb64(s);
  const size_t per = kFrameSize * 2;
  const size_t n = std::min<size_t>(raw.size() / per, 256);
  std::vector<std::vector<float>> out(n, std::vector<float>(kFrameSize));
  for (size_t f = 0; f < n; ++f)
    for (size_t i = 0; i < kFrameSize; ++i) {
      const size_t o = f * per + i * 2;
      const int16_t v = static_cast<int16_t>(static_cast<uint16_t>(raw[o] | (raw[o + 1] << 8)));
      out[f][i] = static_cast<float>(v) / 32767.f;
    }
  return out;
}

// ------------------------------------------------------------------ JSON helpers
Json num(double v) { return Json::makeNumber(v); }
Json str(const std::string& v) { return Json::makeString(v); }
Json boolean(bool b) {
  Json j;
  j.type = Json::Type::Bool;
  j.boolean = b;
  return j;
}
bool getBool(const Json& o, const char* k, bool d) {
  const Json* v = o.find(k);
  if (!v) return d;
  if (v->is(Json::Type::Bool)) return v->boolean;
  if (v->is(Json::Type::Number)) return v->number != 0;
  return d;
}
int getInt(const Json& o, const char* k, int d, int lo, int hi) {
  const Json* v = o.find(k);
  if (!v || !v->is(Json::Type::Number)) return d;
  return std::clamp(jsonInt<int>(v->number), lo, hi);
}
float getF(const Json& o, const char* k, float d, float lo, float hi) {
  const Json* v = o.find(k);
  if (!v || !v->is(Json::Type::Number) || !std::isfinite(v->number)) return d;
  return std::clamp(static_cast<float>(v->number), lo, hi);
}

Json curveJson(const Curve& c) {
  Json a = Json::makeArray();
  for (const auto& p : c) {
    a.array.push_back(num(p.x));
    a.array.push_back(num(p.y));
    a.array.push_back(num(p.c));
  }
  return a;
}
Curve curveFrom(const Json* j) {
  Curve c;
  if (!j || !j->is(Json::Type::Array)) return c;
  const size_t n = std::min<size_t>(j->array.size() / 3, 512);
  for (size_t i = 0; i < n; ++i) {
    const auto& x = j->array[i * 3], &y = j->array[i * 3 + 1], &k = j->array[i * 3 + 2];
    if (!x.is(Json::Type::Number) || !y.is(Json::Type::Number) || !k.is(Json::Type::Number)) continue;
    auto fin = [](double v, double lo, double hi) { return std::isfinite(v) ? static_cast<float>(std::clamp(v, lo, hi)) : 0.f; };
    CurvePt p{fin(x.number, 0, 1), fin(y.number, 0, 1), fin(k.number, -1, 1)};
    if (!c.empty() && p.x < c.back().x) p.x = c.back().x;  // keep x sorted
    c.push_back(p);
  }
  return c;
}
Json floats(const std::vector<float>& v) {
  Json a = Json::makeArray();
  for (float f : v) a.array.push_back(num(f));
  return a;
}
std::vector<float> floatsFrom(const Json* j, size_t maxN, float lo, float hi) {
  std::vector<float> v;
  if (!j || !j->is(Json::Type::Array)) return v;
  for (const auto& e : j->array) {
    if (v.size() >= maxN) break;
    if (e.is(Json::Type::Number) && std::isfinite(e.number)) v.push_back(std::clamp(static_cast<float>(e.number), lo, hi));
  }
  return v;
}

// ------------------------------------------------------------------ embedded media
constexpr const char kMediaPrefix[] = "media:";
uint64_t fnv(uint64_t h, const void* p, size_t n) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
std::string hex16(uint64_t h) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}
std::string addEntry(Model& m, Media e) {
  for (const auto& x : m.media)
    if (x.id == e.id) return kMediaPrefix + e.id;
  m.media.push_back(std::move(e));
  return kMediaPrefix + m.media.back().id;
}
// 32-bit float WAV (keeps levels above full scale).
bool writeFloatWav(const std::string& path, const std::vector<std::vector<float>>& ch, int rate) {
  const uint16_t nc = static_cast<uint16_t>(ch.size());
  const uint32_t frames = static_cast<uint32_t>(ch.empty() ? 0 : ch[0].size());
  std::string out;
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
  };
  auto u16 = [&](uint16_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>(v >> 8);
  };
  const uint32_t data = frames * nc * 4u;
  out += "RIFF";
  u32(36 + data);
  out += "WAVEfmt ";
  u32(16), u16(3), u16(nc), u32(static_cast<uint32_t>(rate)), u32(static_cast<uint32_t>(rate) * nc * 4u), u16(static_cast<uint16_t>(nc * 4)), u16(32);
  out += "data";
  u32(data);
  out.reserve(out.size() + data);
  for (uint32_t i = 0; i < frames; ++i)
    for (uint16_t c = 0; c < nc; ++c) {
      uint32_t u;
      std::memcpy(&u, &ch[c][i], 4);
      u32(u);
    }
  // written under a temporary name, then moved into place (another thread may extract the same entry)
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(std::filesystem::u8path(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
  if (ec) std::filesystem::remove(std::filesystem::u8path(tmp), ec);
  return !ec;
}
}  // namespace

std::string mediaCacheDir() {
  if (const char* e = std::getenv("GRIDLINE_CACHE_DIR"); e && *e) return std::string(e) + "/plasma-media";
  if (const std::string c = platform::cacheRoot(); !c.empty()) return c + "/gridline/plasma-media";
  std::error_code ec;
  return (std::filesystem::temp_directory_path(ec) / "gridline-plasma-media").string();
}

bool isMediaRef(const std::string& ref) { return ref.compare(0, sizeof(kMediaPrefix) - 1, kMediaPrefix) == 0; }

const Media* findMedia(const Model& m, const std::string& ref) {
  if (!isMediaRef(ref)) return nullptr;
  const std::string id = ref.substr(sizeof(kMediaPrefix) - 1);
  for (const auto& e : m.media)
    if (e.id == id) return &e;
  return nullptr;
}

std::string addMedia(Model& m, const std::string& name, int rate, const std::vector<std::vector<float>>& channels) {
  Media e;
  e.name = name;
  e.rate = std::clamp(rate, 1000, 384000);
  e.channels = static_cast<int>(std::clamp<size_t>(channels.size(), 1, 2));
  e.frames = channels.empty() ? 0 : static_cast<int64_t>(channels[0].size());
  float peak = 0;
  for (int c = 0; c < e.channels && c < static_cast<int>(channels.size()); ++c)
    for (float v : channels[static_cast<size_t>(c)])
      if (std::isfinite(v)) peak = std::max(peak, std::fabs(v));
  e.peak = peak > 0 ? peak : 1.f;
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(e.frames) * static_cast<size_t>(e.channels) * 2);
  for (int64_t i = 0; i < e.frames; ++i)
    for (int c = 0; c < e.channels; ++c) {
      const auto& ch = channels[static_cast<size_t>(std::min<int>(c, static_cast<int>(channels.size()) - 1))];
      const float x = static_cast<size_t>(i) < ch.size() && std::isfinite(ch[static_cast<size_t>(i)]) ? ch[static_cast<size_t>(i)] / e.peak : 0.f;
      const int v = static_cast<int>(std::lround(std::clamp(x, -1.f, 1.f) * 32767.f));
      const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(v));
      raw.push_back(static_cast<uint8_t>(u & 0xFF));
      raw.push_back(static_cast<uint8_t>(u >> 8));
    }
  uint64_t h = 1469598103934665603ull;
  h = fnv(h, raw.data(), raw.size());
  h = fnv(h, &e.rate, sizeof e.rate);
  h = fnv(h, &e.channels, sizeof e.channels);
  h = fnv(h, &e.peak, sizeof e.peak);
  e.id = hex16(h);
  e.pcm = b64(raw);
  return addEntry(m, std::move(e));
}

std::string addMediaMap(Model& m, const std::string& name, const std::string& sfzText) {
  Media e;
  e.name = name;
  e.channels = 0;
  e.sfz = sfzText;
  e.id = hex16(fnv(0x9e3779b97f4a7c15ull, sfzText.data(), sfzText.size()));
  return addEntry(m, std::move(e));
}

bool mediaAudio(const Media& e, std::vector<std::vector<float>>& channels) {
  channels.clear();
  if (e.channels < 1 || e.channels > 2 || e.frames <= 0) return false;
  const auto raw = unb64(e.pcm);
  const size_t nc = static_cast<size_t>(e.channels);
  const size_t frames = std::min<size_t>(static_cast<size_t>(e.frames), raw.size() / (2 * nc));
  if (!frames) return false;
  channels.assign(nc, std::vector<float>(frames));
  for (size_t i = 0; i < frames; ++i)
    for (size_t c = 0; c < nc; ++c) {
      const size_t o = (i * nc + c) * 2;
      const int16_t v = static_cast<int16_t>(static_cast<uint16_t>(raw[o] | (raw[o + 1] << 8)));
      channels[c][i] = static_cast<float>(v) / 32767.f * e.peak;
    }
  return true;
}

std::string mediaFile(const Model& m, const std::string& ref) {
  if (!isMediaRef(ref)) return ref;
  const Media* e = findMedia(m, ref);
  if (!e) return "";
  namespace fs = std::filesystem;
  static std::recursive_mutex mu;  // a multisample map extracts its members through this function
  std::lock_guard<std::recursive_mutex> lk(mu);
  const std::string dir = mediaCacheDir();
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  if (e->sfz.empty()) {
    const std::string path = dir + "/" + e->id + ".wav";
    if (fs::is_regular_file(fs::u8path(path), ec)) return path;
    std::vector<std::vector<float>> ch;
    if (!mediaAudio(*e, ch) || !writeFloatWav(path, ch, e->rate)) return "";
    return path;
  }
  // multisample map: its samples first, then the map with their file names
  std::string text = e->sfz;
  for (size_t at = text.find(kMediaPrefix); at != std::string::npos; at = text.find(kMediaPrefix, at)) {
    const size_t idAt = at + sizeof(kMediaPrefix) - 1;
    const std::string id = text.substr(idAt, 16);
    const std::string file = mediaFile(m, kMediaPrefix + id);
    // the map refers to its samples by file name (they sit next to it)
    const std::string name = file.empty() ? id + ".missing" : fs::u8path(file).filename().u8string();
    text.replace(at, sizeof(kMediaPrefix) - 1 + id.size(), name);
    at += name.size();
  }
  const std::string path = dir + "/" + e->id + ".sfz";
  if (fs::is_regular_file(fs::u8path(path), ec)) return path;
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return "";
    f << text;
  }
  fs::rename(fs::u8path(tmp), fs::u8path(path), ec);
  return ec ? "" : path;
}

// ------------------------------------------------------------------ model
Model defaultModel() {
  Model m;
  for (auto& p : m.fxProxy) p.fill(-1);
  for (int i = 0; i < kMacros; ++i) m.macroName[static_cast<size_t>(i)] = "Macro " + std::to_string(i + 1);
  for (auto& a : m.arp) {
    a.shape = ArpUp;
    a.rate = 7;  // 1/16
  }
  for (size_t i = 0; i < m.arp.size(); ++i) m.arp[i].name = "Arp " + std::to_string(i + 1);
  for (size_t i = 0; i < m.clip.size(); ++i) m.clip[i].name = "Clip " + std::to_string(i + 1);
  return m;
}

std::string modelToData(const Model& m) {
  Json r = Json::makeObject();
  r.object["plasma"] = num(1);
  Json oscs = Json::makeArray();
  for (const auto& o : m.osc) {
    Json j = Json::makeObject();
    if (!o.table.empty()) j.object["table"] = str(o.table);
    if (!o.tableName.empty()) j.object["tableName"] = str(o.tableName);
    if (!o.frames.empty()) j.object["frames"] = str(framesToText(o.frames));
    if (!o.sample.empty()) j.object["sample"] = str(o.sample);
    if (!o.sfz.empty()) j.object["sfz"] = str(o.sfz);
    if (!o.slices.empty()) j.object["slices"] = floats(o.slices);
    Json rm = Json::makeArray();
    bool any = false;
    for (const auto& c : o.remap) {
      rm.array.push_back(curveJson(c));
      any = any || !c.empty();
    }
    if (any) j.object["remap"] = rm;
    oscs.array.push_back(j);
  }
  r.object["osc"] = oscs;
  if (!m.noiseFile.empty()) r.object["noiseFile"] = str(m.noiseFile);
  Json lfos = Json::makeArray();
  for (const auto& l : m.lfo) {
    Json j = Json::makeObject();
    if (!l.pts.empty()) j.object["pts"] = curveJson(l.pts);
    if (l.loop >= 0) j.object["loop"] = num(l.loop);
    if (!l.path.empty()) j.object["path"] = curveJson(l.path);
    if (!l.pathX.empty()) j.object["pathX"] = floats(l.pathX);
    if (!l.name.empty()) j.object["name"] = str(l.name);
    lfos.array.push_back(j);
  }
  r.object["lfo"] = lfos;
  if (!m.velo.empty()) r.object["velo"] = curveJson(m.velo);
  if (!m.note.empty()) r.object["note"] = curveJson(m.note);
  Json mc = Json::makeObject();
  for (int s = 0; s < kModSlots; ++s) {
    if (!m.modCurve[static_cast<size_t>(s)].empty()) mc.object["c" + std::to_string(s)] = curveJson(m.modCurve[static_cast<size_t>(s)]);
    if (!m.auxCurve[static_cast<size_t>(s)].empty()) mc.object["a" + std::to_string(s)] = curveJson(m.auxCurve[static_cast<size_t>(s)]);
  }
  if (!mc.object.empty()) r.object["modCurves"] = mc;
  if (!m.busLinks.empty()) {
    Json a = Json::makeArray();
    for (const auto& b : m.busLinks) {
      Json j = Json::makeObject();
      j.object["lfo"] = num(b.lfo);
      j.object["pt"] = num(b.point);
      j.object["axis"] = num(b.axis);
      j.object["bus"] = num(b.bus);
      j.object["depth"] = num(b.depth);
      a.array.push_back(j);
    }
    r.object["busLinks"] = a;
  }
  Json fx = Json::makeArray();
  for (const auto& n : m.fx) {
    Json j = Json::makeObject();
    j.object["slot"] = num(n.slot);
    j.object["rack"] = num(n.rack);
    if (n.parent >= 0) {
      j.object["parent"] = num(n.parent);
      j.object["band"] = num(n.band);
    }
    fx.array.push_back(j);
  }
  r.object["fx"] = fx;
  Json fe = Json::makeObject();
  for (int s = 0; s < kFxSlots; ++s) {
    const FxExtra& e = m.fxExtra[static_cast<size_t>(s)];
    if (e.ir.empty() && e.shapeA.empty() && e.shapeB.empty()) continue;
    Json j = Json::makeObject();
    if (!e.ir.empty()) j.object["ir"] = str(e.ir);
    if (!e.shapeA.empty()) j.object["shapeA"] = curveJson(e.shapeA);
    if (!e.shapeB.empty()) j.object["shapeB"] = curveJson(e.shapeB);
    fe.object[std::to_string(s)] = j;
  }
  if (!fe.object.empty()) r.object["fxExtra"] = fe;
  Json px = Json::makeArray();
  bool anyProxy = false;
  for (const auto& rack : m.fxProxy) {
    Json a = Json::makeArray();
    for (int v : rack) {
      a.array.push_back(num(v));
      anyProxy = anyProxy || v >= 0;
    }
    px.array.push_back(a);
  }
  if (anyProxy) r.object["fxProxy"] = px;
  Json arps = Json::makeArray();
  for (const auto& a : m.arp) {
    Json j = Json::makeObject();
    j.object["shape"] = num(a.shape);
    j.object["rate"] = num(a.rate);
    j.object["hz"] = num(a.hz);
    j.object["bpm"] = boolean(a.bpm);
    j.object["trip"] = boolean(a.trip);
    j.object["dot"] = boolean(a.dot);
    j.object["shift"] = num(a.shift);
    j.object["range"] = num(a.range);
    j.object["rangeShape"] = num(a.rangeShape);
    j.object["latch"] = boolean(a.latch);
    j.object["thru"] = boolean(a.thru);
    j.object["offset"] = num(a.offset);
    j.object["repeats"] = num(a.repeats);
    j.object["gate"] = num(a.gate);
    j.object["chance"] = num(a.chance);
    j.object["chancePre"] = boolean(a.chancePre);
    j.object["rtLaunch"] = boolean(a.retrigLaunch);
    j.object["rtRate"] = boolean(a.retrigRate);
    j.object["rtNote"] = boolean(a.retrigNote);
    j.object["rtFirst"] = boolean(a.retrigFirst);
    j.object["rtDiv"] = num(a.retrigDiv);
    j.object["velo"] = boolean(a.velo);
    j.object["veloRetrig"] = boolean(a.veloRetrig);
    j.object["veloDecay"] = num(a.veloDecay);
    j.object["veloTarget"] = num(a.veloTarget);
    if (!a.steps.empty()) {
      Json st = Json::makeArray();
      for (const auto& s : a.steps) {
        Json e = Json::makeArray();
        e.array = {num(s.time), num(s.len), num(s.row), num(s.vel), num(s.accent ? 1 : 0), num(s.strum)};
        st.array.push_back(e);
      }
      j.object["steps"] = st;
    }
    j.object["patBeats"] = num(a.patternBeats);
    j.object["patMode"] = num(a.patMode);
    j.object["stepMode"] = num(a.stepMode);
    j.object["wrap"] = num(a.wrap);
    j.object["wrapPitch"] = num(a.wrapPitch);
    j.object["rangeWrap"] = num(a.rangeWrap);
    j.object["patTime"] = num(a.patTime);
    j.object["name"] = str(a.name);
    arps.array.push_back(j);
  }
  r.object["arp"] = arps;
  r.object["arpActive"] = num(m.arpActive);
  r.object["arpQuant"] = num(m.arpLaunchQuant);
  Json clips = Json::makeArray();
  for (const auto& c : m.clip) {
    Json j = Json::makeObject();
    Json notes = Json::makeArray();
    for (const auto& n : c.notes) {
      Json e = Json::makeArray();
      e.array = {num(n.start), num(n.len), num(n.key), num(n.vel), num(n.mute ? 1 : 0)};
      notes.array.push_back(e);
    }
    j.object["notes"] = notes;
    if (!c.lanes.empty()) {
      Json lanes = Json::makeArray();
      for (const auto& l : c.lanes) {
        Json e = Json::makeObject();
        e.object["target"] = num(l.target);
        Json pts = Json::makeArray();
        for (const auto& p : l.pts) {
          pts.array.push_back(num(p.first));
          pts.array.push_back(num(p.second));
        }
        e.object["pts"] = pts;
        lanes.array.push_back(e);
      }
      j.object["lanes"] = lanes;
    }
    j.object["len"] = num(c.lengthBeats);
    j.object["span"] = num(c.span);
    j.object["transpose"] = num(c.transpose);
    j.object["mode"] = num(c.mode);
    j.object["modeTime"] = num(c.modeTime);
    j.object["rate"] = num(c.rate);
    j.object["bpm"] = boolean(c.bpm);
    j.object["trip"] = boolean(c.trip);
    j.object["dot"] = boolean(c.dot);
    j.object["quant"] = num(c.launchQuant);
    j.object["retrig"] = boolean(c.retrig);
    j.object["veloTrig"] = boolean(c.veloTrig);
    j.object["noteGate"] = boolean(c.noteGate);
    j.object["startOffset"] = num(c.startOffset);
    j.object["name"] = str(c.name);
    clips.array.push_back(j);
  }
  r.object["clip"] = clips;
  r.object["clipActive"] = num(m.clipActive);
  r.object["clipPoly"] = boolean(m.clipPoly);
  Json vs = Json::makeArray();
  for (const auto& s : m.voiceSeq) {
    Json e = Json::makeArray();
    e.array = {num(s.pan), num(s.detune), num(s.cutoff), num(s.env), num(s.mod1), num(s.mod2)};
    vs.array.push_back(e);
  }
  r.object["voiceSeq"] = vs;
  Json mn = Json::makeArray();
  for (const auto& n : m.macroName) mn.array.push_back(str(n));
  r.object["macroNames"] = mn;
  if (!m.tuning.empty()) {
    r.object["tuning"] = str(m.tuning);
    r.object["tuningName"] = str(m.tuningName);
  }
  if (!m.presetName.empty()) r.object["name"] = str(m.presetName);
  if (!m.author.empty()) r.object["author"] = str(m.author);
  if (!m.description.empty()) r.object["description"] = str(m.description);
  if (!m.tags.empty()) {
    Json t = Json::makeArray();
    for (const auto& s : m.tags) t.array.push_back(str(s));
    r.object["tags"] = t;
  }
  if (!m.openFormat.empty()) {
    Json o = Json::makeObject();
    o.object["format"] = str(m.openFormat);
    Json notes = Json::makeArray(), missing = Json::makeArray();
    for (const auto& s : m.openNotes) notes.array.push_back(str(s));
    for (const auto& s : m.openMissing) missing.array.push_back(str(s));
    o.object["notes"] = notes;
    o.object["missing"] = missing;
    r.object["openReport"] = o;
  }
  r.object["mpeXYZ"] = boolean(m.mpeXYZtoMacros);
  r.object["mpeYZ"] = boolean(m.mpeYZtoMacros);
  r.object["mpeYWheel"] = boolean(m.mpeYtoModWheel);
  r.object["mpeBendX"] = boolean(m.mpeBendToX);
  r.object["lockFx"] = boolean(m.lockFx);
  if (!m.media.empty()) {
    // only what the sound still uses (a replaced sample leaves its entry behind)
    std::string refs;
    for (const auto& o : m.osc) refs += o.sample + "\n" + o.sfz + "\n";
    refs += m.noiseFile + "\n";
    for (const auto& e : m.fxExtra) refs += e.ir + "\n";
    for (const auto& e : m.media)
      if (!e.sfz.empty() && refs.find(kMediaPrefix + e.id) != std::string::npos) refs += e.sfz;
    Json md = Json::makeArray();
    for (const auto& e : m.media) {
      if (refs.find(kMediaPrefix + e.id) == std::string::npos) continue;
      Json j = Json::makeObject();
      j.object["id"] = str(e.id);
      if (!e.name.empty()) j.object["name"] = str(e.name);
      if (!e.sfz.empty()) {
        j.object["sfz"] = str(e.sfz);
      } else {
        j.object["rate"] = num(e.rate);
        j.object["channels"] = num(e.channels);
        j.object["frames"] = num(static_cast<double>(e.frames));
        j.object["peak"] = num(e.peak);
        j.object["pcm"] = str(e.pcm);
      }
      md.array.push_back(std::move(j));
    }
    r.object["media"] = std::move(md);
  }
  return jsonStringify(r, 0);
}

bool modelFromData(const std::string& data, Model& m) {
  m = defaultModel();
  if (data.empty() || data[0] != '{') return false;
  Json r;
  if (!jsonParse(data, r) || !r.is(Json::Type::Object) || !r.find("plasma")) return false;
  if (const Json* oscs = r.find("osc"); oscs && oscs->is(Json::Type::Array)) {
    for (size_t i = 0; i < std::min<size_t>(oscs->array.size(), kOscs); ++i) {
      const Json& j = oscs->array[i];
      if (!j.is(Json::Type::Object)) continue;
      OscData& o = m.osc[i];
      o.table = j.text("table");
      o.tableName = j.text("tableName");
      if (const Json* f = j.find("frames"); f && f->is(Json::Type::String)) o.frames = framesFromText(f->str);
      o.sample = j.text("sample");
      o.sfz = j.text("sfz");
      o.slices = floatsFrom(j.find("slices"), 256, 0, 1);
      std::sort(o.slices.begin(), o.slices.end());
      if (const Json* rm = j.find("remap"); rm && rm->is(Json::Type::Array))
        for (size_t k = 0; k < std::min<size_t>(rm->array.size(), 4); ++k) o.remap[k] = curveFrom(&rm->array[k]);
    }
  }
  m.noiseFile = r.text("noiseFile");
  if (const Json* lfos = r.find("lfo"); lfos && lfos->is(Json::Type::Array)) {
    for (size_t i = 0; i < std::min<size_t>(lfos->array.size(), kLfos); ++i) {
      const Json& j = lfos->array[i];
      if (!j.is(Json::Type::Object)) continue;
      LfoData& l = m.lfo[i];
      l.pts = curveFrom(j.find("pts"));
      l.loop = getInt(j, "loop", -1, -1, 511);
      if (l.loop >= static_cast<int>(l.pts.size())) l.loop = -1;
      l.path = curveFrom(j.find("path"));
      l.pathX = floatsFrom(j.find("pathX"), 512, 0, 1);
      l.name = j.text("name");
    }
  }
  m.velo = curveFrom(r.find("velo"));
  m.note = curveFrom(r.find("note"));
  if (const Json* mc = r.find("modCurves"); mc && mc->is(Json::Type::Object))
    for (int s = 0; s < kModSlots; ++s) {
      m.modCurve[static_cast<size_t>(s)] = curveFrom(mc->find("c" + std::to_string(s)));
      m.auxCurve[static_cast<size_t>(s)] = curveFrom(mc->find("a" + std::to_string(s)));
    }
  if (const Json* bl = r.find("busLinks"); bl && bl->is(Json::Type::Array))
    for (const auto& j : bl->array) {
      if (!j.is(Json::Type::Object) || m.busLinks.size() >= 256) continue;
      BusLink b;
      b.lfo = getInt(j, "lfo", 0, 0, kLfos - 1);
      b.point = getInt(j, "pt", 0, 0, 511);
      b.axis = getInt(j, "axis", 1, 0, 1);
      b.bus = getInt(j, "bus", 0, 0, kLfoBusses - 1);
      b.depth = getF(j, "depth", 1, -1, 1);
      m.busLinks.push_back(b);
    }
  if (const Json* fx = r.find("fx"); fx && fx->is(Json::Type::Array)) {
    std::array<bool, kFxSlots> seen{};
    for (const auto& j : fx->array) {
      if (!j.is(Json::Type::Object)) continue;
      FxNode n;
      n.slot = getInt(j, "slot", -1, -1, kFxSlots - 1);
      if (n.slot < 0 || seen[static_cast<size_t>(n.slot)]) continue;
      seen[static_cast<size_t>(n.slot)] = true;
      n.rack = getInt(j, "rack", 0, 0, 2);
      n.parent = getInt(j, "parent", -1, -1, kFxSlots - 1);
      if (n.parent == n.slot) n.parent = -1;
      n.band = getInt(j, "band", 0, 0, 2);
      m.fx.push_back(n);
    }
  }
  if (const Json* fe = r.find("fxExtra"); fe && fe->is(Json::Type::Object))
    for (int s = 0; s < kFxSlots; ++s) {
      const Json* j = fe->find(std::to_string(s));
      if (!j || !j->is(Json::Type::Object)) continue;
      FxExtra& e = m.fxExtra[static_cast<size_t>(s)];
      e.ir = j->text("ir");
      e.shapeA = curveFrom(j->find("shapeA"));
      e.shapeB = curveFrom(j->find("shapeB"));
    }
  if (const Json* px = r.find("fxProxy"); px && px->is(Json::Type::Array))
    for (size_t rk = 0; rk < std::min<size_t>(px->array.size(), 3); ++rk) {
      const Json& a = px->array[rk];
      if (!a.is(Json::Type::Array)) continue;
      for (size_t i = 0; i < std::min<size_t>(a.array.size(), 16); ++i)
        m.fxProxy[rk][i] = a.array[i].is(Json::Type::Number) ? std::clamp(jsonInt<int>(a.array[i].number), -1, kFxSlots * 64 - 1) : -1;
    }
  if (const Json* arps = r.find("arp"); arps && arps->is(Json::Type::Array))
    for (size_t i = 0; i < std::min<size_t>(arps->array.size(), kArpSlots); ++i) {
      const Json& j = arps->array[i];
      if (!j.is(Json::Type::Object)) continue;
      ArpSlot& a = m.arp[i];
      a.shape = getInt(j, "shape", a.shape, 0, ArpShapeCount - 1);
      a.rate = getInt(j, "rate", a.rate, 0, 31);
      a.hz = getF(j, "hz", a.hz, 0.05f, 100);
      a.bpm = getBool(j, "bpm", a.bpm);
      a.trip = getBool(j, "trip", false);
      a.dot = getBool(j, "dot", false);
      a.shift = getInt(j, "shift", 0, -48, 48);
      a.range = getInt(j, "range", 0, 0, 8);
      a.rangeShape = getInt(j, "rangeShape", 0, 0, 3);
      a.latch = getBool(j, "latch", false);
      a.thru = getBool(j, "thru", false);
      a.offset = getInt(j, "offset", 0, -32, 32);
      a.repeats = getInt(j, "repeats", 0, 0, 64);
      a.gate = getF(j, "gate", 1, 0.01f, 4);
      a.chance = getF(j, "chance", 1, 0, 1);
      a.chancePre = getBool(j, "chancePre", false);
      a.retrigLaunch = getBool(j, "rtLaunch", true);
      a.retrigRate = getBool(j, "rtRate", false);
      a.retrigNote = getBool(j, "rtNote", false);
      a.retrigFirst = getBool(j, "rtFirst", false);
      a.retrigDiv = getInt(j, "rtDiv", 6, 0, 15);
      a.velo = getBool(j, "velo", false);
      a.veloRetrig = getBool(j, "veloRetrig", false);
      a.veloDecay = getF(j, "veloDecay", 0.5f, 0, 1);
      a.veloTarget = getF(j, "veloTarget", 0.5f, 0, 1);
      a.steps.clear();
      if (const Json* st = j.find("steps"); st && st->is(Json::Type::Array))
        for (const auto& e : st->array) {
          if (!e.is(Json::Type::Array) || e.array.size() < 6 || a.steps.size() >= 256) continue;
          auto f = [&](size_t k, double lo, double hi) {
            return e.array[k].is(Json::Type::Number) && std::isfinite(e.array[k].number)
                       ? static_cast<float>(std::clamp(e.array[k].number, lo, hi)) : static_cast<float>(lo);
          };
          ArpSlot::Step s;
          s.time = f(0, 0, 64);
          s.len = std::max(0.001f, f(1, 0, 64));
          s.row = static_cast<int>(f(2, -48, 48));
          s.vel = f(3, 0, 1);
          s.accent = f(4, 0, 1) >= 0.5f;
          s.strum = f(5, -1, 1);
          a.steps.push_back(s);
        }
      a.patternBeats = getF(j, "patBeats", 4, 0.25f, 64);
      a.patMode = getInt(j, "patMode", 0, 0, 3);
      a.stepMode = getInt(j, "stepMode", 0, 0, 3);
      a.wrap = getInt(j, "wrap", 0, 0, 3);
      a.wrapPitch = getInt(j, "wrapPitch", 12, -48, 48);
      a.rangeWrap = getInt(j, "rangeWrap", 0, 0, 3);
      a.patTime = getF(j, "patTime", 1, 0.0625f, 16);
      a.name = j.text("name", a.name);
    }
  m.arpActive = getInt(r, "arpActive", 0, 0, kArpSlots - 1);
  m.arpLaunchQuant = getInt(r, "arpQuant", 6, 0, 15);
  if (const Json* clips = r.find("clip"); clips && clips->is(Json::Type::Array))
    for (size_t i = 0; i < std::min<size_t>(clips->array.size(), kClipSlots); ++i) {
      const Json& j = clips->array[i];
      if (!j.is(Json::Type::Object)) continue;
      ClipSlot& c = m.clip[i];
      c.notes.clear();
      if (const Json* ns = j.find("notes"); ns && ns->is(Json::Type::Array))
        for (const auto& e : ns->array) {
          if (!e.is(Json::Type::Array) || e.array.size() < 5 || c.notes.size() >= 4096) continue;
          auto f = [&](size_t k, double lo, double hi, double d) {
            return e.array[k].is(Json::Type::Number) && std::isfinite(e.array[k].number) ? std::clamp(e.array[k].number, lo, hi) : d;
          };
          ClipNote n;
          n.start = static_cast<float>(f(0, 0, 1024, 0));
          n.len = static_cast<float>(f(1, 1.0 / 256, 1024, 0.25));
          n.key = static_cast<int>(f(2, 0, 127, 60));
          n.vel = static_cast<float>(f(3, 0, 1, 0.8));
          n.mute = f(4, 0, 1, 0) >= 0.5;
          c.notes.push_back(n);
        }
      c.lanes.clear();
      if (const Json* ls = j.find("lanes"); ls && ls->is(Json::Type::Array))
        for (const auto& e : ls->array) {
          if (!e.is(Json::Type::Object) || c.lanes.size() >= 32) continue;
          ClipLane l;
          l.target = getInt(e, "target", -1, -1, kParamCount - 1);
          if (const Json* pts = e.find("pts"); pts && pts->is(Json::Type::Array))
            for (size_t k = 0; k + 1 < pts->array.size() && l.pts.size() < 1024; k += 2)
              if (pts->array[k].is(Json::Type::Number) && pts->array[k + 1].is(Json::Type::Number) &&
                  std::isfinite(pts->array[k].number) && std::isfinite(pts->array[k + 1].number))
                l.pts.push_back({static_cast<float>(std::clamp(pts->array[k].number, 0.0, 1024.0)),
                                 static_cast<float>(std::clamp(pts->array[k + 1].number, 0.0, 1.0))});
          c.lanes.push_back(l);
        }
      c.lengthBeats = getF(j, "len", 4, 0.25f, 1024);
      c.span = getInt(j, "span", 0, 0, 3);
      c.transpose = getInt(j, "transpose", 0, -48, 48);
      c.mode = getInt(j, "mode", 0, 0, 8);
      c.modeTime = getF(j, "modeTime", 1, 0.0625f, 64);
      c.rate = getF(j, "rate", 1, 0.125f, 8);
      c.bpm = getBool(j, "bpm", true);
      c.trip = getBool(j, "trip", false);
      c.dot = getBool(j, "dot", false);
      c.launchQuant = getInt(j, "quant", 6, 0, 15);
      c.retrig = getBool(j, "retrig", true);
      c.veloTrig = getBool(j, "veloTrig", false);
      c.noteGate = getBool(j, "noteGate", false);
      c.startOffset = getF(j, "startOffset", 0, 0, 1024);
      c.name = j.text("name", c.name);
    }
  m.clipActive = getInt(r, "clipActive", 0, 0, kClipSlots - 1);
  m.clipPoly = getBool(r, "clipPoly", false);
  if (const Json* vs = r.find("voiceSeq"); vs && vs->is(Json::Type::Array))
    for (size_t i = 0; i < std::min<size_t>(vs->array.size(), 8); ++i) {
      const Json& e = vs->array[i];
      if (!e.is(Json::Type::Array) || e.array.size() < 6) continue;
      auto f = [&](size_t k) {
        return e.array[k].is(Json::Type::Number) && std::isfinite(e.array[k].number) ? static_cast<float>(std::clamp(e.array[k].number, -1.0, 1.0)) : 0.f;
      };
      m.voiceSeq[i] = {f(0), f(1), f(2), f(3), f(4), f(5)};
    }
  if (const Json* mn = r.find("macroNames"); mn && mn->is(Json::Type::Array))
    for (size_t i = 0; i < std::min<size_t>(mn->array.size(), kMacros); ++i)
      if (mn->array[i].is(Json::Type::String)) m.macroName[i] = mn->array[i].str.substr(0, 64);
  m.tuning = r.text("tuning");
  m.tuningName = r.text("tuningName");
  m.presetName = r.text("name");
  m.author = r.text("author");
  m.description = r.text("description");
  if (const Json* t = r.find("tags"); t && t->is(Json::Type::Array))
    for (const auto& e : t->array)
      if (e.is(Json::Type::String) && m.tags.size() < 32) m.tags.push_back(e.str);
  if (const Json* o = r.find("openReport"); o && o->is(Json::Type::Object)) {
    m.openFormat = o->text("format");
    for (const char* key : {"notes", "missing"})
      if (const Json* a = o->find(key); a && a->is(Json::Type::Array))
        for (const auto& e : a->array)
          if (e.is(Json::Type::String) && m.openNotes.size() + m.openMissing.size() < 256)
            (key[0] == 'n' ? m.openNotes : m.openMissing).push_back(e.str.substr(0, 512));
  }
  m.mpeXYZtoMacros = getBool(r, "mpeXYZ", false);
  m.mpeYZtoMacros = getBool(r, "mpeYZ", false);
  m.mpeYtoModWheel = getBool(r, "mpeYWheel", false);
  m.mpeBendToX = getBool(r, "mpeBendX", false);
  m.lockFx = getBool(r, "lockFx", false);
  if (const Json* md = r.find("media"); md && md->is(Json::Type::Array))
    for (const auto& j : md->array) {
      if (!j.is(Json::Type::Object) || m.media.size() >= 256) continue;
      Media e;
      e.id = j.text("id").substr(0, 16);
      if (e.id.size() != 16 || e.id.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
      e.name = j.text("name").substr(0, 512);
      e.sfz = j.text("sfz");
      e.rate = std::clamp(jsonInt<int>(j.num("rate", 44100)), 1000, 384000);
      e.channels = std::clamp(jsonInt<int>(j.num("channels", 1)), 0, 2);
      e.frames = std::max<int64_t>(0, jsonInt<int64_t>(j.num("frames", 0)));
      const double pk = j.num("peak", 1);
      e.peak = std::isfinite(pk) && pk > 0 && pk < 1e6 ? static_cast<float>(pk) : 1.f;
      e.pcm = j.text("pcm");
      m.media.push_back(std::move(e));
    }
  return true;
}

std::vector<FxNode> resolvedFx(const Model& m, const std::vector<float>& params) {
  auto typeOf = [&](int s) {
    const int k = fxP(s, X_Type);
    return k < static_cast<int>(params.size()) ? std::clamp(static_cast<int>(std::lround(params[static_cast<size_t>(k)])), 0, FxTypeCount - 1) : 0;
  };
  std::vector<FxNode> out;
  std::array<bool, kFxSlots> seen{};
  for (const auto& n : m.fx) {
    if (n.slot < 0 || n.slot >= kFxSlots || seen[static_cast<size_t>(n.slot)]) continue;
    if (typeOf(n.slot) == FxNone) continue;
    FxNode c = n;
    // a band module needs a splitter parent in the same rack, listed before it
    if (c.parent >= 0) {
      bool ok = false;
      for (const auto& p : out)
        if (p.slot == c.parent && p.rack == c.rack && p.parent < 0) {
          const int pt = typeOf(p.slot);
          const int bands = pt == FxSplitLMH ? 3 : (pt == FxSplitLH || pt == FxSplitMS) ? 2 : 0;
          ok = c.band < bands;
        }
      if (!ok) c.parent = -1, c.band = 0;
    }
    seen[static_cast<size_t>(n.slot)] = true;
    out.push_back(c);
  }
  for (int s = 0; s < kFxSlots; ++s)
    if (!seen[static_cast<size_t>(s)] && typeOf(s) != FxNone) out.push_back({s, 0, -1, 0});
  return out;
}

// ------------------------------------------------------------------ arpeggiator orders
void arpOrder(int shape, int count, const std::vector<int>& played, uint32_t& rng, std::vector<int>& out) {
  out.clear();
  if (count <= 0) return;
  auto rnd = [&]() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
  };
  switch (shape) {
    case ArpDown:
      for (int i = count - 1; i >= 0; --i) out.push_back(i);
      break;
    case ArpUpDown:  // no repeated ends
      for (int i = 0; i < count; ++i) out.push_back(i);
      for (int i = count - 2; i >= 1; --i) out.push_back(i);
      break;
    case ArpDownUp:
      for (int i = count - 1; i >= 0; --i) out.push_back(i);
      for (int i = 1; i < count - 1; ++i) out.push_back(i);
      break;
    case ArpUpAndDown:  // ends repeat
      for (int i = 0; i < count; ++i) out.push_back(i);
      for (int i = count - 1; i >= 0; --i) out.push_back(i);
      break;
    case ArpDownAndUp:
      for (int i = count - 1; i >= 0; --i) out.push_back(i);
      for (int i = 0; i < count; ++i) out.push_back(i);
      break;
    case ArpThumbUp:
      for (int i = 1; i < count; ++i) {
        out.push_back(0);
        out.push_back(i);
      }
      if (count == 1) out.push_back(0);
      break;
    case ArpThumbUD:
      for (int i = 1; i < count; ++i) {
        out.push_back(0);
        out.push_back(i);
      }
      for (int i = count - 2; i >= 1; --i) {
        out.push_back(0);
        out.push_back(i);
      }
      if (count == 1) out.push_back(0);
      break;
    case ArpPinkyUp:
      for (int i = 0; i < count - 1; ++i) {
        out.push_back(i);
        out.push_back(count - 1);
      }
      if (count == 1) out.push_back(0);
      break;
    case ArpPinkyUD:
      for (int i = 0; i < count - 1; ++i) {
        out.push_back(i);
        out.push_back(count - 1);
      }
      for (int i = count - 3; i >= 1; --i) {
        out.push_back(i);
        out.push_back(count - 1);
      }
      if (count == 1) out.push_back(0);
      break;
    case ArpConverge: {
      int lo = 0, hi = count - 1;
      while (lo <= hi) {
        out.push_back(lo++);
        if (lo <= hi) out.push_back(hi--);
      }
      break;
    }
    case ArpDiverge:
    case ArpConDiv: {
      int lo = 0, hi = count - 1;
      while (lo <= hi) {
        out.push_back(lo++);
        if (lo <= hi) out.push_back(hi--);
      }
      if (shape == ArpDiverge) {
        std::reverse(out.begin(), out.end());
      } else {
        const size_t n = out.size();  // converge, then back out (ends not repeated)
        for (size_t i = n >= 2 ? n - 2 : 0; i > 0; --i) out.push_back(out[i]);
      }
      break;
    }
    case ArpPlayed:
      for (int i : played)
        if (i >= 0 && i < count) out.push_back(i);
      if (out.empty())
        for (int i = 0; i < count; ++i) out.push_back(i);
      break;
    case ArpChord:
      out.push_back(-1);  // all notes at once
      break;
    case ArpRand:
      for (int i = 0; i < count; ++i) out.push_back(static_cast<int>(rnd() % static_cast<uint32_t>(count)));
      break;
    case ArpRandNoDup: {
      int last = -1;
      for (int i = 0; i < count; ++i) {
        int v = static_cast<int>(rnd() % static_cast<uint32_t>(count));
        if (count > 1 && v == last) v = (v + 1 + static_cast<int>(rnd() % static_cast<uint32_t>(count - 1))) % count;
        out.push_back(v);
        last = v;
      }
      break;
    }
    case ArpRandDrift: {
      int v = static_cast<int>(rnd() % static_cast<uint32_t>(count));
      for (int i = 0; i < count; ++i) {
        out.push_back(v);
        v = std::clamp(v + ((rnd() & 1) ? 1 : -1), 0, count - 1);
      }
      break;
    }
    case ArpRandOnce: {  // a shuffle of every note (each once per cycle)
      for (int i = 0; i < count; ++i) out.push_back(i);
      for (int i = count - 1; i > 0; --i) std::swap(out[static_cast<size_t>(i)], out[rnd() % static_cast<uint32_t>(i + 1)]);
      break;
    }
    case ArpUp:
    default:
      for (int i = 0; i < count; ++i) out.push_back(i);
      break;
  }
}

}  // namespace gl::packs::plasma
