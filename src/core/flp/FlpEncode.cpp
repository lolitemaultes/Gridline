// Gridline — FLP export: plugin encoder engine (see FlpEncode.h).
#include "FlpEncode.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include "../../engine/ChannelIns.h"
#include "../../packs/synths/matrixfm.h"
#include "../../packs/synths/mfmref.h"

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

bool decodeState(const std::string& flName, const Bytes& state, PluginMapping& out, const EncodeContext& ec, bool paramsOnly) {
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
  c.paramsOnly = paramsOnly;
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
  const Bytes sample = ec.channelSample.empty() ? Bytes{} : utf16z(ec.channelSample);
  if (!sample.empty()) {
    Event e;
    e.id = 196;
    e.data = sample.data();
    e.size = static_cast<uint32_t>(sample.size());
    evs.push_back(e);
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
  // J-50: Matrix FM's engine choice is not part of FL's state (imports select the reference model).
  const bool mfm = a.type == "gl.matrixfm";
  for (size_t i = 0; i < n; ++i) {
    if (mfm && i == static_cast<size_t>(gl::packs::synths::mfm::MModel)) continue;
    const float x = i < a.params.size() ? a.params[i] : std::numeric_limits<float>::quiet_NaN();
    const float y = i < b.params.size() ? b.params[i] : std::numeric_limits<float>::quiet_NaN();
    if (!(x == y)) d.push_back(static_cast<int>(i));
  }
  return d;
}

// Data equality; Matrix FM's exact section (J-50) is derived from the FL state itself.
bool sameData(const PluginState& a, const PluginState& b) {
  if (a.type == "gl.matrixfm") return gl::packs::synths::mfmref::withoutExact(a.data) == gl::packs::synths::mfmref::withoutExact(b.data);
  return a.data == b.data;
}

std::atomic<int> gVerifyEvery{-1};  // -1: not read from the environment yet
std::atomic<uint64_t> gVerifyCount{0};
std::atomic<int>& lawMismatchCount() {
  static std::atomic<int> n{0};
  return n;
}

// J-51: probes of one solve() differ only in the solved field, so independent
// candidates (the scans) are decoded on a small pool of threads; the results
// are then taken in the order the sequential search takes them (the first hit
// stops it, ties go to the earlier candidate), so the outcome is the same.
// Decoders are pure functions of the state (thread-local import context).
class ProbePool {
 public:
  static ProbePool& get() {
    static ProbePool* p = new ProbePool();  // never destroyed: detached workers
    return *p;
  }
  int threads() const { return static_cast<int>(workers_) + 1; }
  // Runs fn(i) for every i in [0, n) on the workers and the calling thread.
  // Sequential when the pool is busy (another export) or called from a worker.
  void run(size_t n, const std::function<void(size_t)>& fn) {
    std::unique_lock<std::mutex> busy(runMu_, std::try_to_lock);
    if (workers_ == 0 || n < 2 || tWorker || !busy.owns_lock()) {
      for (size_t i = 0; i < n; ++i) fn(i);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      fn_ = &fn;
      n_ = n;
      next_ = 0;
      active_ = workers_;
      ++job_;
    }
    cv_.notify_all();
    work(fn, n);
    std::unique_lock<std::mutex> lk(mu_);
    done_.wait(lk, [&] { return active_ == 0; });
    fn_ = nullptr;
  }

 private:
  ProbePool() {
    const unsigned hw = std::thread::hardware_concurrency();
    workers_ = hw > 1 ? std::min(hw, 8u) - 1 : 0;
    if (const char* e = std::getenv("GRIDLINE_FLP_SOLVER_THREADS")) workers_ = static_cast<unsigned>(std::clamp(std::atoi(e), 1, 64)) - 1;
    for (unsigned i = 0; i < workers_; ++i) std::thread([this] { loop(); }).detach();
  }
  void work(const std::function<void(size_t)>& fn, size_t n) {
    for (size_t i; (i = next_.fetch_add(1)) < n;) fn(i);
  }
  void loop() {
    tWorker = true;
    uint64_t seen = 0;
    for (;;) {
      const std::function<void(size_t)>* fn;
      size_t n;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return job_ != seen; });
        seen = job_;
        fn = fn_, n = n_;
      }
      work(*fn, n);
      std::lock_guard<std::mutex> lk(mu_);
      if (--active_ == 0) done_.notify_one();
    }
  }
  static thread_local bool tWorker;
  unsigned workers_ = 0;
  std::mutex runMu_, mu_;
  std::condition_variable cv_, done_;
  const std::function<void(size_t)>* fn_ = nullptr;
  size_t n_ = 0;
  std::atomic<size_t> next_{0};
  unsigned active_ = 0;
  uint64_t job_ = 0;
};
thread_local bool ProbePool::tWorker = false;

}  // namespace

