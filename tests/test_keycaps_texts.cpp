// Gridline: "My keys say" (GitHub #79) inside sentences. Tooltips, hints and status lines that name a
// key ("Metronome (Ctrl+M)") are written with a %s and get the key from keys::cmdKey (a command's key:
// follows the person's own binding) or keys::fixedKey (an editor's own key); the Tracker help's key
// column goes through trackerkeys::namedKeys. Runs from the source directory: it reads every such
// sentence out of src/app and src/packs.
//   1. The default (Same as my computer, no shortcut changed): in Studio's set every sentence reads exactly
//      as written before; in every set each command is named by its own key there (the written key when
//      the set gives it none), on a US and a Dvorak computer; the Tracker help rows read as written.
//   2. Each command a sentence names exists, and one of its sets binds it to the key the sentence says.
//   3. Dvorak typed on keys printed QWERTY: a sample of sentences shows the printed key.
//   4. A command bound to other keys shows its own key; unbound, "no key"; Cmd sentences keep Cmd.
// `gridline_tests_keycaps_texts <file>` also writes every sentence at the default to <file>.
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "../src/core/KeyCaps.h"
#include "../src/core/Keymap.h"
#include "../src/core/TrackerKeys.h"
#include "Check.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

struct KeyRef {
  std::string command;  // empty: a fixed key
  std::string key;
};
struct Sentence {
  std::string where;
  std::string text;  // with %s
  std::vector<KeyRef> keys;
  bool asString = false;  // fillKeys (else withKeys)
};

std::string readFile(const fs::path& p) {
  std::ifstream is(p, std::ios::binary);
  std::stringstream ss;
  ss << is.rdbuf();
  return ss.str();
}

void skipSpace(const std::string& s, size_t& i) {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
}
bool eat(const std::string& s, size_t& i, const char* what) {
  skipSpace(s, i);
  const std::string w = what;
  if (s.compare(i, w.size(), w) != 0) return false;
  i += w.size();
  return true;
}
int hexVal(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
// One string literal at i (C escapes undone), or false.
bool literal(const std::string& s, size_t& i, std::string* out) {
  skipSpace(s, i);
  if (i >= s.size() || s[i] != '"') return false;
  for (++i; i < s.size() && s[i] != '"'; ++i) {
    if (s[i] != '\\') {
      *out += s[i];
      continue;
    }
    const char e = s[++i];
    if (e == 'x') {
      int v = 0, n = 0;
      while (n < 2 && hexVal(s[i + 1]) >= 0) v = v * 16 + hexVal(s[++i]), ++n;
      *out += static_cast<char>(v);
    } else {
      *out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
    }
  }
  ++i;
  return true;
}
// Adjacent literals joined ("a" "b").
bool literals(const std::string& s, size_t& i, std::string* out) {
  if (!literal(s, i, out)) return false;
  size_t j = i;
  while (literal(s, j, out)) i = j;
  return true;
}
// keys::cmdKey("id", "Ctrl+M") or keys::fixedKey("Alt+Q") at i.
bool keyRef(const std::string& s, size_t& i, KeyRef* r) {
  size_t j = i;
  if (eat(s, j, "keys::cmdKey(")) {
    if (!literal(s, j, &r->command) || !eat(s, j, ",") || !literal(s, j, &r->key) || !eat(s, j, ")")) return false;
  } else if (eat(s, j, "keys::fixedKey(")) {
    if (!literal(s, j, &r->key) || !eat(s, j, ")")) return false;
  } else {
    return false;
  }
  i = j;
  return true;
}

struct Scan {
  std::vector<Sentence> sentences;
  std::vector<KeyRef> refs;  // every cmdKey / fixedKey with literal arguments, in a sentence or not
  std::vector<std::string> unread;  // withKeys / fillKeys calls this scan could not read
};

Scan scan() {
  Scan out;
  std::vector<fs::path> files;
  for (const char* dir : {"src/app", "src/packs"})
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
      const std::string ext = e.path().extension().string();
      if (ext == ".cpp" || ext == ".h" || ext == ".inc") files.push_back(e.path());
    }
  for (const fs::path& f : files) {
    const std::string s = readFile(f);
    for (size_t at = s.find("keys::"); at != std::string::npos; at = s.find("keys::", at + 1)) {
      size_t i = at;
      KeyRef r;
      if (keyRef(s, i, &r)) out.refs.push_back(r);
      const bool fillK = s.compare(at, 15, "keys::fillKeys(") == 0;
      if (!fillK && s.compare(at, 15, "keys::withKeys(") != 0) continue;
      i = at + 15;
      Sentence st;
      st.where = f.filename().string();
      st.asString = fillK;
      bool ok = (eat(s, i, "i18n::N_(") || eat(s, i, "N_(")) && literals(s, i, &st.text) && eat(s, i, ")") && eat(s, i, ",") && eat(s, i, "{");
      while (ok) {
        KeyRef k;
        if (!keyRef(s, i, &k)) break;
        st.keys.push_back(k);
        if (!eat(s, i, ",")) break;
      }
      ok = ok && eat(s, i, "}") && eat(s, i, ")");
      if (ok) out.sentences.push_back(st);
      else out.unread.push_back(st.where + ": " + s.substr(at, 60));
    }
  }
  return out;
}

