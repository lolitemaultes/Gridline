#include "Expr.h"
#include "Platform.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace gl {

namespace {

enum Code : uint8_t {
  OConst, OVar, ONeg, ONot, OAdd, OSub, OMul, ODiv, OMod, OPow,
  OLt, OLe, OGt, OGe, OEq, ONe, OAnd, OOr, OSelect, OFunc
};

enum Fn : int {
  FSin, FCos, FTan, FAsin, FAcos, FAtan, FAtan2, FSinh, FCosh, FTanh, FSqrt, FAbs, FSign, FExp, FLog, FLog2,
  FLog10, FPow, FMin, FMax, FFloor, FCeil, FRound, FTrunc, FFrac, FFmod, FClamp, FLerp, FStep, FSmooth, FIf,
  FSaw, FTri, FSqr, FPulse, FWrap, FFold, FQuant, FRand, FDeg, FRad, FTension
};

struct FnInfo {
  const char* name;
  int fn;
  int minArgs, maxArgs;  // maxArgs < 0 = any (min/max)
};

const FnInfo kFns[] = {
    {"sin", FSin, 1, 1},     {"cos", FCos, 1, 1},       {"tan", FTan, 1, 1},     {"asin", FAsin, 1, 1},
    {"acos", FAcos, 1, 1},   {"atan", FAtan, 1, 1},     {"atan2", FAtan2, 2, 2}, {"sinh", FSinh, 1, 1},
    {"cosh", FCosh, 1, 1},   {"tanh", FTanh, 1, 1},     {"sqrt", FSqrt, 1, 1},   {"abs", FAbs, 1, 1},
    {"sign", FSign, 1, 1},   {"exp", FExp, 1, 1},       {"log", FLog, 1, 1},     {"ln", FLog, 1, 1},
    {"log2", FLog2, 1, 1},   {"log10", FLog10, 1, 1},   {"pow", FPow, 2, 2},     {"min", FMin, 1, -1},
    {"max", FMax, 1, -1},    {"floor", FFloor, 1, 1},   {"ceil", FCeil, 1, 1},   {"round", FRound, 1, 1},
    {"trunc", FTrunc, 1, 1}, {"frac", FFrac, 1, 1},     {"fmod", FFmod, 2, 2},   {"mod", FFmod, 2, 2},
    {"clamp", FClamp, 3, 3}, {"lerp", FLerp, 3, 3},     {"step", FStep, 2, 2},   {"smoothstep", FSmooth, 3, 3},
    {"if", FIf, 3, 3},       {"saw", FSaw, 1, 1},       {"tri", FTri, 1, 1},     {"sqr", FSqr, 1, 1},
    {"pulse", FPulse, 2, 2}, {"wrap", FWrap, 1, 1},     {"fold", FFold, 1, 1},   {"quant", FQuant, 2, 2},
    {"rand", FRand, 0, 1},   {"deg", FDeg, 1, 1},       {"rad", FRad, 1, 1},     {"tension", FTension, 2, 2},
};

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

class Parser {
 public:
  Parser(const std::string& s, const std::vector<std::string>& vars, std::vector<Expr::Op>& out)
      : s_(s), vars_(vars), out_(out) {}

  bool run(std::string& err) {
    skip();
    if (pos_ >= s_.size()) return fail("empty formula", err);
    if (!ternary()) return fail(msg_, err);
    skip();
    if (pos_ < s_.size()) return fail("unexpected '" + std::string(1, s_[pos_]) + "'", err);
    return true;
  }

 private:
  bool fail(const std::string& m, std::string& err) {
    err = m + " (at " + std::to_string(errPos_ ? errPos_ : pos_ + 1) + ")";
    return false;
  }
  bool error(const std::string& m) {
    if (msg_.empty()) {
      msg_ = m;
      errPos_ = pos_ + 1;
    }
    return false;
  }
  void skip() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
  }
  bool peek(const char* tok) {
    skip();
    size_t n = std::char_traits<char>::length(tok);
    return s_.compare(pos_, n, tok) == 0;
  }
  bool take(const char* tok) {
    if (!peek(tok)) return false;
    pos_ += std::char_traits<char>::length(tok);
    return true;
  }
  void emit(uint8_t code, int index = 0, double value = 0, uint8_t argc = 0) {
    Expr::Op o;
    o.code = code;
    o.index = index;
    o.value = value;
    o.argc = argc;
    out_.push_back(o);
  }
  bool depthOk() { return ++depth_ < 200 ? true : error("formula nested too deeply"); }

