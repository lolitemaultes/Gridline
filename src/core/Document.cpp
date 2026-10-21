#include "Document.h"
#include <algorithm>
#include "Arrangements.h"

namespace gl {

namespace {

// Parameter change; consecutive changes of one parameter merge.
class ParamCommand final : public Command {
 public:
  ParamCommand(ParamId id, double before, double after, bool mergeable)
      : id_(id), before_(before), after_(after), mergeable_(mergeable) {}
  void apply(Song& s) override { setParam(s, id_, after_); }
  void revert(Song& s) override { setParam(s, id_, before_); }
  std::string label() const override { return "change parameter"; }
  uint64_t mergeKey() const override { return mergeable_ ? id_.packed() : 0; }
  bool mergeWith(const Command& next) override {
    auto* n = dynamic_cast<const ParamCommand*>(&next);
    if (!n || n->id_ != id_) return false;
    after_ = n->after_;
    return true;
  }

 private:
  ParamId id_;
  double before_, after_;
  bool mergeable_;
};

// Replaces one object (found by id) with a before/after copy.
template <typename T>
class ObjectEdit final : public Command {
 public:
  using Getter = T* (*)(Song&, ObjectId);
  ObjectEdit(std::string label, Getter get, ObjectId id, T before, T after, uint64_t key)
      : label_(std::move(label)), get_(get), id_(id), before_(std::move(before)), after_(std::move(after)), key_(key) {}
  void apply(Song& s) override { *get_(s, id_) = after_; }
  void revert(Song& s) override { *get_(s, id_) = before_; }
  std::string label() const override { return label_; }
  uint64_t mergeKey() const override { return key_; }
  bool mergeWith(const Command& next) override {
    auto* n = dynamic_cast<const ObjectEdit<T>*>(&next);
    if (!n || n->id_ != id_ || n->get_ != get_) return false;
    after_ = n->after_;
    return true;
  }

 private:
  std::string label_;
  Getter get_;
  ObjectId id_;
  T before_, after_;
  uint64_t key_;
};

// Replaces the internal controller link list; consecutive edits with the same key merge.
class ModLinksCommand final : public Command {
 public:
  ModLinksCommand(std::string label, std::vector<ModLink> before, std::vector<ModLink> after, uint64_t key)
      : label_(std::move(label)), before_(std::move(before)), after_(std::move(after)), key_(key) {}
  void apply(Song& s) override { s.modLinks = after_; }
  void revert(Song& s) override { s.modLinks = before_; }
  std::string label() const override { return label_; }
  uint64_t mergeKey() const override { return key_; }
  bool mergeWith(const Command& next) override {
    auto* n = dynamic_cast<const ModLinksCommand*>(&next);
    if (!n) return false;
    after_ = n->after_;
    return true;
  }

