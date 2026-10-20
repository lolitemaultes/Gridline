// Gridline — FLP export: plugin encoder engine (see FlpEncode.h).
#include "FlpEncode.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include "../../engine/ChannelIns.h"

namespace gl::flp::enc {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

void typeRange(Type t, double& lo, double& hi) {
  switch (t) {
    case Type::U8: lo = 0, hi = 255; break;
    case Type::I8: lo = -128, hi = 127; break;
    case Type::U16: lo = 0, hi = 65535; break;
    case Type::I16: lo = -32768, hi = 32767; break;
    case Type::U32: lo = 0, hi = 4294967295.0; break;
    case Type::I32: lo = -2147483648.0, hi = 2147483647.0; break;
    case Type::F32: lo = -1e6, hi = 1e6; break;
    case Type::F64: lo = -1e6, hi = 1e6; break;
  }
}
bool isFloat(Type t) { return t == Type::F32 || t == Type::F64; }
}  // namespace

size_t typeSize(Type t) {
  switch (t) {
    case Type::U8: case Type::I8: return 1;
    case Type::U16: case Type::I16: return 2;
    case Type::U32: case Type::I32: case Type::F32: return 4;
    case Type::F64: return 8;
  }
  return 4;
}

double readField(const Bytes& b, size_t at, Type t) {
  if (at + typeSize(t) > b.size()) return kNaN;
  switch (t) {
    case Type::U8: return b[at];
    case Type::I8: return static_cast<int8_t>(b[at]);
    case Type::U16: return getU16(b, at);
    case Type::I16: return static_cast<int16_t>(getU16(b, at));
    case Type::U32: return getU32(b, at);
    case Type::I32: return static_cast<int32_t>(getU32(b, at));
    case Type::F32: return getF32(b, at);
    case Type::F64: return getF64(b, at);
  }
  return kNaN;
}

void writeField(Bytes& b, size_t at, Type t, double v) {
  if (at + typeSize(t) > b.size() || !std::isfinite(v)) return;
  double lo, hi;
  typeRange(t, lo, hi);
  if (!isFloat(t)) v = std::clamp(std::round(v), lo, hi);
  switch (t) {
    case Type::U8: case Type::I8: putU8(b, at, static_cast<uint32_t>(static_cast<int64_t>(v))); break;
    case Type::U16: case Type::I16: putU16(b, at, static_cast<uint32_t>(static_cast<int64_t>(v))); break;
    case Type::U32: putU32(b, at, static_cast<uint32_t>(v)); break;
    case Type::I32: putI32(b, at, static_cast<int32_t>(v)); break;
    case Type::F32: putF32(b, at, static_cast<float>(v)); break;
    case Type::F64: putF64(b, at, v); break;
  }
}

std::string flPath(const std::string& path, const EncodeContext& c) {
  std::string p = path;
  if (!c.factoryData.empty() && p.rfind(c.factoryData, 0) == 0) {
    std::string rest = p.substr(c.factoryData.size());
    p = "%FLStudioFactoryData%" + rest;
  }
  if (p.rfind("%FLStudioFactoryData%", 0) == 0)
    for (char& ch : p)
      if (ch == '/') ch = '\\';
  return p;
}

// ------------------------------------------------------------------ registry

const std::vector<Encoder>& encoders() {
  static const std::vector<Encoder> all = [] {
    std::vector<Encoder> v;
    registerFxEq(v);
    registerFxDyn(v);
    registerFxTime(v);
    registerFxK3(v);
    registerFxMisc(v);
    registerFx2Mod(v);
    registerFx2Pitch(v);
    registerFx2Space(v);
    registerFx2Tone(v);
    registerDec3(v);
    registerGenA(v);
    registerGenB(v);
    registerJ22(v);
    registerWrapper(v);
    return v;
  }();
  return all;
}

namespace {
std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}
}  // namespace

