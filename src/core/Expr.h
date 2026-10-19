// Gridline — small math expression language (formula controller, link mapping
// formulas, note scripts). Compiled once on the UI thread into a flat stack
// program; evaluation is allocation-free and realtime-safe.
//
// Syntax:  numbers, variables (names given at compile time), pi, e,
//          + - * / % ^ (power, right-assoc), unary - + !, comparisons
//          < <= > >= == != (yield 0/1), && || (and/or), c ? a : b.
// Functions (case-insensitive):
//   sin cos tan asin acos atan atan2(y,x) sinh cosh tanh sqrt abs sign exp
//   log/ln log2 log10 pow(x,y) min(..) max(..) floor ceil round trunc frac
//   fmod/mod(x,y) clamp(x,lo,hi) lerp(a,b,t) step(edge,x) smoothstep(e0,e1,x)
//   if(c,a,b) saw(x) tri(x) sqr(x) pulse(x,w) wrap(x) fold(x) quant(x,n)
//   rand() rand(seed) deg(x) rad(x) tension(x,t) (bend 0..1 by t in -1..1)
// Waveforms take a phase in cycles and return 0..1 (saw ramps up, tri peaks at
// half a cycle, sqr is 1 for the first half). rand() draws a new value per
// evaluation from the caller's seeded state; rand(seed) is a fixed hash.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl {

class Expr {
 public:
  static constexpr int kMaxStack = 48;

  // Compiles `text`; variable i of `vars` is read from vars[i] at evaluation.
  // On failure returns false, leaves the expression empty (evaluates to 0)
  // and fills `err` with a readable message.
  bool compile(const std::string& text, const std::vector<std::string>& vars, std::string* err = nullptr);
  bool empty() const { return ops_.empty(); }
  // Realtime-safe evaluation. `rng` is the state for rand(); may be null (rand() = 0.5).
  double eval(const double* vars, uint32_t* rng = nullptr) const;
  // Number of variables the expression was compiled against.
  size_t varCount() const { return nvars_; }
  // True when the expression reads variable i.
  bool uses(size_t var) const;

  struct Op {
    uint8_t code = 0;
    uint8_t argc = 0;
    int index = 0;
    double value = 0;
  };

 private:
  std::vector<Op> ops_;
  size_t nvars_ = 0;
};

}  // namespace gl
