// Gridline — minimal JSON value, parser, and serializer (engine subset).
#pragma once
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

// Parses a JSON document; returns false on syntax error (pos reports offset).
bool jsonParse(const std::string& src, Json& out, size_t* errPos = nullptr);
// Serializes with deterministic key order (std::map) and stable formatting.
std::string jsonStringify(const Json& v, int indent = 2);

}  // namespace gl
