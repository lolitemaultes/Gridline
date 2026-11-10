#include "MidiRecord.h"
#include <algorithm>
#include <cmath>
#include "Automation.h"
#include "AutomationEdit.h"
#include "PatternAutomation.h"
#include "Timeline.h"
#include "TrackModes.h"

namespace gl {

namespace {
// K-171: recorded controller moves are thinned to this error (value units), as
// recording a knob's moves already is: inaudible, and far fewer points.
constexpr double kRecordThin = 0.004;
}  // namespace

const char* autoRecordTargetName(AutoRecordTarget t) {
  static const char* n[] = {"Automatic", "Pattern events", "Automation clips"};
  int i = static_cast<int>(t);
  return i >= 0 && i < static_cast<int>(AutoRecordTarget::Count) ? n[i] : "?";
}

Tick quantizeTick(Tick t, Tick grid) {
  if (grid <= 1) return t;
  if (t >= 0) return (t + grid / 2) / grid * grid;
  return -quantizeTick(-t, grid);
}

// ------------------------------------------------------------ NoteRecorder ----

void NoteRecorder::beginPattern(Document& doc, ObjectId pattern, const RecordOptions& o, Tick loopLength) {
  *this = NoteRecorder{};
  const Pattern* p = doc.song().pattern(pattern);
  if (!p) return;
  active_ = true;
  opt_ = o;
  base_ = pattern_ = pattern;
  baseContent_ = *p;
  loop_ = std::max<Tick>(1, loopLength);
  takeIds_ = {pattern};
}

void NoteRecorder::beginSong(Document& doc, ObjectId pattern, const RecordOptions& o) {
  *this = NoteRecorder{};
  const Pattern* p = doc.song().pattern(pattern);
  if (!p) return;
  active_ = true;
  song_ = true;
  opt_ = o;
  base_ = pattern_ = pattern;
  baseContent_ = *p;
  takeIds_ = {pattern};
}

void NoteRecorder::position(Document& doc, double tick) {
  if (!active_) return;
  if (!song_ && last_ >= 0 && tick < last_ - 0.5) newPass(doc);  // the loop wrapped
  last_ = tick;
}

void NoteRecorder::newPass(Document& doc) {
  commitAutomation(doc);
  ++pass_;
  cleared_.clear();
  if (!opt_.takes || song_) return;
  const Song& s = doc.song();
  if (static_cast<int>(s.patterns.size()) >= kMaxPatterns) return;
  std::string name = baseContent_.name + " take " + std::to_string(takeIds_.size() + 1);
  ObjectId id = doc.addPattern(name, baseContent_.bars, baseContent_.stepsPerBar);
  if (id == kNoId) return;
  const Pattern base = baseContent_;
  doc.editPattern(id, "new take", [&base](Pattern& p) {
    p.color = base.color;
    p.steps = base.steps;
    p.notes = base.notes;
    p.automation = base.automation;
  });
  takeIds_.push_back(id);
  pattern_ = id;
}

Tick NoteRecorder::songOrigin(Document& doc, double tick, ObjectId channel) {
  const Song& s = doc.song();
  const Pattern* p = s.pattern(pattern_);
  if (!p) return 0;
  for (const auto& c : s.playlist) {
    if (c.kind != ClipKind::Pattern || c.pattern != pattern_) continue;
    Tick len = s.clipLength(c);
    if (tick >= static_cast<double>(c.start) && tick < static_cast<double>(c.start + len)) return c.start - c.offset;
  }
  // No clip of the pattern here: place one on the bar, on the first free track.
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick start = static_cast<Tick>(std::floor(tick / static_cast<double>(bar))) * bar;
  const Tick len = std::max<Tick>(bar, s.patternLength(*p));
  int track = trackmodes::instrumentTrackOf(s, channel);  // PL-08
  if (track < 0) {
    for (track = 0; track < static_cast<int>(s.tracks.size()); ++track) {
      if (s.tracks[static_cast<size_t>(track)].mode != TrackMode::Unassigned) continue;  // audio / other instruments' tracks
      bool busy = false;
      for (const auto& c : s.playlist)
        if (c.track == track && c.start < start + len && c.start + s.clipLength(c) > start) busy = true;
      if (!busy) break;
    }
  }
  if (track >= kMaxPlaylistTracks) track = 0;
  doc.addClip(pattern_, track, start);
  return start;
}

void NoteRecorder::noteOn(Document& doc, ObjectId channel, int pitch, int velocity, double tick) {
  if (!active_ || !opt_.notes || !doc.song().channel(channel)) return;
  position(doc, tick);
  auto key = std::make_pair(channel, pitch);
  if (auto it = held_.find(key); it != held_.end()) {  // retrigger: close the previous one
    commitNote(doc, it->second, pitch, tick);
    held_.erase(it);
  }
  Held h{channel, std::clamp(velocity, 1, 127), tick, pass_, pattern_, 0};
  if (song_) h.songOrigin = songOrigin(doc, tick, channel);
  held_[key] = h;
}

void NoteRecorder::noteOff(Document& doc, ObjectId channel, int pitch, double tick) {
  if (!active_) return;
  position(doc, tick);
  auto it = held_.find({channel, pitch});
  if (it == held_.end()) return;
  Held h = it->second;
  held_.erase(it);
  commitNote(doc, h, pitch, tick);
}

void NoteRecorder::commitNote(Document& doc, const Held& h, int pitch, double endTick) {
  const Song& s = doc.song();
  if (!s.pattern(h.pattern) || !s.channel(h.channel)) return;
  double start, len;
  if (song_) {
    start = h.start - static_cast<double>(h.songOrigin);
    len = endTick - h.start;
  } else {
    double end = endTick;
    int wraps = pass_ - h.pass;
    if (wraps > 0 || end < h.start) end += static_cast<double>(loop_) * std::max(1, wraps);
    start = h.start;
    len = std::min(end - h.start, static_cast<double>(loop_));
  }
  const Tick g = std::max<Tick>(1, opt_.grid);
  Tick qs = static_cast<Tick>(std::llround(std::max(0.0, start)));
  if (opt_.quantize) qs = quantizeTick(qs, g);
  if (!song_ && loop_ > 0) qs %= loop_;
  Tick ql = std::max<Tick>(1, static_cast<Tick>(std::llround(len)));
  if (opt_.quantizeLength) ql = std::max(g, quantizeTick(ql, g));
  if (!opt_.overdub && !cleared_.count(h.channel)) {
    cleared_.insert(h.channel);
    doc.setNotes(h.pattern, h.channel, {}, "record (replace)");
  }
  doc.addNote(h.pattern, h.channel, Note(qs, ql, pitch, h.velocity));
  ++recorded_;
}

void NoteRecorder::controller(ParamId target, double normalized, double tick) {
  if (!active_ || !opt_.automation) return;
  auto& v = auto_[target.packed()];
  if (!v.empty() && v.back().first == tick) {
    v.back().second = normalized;
    return;
  }
  if (v.size() < 100000) v.push_back({tick, std::clamp(normalized, 0.0, 1.0)});
}

void NoteRecorder::commitAutomation(Document& doc) {
  if (auto_.empty()) return;
  const bool clips = opt_.autoTarget == AutoRecordTarget::AutomationClips ||
                     (opt_.autoTarget == AutoRecordTarget::Auto && song_);
  auto pending = std::move(auto_);
  auto_.clear();
  doc.beginGroup("record automation");
  for (auto& entry : pending) {
    auto& pts = entry.second;
    const uint64_t packed = entry.first;
    if (pts.empty()) continue;
    const ParamId pid = ParamId::unpack(packed);
    if (!describeParam(doc.song(), pid)) continue;
    std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (clips) {
      // A new automation clip from the bar of the first move.
      const Song& s = doc.song();
      const Tick bar = std::max<Tick>(1, s.barTicks());
      const Tick first = static_cast<Tick>(std::floor(pts.front().first));
      const Tick last = static_cast<Tick>(std::ceil(pts.back().first));
      const Tick start = std::max<Tick>(0, first / bar * bar);
      std::string name = paramDisplayName(s, pid);
      doc.editSong("record automation", [&](Song& so) {
        AutomationClip ac;
        ac.id = so.allocateId();
        ac.name = name;
        ac.owner = pid.owner;
        ac.index = pid.index;
        for (const auto& [t, v] : pts) {
          AutoPoint p;
          p.pos = std::max<Tick>(0, static_cast<Tick>(std::llround(t)) - start);
          p.value = static_cast<float>(v);
          p.shape = AutoShape::Hold;
          if (!ac.points.empty() && ac.points.back().pos == p.pos)
            ac.points.back() = p;
          else
            ac.points.push_back(p);
        }
        int track = 0;
        for (; track < static_cast<int>(so.tracks.size()); ++track) {
          if (!trackmodes::clipAllowed(so, ClipKind::Automation, track)) continue;  // PL-08: not on instrument tracks
          bool busy = false;
          for (const auto& c : so.playlist)
            if (c.track == track && c.start <= last && c.start + so.clipLength(c) > start) busy = true;
          if (!busy) break;
        }
        if (track >= static_cast<int>(so.tracks.size())) {
          if (static_cast<int>(so.tracks.size()) >= kMaxPlaylistTracks) return;
          so.tracks.resize(static_cast<size_t>(track) + 1);
        }
        Clip c;
        c.id = so.allocateId();
        c.kind = ClipKind::Automation;
        c.source = ac.id;
        c.track = track;
        c.start = start;
        c.length = std::max<Tick>(bar, (last - start + bar) / bar * bar);
        ac.points = autoedit::thin(ac.points, kRecordThin);  // K-171: a few points, within 0.4 %
        so.automations.push_back(std::move(ac));
        so.playlist.push_back(c);
      });
    } else {
      // Pattern events: replace the lane's points inside the recorded span.
      Tick origin = 0;
      if (song_) origin = songOrigin(doc, pts.front().first);
      const Tick t0 = static_cast<Tick>(std::floor(pts.front().first)) - origin;
      const Tick t1 = static_cast<Tick>(std::ceil(pts.back().first)) - origin;
      doc.editPattern(pattern_, "record automation", [&](Pattern& p) {
        std::vector<AutoPoint> out, take;
        const PatternAutomation* lane = findPatternLane(p, pid.owner, pid.index);
        if (lane)
          for (const auto& q : lane->points)
            if (q.pos < t0 || q.pos > t1) out.push_back(q);
        for (const auto& [t, v] : pts) {
          AutoPoint q;
          q.pos = std::max<Tick>(0, static_cast<Tick>(std::llround(t)) - origin);
          q.value = static_cast<float>(lane ? paramToAutomation(*lane, v) : v);  // J-173: into the clip's range
          q.shape = AutoShape::Hold;
          if (!take.empty() && take.back().pos == q.pos)
            take.back() = q;  // one point per tick (the last value wins)
          else
            take.push_back(q);
        }
        take = autoedit::thin(take, kRecordThin);  // K-171: a few points, within 0.4 %
        out.insert(out.end(), take.begin(), take.end());
        std::stable_sort(out.begin(), out.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.pos < b.pos; });
        // One point per tick (the last value wins).
        std::vector<AutoPoint> dedup;
        for (const auto& q : out) {
          if (!dedup.empty() && dedup.back().pos == q.pos)
            dedup.back() = q;
          else
            dedup.push_back(q);
        }
        setPatternLane(p, pid.owner, pid.index, std::move(dedup));
      });
    }
  }
  doc.endGroup();
}

