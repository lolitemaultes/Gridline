#include "Turtle.h"
#include "../../core/Platform.h"
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gl::host::lv2 {

namespace {
const char kRdf[] = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
const char kXsd[] = "http://www.w3.org/2001/XMLSchema#";

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int hexVal(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

void appendUtf8(std::string& s, uint32_t cp) {
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool nameStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool nameChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c >= 0x80; }
}  // namespace

// ---------------------------------------------------------------- nodes

double Node::number(double def) const {
  if (kind != Literal || value.empty()) return def;
  if (value == "true") return 1;
  if (value == "false") return 0;
  char* end = nullptr;
  double v = platform::parseDouble(value.c_str(), &end);
  return end && end != value.c_str() ? v : def;
}

bool Node::truth() const { return kind == Literal && (value == "true" || value == "1"); }

// ---------------------------------------------------------------- IRIs

std::string fileUri(const std::string& path) {
  std::string out = "file://";
  for (unsigned char c : path) {
    if (std::isalnum(c) || std::strchr("/-._~", c)) {
      out += static_cast<char>(c);
    } else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      out += b;
    }
  }
  return out;
}

bool uriToPath(const std::string& uri, std::string& path) {
  if (uri.compare(0, 7, "file://") != 0) return false;
  size_t start = 7;
  if (uri.compare(7, 9, "localhost") == 0) start = 16;
  path.clear();
  for (size_t i = start; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() && isHex(uri[i + 1]) && isHex(uri[i + 2])) {
      path += static_cast<char>(hexVal(uri[i + 1]) * 16 + hexVal(uri[i + 2]));
      i += 2;
    } else if (uri[i] == '#' || uri[i] == '?') {
      break;
    } else {
      path += uri[i];
    }
  }
  return !path.empty();
}

namespace {
bool hasScheme(const std::string& s) {
  if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s) {
    if (c == ':') return true;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return false;
  }
  return false;
}
// Removes "." and ".." segments of a path.
std::string removeDots(const std::string& p) {
  std::vector<std::string> segs;
  size_t i = 0;
  const bool abs = !p.empty() && p[0] == '/';
  std::vector<std::string> parts;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    parts.push_back(p.substr(i, j - i));
    i = j + 1;
  }
  for (size_t k = abs ? 1 : 0; k < parts.size(); ++k) {
    const std::string& s = parts[k];
    const bool last = k + 1 == parts.size();
    if (s == ".") {
      if (last) segs.push_back("");
    } else if (s == "..") {
      if (!segs.empty()) segs.pop_back();
      if (last) segs.push_back("");
    } else {
      segs.push_back(s);
    }
  }
  std::string out = abs ? "/" : "";
  for (size_t k = 0; k < segs.size(); ++k) out += (k ? "/" : "") + segs[k];
  return out;
}
}  // namespace

std::string resolveIri(const std::string& base, const std::string& ref) {
  if (hasScheme(ref)) return ref;
  if (base.empty()) return ref;
  const size_t colon = base.find(':');
  const std::string scheme = base.substr(0, colon + 1);
  std::string rest = base.substr(colon + 1);
  std::string authority;
  if (rest.compare(0, 2, "//") == 0) {
    size_t e = rest.find('/', 2);
    if (e == std::string::npos) e = rest.size();
    authority = rest.substr(0, e);
    rest = rest.substr(e);
  }
  std::string path = rest;
  size_t cut = path.find_first_of("?#");
  std::string query;
  if (cut != std::string::npos) {
    query = path.substr(cut);
    path = path.substr(0, cut);
  }
  if (ref.empty()) return scheme + authority + path + (query.size() && query[0] == '?' ? query : "");
  if (ref[0] == '#') return scheme + authority + path + ref;
  if (ref.compare(0, 2, "//") == 0) return scheme + ref;
  if (ref[0] == '?') return scheme + authority + path + ref;
  std::string refPath = ref, tail;
  size_t rc = refPath.find_first_of("?#");
  if (rc != std::string::npos) {
    tail = refPath.substr(rc);
    refPath = refPath.substr(0, rc);
  }
  std::string merged;
  if (!refPath.empty() && refPath[0] == '/') {
    merged = refPath;
  } else {
    size_t slash = path.rfind('/');
    merged = (slash == std::string::npos ? std::string(authority.empty() ? "" : "/") : path.substr(0, slash + 1)) + refPath;
  }
  return scheme + authority + removeDots(merged) + tail;
}

// ---------------------------------------------------------------- parser

class Parser {
 public:
  Parser(Model& m, const std::string& text, const std::string& base) : m_(m), s_(text), base_(base) {
    doc_ = "b" + std::to_string(m.docs_++) + ".";
  }