const Encoder* findEncoder(const std::string& flName, const std::string& glType) {
  const Encoder* loose = nullptr;
  for (const auto& e : encoders()) {
    if (lower(e.flName) != lower(flName)) continue;
    if (e.glType == glType) return &e;
    if (e.glType.empty() || (e.glType == "*hosted" && isHostedType(glType))) loose = &e;
  }
  return loose;
}

const Encoder* nativeEncoder(const PluginState& s) {
  const Encoder* best = nullptr;
  for (const auto& e : encoders()) {
    if (e.nativeRank < 0) continue;
    const bool typeOk = e.glType == s.type || (e.glType == "*hosted" && isHostedType(s.type));
    if (!typeOk || (e.accepts && !e.accepts(s))) continue;
    if (!best || e.nativeRank < best->nativeRank) best = &e;
  }
  return best;
}

const char* qualityName(Result::Quality q) {
  switch (q) {
    case Result::Verbatim: return "verbatim";
    case Result::Exact: return "exact";
    case Result::Approx: return "approx";
    case Result::Defaults: return "defaults";
    case Result::None: return "none";
  }
  return "?";
}

// ------------------------------------------------------------------ decoding

bool decodeState(const std::string& flName, const Bytes& state, PluginMapping& out, const EncodeContext& ec) {
  // A throwaway import context per thread (decoders read the song tempo, the
  // factory data folder and occasionally the sections).
  struct Scratch {
    File file;
    Song song;
    Report report;
  };
  thread_local std::unique_ptr<Scratch> sc;
  if (!sc) sc = std::make_unique<Scratch>();
  sc->song.tempo = ec.tempo;
  sc->song.ppq = ec.ppq;
  sc->report = Report{};
  Context c(sc->file, sc->song, sc->report);
  c.factoryData = ec.factoryData;
  c.projectDir = ec.projectDir;
  const Bytes name = utf16z(flName);
  std::vector<Event> evs;
  Event n;
  n.id = 201;
  n.data = name.data();
  n.size = static_cast<uint32_t>(name.size());
  evs.push_back(n);
  if (!state.empty()) {
    Event s;
    s.id = 213;
    s.data = state.data();
    s.size = static_cast<uint32_t>(state.size());
    evs.push_back(s);
  }
  out = PluginMapping{};
  out.flName = flName;
  const PluginDecoder* dec = findDecoder(flName);
  if (!dec) return false;
  bool ok = false;
  try {
    ok = (*dec)(c, evs, out);
  } catch (...) {
    ok = false;
  }
  if (!ok || out.glType.empty()) return false;
  if (out.state.type.empty()) out.state.type = out.glType;
  normalizePluginState(out.state);
  return true;
}

namespace {

// Params of `a` that differ from `b` (by index); size mismatch counts the tail.
std::vector<int> diffParams(const PluginState& a, const PluginState& b) {
  std::vector<int> d;
  const size_t n = std::max(a.params.size(), b.params.size());
  for (size_t i = 0; i < n; ++i) {
    const float x = i < a.params.size() ? a.params[i] : std::numeric_limits<float>::quiet_NaN();
    const float y = i < b.params.size() ? b.params[i] : std::numeric_limits<float>::quiet_NaN();
    if (!(x == y)) d.push_back(static_cast<int>(i));
  }
  return d;
}

struct Solver {
  const std::string& flName;
  const EncodeContext& ec;
  const PluginState& target;
  Bytes& b;
  PluginMapping m;
  size_t decodes = 0;

  // Decoded value of param k for the current bytes (NaN when the decoder rejects them).
  bool decode(PluginState& s) {
    ++decodes;
    if (!decodeState(flName, b, m, ec)) return false;
    s = m.state;
    return true;
  }
  double valueOf(int k) {
    PluginState s;
    if (!decode(s) || k < 0 || static_cast<size_t>(k) >= s.params.size()) return kNaN;
    return s.params[static_cast<size_t>(k)];
  }
  int mismatches() {
    PluginState s;
    if (!decode(s)) return 1 << 20;
    return static_cast<int>(diffParams(s, target).size());
  }