void setVerifyLaws(int every) { gVerifyEvery = std::max(0, every); }
bool verifyLaws() {
  int every = gVerifyEvery.load();
  if (every < 0) {
    const char* e = std::getenv("GRIDLINE_FLP_VERIFY_LAWS");
    every = e ? std::max(0, std::atoi(e)) : 0;
    gVerifyEvery = every;
  }
  return every == 1 || (every > 1 && gVerifyCount.fetch_add(1) % static_cast<uint64_t>(every) == 0);
}
int lawMismatches() { return lawMismatchCount().load(); }
void noteLawMismatch() { ++lawMismatchCount(); }
void parallelFor(size_t n, const std::function<void(size_t)>& fn) { ProbePool::get().run(n, fn); }

namespace {

struct Solver {
  const std::string& flName;
  const EncodeContext& ec;
  const PluginState& target;
  Bytes& b;
  size_t decodes = 0;
  // J-51: decoded states by their bytes (decoding is deterministic): the
  // engine decodes the same bytes several times per field (the pass loop's
  // check, the mismatch counts before / after, the solver's start value).
  struct Entry {
    Bytes bytes;
    bool ok = false;
    PluginState state;
  };
  Entry recent[4];
  size_t nextSlot = 0;

  Solver(const std::string& n, const EncodeContext& c, const PluginState& t, Bytes& bytes) : flName(n), ec(c), target(t), b(bytes) {}

  const Entry& lookup() {
    for (const Entry& e : recent)
      if (!e.bytes.empty() && e.bytes == b) return e;
    ++decodes;
    Entry& e = recent[nextSlot];
    nextSlot = (nextSlot + 1) % 4;
    PluginMapping m;
    e.bytes = b;
    e.ok = decodeState(flName, b, m, ec, true);
    e.state = e.ok ? std::move(m.state) : PluginState{};
    return e;
  }

  // Decoded state of the current bytes (false when the decoder rejects them).
  bool decode(PluginState& s) {
    const Entry& e = lookup();
    if (!e.ok) return false;
    s = e.state;
    return true;
  }
  static double paramOf(const PluginState& s, bool ok, int k) {
    if (!ok || k < 0 || static_cast<size_t>(k) >= s.params.size()) return kNaN;
    return s.params[static_cast<size_t>(k)];
  }
  double valueOf(int k) {
    const Entry& e = lookup();
    return paramOf(e.state, e.ok, k);
  }
  int mismatches() {
    const Entry& e = lookup();
    if (!e.ok) return 1 << 20;
    return static_cast<int>(diffParams(e.state, target).size());
  }

  // Values of param f.gl with field f at each of `rs` (the other bytes as they are).
  std::vector<double> values(const Field& f, const std::vector<double>& rs) {
    std::vector<double> out(rs.size(), kNaN);
    decodes += rs.size();
    ProbePool::get().run(rs.size(), [&](size_t i) {
      thread_local Bytes local;
      local = b;
      writeField(local, f.offset, f.type, rs[i]);
      PluginMapping m;
      const bool ok = decodeState(flName, local, m, ec, true);
      out[i] = paramOf(m.state, ok, f.gl);
    });
    return out;
  }

  // The stored value field f holds after writeField(r) (r finite).
  static double storedAs(const Field& f, double r) {
    if (f.type == Type::F32) return static_cast<float>(r);
    if (f.type == Type::F64) return r;
    double lo, hi;
    typeRange(f.type, lo, hi);
    return std::clamp(std::round(r), lo, hi);
  }
  // Param f.gl as the decoder leaves it for the law's value x: stored as a
  // float (setp), then clamped to the parameter's range (normalizePluginState).
  double lawValue(const Field& f, double r) const {
    const double x = static_cast<float>(f.forward(storedAs(f, r), b));
    const PluginInfo* info = findPlugin(target.type);
    if (!info) return x;
    if (static_cast<size_t>(f.gl) >= info->params.size()) return kNaN;
    return static_cast<float>(info->params[static_cast<size_t>(f.gl)].clamp(x));
  }

