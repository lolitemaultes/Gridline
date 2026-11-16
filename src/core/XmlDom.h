// Gridline — a small XML tree: parse a document into elements with attributes and text, and
// write one back. Enough for interchange formats (K-89 DAWproject); no DTDs, namespaces are kept
// as part of the names, entities are the five predefined ones plus numeric references.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gl::xml {

struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attrs;
  std::vector<Node> children;
  std::string text;  // the character data directly inside (concatenated, trimmed of outer space)

  const std::string* attr(const std::string& key) const;
  std::string get(const std::string& key, const std::string& dflt = "") const;
  double num(const std::string& key, double dflt = 0) const;
  bool flag(const std::string& key, bool dflt = false) const;
  void set(const std::string& key, const std::string& value);
  void set(const std::string& key, double value);
  void setFlag(const std::string& key, bool value) { set(key, std::string(value ? "true" : "false")); }

  const Node* child(const std::string& n) const;  // the first child named n
  Node* child(const std::string& n);
  std::vector<const Node*> all(const std::string& n) const;  // every child named n
  Node& add(const std::string& n);
};

bool parse(const std::string& text, Node& root, std::string* err = nullptr);
// With an XML declaration; children indented by two spaces.
std::string write(const Node& root);
std::string escape(const std::string& s);
// A number as text: integers without a point, others with up to 9 significant digits.
std::string numText(double v);

}  // namespace gl::xml