 private:
  std::string label_;
  std::vector<ModLink> before_, after_;
  uint64_t key_;
};

// Link maintenance when plugins, slots or note processors move or disappear.
bool slotOf(uint32_t index, uint32_t& slot) {
  if (index < kSlotBase) return false;
  slot = (index - kSlotBase) / kSlotStride;
  return true;
}
bool noteFxOf(uint32_t index, uint32_t& fx) {
  if (index < kNoteFxBase) return false;
  fx = (index - kNoteFxBase) / kNoteFxStride;
  return true;
}
// New position of element `i` after moving `from` to `to` (erase + insert).
int movedIndex(int i, int from, int to) {
  if (i == from) return to;
  int j = i > from ? i - 1 : i;  // after erase
  return j >= to ? j + 1 : j;    // after insert
}

Channel* getChannel(Song& s, ObjectId id) { return s.channel(id); }
Pattern* getPattern(Song& s, ObjectId id) { return s.pattern(id); }
Clip* getClip(Song& s, ObjectId id) { return s.clip(id); }
AutomationClip* getAutomation(Song& s, ObjectId id) { return s.automation(id); }
MixerTrack* getMixer(Song& s, ObjectId id) { return s.mixerTrack(id); }
PlaylistTrack* getTrack(Song& s, ObjectId index) { return index < s.tracks.size() ? &s.tracks[index] : nullptr; }

template <typename T>
void insertAt(std::vector<T>& v, int index, T value) {
  if (index < 0 || index > static_cast<int>(v.size())) index = static_cast<int>(v.size());
  v.insert(v.begin() + index, std::move(value));
}

// Distinct, readable default colours for new channels and patterns.
Rgb paletteColor(size_t i) {
  static const Rgb pal[] = {0x5A8FD8, 0xD8775A, 0x6EBF6A, 0xC9A43C, 0xA56CD8, 0x4FB8B0, 0xD85A8F, 0x8FA0B5};
  return pal[i % (sizeof pal / sizeof pal[0])];
}

}  // namespace

void Document::reset(Song song) {
  song_ = std::move(song);
  groups_.clear();
  undo_.clear();
  savedSerial_ = undo_.stateSerial();
  ++revision_;
}

void Document::exec(CommandPtr cmd) {
  ++revision_;
  if (!groups_.empty()) {
    cmd->apply(song_);
    groups_.back()->add(std::move(cmd));
    return;
  }
  undo_.push(song_, std::move(cmd));
}

void Document::beginGroup(const std::string& label) {
  groups_.push_back(std::make_unique<GroupCommand>(label));
}

void Document::endGroup() {
  if (groups_.empty()) return;
  auto g = std::move(groups_.back());
  groups_.pop_back();
  if (g->empty()) return;
  if (!groups_.empty()) {
    groups_.back()->add(std::move(g));  // already applied
    return;
  }
  undo_.breakMerge();
  undo_.pushApplied(std::move(g));
  undo_.breakMerge();
}

void Document::cancelGroup() {
  if (groups_.empty()) return;
  auto g = std::move(groups_.back());
  groups_.pop_back();
  if (g->empty()) return;
  g->revert(song_);
  ++revision_;
}

bool Document::undo() {
  if (!groups_.empty() || !undo_.undo(song_)) return false;
  ++revision_;
  return true;
}

bool Document::redo() {
  if (!groups_.empty() || !undo_.redo(song_)) return false;
  ++revision_;
  return true;
}

void Document::jumpTo(size_t position) {
  if (!groups_.empty()) return;
  undo_.jumpTo(song_, position);
  ++revision_;
}

// --- generic edits ----------------------------------------------------------------

#define GL_OBJECT_EDIT(T, getter, lookup)                                                                \
  T* obj = lookup;                                                                                       \
  if (!obj) return false;                                                                                \
  T after = *obj;                                                                                        \
  fn(after);                                                                                             \
  if (after == *obj) return true;                                                                        \
  exec(std::make_unique<ObjectEdit<T>>(label, getter, id, *obj, std::move(after), mergeKey));            \
  return true;

bool Document::editChannel(ObjectId id, const std::string& label, const std::function<void(Channel&)>& fn,
                           uint64_t mergeKey) {
  GL_OBJECT_EDIT(Channel, getChannel, song_.channel(id))
}
bool Document::editPattern(ObjectId id, const std::string& label, const std::function<void(Pattern&)>& fn,
                           uint64_t mergeKey) {
  GL_OBJECT_EDIT(Pattern, getPattern, song_.pattern(id))
}
bool Document::editClip(ObjectId id, const std::string& label, const std::function<void(Clip&)>& fn, uint64_t mergeKey) {
  GL_OBJECT_EDIT(Clip, getClip, song_.clip(id))
}
bool Document::editAutomation(ObjectId id, const std::string& label, const std::function<void(AutomationClip&)>& fn,
                              uint64_t mergeKey) {
  GL_OBJECT_EDIT(AutomationClip, getAutomation, song_.automation(id))
}
bool Document::editMixerTrack(ObjectId id, const std::string& label, const std::function<void(MixerTrack&)>& fn,
                              uint64_t mergeKey) {
  GL_OBJECT_EDIT(MixerTrack, getMixer, song_.mixerTrack(id))
}
bool Document::editPlaylistTrack(int index, const std::string& label, const std::function<void(PlaylistTrack&)>& fn) {
  if (index < 0) return false;
  ObjectId id = static_cast<ObjectId>(index);
  uint64_t mergeKey = 0;
  GL_OBJECT_EDIT(PlaylistTrack, getTrack, getTrack(song_, id))
}
#undef GL_OBJECT_EDIT

namespace {

// Whole-song edit stored as a delta (J-25): the large containers are kept
// (before and after) only when the edit changed them; everything else lives
// in two small "rest" copies whose large containers are empty. A tempo change
// on a big project used to keep two full copies of the song per undo step.
#define GL_SONG_BIG_FIELDS(X) \
  X(channels)                 \
  X(patterns)                 \
  X(playlist)                 \
  X(tracks)                   \
  X(mixer)                    \
  X(links)                    \
  X(modLinks)                 \
  X(automations)              \
  X(markers)                  \
  X(arrangements)

class SongDeltaCommand final : public Command {
 public:
  // `cur` is the song before the edit (left unchanged), `after` the edited copy.
  SongDeltaCommand(std::string label, Song& cur, Song after) : label_(std::move(label)) {
#define GL_TAKE(f)                                  \
  if (cur.f != after.f) {                           \
    f##_.changed = true;                            \
    f##_.before = cur.f;                            \
    f##_.after = std::move(after.f);                \
  }                                                 \
  decltype(Song::f)().swap(after.f); /* frees the capacity too */
    GL_SONG_BIG_FIELDS(GL_TAKE)
#undef GL_TAKE
    restAfter_ = std::move(after);
    // The small part of the current song: move its large containers out
    // around the copy so they are not duplicated.
#define GL_OUT(f) auto f##Tmp = std::move(cur.f); decltype(Song::f)().swap(cur.f);
    GL_SONG_BIG_FIELDS(GL_OUT)
#undef GL_OUT
    restBefore_ = cur;
#define GL_BACK(f) cur.f = std::move(f##Tmp);
    GL_SONG_BIG_FIELDS(GL_BACK)
#undef GL_BACK
  }
  void apply(Song& s) override { set(s, restAfter_, true); }
  void revert(Song& s) override { set(s, restBefore_, false); }
  std::string label() const override { return label_; }