  // Sets field f so that its parameter decodes to the target value (or as
  // close as the stored range allows). Returns true when it now matches.
  bool solve(const Field& f) {
    if (f.gl < 0 || static_cast<size_t>(f.gl) >= target.params.size()) return false;
    const double want = target.params[static_cast<size_t>(f.gl)];
    const double r0 = readField(b, f.offset, f.type);
    if (!std::isfinite(r0)) return false;
    auto at = [&](double r) {
      writeField(b, f.offset, f.type, r);
      return valueOf(f.gl);
    };
    double cur = valueOf(f.gl);
    if (cur == static_cast<float>(want)) return true;
    double bestR = r0, bestErr = std::isfinite(cur) ? std::fabs(cur - want) : HUGE_VAL;
    auto consider = [&](double r, double v) {
      if (!std::isfinite(v)) return false;
      const double err = std::fabs(v - want);
      if (err < bestErr || (err == bestErr && std::fabs(r - r0) < std::fabs(bestR - r0))) bestErr = err, bestR = r;
      return static_cast<float>(v) == static_cast<float>(want);
    };
    bool hit = false;
    if (f.inverse) {
      double r = f.inverse(want);
      if (std::isfinite(r)) hit = consider(r, at(r));
    }
    double lo = f.lo, hi = f.hi;
    if (hi < lo) typeRange(f.type, lo, hi);
    const bool flt = isFloat(f.type);
    if (!hit && !flt && hi - lo <= 300) {  // small ranges (choices, switches, bytes): every value
      for (double r = lo; r <= hi && !hit; r += 1) hit = consider(r, at(r));
    } else if (!hit) {
      double vlo = at(lo), vhi = at(hi);
      consider(lo, vlo);
      consider(hi, vhi);
      if (std::isfinite(vlo) && std::isfinite(vhi) && vlo != vhi) {
        const bool inc = vhi > vlo;
        double a = lo, z = hi;  // invariant: value(a) on the low side of want, value(z) on the high side
        for (int it = 0; it < 200 && !hit; ++it) {
          double mid = flt ? 0.5 * (a + z) : std::floor(0.5 * (a + z));
          if ((flt && (mid == a || mid == z)) || (!flt && z - a <= 1)) break;
          double v = at(mid);
          if (!std::isfinite(v)) break;
          hit = consider(mid, v);
          if ((v < want) == inc) a = mid;
          else z = mid;
        }
        if (!hit && flt) {  // neighbours in the stored precision
          for (double r : {a, z}) {
            if (f.type == Type::F32) {
              float x = static_cast<float>(r);
              for (int k = 0; k < 4 && !hit; ++k) {
                hit = consider(x, at(x));
                x = std::nextafter(x, inc == (want > r) ? HUGE_VALF : -HUGE_VALF);
              }
            }
          }
        }
      }
      if (!hit) {  // not monotonic: coarse scan, then refine around the best
        const int steps = 256;
        for (int i = 0; i <= steps && !hit; ++i) {
          double r = lo + (hi - lo) * i / steps;
          if (!flt) r = std::round(r);
          hit = consider(r, at(r));
        }
        if (!hit && !flt) {
          const double span = std::ceil((hi - lo) / steps);
          const double c = bestR;
          for (double r = std::max(lo, c - span); r <= std::min(hi, c + span) && !hit; r += 1) hit = consider(r, at(r));
        }
      }
    }
    writeField(b, f.offset, f.type, bestR);
    return hit;
  }
};

}  // namespace