  // Sets field f so that its parameter decodes to the target value (or as
  // close as the stored range allows). Returns true when it now matches.
  bool solve(const Field& f) {
    if (f.gl < 0 || static_cast<size_t>(f.gl) >= target.params.size()) return false;
    const double r0 = readField(b, f.offset, f.type);
    if (!std::isfinite(r0)) return false;
    const double cur = valueOf(f.gl);
    if (cur == static_cast<float>(target.params[static_cast<size_t>(f.gl)])) return true;
    // J-51: a field with the decoder's law searches through the law; the
    // start and the result are checked through the decoder (a law that
    // disagrees is ignored: the search runs again through the decoder).
    if (f.forward && !std::isfinite(cur) == !std::isfinite(lawValue(f, r0)) && (!std::isfinite(cur) || cur == lawValue(f, r0))) {
      double bestR;
      const bool hit = search(f, r0, cur, true, bestR);
      writeField(b, f.offset, f.type, bestR);
      const double got = valueOf(f.gl), law = lawValue(f, bestR);
      if (got == law || (std::isnan(got) && std::isnan(law))) return hit;
      noteLawMismatch(f, bestR, got, law);
      writeField(b, f.offset, f.type, r0);
    }
    double bestR;
    const bool hit = search(f, r0, cur, false, bestR);
    writeField(b, f.offset, f.type, bestR);
    return hit;
  }

  void noteLawMismatch(const Field& f, double r, double decoded, double law) const {
    const int n = ++lawMismatchCount();
    if (n <= 20) std::fprintf(stderr, "flp encoder: %s param %d law differs from the decoder at stored %g (%.9g vs %.9g)\n", flName.c_str(), f.gl, r, decoded, law);
  }

  // The search (bestR: the stored value to keep). With `useLaw` every
  // candidate's value comes from the field's law, else from the decoder; the
  // bytes are left with field f at an arbitrary candidate.
  bool search(const Field& f, double r0, double cur, bool useLaw, double& bestR) {
    const double want = target.params[static_cast<size_t>(f.gl)];
    auto checked = [&](double r, double law) {  // verify mode: the law against the decoder
      writeField(b, f.offset, f.type, r);
      const double got = valueOf(f.gl);
      if (!(got == law || (std::isnan(got) && std::isnan(law)))) noteLawMismatch(f, r, got, law);
      return law;
    };
    auto at = [&](double r) {
      if (useLaw) return verifyLaws() ? checked(r, lawValue(f, r)) : lawValue(f, r);
      writeField(b, f.offset, f.type, r);
      return valueOf(f.gl);
    };
    bestR = r0;
    double bestErr = std::isfinite(cur) ? std::fabs(cur - want) : HUGE_VAL;
    auto consider = [&](double r, double v) {
      if (!std::isfinite(v)) return false;
      const double err = std::fabs(v - want);
      if (err < bestErr || (err == bestErr && std::fabs(r - r0) < std::fabs(bestR - r0))) bestErr = err, bestR = r;
      return static_cast<float>(v) == static_cast<float>(want);
    };
    // Candidates in order until the first hit (the sequential loops `for r
    // in rs: if consider(r, at(r)) break`), decoded in parallel chunks.
    const size_t chunk = static_cast<size_t>(std::max(16, 4 * ProbePool::get().threads()));
    auto scan = [&](const std::vector<double>& rs) {
      if (useLaw) {
        for (double r : rs)
          if (consider(r, at(r))) return true;
        return false;
      }
      for (size_t from = 0; from < rs.size(); from += chunk) {
        const std::vector<double> part(rs.begin() + static_cast<std::ptrdiff_t>(from), rs.begin() + static_cast<std::ptrdiff_t>(std::min(rs.size(), from + chunk)));
        const std::vector<double> v = values(f, part);
        for (size_t i = 0; i < part.size(); ++i)
          if (consider(part[i], v[i])) return true;
      }
      return false;
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
      std::vector<double> rs;
      for (double r = lo; r <= hi; r += 1) rs.push_back(r);
      hit = scan(rs);
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
        std::vector<double> rs;
        for (int i = 0; i <= steps; ++i) {
          double r = lo + (hi - lo) * i / steps;
          if (!flt) r = std::round(r);
          rs.push_back(r);
        }
        hit = scan(rs);
        if (!hit && !flt) {
          const double span = std::ceil((hi - lo) / steps);
          const double c = bestR;
          rs.clear();
          for (double r = std::max(lo, c - span); r <= std::min(hi, c + span); r += 1) rs.push_back(r);
          hit = scan(rs);
        }
      }
    }
    return hit;
  }
};

}  // namespace