 private:
  template <typename V>
  struct Field {
    bool changed = false;
    V before, after;
  };
  void set(Song& s, const Song& rest, bool after) const {
    Song next = rest;
#define GL_SET(f) next.f = f##_.changed ? (after ? f##_.after : f##_.before) : std::move(s.f);
    GL_SONG_BIG_FIELDS(GL_SET)
#undef GL_SET
    s = std::move(next);
  }
  std::string label_;
  Song restBefore_, restAfter_;
#define GL_FIELD(f) Field<decltype(Song::f)> f##_;
  GL_SONG_BIG_FIELDS(GL_FIELD)
#undef GL_FIELD
};
#undef GL_SONG_BIG_FIELDS

}  // namespace

void Document::editSong(const std::string& label, const std::function<void(Song&)>& fn) {
  Song after = song_;
  fn(after);
  if (after == song_) return;
  // The edit is already in `after`: record it, then take it over.
  auto cmd = std::make_unique<SongDeltaCommand>(label, song_, std::move(after));
  exec(std::move(cmd));
}

// --- channels ---------------------------------------------------------------

ObjectId Document::addChannel(const std::string& name, const std::string& pluginType, int index) {
  Channel c;
  c.id = song_.nextId;  // allocated in apply so redo reproduces the same ID
  c.name = name;
  c.color = paletteColor(song_.channels.size());
  c.plugin = defaultPluginState(pluginType);
  c.mixerTrack = song_.masterId();
  ObjectId id = c.id;
  exec(std::make_unique<LambdaCommand>(
      "add channel",
      [c, index](Song& s) {
        insertAt(s.channels, index, c);
        s.nextId = std::max(s.nextId, c.id + 1);
      },
      [id](Song& s) {
        s.channels.erase(s.channels.begin() + s.channelIndex(id));
        s.nextId = id;  // undo runs LIFO, so this was the latest ID
      }));
  return id;
}

bool Document::removeChannel(ObjectId id) {
  int idx = song_.channelIndex(id);
  if (idx < 0) return false;
  Channel saved = song_.channels[static_cast<size_t>(idx)];
  // Pattern data that belongs to the channel goes with it.
  std::vector<std::pair<ObjectId, std::vector<uint8_t>>> steps;
  std::vector<std::pair<ObjectId, std::vector<Note>>> notes;
  std::vector<std::pair<ObjectId, std::vector<StepProps>>> props;
  std::vector<std::pair<ObjectId, int>> loops;  // J-36: J-31 step loops went missing on reload when left behind
  for (const auto& p : song_.patterns) {
    if (auto it = p.steps.find(id); it != p.steps.end()) steps.push_back({p.id, it->second});
    if (auto it = p.notes.find(id); it != p.notes.end()) notes.push_back({p.id, it->second});
    if (auto it = p.stepProps.find(id); it != p.stepProps.end()) props.push_back({p.id, it->second});
    if (auto it = p.stepLoop.find(id); it != p.stepLoop.end()) loops.push_back({p.id, it->second});
  }
  beginGroup("delete channel");
  exec(std::make_unique<LambdaCommand>(
      "delete channel",
      [id](Song& s) {
        s.channels.erase(s.channels.begin() + s.channelIndex(id));
        for (auto& p : s.patterns) {
          p.steps.erase(id);
          p.notes.erase(id);
          p.stepProps.erase(id);
          p.stepLoop.erase(id);
        }
      },
      [saved, idx, steps, notes, props, loops](Song& s) {
        insertAt(s.channels, idx, saved);
        for (const auto& [pid, row] : steps) s.pattern(pid)->steps[saved.id] = row;
        for (const auto& [pid, ns] : notes) s.pattern(pid)->notes[saved.id] = ns;
        for (const auto& [pid, pr] : props) s.pattern(pid)->stepProps[saved.id] = pr;
        for (const auto& [pid, n] : loops) s.pattern(pid)->stepLoop[saved.id] = n;
      }));
  {
    std::vector<ModLink> links = song_.modLinks;
    links.erase(std::remove_if(links.begin(), links.end(),
                               [id](const ModLink& l) { return l.owner == id || (l.sourceOwner == id && l.sourceSlot < 0); }),
                links.end());
    setModLinks(std::move(links), "delete channel");
  }
  // Audio clips playing through the channel (every arrangement) and layer
  // references to it go too: left behind they dangle, and a saved project
  // silently lost them on load (J-21 undo/save stress).
  bool refs = false;
  auto audioOf = [id](const Clip& c) { return c.kind == ClipKind::Audio && c.source == id; };
  for (const auto& c : song_.playlist) refs = refs || audioOf(c);
  for (const auto& a : song_.arrangements)
    for (const auto& c : a.playlist) refs = refs || audioOf(c);
  for (const auto& ch : song_.channels) refs = refs || std::find(ch.layer.begin(), ch.layer.end(), id) != ch.layer.end();
  // J-82: MIDI controller links and pattern automation lanes of its controls go too.
  auto linkOf = [id](const ControllerLink& l) { return l.owner == id && l.action == RemoteAction::None; };
  auto laneOf = [id](const PatternAutomation& l) { return l.owner == id; };
  for (const auto& l : song_.links) refs = refs || linkOf(l);
  for (const auto& p : song_.patterns)
    for (const auto& l : p.automation) refs = refs || laneOf(l);
  if (refs)
    editSong("delete channel", [&](Song& s) {
      auto drop = [&](std::vector<Clip>& v) { v.erase(std::remove_if(v.begin(), v.end(), audioOf), v.end()); };
      drop(s.playlist);
      for (auto& a : s.arrangements) drop(a.playlist);
      for (auto& ch : s.channels) ch.layer.erase(std::remove(ch.layer.begin(), ch.layer.end(), id), ch.layer.end());
      s.links.erase(std::remove_if(s.links.begin(), s.links.end(), linkOf), s.links.end());
      for (auto& p : s.patterns) p.automation.erase(std::remove_if(p.automation.begin(), p.automation.end(), laneOf), p.automation.end());
    });
  endGroup();
  return true;
}

bool Document::moveChannel(ObjectId id, int newIndex) {
  int from = song_.channelIndex(id);
  int n = static_cast<int>(song_.channels.size());
  if (from < 0 || newIndex < 0 || newIndex >= n) return false;
  if (from == newIndex) return true;
  auto move = [id](int to) {
    return [id, to](Song& s) {
      int f = s.channelIndex(id);
      Channel c = s.channels[static_cast<size_t>(f)];
      s.channels.erase(s.channels.begin() + f);
      s.channels.insert(s.channels.begin() + to, c);
    };
  };
  exec(std::make_unique<LambdaCommand>("move channel", move(newIndex), move(from)));
  return true;
}

bool Document::renameChannel(ObjectId id, const std::string& name) {
  return editChannel(id, "rename channel", [&](Channel& c) { c.name = name; });
}

bool Document::setChannelPlugin(ObjectId id, const std::string& pluginType) {
  if (!findPlugin(pluginType)) return false;
  const Channel* old = song_.channel(id);
  bool typeChanged = old && old->plugin.type != pluginType;
  beginGroup("replace channel instrument");
  bool ok = editChannel(id, "replace channel instrument", [&](Channel& c) {
    std::string data = c.plugin.data;
    c.plugin = defaultPluginState(pluginType);
    if (findPlugin(pluginType)->usesSample) c.plugin.data = data;
  });
  if (ok && typeChanged) {  // the old instrument's parameters and outputs are gone
    std::vector<ModLink> links = song_.modLinks;
    links.erase(std::remove_if(links.begin(), links.end(),
                               [id](const ModLink& l) {
                                 return (l.owner == id && l.index >= kChannelPluginBase && l.index < kNoteFxBase) ||
                                        (l.sourceOwner == id && l.sourceSlot < 0);
                               }),
                links.end());
    setModLinks(std::move(links), "replace channel instrument");
  }
  endGroup();
  return ok;
}

ObjectId Document::cloneChannel(ObjectId id) {
  int idx = song_.channelIndex(id);
  if (idx < 0) return kNoId;
  Channel src = song_.channels[static_cast<size_t>(idx)];
  beginGroup("clone channel");
  ObjectId nid = addChannel(src.name + " #2", src.plugin.type, idx + 1);
  editChannel(nid, "clone channel", [&](Channel& c) {
    ObjectId keep = c.id;
    Rgb color = c.color;
    c = src;
    c.id = keep;
    c.color = color;
    c.name = src.name + " #2";
  });
  for (const auto& p : song_.patterns) {
    auto st = p.steps.find(id);
    auto nt = p.notes.find(id);
    auto pt = p.stepProps.find(id);
    if (st == p.steps.end() && nt == p.notes.end()) continue;
    editPattern(p.id, "clone channel", [&](Pattern& q) {
      if (st != p.steps.end()) q.steps[nid] = st->second;
      if (nt != p.notes.end()) q.notes[nid] = nt->second;
      if (pt != p.stepProps.end()) q.stepProps[nid] = pt->second;
    });
  }
  endGroup();
  return nid;
}

bool Document::routeChannel(ObjectId id, ObjectId mixerTrack) {
  if (!song_.mixerTrack(mixerTrack)) return false;
  return editChannel(id, "route channel to mixer track", [&](Channel& c) { c.mixerTrack = mixerTrack; });
}

// --- parameters ---------------------------------------------------------------

bool Document::setParam(ParamId id, double value, bool merge) {
  auto before = getParam(song_, id);
  if (!before) return false;
  double after = describeParam(song_, id)->clamp(value);
  if (id != lastTouched_ && lastTouched_.owner != kNoId) prevTouched_ = lastTouched_;  // J-27: next to last tweaked
  lastTouched_ = id;
  ++touchSerial_;
  if (after == *before) return true;
  if (!merge) undo_.breakMerge();
  exec(std::make_unique<ParamCommand>(id, *before, after, merge));
  if (!merge) undo_.breakMerge();
  if (paramObserver) paramObserver(id, after);
  return true;
}

// --- patterns -----------------------------------------------------------------

ObjectId Document::addPattern(const std::string& name, int bars, int stepsPerBar, int index) {
  if (bars <= 0 || stepsPerBar <= 0 || bars > kMaxPatternBars || stepsPerBar > kMaxStepsPerBar ||
      static_cast<int>(song_.patterns.size()) >= kMaxPatterns)
    return kNoId;
  Pattern p;
  p.id = song_.nextId;
  p.name = name;
  p.bars = bars;
  p.stepsPerBar = stepsPerBar;
  p.color = paletteColor(song_.patterns.size() + 2);
  ObjectId id = p.id;
  exec(std::make_unique<LambdaCommand>(
      "add pattern",
      [p, index](Song& s) {
        insertAt(s.patterns, index, p);
        s.nextId = std::max(s.nextId, p.id + 1);
      },
      [id](Song& s) {
        s.patterns.erase(s.patterns.begin() + s.patternIndex(id));
        s.nextId = id;  // undo runs LIFO, so this was the latest ID
      }));
  return id;
}

bool Document::removePattern(ObjectId id) {
  int idx = song_.patternIndex(id);
  if (idx < 0) return false;
  Pattern saved = song_.patterns[static_cast<size_t>(idx)];
  std::vector<std::pair<int, Clip>> clips;  // (index, clip) in ascending order
  for (size_t i = 0; i < song_.playlist.size(); ++i)
    if (song_.playlist[i].pattern == id) clips.push_back({static_cast<int>(i), song_.playlist[i]});
  std::vector<Arrangement> parked = song_.arrangements;  // other arrangements lose their clips of it too
  std::vector<PatternGroup> groups = song_.patternGroups;  // J-36: it leaves its pattern group too
  exec(std::make_unique<LambdaCommand>(
      "delete pattern",
      [id](Song& s) {
        s.patterns.erase(s.patterns.begin() + s.patternIndex(id));
        s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(),
                                        [id](const Clip& c) { return c.kind == ClipKind::Pattern && c.pattern == id; }),
                         s.playlist.end());
        pruneParkedArrangements(s);
        for (auto& g : s.patternGroups) g.patterns.erase(std::remove(g.patterns.begin(), g.patterns.end(), id), g.patterns.end());
        s.patternGroups.erase(std::remove_if(s.patternGroups.begin(), s.patternGroups.end(),
                                             [](const PatternGroup& g) { return g.patterns.empty(); }),
                              s.patternGroups.end());
      },
      [saved, idx, clips, parked, groups](Song& s) {
        insertAt(s.patterns, idx, saved);
        for (const auto& [i, c] : clips) insertAt(s.playlist, i, c);
        s.arrangements = parked;
        s.patternGroups = groups;
      }));
  return true;
}

