// Gridline — FLP import: project info, tempo, timebase, time signature, master pitch.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include "../Types.h"
#include "Flp.h"

namespace gl::flp {

// Plain text of an RTF document (older projects keep their comments only as
// RTF, event 198): paragraphs become new lines, \'hh and \uN become
// characters, font / colour tables and ignorable destinations are skipped.
std::string rtfToText(const std::string& rtf) {
  std::string out;
  auto put = [&](uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  };
  int depth = 0, skipFrom = -1;  // skip while inside the group opened at depth skipFrom
  for (size_t i = 0; i < rtf.size(); ++i) {
    const char ch = rtf[i];
    if (ch == '{') {
      ++depth;
      continue;
    }
    if (ch == '}') {
      if (depth == skipFrom) skipFrom = -1;
      --depth;
      continue;
    }
    const bool skipping = skipFrom >= 0 && depth >= skipFrom;
    if (ch == '\\' && i + 1 < rtf.size()) {
      const char n = rtf[i + 1];
      if (n == '\'' && i + 3 < rtf.size()) {
        if (!skipping) put(static_cast<uint32_t>(std::strtol(rtf.substr(i + 2, 2).c_str(), nullptr, 16)));
        i += 3;
        continue;
      }
      if (n == '\\' || n == '{' || n == '}') {
        if (!skipping) out += n;
        ++i;
        continue;
      }
      if (n == '*') {
        if (!skipping) skipFrom = depth;
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < rtf.size() && std::isalpha(static_cast<unsigned char>(rtf[j]))) ++j;
      const std::string word = rtf.substr(i + 1, j - i - 1);
      size_t k = j;
      if (k < rtf.size() && (rtf[k] == '-' || std::isdigit(static_cast<unsigned char>(rtf[k])))) {
        ++k;
        while (k < rtf.size() && std::isdigit(static_cast<unsigned char>(rtf[k]))) ++k;
      }
      const std::string num = rtf.substr(j, k - j);
      if (k < rtf.size() && rtf[k] == ' ') ++k;
      i = k - 1;
      if (skipping) continue;
      if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info") skipFrom = depth;
      else if (word == "par" || word == "line") out += '\n';
      else if (word == "tab") out += '\t';
      else if (word == "u" && !num.empty()) put(static_cast<uint32_t>(std::strtol(num.c_str(), nullptr, 10) & 0xFFFF));
      continue;
    }
    if (skipping || ch == '\r' || ch == '\n') continue;
    out += ch;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  return out;
}

void importProject(Context& c) {
  Song& s = c.song;
  // Keep FL's timebase when Gridline supports it (all common FL PPQs are), so
  // every tick position imports exactly.
  int ppq = c.file.ppq;
  if (std::find(std::begin(kPpqChoices), std::end(kPpqChoices), ppq) == std::end(kPpqChoices)) {
    int best = kDefaultPpq;
    for (int p : kPpqChoices)
      if (std::abs(p - ppq) < std::abs(best - ppq)) best = p;
    c.report.warn("timebase " + std::to_string(ppq) + " PPQ is not supported; using " + std::to_string(best));
    ppq = best;
  }
  s.ppq = ppq;
  double coarse = 0, fine = 0;
  for (const auto& e : c.sec.project) {
    switch (e.id) {
      case 156: s.tempo = e.value() / 1000.0; break;  // tempo × 1000 (FL 11+)
      case 66: coarse = e.value(); break;             // legacy coarse tempo
      case 93: fine = e.value(); break;               // legacy fine tempo
      case 17: s.timeSigNum = std::clamp<int>(static_cast<int>(e.value()), 1, 16); break;
      case 18: s.timeSigDen = std::clamp<int>(static_cast<int>(e.value()), 1, 16); break;
      case 80: s.masterPitch = static_cast<float>(static_cast<int16_t>(e.value())) / 100.0f; break;  // cents
      case 194: s.info.title = e.text(); break;
      case 195: s.info.comments = e.text(); break;
      case 197: s.info.url = e.text(); break;
      case 206: s.info.genre = e.text(); break;
      case 207: s.info.author = e.text(); break;
      case 11: s.swing = static_cast<float>(std::clamp(e.value() / 128.0, 0.0, 1.0)); break;  // main swing 0..128
      case 237:  // creation date and time spent, both as day counts (day 0 = 1899-12-30)
        if (e.size >= 16) {
          const double created = e.f64(0), spent = e.f64(8);
          if (std::isfinite(created) && created > 25569.0 && created < 109574.0)  // 1970 … 2199
            s.info.created = static_cast<int64_t>(std::llround((created - 25569.0) * 86400.0));
          if (std::isfinite(spent) && spent > 0 && spent < 36500.0) s.info.workTime = static_cast<int64_t>(std::llround(spent * 86400.0));
        }
        break;
      default: break;
    }
  }
  if (!c.file.events.empty() && !find(c.sec.project, 156) && coarse > 0) s.tempo = coarse + fine / 1000.0;
  s.tempo = std::clamp(s.tempo, 10.0, 522.0);
  // J-43: event 44 is FL's per-project "Automatic crossfades" switch (FL 21+;
  // any non-zero byte = on). FL also stores the crossfades it derives in the
  // clip records; the arrangement import keeps them where Gridline would
  // derive something else (see Importer::importPlaylist). Files without the
  // event (older FL, some exports) still carry derived crossfades: on.
  if (const Event* e = find(c.sec.project, 44)) s.autoCrossfades = e->value() != 0;
  else s.autoCrossfades = true;
  if (s.info.title.empty()) s.info.title = "Imported project";
  if (s.info.comments.empty())  // older projects: comments only as RTF (198)
    if (const Event* e = find(c.sec.project, 198)) s.info.comments = rtfToText(e->text());
}

}  // namespace gl::flp
