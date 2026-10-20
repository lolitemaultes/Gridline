// Gridline — FLP import, third wave: the VFX note effects.
//   VFX Key Mapper         → gl.keymap   (pitch-class map, transpose)
//   VFX Keyboard Splitter  → gl.split    (key / velocity zones, per-zone transpose)
//   VFX Script             → gl.notescript (script kept as comments; the note
//                            formula subset translated to Gridline formulas)
//   VFX Envelope           → gl.envctl   (articulator envelopes, approximate)
//
// State layouts (observed in the shipped presets; notes in the analysis folder):
//   Key Mapper   i32 version 2, enable, base key (0..11), offset (-12..12),
//                multi mode, pitch slide (0..192); then 12 records, one per
//                key of the octave counted from the base key: u32 count,
//                count × i16 target (semitones from the octave's first key).
//   Splitter     4- or 5-byte header, then 16 zones: key envelope, velocity
//                envelope, i32 transpose, u8 absolute velocity, u8, u8.
//                Envelope: u32 3, u32, u32 N, N × {f64 Δx, f64 level, f32
//                tension, u32 flags}, 20 bytes tail. Key envelopes span keys
//                0..132 over x 0..1, velocity envelopes velocity 0..1.
//   Script       u32 length, UTF-8 text (INI): [Script] Source=<code with
//                "<linebreak>" separators>, [Parameters] name=value (knobs as
//                value × 2^18), optional [Surface].
//   Envelope     header, 80 parameter records (16 articulators × 5 targets),
//                then 80 envelope lists {i32 3, f32 ×3, i32 1, i32 3, i32 N,
//                N × {f64 Δbeats, f64 level, f32 tension, u32 flags}}.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include "../../packs/modulation/modulation.h"
#include "../../packs/modulation/modulation_dsp.h"
#include "FlpDec3.h"