ObjectId Document::clonePattern(ObjectId id) {
  int idx = song_.patternIndex(id);
  if (idx < 0) return kNoId;
  Pattern src = song_.patterns[static_cast<size_t>(idx)];
  beginGroup("clone pattern");
  ObjectId nid = addPattern(src.name + " #2", src.bars, src.stepsPerBar, idx + 1);
  editPattern(nid, "clone pattern", [&](Pattern& p) {
    ObjectId keep = p.id;
    p = src;
    p.id = keep;
    p.name = src.name + " #2";
  });
  endGroup();
  return nid;
}

bool Document::renamePattern(ObjectId id, const std::string& name) {
  return editPattern(id, "rename pattern", [&](Pattern& p) { p.name = name; });
}

bool Document::setPatternLength(ObjectId id, int bars, int stepsPerBar) {
  if (bars <= 0 || stepsPerBar <= 0 || bars > kMaxPatternBars || stepsPerBar > kMaxStepsPerBar) return false;
  return editPattern(id, "change pattern length", [&](Pattern& p) {
    p.bars = bars;
    p.stepsPerBar = stepsPerBar;
  });
}

bool Document::setSteps(ObjectId pattern, ObjectId channel, std::vector<uint8_t> row) {
  if (!song_.pattern(pattern) || !song_.channel(channel)) return false;
  for (auto& v : row) v = std::min<uint8_t>(v, 127);
  bool empty = std::all_of(row.begin(), row.end(), [](uint8_t v) { return v == 0; });
  return editPattern(pattern, "edit steps", [&](Pattern& p) {
    if (empty)
      p.steps.erase(channel);
    else
      p.steps[channel] = row;
  });
}

