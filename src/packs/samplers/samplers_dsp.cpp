// Gridline — "samplers" pack: data model (zone maps, SoundFont 2 import,
// factory tones, scratch moves) and runtimes (Multisampler, Scratch Sampler).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include "../../engine/Dsp.h"
#include "../Packs.h"
#include "samplers.h"

namespace gl::packs::samplers {

namespace fs = std::filesystem;
using dsp::kPi;

// =========================================================================
// Text helpers
// =========================================================================
namespace {

std::string enc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '%' || c == ' ' || c == '|' || c == '\n' || c == '\r' || c == '=' || c == '\t') {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", static_cast<unsigned char>(c));
      o += b;
    } else {
      o += c;
    }
  }
  return o;
}

std::string dec(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      o += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> v;
  size_t st = 0;
  while (st <= s.size()) {
    size_t e = s.find('\n', st);
    std::string l = s.substr(st, e == std::string::npos ? std::string::npos : e - st);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    v.push_back(l);
    if (e == std::string::npos) break;
    st = e + 1;
  }
  return v;
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
    if (j > i) v.push_back(s.substr(i, j - i));
    i = j;
  }
  return v;
}

std::string fmtNum(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}

std::string lowerStr(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

bool Zone::operator==(const Zone& o) const {
  return path == o.path && name == o.name && lo == o.lo && hi == o.hi && vlo == o.vlo && vhi == o.vhi && root == o.root &&
         tune == o.tune && keyTrack == o.keyTrack && gain == o.gain && pan == o.pan && start == o.start && end == o.end &&
         loopStart == o.loopStart && loopEnd == o.loopEnd && loop == o.loop && xfade == o.xfade && reverse == o.reverse &&
         group == o.group && choke == o.choke && cutoff == o.cutoff && ownEnv == o.ownEnv && attack == o.attack &&
         hold == o.hold && decay == o.decay && sustain == o.sustain && release == o.release;
}

// =========================================================================
// Zone map text
// =========================================================================
namespace {

void clampZone(Zone& z) {
  z.lo = std::clamp(z.lo, 0, 127);
  z.hi = std::clamp(z.hi, z.lo, 127);
  z.vlo = std::clamp(z.vlo, 0, 127);
  z.vhi = std::clamp(z.vhi, z.vlo, 127);
  z.root = std::clamp(z.root, 0, 127);
  z.loop = std::clamp(z.loop, 0, static_cast<int>(LoopBounce));
  z.tune = std::clamp(z.tune, -4800.f, 4800.f);
  z.keyTrack = std::clamp(z.keyTrack, 0.f, 2.f);
  z.gain = std::clamp(z.gain, -96.f, 24.f);
  z.pan = std::clamp(z.pan, -1.f, 1.f);
  z.start = std::max<int64_t>(0, z.start);
  z.end = std::max<int64_t>(0, z.end);
  z.loopStart = std::max<int64_t>(0, z.loopStart);
  z.loopEnd = std::max<int64_t>(0, z.loopEnd);
  z.xfade = std::max<int64_t>(0, z.xfade);
  z.group = std::clamp(z.group, 0, kMaxGroups - 1);
  z.choke = std::clamp(z.choke, 0, 127);
  z.cutoff = std::clamp(z.cutoff, 0.f, 20000.f);
  z.attack = std::clamp(z.attack, 0.0005f, 20.f);
  z.hold = std::clamp(z.hold, 0.f, 20.f);
  z.decay = std::clamp(z.decay, 0.001f, 60.f);
  z.sustain = std::clamp(z.sustain, 0.f, 1.f);
  z.release = std::clamp(z.release, 0.001f, 60.f);
}

bool parseZoneLine(const std::string& line, Zone& z) {
  size_t bar = line.find(" | ");
  std::string head = line.substr(0, bar);
  if (bar != std::string::npos) z.path = line.substr(bar + 3);
  auto toks = splitWs(head);
  if (toks.empty() || toks[0] != "z") return false;
  for (size_t i = 1; i < toks.size(); ++i) {
    size_t eq = toks[i].find('=');
    if (eq == std::string::npos) continue;
    std::string k = toks[i].substr(0, eq), v = toks[i].substr(eq + 1);
    double d = std::atof(v.c_str());
    int n = static_cast<int>(std::lround(d));
    int64_t n64 = static_cast<int64_t>(std::llround(d));
    if (k == "lo") z.lo = n;
    else if (k == "hi") z.hi = n;
    else if (k == "vlo") z.vlo = n;
    else if (k == "vhi") z.vhi = n;
    else if (k == "root") z.root = n;
    else if (k == "tune") z.tune = static_cast<float>(d);
    else if (k == "kt") z.keyTrack = static_cast<float>(d);
    else if (k == "gain") z.gain = static_cast<float>(d);
    else if (k == "pan") z.pan = static_cast<float>(d);
    else if (k == "start") z.start = n64;
    else if (k == "end") z.end = n64;
    else if (k == "ls") z.loopStart = n64;
    else if (k == "le") z.loopEnd = n64;
    else if (k == "loop") z.loop = n;
    else if (k == "xf") z.xfade = n64;
    else if (k == "rev") z.reverse = n != 0;
    else if (k == "grp") z.group = n;
    else if (k == "choke") z.choke = n;
    else if (k == "fc") z.cutoff = static_cast<float>(d);
    else if (k == "env") z.ownEnv = n != 0;
    else if (k == "a") z.attack = static_cast<float>(d);
    else if (k == "h") z.hold = static_cast<float>(d);
    else if (k == "d") z.decay = static_cast<float>(d);
    else if (k == "s") z.sustain = static_cast<float>(d);
    else if (k == "r") z.release = static_cast<float>(d);
    else if (k == "name") z.name = dec(v);
  }
  clampZone(z);
  return !z.path.empty();
}

}  // namespace

ZoneMap parseZoneMap(const std::string& data, int factorySet) {
  bool blank = data.find_first_not_of(" \t\r\n") == std::string::npos;
  if (blank) return factoryZoneMap(factorySet);
  ZoneMap m;
  auto lines = splitLines(data);
  bool zoneText = data.rfind("#gl-zones", 0) == 0 || data.rfind("z ", 0) == 0 || data.find("\nz ") != std::string::npos;
  if (!zoneText) {
    // A bare audio path (e.g. a file dropped onto the channel rack).
    for (const auto& l : lines)
      if (!l.empty()) {
        Zone z = zoneForFile(l);
        z.lo = 0, z.hi = 127;
        m.zones.push_back(z);
        break;
      }
    return m;
  }
  for (const auto& l : lines) {
    if (l.rfind("z ", 0) == 0) {
      Zone z;
      if (parseZoneLine(l, z)) m.zones.push_back(z);
    } else if (l.rfind("sf2 ", 0) == 0) {
      size_t bar = l.find(" | ");
      auto toks = splitWs(l.substr(0, bar));
      if (toks.size() >= 3) {
        m.sf2Bank = std::atoi(toks[1].c_str());
        m.sf2Program = std::atoi(toks[2].c_str());
      }
      if (toks.size() >= 4) m.sf2Name = dec(toks[3]);
      if (bar != std::string::npos) m.sf2Path = l.substr(bar + 3);
    }
  }
  return m;
}

std::string serializeZoneMap(const ZoneMap& m) {
  std::string o = "#gl-zones 1\n";
  if (!m.sf2Path.empty())
    o += "sf2 " + std::to_string(m.sf2Bank) + " " + std::to_string(m.sf2Program) + " " + enc(m.sf2Name.empty() ? "-" : m.sf2Name) +
         " | " + m.sf2Path + "\n";
  const Zone d;
  for (const auto& z : m.zones) {
    o += "z lo=" + std::to_string(z.lo) + " hi=" + std::to_string(z.hi) + " vlo=" + std::to_string(z.vlo) +
         " vhi=" + std::to_string(z.vhi) + " root=" + std::to_string(z.root);
    if (z.tune != d.tune) o += " tune=" + fmtNum(z.tune);
    if (z.keyTrack != d.keyTrack) o += " kt=" + fmtNum(z.keyTrack);
    if (z.gain != d.gain) o += " gain=" + fmtNum(z.gain);
    if (z.pan != d.pan) o += " pan=" + fmtNum(z.pan);
    if (z.start) o += " start=" + std::to_string(z.start);
    if (z.end) o += " end=" + std::to_string(z.end);
    if (z.loopStart) o += " ls=" + std::to_string(z.loopStart);
    if (z.loopEnd) o += " le=" + std::to_string(z.loopEnd);
    if (z.loop) o += " loop=" + std::to_string(z.loop);
    if (z.xfade) o += " xf=" + std::to_string(z.xfade);
    if (z.reverse) o += " rev=1";
    if (z.group) o += " grp=" + std::to_string(z.group);
    if (z.choke) o += " choke=" + std::to_string(z.choke);
    if (z.cutoff > 0) o += " fc=" + fmtNum(z.cutoff);
    if (z.ownEnv)
      o += " env=1 a=" + fmtNum(z.attack) + " h=" + fmtNum(z.hold) + " d=" + fmtNum(z.decay) + " s=" + fmtNum(z.sustain) +
           " r=" + fmtNum(z.release);
    if (!z.name.empty()) o += " name=" + enc(z.name);
    o += " | " + z.path + "\n";
  }
  return o;
}

std::string zoneLabel(const Zone& z) {
  if (!z.name.empty()) return z.name;
  if (z.path.rfind("tone:", 0) == 0) return z.path.substr(5);
  size_t h = z.path.rfind('#');
  if (z.path.rfind("sf2:", 0) == 0 && h != std::string::npos) return "sample " + z.path.substr(h + 1);
  return fs::path(z.path).stem().string();
}

// =========================================================================
// File-name key mapping
// =========================================================================
int noteFromName(const std::string& nameIn) {
  std::string s = fs::path(nameIn).stem().string();
  if (s.empty()) s = nameIn;
  auto isAlpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
  auto isDigit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  int found = -1;
  static const int pcs[7] = {9, 11, 0, 2, 4, 5, 7};  // A..G
  for (size_t i = 0; i < s.size(); ++i) {
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
    if (c < 'A' || c > 'G') continue;
    if (i > 0 && isAlpha(s[i - 1])) continue;
    size_t j = i + 1;
    int pc = pcs[c - 'A'];
    if (j < s.size() && (s[j] == '#' || ((s[j] == 's' || s[j] == 'S') && j + 1 < s.size() && (isDigit(s[j + 1]) || s[j + 1] == '-')))) {
      ++pc;
      ++j;
    } else if (j < s.size() && s[j] == 'b' && j + 1 < s.size() && (isDigit(s[j + 1]) || s[j + 1] == '-')) {
      --pc;
      ++j;
    }
    bool neg = false;
    if (j < s.size() && s[j] == '-' && j + 1 < s.size() && isDigit(s[j + 1])) {
      neg = true;
      ++j;
    }
    if (j >= s.size() || !isDigit(s[j])) continue;
    int oct = s[j] - '0';
    ++j;
    if (j < s.size() && (isDigit(s[j]) || isAlpha(s[j]))) continue;  // "C10", "A4b"…
    if (neg) oct = -oct;
    int note = 12 * (oct + 1) + pc;  // C4 = 60
    if (note >= 0 && note <= 127) found = note;
  }
  if (found >= 0) return found;
  // Plain MIDI number token (2–3 digits).
  for (size_t i = 0; i < s.size();) {
    if (!isDigit(s[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && isDigit(s[j])) ++j;
    bool delimited = (i == 0 || !isAlpha(s[i - 1])) && (j == s.size() || !isAlpha(s[j]));
    if (delimited && j - i >= 2 && j - i <= 3) {
      int n = std::atoi(s.substr(i, j - i).c_str());
      if (n >= 0 && n <= 127) found = n;
    }
    i = j;
  }
  return found;
}

namespace {
// Lower = softer, for ordering velocity layers that share a root.
int velocityRank(const std::string& nameIn) {
  std::string s = lowerStr(fs::path(nameIn).stem().string());
  std::vector<std::string> toks;
  std::string cur;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      cur += c;
    else if (!cur.empty())
      toks.push_back(cur), cur.clear();
  }
  if (!cur.empty()) toks.push_back(cur);
  static const std::pair<const char*, int> words[] = {{"ppp", 1}, {"pp", 2}, {"p", 3}, {"soft", 3}, {"mp", 4}, {"med", 5},
                                                      {"medium", 5}, {"mf", 6}, {"f", 7}, {"hard", 8}, {"loud", 8}, {"ff", 9}, {"fff", 10}};
  int rank = -1;  // the last matching token wins ("p_C3_soft" is soft)
  for (const auto& t : toks) {
    for (const auto& w : words)
      if (t == w.first) rank = w.second * 10;
    if (t.size() >= 2 && t[0] == 'v' && std::isdigit(static_cast<unsigned char>(t[1]))) rank = std::atoi(t.c_str() + 1);
    if (t.rfind("vel", 0) == 0 && t.size() > 3 && std::isdigit(static_cast<unsigned char>(t[3]))) rank = std::atoi(t.c_str() + 3);
  }
  return rank;
}
}  // namespace

Zone zoneForFile(const std::string& path) {
  Zone z;
  z.path = path;
  int n = noteFromName(path);
  z.root = n >= 0 ? n : 60;
  z.lo = z.hi = z.root;
  return z;
}

void autoMap(std::vector<Zone>& zones) {
  if (zones.empty()) return;
  std::vector<int> roots;
  for (const auto& z : zones) roots.push_back(z.root);
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  for (size_t r = 0; r < roots.size(); ++r) {
    int lo = r == 0 ? 0 : (roots[r - 1] + roots[r]) / 2 + 1;
    int hi = r + 1 == roots.size() ? 127 : (roots[r] + roots[r + 1]) / 2;
    std::vector<Zone*> layer;
    for (auto& z : zones)
      if (z.root == roots[r]) layer.push_back(&z);
    std::stable_sort(layer.begin(), layer.end(), [](const Zone* a, const Zone* b) {
      int ra = velocityRank(a->path), rb = velocityRank(b->path);
      if (ra != rb) return ra < rb;
      return zoneLabel(*a) < zoneLabel(*b);
    });
    for (size_t k = 0; k < layer.size(); ++k) {
      Zone& z = *layer[k];
      z.lo = lo;
      z.hi = hi;
      z.vlo = static_cast<int>(128 * k / layer.size());
      z.vhi = static_cast<int>(128 * (k + 1) / layer.size()) - 1;
    }
  }
}

void mapAsDrumKit(std::vector<Zone>& zones, int base) {
  int k = std::clamp(base, 0, 127);
  for (auto& z : zones) {
    z.lo = z.hi = z.root = std::min(k, 127);
    z.vlo = 0, z.vhi = 127;
    z.loop = LoopOneShot;
    ++k;
  }
}

// =========================================================================
// Factory tones: loopable multisamples rendered from harmonic recipes
// =========================================================================
namespace {

const char* const kSetIds[] = {"keys", "bell", "pad", "bass", "pluck"};

struct ToneOut {
  SamplePtr smp;
  int64_t loopStart = 0, loopEnd = 0;
  float tuneCents = 0;
};

ToneOut renderTone(int set, int root, int layer) {
  const double sr = 44100;
  const double target = dsp::noteToHz(root);
  const int64_t N = 8192;                    // loop length (frames)
  const int64_t L0 = set == 1 ? 26000 : 12000;  // transient before the loop
  int m = std::max(1, static_cast<int>(std::lround(target * N / sr)));
  double f = m * sr / static_cast<double>(N);
  const bool hard = layer > 0;
  const double nyq = sr * 0.45;
  // Steady periodic part over one loop length; partials are integer cycles per loop.
  std::vector<float> per(static_cast<size_t>(N), 0.f);
  auto addPartial = [&](double cycles, double amp) {
    if (amp == 0 || cycles * sr / static_cast<double>(N) >= nyq) return;
    double w = dsp::kTwoPi * std::round(cycles) / static_cast<double>(N);
    for (int64_t n = 0; n < N; ++n) per[static_cast<size_t>(n)] += static_cast<float>(amp * std::sin(w * static_cast<double>(n)));
  };
  double bright = hard ? 1.0 : 0.45;
  switch (set) {
    case 0:  // tine keys: strong fundamental, soft upper harmonics
      addPartial(m, 1.0);
      addPartial(2.0 * m, 0.32 * bright);
      addPartial(3.0 * m, 0.10 * bright * bright);
      addPartial(4.0 * m, 0.05 * bright * bright);
      break;
    case 1:  // glass bell: odd harmonics, hollow
      addPartial(m, 1.0);
      addPartial(3.0 * m, 0.22 * bright);
      addPartial(5.0 * m, 0.08 * bright);
      break;
    case 2:  // warm pad: three detuned saw stacks (beating stays loop-periodic)
      for (int d = -1; d <= 1; ++d)
        for (int k = 1; k <= 28; ++k) {
          double amp = (1.0 / k) * std::pow(bright, 0.12 * (k - 1)) * (d == 0 ? 0.5 : 0.35);
          addPartial(static_cast<double>(k) * m + d, amp);  // +-1 cycle per loop: slow, loop-periodic beating
        }
      break;
    case 3:  // round bass: sine core with a little body
      addPartial(m, 1.0);
      addPartial(2.0 * m, 0.25 + 0.2 * bright);
      addPartial(3.0 * m, 0.08 + 0.18 * bright);
      addPartial(5.0 * m, 0.06 * bright);
      break;
    default:  // pluck: bright saw-ish spectrum
      for (int k = 1; k <= 24; ++k) addPartial(static_cast<double>(k) * m, (1.0 / k) * std::pow(bright, 0.25 * (k - 1)));
      break;
  }
  float pk = 1e-9f;
  for (float v : per) pk = std::max(pk, std::fabs(v));
  const float level = hard ? 0.8f : 0.55f;
  for (auto& v : per) v *= level / pk;

  auto s = std::make_shared<Sample>();
  s->path = std::string("tone:") + kSetIds[set] + ":" + std::to_string(root) + ":" + std::to_string(layer);
  s->sampleRate = sr;
  const int64_t total = L0 + N;
  s->left.resize(static_cast<size_t>(total));
  for (int64_t n = 0; n < total; ++n) s->left[static_cast<size_t>(n)] = per[static_cast<size_t>(n % N)];
  // Transient (inharmonic tine / hammer), faded to exactly zero at the loop start.
  struct P { double ratio, amp, tau; };
  std::vector<P> tr;
  if (set == 0) tr = {{7.1, 0.35, 0.05}, {13.3, 0.15, 0.025}, {2.0, 0.25, 0.15}};
  if (set == 1) tr = {{2.76, 0.6, 0.35}, {5.40, 0.4, 0.22}, {8.93, 0.25, 0.12}, {13.3, 0.12, 0.06}};
  if (set == 3) tr = {{1.0, 0.5, 0.06}, {4.0, 0.2, 0.02}};
  if (set == 4) tr = {{9.0, 0.3, 0.02}, {1.0, 0.6, 0.12}};
  for (const auto& p : tr) {
    double fr = f * p.ratio;
    if (fr >= nyq) continue;
    double amp = p.amp * (hard ? 1.0 : 0.45) * level;
    for (int64_t n = 0; n < L0; ++n) {
      double t = static_cast<double>(n) / sr;
      double win = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(n) / static_cast<double>(L0));
      s->left[static_cast<size_t>(n)] += static_cast<float>(amp * std::exp(-t / p.tau) * win * std::sin(dsp::kTwoPi * fr * t));
    }
  }
  // Short fade-in so the first sample is zero.
  for (int64_t n = 0; n < 64; ++n) s->left[static_cast<size_t>(n)] *= static_cast<float>(n) / 64.f;
  s->right = s->left;
  ToneOut o;
  o.smp = s;
  o.loopStart = L0;
  o.loopEnd = total;
  o.tuneCents = static_cast<float>(1200.0 * std::log2(target / f));
  return o;
}

std::mutex gToneMu;
std::map<std::string, ToneOut> gTones;

ToneOut toneFor(const std::string& path) {
  // tone:<set>:<root>:<layer>
  {
    std::lock_guard<std::mutex> lk(gToneMu);
    auto it = gTones.find(path);
    if (it != gTones.end()) return it->second;
  }
  ToneOut o;
  std::string rest = path.substr(5);
  size_t c1 = rest.find(':'), c2 = rest.find(':', c1 == std::string::npos ? 0 : c1 + 1);
  if (c1 == std::string::npos || c2 == std::string::npos) return o;
  std::string id = rest.substr(0, c1);
  int set = -1;
  for (int k = 0; k < 5; ++k)
    if (id == kSetIds[k]) set = k;
  int root = std::atoi(rest.substr(c1 + 1, c2 - c1 - 1).c_str());
  int layer = std::atoi(rest.substr(c2 + 1).c_str());
  if (set < 0 || root < 0 || root > 127) return o;
  o = renderTone(set, root, layer);
  std::lock_guard<std::mutex> lk(gToneMu);
  gTones[path] = o;
  return o;
}

}  // namespace

const std::vector<std::string>& factorySetNames() {
  static const std::vector<std::string> v = {"Tine Keys", "Glass Bell", "Warm Pad", "Round Bass", "Pluck"};
  return v;
}

ZoneMap factoryZoneMap(int set) {
  set = std::clamp(set, 0, 4);
  ZoneMap m;
  std::vector<int> roots = set == 3 ? std::vector<int>{24, 36, 48} : std::vector<int>{36, 48, 60, 72, 84};
  for (int layer = 0; layer < 2; ++layer)
    for (int r : roots) {
      Zone z;
      z.path = std::string("tone:") + kSetIds[set] + ":" + std::to_string(r) + ":" + std::to_string(layer);
      z.name = factorySetNames()[static_cast<size_t>(set)] + " " + (layer ? "hard " : "soft ") + std::to_string(r);
      z.root = r;
      ToneOut t = toneFor(z.path);
      z.tune = std::round(t.tuneCents * 100.f) / 100.f;
      z.loop = LoopSustain;
      z.loopStart = t.loopStart;
      z.loopEnd = t.loopEnd;
      z.vlo = layer ? 90 : 0;
      z.vhi = layer ? 127 : 89;
      m.zones.push_back(z);
    }
  autoMap(m.zones);  // key ranges from the roots; velocity split re-applied below
  for (auto& z : m.zones) {
    bool hard = z.path.back() == '1';
    z.vlo = hard ? 90 : 0;
    z.vhi = hard ? 127 : 89;
  }
  return m;
}

// =========================================================================
// SoundFont 2
// =========================================================================
namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
std::string rdName(const uint8_t* p, size_t n) {
  std::string s(reinterpret_cast<const char*>(p), n);
  size_t z = s.find('\0');
  if (z != std::string::npos) s.resize(z);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// SF2 generator operators used here.
enum Gen {
  gStartOfs = 0, gEndOfs = 1, gLoopStartOfs = 2, gLoopEndOfs = 3, gStartCoarse = 4, gFilterFc = 8, gEndCoarse = 12,
  gPan = 17, gDelayVol = 33, gAttackVol = 34, gHoldVol = 35, gDecayVol = 36, gSustainVol = 37, gReleaseVol = 38,
  gInstrument = 41, gKeyRange = 43, gVelRange = 44, gLoopStartCoarse = 45, gAttenuation = 48, gLoopEndCoarse = 50,
  gCoarseTune = 51, gFineTune = 52, gSampleId = 53, gSampleModes = 54, gScaleTuning = 56, gExclusive = 57,
  gRootKey = 58, gCount = 61
};

struct GenSet {
  std::array<int, gCount> v{};
  std::array<bool, gCount> set{};
  int klo = 0, khi = 127, vlo = 0, vhi = 127;
  void defaults() {
    v.fill(0);
    set.fill(false);
    v[gFilterFc] = 13500;
    for (int g : {21, 23, 25, 26, 27, 28, 30, 33, 34, 35, 36, 38}) v[static_cast<size_t>(g)] = -12000;
    v[gScaleTuning] = 100;
    v[gRootKey] = -1;
    klo = vlo = 0;
    khi = vhi = 127;
  }
  void apply(const Sf2File::Gen& g) {
    if (g.oper >= gCount) return;
    if (g.oper == gKeyRange) {
      klo = g.lo, khi = g.hi;
    } else if (g.oper == gVelRange) {
      vlo = g.lo, vhi = g.hi;
    } else {
      v[g.oper] = g.amount;
    }
    set[g.oper] = true;
  }
};

double tc2s(int tc) { return std::pow(2.0, std::clamp(tc, -12000, 8000) / 1200.0); }

}  // namespace

std::shared_ptr<const Sf2File> parseSf2(const uint8_t* b, size_t n, std::string* err) {
  auto fail = [&](const char* m) -> std::shared_ptr<const Sf2File> {
    if (err) *err = m;
    return nullptr;
  };
  if (n < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "sfbk", 4) != 0) return fail("not a SoundFont 2 file");
  auto f = std::make_shared<Sf2File>();
  size_t riffEnd = std::min(n, static_cast<size_t>(rd32(b + 4)) + 8);
  const uint8_t *phdr = nullptr, *pbag = nullptr, *pgen = nullptr, *inst = nullptr, *ibag = nullptr, *igen = nullptr, *shdr = nullptr;
  size_t nPhdr = 0, nPbag = 0, nPgen = 0, nInst = 0, nIbag = 0, nIgen = 0, nShdr = 0;
  const uint8_t* smpl = nullptr;
  size_t smplBytes = 0;
  size_t pos = 12;
  while (pos + 8 <= riffEnd) {
    const uint8_t* ck = b + pos;
    size_t sz = rd32(ck + 4);
    if (pos + 8 + sz > riffEnd) sz = riffEnd - pos - 8;
    if (std::memcmp(ck, "LIST", 4) == 0 && sz >= 4) {
      size_t lp = pos + 12, lend = pos + 8 + sz;
      while (lp + 8 <= lend) {
        const uint8_t* sc = b + lp;
        size_t ssz = rd32(sc + 4);
        if (lp + 8 + ssz > lend) ssz = lend - lp - 8;
        const uint8_t* d = sc + 8;
        auto is = [&](const char* id) { return std::memcmp(sc, id, 4) == 0; };
        if (is("INAM")) f->name = rdName(d, ssz);
        else if (is("smpl")) smpl = d, smplBytes = ssz;
        else if (is("phdr")) phdr = d, nPhdr = ssz / 38;
        else if (is("pbag")) pbag = d, nPbag = ssz / 4;
        else if (is("pgen")) pgen = d, nPgen = ssz / 4;
        else if (is("inst")) inst = d, nInst = ssz / 22;
        else if (is("ibag")) ibag = d, nIbag = ssz / 4;
        else if (is("igen")) igen = d, nIgen = ssz / 4;
        else if (is("shdr")) shdr = d, nShdr = ssz / 46;
        lp += 8 + ssz + (ssz & 1);
      }
    }
    pos += 8 + sz + (sz & 1);
  }
  if (!phdr || !pbag || !pgen || !inst || !ibag || !igen || !shdr || nPhdr < 2 || nInst < 2 || nShdr < 1)
    return fail("SoundFont is missing preset data");
  if (!smpl) return fail("SoundFont has no sample data");
  f->data.resize(smplBytes / 2);
  for (size_t i = 0; i < f->data.size(); ++i) f->data[i] = static_cast<int16_t>(rd16(smpl + i * 2));
  auto readGens = [](const uint8_t* p, size_t cnt, std::vector<Sf2File::Gen>& out) {
    out.resize(cnt);
    for (size_t i = 0; i < cnt; ++i) {
      out[i].oper = rd16(p + i * 4);
      out[i].amount = static_cast<int16_t>(rd16(p + i * 4 + 2));
      out[i].lo = p[i * 4 + 2];
      out[i].hi = p[i * 4 + 3];
    }
  };
  readGens(pgen, nPgen, f->pgen);
  readGens(igen, nIgen, f->igen);
  f->pbag.resize(nPbag);
  for (size_t i = 0; i < nPbag; ++i) f->pbag[i].gen = rd16(pbag + i * 4);
  f->ibag.resize(nIbag);
  for (size_t i = 0; i < nIbag; ++i) f->ibag[i].gen = rd16(ibag + i * 4);
  f->inst.resize(nInst);
  for (size_t i = 0; i < nInst; ++i) {
    f->inst[i].name = rdName(inst + i * 22, 20);
    f->inst[i].bag = rd16(inst + i * 22 + 20);
  }
  f->samples.resize(nShdr);
  for (size_t i = 0; i < nShdr; ++i) {
    const uint8_t* p = shdr + i * 46;
    auto& s = f->samples[i];
    s.name = rdName(p, 20);
    s.start = rd32(p + 20);
    s.end = rd32(p + 24);
    s.loopStart = rd32(p + 28);
    s.loopEnd = rd32(p + 32);
    s.rate = rd32(p + 36);
    s.originalPitch = p[40];
    s.correction = static_cast<int8_t>(p[41]);
    s.type = rd16(p + 44);
  }
  for (size_t i = 0; i + 1 < nPhdr; ++i) {
    const uint8_t* p = phdr + i * 38;
    Sf2Preset pr;
    pr.name = rdName(p, 20);
    pr.program = rd16(p + 20);
    pr.bank = rd16(p + 22);
    pr.bagStart = rd16(p + 24);
    pr.bagEnd = rd16(p + 38 + 24);
    if (pr.bagEnd < pr.bagStart || pr.bagEnd > nPbag) continue;
    f->presets.push_back(pr);
  }
  std::stable_sort(f->presets.begin(), f->presets.end(), [](const Sf2Preset& a, const Sf2Preset& c) {
    return a.bank != c.bank ? a.bank < c.bank : a.program < c.program;
  });
  if (f->presets.empty()) return fail("SoundFont has no presets");
  return f;
}