namespace gl::flp::dec3 {

namespace {

namespace mod = gl::packs::modulation;
using gen2::begin;
using gen2::set;

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ------------------------------------------------------------- Key Mapper

bool decodeKeyMapper(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 24 + 12 * 4 || b.i32(0) < 1 || b.i32(0) > 8) return false;
  begin(m, "gl.keymap");
  const bool enabled = b.i32(4) != 0;
  const int baseKey = ((b.i32(8) % 12) + 12) % 12;
  const int offset = b.i32(12), mode = b.i32(16), slide = b.i32(20);
  bool exact = enabled && slide == 0;
  bool anyBoth = false;
  int offs[12] = {};
  bool both[12] = {};
  size_t p = 24;
  for (int r = 0; r < 12; ++r) {
    if (!b.has(p, 4)) return false;
    const uint32_t n = b.u32(p);
    p += 4;
    if (n > 64 || !b.has(p, 2 * n)) return false;
    std::vector<int> t;
    for (uint32_t k = 0; k < n; ++k) t.push_back(b.i16(p + 2 * k));
    p += 2 * n;
    const int pc = (r + baseKey) % 12;
    if (t.empty()) {
      offs[pc] = -13;  // muted key
      continue;
    }
    // One target: a plain map. Two with the original key: map + original.
    // More (chords): the first target other than the key itself, approximate.
    std::vector<int> other;
    bool self = false;
    for (int v : t) {
      if (v == r) self = true;
      else other.push_back(v);
    }
    if (t.size() == 1) {
      offs[pc] = t[0] - r;
    } else if (other.empty()) {
      offs[pc] = 0;
    } else {
      offs[pc] = other[0] - r;
      both[pc] = self;
      if (t.size() > 2 || !self || mode >= 2) exact = false;
      if (mode == 1) exact = false;  // gain-compensated: level change not modelled
    }
    if (offs[pc] < -12 || offs[pc] > 12) exact = false;
  }
  for (int pc = 0; pc < 12; ++pc) anyBoth = anyBoth || both[pc];
  // Gridline has one multi mode for all keys: keys mapped to themselves plus
  // another key play both; others are plain maps.
  for (int pc = 0; pc < 12; ++pc) {
    if (anyBoth && !both[pc] && offs[pc] != 0 && offs[pc] > -13) exact = false;
    set(m, mod::KmMap + pc, clampd(offs[pc], -13, 12));
  }
  set(m, mod::KmMulti, anyBoth ? 1 : 0);
  if (!enabled)
    for (int pc = 0; pc < 12; ++pc) set(m, mod::KmMap + pc, 0);
  applyRows(m, {{2, -12, 12, mod::KmTranspose}}, {0, 0, static_cast<double>(offset)});
  m.exact = exact;
  return true;
}

// ------------------------------------------------------ Keyboard Splitter

struct Env {
  std::vector<double> x, y, tension;
  // Level at position q (0..1): linear between points; steps (equal x) take
  // the earlier segment's end.
  double at(double q) const {
    if (x.empty()) return 0;
    if (q <= x.front()) return y.front();
    for (size_t i = 0; i + 1 < x.size(); ++i) {
      if (q > x[i + 1] || x[i + 1] <= x[i]) continue;
      if (q < x[i]) continue;
      return y[i] + (y[i + 1] - y[i]) * (q - x[i]) / (x[i + 1] - x[i]);
    }
    return y.back();
  }
};

// Reads one envelope at p (advances p); false when malformed.
bool readEnv(const Blob& b, size_t& p, Env& e) {
  if (!b.has(p, 12)) return false;
  const uint32_t n = b.u32(p + 8);
  if (b.u32(p) != 3 || n == 0 || n > 512 || !b.has(p + 12, n * 24 + 20)) return false;
  p += 12;
  double x = 0;
  for (uint32_t k = 0; k < n; ++k, p += 24) {
    x += b.f64(p);
    e.x.push_back(x);
    e.y.push_back(b.f64(p + 8));
    e.tension.push_back(b.f32(p + 16));
  }
  p += 20;
  return std::isfinite(x);
}

bool binary(double v) { return std::fabs(v) < 1e-3 || std::fabs(v - 1) < 1e-3; }

bool decodeSplitter(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  size_t p = 0;
  for (size_t h : {4, 5})
    if (b.u32(h) == 3 && b.u32(h + 4) == 3) {
      p = h;
      break;
    }
  if (p == 0) return false;
  begin(m, "gl.split");
  bool exact = true;
  int used = 0;
  for (int z = 0; z < 16; ++z) {
    Env key, vel;
    if (!readEnv(b, p, key) || !readEnv(b, p, vel) || !b.has(p, 7)) {
      if (z == 0) return false;
      exact = false;
      break;
    }
    const int transpose = b.i32(p);
    const bool absolute = b.u8(p + 4) != 0;
    p += 7;
    // Keys 0..131 sampled at whole keys.
    int lo = -1, hi = -1;
    bool keyClean = true;
    for (int k = 0; k < 132; ++k) {
      double y = key.at(k / 132.0);
      if (!binary(y)) keyClean = false;
      if (y >= 0.5) {
        if (lo < 0) lo = k;
        else if (hi != k - 1) keyClean = false;  // gap: several ranges
        hi = k;
      }
    }
    int vlo = -1, vhi = -1;
    bool velClean = true;
    for (int k = 0; k <= 127; ++k) {
      double y = vel.at(k / 127.0);
      if (!binary(y)) velClean = false;
      if (y >= 0.5) {
        if (vlo < 0) vlo = k;
        else if (vhi != k - 1) velClean = false;
        vhi = k;
      }
    }
    if (lo < 0 || vlo < 0) continue;  // zone plays nothing
    if (used >= mod::kSplitZones) {
      exact = false;
      continue;
    }
    // Absolute mode replaces the velocity with the mapping level: not modelled.
    if (!keyClean || !velClean || absolute || lo > 127) exact = false;
    if (transpose < -48 || transpose > 48) exact = false;
    const int zi = used++;
    set(m, mod::splitParam(zi, mod::SpOn), 1);
    set(m, mod::splitParam(zi, mod::SpLow), std::min(lo, 127));
    set(m, mod::splitParam(zi, mod::SpHigh), std::min(hi, 127));
    set(m, mod::splitParam(zi, mod::SpTranspose), clampd(transpose, -48, 48));
    set(m, mod::splitParam(zi, mod::SpVelLow), vlo / 127.0);
    set(m, mod::splitParam(zi, mod::SpVelHigh), vhi / 127.0);
  }
  for (int z = used; z < mod::kSplitZones; ++z) set(m, mod::splitParam(z, mod::SpOn), 0);
  set(m, mod::SpMode, 0);  // every matching zone plays
  m.exact = exact && used > 0;
  linkAll(m, {}, {});
  return true;
}

// ------------------------------------------------------------- VFX Script

// FL note-formula expressions (Python syntax) → Gridline formula text.
class FormulaTranslator {
 public:
  struct Val {
    std::string expr;
    std::vector<std::string> list;
    bool isList = false;
    bool boolean = false;
    bool random = false;
    std::vector<Val> factors;  // set when the value is a pure product
  };
  double dKnob = 0;          // value substituted for knob D (Gridline has a, b, c)
  bool approximate = false;  // a construct was approximated
  bool usedD = false;