bool Document::setStep(ObjectId pattern, ObjectId channel, int step, uint8_t velocity) {
  const Pattern* p = song_.pattern(pattern);
  if (!p || step < 0) return false;
  std::vector<uint8_t> row;
  if (auto it = p->steps.find(channel); it != p->steps.end()) row = it->second;
  int total = p->bars * p->stepsPerBar;
  if (step >= total) return false;
  if (static_cast<int>(row.size()) < total) row.resize(static_cast<size_t>(total), 0);
  row[static_cast<size_t>(step)] = velocity;
  return setSteps(pattern, channel, std::move(row));
}

bool Document::setNotes(ObjectId pattern, ObjectId channel, std::vector<Note> notes, const std::string& label,
                        uint64_t mergeKey) {
  if (!song_.pattern(pattern) || !song_.channel(channel)) return false;
  for (auto& n : notes) {
    n.pitch = std::clamp(n.pitch, 0, 127);
    n.velocity = std::clamp(n.velocity, 1, 127);
    n.start = std::max<Tick>(n.start, 0);
    n.length = std::max<Tick>(n.length, 1);
  }
  std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
  return editPattern(
      pattern, label,
      [&](Pattern& p) {
        if (notes.empty())
          p.notes.erase(channel);
        else
          p.notes[channel] = notes;
      },
      mergeKey);
}

bool Document::addNote(ObjectId pattern, ObjectId channel, const Note& note) {
  const Pattern* p = song_.pattern(pattern);
  if (!p) return false;
  std::vector<Note> ns;
  if (auto it = p->notes.find(channel); it != p->notes.end()) ns = it->second;
  ns.push_back(note);
  return setNotes(pattern, channel, std::move(ns), "add note");
}

// --- playlist -----------------------------------------------------------------

ObjectId Document::addClip(ObjectId pattern, int track, Tick start, Tick length) {
  if (!song_.pattern(pattern) || track < 0 || track >= kMaxPlaylistTracks || start < 0 || length < 0) return kNoId;
  Clip c;
  c.id = song_.nextId;
  c.pattern = pattern;
  c.track = track;
  c.start = start;
  c.length = length;
  ObjectId id = c.id;
  size_t tracksBefore = song_.tracks.size();
  exec(std::make_unique<LambdaCommand>(
      "add clip",
      [c](Song& s) {
        s.playlist.push_back(c);
        s.nextId = std::max(s.nextId, c.id + 1);
        if (static_cast<size_t>(c.track) >= s.tracks.size()) s.tracks.resize(static_cast<size_t>(c.track) + 1);
      },
      [id, tracksBefore](Song& s) {
        s.playlist.erase(s.playlist.begin() + s.clipIndex(id));
        s.tracks.resize(tracksBefore);
        s.nextId = id;  // undo runs LIFO, so this was the latest ID
      }));
  return id;
}