  bool ternary() {
    if (!depthOk()) return false;
    if (!orExpr()) return false;
    if (take("?")) {
      if (!ternary()) return false;
      if (!take(":")) return error("expected ':'");
      if (!ternary()) return false;
      emit(OSelect);
    }
    --depth_;
    return true;
  }
  bool orExpr() {
    if (!andExpr()) return false;
    for (;;) {
      if (take("||") || takeWord("or")) {
        if (!andExpr()) return false;
        emit(OOr);
      } else {
        return true;
      }
    }
  }
  bool andExpr() {
    if (!cmp()) return false;
    for (;;) {
      if (take("&&") || takeWord("and")) {
        if (!cmp()) return false;
        emit(OAnd);
      } else {
        return true;
      }
    }
  }
  bool takeWord(const char* w) {
    skip();
    size_t n = std::char_traits<char>::length(w);
    if (lower(s_.substr(pos_, n)) != w) return false;
    if (pos_ + n < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_ + n])) || s_[pos_ + n] == '_')) return false;
    pos_ += n;
    return true;
  }
  bool cmp() {
    if (!add()) return false;
    for (;;) {
      uint8_t op;
      if (take("<=")) op = OLe;
      else if (take(">=")) op = OGe;
      else if (take("==")) op = OEq;
      else if (take("!=") || take("<>")) op = ONe;
      else if (take("<")) op = OLt;
      else if (take(">")) op = OGt;
      else if (peek("=") && !peek("==")) {
        ++pos_;
        op = OEq;  // single '=' compares too
      } else
        return true;
      if (!add()) return false;
      emit(op);
    }
  }
  bool add() {
    if (!mul()) return false;
    for (;;) {
      uint8_t op;
      if (take("+")) op = OAdd;
      else if (take("-")) op = OSub;
      else return true;
      if (!mul()) return false;
      emit(op);
    }
  }
  bool mul() {
    if (!unary()) return false;
    for (;;) {
      uint8_t op;
      if (take("*")) op = OMul;
      else if (take("/")) op = ODiv;
      else if (take("%")) op = OMod;
      else return true;
      if (!unary()) return false;
      emit(op);
    }
  }
  bool unary() {
    if (!depthOk()) return false;
    bool ok;
    if (take("-")) {
      ok = unary();
      if (ok) emit(ONeg);
    } else if (take("+")) {
      ok = unary();
    } else if (peek("!") && !peek("!=")) {
      ++pos_;
      ok = unary();
      if (ok) emit(ONot);
    } else {
      ok = power();
    }
    --depth_;
    return ok;
  }
  bool power() {
    if (!primary()) return false;
    if (take("^")) {
      if (!unary()) return false;  // right-associative, binds tighter than unary minus on the left
      emit(OPow);
    }
    return true;
  }
  bool primary() {
    skip();
    if (pos_ >= s_.size()) return error("unexpected end of formula");
    char c = s_[pos_];
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      const char* start = s_.c_str() + pos_;
      char* end = nullptr;
      double v = platform::parseDouble(start, &end);
      if (end == start) return error("bad number");
      pos_ += static_cast<size_t>(end - start);
      emit(OConst, 0, v);
      return true;
    }
    if (c == '(') {
      ++pos_;
      if (!ternary()) return false;
      if (!take(")")) return error("expected ')'");
      return true;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t st = pos_;
      while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
      std::string name = lower(s_.substr(st, pos_ - st));
      if (peek("(")) {
        ++pos_;
        const FnInfo* fi = nullptr;
        for (const auto& f : kFns)
          if (name == f.name) fi = &f;
        if (!fi) {
          pos_ = st;
          return error("unknown function '" + name + "'");
        }
        int argc = 0;
        if (!take(")")) {
          for (;;) {
            if (!ternary()) return false;
            ++argc;
            if (take(")")) break;
            if (!take(",")) return error("expected ',' or ')'");
          }
        }
        if (argc < fi->minArgs || (fi->maxArgs >= 0 && argc > fi->maxArgs)) {
          pos_ = st;
          return error("wrong number of arguments for '" + name + "'");
        }
        if (fi->fn == FMin || fi->fn == FMax) {
          for (int k = 1; k < argc; ++k) emit(OFunc, fi->fn, 0, 2);
          return true;
        }
        if (fi->fn == FIf) {
          emit(OSelect);
          return true;
        }
        emit(OFunc, fi->fn, 0, static_cast<uint8_t>(argc));
        return true;
      }
      for (size_t i = 0; i < vars_.size(); ++i)
        if (lower(vars_[i]) == name) {
          emit(OVar, static_cast<int>(i));
          return true;
        }
      if (name == "pi") return emit(OConst, 0, 3.14159265358979323846), true;
      if (name == "e") return emit(OConst, 0, 2.71828182845904523536), true;
      if (name == "true") return emit(OConst, 0, 1), true;
      if (name == "false") return emit(OConst, 0, 0), true;
      pos_ = st;
      return error("unknown name '" + name + "'");
    }
    return error("unexpected '" + std::string(1, c) + "'");
  }

  const std::string& s_;
  const std::vector<std::string>& vars_;
  std::vector<Expr::Op>& out_;
  size_t pos_ = 0;
  int depth_ = 0;
  std::string msg_;
  size_t errPos_ = 0;
};

