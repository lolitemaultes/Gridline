// Gridline - K-93 command palette rows and ranking (see CommandPalette.h).
#include "CommandPalette.h"
#include <algorithm>
#include <unordered_map>
#include "I18n.h"

namespace gl::cmdpal {

namespace {

bool isWordChar(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80; }

// Byte offsets where a word starts (after a non-word character, or at 0).
std::vector<int> wordStarts(const std::string& t) {
  std::vector<int> out;
  for (size_t i = 0; i < t.size(); ++i)
    if (isWordChar(static_cast<unsigned char>(t[i])) && (i == 0 || !isWordChar(static_cast<unsigned char>(t[i - 1])))) out.push_back(static_cast<int>(i));
  return out;
}

int wordEnd(const std::string& t, int start) {
  size_t i = static_cast<size_t>(start);
  while (i < t.size() && isWordChar(static_cast<unsigned char>(t[i]))) ++i;
  return static_cast<int>(i);
}

void range(std::vector<int>* hits, int from, int n) {
  if (!hits) return;
  for (int i = 0; i < n; ++i) hits->push_back(from + i);
}

std::vector<std::string> splitWords(const std::string& q) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : q) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Command: return i18n::N_("Commands");
    case Kind::Window: return i18n::N_("Windows");
    case Kind::Setting: return i18n::N_("Settings");
    case Kind::Plugin: return i18n::N_("Plugins");
    default: return "";
  }
}

std::string lower(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::string stripId(const std::string& label) {
  const size_t p = label.find("##");
  return p == std::string::npos ? label : label.substr(0, p);
}

int editDistance(const std::string& a, const std::string& b, int limit) {
  const size_t n = a.size(), m = b.size();
  if (static_cast<int>(n > m ? n - m : m - n) > limit) return limit + 1;
  // Optimal string alignment distance (an adjacent swap is one edit).
  std::vector<std::vector<int>> d(n + 1, std::vector<int>(m + 1, 0));
  for (size_t i = 0; i <= n; ++i) d[i][0] = static_cast<int>(i);
  for (size_t j = 0; j <= m; ++j) d[0][j] = static_cast<int>(j);
  for (size_t i = 1; i <= n; ++i) {
    int rowMin = limit + 1;
    for (size_t j = 1; j <= m; ++j) {
      const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      int v = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, d[i - 2][j - 2] + 1);
      d[i][j] = v;
      rowMin = std::min(rowMin, v);
    }
    if (rowMin > limit) return limit + 1;
  }
  return std::min(d[n][m], limit + 1);
}

int wordScore(const std::string& word, const std::string& text, std::vector<int>* hits) {
  const std::string w = lower(word), t = lower(text);
  if (w.empty()) return 0;
  if (t.empty()) return -1;
  const int wn = static_cast<int>(w.size()), tn = static_cast<int>(t.size());
  // 1. The whole text.
  if (t == w) {
    range(hits, 0, wn);
    return 1000;
  }
  // 2. The start of the text (shorter texts first).
  if (t.compare(0, w.size(), w) == 0) {
    range(hits, 0, wn);
    return 900 - std::min(100, tn - wn);
  }
  const std::vector<int> starts = wordStarts(t);
  // 3. The start of a later word.
  for (size_t k = 0; k < starts.size(); ++k) {
    const int s = starts[k];
    if (s > 0 && t.compare(static_cast<size_t>(s), w.size(), w) == 0) {
      range(hits, s, wn);
      return 760 - std::min(60, 12 * static_cast<int>(k)) - std::min(60, tn / 4);
    }
  }
  // 4. The words' initials ("pdc": Plugin delay compensation), consecutive or with words skipped.
  if (wn >= 2 && starts.size() >= static_cast<size_t>(wn)) {
    for (size_t first = 0; first + static_cast<size_t>(wn) <= starts.size(); ++first) {
      bool all = true;
      for (int j = 0; j < wn && all; ++j) all = t[static_cast<size_t>(starts[first + static_cast<size_t>(j)])] == w[static_cast<size_t>(j)];
      if (all) {
        for (int j = 0; j < wn; ++j) range(hits, starts[first + static_cast<size_t>(j)], 1);
        return 640 - std::min(60, 12 * static_cast<int>(first));
      }
    }
    std::vector<int> got;
    for (size_t k = 0; k < starts.size() && got.size() < static_cast<size_t>(wn); ++k)
      if (t[static_cast<size_t>(starts[k])] == w[got.size()]) got.push_back(starts[k]);
    if (got.size() == static_cast<size_t>(wn)) {
      for (int p : got) range(hits, p, 1);
      return 560;
    }
  }
  // 5. Anywhere.
  if (const size_t p = t.find(w); p != std::string::npos) {
    range(hits, static_cast<int>(p), wn);
    return 450 - std::min(100, static_cast<int>(p));
  }
  // 6. Letters in order, with gaps (two letters at least).
  if (wn >= 2) {
    std::vector<int> pos;
    size_t j = 0;
    for (size_t i = 0; i < t.size() && j < w.size(); ++i)
      if (t[i] == w[j]) {
        pos.push_back(static_cast<int>(i));
        ++j;
      }
    if (j == w.size()) {
      int gaps = 0;
      for (size_t k = 1; k < pos.size(); ++k) gaps += pos[k] - pos[k - 1] - 1;
      if (hits) hits->insert(hits->end(), pos.begin(), pos.end());
      return std::max(60, 300 - 8 * gaps - std::min(50, pos.front()));
    }
  }
  // 7. A typo: a word of the text (or its start, while typing) within a small edit distance.
  if (wn >= 3) {
    const int limit = wn >= 7 ? 2 : 1;
    int best = limit + 1, bestStart = -1, bestLen = 0;
    for (int s : starts) {
      const int e = wordEnd(t, s), len = e - s;
      for (int cut = std::max(1, wn - 1); cut <= std::min(len, wn + 1); ++cut) {
        const int d = editDistance(w, t.substr(static_cast<size_t>(s), static_cast<size_t>(cut)), limit);
        if (d < best) {
          best = d;
          bestStart = s;
          bestLen = cut;
        }
      }
    }
    if (best <= limit) {
      range(hits, bestStart, bestLen);
      return 200 - 40 * best;
    }
  }
  return -1;
}