void NoteRecorder::end(Document& doc, double tick) {
  if (!active_) return;
  position(doc, tick);
  auto held = std::move(held_);
  held_.clear();
  for (const auto& [key, h] : held) commitNote(doc, h, key.second, tick);
  commitAutomation(doc);
  active_ = false;
}

// ------------------------------------------------------------ StepRecorder ----

void StepRecorder::press(int pitch, int velocity) {
  down_.insert(pitch);
  chord_[pitch] = std::clamp(velocity, 1, 127);
}

bool StepRecorder::release(Document& doc, ObjectId pattern, ObjectId channel, int pitch) {
  down_.erase(pitch);
  if (!down_.empty() || chord_.empty()) return false;
  const Song& s = doc.song();
  if (!s.pattern(pattern) || !s.channel(channel)) {
    chord_.clear();
    return false;
  }
  std::vector<Note> notes;
  if (auto it = s.pattern(pattern)->notes.find(channel); it != s.pattern(pattern)->notes.end()) notes = it->second;
  const Tick len = lengthOverride > 0 ? lengthOverride : std::max<Tick>(1, step);
  for (const auto& cv : chord_) {
    const int p = cv.first;
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n) { return n.start == cursor && n.pitch == p; }),
                notes.end());
    notes.push_back(Note(cursor, len, p, cv.second));
  }
  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
  chord_.clear();
  doc.setNotes(pattern, channel, std::move(notes), "step record");
  cursor += std::max<Tick>(1, step);
  return true;
}