inline double frac(double x) { return x - std::floor(x); }

inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

inline double finite(double v) { return std::isfinite(v) ? v : 0.0; }

}  // namespace

bool Expr::compile(const std::string& text, const std::vector<std::string>& vars, std::string* err) {
  ops_.clear();
  nvars_ = vars.size();
  std::vector<Op> out;
  std::string e;
  Parser p(text, vars, out);
  if (!p.run(e)) {
    if (err) *err = e;
    return false;
  }
  // Verify the stack depth so evaluation can use a fixed array.
  int depth = 0, maxDepth = 0;
  for (const auto& o : out) {
    switch (o.code) {
      case OConst:
      case OVar: ++depth; break;
      case ONeg:
      case ONot: break;
      case OSelect: depth -= 2; break;
      case OFunc: depth += 1 - o.argc; break;
      default: depth -= 1; break;
    }
    maxDepth = std::max(maxDepth, depth);
  }
  if (depth != 1 || maxDepth > kMaxStack) {
    if (err) *err = depth != 1 ? "malformed formula" : "formula too complex";
    return false;
  }
  ops_ = std::move(out);
  if (err) err->clear();
  return true;
}

bool Expr::uses(size_t var) const {
  for (const auto& o : ops_)
    if (o.code == OVar && static_cast<size_t>(o.index) == var) return true;
  return false;
}