  bool translate(const std::string& text, Val& out) {
    toks_.clear();
    pos_ = 0;
    if (!tokenize(text)) return false;
    if (!parseOr(out)) return false;
    return pos_ == toks_.size() && !out.isList;
  }

 private:
  struct Tok {
    enum Kind { Num, Name, Op } kind;
    std::string s;
  };
  std::vector<Tok> toks_;
  size_t pos_ = 0;

  bool tokenize(const std::string& t) {
    size_t i = 0;
    while (i < t.size()) {
      char c = t[i];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++i;
      } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < t.size() && std::isdigit(static_cast<unsigned char>(t[i + 1])))) {
        size_t j = i;
        while (j < t.size() && (std::isdigit(static_cast<unsigned char>(t[j])) || t[j] == '.')) ++j;
        if (j < t.size() && (t[j] == 'e' || t[j] == 'E')) {
          size_t k = j + 1;
          if (k < t.size() && (t[k] == '+' || t[k] == '-')) ++k;
          if (k < t.size() && std::isdigit(static_cast<unsigned char>(t[k]))) {
            j = k;
            while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
          }
        }
        toks_.push_back({Tok::Num, t.substr(i, j - i)});
        i = j;
      } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        size_t j = i;
        while (j < t.size() && (std::isalnum(static_cast<unsigned char>(t[j])) || t[j] == '_' || t[j] == '.')) ++j;
        toks_.push_back({Tok::Name, t.substr(i, j - i)});
        i = j;
      } else {
        static const char* two[] = {"**", "//", "==", "!=", "<=", ">="};
        bool done = false;
        for (const char* op : two)
          if (t.compare(i, 2, op) == 0) {
            toks_.push_back({Tok::Op, op});
            i += 2;
            done = true;
            break;
          }
        if (done) continue;
        if (std::string("+-*/%()[],<>").find(c) == std::string::npos) return false;
        toks_.push_back({Tok::Op, std::string(1, c)});
        ++i;
      }
    }
    return true;
  }
  bool isOp(const char* s) const { return pos_ < toks_.size() && toks_[pos_].kind == Tok::Op && toks_[pos_].s == s; }
  bool isWord(const char* s) const { return pos_ < toks_.size() && toks_[pos_].kind == Tok::Name && toks_[pos_].s == s; }
  static Val scalar(std::string e, bool boolean = false, bool random = false) {
    Val v;
    v.expr = std::move(e);
    v.boolean = boolean;
    v.random = random;
    return v;
  }

  bool parseOr(Val& out) {
    if (!parseAnd(out)) return false;
    while (isWord("or")) {
      ++pos_;
      Val r;
      if (!parseAnd(r) || out.isList || r.isList) return false;
      out = scalar("(" + out.expr + " || " + r.expr + ")", true, out.random || r.random);
    }
    return true;
  }
  bool parseAnd(Val& out) {
    if (!parseNot(out)) return false;
    while (isWord("and")) {
      ++pos_;
      Val r;
      if (!parseNot(r) || out.isList || r.isList) return false;
      out = scalar("(" + out.expr + " && " + r.expr + ")", true, out.random || r.random);
    }
    return true;
  }
  bool parseNot(Val& out) {
    if (isWord("not")) {
      ++pos_;
      Val v;
      if (!parseNot(v) || v.isList) return false;
      out = scalar("(!" + v.expr + ")", true, v.random);
      return true;
    }
    return parseCmp(out);
  }
  bool parseCmp(Val& out) {
    if (!parseSum(out)) return false;
    std::vector<std::string> parts;
    Val left = out;
    bool random = out.random;
    while (isOp("<") || isOp(">") || isOp("<=") || isOp(">=") || isOp("==") || isOp("!=")) {
      std::string op = toks_[pos_++].s;
      Val r;
      if (!parseSum(r) || left.isList || r.isList) return false;
      parts.push_back("(" + left.expr + " " + op + " " + r.expr + ")");
      random = random || r.random;
      left = r;
    }
    if (parts.empty()) return true;
    std::string e = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) e = "(" + e + " && " + parts[i] + ")";
    out = scalar(e, true, random);
    return true;
  }
  bool parseSum(Val& out) {
    if (!parseTerm(out)) return false;
    while (isOp("+") || isOp("-")) {
      std::string op = toks_[pos_++].s;
      Val r;
      if (!parseTerm(r) || out.isList || r.isList) return false;
      out = scalar("(" + out.expr + " " + op + " " + r.expr + ")", false, out.random || r.random);
    }
    return true;
  }
  bool parseTerm(Val& out) {
    Val first;
    if (!parseUnary(first)) return false;
    std::vector<Val> factors{first};
    bool pure = true;
    out = first;
    while (isOp("*") || isOp("/") || isOp("//") || isOp("%")) {
      std::string op = toks_[pos_++].s;
      Val r;
      if (!parseUnary(r) || out.isList || r.isList) return false;
      if (op != "*") pure = false;
      factors.push_back(r);
      if (op == "//") out = scalar("floor(" + out.expr + " / " + r.expr + ")", false, out.random || r.random);
      else out = scalar("(" + out.expr + " " + op + " " + r.expr + ")", false, out.random || r.random);
    }
    if (pure && factors.size() > 1) out.factors = factors;
    return true;
  }
  bool parseUnary(Val& out) {
    if (isOp("-") || isOp("+")) {
      std::string op = toks_[pos_++].s;
      Val v;
      if (!parseUnary(v) || v.isList) return false;
      out = scalar(op == "-" ? "(-" + v.expr + ")" : v.expr, false, v.random);
      return true;
    }
    return parsePower(out);
  }
  bool parsePower(Val& out) {
    if (!parsePostfix(out)) return false;
    if (isOp("**")) {
      ++pos_;
      Val r;
      if (!parseUnary(r) || out.isList || r.isList) return false;
      out = scalar("(" + out.expr + " ^ " + r.expr + ")", false, out.random || r.random);
    }
    return true;
  }
  // list[index] → nested choices on the index.
  Val pick(const Val& list, const std::string& index, bool random) {
    std::string e = list.list.back();
    for (size_t k = list.list.size() - 1; k-- > 0;)
      e = "(floor(" + index + ") == " + std::to_string(k) + " ? " + list.list[k] + " : " + e + ")";
    return scalar(e, false, random || list.random);
  }
  bool parsePostfix(Val& out) {
    if (!parseAtom(out)) return false;
    while (isOp("[")) {
      ++pos_;
      Val idx;
      if (!parseOr(idx) || idx.isList || !isOp("]") || !out.isList || out.list.empty()) return false;
      ++pos_;
      if (idx.random) approximate = true;  // the index is drawn once per comparison
      out = pick(out, idx.expr, idx.random);
    }
    return true;
  }
  bool parseArgs(std::vector<Val>& args) {
    ++pos_;  // (
    if (isOp(")")) return ++pos_, true;
    for (;;) {
      Val a;
      if (!parseOr(a)) return false;
      args.push_back(a);
      if (isOp(",")) {
        ++pos_;
        continue;
      }
      if (!isOp(")")) return false;
      ++pos_;
      return true;
    }
  }
  bool parseAtom(Val& out) {
    if (pos_ >= toks_.size()) return false;
    const Tok t = toks_[pos_];
    if (t.kind == Tok::Num) {
      ++pos_;
      out = scalar(t.s);
      return true;
    }
    if (t.kind == Tok::Op && t.s == "(") {
      ++pos_;
      if (!parseOr(out) || !isOp(")")) return false;
      ++pos_;  // every emitted compound is already parenthesized
      return true;
    }
    if (t.kind == Tok::Op && t.s == "[") {
      ++pos_;
      Val l;
      l.isList = true;
      while (!isOp("]")) {
        Val e;
        if (!parseOr(e) || e.isList) return false;
        l.list.push_back(e.expr);
        l.random = l.random || e.random;
        if (isOp(",")) ++pos_;
        else if (!isOp("]")) return false;
      }
      ++pos_;
      out = l;
      return true;
    }
    if (t.kind != Tok::Name) return false;
    ++pos_;
    std::string n = t.s;
    if (n.rfind("math.", 0) == 0) n = n.substr(5);
    if (n.rfind("random.", 0) == 0) n = n.substr(7);
    if (isOp("(")) return call(n, out);
    static const std::map<std::string, std::string> vars = {
        {"note", "pitch"}, {"velocity", "vel"}, {"A", "a"}, {"B", "b"}, {"C", "c"}, {"clock", "beat"},
        {"count", "n"},    {"pi", "pi"},        {"e", "e"}, {"True", "1"}, {"False", "0"}};
    if (n == "D") {
      usedD = true;
      out = scalar(std::to_string(dKnob));
      return true;
    }
    auto it = vars.find(n);
    if (it == vars.end()) return false;
    out = scalar(it->second);
    return true;
  }
  bool call(const std::string& f, Val& out) {
    std::vector<Val> a;
    if (!parseArgs(a)) return false;
    for (const auto& v : a)
      if (v.isList && f != "choice" && f != "choices") return false;
    auto anyRandom = [&] {
      bool r = false;
      for (const auto& v : a) r = r || v.random;
      return r;
    };
    auto join = [&] {
      std::string s;
      for (size_t i = 0; i < a.size(); ++i) s += (i ? ", " : "") + a[i].expr;
      return s;
    };
    static const std::map<std::string, std::string> same = {
        {"sin", "sin"},     {"cos", "cos"},     {"tan", "tan"},   {"asin", "asin"}, {"acos", "acos"},   {"atan", "atan"},
        {"atan2", "atan2"}, {"sinh", "sinh"},   {"cosh", "cosh"}, {"tanh", "tanh"}, {"sqrt", "sqrt"},   {"exp", "exp"},
        {"log10", "log10"}, {"pow", "pow"},     {"floor", "floor"}, {"ceil", "ceil"}, {"fabs", "abs"}, {"abs", "abs"},
        {"min", "min"},     {"max", "max"},     {"round", "round"}, {"int", "trunc"}, {"degrees", "deg"}, {"radians", "rad"},
        {"fmod", "fmod"}};
    if (f == "random" && a.empty()) {
      out = scalar("rand()", false, true);
      return true;
    }
    if (f == "float" && a.size() == 1) {
      out = a[0];
      return true;
    }
    if (f == "log" && (a.size() == 1 || a.size() == 2)) {
      out = scalar(a.size() == 1 ? "log(" + a[0].expr + ")" : "(log(" + a[0].expr + ") / log(" + a[1].expr + "))", false, anyRandom());
      return true;
    }
    if (f == "uniform" && a.size() == 2) {
      out = scalar("(" + a[0].expr + " + (" + a[1].expr + " - " + a[0].expr + ") * rand())", false, true);
      return true;
    }
    if (f == "randint" && a.size() == 2) {
      out = scalar("floor(" + a[0].expr + " + (" + a[1].expr + " - " + a[0].expr + " + 1) * rand())", false, true);
      return true;
    }
    if ((f == "gauss" || f == "normalvariate") && a.size() == 2) {
      // Bell curve from four uniforms (same mean and spread, bounded tails).
      approximate = true;
      out = scalar("(" + a[0].expr + " + " + a[1].expr + " * (rand() + rand() + rand() + rand() - 2) * 1.7320508)", false, true);
      return true;
    }
    if ((f == "choice" || f == "choices") && !a.empty() && a[0].isList && !a[0].list.empty()) {
      // One draw per note, shared by all comparisons: a fixed hash of the note count and time.
      approximate = true;
      if (f == "choices" && a.size() > 1) approximate = true;  // weights ignored
      std::string idx = "(rand(n * 7.31 + t * 13.7) * " + std::to_string(a[0].list.size()) + ")";
      out = pick(a[0], idx, true);
      return true;
    }
    auto it = same.find(f);
    if (it == same.end()) return false;
    out = scalar(it->second + "(" + join() + ")", false, anyRandom());
    return true;
  }
};

