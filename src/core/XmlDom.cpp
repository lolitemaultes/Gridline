// Gridline — small XML tree. See XmlDom.h.
#include "XmlDom.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "Platform.h"

namespace gl::xml {

const std::string* Node::attr(const std::string& key) const {
  for (const auto& [k, v] : attrs)
    if (k == key) return &v;
  return nullptr;
}
std::string Node::get(const std::string& key, const std::string& dflt) const {
  const std::string* v = attr(key);
  return v ? *v : dflt;
}
double Node::num(const std::string& key, double dflt) const {
  const std::string* v = attr(key);
  if (!v || v->empty()) return dflt;
  const double x = platform::parseDouble(v->c_str());
  return std::isfinite(x) ? x : dflt;
}
bool Node::flag(const std::string& key, bool dflt) const {
  const std::string* v = attr(key);
  if (!v) return dflt;
  return *v == "true" || *v == "1";
}
void Node::set(const std::string& key, const std::string& value) {
  for (auto& [k, v] : attrs)
    if (k == key) {
      v = value;
      return;
    }
  attrs.emplace_back(key, value);
}
void Node::set(const std::string& key, double value) { set(key, numText(value)); }
const Node* Node::child(const std::string& n) const {
  for (const auto& c : children)
    if (c.name == n) return &c;
  return nullptr;
}
Node* Node::child(const std::string& n) {
  for (auto& c : children)
    if (c.name == n) return &c;
  return nullptr;
}
std::vector<const Node*> Node::all(const std::string& n) const {
  std::vector<const Node*> out;
  for (const auto& c : children)
    if (c.name == n) out.push_back(&c);
  return out;
}
Node& Node::add(const std::string& n) {
  children.emplace_back();
  children.back().name = n;
  return children.back();
}

namespace {

void utf8(std::string& out, unsigned long cp) {
  if (cp < 0x80) out += static_cast<char>(cp);
  else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
  else if (cp < 0x10000)
    out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
  else
    out += static_cast<char>(0xF0 | (cp >> 18)), out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
}

std::string unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const size_t semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out += s[i];
      continue;
    }
    const std::string e = s.substr(i + 1, semi - i - 1);
    if (e == "lt") out += '<';
    else if (e == "gt") out += '>';
    else if (e == "amp") out += '&';
    else if (e == "quot") out += '"';
    else if (e == "apos") out += '\'';
    else if (!e.empty() && e[0] == '#') {
      const unsigned long cp = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtoul(e.c_str() + 2, nullptr, 16) : std::strtoul(e.c_str() + 1, nullptr, 10);
      utf8(out, cp);
    } else {
      out += s.substr(i, semi - i + 1);
    }
    i = semi;
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

struct Parser {
  explicit Parser(const std::string& text) : s(text) {}
  const std::string& s;
  size_t p = 0;
  std::string err;
  int depth = 0;

  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at byte " + std::to_string(p);
    return false;
  }
  void skipSpace() {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
  }
  bool starts(const char* t) const { return s.compare(p, std::char_traits<char>::length(t), t) == 0; }
  // Comments, processing instructions and a doctype.
  bool skipMisc() {
    for (;;) {
      skipSpace();
      if (starts("<?")) {
        const size_t e = s.find("?>", p);
        if (e == std::string::npos) return fail("unclosed declaration");
        p = e + 2;
      } else if (starts("<!--")) {
        const size_t e = s.find("-->", p);
        if (e == std::string::npos) return fail("unclosed comment");
        p = e + 3;
      } else if (starts("<!DOCTYPE") || starts("<!doctype")) {
        const size_t e = s.find('>', p);
        if (e == std::string::npos) return fail("unclosed doctype");
        p = e + 1;
      } else {
        return true;
      }
    }
  }
  std::string name() {
    const size_t a = p;
    while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) && s[p] != '>' && s[p] != '/' && s[p] != '=') ++p;
    return s.substr(a, p - a);
  }
  bool element(Node& n) {
    if (++depth > 256) return fail("nested too deeply");
    if (p >= s.size() || s[p] != '<') return fail("expected an element");
    ++p;
    n.name = name();
    if (n.name.empty()) return fail("an element without a name");
    for (;;) {
      skipSpace();
      if (p >= s.size()) return fail("unclosed tag");
      if (s[p] == '/') {
        if (p + 1 >= s.size() || s[p + 1] != '>') return fail("bad empty tag");
        p += 2;
        --depth;
        return true;
      }
      if (s[p] == '>') {
        ++p;
        break;
      }
      std::string k = name();
      skipSpace();
      if (k.empty() || p >= s.size() || s[p] != '=') return fail("bad attribute");
      ++p;
      skipSpace();
      if (p >= s.size() || (s[p] != '"' && s[p] != '\'')) return fail("unquoted attribute");
      const char q = s[p++];
      const size_t e = s.find(q, p);
      if (e == std::string::npos) return fail("unclosed attribute");
      n.attrs.emplace_back(std::move(k), unescape(s.substr(p, e - p)));
      p = e + 1;
    }
    std::string text;
    for (;;) {
      if (p >= s.size()) return fail("unclosed element");
      if (starts("</")) {
        p += 2;
        const std::string close = name();
        skipSpace();
        if (close != n.name || p >= s.size() || s[p] != '>') return fail("mismatched closing tag");
        ++p;
        n.text = trim(text);
        --depth;
        return true;
      }
      if (starts("<!--")) {
        const size_t e = s.find("-->", p);
        if (e == std::string::npos) return fail("unclosed comment");
        p = e + 3;
        continue;
      }
      if (starts("<![CDATA[")) {
        const size_t e = s.find("]]>", p);
        if (e == std::string::npos) return fail("unclosed CDATA");
        text += s.substr(p + 9, e - p - 9);
        p = e + 3;
        continue;
      }
      if (starts("<?")) {
        const size_t e = s.find("?>", p);
        if (e == std::string::npos) return fail("unclosed instruction");
        p = e + 2;
        continue;
      }
      if (s[p] == '<') {
        n.children.emplace_back();
        if (!element(n.children.back())) return false;
        continue;
      }
      const size_t e = s.find('<', p);
      const size_t end = e == std::string::npos ? s.size() : e;
      text += unescape(s.substr(p, end - p));
      p = end;
    }
  }
};