double Expr::eval(const double* vars, uint32_t* rng) const {
  if (ops_.empty()) return 0.0;
  double st[kMaxStack + 2];
  int sp = 0;
  for (const auto& o : ops_) {
    switch (o.code) {
      case OConst: st[sp++] = o.value; break;
      case OVar: st[sp++] = vars ? vars[o.index] : 0.0; break;
      case ONeg: st[sp - 1] = -st[sp - 1]; break;
      case ONot: st[sp - 1] = st[sp - 1] == 0.0 ? 1.0 : 0.0; break;
      case OSelect: {
        double b = st[--sp], a = st[--sp];
        st[sp - 1] = st[sp - 1] != 0.0 ? a : b;
        break;
      }
      case OFunc: {
        int argc = o.argc;
        double* a = st + sp - argc;
        double r = 0;
        switch (o.index) {
          case FSin: r = std::sin(a[0]); break;
          case FCos: r = std::cos(a[0]); break;
          case FTan: r = std::tan(a[0]); break;
          case FAsin: r = std::asin(std::clamp(a[0], -1.0, 1.0)); break;
          case FAcos: r = std::acos(std::clamp(a[0], -1.0, 1.0)); break;
          case FAtan: r = std::atan(a[0]); break;
          case FAtan2: r = std::atan2(a[0], a[1]); break;
          case FSinh: r = std::sinh(a[0]); break;
          case FCosh: r = std::cosh(a[0]); break;
          case FTanh: r = std::tanh(a[0]); break;
          case FSqrt: r = a[0] > 0 ? std::sqrt(a[0]) : 0.0; break;
          case FAbs: r = std::fabs(a[0]); break;
          case FSign: r = a[0] > 0 ? 1.0 : (a[0] < 0 ? -1.0 : 0.0); break;
          case FExp: r = std::exp(std::min(a[0], 700.0)); break;
          case FLog: r = a[0] > 0 ? std::log(a[0]) : -1e9; break;
          case FLog2: r = a[0] > 0 ? std::log2(a[0]) : -1e9; break;
          case FLog10: r = a[0] > 0 ? std::log10(a[0]) : -1e9; break;
          case FPow: r = std::pow(a[0], a[1]); break;
          case FMin: r = std::min(a[0], a[1]); break;
          case FMax: r = std::max(a[0], a[1]); break;
          case FFloor: r = std::floor(a[0]); break;
          case FCeil: r = std::ceil(a[0]); break;
          case FRound: r = std::round(a[0]); break;
          case FTrunc: r = std::trunc(a[0]); break;
          case FFrac: r = frac(a[0]); break;
          case FFmod: r = a[1] != 0 ? std::fmod(a[0], a[1]) : 0.0; break;
          case FClamp: r = std::clamp(a[0], std::min(a[1], a[2]), std::max(a[1], a[2])); break;
          case FLerp: r = a[0] + (a[1] - a[0]) * a[2]; break;
          case FStep: r = a[1] >= a[0] ? 1.0 : 0.0; break;
          case FSmooth: {
            double d = a[1] - a[0];
            double t = d != 0 ? std::clamp((a[2] - a[0]) / d, 0.0, 1.0) : (a[2] >= a[0] ? 1.0 : 0.0);
            r = t * t * (3 - 2 * t);
            break;
          }
          case FSaw: r = frac(a[0]); break;
          case FTri: r = 1.0 - std::fabs(2.0 * frac(a[0]) - 1.0); break;
          case FSqr: r = frac(a[0]) < 0.5 ? 1.0 : 0.0; break;
          case FPulse: r = frac(a[0]) < a[1] ? 1.0 : 0.0; break;
          case FWrap: r = frac(a[0]); break;
          case FFold: r = 1.0 - std::fabs(2.0 * frac(a[0] * 0.5) - 1.0); break;
          case FQuant: r = a[1] > 0 ? std::round(a[0] * a[1]) / a[1] : a[0]; break;
          case FRand:
            if (argc == 1) {
              r = static_cast<double>(hash32(static_cast<uint32_t>(static_cast<int64_t>(std::floor(a[0])))) & 0xFFFFFF) / 16777215.0;
            } else if (rng) {
              *rng = *rng * 1664525u + 1013904223u;
              r = static_cast<double>(*rng >> 8) / 16777215.0;
            } else {
              r = 0.5;
            }
            break;
          case FDeg: r = a[0] * 57.29577951308232; break;
          case FRad: r = a[0] * 0.017453292519943295; break;
          case FTension: r = std::pow(std::clamp(a[0], 0.0, 1.0), std::pow(8.0, std::clamp(a[1], -1.0, 1.0))); break;
          default: r = 0; break;
        }
        sp -= argc;
        st[sp++] = finite(r);
        break;
      }
      default: {
        double b = st[--sp];
        double& a = st[sp - 1];
        switch (o.code) {
          case OAdd: a += b; break;
          case OSub: a -= b; break;
          case OMul: a *= b; break;
          case ODiv: a = b != 0 ? a / b : 0.0; break;
          case OMod: a = b != 0 ? std::fmod(a, b) : 0.0; break;
          case OPow: a = finite(std::pow(a, b)); break;
          case OLt: a = a < b ? 1.0 : 0.0; break;
          case OLe: a = a <= b ? 1.0 : 0.0; break;
          case OGt: a = a > b ? 1.0 : 0.0; break;
          case OGe: a = a >= b ? 1.0 : 0.0; break;
          case OEq: a = std::fabs(a - b) < 1e-9 ? 1.0 : 0.0; break;
          case ONe: a = std::fabs(a - b) >= 1e-9 ? 1.0 : 0.0; break;
          case OAnd: a = (a != 0.0 && b != 0.0) ? 1.0 : 0.0; break;
          case OOr: a = (a != 0.0 || b != 0.0) ? 1.0 : 0.0; break;
          default: break;
        }
        break;
      }
    }
  }
  return finite(st[0]);
}

}  // namespace gl