// The sentence with each %s replaced by the key as written.
std::string asWritten(const Sentence& st) {
  std::string t, ks;
  size_t k = 0;
  for (size_t i = 0; i < st.text.size(); ++i) {
    if (st.text.compare(i, 2, "%s") == 0 && k < st.keys.size()) {
      t += st.keys[k++].key;
      ++i;
    } else {
      t += st.text[i];
    }
  }
  return t;
}

std::string shown(const Sentence& st) {
  std::vector<std::string> ks;
  for (const KeyRef& k : st.keys) ks.push_back(k.command.empty() ? keycaps::fixedKey(k.key.c_str()) : keycaps::cmdKey(k.command.c_str(), k.key.c_str()));
  // (initializer_list from up to three keys: what the sources use)
  switch (ks.size()) {
    case 0: return keycaps::fill(st.text.c_str(), {});
    case 1: return st.asString ? keycaps::fill(st.text.c_str(), {ks[0]}) : keycaps::withKeys(st.text.c_str(), {ks[0]});
    case 2: return keycaps::fill(st.text.c_str(), {ks[0], ks[1]});
    default: return keycaps::fill(st.text.c_str(), {ks[0], ks[1], ks[2]});
  }
}

const Sentence* find(const Scan& sc, const std::string& text) {
  for (const Sentence& st : sc.sentences)
    if (st.text == text) return &st;
  return nullptr;
}

uint8_t foldCmd(uint8_t m) { return (m & keys::kCmd) ? static_cast<uint8_t>((m & ~keys::kCmd) | keys::kCtrl) : m; }

// A sentence written with Cmd is a Clips sentence (shown only there, with Clips' set).
bool clipsOnly(const Sentence& st) {
  for (const KeyRef& k : st.keys)
    if (k.key.rfind("Cmd+", 0) == 0) return true;
  return false;
}
// The set a sentence shows with: Clips' for a Clips sentence, else Studio's.
void ownSet(const Sentence& st) { keys::keymap().setProfile(clipsOnly(st) ? keys::Profile::Live : keys::Profile::Studio); }

}  // namespace

