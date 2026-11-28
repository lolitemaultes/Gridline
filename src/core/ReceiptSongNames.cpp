// Gridline - Track Receipt: the song identity of sample file names that are not named like store
// downloads (see SongIdentity in TrackReceipt.h for the rules).
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>
#include "TrackReceipt.h"

namespace gl::receipt {

namespace {

struct Word {
  std::string text, low;
  const char* why = nullptr;  // set: left out
};

bool isDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

// "98", "98.00"
bool isNumber(const std::string& s) {
  if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0]))) return false;
  int dots = 0;
  for (char c : s) {
    if (c == '.') ++dots;
    else if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return dots <= 1 && s.back() != '.';
}

bool endsWith(const std::string& s, const std::string& t) { return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }

// "86bpm", "98.00bpm", "bpm98"
bool isTempo(const std::string& w) {
  if (endsWith(w, "bpm")) return isNumber(w.substr(0, w.size() - 3));
  if (w.rfind("bpm", 0) == 0) return isNumber(w.substr(3));
  return false;
}

// A note name with its accidental: "g", "c#", "bb", "fsharp"; the rest of the word in `rest`.
bool notePrefix(const std::string& w, std::string& rest) {
  if (w.empty() || w[0] < 'a' || w[0] > 'g') return false;
  size_t i = 1;
  if (w.compare(1, 5, "sharp") == 0) i = 6;
  else if (w.compare(1, 4, "flat") == 0) i = 5;
  else if (i < w.size() && (w[i] == '#' || w[i] == 'b')) {
    // "b" after the note is a flat only when a quality or nothing follows ("bb", "ebmin", but not "bmaj" read as "b" + "maj")
    const std::string after = w.substr(i + 1);
    if (after.empty() || after == "m" || after == "maj" || after == "major" || after == "min" || after == "minor" || w[i] == '#') ++i;
  }
  rest = w.substr(i);
  return true;
}

// Keys written in full: "gminor", "dmaj", "c#min", "fsharpmajor".
bool isLongKey(const std::string& w) {
  std::string q;
  if (!notePrefix(w, q)) return false;
  return q == "maj" || q == "major" || q == "min" || q == "minor";
}

// Short keys: "bm", "f#", "am", "c" (only next to a tempo or the word "key").
bool isShortKey(const std::string& w) {
  std::string q;
  if (!notePrefix(w, q)) return false;
  return q.empty() || q == "m";
}

bool isNoteOnly(const std::string& w) {
  std::string q;
  return notePrefix(w, q) && q.empty();
}

// Parts of a song and how a file of it was made: they say which stem, not which song.
const std::set<std::string>& partWords() {
  static const std::set<std::string> w = {
      "vocal", "vocals", "vox", "voc", "acapella", "acapellas", "acappella", "acap", "lead", "leads", "backing", "bgv", "bgvs", "bv",
      "bvs", "harmony", "harmonies", "adlib", "adlibs", "chop", "chops", "chopped", "drum", "drums", "beat", "beats", "bass", "sub",
      "808", "kick", "kicks", "snare", "snares", "hat", "hats", "hihat", "hihats", "clap", "claps", "perc", "percs", "percussion",
      "cymbal", "cymbals", "tom", "toms", "shaker", "guitar", "guitars", "gtr", "keys", "piano", "rhodes", "organ", "synth", "synths",
      "pad", "pads", "strings", "brass", "horn", "horns", "melody", "melodies", "chord", "chords", "arp", "pluck", "plucks", "fx",
      "sfx", "riser", "texture", "atmos", "ambience", "loop", "loops", "stem", "stems", "oneshot", "instrumental", "inst", "instr",
      "full", "mix", "master", "mastered", "premaster", "dry", "wet", "processed", "separated", "isolated", "extracted",
      "verse", "chorus", "prechorus", "hook", "intro", "outro", "bridge"};
  return w;
}

// Notes a store or a pack puts on a file.
const std::set<std::string>& noteWords() {
  static const std::set<std::string> w = {"clearance", "required", "cleared", "uncleared", "royalty", "free", "royaltyfree",
                                          "tracklib", "sample", "samples", "copy", "wav", "aif", "aiff", "flac", "mp3"};
  return w;
}

// Words a number after which is a take or version ("take 2"), also written with it ("v3", "take2").
const std::set<std::string>& takeWords() {
  static const std::set<std::string> w = {"take", "tk", "version", "ver", "v", "alt", "vol", "volume"};
  return w;
}

// "part 2", "pt 2", "part2", "pt2": the part number (0 = not a part word).
bool isPartWord(const std::string& w) { return w == "part" || w == "pt"; }
int joinedNumber(const std::string& w, const std::set<std::string>& words) {
  size_t i = 0;
  while (i < w.size() && std::isalpha(static_cast<unsigned char>(w[i]))) ++i;
  if (i == 0 || i == w.size() || !words.count(w.substr(0, i)) || !isDigits(w.substr(i)) || w.size() - i > 3) return 0;
  return std::max(1, std::atoi(w.c_str() + i));
}

// Pack and version codes: up to four letters then one to three digits ("vbv3", "sp404", "v2").
bool isCode(const std::string& w) {
  size_t i = 0;
  while (i < w.size() && std::isalpha(static_cast<unsigned char>(w[i]))) ++i;
  if (i == 0 || i > 4 || i == w.size()) return false;
  const std::string digits = w.substr(i);
  return digits.size() <= 3 && isDigits(digits);
}

