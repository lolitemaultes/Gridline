// Gridline - "tracker" pack, K-106 cp17: the Tracker Sampler's editing session (tracker_session.h):
// the memory per instrument, the clipboard, the snapshot cache and the operations. UI thread.
#include "tracker_session.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include "../../app/App.h"
#include "../../app/AudioRecordUi.h"
#include "../../app/PluginUI.h"
#include "../../core/I18n.h"
#include "../../engine/Sample.h"

namespace gl::packs::trackerpack {

namespace {
std::map<uint64_t, Memory>& memories() {
  static std::map<uint64_t, Memory> m;
  return m;
}
long long gBuilt = 0, gServed = 0;
}  // namespace

Memory& memory(uint64_t targetKey) { return memories()[targetKey]; }

const Memory* findMemory(uint64_t targetKey) {
  const auto it = memories().find(targetKey);
  return it == memories().end() ? nullptr : &it->second;
}

Span rangeOf(const Memory& m, int k) {
  if (m.selSlot != k || m.selA < 0 || m.selB < 0) return Span{};
  return Span{std::min(m.selA, m.selB), std::max(m.selA, m.selB)};
}

Wave& clipboard() {
  static Wave w;
  return w;
}

uint64_t mergeKey(const PluginTarget& t, int field) { return mergeKeyFor(t.key(), field); }

bool commitEdit(App& a, const PluginTarget& t, const Edit& e, const char* label, uint64_t merge) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur) return false;
  PluginState ns = *cur;
  ns.data = writeEdit(e);
  if (ns.data == cur->data) return false;
  setPluginState(a, t, ns, label, merge);
  return true;
}

const Snapshot& snapshot(App& a, const PluginTarget& t) {
  struct Entry {
    uint64_t revision = ~uint64_t{0};
    uint64_t samples = 0;  // the sample cache's generation
    const PluginState* st = nullptr;
    Snapshot snap;
  };
  static std::map<uint64_t, Entry> cache;
  static const Snapshot none;
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != trk::kInstrumentType) return none;
  Entry& en = cache[t.key()];
  const uint64_t rev = a.doc.revision();
  // (The state's address and the revision: an edit anywhere bumps the revision; the song swapped
  // for another moves the state.)
  const uint64_t gen = sampleCache().generation();
  if (en.revision == rev && en.st == st && en.snap.ok) {
    // K-106 cp20: a file that arrived (a collaborator's, a decode finished) since: the instrument
    // again (the same one unless it lacked a file).
    if (en.samples != gen) {
      en.samples = gen;
      if (auto inst = instrumentFor(*st)) {
        en.snap.inst = inst;
      }
    }
    ++gServed;
    return en.snap;
  }
  ++gBuilt;
  en.revision = rev, en.st = st, en.samples = gen;
  en.snap.ok = true;
  en.snap.st = st;
  en.snap.inst = instrumentFor(*st);
  en.snap.sound = soundOf(*st);
  en.snap.e = readEdit(st->data, en.snap.sound);
  en.snap.ok = en.snap.inst != nullptr;
  return en.snap;
}

void snapshotCounts(long long* built, long long* served) {
  if (built) *built = gBuilt;
  if (served) *served = gServed;
}

namespace {
// The current state read, `f` applied (false: nothing to do), one commit.
template <class F>
bool run(App& a, const PluginTarget& t, const char* label, uint64_t merge, Edit* mirror, F&& f) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != trk::kInstrumentType) return false;
  Edit e = readEdit(st->data, soundOf(*st));
  if (!f(e)) return false;
  const bool changed = commitEdit(a, t, e, label, merge);
  if (mirror) *mirror = e;
  return changed;
}
}  // namespace

bool opSampleField(App& a, const PluginTarget& t, int k, SampleField f, long v, Edit* mirror) {
  return run(a, t, sampleFieldLabel(f), mergeKey(t, sampleFieldMerge(f)), mirror, [&](Edit& e) {
    applySampleField(e, k, f, v);
    return true;
  });
}

bool opSampleRoot(App& a, const PluginTarget& t, int k, double rate, int note, Edit* mirror) {
  return run(a, t, "sample root", mergeKey(t, 5), mirror, [&](Edit& e) {
    setRootNote(e.text.def.samples[static_cast<size_t>(k)], rate, note);
    return true;
  });
}

bool opSampleLoopType(App& a, const PluginTarget& t, int k, uint8_t loop, int32_t s0, int32_t s1, Edit* mirror) {
  return run(a, t, "sample loop mode", 0, mirror, [&](Edit& e) {
    applyLoopType(e, k, loop, s0, s1);
    return true;
  });
}

bool opSampleBits(App& a, const PluginTarget& t, int k, bool eight, Edit* mirror) {
  return run(a, t, "sample bit depth", 0, mirror, [&](Edit& e) {
    e.eightBit[static_cast<size_t>(k)] = eight;
    return true;
  });
}

bool opSampleMarker(App& a, const PluginTarget& t, int k, int marker, int32_t f, int32_t frames, Edit* mirror) {
  return run(a, t, markerLabel(marker), mergeKey(t, 100 + marker), mirror, [&](Edit& e) {
    applyMarker(e, k, marker, f, frames);
    return true;
  });
}

bool opRenameSample(App& a, const PluginTarget& t, int k, const std::string& name, Edit* mirror) {
  return run(a, t, "rename sample", 0, mirror, [&](Edit& e) {
    e.text.sampleNames[static_cast<size_t>(k)] = name;
    return true;
  });
}

bool opSwapSamples(App& a, const PluginTarget& t, int k, int to, Edit* mirror) {
  return run(a, t, "move sample", 0, mirror, [&](Edit& e) {
    swapSamples(e, k, to);
    return true;
  });
}