struct Ini {
  std::vector<std::string> source;                              // script lines
  std::vector<std::pair<std::string, std::string>> parameters;  // in order
  const std::string* param(const std::string& k) const {
    for (const auto& [n, v] : parameters)
      if (n == k) return &v;
    return nullptr;
  }
};

Ini parseIni(const std::string& text) {
  Ini ini;
  std::string section;
  size_t p = 0;
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) p = 3;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    std::string ln = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
    p = e == std::string::npos ? text.size() : e + 1;
    while (!ln.empty() && (ln.back() == '\r' || ln.back() == '\n')) ln.pop_back();
    if (ln.size() >= 2 && ln.front() == '[' && ln.back() == ']') {
      section = ln.substr(1, ln.size() - 2);
      continue;
    }
    size_t eq = ln.find('=');
    if (eq == std::string::npos) continue;
    std::string key = ln.substr(0, eq), val = ln.substr(eq + 1);
    if (section == "Script" && key == "Source") {
      size_t q = 0;
      for (;;) {
        size_t k = val.find("<linebreak>", q);
        ini.source.push_back(val.substr(q, k == std::string::npos ? std::string::npos : k - q));
        if (k == std::string::npos) break;
        q = k + 11;
      }
    } else if (section == "Parameters") {
      ini.parameters.emplace_back(key, val);
    }
  }
  return ini;
}