int main(int argc, char** argv) {
  Scan sc = scan();
  // The LFO page's tab (TrackerFrame's page table keeps its command and key beside the hint).
  sc.sentences.push_back({"TrackerFrame.cpp (page table)", "The LFO board: the song's LFOs and the knobs they move (%s)", {{"view.lfoBoard", "Alt+F9"}}, false});
  sc.refs.push_back({"view.lfoBoard", "Alt+F9"});
  for (const std::string& u : sc.unread) std::printf("# unread: %s\n", u.c_str());
  expect(sc.unread.size() == 1 && sc.unread[0].rfind("TrackerFrame.cpp", 0) == 0, "every sentence in the sources is read (but the page table's)");
  std::printf("# %zu sentences, %zu keys\n", sc.sentences.size(), sc.refs.size());
  expect(sc.sentences.size() >= 50, "the sources' sentences are found");
  bool placeholders = true;
  for (const Sentence& st : sc.sentences) {
    size_t n = 0;
    for (size_t i = st.text.find("%s"); i != std::string::npos; i = st.text.find("%s", i + 2)) ++n;
    if (n != st.keys.size()) std::printf("# %s: %zu %%s, %zu keys: %s\n", st.where.c_str(), n, st.keys.size(), st.text.c_str());
    placeholders = placeholders && n == st.keys.size() && !st.keys.empty();
  }
  expect(placeholders, "each sentence has a %s for each of its keys");

  // 2. The commands are real, and a set binds each to the key its sentence names.
  bool real = true;
  for (const KeyRef& r : sc.refs) {
    const keys::Chord want = keys::parse(r.key);
    if (!want.valid()) {
      std::printf("# not a key: %s\n", r.key.c_str());
      real = false;
      continue;
    }
    if (r.command.empty()) continue;
    bool bound = false;
    for (int p = 0; p < static_cast<int>(keys::Profile::Count); ++p)
      for (bool sep : {false, true}) {
        const auto& d = keys::profileDefaults(static_cast<keys::Profile>(p), sep);
        const auto it = d.find(r.command);
        if (it == d.end()) continue;
        for (const keys::Chord& c : it->second) bound = bound || (c.key == want.key && foldCmd(c.mods) == foldCmd(want.mods));
      }
    if (!keys::findCommand(r.command) || !bound) std::printf("# %s is not bound to %s in any set\n", r.command.c_str(), r.key.c_str());
    real = real && keys::findCommand(r.command) && bound;
  }
  expect(real, "every command a sentence names exists and a shortcut set binds it to that key");

  // 1. The default: Studio's set gives every sentence as written; every set names each command by its
  // own key there (the written key when the set gives the command none), on a US and a Dvorak computer.
  std::ofstream dump;
  if (argc > 1) dump.open(argv[1], std::ios::binary);
  for (TypingLayout sys : {TypingLayout::Standard, TypingLayout::Dvorak}) {
    keycaps::simulateSystem(sys);
    bool studio = true, ownKey = true;
    for (int p = 0; p < static_cast<int>(keys::Profile::Count); ++p)
      for (bool sep : {false, true}) {
        const auto prof = static_cast<keys::Profile>(p);
        if (sep && prof != keys::Profile::Logic) continue;
        keys::keymap().setProfile(prof, sep);
        const std::string set = std::string(keys::profileId(prof)) + (sep ? "-cmd" : "");
        for (const Sentence& st : sc.sentences) {
          const std::string got = shown(st), want = asWritten(st);
          if (prof == (clipsOnly(st) ? keys::Profile::Live : keys::Profile::Studio)) {  // its own set: as written
            if (got != want) std::printf("# own set %s: \"%s\" for \"%s\"\n", st.where.c_str(), got.c_str(), want.c_str());
            studio = studio && got == want;
          }
          for (const KeyRef& k : st.keys) {
            if (k.command.empty()) continue;
            const std::string named = keycaps::cmdKey(k.command.c_str(), k.key.c_str());
            const std::vector<keys::Chord>& b = keys::keymap().bindings(k.command);
            const keys::Chord n = keys::parse(named);
            bool ok = b.empty() ? named == k.key : false;
            for (const keys::Chord& c : b) ok = ok || (c.key == n.key && foldCmd(c.mods) == foldCmd(n.mods));
            if (!ok) std::printf("# %s %s: %s shows \"%s\"\n", set.c_str(), st.where.c_str(), k.command.c_str(), named.c_str());
            ownKey = ownKey && ok;
          }
          if (dump && sys == TypingLayout::Standard && got != want && (!clipsOnly(st) || prof == keys::Profile::Live)) dump << set << "\t" << st.where << "\t" << want << "\t->\t" << got << "\n";
        }
      }
    expect(studio, (std::string("default: every sentence as written in Studio's set (a Clips sentence in Clips'), ") + typingLayoutName(sys) + " computer").c_str());
    expect(ownKey, (std::string("default: every set names each command by its own key, ") + typingLayoutName(sys) + " computer").c_str());
    bool help = true;
    for (const trackerkeys::Row& r : trackerkeys::rows()) {
      const std::string got = trackerkeys::namedKeys(r);
      if (got != r.keys) std::printf("# help %s: \"%s\"\n", r.id, got.c_str());
      help = help && got == r.keys;
    }
    expect(help, (std::string("default: the Tracker help's keys as written, ") + typingLayoutName(sys) + " computer").c_str());
  }
  keys::keymap().setProfile(keys::Profile::Studio);

  // 3. Dvorak typed on keys printed QWERTY.
  keycaps::simulateSystem(TypingLayout::Dvorak);
  keycaps::setCaps(TypingLayout::Standard);
  const struct {
    const char* text;
    const char* want;
  } kSample[] = {
      {"Stops the sample playing (%s)", "Stops the sample playing (Alt+X)"},
      {"Plays the whole sample at the play note (%s)", "Plays the whole sample at the play note (Alt+R)"},
      {"The range (or the whole sample) to the clipboard (%s)", "The range (or the whole sample) to the clipboard (Alt+I)"},
      {"The clipboard over the range, or in at the cursor (%s)", "The clipboard over the range, or in at the cursor (Alt+.)"},
      {"Typing keyboard to piano (%s, right-click: options)", "Typing keyboard to piano (Ctrl+K, right-click: options)"},
      {"Quantize note starts to the snap grid (%s)", "Quantize note starts to the snap grid (Ctrl+X)"},
      {"Used by File > New (%s) and at startup", "Used by File > New (Ctrl+L) and at startup"},
      {"Waveforms on the strips (%s)", "Waveforms on the strips (Alt+,)"},
      {"Folds to a small REC pill (%s)", "Folds to a small REC pill (J)"},
      {"Click Tap (or press %s) on every beat", "Click Tap (or press K) on every beat"},
      {"Pattern mode: play the selected pattern (%s)", "Pattern mode: play the selected pattern (P)"},
      {"Every track in view (%s): the tracks narrow as far as they need", "Every track in view (Ctrl+Alt+[): the tracks narrow as far as they need"},
      {"Wider tracks, fewer in view (%s or Ctrl+wheel up)", "Wider tracks, fewer in view (Ctrl+Alt+E or Ctrl+wheel up)"},
      {"Converted to a Tracker Sampler (%s: back)", "Converted to a Tracker Sampler (Ctrl+/: back)"},
      {"Stop: stops the track's clip (%s removes the button)", "Stop: stops the track's clip (Cmd+D removes the button)"},
      {"Loop start: the loop brace (drag it in the ruler, or %s on a time selection)", "Loop start: the loop brace (drag it in the ruler, or Cmd+P on a time selection)"},
      {"Transpose tools in the scope box (%s)", "Transpose tools in the scope box (Ctrl+Alt+K)"},
  };
  int good = 0;
  for (const auto& e : kSample) {
    const Sentence* st = find(sc, e.text);
    if (st) ownSet(*st);
    const std::string got = st ? shown(*st) : std::string("(not in the sources)");
    if (got != e.want) std::printf("# \"%s\": \"%s\"\n", e.text, got.c_str());
    good += got == e.want;
  }
  expect(good == static_cast<int>(sizeof kSample / sizeof kSample[0]) && good >= 10, "Dvorak + QWERTY keys: the sample sentences show the printed keys");
  // The Tracker help: the Sample editor's clipboard keys, the effect letters; the note keys by place.
  std::string clip, fx, notes;
  for (const trackerkeys::Row& r : trackerkeys::rows()) {
    if (std::string(r.id) == "sample.clipboard") clip = trackerkeys::namedKeys(r);
    if (std::string(r.id) == "pattern.effects") fx = trackerkeys::namedKeys(r);
    if (std::string(r.id) == "pattern.notes") notes = trackerkeys::namedKeys(r);
  }
  expect(clip == "Ctrl+B I ." && fx == "G U R K B T O" && notes == "Z S X D C ... /",
         ("Dvorak + QWERTY keys: the Tracker help (" + clip + " | " + fx + " | " + notes + ")").c_str());
  // Dvorak keys on a Dvorak computer: as written again (the printed keys type what they say).
  keycaps::setCaps(TypingLayout::Dvorak);
  bool dv = true;
  for (const Sentence& st : sc.sentences) {
    ownSet(st);
    dv = dv && shown(st) == asWritten(st);
  }
  keys::keymap().setProfile(keys::Profile::Studio);
  expect(dv, "Dvorak keys on a Dvorak computer: every sentence as written");
  // US computer + Dvorak keys: the notes are named after the Dvorak keys at their places.
  keycaps::simulateSystem(TypingLayout::Standard);
  for (const trackerkeys::Row& r : trackerkeys::rows())
    if (std::string(r.id) == "pattern.notes") notes = trackerkeys::namedKeys(r);
  expect(notes == "; O Q E J ... Z", ("US computer + Dvorak keys: the note keys by place (" + notes + ")").c_str());
  keycaps::setCaps(TypingLayout::Computer);
  keycaps::simulateSystem(TypingLayout::Computer);

  // 4. The person's own keys.
  const Sentence* quant = find(sc, "Quantize note starts to the snap grid (%s)");
  const Sentence* loop = find(sc, "Loop start: the loop brace (drag it in the ruler, or %s on a time selection)");
  expect(quant && loop, "the rebinding sentences are in the sources");
  if (quant && loop) {
    keys::Keymap& km = keys::keymap();
    km.set("piano.quickQuantize", {keys::Chord{keys::Key::U, keys::kCtrl | keys::kShift}});
    expect(shown(*quant) == "Quantize note starts to the snap grid (Ctrl+Shift+U)", "bound to other keys: its own key");
    km.set("piano.quickQuantize", {keys::Chord{keys::Key::F5, 0}, keys::Chord{keys::Key::Q, keys::kCtrl}});
    expect(shown(*quant) == "Quantize note starts to the snap grid (Ctrl+Q)", "still bound to the sentence's key as well: as written");
    km.set("piano.quickQuantize", {});
    expect(shown(*quant) == "Quantize note starts to the snap grid (no key)", "no key at all: says so");
    km.reset("piano.quickQuantize");
    expect(shown(*quant) == "Quantize note starts to the snap grid (Ctrl+Q)", "reset: as written");
    km.set("playlist.loopMarkers", {keys::Chord{keys::Key::K, keys::kCtrl | keys::kAlt}});
    expect(shown(*loop) == "Loop start: the loop brace (drag it in the ruler, or Cmd+Alt+K on a time selection)", "a Cmd sentence keeps Cmd for the new key");
    keycaps::simulateSystem(TypingLayout::Dvorak);
    keycaps::setCaps(TypingLayout::Standard);
    expect(shown(*loop) == "Loop start: the loop brace (drag it in the ruler, or Cmd+Alt+V on a time selection)", "and names it as the keys say");
    keycaps::setCaps(TypingLayout::Computer);
    keycaps::simulateSystem(TypingLayout::Computer);
    km.resetAll();
  }
  expect(keycaps::fill("%s and %s", {"A"}) == "A and " && keycaps::fill("100% of %s", {"B"}) == "100% of B", "fill: missing keys are empty, other % stay");
  return finish();
}