std::vector<Word> split(const std::string& name) {
  std::vector<Word> out;
  std::string cur;
  auto push = [&]() {
    if (cur.empty()) return;
    Word w;
    w.text = cur;
    w.low = lowerText(cur);
    out.push_back(w);
    cur.clear();
  };
  for (size_t i = 0; i < name.size(); ++i) {
    const auto c = static_cast<unsigned char>(name[i]);
    const bool digitDot = c == '.' && i > 0 && i + 1 < name.size() && std::isdigit(static_cast<unsigned char>(name[i - 1])) &&
                          std::isdigit(static_cast<unsigned char>(name[i + 1]));
    if (std::isalnum(c) || c == '#' || c >= 0x80 || digitDot) cur += static_cast<char>(c);
    else push();
  }
  push();
  return out;
}

std::string stripExt(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0 || name.size() - dot > 6) return name;
  for (size_t i = dot + 1; i < name.size(); ++i)
    if (!std::isalnum(static_cast<unsigned char>(name[i]))) return name;
  return name.substr(0, dot);
}

}  // namespace

SongIdentity songIdentity(const std::string& pathOrName) {
  SongIdentity id;
  std::vector<Word> w = split(stripExt(baseName(pathOrName)));
  const size_t n = w.size();
  auto out = [&](size_t i, const char* why) {
    if (i < n && !w[i].why) w[i].why = why;
  };
  // 1. tempo: "86bpm", and a number next to the word "bpm"
  for (size_t i = 0; i < n; ++i) {
    if (isTempo(w[i].low)) out(i, "tempo");
    if (w[i].low == "bpm") {
      out(i, "tempo");
      if (i > 0 && isNumber(w[i - 1].low)) out(i - 1, "tempo");
      else if (i + 1 < n && isNumber(w[i + 1].low)) out(i + 1, "tempo");
    }
  }
  auto tempoAt = [&](size_t i) { return i < n && w[i].why && std::string(w[i].why) == "tempo"; };
  // 2. keys
  for (size_t i = 0; i < n; ++i) {
    const std::string& l = w[i].low;
    if (isLongKey(l)) out(i, "key");
    const auto quality = [&](size_t j) { return j < n && (w[j].low == "major" || w[j].low == "minor" || w[j].low == "maj" || w[j].low == "min"); };
    if (isNoteOnly(l) && quality(i + 1)) out(i, "key"), out(i + 1, "key");
    if (isNoteOnly(l) && i + 1 < n && (w[i + 1].low == "sharp" || w[i + 1].low == "flat") && quality(i + 2))  // "C sharp minor"
      out(i, "key"), out(i + 1, "key"), out(i + 2, "key");
    if (l == "key" && i + 1 < n && (isShortKey(w[i + 1].low) || isLongKey(w[i + 1].low))) out(i, "key"), out(i + 1, "key");
    if (isShortKey(l) && ((i > 0 && tempoAt(i - 1)) || tempoAt(i + 1))) out(i, "key");
  }
  // 3. parts of a long song ("part 2": the part is kept apart in `part`), takes and versions,
  //    parts of a song, store notes, pack codes
  static const std::set<std::string> partNames = {"part", "pt"};
  for (size_t i = 0; i < n; ++i) {
    const std::string& l = w[i].low;
    if (isPartWord(l) && i + 1 < n && isDigits(w[i + 1].low) && w[i + 1].low.size() <= 3) {
      out(i, "part"), out(i + 1, "part");
      if (id.part == 0) id.part = std::max(1, std::atoi(w[i + 1].low.c_str()));
    } else if (const int p = joinedNumber(l, partNames); p > 0) {
      out(i, "part");
      if (id.part == 0) id.part = p;
    }
    if (takeWords().count(l)) {
      if (i + 1 < n && isDigits(w[i + 1].low)) out(i, "version"), out(i + 1, "version");
      else if (l != "v") out(i, "version");
    } else if (joinedNumber(l, takeWords()) > 0) {
      out(i, "version");
    }
    if (partWords().count(l)) out(i, "stem");
    if (noteWords().count(l)) out(i, "note");
    if (!w[i].why && isCode(l)) out(i, "code");
  }
  for (size_t i = 0; i + 1 < n; ++i) id.clearance = id.clearance || (w[i].low == "clearance" && w[i + 1].low == "required");
  std::vector<const Word*> kept;
  for (const Word& x : w) {
    if (x.why) id.dropped.push_back({x.text, x.why});
    else kept.push_back(&x);
  }
  // Too little left to tell songs apart: no identity.
  bool enough = kept.size() >= 2;
  if (kept.size() == 1) {
    size_t letters = 0;
    for (char c : kept[0]->low) letters += std::isalpha(static_cast<unsigned char>(c)) ? 1 : 0;
    enough = letters >= 5;
  }
  bool anyLetter = false;
  for (const Word* x : kept)
    for (char c : x->low) anyLetter = anyLetter || std::isalpha(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80;
  if (!enough || !anyLetter) {
    const bool clearance = id.clearance;
    id = SongIdentity{};
    id.clearance = clearance;
    return id;
  }
  for (const Word* x : kept) {
    id.key += (id.key.empty() ? "" : " ") + x->low;
    id.title += (id.title.empty() ? "" : " ") + x->text;
  }
  return id;
}

}  // namespace gl::receipt