// J-72: a plugin state's channel-envelope copy holds the envelopes and the
// filter only; the velocity / key tracking (event 228) and the timing switch
// live on the channel, so they do not count as a state mismatch.
static bool sameInsForState(const std::string& state, const std::string& want) {
  if (state == want) return true;
  InsSettings a, b;
  if (!parseIns(want, b)) return false;
  parseIns(state, a);
  for (int k = 0; k < 2; ++k)
    for (int j = 0; j < 4; ++j) a.track[k][j] = b.track[k][j];
  a.timing = b.timing;
  return a == b;
}

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

namespace {
Result encodeUncached(const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec);

// J-51: solved states, by everything an encode depends on (the Gridline
// state, the origin, the context). Encoding is deterministic, so an export
// after a small edit (or a second export of the same song) only solves the
// plugins that changed. Hosted plugins (their descriptions can change at
// run time) are not kept. Bounded: the oldest entries go first.
class EncodeCache {
 public:
  static EncodeCache& get() {
    static EncodeCache* c = new EncodeCache();  // never destroyed (exports may run at exit)
    return *c;
  }
  static std::string key(const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec) {
    std::string k;
    auto put = [&k](const void* p, size_t n) {
      const uint64_t len = n;
      k.append(reinterpret_cast<const char*>(&len), sizeof len);
      k.append(static_cast<const char*>(p), n);
    };
    auto str = [&](const std::string& x) { put(x.data(), x.size()); };
    str(g.type), str(g.data), str(g.ins);
    put(g.params.data(), g.params.size() * sizeof(float));
    str(originFlName);
    const char has = originState ? 1 : 0;
    put(&has, 1);
    if (originState) put(originState->data(), originState->size());
    put(&ec.tempo, sizeof ec.tempo), put(&ec.ppq, sizeof ec.ppq);
    str(ec.factoryData), str(ec.projectDir), str(ec.wrapAs), str(ec.channelSample);
    return k;
  }
  bool find(const std::string& k, Result& out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(k);
    if (it == map_.end()) return false;
    out = it->second;
    return true;
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear(), order_.clear(), size_ = 0;
  }
  void put(std::string k, const Result& r) {
    std::lock_guard<std::mutex> lk(mu_);
    if (map_.count(k)) return;
    size_ += k.size() + r.state.size();
    order_.push_back(k);
    map_.emplace(std::move(k), r);
    while (!order_.empty() && (order_.size() > kEntries || size_ > kBytes)) {
      auto it = map_.find(order_.front());
      if (it != map_.end()) size_ -= it->first.size() + it->second.state.size(), map_.erase(it);
      order_.pop_front();
    }
  }

 private:
  static constexpr size_t kEntries = 1024, kBytes = 64u << 20;
  std::mutex mu_;
  std::map<std::string, Result> map_;
  std::deque<std::string> order_;
  size_t size_ = 0;
};
}  // namespace

void clearEncodeCache() { EncodeCache::get().clear(); }

Result encode(const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec) {
  if (isHostedType(g.type)) return encodeUncached(g, originFlName, originState, ec);
  std::string k = EncodeCache::key(g, originFlName, originState, ec);
  Result r;
  if (EncodeCache::get().find(k, r)) return r;
  r = encodeUncached(g, originFlName, originState, ec);
  EncodeCache::get().put(std::move(k), r);
  return r;
}

