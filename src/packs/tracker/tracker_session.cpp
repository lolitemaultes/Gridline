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
#include "../../core/PluginInfo.h"
#include "../../core/PluginParams.h"
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

// ---- 0.7.8: a Sampler seen as a one-sample Tracker Sampler (the Tracker's classic Sample editor) ----
namespace {
bool isSampler(const PluginState* st) { return st && st->type == "gl.sampler"; }
struct Virtual {
  std::string data;    // the Sampler's file and settings the view was made from
  std::vector<float> params;
  uint64_t gen = ~uint64_t{0};
  PluginState state;   // the Tracker Sampler it reads as
};
std::map<uint64_t, Virtual>& virtuals() {
  static std::map<uint64_t, Virtual> m;
  return m;
}
// The Sampler's file as sample 01 of a Tracker Sampler: its start and end the sample's, its loop
// points and type, its reverse as Backwards (fromSamplerState, as Convert makes it). No file yet:
// an instrument with no sample (the editor's Load picks one).
const PluginState& virtualOf(const PluginTarget& t, const PluginState& smp) {
  Virtual& v = virtuals()[t.key()];
  const uint64_t gen = sampleCache().generation();
  if (v.gen == gen && v.data == smp.data && v.params == smp.params) return v.state;
  v.data = smp.data, v.params = smp.params, v.gen = gen;
  v.state = defaultPluginState(trk::kInstrumentType);
  if (!smp.data.empty()) {
    const SamplePtr sp = sampleCache().get(smp.data);
    if (sp && sp->frames() > 0)
      v.state = fromSamplerState(smp, static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF)), sp->sampleRate > 0 ? sp->sampleRate : 44100.0,
                                 std::filesystem::u8path(smp.data).stem().u8string());
  }
  return v.state;
}
float frac(int32_t f, int32_t frames) { return frames > 0 ? std::clamp(static_cast<float>(static_cast<double>(f) / frames), 0.0f, 1.0f) : 0.0f; }
// The edit written back into the Sampler: its file (a new one after an audio edit or a load), the
// start and end, the loop and Backwards. What a Sampler cannot hold (a second sample, 8 bits) is
// refused with a note. One undo step, as the Tracker Sampler's.
bool commitSampler(App& a, const PluginTarget& t, const PluginState& cur, const Edit& e, const char* label, uint64_t merge) {
  const Edit was = readEdit(virtualOf(t, cur).data, 0);
  for (int k = 1; k < trk::kMaxSamples; ++k)
    if (hasSample(e, k) && !hasSample(was, k)) {
      a.notify(i18n::tr("A Sampler plays one sample: convert it to a Tracker Sampler for more entries"));
      return false;
    }
  if (e.eightBit[0] != was.eightBit[0]) {
    a.notify(i18n::tr("A Sampler plays its sample as it is: convert it to a Tracker Sampler for 8-bit"));
    return false;
  }
  const trk::InstrumentText& tx = e.text;
  const int fi = tx.fileOf[0];
  const std::string path = fi >= 0 && fi < static_cast<int>(tx.files.size()) ? tx.files[static_cast<size_t>(fi)] : std::string();
  if (path.empty()) {
    a.notify(i18n::tr("A Sampler keeps its sample: load another file to replace it"));
    return false;
  }
  const SamplePtr sp = sampleCache().get(path);
  const int32_t frames = sp ? static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF)) : 0;
  if (frames <= 0) {
    a.notify(i18n::tr("The sample file is missing"));
    return false;
  }
  PluginState ns = cur;
  const bool newFile = path != cur.data;
  ns.data = path;
  if (ns.params.size() < static_cast<size_t>(pp::SmpCount)) {
    const PluginState d = defaultPluginState("gl.sampler");
    for (size_t i = ns.params.size(); i < d.params.size(); ++i) ns.params.push_back(d.params[i]);
  }
  auto set = [&](int i, float v) {
    if (i < static_cast<int>(ns.params.size())) ns.params[static_cast<size_t>(i)] = v;
  };
  const trk::SampleDef& sd = tx.def.samples[0];
  const trk::SampleDef& od = was.text.def.samples[0];
  // (only what the edit changed: the rest keeps the Sampler's own values exactly)
  if (newFile || e.cuts.start[0] != was.cuts.start[0]) set(pp::SmpStart, frac(e.cuts.start[0], frames));
  if (newFile || e.cuts.end[0] != was.cuts.end[0]) set(pp::SmpEnd, e.cuts.end[0] > 0 ? frac(e.cuts.end[0], frames) : 1.0f);
  const bool loops = sd.loop != trk::kLoopOff && sd.loopLength > 0;
  if (newFile || sd.loop != od.loop || sd.loopStart != od.loopStart || sd.loopLength != od.loopLength) {
    set(pp::SmpLoop, loops ? 1.0f : 0.0f);
    if (loops) {
      set(pp::SmpLoopMode, sd.loop == trk::kLoopPingPong ? 1.0f : 0.0f);
      set(pp::SmpLoopStart, frac(sd.loopStart, frames));
      set(pp::SmpLoopEnd, frac(sd.loopStart + sd.loopLength, frames));
    }
  }
  if (sd.reverse != od.reverse) set(pp::SmpReverse, sd.reverse ? 1.0f : 0.0f);
  if (ns.data == cur.data && ns.params == cur.params) return false;
  setPluginState(a, t, ns, label, merge);
  return true;
}
}  // namespace