int entryScore(const Entry& e, const std::string& query, std::vector<int>* labelHits) {
  const std::vector<std::string> words = splitWords(query);
  if (words.empty()) return 0;
  const std::string& en = e.labelEn.empty() ? e.label : e.labelEn;
  const std::string& pathEn = e.pathEn.empty() ? e.path : e.pathEn;
  int total = 0;
  std::vector<int> hits;
  for (const std::string& w : words) {
    std::vector<int> h;
    int best = wordScore(w, e.label, &h);
    int ens = en != e.label ? wordScore(w, en) : -1;
    if (ens > best) {
      best = ens;
      h.clear();
    }
    // Where the row lives counts, less than its name.
    for (const std::string* p : {&e.path, &pathEn}) {
      const int ps = wordScore(w, *p);
      if (ps >= 0 && ps * 11 / 20 > best) {
        best = ps * 11 / 20;
        h.clear();
      }
      if (p == &e.path && pathEn == e.path) break;
    }
    // Other words it is known by (a keymap description, a settings group).
    for (const std::string* al : {&e.alias, &e.aliasEn}) {
      if (al->empty() || (al == &e.aliasEn && e.aliasEn == e.alias)) continue;
      const int as = wordScore(w, *al);
      if (as >= 0 && as * 7 / 10 > best) {
        best = as * 7 / 10;
        h.clear();
      }
    }
    if (!e.shortcut.empty() && lower(w) == lower(e.shortcut) && 500 > best) {
      best = 500;
      h.clear();
    }
    if (best < 0) return -1;
    total += best;
    hits.insert(hits.end(), h.begin(), h.end());
  }
  int score = total / static_cast<int>(words.size());
  if (words.size() > 1) {  // "save as": the words together as the label's start rank first
    std::vector<int> h;
    const int whole = std::max(wordScore(query, e.label, &h), en != e.label ? wordScore(query, en) : -1);
    if (whole > score) {
      score = whole;
      if (!h.empty()) hits = h;
    }
  }
  if (labelHits) {
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    *labelHits = std::move(hits);
  }
  return score;
}

std::vector<Ranked> rank(const std::vector<Entry>& entries, const std::string& query, const std::vector<std::string>& recent, int kind) {
  std::vector<Ranked> out;
  std::unordered_map<std::string, int> recentAt;
  for (size_t i = 0; i < recent.size(); ++i) recentAt.emplace(recent[i], static_cast<int>(i));
  auto wanted = [kind](const Entry& e) { return kind < 0 || static_cast<int>(e.kind) == kind; };
  if (splitWords(query).empty()) {
    std::vector<Ranked> rec(recent.size());
    std::vector<bool> have(recent.size(), false);
    for (size_t i = 0; i < entries.size(); ++i) {
      if (!wanted(entries[i])) continue;
      auto it = recentAt.find(entries[i].id);
      if (it != recentAt.end() && !have[static_cast<size_t>(it->second)]) {
        have[static_cast<size_t>(it->second)] = true;
        rec[static_cast<size_t>(it->second)] = Ranked{static_cast<int>(i), 0, true, {}};
      }
    }
    for (size_t k = 0; k < rec.size(); ++k)
      if (have[k]) out.push_back(rec[k]);
    for (size_t i = 0; i < entries.size(); ++i)
      if (wanted(entries[i]) && !recentAt.count(entries[i].id)) out.push_back(Ranked{static_cast<int>(i), 0, false, {}});
    return out;
  }
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    if (!wanted(e)) continue;
    Ranked r;
    r.index = static_cast<int>(i);
    r.score = entryScore(e, query, &r.hits);
    if (r.score < 0) continue;
    if (auto it = recentAt.find(e.id); it != recentAt.end()) {
      r.recent = true;
      r.score += 30 - 2 * it->second;
    }
    if (!e.enabled) r.score -= 20;
    out.push_back(std::move(r));
  }
  std::stable_sort(out.begin(), out.end(), [&](const Ranked& x, const Ranked& y) {
    if (x.score != y.score) return x.score > y.score;
    return entries[static_cast<size_t>(x.index)].order < entries[static_cast<size_t>(y.index)].order;
  });
  return out;
}

std::vector<std::string> parseRecent(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty() && std::find(out.begin(), out.end(), cur) == out.end() && out.size() < kRecentMax) out.push_back(cur);
    cur.clear();
  };
  for (char c : text) {
    if (c == '\n' || c == '\r') flush();
    else cur += c;
  }
  flush();
  return out;
}

std::string serializeRecent(const std::vector<std::string>& ids) {
  std::string out;
  for (const auto& id : ids) {
    if (id.empty() || id.find('\n') != std::string::npos) continue;
    if (!out.empty()) out += '\n';
    out += id;
  }
  return out;
}

void pushRecent(std::vector<std::string>& ids, const std::string& id, size_t cap) {
  if (id.empty()) return;
  ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
  ids.insert(ids.begin(), id);
  if (ids.size() > cap) ids.resize(cap);
}

}  // namespace gl::cmdpal