void StepRecorder::backspace(Document& doc, ObjectId pattern, ObjectId channel) {
  cursor = std::max<Tick>(0, cursor - std::max<Tick>(1, step));
  const Pattern* p = doc.song().pattern(pattern);
  if (!p) return;
  auto it = p->notes.find(channel);
  if (it == p->notes.end()) return;
  std::vector<Note> notes = it->second;
  const Tick a = cursor, b = cursor + std::max<Tick>(1, step);
  size_t before = notes.size();
  notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const Note& n) { return n.start >= a && n.start < b; }),
              notes.end());
  if (notes.size() != before) doc.setNotes(pattern, channel, std::move(notes), "step record: delete");
}

// ------------------------------------------------------------ TypingKeyboard ----

const char* chordTypeName(ChordType c) {
  static const char* n[] = {"Major", "Minor", "Dominant 7th", "Major 7th", "Minor 7th", "Sus 4", "Power (5th)", "Octave"};
  int i = static_cast<int>(c);
  return i >= 0 && i < static_cast<int>(ChordType::Count) ? n[i] : "?";
}

std::vector<int> chordIntervals(ChordType c) {
  switch (c) {
    case ChordType::Minor: return {0, 3, 7};
    case ChordType::Seventh: return {0, 4, 7, 10};
    case ChordType::Major7: return {0, 4, 7, 11};
    case ChordType::Minor7: return {0, 3, 7, 10};
    case ChordType::Sus4: return {0, 5, 7};
    case ChordType::Power: return {0, 7};
    case ChordType::Octave: return {0, 12};
    default: return {0, 4, 7};
  }
}

