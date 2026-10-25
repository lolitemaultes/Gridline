// Gridline: Kickstart / Kickstart 2 states read by Tideline (stand-ins, see
// core/PluginStandIn.h and tideline.h).
//
// Worked out by black-box means only (states saved by the plugin itself and
// its published parameter list; no vendor code or data): the VST3 component
// state (and the AU "jucePluginState" / VST2 chunk) is a JUCE value tree,
// usually "#zip#\0" + zlib data. Its root "PluginState" holds children
// {type: int/float/bool, id, value}: loopLength (0..3), usedWave (0..15),
// speedType, mix (0..1), bandOn, bandFrequency (0..1), bandSolo,
// triggerSmoothing, "oneShotEndPhase" (one value per curve), "midi" and
// "waves": 16 curves of points {x 0..8192 over one loop, y -1..1, factor}.
//
// Approximations: the curve is redrawn through its points with Tideline's 8
// nodes (Kickstart's curve segments are not reproduced exactly); band
// frequency uses Tideline's logarithmic 20 Hz .. 5.12 kHz range; speedType
// 1 is read as MIDI triggering.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../core/Plist.h"
#include "../../core/PluginInfo.h"
#include "../../core/Zip.h"
#include "../../host/Backend.h"
#include "../../host/HostedPlugins.h"
#include "tideline.h"