namespace {
std::mutex gSfMu;
std::map<std::string, std::shared_ptr<const Sf2File>> gSf2;
std::map<std::string, SamplePtr> gSfSamples;
}  // namespace

std::shared_ptr<const Sf2File> loadSf2(const std::string& path, std::string* err) {
  {
    std::lock_guard<std::mutex> lk(gSfMu);
    auto it = gSf2.find(path);
    if (it != gSf2.end()) return it->second;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot open " + path;
    return nullptr;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto f = parseSf2(bytes.data(), bytes.size(), err);
  if (f) {
    std::lock_guard<std::mutex> lk(gSfMu);
    gSf2[path] = f;
  }
  return f;
}

SamplePtr sf2MakeSample(const Sf2File& f, size_t idx) {
  if (idx >= f.samples.size()) return nullptr;
  const auto& h = f.samples[idx];
  size_t a = std::min<size_t>(h.start, f.data.size()), e = std::min<size_t>(h.end, f.data.size());
  if (e <= a + 1) return nullptr;
  auto s = std::make_shared<Sample>();
  s->sampleRate = h.rate > 0 ? h.rate : 44100;
  s->left.resize(e - a);
  for (size_t i = a; i < e; ++i) s->left[i - a] = static_cast<float>(f.data[i]) / 32768.f;
  s->right = s->left;
  return s;
}

std::vector<Zone> sf2PresetZones(const Sf2File& f, const std::string& path, size_t preset) {
  std::vector<Zone> out;
  if (preset >= f.presets.size()) return out;
  const Sf2Preset& pr = f.presets[preset];
  auto gensOf = [](const std::vector<Sf2File::Bag>& bags, const std::vector<Sf2File::Gen>& gens, size_t bag) {
    std::pair<size_t, size_t> r{0, 0};
    if (bag >= bags.size()) return r;
    r.first = bags[bag].gen;
    r.second = bag + 1 < bags.size() ? bags[bag + 1].gen : gens.size();
    r.first = std::min(r.first, gens.size());
    r.second = std::clamp(r.second, r.first, gens.size());
    return r;
  };
  GenSet pGlobal;
  pGlobal.defaults();
  // Preset values are offsets: start the global preset zone from zeros.
  pGlobal.v.fill(0);
  bool firstP = true;
  for (size_t pb = pr.bagStart; pb < pr.bagEnd && pb < f.pbag.size(); ++pb) {
    auto [g0, g1] = gensOf(f.pbag, f.pgen, pb);
    GenSet pz = pGlobal;
    bool hasInst = false;
    for (size_t g = g0; g < g1; ++g) {
      pz.apply(f.pgen[g]);
      if (f.pgen[g].oper == gInstrument) hasInst = true;
    }
    if (!hasInst) {
      if (firstP) pGlobal = pz;
      firstP = false;
      continue;
    }
    firstP = false;
    size_t ii = static_cast<size_t>(static_cast<uint16_t>(pz.v[gInstrument]));
    if (ii + 1 >= f.inst.size()) continue;
    GenSet iGlobal;
    iGlobal.defaults();
    bool firstI = true;
    for (size_t ib = f.inst[ii].bag; ib < f.inst[ii + 1].bag && ib < f.ibag.size(); ++ib) {
      auto [h0, h1] = gensOf(f.ibag, f.igen, ib);
      GenSet iz = iGlobal;
      bool hasSmp = false;
      for (size_t g = h0; g < h1; ++g) {
        iz.apply(f.igen[g]);
        if (f.igen[g].oper == gSampleId) hasSmp = true;
      }
      if (!hasSmp) {
        if (firstI) iGlobal = iz;
        firstI = false;
        continue;
      }
      firstI = false;
      size_t si = static_cast<size_t>(static_cast<uint16_t>(iz.v[gSampleId]));
      if (si >= f.samples.size()) continue;
      const auto& sh = f.samples[si];
      if (sh.type & 0x8000) continue;  // ROM sample
      int klo = std::max(iz.klo, pz.klo), khi = std::min(iz.khi, pz.khi);
      int vlo = std::max(iz.vlo, pz.vlo), vhi = std::min(iz.vhi, pz.vhi);
      if (klo > khi || vlo > vhi) continue;
      // Instrument value + preset offset for additive generators.
      auto val = [&](int g) {
        bool additive = g != gSampleId && g != gSampleModes && g != gExclusive && g != gRootKey && g != gKeyRange &&
                        g != gVelRange && g != gInstrument && !(g <= 4) && g != gEndCoarse && g != gLoopStartCoarse && g != gLoopEndCoarse;
        return iz.v[static_cast<size_t>(g)] + (additive ? pz.v[static_cast<size_t>(g)] : 0);
      };
      Zone z;
      z.path = "sf2:" + path + "#" + std::to_string(si);
      z.name = sh.name;
      z.lo = klo, z.hi = khi, z.vlo = vlo, z.vhi = vhi;
      int rk = iz.v[gRootKey];
      z.root = rk >= 0 && rk <= 127 ? rk : (sh.originalPitch <= 127 ? sh.originalPitch : 60);
      z.tune = static_cast<float>(val(gCoarseTune) * 100 + val(gFineTune) + sh.correction);
      z.keyTrack = static_cast<float>(val(gScaleTuning)) / 100.f;
      z.gain = -0.04f * static_cast<float>(std::clamp(val(gAttenuation), 0, 1440));  // EMU-style 0.4 dB per cB step
      z.pan = std::clamp(static_cast<float>(val(gPan)) / 500.f, -1.f, 1.f);
      int64_t len = static_cast<int64_t>(sh.end) - sh.start;
      int64_t st = static_cast<int64_t>(iz.v[gStartOfs]) + 32768LL * iz.v[gStartCoarse];
      int64_t en = len + iz.v[gEndOfs] + 32768LL * iz.v[gEndCoarse];
      int64_t ls = static_cast<int64_t>(sh.loopStart) - sh.start + iz.v[gLoopStartOfs] + 32768LL * iz.v[gLoopStartCoarse];
      int64_t le = static_cast<int64_t>(sh.loopEnd) - sh.start + iz.v[gLoopEndOfs] + 32768LL * iz.v[gLoopEndCoarse];
      z.start = std::clamp<int64_t>(st, 0, std::max<int64_t>(0, len - 1));
      en = std::clamp<int64_t>(en, z.start + 1, len);
      z.end = en >= len ? 0 : en;
      int modes = iz.v[gSampleModes] & 3;
      if ((modes == 1 || modes == 3) && le > ls + 1 && ls >= 0 && le <= len) {
        z.loop = modes == 1 ? LoopForward : LoopSustain;
        z.loopStart = ls;
        z.loopEnd = le;
      }
      z.choke = std::clamp(iz.v[gExclusive], 0, 127);
      int fc = val(gFilterFc);
      if (fc < 13500) z.cutoff = static_cast<float>(std::clamp(8.176 * std::pow(2.0, fc / 1200.0), 20.0, 20000.0));
      z.ownEnv = true;
      z.attack = static_cast<float>(tc2s(val(gAttackVol)) + (val(gDelayVol) > -12000 ? tc2s(val(gDelayVol)) : 0.0));
      z.hold = static_cast<float>(val(gHoldVol) > -12000 ? tc2s(val(gHoldVol)) : 0.0);
      z.decay = static_cast<float>(tc2s(val(gDecayVol)));
      z.sustain = static_cast<float>(std::pow(10.0, -std::clamp(val(gSustainVol), 0, 1440) / 200.0));
      z.release = static_cast<float>(tc2s(val(gReleaseVol)));
      clampZone(z);
      out.push_back(z);
    }
  }
  return out;
}

ZoneMap sf2PresetMap(const std::string& path, size_t preset, std::string* err) {
  ZoneMap m;
  auto f = loadSf2(path, err);
  if (!f || preset >= f->presets.size()) return m;
  m.zones = sf2PresetZones(*f, path, preset);
  m.sf2Path = path;
  m.sf2Bank = f->presets[preset].bank;
  m.sf2Program = f->presets[preset].program;
  m.sf2Name = f->presets[preset].name;
  return m;
}

SamplePtr resolveSample(const std::string& path) {
  if (path.rfind("tone:", 0) == 0) return toneFor(path).smp;
  if (path.rfind("sf2:", 0) == 0) {
    {
      std::lock_guard<std::mutex> lk(gSfMu);
      auto it = gSfSamples.find(path);
      if (it != gSfSamples.end()) return it->second;
    }
    size_t h = path.rfind('#');
    if (h == std::string::npos || h < 4) return nullptr;
    auto f = loadSf2(path.substr(4, h - 4));
    if (!f) return nullptr;
    SamplePtr s = sf2MakeSample(*f, static_cast<size_t>(std::atol(path.c_str() + h + 1)));
    if (s) {
      std::lock_guard<std::mutex> lk(gSfMu);
      gSfSamples[path] = s;
    }
    return s;
  }
  return sampleCache().get(path);
}

// =========================================================================
// Scratch moves
// =========================================================================
namespace {
ScratchMove mv(std::initializer_list<ScratchPoint> p, uint32_t mutes = 0) {
  ScratchMove m;
  m.pts = p;
  m.mutes = mutes;
  return m;
}
}  // namespace

const char* factoryMoveName(int k) {
  static const char* n[kMoves] = {"Baby", "Double", "Forward", "Backward", "Chirp", "Flare",
                                  "Tear", "Stab", "Scribble", "Drag", "Rewind", "Straight"};
  return n[std::clamp(k, 0, kMoves - 1)];
}

ScratchMove factoryMove(int k) {
  switch (std::clamp(k, 0, kMoves - 1)) {
    case 0: return mv({{0, 0, CurveSmooth}, {0.5f, 1, CurveSmooth}, {1, 0, CurveSmooth}});
    case 1: return mv({{0, 0, CurveSmooth}, {0.25f, 1, CurveSmooth}, {0.5f, 0, CurveSmooth}, {0.75f, 1, CurveSmooth}, {1, 0, CurveSmooth}});
    case 2: return mv({{0, 0, CurveSmooth}, {0.5f, 1, CurveSmooth}, {1, 0, CurveSmooth}}, 0xFF00u);
    case 3: return mv({{0, 0, CurveSmooth}, {0.5f, 1, CurveSmooth}, {1, 0, CurveSmooth}}, 0x00FFu);
    case 4: return mv({{0, 0, CurveEaseOut}, {0.5f, 1, CurveEaseIn}, {1, 0, CurveSmooth}}, 0x03C0u);
    case 5: return mv({{0, 0, CurveLinear}, {0.5f, 1, CurveLinear}, {1, 0, CurveLinear}}, 0x2424u);
    case 6: return mv({{0, 0, CurveSmooth}, {0.2f, 0.45f, CurveHold}, {0.3f, 0.45f, CurveSmooth}, {0.5f, 1, CurveSmooth}, {1, 0, CurveSmooth}});
    case 7: return mv({{0, 0, CurveEaseOut}, {0.18f, 1, CurveHold}, {1, 1, CurveHold}}, 0xFFF0u);
    case 8: {
      ScratchMove m;
      for (int i = 0; i <= 16; ++i) m.pts.push_back({i / 16.f, 0.25f + ((i & 1) ? 0.12f : 0.f), CurveSmooth});
      return m;
    }
    case 9: return mv({{0, 0, CurveLinear}, {1, 0.35f, CurveLinear}});
    case 10: return mv({{0, 0.9f, CurveEaseIn}, {1, 0, CurveLinear}});
    default: return mv({{0, 0, CurveLinear}, {1, 1, CurveLinear}});
  }
}

float evalMove(const ScratchMove& m, float t) {
  if (m.pts.empty()) return 0;
  if (t <= m.pts.front().t) return m.pts.front().y;
  for (size_t i = 0; i + 1 < m.pts.size(); ++i) {
    const auto& a = m.pts[i];
    const auto& b = m.pts[i + 1];
    if (t > b.t) continue;
    float span = b.t - a.t;
    float u = span > 1e-6f ? (t - a.t) / span : 1.f;
    float w = u;
    switch (a.curve) {
      case CurveSmooth: w = 0.5f - 0.5f * std::cos(static_cast<float>(kPi) * u); break;
      case CurveEaseIn: w = 1.f - std::cos(static_cast<float>(kPi) * 0.5f * u); break;
      case CurveEaseOut: w = std::sin(static_cast<float>(kPi) * 0.5f * u); break;
      case CurveHold: w = u >= 1.f ? 1.f : 0.f; break;
      default: break;
    }
    return a.y + (b.y - a.y) * w;
  }
  return m.pts.back().y;
}

ScratchData parseScratch(const std::string& data) {
  ScratchData d;
  for (int k = 0; k < kMoves; ++k) d.moves[static_cast<size_t>(k)] = factoryMove(k);
  auto lines = splitLines(data);
  if (!lines.empty() && lines[0].rfind("m ", 0) != 0) d.path = lines[0];
  for (const auto& l : lines) {
    if (l.rfind("m ", 0) != 0) continue;
    auto toks = splitWs(l);
    if (toks.size() < 3) continue;
    int k = std::atoi(toks[1].c_str());
    if (k < 0 || k >= kMoves) continue;
    ScratchMove m;
    m.mutes = static_cast<uint32_t>(std::strtoul(toks[2].c_str(), nullptr, 16)) & ((1u << kMuteSteps) - 1);
    for (size_t i = 3; i < toks.size() && m.pts.size() < 64; ++i) {
      ScratchPoint p;
      int c = 0;
      if (std::sscanf(toks[i].c_str(), "%f,%f,%d", &p.t, &p.y, &c) >= 2) {
        p.t = std::clamp(p.t, 0.f, 1.f);
        p.y = std::clamp(p.y, 0.f, 1.f);
        p.curve = std::clamp(c, 0, static_cast<int>(CurveHold));
        m.pts.push_back(p);
      }
    }
    std::stable_sort(m.pts.begin(), m.pts.end(), [](const ScratchPoint& a, const ScratchPoint& b) { return a.t < b.t; });
    if (m.pts.size() >= 2) {
      m.pts.front().t = 0;
      m.pts.back().t = 1;
      d.moves[static_cast<size_t>(k)] = m;
    }
  }
  return d;
}

std::string serializeScratch(const ScratchData& d) {
  std::string o = d.path + "\n";
  for (int k = 0; k < kMoves; ++k) {
    const auto& m = d.moves[static_cast<size_t>(k)];
    ScratchMove f = factoryMove(k);
    bool same = f.mutes == m.mutes && f.pts.size() == m.pts.size();
    for (size_t i = 0; same && i < m.pts.size(); ++i)
      same = f.pts[i].t == m.pts[i].t && f.pts[i].y == m.pts[i].y && f.pts[i].curve == m.pts[i].curve;
    if (same) continue;
    char b[32];
    std::snprintf(b, sizeof b, "m %d %x", k, m.mutes);
    o += b;
    for (const auto& p : m.pts) {
      std::snprintf(b, sizeof b, " %.4g,%.4g,%d", p.t, p.y, p.curve);
      o += b;
    }
    o += "\n";
  }
  return o;
}

SamplePtr scratchDefaultSample() {
  static std::once_flag once;
  static SamplePtr smp;
  std::call_once(once, [] {
    const double sr = 44100;
    const size_t n = static_cast<size_t>(sr * 2.0);
    auto s = std::make_shared<Sample>();
    s->path = "tone:phrase";
    s->sampleRate = sr;
    s->left.assign(n, 0.f);
    // Two vocal-like "ah"/"oh" stabs: a gliding buzz through two formant band-passes.
    auto voice = [&](double t0, double dur, double f0, double f1, double fa, double fb) {
      dsp::Biquad b1, b2;
      b1.set(dsp::Biquad::BandPass, sr, fa, 6, 0);
      b2.set(dsp::Biquad::BandPass, sr, fb, 8, 0);
      double ph = 0;
      size_t a = static_cast<size_t>(t0 * sr), e = std::min(n, a + static_cast<size_t>(dur * sr));
      for (size_t i = a; i < e; ++i) {
        double u = static_cast<double>(i - a) / static_cast<double>(e - a);
        double f = f0 + (f1 - f0) * u;
        ph += f / sr;
        ph -= std::floor(ph);
        double saw = 2.0 * ph - 1.0;
        double env = std::min(1.0, u * 40.0) * std::pow(1.0 - u, 1.5);
        s->left[i] += static_cast<float>(env * (b1.process(saw) * 1.6 + b2.process(saw) * 0.9));
      }
    };
    voice(0.0, 0.45, 210, 150, 760, 1180);
    voice(1.5, 0.45, 160, 120, 520, 900);
    // A small beat from the built-in drum sounds.
    auto hit = [&](const char* name, double t, float g) {
      SamplePtr d = sampleCache().get(std::string("builtin:") + name);
      if (!d) return;
      size_t a = static_cast<size_t>(t * sr);
      for (size_t i = 0; i < d->frames() && a + i < n; ++i) s->left[a + i] += g * d->left[i];
    };
    hit("kick", 0.5, 0.9f);
    hit("hat", 0.75, 0.5f);
    hit("snare", 1.0, 0.8f);
    hit("hat", 1.25, 0.5f);
    float pk = 1e-9f;
    for (float v : s->left) pk = std::max(pk, std::fabs(v));
    for (auto& v : s->left) v *= 0.85f / pk;
    s->right = s->left;
    smp = s;
  });
  return smp;
}

}  // namespace gl::packs::samplers

#include "samplers_rt.h"