int TypingKeyboard::keyOffset(int key) {
  switch (key) {
    // lower row: from the base C
    case 'z': return 0;
    case 's': return 1;
    case 'x': return 2;
    case 'd': return 3;
    case 'c': return 4;
    case 'v': return 5;
    case 'g': return 6;
    case 'b': return 7;
    case 'h': return 8;
    case 'n': return 9;
    case 'j': return 10;
    case 'm': return 11;
    case ',': return 12;
    case 'l': return 13;
    case '.': return 14;
    case ';': return 15;
    case '/': return 16;
    // upper row: one octave up
    case 'q': return 12;
    case '2': return 13;
    case 'w': return 14;
    case '3': return 15;
    case 'e': return 16;
    case 'r': return 17;
    case '5': return 18;
    case 't': return 19;
    case '6': return 20;
    case 'y': return 21;
    case '7': return 22;
    case 'u': return 23;
    case 'i': return 24;
    case '9': return 25;
    case 'o': return 26;
    case '0': return 27;
    case 'p': return 28;
    default: return -1;
  }
}

bool TypingKeyboard::isControlKey(int key) {
  return key == '[' || key == ']' || key == '-' || key == '=' || key == '\\' || key == '\'';
}

namespace {
struct KeyPosition {
  int position, key;
};
// Each key's USB usage. Where one key code has two places the first is the one positionOfKey gives.
constexpr KeyPosition kKeyPositions[] = {
    {29, 'z'}, {22, 's'}, {27, 'x'}, {7, 'd'},  {6, 'c'},  {25, 'v'}, {10, 'g'}, {5, 'b'},  {11, 'h'}, {17, 'n'}, {13, 'j'}, {16, 'm'},
    {54, ','}, {15, 'l'}, {55, '.'}, {51, ';'}, {56, '/'}, {20, 'q'}, {31, '2'}, {26, 'w'}, {32, '3'}, {8, 'e'},  {21, 'r'}, {34, '5'},
    {23, 't'}, {35, '6'}, {28, 'y'}, {36, '7'}, {24, 'u'}, {12, 'i'}, {38, '9'}, {18, 'o'}, {39, '0'}, {19, 'p'},
    {47, '['}, {48, ']'}, {45, '-'}, {46, '='}, {49, '\\'}, {52, '\''},
    {50, '\\'},                                       // the same key on ISO keyboards
    {86, '['}, {87, ']'}, {84, '-'}, {85, '='},        // keypad - + change the octave, / * the velocity
};
}  // namespace