bool Document::removeClip(ObjectId id) {
  int idx = song_.clipIndex(id);
  if (idx < 0) return false;
  Clip saved = song_.playlist[static_cast<size_t>(idx)];
  exec(std::make_unique<LambdaCommand>(
      "delete clip", [id](Song& s) { s.playlist.erase(s.playlist.begin() + s.clipIndex(id)); },
      [saved, idx](Song& s) { insertAt(s.playlist, idx, saved); }));
  return true;
}

bool Document::moveClip(ObjectId id, int track, Tick start) {
  if (track < 0 || track >= static_cast<int>(song_.tracks.size()) || start < 0) return false;
  return editClip(id, "move clip", [&](Clip& c) {
    c.track = track;
    c.start = start;
  });
}

bool Document::setClipLength(ObjectId id, Tick length) {
  if (length < 0) return false;
  return editClip(id, "resize clip", [&](Clip& c) { c.length = length; });
}

namespace {
class ClipsEdit final : public Command {
 public:
  ClipsEdit(std::string label, std::vector<Clip> before, std::vector<Clip> after, uint64_t key)
      : label_(std::move(label)), before_(std::move(before)), after_(std::move(after)), key_(key) {}
  void apply(Song& s) override { s.playlist = after_; }
  void revert(Song& s) override { s.playlist = before_; }
  std::string label() const override { return label_; }
  uint64_t mergeKey() const override { return key_; }
  bool mergeWith(const Command& next) override {
    auto* n = dynamic_cast<const ClipsEdit*>(&next);
    if (!n) return false;
    after_ = n->after_;
    return true;
  }

 private:
  std::string label_;
  std::vector<Clip> before_, after_;
  uint64_t key_;
};
}  // namespace

bool Document::editClips(const std::string& label, const std::function<void(std::vector<Clip>&)>& fn, uint64_t mergeKey) {
  std::vector<Clip> after = song_.playlist;
  fn(after);
  if (after.size() != song_.playlist.size()) return false;
  for (size_t i = 0; i < after.size(); ++i) {
    // J-28: every clip kind is checked against its own source. (This used to
    // require a pattern for every clip, so any song holding an audio or
    // automation clip refused every move / resize / trim of all clips.)
    const Clip& c = after[i];
    const bool valid = c.kind == ClipKind::Audio        ? song_.channel(c.source) != nullptr
                       : c.kind == ClipKind::Automation ? song_.automation(c.source) != nullptr
                                                        : song_.pattern(c.pattern) != nullptr;
    if (c.id != song_.playlist[i].id) return false;
    if (!valid && !(c == song_.playlist[i])) return false;  // untouched dangling clips don't block the edit
    after[i].track = std::clamp(after[i].track, 0, static_cast<int>(song_.tracks.size()) - 1);
    after[i].start = std::max<Tick>(0, after[i].start);
    after[i].length = std::max<Tick>(0, after[i].length);
  }
  if (after == song_.playlist) return true;
  exec(std::make_unique<ClipsEdit>(label, song_.playlist, std::move(after), mergeKey));
  return true;
}

bool Document::setPlaylistTrackCount(int count) {
  int used = 0;
  for (const auto& c : song_.playlist) used = std::max(used, c.track + 1);
  if (count < used || count < 1 || count > kMaxPlaylistTracks) return false;
  editSong("change playlist track count", [&](Song& s) { s.tracks.resize(static_cast<size_t>(count)); });
  return true;
}

// --- mixer --------------------------------------------------------------------

ObjectId Document::addMixerTrack() {
  if (static_cast<int>(song_.mixer.size()) > kMaxMixerInserts) return kNoId;
  ObjectId id = song_.nextId;
  editSong("add mixer track", [&](Song& s) {
    ensureMixer(s, 0);
    MixerTrack t;
    t.id = s.allocateId();
    t.sends.push_back({s.masterId(), 1.0f, false});
    s.mixer.push_back(t);
  });
  return song_.mixerTrack(id) ? id : song_.mixer.back().id;
}

bool Document::removeMixerTrack(ObjectId id) {
  if (!song_.mixerTrack(id) || id == song_.masterId()) return false;
  editSong("delete mixer track", [&](Song& s) {
    ObjectId m = s.masterId();
    for (auto& c : s.channels) {
      if (c.mixerTrack == id) c.mixerTrack = m;
      for (ObjectId& a : c.auxOuts)
        if (a == id) a = kNoId;
    }
    for (auto& t : s.mixer) {
      bool had = false;
      for (const auto& snd : t.sends) had = had || snd.to == id;
      t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [&](const Send& x) { return x.to == id; }),
                    t.sends.end());
      if (had && t.sends.empty() && t.id != m) t.sends.push_back({m, 1.0f, false});
    }
    s.mixer.erase(s.mixer.begin() + s.mixerIndex(id));
    s.modLinks.erase(std::remove_if(s.modLinks.begin(), s.modLinks.end(),
                                    [&](const ModLink& l) { return l.owner == id || (l.sourceOwner == id && l.sourceSlot >= 0); }),
                     s.modLinks.end());
  });
  return true;
}

bool Document::setSend(ObjectId from, ObjectId to, float level) {
  const MixerTrack* src = song_.mixerTrack(from);
  if (!src || !song_.mixerTrack(to)) return false;
  bool exists = false;
  for (const auto& s : src->sends) exists = exists || s.to == to;
  if (!exists && level > 0 && !canRoute(song_, from, to)) return false;
  return editMixerTrack(
      from, level > 0 ? "route mixer track" : "unroute mixer track",
      [&](MixerTrack& t) {
        auto it = std::find_if(t.sends.begin(), t.sends.end(), [&](const Send& s) { return s.to == to; });
        if (level <= 0) {
          if (it != t.sends.end()) t.sends.erase(it);
        } else if (it != t.sends.end()) {
          it->level = std::min(level, 1.25f);
        } else {
          t.sends.push_back({to, std::min(level, 1.25f), false});
        }
      },
      exists && level > 0 ? (static_cast<uint64_t>(from) << 32 | to) ^ 0x5E4D000000000000ull : 0);
}

