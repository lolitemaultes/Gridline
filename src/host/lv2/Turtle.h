// Gridline — a small RDF Turtle reader for LV2 bundle descriptions
// (manifest.ttl, plugin / UI / preset .ttl files). Gridline's own code: no
// serd / sord / lilv. It reads the Turtle 1.1 syntax LV2 bundles use —
// @prefix / @base (and SPARQL-style PREFIX / BASE), IRIs relative to the
// document, prefixed names, 'a', blank nodes ([ … ] and _:labels),
// collections, string literals (short and long, escapes, @lang, ^^type),
// numbers and booleans — into a flat triple store.
//
// Literals keep their lexical form; numbers carry their XSD datatype.
// Blank node labels are made unique per loaded document.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace gl::host::lv2 {

struct Node {
  enum Kind : unsigned char { None, Iri, Blank, Literal } kind = None;
  std::string value;     // IRI / blank label / literal text
  std::string datatype;  // literal datatype IRI ("" plain string)
  std::string lang;
  bool isIri() const { return kind == Iri; }
  bool isBlank() const { return kind == Blank; }
  bool isLiteral() const { return kind == Literal; }
  bool isResource() const { return kind == Iri || kind == Blank; }
  // Store key of a subject node.
  std::string key() const { return (kind == Blank ? "_:" : "") + value; }
  double number(double def = 0) const;  // numeric literal (or "true"/"false")
  bool truth() const;                   // boolean literal ("true" / "1")
};

struct Triple {
  Node s;
  std::string p;  // predicate IRI
  Node o;
};

class Model {
 public:
  // Parses a Turtle file; base IRI = the file's own file:// URI. Returns
  // false (with a message) on a read or syntax error; triples parsed before
  // the error are kept.
  bool loadFile(const std::string& path, std::string* err = nullptr);
  bool loadString(const std::string& text, const std::string& baseIri, std::string* err = nullptr);
  bool loaded(const std::string& path) const;

  // Queries. `s` is a subject key (IRI, or "_:label" for blank nodes).
  std::vector<const Node*> objects(const std::string& s, const std::string& p) const;
  const Node* object(const std::string& s, const std::string& p) const;  // first, nullptr when none
  std::string text(const std::string& s, const std::string& p) const;    // first object's value, "" when none
  bool has(const std::string& s, const std::string& p, const std::string& oIri) const;
  // Subjects with (p, IRI o).
  std::vector<std::string> subjects(const std::string& p, const std::string& oIri) const;
  const std::vector<Triple>& triples() const { return triples_; }
  // Every (predicate, object) pair of a subject, in document order.
  std::vector<const Triple*> about(const std::string& s) const;

 private:
  std::vector<Triple> triples_;
  std::unordered_map<std::string, std::vector<size_t>> bySubject_;
  std::vector<std::string> files_;
  int docs_ = 0;
  friend class Parser;
  void add(Node s, std::string p, Node o);
};

// file:// URI <-> path (percent-encoding).
std::string fileUri(const std::string& path);
bool uriToPath(const std::string& uri, std::string& path);
// Resolves a relative IRI reference against a base IRI (RFC 3986, simplified).
std::string resolveIri(const std::string& base, const std::string& ref);

}  // namespace gl::host::lv2