namespace gl::packs::tideline {

namespace {

// ------------------------------------------------------------------ JUCE value tree
struct Var {
  int type = 0;  // 1 int, 2 true, 3 false, 4 double, 5 string, 6 int64
  double num = 0;
  std::string str;
};
struct Tree {
  std::string type;
  std::vector<std::pair<std::string, Var>> props;
  std::vector<Tree> kids;
  const Var* prop(const char* k) const {
    for (const auto& [n, v] : props)
      if (n == k) return &v;
    return nullptr;
  }
  std::string id() const {
    const Var* v = prop("id");
    return v && v->type == 5 ? v->str : "";
  }
  const Tree* child(const char* id) const {
    for (const auto& k : kids)
      if (k.id() == id) return &k;
    return nullptr;
  }
};

struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  int depth = 0;
  uint8_t byte() {
    if (at >= n) {
      ok = false;
      return 0;
    }
    return p[at++];
  }
  int64_t cint() {
    const uint8_t b = byte();
    const int len = b & 0x7F;
    if (len > 8 || at + static_cast<size_t>(len) > n) {
      ok = false;
      return 0;
    }
    uint64_t v = 0;
    for (int i = len - 1; i >= 0; --i) v = v << 8 | p[at + static_cast<size_t>(i)];
    at += static_cast<size_t>(len);
    return (b & 0x80) ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
  }
  std::string string() {
    std::string s;
    while (ok && at < n && p[at]) s += static_cast<char>(p[at++]);
    if (at >= n) ok = false;
    else ++at;
    return s;
  }
  Var var() {
    Var v;
    const int64_t size = cint();
    if (!ok || size < 1 || static_cast<uint64_t>(size) > n - at) {
      ok = false;
      return v;
    }
    const size_t start = at, end = at + static_cast<size_t>(size);
    v.type = p[at++];
    const size_t body = end - at;
    if (v.type == 1 && body >= 4) {
      int32_t i;
      std::memcpy(&i, p + at, 4);
      v.num = i;
    } else if (v.type == 2 || v.type == 3) {
      v.num = v.type == 2 ? 1 : 0;
    } else if (v.type == 4 && body >= 8) {
      std::memcpy(&v.num, p + at, 8);
    } else if (v.type == 5) {
      v.str.assign(reinterpret_cast<const char*>(p + at), body ? body - 1 : 0);
    } else if (v.type == 6 && body >= 8) {
      int64_t i;
      std::memcpy(&i, p + at, 8);
      v.num = static_cast<double>(i);
    }
    (void)start;
    at = end;
    return v;
  }
  bool tree(Tree& t) {
    if (++depth > 32) return ok = false;
    t.type = string();
    const int64_t np = cint();
    if (!ok || np < 0 || np > 4096) return ok = false;
    for (int64_t i = 0; i < np && ok; ++i) {
      std::string k = string();
      t.props.emplace_back(std::move(k), var());
    }
    const int64_t nc = cint();
    if (!ok || nc < 0 || nc > 65536) return ok = false;
    t.kids.resize(static_cast<size_t>(nc));
    for (auto& k : t.kids)
      if (!tree(k)) return false;
    --depth;
    return ok;
  }
};

// The value tree inside saved state bytes (plain or "#zip#" compressed; searched).
bool readTree(const std::string& bytes, Tree& t) {
  const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
  const size_t z = bytes.find("#zip#");
  if (z != std::string::npos && z + 8 < bytes.size()) {
    std::vector<uint8_t> raw;
    if (!inflateRaw(p + z + 8, bytes.size() - z - 8, raw, size_t(16) << 20)) return false;
    Reader r{raw.data(), raw.size()};
    return r.tree(t) && t.type == "PluginState";
  }
  const size_t s = bytes.find(std::string("PluginState\0", 12));
  if (s == std::string::npos) return false;
  Reader r{p + s, bytes.size() - s};
  return r.tree(t);
}

// Hosted state -> the bytes the plugin saved.
bool stateBytes(const std::string& data, std::string& out, std::string& fmt) {
  if (data.compare(0, 7, "vst3s1:") == 0) {
    std::string ctrl;
    fmt = "VST3 state";
    return host::decodeState(data, out, ctrl);
  }
  if (data.compare(0, 4, "au1:") == 0) {
    std::string plist;
    fmt = "AU state";
    if (!host::base64Decode(data.substr(4), plist)) return false;
    return plist::topValue(plist, "jucePluginState", out) || plist::topValue(plist, "vstdata", out);
  }
  if (data.compare(0, 7, "vst2c1:") == 0) {
    fmt = "VST2 chunk";
    return host::base64Decode(data.substr(7), out);
  }
  return false;
}

double num(const Tree& root, const char* id, double def) {
  const Tree* c = root.child(id);
  if (!c) return def;
  const Var* v = c->prop("value");
  return v && v->type != 5 && v->type != 0 ? v->num : def;
}

// ------------------------------------------------------------------ parameters
bool isVst2(const std::string& type) { return type.compare(0, 5, "vst2:") == 0; }

// Hosted parameter positions (VST3 / AU: 5 parameters; VST2: 12 with reserved slots).
enum KParam { KLoop, KWave, KBandOn, KBandFreq, KMix, KCount };
int kparamOf(const std::string& type, int index) {
  if (isVst2(type)) {
    static const int map[12] = {-1, KLoop, -1, KWave, -1, -1, -1, -1, KBandOn, KBandFreq, -1, KMix};
    return index >= 0 && index < 12 ? map[index] : -1;
  }
  return index >= 0 && index < KCount ? index : -1;
}
const char* const kTitles[KCount] = {"Loop Length", "Used Wave", "Band On", "Band Frequency", "Mix"};

int rateOfLoop(int loop) { return std::clamp(loop, 0, 3) + 1; }  // 1/8, 1/4, 1/2, 1 bar
double bandHz(double n) { return 20.0 * std::pow(256.0, std::clamp(n, 0.0, 1.0)); }

// Points (phase 0..1, gain 0..1) -> 8 nodes: kept by largest deviation, padded on the longest gap.
void fitNodes(std::vector<std::pair<double, double>> pts, std::vector<float>& out) {
  std::sort(pts.begin(), pts.end());
  if (pts.empty()) return;
  std::vector<size_t> keep = {0, pts.size() - 1};
  auto interp = [&](size_t i) {
    size_t a = 0, b = pts.size() - 1;
    for (size_t k : keep) {
      if (k <= i && k >= a) a = k;
      if (k >= i && k <= b) b = k;
    }
    const double w = pts[b].first - pts[a].first;
    const double t = w > 1e-9 ? (pts[i].first - pts[a].first) / w : 0;
    return pts[a].second + (pts[b].second - pts[a].second) * t;
  };
  while (keep.size() < static_cast<size_t>(kNodes) && keep.size() < pts.size()) {
    double worst = -1;
    size_t at = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
      if (std::find(keep.begin(), keep.end(), i) != keep.end()) continue;
      const double e = std::fabs(pts[i].second - interp(i));
      if (e > worst) worst = e, at = i;
    }
    keep.push_back(at);
  }
  std::sort(keep.begin(), keep.end());
  std::vector<std::pair<double, double>> nodes;
  for (size_t k : keep) nodes.push_back(pts[k]);
  while (nodes.size() < static_cast<size_t>(kNodes)) {  // split the longest segment (exact on a line)
    size_t at = 0;
    double gap = -1;
    for (size_t i = 0; i + 1 < nodes.size(); ++i)
      if (nodes[i + 1].first - nodes[i].first > gap) gap = nodes[i + 1].first - nodes[i].first, at = i;
    if (nodes.size() == 1) {
      nodes.push_back(nodes[0]);
      continue;
    }
    const auto a = nodes[at], b = nodes[at + 1];
    nodes.insert(nodes.begin() + static_cast<std::ptrdiff_t>(at + 1), {(a.first + b.first) / 2, (a.second + b.second) / 2});
  }
  for (int i = 0; i < kNodes; ++i) {
    out[static_cast<size_t>(BwNodeX0 + i)] = static_cast<float>(std::clamp(nodes[static_cast<size_t>(i)].first, 0.0, 1.0));
    out[static_cast<size_t>(BwNodeY0 + i)] = static_cast<float>(std::clamp(nodes[static_cast<size_t>(i)].second, 0.0, 1.0));
  }
}

