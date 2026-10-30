// Gridline - command / shortcut registry (see Keymap.h).
#include "Keymap.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include "I18n.h"
#include "Json.h"

namespace gl::keys {

using i18n::N_;

namespace {

const char* const kKeyNames[] = {
    "",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Space", "Enter", "Esc", "Tab", "Backspace", "Ins", "Del", "Home", "End", "PgUp", "PgDn",
    "Up", "Down", "Left", "Right",
    "Num +", "Num -", "Num *", "Num /", "Num Enter",
    "Num 0", "Num 1", "Num 2", "Num 3", "Num 4", "Num 5", "Num 6", "Num 7", "Num 8", "Num 9",
    "-", "=", ",", ".", "/", ";", "'", "[", "]", "\\", "`",
};
static_assert(sizeof(kKeyNames) / sizeof(kKeyNames[0]) == static_cast<size_t>(Key::Count), "key name table");

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Chord ch(Key k, uint8_t m = 0) { return Chord{k, m}; }
constexpr uint8_t C = kCtrl, S = kShift, A = kAlt;

std::vector<Command> buildCommands() {
  using K = Key;
  const Context G = Context::Global, PL = Context::Playlist, PR = Context::PianoRoll, CR = Context::ChannelRack,
                MX = Context::Mixer, AU = Context::Automation;
  return {
      // ---- global: file ----
      {"file.new", G, N_("New project"), {ch(K::N, C)}},
      {"file.open", G, N_("Open project"), {ch(K::O, C)}},
      {"file.save", G, N_("Save"), {ch(K::S, C)}},
      {"file.saveAs", G, N_("Save as"), {ch(K::S, C | S)}},
      {"file.saveNewVersion", G, N_("Save new version"), {ch(K::S, C | A)}},
      {"file.render", G, N_("Render to audio file"), {ch(K::R, C)}},
      {"file.renderStems", G, N_("Render mixer tracks (stems)"), {ch(K::R, C | S)}},
      {"file.exportMidi", G, N_("Export song as MIDI file"), {ch(K::M, C | S)}},
      {"file.importUrl", G, N_("Import audio from a link (URL)"), {ch(K::U, C | S)}},  // J-192
      {"file.projectInfo", G, N_("Project info"), {ch(K::F11)}},
      {"file.quit", G, N_("Quit"), {ch(K::Q, C)}},
      // ---- global: edit ----
      {"edit.undo", G, N_("Undo"), {ch(K::Z, C)}},
      {"edit.redo", G, N_("Redo"), {ch(K::Y, C), ch(K::Z, C | S)}},
      {"edit.history", G, N_("Undo history"), {ch(K::Z, A)}},
      {"edit.rename", G, N_("Rename (pattern, channel or mixer track)"), {ch(K::F2)}},
      // ---- global: transport and switches ----
      {"transport.play", G, N_("Play / stop"), {ch(K::Space)}},
      {"transport.songMode", G, N_("Switch pattern / song mode"), {ch(K::L)}},
      {"options.typingKeyboard", G, N_("Typing keyboard to piano"), {ch(K::T, C)}},
      {"options.metronome", G, N_("Metronome"), {ch(K::M, C)}},
      {"tools.stopSound", G, N_("Stop all sound"), {ch(K::H, C)}},
      // ---- global: patterns ----
      {"pattern.new", G, N_("New pattern"), {ch(K::F4)}},
      {"pattern.findFirstEmpty", G, N_("Find first empty pattern"), {ch(K::F4, S)}},
      {"pattern.findNextEmpty", G, N_("Find next empty pattern"), {ch(K::F4, C)}},
      {"pattern.clone", G, N_("Clone pattern"), {ch(K::C, C | S)}},
      {"pattern.insert", G, N_("Insert pattern"), {ch(K::Insert, C | S)}},
      {"pattern.delete", G, N_("Delete pattern"), {ch(K::Delete, C | S)}},
      {"pattern.moveUp", G, N_("Move pattern up"), {ch(K::Up, C | S)}, true},
      {"pattern.moveDown", G, N_("Move pattern down"), {ch(K::Down, C | S)}, true},
      {"pattern.next", G, N_("Next pattern"), {ch(K::NumAdd)}, true},
      {"pattern.previous", G, N_("Previous pattern"), {ch(K::NumSubtract)}, true},
      // ---- global: windows ----
      {"view.playlist", G, N_("Playlist"), {ch(K::F5)}},
      {"view.channelRack", G, N_("Channel rack"), {ch(K::F6)}},
      {"view.pianoRoll", G, N_("Piano roll"), {ch(K::F7)}},
      {"view.pluginPicker", G, N_("Plugin picker"), {ch(K::F8)}},
      {"view.browser", G, N_("Browser"), {ch(K::F8, A)}},
      {"view.mixer", G, N_("Mixer"), {ch(K::F9)}},
      {"view.settings", G, N_("Settings (MIDI page)"), {ch(K::F10)}},
      {"view.closeAll", G, N_("Close all windows"), {ch(K::F12)}},
      {"view.closePlugins", G, N_("Close plugin windows"), {ch(K::F12, A)}},
      {"view.resetLayout", G, N_("Reset window layout"), {ch(K::H, C | S)}},
      {"help.shortcuts", G, N_("Keyboard shortcuts"), {ch(K::F1)}},
      // ---- playlist ----
      {"playlist.delete", PL, N_("Delete selected clips"), {ch(K::Delete), ch(K::Backspace)}},
      {"playlist.selectAll", PL, N_("Select all clips"), {ch(K::A, C)}},
      {"playlist.deselect", PL, N_("Deselect"), {ch(K::D, C)}},
      {"playlist.copy", PL, N_("Copy clips"), {ch(K::C, C)}},
      {"playlist.paste", PL, N_("Paste clips"), {ch(K::V, C)}},
      {"playlist.duplicate", PL, N_("Duplicate selected clips"), {ch(K::B, C)}},
      {"playlist.merge", PL, N_("Merge selected clips"), {ch(K::G, C)}},
      {"playlist.mute", PL, N_("Mute / unmute selected clips"), {ch(K::M)}},
      {"playlist.toolDraw", PL, N_("Draw tool"), {ch(K::P)}},
      {"playlist.toolPaint", PL, N_("Paint tool"), {ch(K::B)}},
      {"playlist.toolDelete", PL, N_("Delete tool"), {ch(K::D)}},
      {"playlist.toolMute", PL, N_("Mute tool"), {ch(K::T)}},
      {"playlist.toolSlip", PL, N_("Slip tool"), {ch(K::S)}},
      {"playlist.toolSlice", PL, N_("Slice tool"), {ch(K::C)}},
      {"playlist.toolSelect", PL, N_("Select tool"), {ch(K::E)}},
      {"playlist.toolZoom", PL, N_("Zoom tool"), {ch(K::Z)}},
      {"playlist.toolPlay", PL, N_("Playback tool"), {ch(K::Y)}},
      {"playlist.marker", PL, N_("Add a marker at the song position"), {ch(K::T, A)}},
      {"playlist.loopMarkers", PL, N_("Place loop: around the time selection, else a loop marker at the song position"), {ch(K::T, S)}},
      {"playlist.stretchResize", PL, N_("Resizing audio clips stretches them (on / off)"), {ch(K::M, S)}},
      {"playlist.fades", PL, N_("Show fade and volume handles on audio clips (on / off)"), {ch(K::F, S)}},
      {"playlist.zeroCross", PL, N_("Audio clip edits land on zero crossings (on / off)"), {ch(K::X, S)}},  // J-79 (FL has no key for it)
      {"playlist.picker", PL, N_("Picker panel"), {ch(K::P, A)}},
      {"playlist.quickZoom1", PL, N_("Quick zoom 1"), {ch(K::D1, S)}},
      {"playlist.quickZoom2", PL, N_("Quick zoom 2"), {ch(K::D2, S)}},
      {"playlist.quickZoom3", PL, N_("Quick zoom 3"), {ch(K::D3, S)}},
      {"playlist.zoomSelection", PL, N_("Zoom to the selection"), {ch(K::D4, S)}},
      // PL-13: chop, stutter and time selection editing.
      {"playlist.chopWindow", PL, N_("Chop and stutter window"), {ch(K::C, A)}},
      {"playlist.sliceGrid", PL, N_("Slice selected clips at the snap grid"), {ch(K::C, S)}},
      {"playlist.duplicateTime", PL, N_("Duplicate the time selection"), {ch(K::B, A)}},
      {"playlist.insertSpace", PL, N_("Insert space at the time selection"), {ch(K::Insert, C)}},
      {"playlist.deleteSpace", PL, N_("Delete the time selection's space"), {ch(K::Delete, C)}},
      {"playlist.consolidateTime", PL, N_("Consolidate the time selection to audio"), {ch(K::C, C | A)}},
      {"playlist.zoomIn", PL, N_("Zoom in"), {ch(K::PageUp)}, true},
      {"playlist.zoomOut", PL, N_("Zoom out"), {ch(K::PageDown)}, true},
      {"playlist.zoomFit", PL, N_("Zoom to fit the song"), {ch(K::Home)}},
      {"playlist.uniqueCopy", PL, N_("While Shift+dragging copies: make them unique (new patterns)"), {ch(K::V, S)}},  // Q-3 (FL: drag + Shift+V)
      {"playlist.cancelDrag", PL, N_("Cancel the clip drag (copies, moves, resizes)"), {ch(K::Escape)}},  // Q-3
      // ---- piano roll ----
      {"piano.delete", PR, N_("Delete selected notes"), {ch(K::Delete), ch(K::Backspace)}},
      {"piano.selectAll", PR, N_("Select all notes"), {ch(K::A, C)}},
      {"piano.deselect", PR, N_("Deselect"), {ch(K::D, C)}},
      {"piano.copy", PR, N_("Copy notes"), {ch(K::C, C)}},
      {"piano.cut", PR, N_("Cut notes"), {ch(K::X, C)}},
      {"piano.paste", PR, N_("Paste notes"), {ch(K::V, C)}},
      {"piano.duplicate", PR, N_("Duplicate notes"), {ch(K::B, C)}},
      {"piano.cancelDrag", PR, N_("Cancel the note drag (copies, moves, resizes)"), {ch(K::Escape)}},  // Q-3
      {"piano.quickQuantize", PR, N_("Quick quantize start times"), {ch(K::Q, C)}},
      {"piano.transposeUp", PR, N_("Transpose up a semitone"), {ch(K::Up)}, true},
      {"piano.transposeDown", PR, N_("Transpose down a semitone"), {ch(K::Down)}, true},
      {"piano.octaveUp", PR, N_("Transpose up an octave"), {ch(K::Up, S)}, true},
      {"piano.octaveDown", PR, N_("Transpose down an octave"), {ch(K::Down, S)}, true},
      {"piano.moveRight", PR, N_("Move notes right by the snap"), {ch(K::Right)}, true},
      {"piano.moveLeft", PR, N_("Move notes left by the snap"), {ch(K::Left)}, true},
      {"piano.legato", PR, N_("Legato"), {ch(K::L, C)}},
      {"piano.chopSteps", PR, N_("Chop into steps"), {ch(K::U, C)}},
      {"piano.glue", PR, N_("Glue notes"), {ch(K::G, C)}},
      {"piano.quantize", PR, N_("Quantize tool"), {ch(K::Q, A)}},
      {"piano.randomize", PR, N_("Randomize tool"), {ch(K::R, A)}},
      {"piano.chop", PR, N_("Chop tool"), {ch(K::U, A)}},
      {"piano.strum", PR, N_("Strum tool"), {ch(K::S, A)}},
      {"piano.arpeggiate", PR, N_("Arpeggiate tool"), {ch(K::A, A)}},
      {"piano.articulate", PR, N_("Articulate tool"), {ch(K::L, A)}},
      {"piano.limit", PR, N_("Limit key range tool"), {ch(K::K, A)}},
      {"piano.lfo", PR, N_("Note LFO tool"), {ch(K::O, A)}},
      {"piano.scaleLevels", PR, N_("Scale levels tool"), {ch(K::X, A)}},
      {"piano.flam", PR, N_("Grace notes tool"), {ch(K::G, A)}},
      {"piano.properties", PR, N_("Note properties"), {ch(K::N, A)}},
      {"piano.humanize", PR, N_("Humanize"), {ch(K::R, A | S)}},
      {"piano.invertSelection", PR, N_("Invert selection"), {ch(K::I, S)}},
      {"piano.selectRandom", PR, N_("Select random notes"), {ch(K::R, S)}},
      {"piano.toolDraw", PR, N_("Draw tool"), {ch(K::P)}},
      {"piano.toolPaint", PR, N_("Paint tool"), {ch(K::B)}},
      {"piano.toolDelete", PR, N_("Delete tool"), {ch(K::D)}},
      {"piano.toolSelect", PR, N_("Select tool"), {ch(K::E)}},
      {"piano.toolSlice", PR, N_("Slice tool"), {ch(K::C)}},
      {"piano.toolMute", PR, N_("Mute tool"), {ch(K::T)}},
      {"piano.toolPlay", PR, N_("Playback tool"), {ch(K::Y)}},
      {"piano.toolZoom", PR, N_("Zoom tool"), {ch(K::Z)}},
      {"piano.toolDetune", PR, N_("Detune tool"), {ch(K::U)}},
      {"piano.zoomIn", PR, N_("Zoom in"), {ch(K::PageUp)}, true},
      {"piano.zoomOut", PR, N_("Zoom out"), {ch(K::PageDown)}, true},
      {"piano.zoomFit", PR, N_("Zoom to fit the pattern"), {ch(K::Home)}},
      // ---- channel rack ----
      {"rack.clone", CR, N_("Clone selected channels"), {ch(K::C, A)}},
      {"rack.randomColours", CR, N_("Random colours for selected channels"), {ch(K::C, C | A)}},
      {"rack.delete", CR, N_("Delete selected channels"), {ch(K::Delete, A)}},
      {"rack.moveUp", CR, N_("Move selected channels up"), {ch(K::Up, A)}, true},
      {"rack.moveDown", CR, N_("Move selected channels down"), {ch(K::Down, A)}, true},
      {"rack.routeFree", CR, N_("Route selected channels to free mixer tracks"), {ch(K::L, C)}},
      {"rack.zip", CR, N_("Zip selected channels"), {ch(K::Z, A)}},
      {"rack.unzipAll", CR, N_("Unzip all channels"), {ch(K::U, A)}},
      {"rack.rotateLeft", CR, N_("Rotate steps left"), {ch(K::Left, C | S)}, true},
      {"rack.rotateRight", CR, N_("Rotate steps right"), {ch(K::Right, C | S)}, true},
      {"rack.copy", CR, N_("Copy"), {ch(K::C, C)}},
      {"rack.cut", CR, N_("Cut"), {ch(K::X, C)}},
      {"rack.paste", CR, N_("Paste"), {ch(K::V, C)}},
      {"rack.selectAll", CR, N_("Select all channels"), {ch(K::A, C)}},
      {"rack.graphEditor", CR, N_("Graph editor (step velocities)"), {ch(K::K, C)}},
      {"rack.insertStep", CR, N_("Insert a step (over a step)"), {ch(K::Insert)}},
      {"rack.deleteStep", CR, N_("Remove a step (over a step)"), {ch(K::Delete)}},
      // ---- mixer ----
      {"mixer.routeHere", MX, N_("Route selected channels to this track"), {ch(K::L, C)}},
      {"mixer.routeFromHere", MX, N_("Route selected channels from this track on"), {ch(K::L, C | S)}},
      {"mixer.moveLeft", MX, N_("Move track left"), {ch(K::Left, A)}, true},
      {"mixer.moveRight", MX, N_("Move track right"), {ch(K::Right, A)}, true},
      {"mixer.solo", MX, N_("Solo selected track"), {ch(K::S)}},
      {"mixer.renderArmed", MX, N_("Render armed tracks"), {ch(K::R, A)}},
      {"mixer.renderSelected", MX, N_("Render selected tracks"), {ch(K::R, A | S)}},
      {"mixer.waveforms", MX, N_("Show waveforms"), {ch(K::W, A)}},
      // ---- automation clip editor ----
      {"auto.delete", AU, N_("Delete selected points"), {ch(K::Delete), ch(K::Backspace)}},
      {"auto.selectAll", AU, N_("Select all points"), {ch(K::A, C)}},
      {"auto.deselect", AU, N_("Deselect"), {ch(K::D, C)}},
      {"auto.invertSelection", AU, N_("Invert selection"), {ch(K::I, C)}},
      {"auto.copy", AU, N_("Copy points"), {ch(K::C, C)}},
      {"auto.cut", AU, N_("Cut points"), {ch(K::X, C)}},
      {"auto.paste", AU, N_("Paste points"), {ch(K::V, C)}},
      {"auto.pasteReplace", AU, N_("Paste, replacing points"), {ch(K::R, C)}},
      {"auto.toolDraw", AU, N_("Draw tool"), {ch(K::P)}},
      {"auto.toolSelect", AU, N_("Select tool"), {ch(K::E)}},
      {"auto.toolDelete", AU, N_("Delete tool"), {ch(K::D)}},
      {"auto.lfo", AU, N_("LFO tool"), {ch(K::L)}},
      {"auto.quantize", AU, N_("Quantize points"), {ch(K::Q)}},
      {"auto.cancel", AU, N_("Close the tool / clear the selection"), {ch(K::Escape)}},
  };
}

const std::vector<Chord> kNone;

}  // namespace

const char* keyName(Key k) {
  size_t i = static_cast<size_t>(k);
  return i < static_cast<size_t>(Key::Count) ? kKeyNames[i] : "";
}

Key keyFromName(const std::string& name) {
  std::string n = lower(name);
  if (n.empty()) return Key::None;
  for (size_t i = 1; i < static_cast<size_t>(Key::Count); ++i)
    if (lower(kKeyNames[i]) == n) return static_cast<Key>(i);
  // A few spellings people type.
  if (n == "escape") return Key::Escape;
  if (n == "insert") return Key::Insert;
  if (n == "delete") return Key::Delete;
  if (n == "pageup") return Key::PageUp;
  if (n == "pagedown") return Key::PageDown;
  if (n == "return") return Key::Enter;
  return Key::None;
}

std::string format(const Chord& c) {
  if (!c.valid()) return {};
  std::string s;
  if (c.mods & kCtrl) s += "Ctrl+";
  if (c.mods & kAlt) s += "Alt+";
  if (c.mods & kShift) s += "Shift+";
  return s + keyName(c.key);
}

Chord parse(const std::string& text) {
  Chord c;
  std::string rest = text;
  for (;;) {
    std::string l = lower(rest);
    auto strip = [&](const char* p, uint8_t m) {
      size_t n = std::char_traits<char>::length(p);
      if (l.size() > n && l.compare(0, n, p) == 0) {
        c.mods |= m;
        rest = rest.substr(n);
        return true;
      }
      return false;
    };
    if (strip("ctrl+", kCtrl) || strip("alt+", kAlt) || strip("shift+", kShift)) continue;
    break;
  }
  c.key = keyFromName(rest);
  if (!c.valid()) c.mods = 0;
  return c;
}

const char* contextName(Context c) {
  switch (c) {
    case Context::Global: return N_("Global");
    case Context::Playlist: return N_("Playlist");
    case Context::PianoRoll: return N_("Piano roll");
    case Context::ChannelRack: return N_("Channel rack");
    case Context::Mixer: return N_("Mixer");
    case Context::Automation: return N_("Automation clip editor");
    default: return "";
  }
}

const std::vector<Command>& commands() {
  static const std::vector<Command> c = buildCommands();
  return c;
}

const Command* findCommand(const std::string& id) {
  static const std::unordered_map<std::string, const Command*> index = [] {
    std::unordered_map<std::string, const Command*> m;
    for (const auto& c : commands()) m.emplace(c.id, &c);
    return m;
  }();
  auto it = index.find(id);
  return it == index.end() ? nullptr : it->second;
}

const std::vector<Chord>& Keymap::bindings(const std::string& id) const {
  auto it = overrides_.find(id);
  if (it != overrides_.end()) return it->second;
  const Command* c = findCommand(id);
  return c ? c->defaults : kNone;
}

bool Keymap::set(const std::string& id, std::vector<Chord> chords) {
  const Command* c = findCommand(id);
  if (!c) return false;
  std::vector<Chord> clean;
  for (const auto& x : chords)
    if (x.valid() && std::find(clean.begin(), clean.end(), x) == clean.end()) clean.push_back(x);
  if (clean == c->defaults)
    overrides_.erase(id);
  else
    overrides_[id] = std::move(clean);
  ++version_;
  return true;
}

void Keymap::reset(const std::string& id) {
  if (overrides_.erase(id)) ++version_;
}

void Keymap::resetAll() {
  overrides_.clear();
  ++version_;
}

bool Keymap::customized(const std::string& id) const { return overrides_.count(id) != 0; }

std::vector<const Command*> Keymap::lookup(Context ctx, const Chord& chord) const {
  std::vector<const Command*> out;
  if (!chord.valid()) return out;
  for (const auto& c : commands()) {
    if (c.context != ctx) continue;
    const auto& b = bindings(c.id);
    if (std::find(b.begin(), b.end(), chord) != b.end()) out.push_back(&c);
  }
  return out;
}

std::vector<const Command*> Keymap::conflicts(const std::string& id, const Chord& chord) const {
  std::vector<const Command*> out;
  const Command* self = findCommand(id);
  if (!self) return out;
  for (const Command* c : lookup(self->context, chord))
    if (id != c->id) out.push_back(c);
  return out;
}

std::vector<const Command*> Keymap::shadows(const std::string& id, const Chord& chord) const {
  std::vector<const Command*> out;
  const Command* self = findCommand(id);
  if (!self || !chord.valid()) return out;
  for (const auto& c : commands()) {
    if (c.context == self->context) continue;
    if (self->context != Context::Global && c.context != Context::Global) continue;  // editors never overlap
    const auto& b = bindings(c.id);
    if (std::find(b.begin(), b.end(), chord) != b.end()) out.push_back(&c);
  }
  return out;
}

std::vector<std::string> Keymap::allConflicts() const {
  std::vector<std::string> out;
  const auto& cmds = commands();
  for (size_t i = 0; i < cmds.size(); ++i)
    for (const auto& chord : bindings(cmds[i].id))
      for (size_t j = i + 1; j < cmds.size(); ++j) {
        if (cmds[j].context != cmds[i].context) continue;
        const auto& b = bindings(cmds[j].id);
        if (std::find(b.begin(), b.end(), chord) != b.end())
          out.push_back(std::string(cmds[i].id) + "|" + cmds[j].id + "|" + format(chord));
      }
  return out;
}

std::vector<std::string> Keymap::assign(const std::string& id, const Chord& chord, bool replaceAll) {
  std::vector<std::string> lost;
  if (!findCommand(id) || !chord.valid()) return lost;
  for (const Command* c : conflicts(id, chord)) {
    std::vector<Chord> b = bindings(c->id);
    b.erase(std::remove(b.begin(), b.end(), chord), b.end());
    set(c->id, b);
    lost.push_back(c->id);
  }
  std::vector<Chord> mine = replaceAll ? std::vector<Chord>{} : bindings(id);
  mine.push_back(chord);
  set(id, mine);
  return lost;
}

std::string Keymap::serialize() const {
  if (overrides_.empty()) return {};  // the settings default: no overrides
  Json j = Json::makeObject();
  for (const auto& [id, chords] : overrides_) {
    Json arr = Json::makeArray();
    for (const auto& c : chords) arr.array.push_back(Json::makeString(format(c)));
    j.object[id] = std::move(arr);
  }
  return jsonStringify(j, 0);
}

bool Keymap::load(const std::string& json, std::string* err) {
  overrides_.clear();
  ++version_;
  if (json.empty()) return true;
  Json j;
  if (!jsonParse(json, j) || !j.is(Json::Type::Object)) {
    if (err) *err = "shortcut map is not a JSON object";
    return false;
  }
  for (const auto& [id, v] : j.object) {
    const Command* c = findCommand(id);
    if (!c || !v.is(Json::Type::Array)) continue;
    std::vector<Chord> chords;
    for (const auto& x : v.array)
      if (x.is(Json::Type::String))
        if (Chord cc = parse(x.str); cc.valid()) chords.push_back(cc);
    if (chords != c->defaults) overrides_[id] = std::move(chords);
  }
  return true;
}

Keymap& keymap() {
  static Keymap k;
  return k;
}

const std::vector<MouseModifier>& mouseModifiers() {
  static const std::vector<MouseModifier> v = {
      {"piano.laneSelectDrag", Context::PianoRoll, N_("Event lane: drag selects notes"), kCtrl},
      {"piano.laneDefaultDrag", Context::PianoRoll, N_("Event lane: drag sets the default value"), kAlt},
      // Q-8: the reference moves an effect with the plain wheel over its slot (no modifier).
      {"mixer.slotWheelMove", Context::Mixer, N_("Effect slot: mouse wheel moves the effect up / down"), 0},
      // J-83: while the mixer scrolls (thin layout, or Settings > Interface "scroll") the plain
      // wheel scrolls it; with this held the wheel adjusts the control under the pointer.
      // J-84: the same modifier adjusts channel rack knobs, step velocities and keyboard-lane notes while the rack scrolls.
      {"mixer.wheelAdjust", Context::Mixer, N_("Mixer / channel rack control: mouse wheel adjusts it while the pane scrolls"), kCtrl},
  };
  return v;
}

uint8_t mouseModifier(const std::string& id) {
  for (const auto& m : mouseModifiers())
    if (id == m.id) return m.mods;
  return 0;
}

}  // namespace gl::keys