void writeInsCopy(Bytes& b, size_t envAt, size_t levAt, const std::string& ins) {
  InsSettings st;
  if (!ins.empty()) parseIns(ins, st);
  for (int t = 0; t < InsTargets; ++t) {
    const size_t r = envAt + 68 * static_cast<size_t>(t);
    if (r + 68 > b.size()) break;
    const InsTarget& x = st.t[t];
    const int32_t v[17] = {x.flags,     x.on ? -1 : 0, x.predelay,   x.attack,     x.hold,     x.decay,    x.sustain,  x.release, x.amount,
                           x.lfoDelay,  x.lfoAttack,   x.lfoAmount,  x.lfoSpeed,   x.lfoShape, x.tensionA, x.tensionD, x.tensionR};
    for (int k = 0; k < 17; ++k) putI32(b, r + 4 * static_cast<size_t>(k), v[k]);
  }
  if (levAt + 24 <= b.size()) putI32(b, levAt + 8, st.cutoff), putI32(b, levAt + 12, st.reso), putI32(b, levAt + 20, st.type);
}

namespace {
Result encodeWith(const Encoder* e, const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec);
}  // namespace

Result encode(const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec) {
  if (!originFlName.empty()) return encodeWith(findEncoder(originFlName, g.type), g, originFlName, originState, ec);
  // Native instance: every FL plugin that decodes to this type, preferred
  // first; the first that reproduces the state exactly wins, else the closest.
  std::vector<const Encoder*> cands;
  for (const auto& e : encoders()) {
    const bool typeOk = e.glType == g.type || (e.glType == "*hosted" && isHostedType(g.type));
    if (e.nativeRank >= 0 && typeOk && (!e.accepts || e.accepts(g))) cands.push_back(&e);
  }
  std::stable_sort(cands.begin(), cands.end(), [](const Encoder* a, const Encoder* b) { return a->nativeRank < b->nativeRank; });
  Result best;
  best.note = "no FL plugin for " + g.type;
  bool have = false;
  for (const Encoder* e : cands) {
    Result r = encodeWith(e, g, "", nullptr, ec);
    if (r.quality == Result::Exact) return r;
    auto score = [](const Result& x) {
      return x.quality == Result::Approx ? static_cast<int>(x.mismatched.size()) + (x.dataMismatch ? 1 : 0) : 1 << 20;
    };
    if (!have || score(r) < score(best)) best = r, have = true;
  }
  return best;
}

