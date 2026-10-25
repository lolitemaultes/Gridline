// Gridline — minimal JSON value, parser, and serializer (engine subset).
#pragma once
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gl {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string str;
  std::vector<Json> array;
  std::map<std::string, Json> object;

  static Json makeObject() { Json j; j.type = Type::Object; return j; }
  static Json makeArray() { Json j; j.type = Type::Array; return j; }
  static Json makeNumber(double v) { Json j; j.type = Type::Number; j.number = v; return j; }
  static Json makeString(const std::string& v) { Json j; j.type = Type::String; j.str = v; return j; }

  bool is(Type t) const { return type == t; }
  const Json* find(const std::string& key) const {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }
  double num(const std::string& key, double dflt = 0.0) const {
    const Json* v = find(key);
    return v && v->is(Type::Number) ? v->number : dflt;
  }
  std::string text(const std::string& key, const std::string& dflt = "") const {
    const Json* v = find(key);
    return v && v->is(Type::String) ? v->str : dflt;
  }
};

// Saturating conversion of a JSON number (untrusted file data) to an integer
// type: out-of-range values clamp, NaN gives 0. A plain static_cast of an
// out-of-range double is undefined behaviour.
template <class T>
T jsonInt(double v) {
  if (std::isnan(v)) return T(0);
  constexpr double lo = static_cast<double>(std::numeric_limits<T>::lowest());
  constexpr double hi = static_cast<double>(std::numeric_limits<T>::max());
  if (v <= lo) return std::numeric_limits<T>::lowest();
  if (v >= hi) return std::numeric_limits<T>::max();  // hi may round up (int64): >= keeps the cast in range
  return static_cast<T>(v);
}

// Parses a JSON document; returns false on syntax error (pos reports offset).
bool jsonParse(const std::string& src, Json& out, size_t* errPos = nullptr);
// Serializes with deterministic key order (std::map) and stable formatting.
std::string jsonStringify(const Json& v, int indent = 2);
// J-111: one line, no spaces, keys escaped too (wire formats, signatures, codes).
std::string jsonCompact(const Json& v);

}  // namespace gl