namespace {
Result encodeUncached(const PluginState& g, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec) {
  if (!originFlName.empty()) return encodeWith(findEncoder(originFlName, g.type), g, originFlName, originState, ec);
  // Native instance: every FL plugin that decodes to this type, preferred
  // first; the first that reproduces the state exactly wins, else the closest.
  // J-39: then the FL plugins that are no native home of the type (other FL
  // plugins the importer maps to it) — used only when one reproduces the
  // state exactly, which is what an instance imported from it does.
  std::vector<const Encoder*> cands, fallback;
  for (const auto& e : encoders()) {
    const bool typeOk = e.glType == g.type || (e.glType == "*hosted" && isHostedType(g.type));
    // (glType "": FL plugins in FL's plugin wrapper, as old projects load them — any type)
    if (!(typeOk || (e.glType.empty() && e.nativeRank < 0)) || (e.accepts && !e.accepts(g))) continue;
    (e.nativeRank >= 0 && typeOk ? cands : fallback).push_back(&e);
  }
  std::stable_sort(cands.begin(), cands.end(), [](const Encoder* a, const Encoder* b) { return a->nativeRank < b->nativeRank; });
  const size_t ranked = cands.size();
  cands.insert(cands.end(), fallback.begin(), fallback.end());
  Result best;
  best.note = "no FL plugin for " + g.type;
  bool have = false;
  for (size_t i = 0; i < cands.size(); ++i) {
    const Encoder* e = cands[i];
    if (i >= ranked && !e->defaults && !e->defaultsIn) continue;  // a fallback needs a state of its own
    if (i >= ranked && e->flName == "Fruity Wrapper" && e->glType.empty() && ec.wrapAs.empty()) {
      // FL's plugin wrapper: each FL plugin that can carry the state in turn
      // (an old project's wrapped plugin stores its values at its own resolution).
      for (const std::string& n : wrapCandidates(g)) {
        EncodeContext c2 = ec;
        c2.wrapAs = n;
        Result r = encodeWith(e, g, "", nullptr, c2);
        if (r.quality == Result::Exact) return r;
      }
      continue;
    }
    Result r = encodeWith(e, g, "", nullptr, ec);
    // Exact, or no state at all because FL's defaults decode to the instance.
    const bool defaultsMatch = r.quality == Result::Defaults && r.mismatched.empty() && !r.dataMismatch && r.note.rfind("FL defaults (the defaults match)", 0) == 0;
    if (r.quality == Result::Exact || defaultsMatch) return r;
    if (i >= ranked) continue;  // fallbacks only when exact
    auto score = [](const Result& x) {
      return x.quality == Result::Approx ? static_cast<int>(x.mismatched.size()) + (x.dataMismatch ? 1 : 0) : 1 << 20;
    };
    if (!have || score(r) < score(best)) best = r, have = true;
  }
  return best;
}
}  // namespace