bool Document::setSlotPlugin(ObjectId track, int slot, const std::string& pluginType) {
  if (slot < 0 || slot >= kEffectSlots) return false;
  if (!pluginType.empty()) {
    const PluginInfo* p = findPlugin(pluginType);
    if (!p || p->kind != PluginKind::Effect) return false;
  }
  const char* label = pluginType.empty() ? "remove effect" : "insert effect";
  beginGroup(label);
  bool ok = editMixerTrack(track, label, [&](MixerTrack& t) {
    EffectSlot& s = t.slots[static_cast<size_t>(slot)];
    s = EffectSlot{};
    if (!pluginType.empty()) s.plugin = defaultPluginState(pluginType);
  });
  if (ok) {  // links to or from the replaced plugin no longer apply
    std::vector<ModLink> links = song_.modLinks;
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const ModLink& l) {
                                 uint32_t sl = 0;
                                 return (l.owner == track && slotOf(l.index, sl) && sl == static_cast<uint32_t>(slot) &&
                                         (l.index - kSlotBase) % kSlotStride != kSlotMix) ||
                                        (l.sourceOwner == track && l.sourceSlot == slot);
                               }),
                links.end());
    setModLinks(std::move(links), label);
  }
  endGroup();
  return ok;
}

bool Document::moveSlot(ObjectId track, int from, int to) {
  if (from < 0 || to < 0 || from >= kEffectSlots || to >= kEffectSlots) return false;
  beginGroup("move effect");
  bool ok = editMixerTrack(track, "move effect", [&](MixerTrack& t) {
    EffectSlot s = t.slots[static_cast<size_t>(from)];
    t.slots.erase(t.slots.begin() + from);
    t.slots.insert(t.slots.begin() + to, s);
  });
  if (ok) {  // links follow their plugins
    std::vector<ModLink> links = song_.modLinks;
    for (auto& l : links) {
      uint32_t sl = 0;
      if (l.owner == track && slotOf(l.index, sl)) {
        uint32_t k = (l.index - kSlotBase) % kSlotStride;
        l.index = kSlotBase + static_cast<uint32_t>(movedIndex(static_cast<int>(sl), from, to)) * kSlotStride + k;
      }
      if (l.sourceOwner == track && l.sourceSlot >= 0) l.sourceSlot = movedIndex(l.sourceSlot, from, to);
    }
    setModLinks(std::move(links), "move effect");
    // Q-8: automation clips, pattern event lanes and MIDI controller links
    // that target a slot parameter of this track follow their effect too.
    auto remap = [&](ObjectId owner, uint32_t& index) {
      uint32_t sl = 0;
      if (owner != track || !slotOf(index, sl)) return false;
      const uint32_t k = (index - kSlotBase) % kSlotStride;
      const uint32_t n = kSlotBase + static_cast<uint32_t>(movedIndex(static_cast<int>(sl), from, to)) * kSlotStride + k;
      if (n == index) return false;
      index = n;
      return true;
    };
    auto hits = [&](ObjectId owner, uint32_t index) { return remap(owner, index); };  // by value: a dry run
    bool any = false;
    for (const auto& c : song_.automations) any = any || hits(c.owner, c.index);
    for (const auto& p : song_.patterns)
      for (const auto& l : p.automation) any = any || hits(l.owner, l.index);
    for (const auto& l : song_.links) any = any || hits(l.owner, l.index);
    if (any)
      editSong("move effect", [&](Song& x) {
        for (auto& c : x.automations) remap(c.owner, c.index);
        for (auto& p : x.patterns)
          for (auto& l : p.automation) remap(l.owner, l.index);
        for (auto& l : x.links) remap(l.owner, l.index);
      });
  }
  endGroup();
  return ok;
}

bool Document::setSendSidechain(ObjectId from, ObjectId to, bool sidechainOnly) {
  const MixerTrack* src = song_.mixerTrack(from);
  if (!src) return false;
  bool exists = false;
  for (const auto& sd : src->sends) exists = exists || sd.to == to;
  if (!exists) return false;
  return editMixerTrack(from, sidechainOnly ? "sidechain to mixer track" : "route mixer track", [&](MixerTrack& t) {
    for (auto& sd : t.sends)
      if (sd.to == to) sd.sidechain = sidechainOnly;
  });
}

// --- internal controller links ------------------------------------------------

void Document::setModLinks(std::vector<ModLink> after, const std::string& label) {
  if (after == song_.modLinks) return;
  exec(std::make_unique<ModLinksCommand>(label, song_.modLinks, std::move(after), 0));
}

bool Document::setModLink(const ModLink& link) {
  if (!describeParam(song_, {link.owner, link.index})) return false;
  if (!isControllerSource(song_, link.sourceOwner, link.sourceSlot)) return false;
  std::vector<ModLink> links = song_.modLinks;
  bool replaced = false;
  for (auto& l : links)
    if (l.owner == link.owner && l.index == link.index) {
      l = link;
      replaced = true;
    }
  if (!replaced) links.push_back(link);
  undo_.breakMerge();
  setModLinks(std::move(links), "link to controller");
  undo_.breakMerge();
  return true;
}