// The script's executable code (comments, docstrings and blank lines removed).
std::string scriptCode(const std::vector<std::string>& lines) {
  std::string code;
  bool doc = false;
  for (const auto& l : lines) {
    std::string s = l;
    size_t q = s.find("\"\"\"");
    if (doc) {
      if (q == std::string::npos) continue;
      doc = false;
      s = s.substr(q + 3);
    } else if (q != std::string::npos) {
      size_t q2 = s.find("\"\"\"", q + 3);
      if (q2 == std::string::npos) {
        doc = true;
        s = s.substr(0, q);
      } else {
        s.erase(q, q2 + 3 - q);
      }
    }
    size_t h = s.find('#');
    if (h != std::string::npos) s = s.substr(0, h);
    if (s.find_first_not_of(" \t\r") == std::string::npos) continue;
    code += s + "\n";
  }
  return code;
}

// A script that only re-triggers every incoming voice unchanged.
bool isPassThrough(const Ini& ini) {
  if (!ini.parameters.empty()) return false;
  std::string code = scriptCode(ini.source);
  static const char* changes[] = {".note", ".velocity", ".pan", ".length", ".fcut", ".fres", ".finePitch", ".color",
                                  ".output", "Output", "setNormalizedValue", "ticks", "random", "time"};
  for (const char* c : changes)
    if (code.find(c) != std::string::npos) return false;
  size_t n = 0;
  for (size_t q = code.find(".trigger("); q != std::string::npos; q = code.find(".trigger(", q + 1)) ++n;
  return n == 1 && code.find(".release(") != std::string::npos && code.find("copyFrom") != std::string::npos;
}