namespace {

Result encodeWith(const Encoder* e, const PluginState& g0, const std::string& originFlName, const Bytes* originState, const EncodeContext& ec0) {
  // J-53: settings the FL plugin does not store are left out.
  PluginState stripped;
  if (e && e->strip) stripped = g0, e->strip(stripped);
  const PluginState& g = e && e->strip ? stripped : g0;
  // J-44: a plugin that plays the channel sample reads it from the channel's
  // event 196, which the channel writer adds for it (FlpExportChannels.cpp):
  // decode the state with that event, as FL and the importer see it.
  EncodeContext ec = ec0;
  if (ec.channelSample.empty() && !g.data.empty() && g.data.find('\n') == std::string::npos)
    if (const PluginInfo* info = findPlugin(g.type); info && info->usesSample && info->kind == PluginKind::Generator)
      ec.channelSample = flPath(g.data, ec);
  Result r;
  r.flName = e ? e->flName : originFlName;
  // Template: the origin state, else the encoder's defaults.
  Bytes t;
  bool fromOrigin = false;
  if (originState && !originFlName.empty()) t = *originState, fromOrigin = true;
  else if (e && e->defaults) t = e->defaults();
  else if (e && e->defaultsIn) t = e->defaultsIn(ec);
  if (r.flName.empty()) {
    r.quality = Result::None;
    r.note = "no FL plugin for " + g.type;
    return r;
  }
  PluginMapping m;
  const bool decOk = decodeState(r.flName, t, m, ec);
  // Hosted plugins: the parameters mirror the state (data); compare type and data only.
  const bool hosted = isHostedType(g.type);
  if (decOk && m.state.type == g.type && (hosted || diffParams(m.state, g).empty()) && sameData(m.state, g) && sameInsForState(m.state.ins, g.ins)) {
    r.state = t;
    r.hasState = !t.empty();
    const bool stateless = e && e->stateless;
    r.quality = fromOrigin ? Result::Verbatim : (t.empty() && !stateless ? Result::Defaults : Result::Exact);
    if (r.quality == Result::Defaults) r.note = "FL defaults (the defaults match)";
    if (!r.hasState && r.quality == Result::Exact && !stateless) r.quality = Result::Defaults;
    return r;
  }
  if (!e || t.empty()) {
    // No encoder (or no way to build a state): the origin as it was, or FL's defaults.
    r.state = t;
    r.hasState = !t.empty();
    r.quality = t.empty() ? Result::Defaults : Result::Approx;
    if (decOk && m.state.type == g.type) r.mismatched = diffParams(m.state, g), r.dataMismatch = !sameData(m.state, g);
    r.note = !e ? "no encoder for " + r.flName : "no default state for " + r.flName;
    return r;
  }
  Bytes b = t;
  if (e->write) e->write(g, b, ec);
  Solver s(r.flName, ec, g, b);
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
  r.state = b;
  r.hasState = !b.empty();  // (an encoder may decide FL's defaults are the state: none)
  PluginMapping fm;  // decoded in full (the solver's decodes read the parameters only)
  if (decodeState(r.flName, b, fm, ec)) {
    const PluginState& fin = fm.state;
    r.mismatched = fin.type != g.type ? std::vector<int>{-1} : hosted ? std::vector<int>{} : diffParams(fin, g);
    // J-33: channel envelopes / filter (PluginState::ins). J-39: a state that
    // carries none (the channel's own events do: DX10, …) is not a mismatch.
    r.dataMismatch = !sameData(fin, g) || (!sameInsForState(fin.ins, g.ins) && !fin.ins.empty());
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
          Bytes t = enc->defaults ? enc->defaults() : enc->defaultsIn ? enc->defaultsIn(ec) : Bytes{};
          // encode() picks the native encoder by type; force this FL plugin:
          // pass the defaults as an "origin" so the same FL plugin is used.
          r = encode(m.state, name, t.empty() ? nullptr : &t, ec);
          if (t.empty() && !enc->stateless) r.quality = Result::Defaults;
        }
        if (r.quality == Result::Exact || r.quality == Result::Verbatim) ++nExact;
        else if (r.quality == Result::Approx) ++nApprox;
        else ++nDefaults;
        for (int k : r.mismatched) worst[paramName(glType, k)]++;
        if (r.dataMismatch) worst["(data)"]++;
        if (o.verbose && r.quality != Result::Exact && r.quality != Result::Verbatim) {
          detail << name << "\t" << in.file << "\tnative " << qualityName(r.quality) << ":";
          PluginMapping got;
          const bool gotOk = r.hasState && decodeState(r.flName, r.state, got, ec);
          for (int k : r.mismatched) {
            detail << " " << paramName(glType, k);
            if (gotOk && k >= 0 && static_cast<size_t>(k) < m.state.params.size() && static_cast<size_t>(k) < got.state.params.size())
              detail << "(" << m.state.params[static_cast<size_t>(k)] << "→" << got.state.params[static_cast<size_t>(k)] << ")";
          }
          if (r.dataMismatch) {
            detail << " (data)";
            // First differing line of the data texts (want / decoded again).
            PluginMapping back;
            if (decodeState(r.flName, r.state, back, ec)) {
              std::istringstream a(m.state.data), b(back.state.data);
              std::string la, lb;
              while (true) {
                const bool ga = static_cast<bool>(std::getline(a, la)), gb = static_cast<bool>(std::getline(b, lb));
                if (!ga && !gb) break;
                if (!ga) la = "<end>";
                if (!gb) lb = "<end>";
                if (la != lb) {
                  detail << "\n    want " << la.substr(0, 200) << "\n    got  " << lb.substr(0, 200);
                  break;
                }
              }
            }
          }
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
