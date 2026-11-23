// Gridline - "My keys say" (see KeyCaps.h).
#include "KeyCaps.h"
#include <array>
#include <cctype>
#include <cstring>
#include <unordered_set>
#include "I18n.h"

namespace gl::keycaps {

using keys::Key;

namespace {

constexpr int kFirst = 4, kLast = 56;  // the letter, digit and punctuation positions

int (*gSystem)(int) = nullptr;
TypingLayout gSim = TypingLayout::Computer;
TypingLayout gCaps = TypingLayout::Computer;
uint32_t gVersion = 1;
bool gBuilt = false;
bool gByTyped = false;
std::array<int, kLast + 1> gChars{};  // what the computer types at each position
std::array<Key, kLast + 1> gKeys{};   // the key each position gives the shortcuts

bool letterDigitOrPunct(int p) { return p >= kFirst && p <= kLast && (p <= 39 || p >= 45) && p != 50; }

int rawSystemChar(int p) {
  if (gSim != TypingLayout::Computer) {
    if (p >= 30 && p <= 39) return TypingKeyboard::usChar(p);  // SDL keeps the digit row as digits on every layout
    return TypingKeyboard::layoutChar(gSim, p);
  }
  if (gSystem) return gSystem(p);
  return TypingKeyboard::usChar(p);
}

// The key the interface makes of a press: by what it types for letters, digits and , . ;, else by
// where it sits for the other punctuation (Dear ImGui's SDL2 backend, ImGui_ImplSDL2_KeyEventToImGuiKey).
Key byTyped(int c) {
  if (c >= 'a' && c <= 'z') return static_cast<Key>(static_cast<int>(Key::A) + (c - 'a'));
  if (c >= '0' && c <= '9') return static_cast<Key>(static_cast<int>(Key::D0) + (c - '0'));
  if (c == ',') return Key::Comma;
  if (c == '.') return Key::Period;
  if (c == ';') return Key::Semicolon;
  return Key::None;
}
Key bySpot(int p) {
  switch (p) {
    case 53: return Key::Grave;
    case 45: return Key::Minus;
    case 46: return Key::Equal;
    case 47: return Key::LeftBracket;
    case 48: return Key::RightBracket;
    case 49: return Key::Backslash;
    case 51: return Key::Semicolon;
    case 52: return Key::Apostrophe;
    case 54: return Key::Comma;
    case 55: return Key::Period;
    case 56: return Key::Slash;
    default: return Key::None;
  }
}
// The punctuation that goes by its spot, by what it types (the gap rule).
Key punctTyped(int c) {
  switch (c) {
    case '-': return Key::Minus;
    case '=': return Key::Equal;
    case '[': return Key::LeftBracket;
    case ']': return Key::RightBracket;
    case '\\': return Key::Backslash;
    case '\'': return Key::Apostrophe;
    case '/': return Key::Slash;
    case '`': return Key::Grave;
    default: return Key::None;
  }
}
int spotOf(Key k) {
  for (int p = 45; p <= 56; ++p)
    if (p != 50 && p != 51 && p != 54 && p != 55 && bySpot(p) == k) return p;
  return 0;
}
bool shortcutKey(Key k) { return (k >= Key::A && k <= Key::D9) || (k >= Key::Minus && k <= Key::Grave); }

int missing(const std::array<Key, kLast + 1>& m, std::array<bool, static_cast<size_t>(Key::Count)>* has) {
  has->fill(false);
  for (int p = kFirst; p <= kLast; ++p) (*has)[static_cast<size_t>(m[static_cast<size_t>(p)])] = true;
  int n = 0;
  for (size_t k = 0; k < has->size(); ++k)
    if (shortcutKey(static_cast<Key>(k)) && !(*has)[k]) ++n;
  return n;
}

void build() {
  std::array<int, kLast + 1> chars{};
  for (int p = kFirst; p <= kLast; ++p) chars[static_cast<size_t>(p)] = letterDigitOrPunct(p) ? rawSystemChar(p) : 0;
  if (gBuilt && chars == gChars) return;
  gChars = chars;
  gBuilt = true;
  ++gVersion;
  std::array<Key, kLast + 1> plain{}, typed{};
  for (int p = kFirst; p <= kLast; ++p) {
    if (!letterDigitOrPunct(p)) continue;
    const int c = chars[static_cast<size_t>(p)];
    Key k = byTyped(c);
    if (k == Key::None) k = bySpot(p);
    plain[static_cast<size_t>(p)] = k;
    const Key t = punctTyped(c);
    typed[static_cast<size_t>(p)] = t != Key::None ? t : k;
  }
  // The punctuation goes by what it types only where the interface's own rule leaves a key out and
  // this gives more keys a place, never fewer.
  std::array<bool, static_cast<size_t>(Key::Count)> hasPlain{}, hasTyped{};
  const int lostPlain = missing(plain, &hasPlain), lostTyped = missing(typed, &hasTyped);
  bool better = lostPlain > 0 && lostTyped < lostPlain;
  for (size_t k = 0; better && k < hasPlain.size(); ++k)
    if (hasPlain[k] && !hasTyped[k]) better = false;
  gByTyped = better;
  gKeys = better ? typed : plain;
}

void ensure() {
  if (!gBuilt) build();
}

}  // namespace

void setSystem(int (*typedAt)(int)) {
  gSystem = typedAt;
  gBuilt = false;
  ++gVersion;
}

void simulateSystem(TypingLayout l) {
  gSim = l < TypingLayout::Count ? l : TypingLayout::Computer;
  gBuilt = false;
  ++gVersion;
}
TypingLayout simulatedSystem() { return gSim; }

int systemChar(int position) { return rawSystemChar(position); }

void refresh() { build(); }

void setCaps(TypingLayout l) {
  const TypingLayout n = l < TypingLayout::Count ? l : TypingLayout::Computer;
  if (n != gCaps) ++gVersion;
  gCaps = n;
}
TypingLayout caps() { return gCaps; }

const char* capsId(TypingLayout l) {
  switch (l) {
    case TypingLayout::Standard: return "qwerty";
    case TypingLayout::Qwertz: return "qwertz";
    case TypingLayout::Azerty: return "azerty";
    case TypingLayout::Dvorak: return "dvorak";
    case TypingLayout::Colemak: return "colemak";
    default: return "computer";
  }
}

TypingLayout capsFromId(const std::string& id) {
  if (id == "standard") return TypingLayout::Standard;
  for (TypingLayout l : kCapsOrder)
    if (id == capsId(l)) return l;
  return TypingLayout::Computer;
}

const char* capsName(TypingLayout l) {
  switch (l) {
    case TypingLayout::Standard: return i18n::N_("QWERTY");
    case TypingLayout::Qwertz: return i18n::N_("QWERTZ");
    case TypingLayout::Azerty: return i18n::N_("AZERTY");
    case TypingLayout::Dvorak: return i18n::N_("Dvorak");
    case TypingLayout::Colemak: return i18n::N_("Colemak");
    default: return i18n::N_("Same as my computer");
  }
}

uint32_t version() {
  ensure();
  return gVersion;
}

Key keyAt(int position) {
  ensure();
  return position >= kFirst && position <= kLast ? gKeys[static_cast<size_t>(position)] : Key::None;
}

int positionOf(Key k) {
  if (!shortcutKey(k)) return -1;
  ensure();
  // Its own US place first when that gives it, then the first place that does.
  const int home = k >= Key::Minus ? spotOf(k) : 0;
  if (home && gKeys[static_cast<size_t>(home)] == k) return home;
  for (int p = kFirst; p <= kLast; ++p)
    if (gKeys[static_cast<size_t>(p)] == k) return p;
  return 0;
}

bool punctuationByTyped() {
  ensure();
  return gByTyped;
}

int remapPosition(int position) {
  ensure();
  if (!gByTyped || position < kFirst || position > kLast) return 0;
  const Key want = gKeys[static_cast<size_t>(position)];
  const int c = gChars[static_cast<size_t>(position)];
  Key plain = byTyped(c);
  if (plain == Key::None) plain = bySpot(position);
  if (want == plain || want < Key::Minus) return 0;
  return spotOf(want);
}

std::string keyName(Key k) {
  const int p = positionOf(k);
  if (gCaps == TypingLayout::Computer || p <= 0) return keys::keyName(k);
  if (p >= 30 && p <= 39) return std::string(1, static_cast<char>(TypingKeyboard::usChar(p)));  // the digit row: its digit
  return TypingKeyboard::keyLabel(TypingKeyboard::usChar(p), TypingKeyboard::layoutChar(gCaps, p));
}

std::string format(const keys::Chord& c) {
  if (!c.valid()) return {};
  const int p = positionOf(c.key);
  if (gCaps == TypingLayout::Computer && p != 0) return keys::format(c);  // as before, byte for byte
  std::string s;
  if (c.mods & keys::kCtrl) s += "Ctrl+";
  if (c.mods & keys::kCmd) s += "Cmd+";
  if (c.mods & keys::kAlt) s += "Alt+";
  if (c.mods & keys::kShift) s += "Shift+";
  s += keycaps::keyName(c.key);
  if (p == 0) s += std::string(" (") + i18n::tr("no key") + ")";
  return s;
}

std::string formatText(const std::string& text) {
  const keys::Chord c = keys::parse(text);
  if (!c.valid() || keys::format(c) != text) return text;
  return keycaps::format(c);
}

// ---- Keys named inside a sentence ----

namespace {

uint8_t foldCmd(uint8_t m) { return (m & keys::kCmd) ? static_cast<uint8_t>((m & ~keys::kCmd) | keys::kCtrl) : m; }

bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
// A key one character names whose name the keys can change (a letter, digit or punctuation key).
bool oneCharKey(char c) { return c && (alnum(c) || std::strchr("-=[]\\;',./`", c) != nullptr); }

std::string oneKey(const std::string& tok, bool byPlace) {
  if (tok.size() != 1 || !oneCharKey(tok[0])) return tok;
  if (byPlace && gCaps != TypingLayout::Computer) {  // (at the default: as written, as before)
    const int c = std::tolower(static_cast<unsigned char>(tok[0]));
    return TypingKeyboard::positionOfKey(c) ? placeName(c) : tok;
  }
  const Key k = keys::keyFromName(tok);
  return k == Key::None || keys::keyName(k) != tok ? tok : keycaps::keyName(k);
}

}  // namespace

std::string cmdKey(const char* id, const char* fixed) {
  const keys::Chord f = keys::parse(fixed ? fixed : "");
  if (!f.valid()) return fixed ? fixed : "";
  const keys::Keymap& km = keys::keymap();
  const std::vector<keys::Chord>& b = km.bindings(id);
  for (const keys::Chord& c : b)
    if (c.key == f.key && foldCmd(c.mods) == foldCmd(f.mods)) return keycaps::format(f);  // as written
  // The set has no key for it (and the person took none away): the sentence as written.
  if (b.empty()) return km.customized(id) ? std::string(i18n::tr("no key")) : keycaps::format(f);
  // The set's (or the person's) own key, spelt as the menus spell it.
  keys::Chord c = b.front();
  bool cmd = (f.mods & keys::kCmd) != 0;  // a Clips sentence written with Cmd
#ifdef __APPLE__
  cmd = cmd || (km.profile() == keys::Profile::Live && !km.separateCmd());  // Clips on a Mac: Ctrl shows as Cmd
#endif
  if (cmd && (c.mods & keys::kCtrl) && !(c.mods & keys::kCmd)) c.mods = static_cast<uint8_t>((c.mods & ~keys::kCtrl) | keys::kCmd);
  return keycaps::format(c);
}

std::string fixedKey(const char* fixed) { return fixed ? formatText(fixed) : std::string(); }

std::string fill(const char* text, std::initializer_list<std::string> keyTexts) {
  const char* t = i18n::tr(text ? text : "");
  std::string out;
  auto next = keyTexts.begin();
  for (const char* q = t; *q; ++q) {
    if (q[0] == '%' && q[1] == 's') {
      if (next != keyTexts.end()) out += *next++;
      ++q;
      continue;
    }
    out += *q;
  }
  return out;
}

const char* withKeys(const char* text, std::initializer_list<std::string> keyTexts) {
  static std::unordered_set<std::string> kept;  // (elements never move: the pointers stay valid)
  return kept.insert(fill(text, keyTexts)).first->c_str();
}

std::string keyList(const std::string& text, bool byPlace) {
  if (text.find("eypad") != std::string::npos) return text;  // the keypad's keys: never renamed
  std::string out, tok;
  auto flush = [&] {
    const size_t plus = tok.size() > 1 ? tok.rfind('+', tok.size() - 2) : std::string::npos;
    out += plus != std::string::npos ? tok.substr(0, plus + 1) + oneKey(tok.substr(plus + 1), false) : oneKey(tok, byPlace);
    tok.clear();
  };
  for (char c : text) {
    if (c == ' ' || c == ',') {
      flush();
      out += c;
    } else {
      tok += c;
    }
  }
  flush();
  return out;
}

std::string placeName(int key) {
  const int position = TypingKeyboard::positionOfKey(key);
  const int typed = gCaps == TypingLayout::Computer ? systemChar(position) : TypingKeyboard::layoutChar(gCaps, position);
  return TypingKeyboard::keyLabel(key, typed);
}

}  // namespace gl::keycaps