bool Document::removeModLink(ParamId target) {
  std::vector<ModLink> links = song_.modLinks;
  auto it = std::remove_if(links.begin(), links.end(),
                           [&](const ModLink& l) { return l.owner == target.owner && l.index == target.index; });
  if (it == links.end()) return false;
  links.erase(it, links.end());
  undo_.breakMerge();
  setModLinks(std::move(links), "remove controller link");
  undo_.breakMerge();
  return true;
}

bool Document::editModLink(ParamId target, const std::string& label, const std::function<void(ModLink&)>& fn,
                           uint64_t mergeKey) {
  std::vector<ModLink> links = song_.modLinks;
  ModLink* l = nullptr;
  for (auto& x : links)
    if (x.owner == target.owner && x.index == target.index) l = &x;
  if (!l) return false;
  fn(*l);
  l->owner = target.owner;  // the target identifies the link
  l->index = target.index;
  l->min = std::clamp(l->min, 0.0f, 1.0f);
  l->max = std::clamp(l->max, 0.0f, 1.0f);
  l->smoothMs = std::clamp(l->smoothMs, 0.0f, 10000.0f);
  if (links == song_.modLinks) return true;
  exec(std::make_unique<ModLinksCommand>(label, song_.modLinks, std::move(links), mergeKey));
  return true;
}

// --- channel note processors --------------------------------------------------

bool Document::addNoteFx(ObjectId channel, const std::string& type, int index) {
  const PluginInfo* info = findPlugin(type);
  const Channel* c = song_.channel(channel);
  if (!info || info->kind != PluginKind::NoteFx || !c || c->noteFx.size() >= static_cast<size_t>(kMaxNoteFx)) return false;
  int n = static_cast<int>(c->noteFx.size());
  if (index < 0 || index > n) index = n;
  beginGroup("add note processor");
  editChannel(channel, "add note processor", [&](Channel& x) {
    x.noteFx.insert(x.noteFx.begin() + index, defaultPluginState(type));
  });
  std::vector<ModLink> links = song_.modLinks;
  for (auto& l : links) {
    uint32_t fx = 0;
    if (l.owner == channel && noteFxOf(l.index, fx) && fx >= static_cast<uint32_t>(index)) l.index += kNoteFxStride;
  }
  setModLinks(std::move(links), "add note processor");
  endGroup();
  return true;
}

bool Document::removeNoteFx(ObjectId channel, int index) {
  const Channel* c = song_.channel(channel);
  if (!c || index < 0 || index >= static_cast<int>(c->noteFx.size())) return false;
  beginGroup("remove note processor");
  editChannel(channel, "remove note processor", [&](Channel& x) { x.noteFx.erase(x.noteFx.begin() + index); });
  std::vector<ModLink> links;
  for (auto l : song_.modLinks) {
    uint32_t fx = 0;
    if (l.owner == channel && noteFxOf(l.index, fx)) {
      if (fx == static_cast<uint32_t>(index)) continue;
      if (fx > static_cast<uint32_t>(index)) l.index -= kNoteFxStride;
    }
    links.push_back(l);
  }
  setModLinks(std::move(links), "remove note processor");
  endGroup();
  return true;
}

bool Document::moveNoteFx(ObjectId channel, int from, int to) {
  const Channel* c = song_.channel(channel);
  int n = c ? static_cast<int>(c->noteFx.size()) : 0;
  if (!c || from < 0 || to < 0 || from >= n || to >= n) return false;
  if (from == to) return true;
  beginGroup("move note processor");
  editChannel(channel, "move note processor", [&](Channel& x) {
    PluginState p = x.noteFx[static_cast<size_t>(from)];
    x.noteFx.erase(x.noteFx.begin() + from);
    x.noteFx.insert(x.noteFx.begin() + to, p);
  });
  std::vector<ModLink> links = song_.modLinks;
  for (auto& l : links) {
    uint32_t fx = 0;
    if (l.owner == channel && noteFxOf(l.index, fx)) {
      uint32_t k = (l.index - kNoteFxBase) % kNoteFxStride;
      l.index = kNoteFxBase + static_cast<uint32_t>(movedIndex(static_cast<int>(fx), from, to)) * kNoteFxStride + k;
    }
  }
  setModLinks(std::move(links), "move note processor");
  endGroup();
  return true;
}

bool Document::setNoteFxState(ObjectId channel, int index, const PluginState& state, const std::string& label,
                              uint64_t mergeKey) {
  const Channel* c = song_.channel(channel);
  if (!c || index < 0 || index >= static_cast<int>(c->noteFx.size())) return false;
  PluginState st = state;
  normalizePluginState(st);
  return editChannel(channel, label, [&](Channel& x) { x.noteFx[static_cast<size_t>(index)] = st; }, mergeKey);
}

// --- song ---------------------------------------------------------------------

bool Document::setPpq(int ppq, int* roundedOut) {
  if (!isValidPpq(ppq)) return false;
  if (roundedOut) *roundedOut = countPpqRoundings(song_, ppq);
  if (ppq == song_.ppq) return true;
  // Rescaling can round, so revert restores an exact snapshot.
  editSong("change PPQ", [ppq](Song& s) { changePpq(s, ppq); });
  return true;
}

bool Document::setTimeSignature(int num, int den) {
  if (num < 1 || num > 16 || (den != 2 && den != 4 && den != 8 && den != 16)) return false;
  editSong("change time signature", [num, den](Song& s) {
    s.timeSigNum = num;
    s.timeSigDen = den;
  });
  return true;
}

void Document::setInfo(const SongInfo& info) {
  editSong("edit song info", [&info](Song& s) { s.info = info; });
}

}  // namespace gl