  bool run(std::string* err) {
    try {
      for (;;) {
        ws();
        if (eof()) break;
        statement();
      }
    } catch (const std::string& e) {
      if (err) *err = e + " (line " + std::to_string(line()) + ")";
      return false;
    }
    return true;
  }

 private:
  Model& m_;
  const std::string& s_;
  std::string base_;
  std::string doc_;
  size_t i_ = 0;
  std::unordered_map<std::string, std::string> prefixes_;
  int anon_ = 0;

  [[noreturn]] void fail(const std::string& what) { throw what; }
  bool eof() const { return i_ >= s_.size(); }
  char peek(size_t k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
  int line() const {
    int n = 1;
    for (size_t k = 0; k < i_ && k < s_.size(); ++k) n += s_[k] == '\n';
    return n;
  }
  void ws() {
    while (!eof()) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++i_;
      } else if (c == '#') {
        while (!eof() && s_[i_] != '\n') ++i_;
      } else {
        break;
      }
    }
  }
  void expect(char c) {
    ws();
    if (peek() != c) fail(std::string("expected '") + c + "'");
    ++i_;
  }
  bool keyword(const char* kw, bool caseInsensitive) {
    size_t n = std::strlen(kw);
    if (i_ + n > s_.size()) return false;
    for (size_t k = 0; k < n; ++k) {
      char a = s_[i_ + k], b = kw[k];
      if (caseInsensitive ? std::tolower(static_cast<unsigned char>(a)) != std::tolower(static_cast<unsigned char>(b)) : a != b) return false;
    }
    char after = i_ + n < s_.size() ? s_[i_ + n] : ' ';
    if (nameChar(static_cast<unsigned char>(after)) || after == ':') return false;
    i_ += n;
    return true;
  }

  void statement() {
    if (peek() == '@') {
      ++i_;
      if (keyword("prefix", false)) {
        prefixDecl();
        expect('.');
      } else if (keyword("base", false)) {
        ws();
        base_ = iriRef();
        expect('.');
      } else {
        fail("unknown directive");
      }
      return;
    }
    {
      size_t save = i_;
      if (keyword("PREFIX", true)) {
        prefixDecl();
        return;
      }
      if (keyword("BASE", true)) {
        ws();
        base_ = iriRef();
        return;
      }
      i_ = save;
    }
    // triples
    Node subj;
    ws();
    if (peek() == '[') {
      ++i_;
      ws();
      subj = blank();
      if (peek() != ']') predicateObjectList(subj);
      expect(']');
      ws();
      if (peek() != '.') predicateObjectList(subj);
    } else {
      subj = subject();
      predicateObjectList(subj);
    }
    expect('.');
  }

  void prefixDecl() {
    ws();
    std::string name;
    while (!eof() && s_[i_] != ':') {
      char c = s_[i_];
      if (!nameChar(static_cast<unsigned char>(c)) && c != '.') fail("bad prefix name");
      name += c;
      ++i_;
    }
    if (eof()) fail("bad prefix");
    ++i_;  // ':'
    ws();
    prefixes_[name] = iriRef();
  }

  Node blank() {
    Node n;
    n.kind = Node::Blank;
    n.value = doc_ + "anon" + std::to_string(anon_++);
    return n;
  }

  Node subject() {
    ws();
    char c = peek();
    if (c == '_' && peek(1) == ':') return blankLabel();
    if (c == '<' || nameStart(static_cast<unsigned char>(c)) || c == ':') {
      Node n;
      n.kind = Node::Iri;
      n.value = iri();
      return n;
    }
    if (c == '(') return collection();
    fail("expected a subject");
  }

  Node blankLabel() {
    i_ += 2;
    std::string label;
    while (!eof() && (nameChar(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.')) label += s_[i_++];
    while (!label.empty() && label.back() == '.') {
      label.pop_back();
      --i_;
    }
    Node n;
    n.kind = Node::Blank;
    n.value = doc_ + "l." + label;
    return n;
  }

  void predicateObjectList(const Node& subj) {
    for (;;) {
      ws();
      std::string pred = verb();
      objectList(subj, pred);
      ws();
      if (peek() != ';') return;
      while (peek() == ';') {
        ++i_;
        ws();
      }
      char c = peek();
      if (c == '.' || c == ']' || eof()) return;
    }
  }

  std::string verb() {
    ws();
    if (peek() == 'a') {
      char after = peek(1);
      if (!nameChar(static_cast<unsigned char>(after)) && after != ':') {
        ++i_;
        return std::string(kRdf) + "type";
      }
    }
    return iri();
  }

  void objectList(const Node& subj, const std::string& pred) {
    for (;;) {
      Node o = object();
      m_.add(subj, pred, std::move(o));
      ws();
      if (peek() != ',') return;
      ++i_;
    }
  }

  Node object() {
    ws();
    char c = peek();
    if (c == '<') {
      Node n;
      n.kind = Node::Iri;
      n.value = iri();
      return n;
    }
    if (c == '_' && peek(1) == ':') return blankLabel();
    if (c == '[') {
      ++i_;
      Node b = blank();
      ws();
      if (peek() != ']') predicateObjectList(b);
      expect(']');
      return b;
    }
    if (c == '(') return collection();
    if (c == '"' || c == '\'') return stringLiteral();
    if (c == '+' || c == '-' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) return numeric();
    for (const char* b : {"true", "false"})
      if (keyword(b, false)) {
        Node n;
        n.kind = Node::Literal;
        n.value = b;
        n.datatype = std::string(kXsd) + "boolean";
        return n;
      }
    Node n;
    n.kind = Node::Iri;
    n.value = iri();
    return n;
  }

  Node collection() {
    expect('(');
    std::vector<Node> items;
    for (;;) {
      ws();
      if (peek() == ')') {
        ++i_;
        break;
      }
      if (eof()) fail("unterminated collection");
      items.push_back(object());
    }
    Node nil;
    nil.kind = Node::Iri;
    nil.value = std::string(kRdf) + "nil";
    if (items.empty()) return nil;
    Node head = blank(), cur = head;
    for (size_t k = 0; k < items.size(); ++k) {
      m_.add(cur, std::string(kRdf) + "first", items[k]);
      Node next = k + 1 < items.size() ? blank() : nil;
      m_.add(cur, std::string(kRdf) + "rest", next);
      cur = next;
    }
    return head;
  }

  std::string iri() {
    ws();
    if (peek() == '<') return iriRef();
    // prefixed name
    std::string pfx;
    while (!eof() && s_[i_] != ':') {
      char c = s_[i_];
      if (!nameChar(static_cast<unsigned char>(c)) && c != '.') fail("expected an IRI");
      pfx += c;
      ++i_;
    }
    if (eof()) fail("expected an IRI");
    ++i_;  // ':'
    std::string local;
    while (!eof()) {
      char c = s_[i_];
      if (nameChar(static_cast<unsigned char>(c)) || c == '.' || c == ':') {
        local += c;
        ++i_;
      } else if (c == '%' && isHex(peek(1)) && isHex(peek(2))) {
        local.append(s_, i_, 3);
        i_ += 3;
      } else if (c == '\\' && i_ + 1 < s_.size()) {
        local += s_[i_ + 1];
        i_ += 2;
      } else {
        break;
      }
    }
    while (!local.empty() && local.back() == '.') {
      local.pop_back();
      --i_;
    }
    auto it = prefixes_.find(pfx);
    if (it == prefixes_.end()) fail("undefined prefix '" + pfx + "'");
    return it->second + local;
  }

  std::string iriRef() {
    if (peek() != '<') fail("expected '<'");
    ++i_;
    std::string v;
    while (!eof() && s_[i_] != '>') {
      char c = s_[i_];
      if (c == '\\' && (peek(1) == 'u' || peek(1) == 'U')) {
        i_ += 2;
        v += unicodeEscape(s_[i_ - 1] == 'u' ? 4 : 8);
        continue;
      }
      if (c == '\n') fail("newline in IRI");
      v += c;
      ++i_;
    }
    if (eof()) fail("unterminated IRI");
    ++i_;
    return resolveIri(base_, v);
  }

  std::string unicodeEscape(int digits) {
    uint32_t cp = 0;
    for (int k = 0; k < digits; ++k) {
      if (!isHex(peek())) fail("bad unicode escape");
      cp = cp * 16 + static_cast<uint32_t>(hexVal(s_[i_++]));
    }
    std::string out;
    appendUtf8(out, cp);
    return out;
  }

  Node stringLiteral() {
    const char q = s_[i_];
    const bool longForm = peek(1) == q && peek(2) == q;
    i_ += longForm ? 3 : 1;
    std::string v;
    for (;;) {
      if (eof()) fail("unterminated string");
      char c = s_[i_];
      if (longForm) {
        if (c == q && peek(1) == q && peek(2) == q) {
          i_ += 3;
          // more quotes directly after belong to the content ("""a"""" ends with a quote)
          while (peek() == q) {
            v += q;
            ++i_;
          }
          break;
        }
      } else {
        if (c == q) {
          ++i_;
          break;
        }
        if (c == '\n' || c == '\r') fail("newline in string");
      }
      if (c == '\\') {
        char e = peek(1);
        i_ += 2;
        switch (e) {
          case 't': v += '\t'; break;
          case 'b': v += '\b'; break;
          case 'n': v += '\n'; break;
          case 'r': v += '\r'; break;
          case 'f': v += '\f'; break;
          case '"': v += '"'; break;
          case '\'': v += '\''; break;
          case '\\': v += '\\'; break;
          case 'u': v += unicodeEscape(4); break;
          case 'U': v += unicodeEscape(8); break;
          default: fail("bad escape");
        }
        continue;
      }
      v += c;
      ++i_;
    }
    Node n;
    n.kind = Node::Literal;
    n.value = std::move(v);
    if (peek() == '@') {
      ++i_;
      while (!eof() && (std::isalnum(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-')) n.lang += s_[i_++];
    } else if (peek() == '^' && peek(1) == '^') {
      i_ += 2;
      n.datatype = iri();
    }
    return n;
  }

  Node numeric() {
    std::string v;
    if (peek() == '+' || peek() == '-') v += s_[i_++];
    bool digits = false, dot = false, exp = false;
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
      v += s_[i_++];
      digits = true;
    }
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
      dot = true;
      v += s_[i_++];
      while (std::isdigit(static_cast<unsigned char>(peek()))) v += s_[i_++];
      digits = true;
    }
    if (digits && (peek() == 'e' || peek() == 'E')) {
      exp = true;
      v += s_[i_++];
      if (peek() == '+' || peek() == '-') v += s_[i_++];
      if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("bad exponent");
      while (std::isdigit(static_cast<unsigned char>(peek()))) v += s_[i_++];
    }
    if (!digits) fail("bad number");
    Node n;
    n.kind = Node::Literal;
    n.value = v;
    n.datatype = std::string(kXsd) + (exp ? "double" : dot ? "decimal" : "integer");
    return n;
  }
};

// ---------------------------------------------------------------- model

void Model::add(Node s, std::string p, Node o) {
  bySubject_[s.key()].push_back(triples_.size());
  triples_.push_back({std::move(s), std::move(p), std::move(o)});
}

bool Model::loadString(const std::string& text, const std::string& baseIri, std::string* err) {
  Parser p(*this, text, baseIri);
  return p.run(err);
}

bool Model::loadFile(const std::string& path, std::string* err) {
  if (loaded(path)) return true;
  files_.push_back(path);
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot read " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string e;
  if (!loadString(ss.str(), fileUri(path), &e)) {
    if (err) *err = path + ": " + e;
    return false;
  }
  return true;
}

bool Model::loaded(const std::string& path) const {
  for (const auto& f : files_)
    if (f == path) return true;
  return false;
}

std::vector<const Node*> Model::objects(const std::string& s, const std::string& p) const {
  std::vector<const Node*> out;
  auto it = bySubject_.find(s);
  if (it == bySubject_.end()) return out;
  for (size_t i : it->second)
    if (triples_[i].p == p) out.push_back(&triples_[i].o);
  return out;
}

const Node* Model::object(const std::string& s, const std::string& p) const {
  auto it = bySubject_.find(s);
  if (it == bySubject_.end()) return nullptr;
  for (size_t i : it->second)
    if (triples_[i].p == p) return &triples_[i].o;
  return nullptr;
}

std::string Model::text(const std::string& s, const std::string& p) const {
  const Node* n = object(s, p);
  return n ? n->value : std::string();
}

bool Model::has(const std::string& s, const std::string& p, const std::string& oIri) const {
  auto it = bySubject_.find(s);
  if (it == bySubject_.end()) return false;
  for (size_t i : it->second)
    if (triples_[i].p == p && triples_[i].o.kind == Node::Iri && triples_[i].o.value == oIri) return true;
  return false;
}

std::vector<std::string> Model::subjects(const std::string& p, const std::string& oIri) const {
  std::vector<std::string> out;
  for (const auto& t : triples_)
    if (t.p == p && t.o.kind == Node::Iri && t.o.value == oIri) {
      std::string k = t.s.key();
      bool dup = false;
      for (const auto& x : out) dup = dup || x == k;
      if (!dup) out.push_back(std::move(k));
    }
  return out;
}

std::vector<const Triple*> Model::about(const std::string& s) const {
  std::vector<const Triple*> out;
  auto it = bySubject_.find(s);
  if (it != bySubject_.end())
    for (size_t i : it->second) out.push_back(&triples_[i]);
  return out;
}

}  // namespace gl::host::lv2
