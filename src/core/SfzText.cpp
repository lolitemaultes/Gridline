// Gridline: SFZ text. See SfzText.h.
#include "SfzText.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace gl::sfz {

namespace {

constexpr int kMaxDepth = 32;                         // includes inside includes
constexpr size_t kMaxText = size_t{64} << 20;         // repeated includes cannot grow without bound

struct Reader {
  fs::path top;
  std::map<std::string, std::string> defines;
  std::set<std::string> reading;  // the files being read now: the guard against include loops
  Expanded* out = nullptr;
};

bool isBlank(char c) { return c == ' ' || c == '\t'; }

bool readFile(const fs::path& p, std::string& text) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

void stripComments(std::string& text) {
  for (size_t p = text.find("//"); p != std::string::npos; p = text.find("//", p)) {
    const size_t e = text.find('\n', p);
    text.erase(p, e == std::string::npos ? std::string::npos : e - p);
  }
}

bool isFile(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

// The defined name at `at` ("$" included), longest first: "" when none.
const std::string* definedAt(const std::map<std::string, std::string>& defines, const std::string& text, size_t at, std::string* name) {
  const std::string* best = nullptr;
  size_t bestLen = 0;
  for (const auto& [k, v] : defines)
    if (k.size() > bestLen && text.compare(at, k.size(), k) == 0) {
      best = &v;
      bestLen = k.size();
      *name = k;
    }
  return best;
}

bool startsToken(const std::string& text, size_t at) {
  return at == 0 || std::isspace(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '>';
}

bool read(Reader& r, const fs::path& file, std::string& sink, int depth) {
  std::string text;
  if (!readFile(file, text)) return false;
  stripComments(text);
  const std::string key = file.lexically_normal().generic_u8string();
  r.reading.insert(key);
  static const std::string kInclude = "#include", kDefine = "#define";
  for (size_t i = 0; i < text.size() && sink.size() < kMaxText;) {
    const char c = text[i];
    if (c == '#' && startsToken(text, i) && text.compare(i, kDefine.size(), kDefine) == 0) {
      size_t at = i + kDefine.size();
      while (at < text.size() && isBlank(text[at])) ++at;
      const size_t nameAt = at;
      while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) ++at;
      const std::string name = text.substr(nameAt, at - nameAt);
      while (at < text.size() && isBlank(text[at])) ++at;
      const size_t valueAt = at;
      while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) ++at;
      std::string value;
      for (size_t k = valueAt; k < at;) {  // a value may use earlier names
        std::string used;
        if (text[k] == '$')
          if (const std::string* v = definedAt(r.defines, text, k, &used)) {
            value += *v;
            k += used.size();
            continue;
          }
        value += text[k++];
      }
      if (name.size() > 1 && name[0] == '$') r.defines[name] = value;
      i = at;
      continue;
    }
    if (c == '#' && startsToken(text, i) && text.compare(i, kInclude.size(), kInclude) == 0) {
      size_t at = i + kInclude.size();
      while (at < text.size() && isBlank(text[at])) ++at;
      if (at < text.size() && text[at] == '"') {
        const size_t close = text.find('"', at + 1);
        if (close != std::string::npos) {
          std::string name;
          for (size_t k = at + 1; k < close;) {  // names may come from a define
            std::string used;
            if (text[k] == '$')
              if (const std::string* v = definedAt(r.defines, text, k, &used)) {
                name += *v;
                k += used.size();
                continue;
              }
            name += text[k++];
          }
          std::replace(name.begin(), name.end(), '\\', '/');
          fs::path inc = fs::u8path(name);
          if (!inc.is_absolute()) {
            const fs::path beside = (r.top / inc).lexically_normal();
            inc = isFile(beside) ? beside : (file.parent_path() / inc).lexically_normal();
          }
          const std::string incKey = inc.lexically_normal().generic_u8string();
          if (!isFile(inc)) {
            r.out->missing.push_back(inc.u8string());
          } else if (r.reading.count(incKey) || depth >= kMaxDepth) {
            r.out->looped = true;
          } else {
            r.out->included.push_back(inc.u8string());
            sink += '\n';
            read(r, inc, sink, depth + 1);
            sink += '\n';
          }
          i = close + 1;
          continue;
        }
      }
    }
    if (c == '$' && !r.defines.empty()) {
      std::string used;
      if (const std::string* v = definedAt(r.defines, text, i, &used)) {
        sink += *v;
        i += used.size();
        continue;
      }
    }
    sink += c;
    ++i;
  }
  r.reading.erase(key);
  return true;
}

}  // namespace

bool expand(const std::string& path, Expanded& out) {
  out = Expanded{};
  Reader r;
  const fs::path file = fs::u8path(path);
  r.top = file.parent_path();
  r.out = &out;
  return read(r, file, out.text, 0);
}

}  // namespace gl::sfz