int opDuplicateSample(App& a, const PluginTarget& t, int k, Edit* mirror) {
  int to = -1;
  run(a, t, "copy sample", 0, mirror, [&](Edit& e) {
    to = duplicateSample(e, k);
    return to >= 0;
  });
  return to;
}

bool opRemoveSample(App& a, const PluginTarget& t, int k, Edit* mirror) {
  return run(a, t, "remove sample", 0, mirror, [&](Edit& e) {
    removeSample(e, k);
    return true;
  });
}

bool opRenameInstrument(App& a, const PluginTarget& t, const std::string& name, Edit* mirror) {
  return run(a, t, "rename instrument", 0, mirror, [&](Edit& e) {
    e.text.name = name;
    return true;
  });
}

bool opInsFadeout(App& a, const PluginTarget& t, long v, Edit* mirror) {
  return run(a, t, "fadeout", mergeKey(t, 30), mirror, [&](Edit& e) {
    applyFadeout(e, v);
    return true;
  });
}

bool opInsVibrato(App& a, const PluginTarget& t, VibField f, long v, Edit* mirror) {
  return run(a, t, vibLabel(f), mergeKey(t, vibMerge(f)), mirror, [&](Edit& e) {
    applyVibrato(e, f, v);
    return true;
  });
}

bool opInsVibShape(App& a, const PluginTarget& t, int shape, Edit* mirror) {
  return run(a, t, "vibrato shape", 0, mirror, [&](Edit& e) {
    e.text.def.vibType = static_cast<uint8_t>(shape);
    return true;
  });
}

bool opEnvFlag(App& a, const PluginTarget& t, bool pan, uint8_t flag, Edit* mirror) {
  return run(a, t, envLabel(pan), 0, mirror, [&](Edit& e) {
    applyEnvFlag(e, pan, flag);
    return true;
  });
}

int opEnvPointAdd(App& a, const PluginTarget& t, bool pan, int x, int y, Edit* mirror) {
  int i = -1;
  run(a, t, envLabel(pan), mergeKey(t, 20 + (pan ? 1 : 0)), mirror, [&](Edit& e) {
    i = applyEnvAdd(e, pan, x, y);
    return i >= 0;
  });
  return i;
}

bool opEnvPointMove(App& a, const PluginTarget& t, bool pan, int i, int x, int y, Edit* mirror) {
  return run(a, t, envLabel(pan), mergeKey(t, 20 + (pan ? 1 : 0)), mirror, [&](Edit& e) { return applyEnvMove(e, pan, i, x, y); });
}

bool opEnvPointDelete(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror) {
  return run(a, t, envLabel(pan), 0, mirror, [&](Edit& e) { return applyEnvDelete(e, pan, i); });
}

bool opEnvSustain(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror) {
  return run(a, t, envLabel(pan), 0, mirror, [&](Edit& e) {
    applyEnvSustain(e, pan, i);
    return true;
  });
}

bool opEnvLoopStart(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror) {
  return run(a, t, envLabel(pan), 0, mirror, [&](Edit& e) {
    applyEnvLoopStart(e, pan, i);
    return true;
  });
}

bool opEnvLoopEnd(App& a, const PluginTarget& t, bool pan, int i, Edit* mirror) {
  return run(a, t, envLabel(pan), 0, mirror, [&](Edit& e) {
    applyEnvLoopEnd(e, pan, i);
    return true;
  });
}

bool opEnvApplySlot(App& a, const PluginTarget& t, bool pan, const EnvSlot& slot, Edit* mirror) {
  return run(a, t, pan ? "panning envelope slot" : "volume envelope slot", 0, mirror, [&](Edit& e) {
    applyEnvSlot(e.text.def, slot, pan);
    return true;
  });
}

bool opMapKeys(App& a, const PluginTarget& t, int lo, int hi, int slot, Edit* mirror) {
  return run(a, t, "map keys", 0, mirror, [&](Edit& e) {
    mapKeys(e, lo, hi, slot);
    return true;
  });
}

bool opMapKeysAway(App& a, const PluginTarget& t, int slot, Edit* mirror) {
  return run(a, t, "map keys", 0, mirror, [&](Edit& e) { return applyKeysAway(e, slot); });
}

std::string envSlotsPath(App& a) { return (std::filesystem::u8path(a.configDir()) / "tracker-envelopes.txt").u8string(); }
EnvSlots& envSlots(App& a) {
  static EnvSlots s;
  static bool loaded = false;
  if (!loaded) {
    s = loadEnvSlots(envSlotsPath(a));
    loaded = true;
  }
  return s;
}

bool opSetMidi(App& a, const PluginTarget& t, const MidiSettings& m, bool syncMute, const char* label, int mergeField, Edit* mirror) {
  return run(a, t, label, mergeField > 0 ? mergeKey(t, mergeField) : 0, mirror, [&](Edit& e) {
    applyMidi(e, m, syncMute);
    return true;
  });
}

bool opWaveCommit(App& a, const PluginTarget& t, int k, const Wave& w, const char* label, Edit* mirror) {
  const PluginState* st = t.state(a.doc.song());
  if (!st || st->type != trk::kInstrumentType) return false;
  Edit e = readEdit(st->data, soundOf(*st));
  const std::string path = commitWave(e, k, w, editedDir(a), editName(e, k));
  if (path.empty()) {
    a.notify(i18n::tr("The edited sample could not be written"), true);
    return false;
  }
  (void)sampleCache().get(path);
  commitEdit(a, t, e, label);
  if (mirror) *mirror = e;
  return true;
}

}  // namespace gl::packs::trackerpack