int TypingKeyboard::keyAtPosition(int position) {
  for (const KeyPosition& k : kKeyPositions)
    if (k.position == position) return k.key;
  return 0;
}

int TypingKeyboard::positionOfKey(int key) {
  for (const KeyPosition& k : kKeyPositions)
    if (k.key == key) return k.position;
  return 0;
}

std::string TypingKeyboard::keyLabel(int key, int typed) {
  const bool ascii = typed > 32 && typed < 127, latin1 = typed >= 0xA1 && typed <= 0xFF && typed != 0xAD;  // (0xAD: soft hyphen)
  int c = ascii || latin1 ? typed : key;
  if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
  else if (c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 0x20;  // à..þ in capitals (ß and ÿ have none in Latin-1)
  if (c < 0x80) return std::string(1, static_cast<char>(c));
  return {static_cast<char>(0xC0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3F))};  // UTF-8
}

namespace {
// K-113: each layout's characters where they differ from US QWERTY, by USB usage (the typing keys only).
constexpr KeyPosition kQwertz[] = {{29, 'y'}, {28, 'z'}, {51, 0xF6}, {52, 0xE4}, {47, 0xFC}, {48, '+'},
                                   {45, 0xDF}, {46, 0xB4}, {49, '#'}, {50, '#'}, {56, '-'}};
constexpr KeyPosition kAzerty[] = {{20, 'a'}, {26, 'z'}, {29, 'w'}, {16, ','}, {54, ';'}, {55, ':'}, {56, '!'}, {51, 'm'},
                                   {52, 0xF9}, {47, '^'}, {48, '$'}, {45, ')'}, {46, '='}, {49, '*'}, {50, '*'}, {31, 0xE9},
                                   {32, '"'}, {34, '('}, {35, '-'}, {36, 0xE8}, {38, 0xE7}, {39, 0xE0}};
constexpr KeyPosition kDvorak[] = {{29, ';'}, {22, 'o'}, {27, 'q'}, {7, 'e'},  {6, 'j'},  {25, 'k'}, {10, 'i'}, {5, 'x'},
                                   {11, 'd'}, {17, 'b'}, {13, 'h'}, {54, 'w'}, {15, 'n'}, {55, 'v'}, {51, 's'}, {56, 'z'},
                                   {20, '\''}, {26, ','}, {8, '.'},  {21, 'p'}, {23, 'y'}, {28, 'f'}, {24, 'g'}, {12, 'c'},
                                   {18, 'r'}, {19, 'l'}, {47, '/'}, {48, '='}, {45, '['}, {46, ']'}, {52, '-'}};
constexpr KeyPosition kColemak[] = {{22, 'r'}, {7, 's'},  {10, 'd'}, {11, 'h'}, {17, 'k'}, {13, 'n'}, {15, 'i'}, {51, 'o'},
                                    {8, 'f'},  {21, 'p'}, {23, 'g'}, {28, 'j'}, {24, 'l'}, {12, 'u'}, {18, 'y'}, {19, ';'}};

template <size_t N>
int lookUp(const KeyPosition (&table)[N], int position) {
  for (const KeyPosition& k : table)
    if (k.position == position) return k.key;
  return 0;
}
}  // namespace