const PluginState* editState(App& a, const PluginTarget& t) {
  const PluginState* st = t.state(a.doc.song());
  if (st && st->type == trk::kInstrumentType) return st;
  if (isSampler(st) && t.isChannel()) return &virtualOf(t, *st);
  return nullptr;
}

namespace {
bool gBridge = false;
}  // namespace
void setSamplerBridge(bool on) { gBridge = on; }
bool samplerBridge() { return gBridge; }

bool bridgesSampler(App& a, const PluginTarget& t) { return t.isChannel() && isSampler(t.state(a.doc.song())); }

bool commitEdit(App& a, const PluginTarget& t, const Edit& e, const char* label, uint64_t merge) {
  const PluginState* cur = t.state(a.doc.song());
  if (!cur) return false;
  if (isSampler(cur) && t.isChannel()) return commitSampler(a, t, *cur, e, label, merge);
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
  const PluginState* st = editState(a, t);  // (0.7.8: a Sampler's, as a one-sample Tracker Sampler)
  if (!st) return none;
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
  en.snap.sampler = bridgesSampler(a, t);
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
  const PluginState* st = editState(a, t);
  if (!st) return false;
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

bool opSampleReverse(App& a, const PluginTarget& t, int k, bool backwards, Edit* mirror) {
  if (k < 0 || k >= trk::kMaxSamples) return false;
  return run(a, t, "sample play direction", 0, mirror, [&](Edit& e) {
    trk::SampleDef& sd = e.text.def.samples[static_cast<size_t>(k)];
    if (!sd.present() || sd.reverse == backwards) return false;
    sd.reverse = backwards;
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

bool opCopyEntry(App& a, const PluginTarget& t, int from, int to, Edit* mirror) {
  return run(a, t, "copy sample", 0, mirror, [&](Edit& e) { return from != to && copySampleInto(e, to, e, from) >= 0; });
}

bool opExchangeEntries(App& a, const PluginTarget& t, int x, int y, Edit* mirror) {
  return run(a, t, "exchange samples", 0, mirror, [&](Edit& e) {
    if (x == y || x < 0 || y < 0 || x >= trk::kMaxSamples || y >= trk::kMaxSamples || (!hasSample(e, x) && !hasSample(e, y))) return false;
    exchangeSampleData(e, x, y);
    return true;
  });
}

int opRegionToEntry(App& a, const PluginTarget& t, int from, Span r, int to, Edit* mirror) {
  int slot = -1;
  run(a, t, "range to entry", 0, mirror, [&](Edit& e) {
    slot = regionToEntry(e, from, r, to);
    return slot >= 0;
  });
  return slot;
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
  const PluginState* st = editState(a, t);
  if (!st) return false;
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