namespace {

Result encodeWith(const Encoder* e, const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec) {
  Result r;
  r.flName = e ? e->flName : originFlName;
  // Template: the origin state, else the encoder's defaults.
  Bytes t;
  bool fromOrigin = false;
  if (originState && !originFlName.empty()) t = *originState, fromOrigin = true;
  else if (e && e->defaults) t = e->defaults();
  if (r.flName.empty()) {
    r.quality = Result::None;
    r.note = "no FL plugin for " + g.type;
    return r;
  }
  PluginMapping m;
  const bool decOk = decodeState(r.flName, t, m, ec);
  // Hosted plugins: the parameters mirror the state (data); compare type and data only.
  const bool hosted = isHostedType(g.type);
  if (decOk && m.state.type == g.type && (hosted || m.state.params == g.params) && m.state.data == g.data && m.state.ins == g.ins) {
    r.state = t;
    r.hasState = !t.empty();
    r.quality = fromOrigin ? Result::Verbatim : (t.empty() ? Result::Defaults : Result::Exact);
    if (r.quality == Result::Defaults) r.note = "FL defaults (the defaults match)";
    if (!r.hasState && r.quality == Result::Exact) r.quality = Result::Defaults;
    return r;
  }
  if (!e || t.empty()) {
    // No encoder (or no way to build a state): the origin as it was, or FL's defaults.
    r.state = t;
    r.hasState = !t.empty();
    r.quality = t.empty() ? Result::Defaults : Result::Approx;
    if (decOk && m.state.type == g.type) r.mismatched = diffParams(m.state, g), r.dataMismatch = m.state.data != g.data;
    r.note = !e ? "no encoder for " + r.flName : "no default state for " + r.flName;
    return r;
  }
  Bytes b = t;
  if (e->write) e->write(g, b, ec);
  Solver s{r.flName, ec, g, b, {}, 0};
  std::vector<Field> fields = e->fields ? e->fields(b) : std::vector<Field>{};
  int before = s.mismatches();
  for (int pass = 0; pass < 3 && before > 0; ++pass) {
    for (const Field& f : fields) {
      PluginState cur;
      if (!s.decode(cur) || f.gl < 0 || static_cast<size_t>(f.gl) >= cur.params.size() ||
          static_cast<size_t>(f.gl) >= g.params.size() || cur.params[static_cast<size_t>(f.gl)] == g.params[static_cast<size_t>(f.gl)])
        continue;
      const Bytes keep = b;
      const int was = s.mismatches();
      s.solve(f);
      if (s.mismatches() > was) b = keep;  // a field with side effects that made things worse
    }
    if (e->fields) fields = e->fields(b);  // sizes / versions may have changed
    const int now = s.mismatches();
    if (now >= before) break;
    before = now;
  }
  if (e->finish) e->finish(b);
  PluginState fin;
  r.state = b;
  r.hasState = true;
  if (s.decode(fin)) {
    r.mismatched = fin.type != g.type ? std::vector<int>{-1} : hosted ? std::vector<int>{} : diffParams(fin, g);
    r.dataMismatch = fin.data != g.data || fin.ins != g.ins;  // J-33: channel envelopes / filter (PluginState::ins)
  } else {
    r.mismatched = {-1};
    r.note = "the encoded state does not decode";
  }
  r.quality = r.mismatched.empty() && !r.dataMismatch ? Result::Exact : Result::Approx;
  return r;
}

}  // namespace

// ------------------------------------------------------------------ corpus check

namespace {

struct Instance {
  std::string flName;
  Bytes state;
  std::string file;
};

void collect(const std::string& path, std::map<std::string, std::vector<Instance>>& out, const CheckOptions& o) {
  File f;
  std::string err;
  if (!readFile(path, f, &err)) return;
  Sections sec = split(f);
  auto take = [&](const std::vector<Event>& evs) {
    const Event* n = find(evs, 201);
    if (!n) return;
    std::string name = n->text();
    if (name.empty()) return;
    if (!o.plugins.empty() && std::find(o.plugins.begin(), o.plugins.end(), name) == o.plugins.end()) return;
    auto& v = out[name];
    if (v.size() >= o.maxPerPlugin) return;
    Instance in;
    in.flName = name;
    if (const Event* s = find(evs, 213)) in.state.assign(s->data, s->data + s->size);
    in.file = path;
    v.push_back(std::move(in));
  };
  for (const auto& [k, evs] : sec.channels) take(evs);
  for (const auto& ins : sec.inserts)
    for (const auto& sl : ins.slots) take(sl.events);
  if (sec.channels.empty() && sec.inserts.empty()) take(f.events);
}

std::string paramName(const std::string& type, int k) {
  const PluginInfo* info = findPlugin(type);
  if (k < 0) return "(decode)";
  if (!info || static_cast<size_t>(k) >= info->params.size()) return "#" + std::to_string(k);
  return info->params[static_cast<size_t>(k)].name + "#" + std::to_string(k);
}

}  // namespace

