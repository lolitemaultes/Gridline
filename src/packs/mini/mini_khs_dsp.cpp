// Gridline: carrying a missing Kilohearts effect's saved parameters over to
// its Gridline Suite Mini stand-in (see mini_khs.h; the table is mini_khs_info.cpp).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include "../Packs.h"
#include "mini_khs.h"

namespace gl::packs::mini::khs {

namespace {

std::string fold(const std::string& s) {
  std::string o;
  for (unsigned char c : s)
    if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
  return o;
}

std::string upper(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return o;
}

enum class Fmt { Au, Vst3, Vst2, Other };

Fmt fmtOf(const Product& p, const std::string& type) {
  if (type == auType(p)) return Fmt::Au;
  if (upper(type) == upper(vst3Type(p))) return Fmt::Vst3;
  if (upper(type) == upper(vst2Type(p))) return Fmt::Vst2;
  return Fmt::Other;
}

int paramIndex(const Product& p, const char* title) {
  for (size_t i = 0; i < p.params.size(); ++i)
    if (std::strcmp(p.params[i].title, title) == 0) return static_cast<int>(i);
  return -1;
}

// The plugin's normalized value of product parameter `pi` (-1 when not saved).
double normalized(const Product& p, const std::vector<int>& list, bool vst3, const std::vector<float>& hosted, int pi) {
  for (size_t k = 0; k < list.size() && k < hosted.size(); ++k) {
    if (list[k] != pi) continue;
    double v = hosted[k];
    const int steps = p.params[static_cast<size_t>(pi)].vst3Steps;
    if (vst3 && steps > 1) v /= steps;  // VST3 choices are saved as their index
    return std::clamp(std::isfinite(v) ? v : 0.0, 0.0, 1.0);
  }
  return -1;
}

double plainOf(const Map& m, double n) {
  switch (m.read) {
    case Read::Lin: return m.v.size() >= 2 ? m.v[0] + (m.v[1] - m.v[0]) * n : n;
    case Read::Exp: return m.v.size() >= 2 && m.v[0] > 0 ? m.v[0] * std::pow(static_cast<double>(m.v[1]) / m.v[0], n) : n;
    case Read::Points: {
      if (m.v.size() < 2) return n;
      const double x = n * static_cast<double>(m.v.size() - 1);
      const size_t i = std::min(static_cast<size_t>(x), m.v.size() - 2);
      return m.v[i] + (m.v[i + 1] - m.v[i]) * (x - static_cast<double>(i));
    }
    case Read::Index: {
      if (m.v.empty()) return kKeepDefault;
      const size_t i = static_cast<size_t>(std::lround(n * static_cast<double>(m.v.size() - 1)));
      return m.v[std::min(i, m.v.size() - 1)];
    }
    case Read::Beats: break;
  }
  return kKeepDefault;
}

// The Mini value of map `m` at normalized n (kKeepDefault: none).
double miniValue(const Map& m, double n, const ParamDesc& d) {
  double v = plainOf(m, n);
  if (v <= kKeepDefault * 0.5) return kKeepDefault;
  if (m.read != Read::Index) v = v * m.mul + m.add;
  return std::clamp(v, d.min, d.max);
}

// Beats of "steps" (1..64) and "Delay Denominator" (4, 4t, 8, 8t, 16, 16t, 32).
double beatsOf(double nSteps, double nDenom) {
  static const double kDen[] = {1.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
  const double steps = 1.0 + std::round(nSteps * 63.0);
  const int d = static_cast<int>(std::lround(std::clamp(nDenom, 0.0, 1.0) * 6.0));
  return steps * kDen[std::clamp(d, 0, 6)];
}

int nearestBeat(const Map& m, double beats) {
  int best = 0;
  double err = 1e9;
  for (int i = 0; i < m.beatCount; ++i) {
    const double e = std::fabs(std::log(m.beats[i] / beats));
    if (e < err) err = e, best = i;
  }
  return best;
}

}  // namespace

std::string auType(const Product& p) { return std::string("au:aufx/") + p.au + "/ kHs"; }
std::string vst3Type(const Product& p) { return std::string("vst3:") + p.vst3; }
std::string vst2Type(const Product& p) { return std::string("vst2:") + p.vst2; }

const Product* find(const std::string& type, const std::string& name, const std::string& vendor) {
  const std::string ut = upper(type);
  for (const Product& p : products())
    if (type == auType(p) || ut == upper(vst3Type(p)) || ut == upper(vst2Type(p))) return &p;
  if (name.empty()) return nullptr;
  const std::string v = fold(vendor);
  if (!v.empty() && v.find("kilohearts") == std::string::npos) return nullptr;
  std::string n = fold(name);
  if (n.size() > 3 && n.compare(n.size() - 3, 3, "x64") == 0) n.resize(n.size() - 3);
  for (const Product& p : products())
    if (n == fold(p.name)) return &p;
  return nullptr;
}

std::vector<int> hostedList(const Product& p, const std::string& type, size_t count) {
  const Fmt f = fmtOf(p, type);
  std::vector<int> all, vst3;
  for (size_t i = 0; i < p.params.size(); ++i) {
    all.push_back(static_cast<int>(i));
    if (p.params[i].vst3Id >= 0) vst3.push_back(static_cast<int>(i));
  }
  if (f == Fmt::Vst3) return vst3;
  if (f == Fmt::Au || f == Fmt::Vst2) return all;
  // A format the table does not name (e.g. a project from another build): by the saved count.
  if (count == all.size()) return all;
  if (count == vst3.size()) return vst3;
  return {};
}

bool toMini(const Product& p, const std::string& type, const std::vector<float>& hosted, PluginState& out, std::vector<std::string>& notes) {
  const PluginInfo* info = findPlugin(p.mini);
  if (!info) return false;
  out = defaultPluginState(p.mini);
  const std::vector<int> list = hostedList(p, type, hosted.size());
  const bool vst3 = fmtOf(p, type) == Fmt::Vst3 || (fmtOf(p, type) == Fmt::Other && list.size() < p.params.size());
  if (list.empty() || hosted.size() < list.size()) {
    notes.push_back("No saved parameters could be read: " + info->name + " starts from its defaults");
    return true;
  }
  int carried = 0;
  for (const Map& m : p.maps) {
    if (m.mini < 0 || static_cast<size_t>(m.mini) >= info->params.size()) continue;
    const ParamDesc& d = info->params[static_cast<size_t>(m.mini)];
    const double n = normalized(p, list, vst3, hosted, paramIndex(p, m.title));
    if (n < 0) continue;
    double v;
    if (m.read == Read::Beats) {
      const double nd = normalized(p, list, vst3, hosted, paramIndex(p, "Delay Denominator"));
      v = nearestBeat(m, beatsOf(n, nd < 0 ? 0.5 : nd));
    } else {
      v = miniValue(m, n, d);
      if (v <= kKeepDefault * 0.5) {
        notes.push_back(std::string(m.title) + ": no counterpart in " + info->name + ", its default is used");
        continue;
      }
    }
    if (d.kind != ParamKind::Continuous) v = std::round(v);
    out.params[static_cast<size_t>(m.mini)] = static_cast<float>(v);
    ++carried;
  }
  normalizePluginState(out);
  const int en = paramIndex(p, "Enabled");
  if (en >= 0) {
    const double n = normalized(p, list, vst3, hosted, en);
    if (n >= 0 && n < 0.5) notes.push_back(std::string(p.name) + " was switched off in the project");
  }
  const size_t others = p.params.size() - p.maps.size() - (en >= 0 ? 1 : 0);
  if (others > 0)
    notes.push_back(std::to_string(others) + " other setting" + (others == 1 ? "" : "s") + " of " + p.name + " ha" + (others == 1 ? "s" : "ve") +
                    " no counterpart in " + info->name + " (its default is used)");
  if (carried == 0) notes.push_back("No setting of " + std::string(p.name) + " carried over: " + info->name + " starts from its defaults");
  return true;
}

bool paramLink(const Product& p, const std::string& type, size_t count, int index, int& mini, std::vector<float>& curve, std::string& title) {
  mini = -1;
  curve.clear();
  const std::vector<int> list = hostedList(p, type, count);
  if (index < 0 || static_cast<size_t>(index) >= list.size()) return false;
  const Param& pr = p.params[static_cast<size_t>(list[static_cast<size_t>(index)])];
  title = pr.title;
  const PluginInfo* info = findPlugin(p.mini);
  if (!info) return false;
  for (const Map& m : p.maps) {
    if (std::strcmp(m.title, pr.title) != 0 || m.read == Read::Beats) continue;
    if (m.mini < 0 || static_cast<size_t>(m.mini) >= info->params.size()) return false;
    const ParamDesc& d = info->params[static_cast<size_t>(m.mini)];
    constexpr int kSamples = 17;
    for (int i = 0; i < kSamples; ++i) {
      double v = miniValue(m, static_cast<double>(i) / (kSamples - 1), d);
      if (v <= kKeepDefault * 0.5) v = d.def;
      if (d.kind != ParamKind::Continuous) v = std::round(v);
      curve.push_back(static_cast<float>(d.toNormalized(v)));
    }
    mini = m.mini;
    return true;
  }
  return false;
}

int paramIndexOfId(const Product& p, const std::string& type, uint32_t id) {
  const Fmt f = fmtOf(p, type);
  const std::vector<int> list = hostedList(p, type, 0);
  if (f == Fmt::Vst2 || f == Fmt::Au) return id < list.size() ? static_cast<int>(id) : -1;  // AU ids are the positions
  if (f != Fmt::Vst3) return -1;
  for (size_t k = 0; k < list.size(); ++k)
    if (p.params[static_cast<size_t>(list[k])].vst3Id == static_cast<int>(id)) return static_cast<int>(k);
  return -1;
}

}  // namespace gl::packs::mini::khs