constexpr double kVfxKnob = 262144.0;  // stored knob value of 1.0

double knobValue(const Ini& ini, const char* name, double def) {
  const std::string* v = ini.param(name);
  if (!v) return def;
  char* end = nullptr;
  double x = std::strtod(v->c_str(), &end);
  return end == v->c_str() ? def : clampd(x / kVfxKnob, 0, 1);
}

bool decodeScript(Context&, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 4) return false;
  size_t len = std::min<size_t>(b.u32(0), b.size() - 4);
  const std::string text(reinterpret_cast<const char*>(b.p + 4), len);
  if (text.find('[') == std::string::npos) return false;
  Ini ini = parseIni(text);
  begin(m, "gl.notescript");
  std::string body = "# Converted from an FL Studio VFX script.\n";
  bool exact = false;
  std::string formulas;
  const std::string* noteF = ini.param("Formulas: note out :");
  const std::string* velF = ini.param("Formulas: velocity out :");
  if (noteF && velF) {
    // Note formula script: the two formulas and knobs A..C map 1:1.
    FormulaTranslator tr;
    tr.dKnob = knobValue(ini, "Parameters: D", 0);
    FormulaTranslator::Val pv, vv;
    bool ok = tr.translate(*noteF, pv) && tr.translate(*velF, vv);
    if (ok) {
      formulas = "pitch = " + pv.expr + "\n";
      // "(condition) * (velocity …)": the condition decides whether the note plays.
      std::string play, vel = vv.expr;
      if (!vv.factors.empty()) {
        std::string keep;
        for (const auto& f : vv.factors) {
          if (f.boolean) play += (play.empty() ? "" : " && ") + f.expr;
          else keep += (keep.empty() ? "" : " * ") + f.expr;
        }
        if (!play.empty()) vel = keep.empty() ? "1" : keep;
      }
      formulas += "velocity = " + vel + "\n";
      if (!play.empty()) formulas += "play = " + play + "\n";
      mod::NoteScriptProgram prog;
      mod::compileNoteScript(formulas, prog);
      ok = prog.error.empty();
    }
    if (ok) {
      exact = !tr.approximate && !tr.usedD;
    } else {
      formulas.clear();
      body += "# The formulas could not be translated; notes pass unchanged.\n";
    }
    set(m, mod::NsA, knobValue(ini, "Parameters: A", 0));
    set(m, mod::NsB, knobValue(ini, "Parameters: B", 0));
    set(m, mod::NsC, knobValue(ini, "Parameters: C", 0));
    body += "# note out: " + *noteF + "\n# velocity out: " + *velF + "\n";
    if (tr.usedD) body += "# knob D is fixed at " + std::to_string(tr.dKnob) + "\n";
  } else if (isPassThrough(ini)) {
    exact = true;
  } else {
    body += "# The script is kept below for reference; Gridline passes notes unchanged.\n";
  }
  if (formulas.empty()) formulas = "pitch = pitch\nvelocity = vel\n";
  if (!ini.parameters.empty()) {
    body += "# --- parameters ---\n";
    for (const auto& [k, v] : ini.parameters)
      if (k.find("PluginData") == std::string::npos) body += "# " + k + " = " + v + "\n";
  }
  body += "# --- script ---\n";
  for (const auto& l : ini.source) {
    std::string s = l;
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    body += "# " + s + "\n";
  }
  m.state.data = body + formulas;
  set(m, mod::NsEnable, 1);
  // Automation: the script's controls in [Parameters] order; knobs A..C.
  std::vector<Link> links;
  std::vector<RawRange> ranges;
  for (size_t i = 0; i < ini.parameters.size(); ++i) {
    const std::string& k = ini.parameters[i].first;
    int gl = k == "Parameters: A" ? mod::NsA : k == "Parameters: B" ? mod::NsB : k == "Parameters: C" ? mod::NsC : -1;
    if (gl < 0 || !noteF) continue;
    links.push_back({static_cast<int>(i), gl, nullptr});
    ranges.push_back({static_cast<int>(i), 0, kVfxKnob});
  }
  linkAll(m, std::move(links), std::move(ranges));
  m.exact = exact;
  return true;
}