std::string encodeCheck(const std::vector<std::string>& files, const CheckOptions& o, const EncodeContext& ec) {
  std::map<std::string, std::vector<Instance>> inst;
  for (const auto& f : files) collect(f, inst, o);
  std::ostringstream os;
  os << "plugin\tencoder\tinstances\tdecoded\tnative exact\tnative approx\tdefaults-only\tpatch exact\tpatch approx\tworst params\n";
  int totalInst = 0, totalNativeExact = 0, totalDecoded = 0;
  std::ostringstream detail;
  for (const auto& [name, v] : inst) {
    int decoded = 0, nExact = 0, nApprox = 0, nDefaults = 0, pExact = 0, pApprox = 0;
    std::map<std::string, int> worst;
    std::string glType;
    const Encoder* enc = nullptr;
    for (const Instance& in : v) {
      PluginMapping m;
      if (!decodeState(name, in.state, m, ec)) continue;
      ++decoded;
      glType = m.state.type;
      enc = findEncoder(name, m.state.type);
      // Native path: no origin, the encoder's defaults as template.
      {
        Result r;
        if (enc) {
          Bytes t = enc->defaults ? enc->defaults() : Bytes{};
          // encode() picks the native encoder by type; force this FL plugin:
          // pass the defaults as an "origin" so the same FL plugin is used.
          r = encode(m.state, name, t.empty() ? nullptr : &t, ec);
          if (t.empty()) r.quality = Result::Defaults;
        }
        if (r.quality == Result::Exact || r.quality == Result::Verbatim) ++nExact;
        else if (r.quality == Result::Approx) ++nApprox;
        else ++nDefaults;
        for (int k : r.mismatched) worst[paramName(glType, k)]++;
        if (r.dataMismatch) worst["(data)"]++;
        if (o.verbose && r.quality != Result::Exact && r.quality != Result::Verbatim) {
          detail << name << "\t" << in.file << "\tnative " << qualityName(r.quality) << ":";
          for (int k : r.mismatched) detail << " " << paramName(glType, k);
          if (r.dataMismatch) detail << " (data)";
          detail << "\n";
        }
      }
      // Patch path: the origin with one parameter moved to another valid value.
      if (enc && !m.state.params.empty()) {
        PluginState g = m.state;
        const PluginInfo* info = findPlugin(g.type);
        int moved = -1;
        if (info)
          for (size_t k = 0; k < g.params.size() && k < info->params.size() && moved < 0; ++k) {
            const ParamDesc& d = info->params[k];
            // a parameter some field of this state stores
            bool stored = false;
            for (const Field& f : enc->fields ? enc->fields(in.state) : std::vector<Field>{}) stored |= f.gl == static_cast<int>(k);
            if (!stored) continue;
            double nv = d.clamp(d.fromNormalized(std::fmod(d.toNormalized(g.params[k]) + 0.37, 1.0)));
            if (static_cast<float>(nv) != g.params[k]) g.params[k] = static_cast<float>(nv), moved = static_cast<int>(k);
          }
        if (moved >= 0) {
          Result r = encode(g, name, &in.state, ec);
          // the moved value may not be representable; count exact when everything else stayed
          std::vector<int> other;
          for (int k : r.mismatched)
            if (k != moved) other.push_back(k);
          if (other.empty()) ++pExact;
          else ++pApprox;
        }
      }
    }
    totalInst += static_cast<int>(v.size());
    totalDecoded += decoded;
    totalNativeExact += nExact;
    std::vector<std::pair<int, std::string>> ws;
    for (const auto& [k, c] : worst) ws.push_back({c, k});
    std::sort(ws.rbegin(), ws.rend());
    std::string wtxt;
    for (size_t i = 0; i < ws.size() && i < 6; ++i) wtxt += (i ? ", " : "") + ws[i].second + " x" + std::to_string(ws[i].first);
    os << name << "\t" << (enc ? glType : (glType.empty() ? "-" : glType + " (no encoder)")) << "\t" << v.size() << "\t" << decoded << "\t" << nExact
       << "\t" << nApprox << "\t" << nDefaults << "\t" << pExact << "\t" << pApprox << "\t" << wtxt << "\n";
  }
  os << "TOTAL\t\t" << totalInst << "\t" << totalDecoded << "\t" << totalNativeExact << "\n";
  if (o.verbose) os << "\n" << detail.str();
  return os.str();
}

}  // namespace gl::flp::enc