bool waveNodes(const Tree& root, int wave, std::vector<float>& out) {
  const Tree* waves = root.child("waves");
  if (!waves || wave < 0 || wave >= static_cast<int>(waves->kids.size())) return false;
  const Tree& w = waves->kids[static_cast<size_t>(wave)];
  const Tree* points = w.child("points");
  if (!points) return false;
  std::vector<std::pair<double, double>> pts;
  for (const auto& pt : points->kids) {
    const double x = num(pt, "x", -1), y = num(pt, "y", 0);
    if (x < 0 || x > 8192 || !std::isfinite(y)) continue;
    pts.emplace_back(x / 8192.0, (std::clamp(y, -1.0, 1.0) + 1.0) / 2.0);
  }
  if (pts.size() < 2) return false;
  fitNodes(std::move(pts), out);
  return true;
}

}  // namespace

bool kickstartToTideline(const std::string& type, const std::string& data, const std::vector<float>& hosted, std::vector<float>& out,
                         KickstartReport& rep) {
  rep = KickstartReport{};
  const PluginInfo* info = findPlugin("gl.tideline");
  if (!info) {
    rep.error = "Tideline is not available";
    return false;
  }
  out.assign(info->params.size(), 0.0f);
  for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(info->params[i].def);
  out[BwTension] = 0;
  std::string bytes, fmt;
  Tree root;
  const bool tree = stateBytes(data, bytes, fmt) && readTree(bytes, root);
  auto hp = [&](int k, double def) {
    for (int i = 0; i < static_cast<int>(hosted.size()); ++i)
      if (kparamOf(type, i) == k) return static_cast<double>(hosted[static_cast<size_t>(i)]);
    return def;
  };
  if (tree) {
    rep.format = "Kickstart (" + fmt + ")";
    const int wave = static_cast<int>(std::lround(num(root, "usedWave", 0)));
    out[BwRate] = static_cast<float>(rateOfLoop(static_cast<int>(std::lround(num(root, "loopLength", 1)))));
    out[BwAmount] = static_cast<float>(std::clamp(num(root, "mix", 1), 0.0, 1.0));
    out[BwBand] = num(root, "bandOn", 0) >= 0.5 ? BandLows : BandFull;
    out[BwBandFreq] = static_cast<float>(bandHz(num(root, "bandFrequency", 0.5)));
    out[BwBandListen] = num(root, "bandSolo", 0) >= 0.5 ? 1.0f : 0.0f;
    out[BwSmooth] = num(root, "triggerSmoothing", 1) >= 0.5 ? 1.0f : 0.0f;
    const int speed = static_cast<int>(std::lround(num(root, "speedType", 0)));
    out[BwTrigger] = speed == 1 ? TrigMidi : TrigTempo;
    if (speed > 1) rep.notes.push_back("Kickstart trigger mode not recognized: tempo sync is used");
    if (const Tree* ends = root.child("oneShotEndPhase"); ends && wave >= 0 && wave < static_cast<int>(ends->kids.size())) {
      const Var* v = ends->kids[static_cast<size_t>(wave)].prop("value");
      if (v && v->type == 4) out[BwEnd] = static_cast<float>(std::clamp(v->num, 0.0, 1.0));
    }
    if (waveNodes(root, wave, out)) {
      out[BwShape] = kShapeCustom;
      rep.notes.push_back("The Kickstart curve is redrawn with Tideline's 8 custom points");
    } else {
      out[BwShape] = 0;
      rep.notes.push_back("The Kickstart curve could not be read: Tideline's Pump shape is used");
    }
  } else if (!hosted.empty()) {
    rep.format = "Kickstart (parameters only)";
    out[BwRate] = static_cast<float>(rateOfLoop(static_cast<int>(std::lround(hp(KLoop, 1.0 / 3) * 3))));
    out[BwAmount] = static_cast<float>(std::clamp(hp(KMix, 1), 0.0, 1.0));
    out[BwBand] = hp(KBandOn, 0) >= 0.5 ? BandLows : BandFull;
    out[BwBandFreq] = static_cast<float>(bandHz(hp(KBandFreq, 0.5)));
    out[BwShape] = 0;
    rep.notes.push_back("The Kickstart state could not be read: its curve is replaced by Tideline's Pump shape");
  } else {
    rep.error = data.empty() ? "no saved state" : "unsupported state format";
    return false;
  }
  if (out[BwBand] != BandFull) rep.notes.push_back("Band split frequency follows Tideline's range (approximate)");
  if (out[BwTrigger] == TrigMidi) rep.notes.push_back("MIDI triggering listens to any note");
  return true;
}

