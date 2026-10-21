#include "Json.h"
#include "Platform.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gl {
namespace {
struct P {
  const std::string& s;
  size_t i = 0;
  bool fail = false;

  void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
  bool lit(const char* w) {
    size_t n = std::strlen(w);
    if (s.compare(i, n, w) == 0) { i += n; return true; }
    return false;
  }
  bool value(Json& out) {
    ws();
    if (i >= s.size()) { fail = true; return false; }
    char c = s[i];
    if (c == '{') return objectV(out);
    if (c == '[') return arrayV(out);
    if (c == '"') { out.type = Json::Type::String; return stringV(out.str); }
    if (c == 't') { if (lit("true")) { out.type = Json::Type::Bool; out.boolean = true; return true; } fail = true; return false; }
    if (c == 'f') { if (lit("false")) { out.type = Json::Type::Bool; out.boolean = false; return true; } fail = true; return false; }
    if (c == 'n') { if (lit("null")) { out.type = Json::Type::Null; return true; } fail = true; return false; }
    return numberV(out);
  }
  bool stringV(std::string& out) {
    if (s[i] != '"') { fail = true; return false; }
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c == '\\' && i < s.size()) {
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            if (i + 4 > s.size()) { fail = true; return false; }
            unsigned cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s[i++];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= h - '0';
              else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
              else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
              else { fail = true; return false; }
            }
            // UTF-8 encode (BMP only; surrogate pairs collapse to '?')
            if (cp >= 0xD800 && cp <= 0xDFFF) { out += '?'; }
            else if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) {
              out += static_cast<char>(0xC0 | (cp >> 6));
              out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
              out += static_cast<char>(0xE0 | (cp >> 12));
              out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
              out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
          }
          default: out += e;
        }
      } else {
        out += c;
      }
    }
    if (i >= s.size()) { fail = true; return false; }
    ++i;  // closing quote
    return true;
  }
  bool numberV(Json& out) {
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    bool any = false;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                            s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+')) {
      ++i; any = true;
    }
    if (!any) { fail = true; return false; }
    // strtod, not std::stod: stod throws on ERANGE, which libc++ also reports
    // for subnormal results (e.g. a denormal parameter value that was saved).
    const std::string tok = s.substr(start, i - start);
    char* end = nullptr;
    double v = platform::parseDouble(tok.c_str(), &end);  // '.' whatever the locale
    if (end != tok.c_str() + tok.size() || !std::isfinite(v)) { fail = true; return false; }
    out.type = Json::Type::Number;
    out.number = v;
    return true;
  }
  bool arrayV(Json& out) {
    ++i;  // [
    out.type = Json::Type::Array;
    ws();
    if (i < s.size() && s[i] == ']') { ++i; return true; }
    while (i < s.size()) {
      Json v;
      if (!value(v)) return false;
      out.array.push_back(std::move(v));
      ws();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == ']') { ++i; return true; }
      fail = true; return false;
    }
    fail = true; return false;
  }
  bool objectV(Json& out) {
    ++i;  // {
    out.type = Json::Type::Object;
    ws();
    if (i < s.size() && s[i] == '}') { ++i; return true; }
    while (i < s.size()) {
      ws();
      std::string key;
      if (!stringV(key)) return false;
      ws();
      if (i >= s.size() || s[i] != ':') { fail = true; return false; }
      ++i;
      Json v;
      if (!value(v)) return false;
      out.object.emplace(std::move(key), std::move(v));
      ws();
      if (i < s.size() && s[i] == ',') { ++i; continue; }
      if (i < s.size() && s[i] == '}') { ++i; return true; }
      fail = true; return false;
    }
    fail = true; return false;
  }
};

void emit(const Json& v, std::string& out, int indent, int depth) {
  auto pad = [&](int n) { out.append(static_cast<size_t>(n) * indent, ' '); };
  switch (v.type) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += v.boolean ? "true" : "false"; break;
    case Json::Type::Number: {
      char buf[32];
      if (!std::isfinite(v.number))  // not representable in JSON: readers fall back to their default
        std::snprintf(buf, sizeof buf, "null");
      else if (std::fabs(v.number) < 1e15 && v.number == static_cast<double>(static_cast<long long>(v.number)))
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v.number));
      else
        std::snprintf(buf, sizeof buf, "%s", platform::formatDouble("%.17g", v.number).c_str());  // round-trips exactly, '.' always
      out += buf;
      break;
    }
    case Json::Type::String: {
      out += '"';
      for (char c : v.str) {
        switch (c) {
          case '"': out += "\\\""; break;
          case '\\': out += "\\\\"; break;
          case '\n': out += "\\n"; break;
          case '\t': out += "\\t"; break;
          case '\r': out += "\\r"; break;
          default:
            if (static_cast<unsigned char>(c) < 0x20) {  // other control characters must be escaped in JSON
              char esc[8];
              std::snprintf(esc, sizeof esc, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
              out += esc;
            } else {
              out += c;
            }
        }
      }
      out += '"';
      break;
    }
    case Json::Type::Array: {
      out += "[";
      if (!v.array.empty()) {
        out += "\n";
        for (size_t k = 0; k < v.array.size(); ++k) {
          pad(depth + 1);
          emit(v.array[k], out, indent, depth + 1);
          if (k + 1 < v.array.size()) out += ",";
          out += "\n";
        }
        pad(depth);
      }
      out += "]";
      break;
    }
    case Json::Type::Object: {
      out += "{";
      if (!v.object.empty()) {
        out += "\n";
        size_t k = 0;
        for (const auto& [key, val] : v.object) {
          pad(depth + 1);
          out += '"'; out += key; out += "\": ";
          emit(val, out, indent, depth + 1);
          if (++k < v.object.size()) out += ",";
          out += "\n";
        }
        pad(depth);
      }
      out += "}";
      break;
    }
  }
}
}  // namespace

bool jsonParse(const std::string& src, Json& out, size_t* errPos) {
  P p{src};
  bool ok = p.value(out);
  p.ws();
  if (ok && p.i != src.size()) ok = false;
  if (!ok && errPos) *errPos = p.i;
  return ok;
}

std::string jsonStringify(const Json& v, int indent) {
  std::string out;
  emit(v, out, indent, 0);
  out += "\n";
  return out;
}

}  // namespace gl