int TypingKeyboard::layoutChar(TypingLayout layout, int position) {
  int c = 0;
  switch (layout) {
    case TypingLayout::Standard: break;
    case TypingLayout::Qwertz: c = lookUp(kQwertz, position); break;
    case TypingLayout::Azerty: c = lookUp(kAzerty, position); break;
    case TypingLayout::Dvorak: c = lookUp(kDvorak, position); break;
    case TypingLayout::Colemak: c = lookUp(kColemak, position); break;
    default: return 0;  // Computer (the system's layout) or unknown
  }
  return c ? c : keyAtPosition(position);
}

namespace {
struct LayoutNames {
  const char* id;
  const char* name;
};
constexpr LayoutNames kLayoutNames[] = {{"standard", "Standard (QWERTY)"}, {"qwertz", "QWERTZ"}, {"azerty", "AZERTY"},
                                        {"dvorak", "Dvorak"},              {"colemak", "Colemak"}, {"computer", "Follow the computer"}};
static_assert(sizeof(kLayoutNames) / sizeof(kLayoutNames[0]) == static_cast<size_t>(TypingLayout::Count), "typing layout names");
}  // namespace

const char* typingLayoutName(TypingLayout l) {
  return l < TypingLayout::Count ? kLayoutNames[static_cast<size_t>(l)].name : kLayoutNames[0].name;
}
const char* typingLayoutId(TypingLayout l) { return l < TypingLayout::Count ? kLayoutNames[static_cast<size_t>(l)].id : kLayoutNames[0].id; }
TypingLayout typingLayoutFromId(const std::string& id) {
  for (size_t i = 0; i < static_cast<size_t>(TypingLayout::Count); ++i)
    if (id == kLayoutNames[i].id) return static_cast<TypingLayout>(i);
  return TypingLayout::Standard;
}

std::vector<int> TypingKeyboard::heldNotes() const {
  std::vector<int> notes;
  for (const auto& [key, on] : held_) notes.insert(notes.end(), on.begin(), on.end());
  return notes;
}

std::vector<int> TypingKeyboard::keyDown(int key) {
  switch (key) {
    case '[': octave = std::max(0, octave - 1); return {};
    case ']': octave = std::min(9, octave + 1); return {};
    case '-': velocity = std::max(1, velocity - 10); return {};
    case '=': velocity = std::min(127, velocity + 10); return {};
    case '\\': chordMode = !chordMode; return {};
    case '\'':
      chord = static_cast<ChordType>((static_cast<int>(chord) + 1) % static_cast<int>(ChordType::Count));
      return {};
    default: break;
  }
  int off = keyOffset(key);
  if (off < 0 || held_.count(key)) return {};
  std::vector<int> notes;
  const int root = baseNote() + off;
  if (chordMode) {
    for (int iv : chordIntervals(chord))
      if (root + iv >= 0 && root + iv <= 127) notes.push_back(root + iv);
  } else if (root >= 0 && root <= 127) {
    notes.push_back(root);
  }
  // A note already sounding from another key is not started twice.
  for (const auto& [k, v] : held_)
    for (int n : v) notes.erase(std::remove(notes.begin(), notes.end(), n), notes.end());
  if (notes.empty()) return {};
  held_[key] = notes;
  return notes;
}

std::vector<int> TypingKeyboard::keyUp(int key) {
  auto it = held_.find(key);
  if (it == held_.end()) return {};
  std::vector<int> v = it->second;
  held_.erase(it);
  return v;
}

std::vector<int> TypingKeyboard::releaseAll() {
  std::vector<int> v;
  for (const auto& [k, n] : held_) v.insert(v.end(), n.begin(), n.end());
  held_.clear();
  return v;
}

}  // namespace gl