void put(std::string& out, const Node& n, int indent) {
  out.append(static_cast<size_t>(indent) * 2, ' ');
  out += '<' + n.name;
  for (const auto& [k, v] : n.attrs) out += ' ' + k + "=\"" + escape(v) + '"';
  if (n.children.empty() && n.text.empty()) {
    out += "/>\n";
    return;
  }
  out += '>';
  if (n.children.empty()) {
    out += escape(n.text) + "</" + n.name + ">\n";
    return;
  }
  out += '\n';
  if (!n.text.empty()) out.append(static_cast<size_t>(indent + 1) * 2, ' '), out += escape(n.text) + '\n';
  for (const auto& c : n.children) put(out, c, indent + 1);
  out.append(static_cast<size_t>(indent) * 2, ' ');
  out += "</" + n.name + ">\n";
}

}  // namespace

bool parse(const std::string& text, Node& root, std::string* err) {
  Parser ps(text);
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF)
    ps.p = 3;
  root = Node();
  if (!ps.skipMisc() || !ps.element(root)) {
    if (err) *err = ps.err;
    return false;
  }
  return true;
}

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '&': out += "&amp;"; break;
      case '"': out += "&quot;"; break;
      case '\n': out += "&#10;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string numText(double v) {
  if (!std::isfinite(v)) v = 0;
  if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(std::llround(v)));
  return platform::formatDouble("%.9g", v);
}

std::string write(const Node& root) {
  std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  put(out, root, 0);
  return out;
}

}  // namespace gl::xml
