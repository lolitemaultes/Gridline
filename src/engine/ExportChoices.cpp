// Gridline — the export dialog's choices as plain data (see ExportChoices.h).
#include "ExportChoices.h"
#include <algorithm>

namespace gl::exportchoice {

std::vector<ExportFormat> tickedFormats(const bool* ticks, int count) {
  std::vector<ExportFormat> out;
  for (int k = 0; k < std::min(count, static_cast<int>(ExportFormat::Count)); ++k)
    if (ticks[k]) out.push_back(static_cast<ExportFormat>(k));
  return out;
}

std::vector<NamePiece> parseNamePattern(const std::string& pattern, const std::string& tokens) {
  std::vector<NamePiece> out;
  const auto text = [&](const std::string& t) {
    if (t.empty()) return;
    if (!out.empty() && out.back().token == 0) out.back().text += t;
    else out.push_back({0, t});
  };
  for (size_t i = 0; i < pattern.size(); ++i) {
    const char c = pattern[i];
    if (c == '%' && i + 1 < pattern.size()) {
      const char t = pattern[i + 1];
      if (t == '%') {
        text("%");
        ++i;
        continue;
      }
      if (tokens.find(t) != std::string::npos) {
        out.push_back({t, {}});
        ++i;
        continue;
      }
    }
    text(std::string(1, c));
  }
  return out;
}

std::string joinNamePattern(const std::vector<NamePiece>& pieces) {
  std::string out;
  for (const NamePiece& p : pieces) {
    if (p.token) {
      out += '%';
      out += p.token;
      continue;
    }
    for (char c : p.text) {
      out += c;
      if (c == '%') out += '%';
    }
  }
  return out;
}

const char* tokenLabel(char token) {
  switch (token) {
    case 'P': return "Project";
    case 'M': return "Marker";
    case 'T': return "Track";
    case 'N': return "Number";
    case 'D': return "Date";
    case 'R': return "Sample rate";
    case 'S': return "What";
    case 'F': return "Format";
    default: return "?";
  }
}

std::string appendToken(const std::string& pattern, char token, const std::string& tokens) {
  std::vector<NamePiece> p = parseNamePattern(pattern, tokens);
  if (!p.empty()) {
    const char last = p.back().token ? 'x' : (p.back().text.empty() ? ' ' : p.back().text.back());
    if (last != ' ' && last != '-' && last != '_' && last != '.') p.push_back({0, " - "});
  }
  p.push_back({token, {}});
  return joinNamePattern(p);
}

std::string appendText(const std::string& pattern, const std::string& text, const std::string& tokens) {
  std::vector<NamePiece> p = parseNamePattern(pattern, tokens);
  if (!text.empty()) {
    if (!p.empty() && p.back().token == 0) p.back().text += text;
    else p.push_back({0, text});
  }
  return joinNamePattern(p);
}

std::string removePiece(const std::string& pattern, size_t index, const std::string& tokens) {
  std::vector<NamePiece> p = parseNamePattern(pattern, tokens);
  if (index >= p.size()) return pattern;
  p.erase(p.begin() + static_cast<long>(index));
  const auto onlySeps = [](const std::string& t) { return t.find_first_not_of(" -_") == std::string::npos; };
  // A token between two separators ("A - B C" without B): one separator stays, not both.
  if (index > 0 && index < p.size() && p[index - 1].token == 0 && p[index].token == 0 && onlySeps(p[index].text) && onlySeps(p[index - 1].text))
    p.erase(p.begin() + static_cast<long>(index));
  // Merge text pieces that now touch, then trim separators left at the ends.
  std::vector<NamePiece> m;
  for (const NamePiece& q : p) {
    if (!m.empty() && m.back().token == 0 && q.token == 0) m.back().text += q.text;
    else m.push_back(q);
  }
  const auto sep = [](char c) { return c == ' ' || c == '-' || c == '_'; };
  if (!m.empty() && m.front().token == 0) {
    std::string& t = m.front().text;
    size_t k = 0;
    while (k < t.size() && sep(t[k])) ++k;
    t.erase(0, k);
    if (t.empty()) m.erase(m.begin());
  }
  if (!m.empty() && m.back().token == 0) {
    std::string& t = m.back().text;
    while (!t.empty() && sep(t.back())) t.pop_back();
    if (t.empty()) m.pop_back();
  }
  return joinNamePattern(m);
}

std::string shortFolder(const std::string& path, const std::string& home, const std::function<bool(const std::string&)>& fits) {
  std::string p = path;
  if (!home.empty() && home != "/" && (p == home || (p.size() > home.size() && p.compare(0, home.size(), home) == 0 && (p[home.size()] == '/' || p[home.size()] == '\\'))))
    p = "~" + p.substr(home.size());
  if (fits(p)) return p;
  const char sepc = p.find('\\') != std::string::npos && p.find('/') == std::string::npos ? '\\' : '/';
  std::vector<std::string> parts;
  size_t a = 0;
  for (size_t i = 0; i <= p.size(); ++i)
    if (i == p.size() || p[i] == '/' || p[i] == '\\') {
      parts.push_back(p.substr(a, i - a));
      a = i + 1;
    }
  while (parts.size() > 1 && parts.back().empty()) parts.pop_back();  // a trailing separator
  const std::string ell = "\xE2\x80\xA6";                              // …
  const std::string s(1, sepc);
  const std::string last = parts.back();
  // Keep `f` parts from the start (at most two) and `e` from the end, the most parts that fit.
  const size_t n = parts.size();
  for (size_t total = n - 1; total >= 2 && n >= 3; --total)
    for (size_t f = std::min<size_t>(2, total - 1); f >= 1; --f) {
      const size_t e = total - f;
      std::string t;
      for (size_t k = 0; k < f; ++k) t += (k ? s : std::string()) + parts[k];
      t += s + ell;
      for (size_t k = n - e; k < n; ++k) t += s + parts[k];
      if (fits(t)) return t;
    }
  return ell + s + last;
}

}  // namespace gl::exportchoice