// ----------------------------------------------------------- VFX Envelope

bool decodeVfxEnvelope(Context& c, const std::vector<Event>& evs, PluginMapping& m) {
  Blob b = gen2::stateBlob(evs);
  if (b.size() < 0x100 || b.u16(0) != 3) return false;
  begin(m, "gl.envctl");
  set(m, mod::EcModel, 0);  // the four-envelope simple model (J-41)
  // Envelope lists in order: list 5·a + t = articulator a, target t.
  struct List {
    size_t at;
    int n;
  };
  std::vector<List> lists;
  for (size_t p = 0x40; b.has(p, 28);) {
    const int n = b.i32(p + 24);
    if (b.i32(p) == 3 && b.i32(p + 16) == 1 && b.i32(p + 20) == 3 && n >= 1 && n <= 64 && b.has(p + 28, 24 * static_cast<size_t>(n))) {
      lists.push_back({p, n});
      p += 28 + 24 * static_cast<size_t>(n);
    } else {
      p += 4;
    }
  }
  if (lists.empty()) return false;
  const double secPerBeat = 60.0 / (c.song.tempo > 1 ? c.song.tempo : 120.0);
  for (int a = 0; a < mod::kEnvArts; ++a) {
    const size_t li = static_cast<size_t>(a) * 5;
    if (li >= lists.size()) break;
    const List& l = lists[li];
    std::vector<double> dt, y;
    for (int k = 0; k < l.n; ++k) {
      double t = b.f64(l.at + 28 + 24 * static_cast<size_t>(k)), v = b.f64(l.at + 36 + 24 * static_cast<size_t>(k));
      if (!std::isfinite(t) || !std::isfinite(v) || t < 0 || t > 1000) break;
      dt.push_back(t);
      y.push_back(clampd(v, 0, 1));
    }
    if (dt.empty()) continue;
    if (dt.size() == 1) dt.push_back(0), y.push_back(y[0]);  // flat
    const int np = std::min(static_cast<int>(dt.size()), mod::kEnvPoints);
    for (int k = 0; k < np; ++k) {
      set(m, mod::envPointTime(a, k), clampd(dt[static_cast<size_t>(k)] * secPerBeat, 0.001, 10));
      set(m, mod::envPointLevel(a, k), y[static_cast<size_t>(k)]);
    }
    set(m, mod::envParam(a, mod::EaNumPoints), std::max(2, np));
    // Hold before a final fall to zero (release), else one-shot.
    int sustain = np >= 3 && y[static_cast<size_t>(np - 1)] < 1e-6 && y[static_cast<size_t>(np - 2)] > 1e-6 ? np - 1 : 0;
    set(m, mod::envParam(a, mod::EaSustain), sustain);
    set(m, mod::envParam(a, mod::EaBase), 0.0);
    set(m, mod::envParam(a, mod::EaEnvAmt), 1.0);
    set(m, mod::envParam(a, mod::EaLfoAmt), 0.0);
  }
  // FL controls 8..10 (base, envelope, LFO level of the shown articulator) → articulator A.
  applyRows(m,
            {{8, 0, 256, mod::envParam(0, mod::EaBase), [](double v) { return v / 256.0; }},
             {9, -256, 256, mod::envParam(0, mod::EaEnvAmt), [](double v) { return v / 256.0; }},
             {10, -256, 256, mod::envParam(0, mod::EaLfoAmt), [](double v) { return v / 256.0; }}},
            {});
  // Per-voice note parameter envelopes become control outputs: approximate.
  m.exact = false;
  return true;
}

}  // namespace

void registerVfx(std::map<std::string, PluginDecoder>& d) {
  add(d, "VFX Key Mapper", decodeKeyMapper);
  add(d, "VFX Keyboard Splitter", decodeSplitter);
  add(d, "VFX Script", decodeScript);
  add(d, "VFX Envelope", decodeVfxEnvelope);
}

}  // namespace gl::flp::dec3