int kickstartParamIndexOfId(const std::string& type, uint32_t id) {
  if (isVst2(type)) return id < 12 ? static_cast<int>(id) : -1;
  static const uint32_t ids[KCount] = {49, 51, 56, 57, 1568};
  for (int k = 0; k < KCount; ++k)
    if (ids[k] == id) return k;
  return -1;
}

bool kickstartParamLink(const std::string& type, int index, int& param, std::vector<float>& curve, std::string& title) {
  param = -1;
  curve.clear();
  const int k = kparamOf(type, index);
  if (k < 0) return false;
  title = kTitles[k];
  const PluginInfo* info = findPlugin("gl.tideline");
  if (!info || k == KWave) return false;  // curve selection: no single Tideline parameter
  constexpr int kSamples = 17;
  param = k == KLoop ? BwRate : k == KBandOn ? BwBand : k == KBandFreq ? BwBandFreq : BwAmount;
  const auto& pd = info->params[static_cast<size_t>(param)];
  for (int i = 0; i < kSamples; ++i) {
    const double n = static_cast<double>(i) / (kSamples - 1);
    double plain = n;
    if (k == KLoop) plain = rateOfLoop(static_cast<int>(std::lround(n * 3)));
    else if (k == KBandOn) plain = n >= 0.5 ? BandLows : BandFull;
    else if (k == KBandFreq) plain = bandHz(n);
    curve.push_back(static_cast<float>(pd.toNormalized(plain)));
  }
  return true;
}

}  // namespace gl::packs::tideline
