// Gridline - the Live layout's own chrome (K-112 phase C). See LiveLook.h.
#include "LiveLook.h"
#include "AutomationEditor.h"
#include "LiveGrid.h"
#include "PixelGrid.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/LayoutShell.h"
#include "../core/MenuOps.h"
#include "../core/MenuParity.h"
#include "../core/Params.h"
#include "../core/PluginInfo.h"
#include "../core/RemoteFs.h"
#include "../core/Arrangements.h"
#include "../core/ClipsMeter.h"
#include "../core/ClipLoop.h"
#include "../core/ClipWindow.h"
#include "../core/TrackModes.h"
#include "../core/MixerHide.h"
#include "../core/TrackOrder.h"
#include "../engine/Chop.h"
#include "../engine/ClipOps.h"
#include "../engine/Dsp.h"
#include "MenuParityUi.h"
#include "../packs/PacksUi.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "BrowserUi.h"
#include "ClipEditor.h"
#include "CollabUi.h"
#include "ContextMenus.h"
#include "CursorAnchor.h"
#include "DeviceViews.h"
#include "HostedUi.h"
#include "Keys.h"
#include "HostedBodies.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "LiveIo.h"
#include "MixerMeters.h"
#include "PluginUI.h"
#include "Theme.h"
#include "TrackModeUi.h"
#include "MixerHideUi.h"
#include "Widgets.h"
#include "Windows.h"
#include "RenderSheetUi.h"

namespace gl::livelook {
namespace {
void pendingLaunchFrame(App& a);  // grid-1118 (the clip grid's part, below)
}

using theme::pal;

bool active(const App& a) { return layoutui::active() == layout::Preset::Live && layoutui::shellBuilt(a, layout::Preset::Live); }

bool gOutside = true;  // N45-10: a frame outside Clips went by (the next Clips frame is an entry)
void leftFrame() {
  GImGui->ConfigNavEnableTabbing = true;  // (C44-03: tabbing as before outside Clips)
  gOutside = true;
}

Tick arrangementOrigin(const App& a) { return active(a) ? std::max<Tick>(0, a.doc.song().live.areaEnd) : 0; }

namespace {
// Checkpoint 55 (gap 5): the dragged row heights, loaded from the person's settings once.
// Checkpoint 56 (N55-06): keyed by the Set, not only its path: an unsaved Set has a key of its own
// ("unsaved <time>-<epoch>"), which a first save or Save As carries over to the file (as a Save As
// from one file to another does); a Set renamed or moved outside Gridline is found again by the
// fingerprint of its file's bytes as last saved here (clips.rowHeightFiles). Each line carries the
// time it was last set, and the oldest lines go first past the limit. Never in the song.
struct RowScale {
  float v = 1.0f;
  long long at = 0;  // seconds since 1970, when it was last set (0: from before checkpoint 56)
};
std::map<std::string, RowScale> gRowScales;  // "<set>\t<track key>" -> scale
bool gRowScalesLoaded = false;
std::string gSetKey;              // the open Set's key (its file, else its unsaved key)
uint64_t gSetEpoch = UINT64_MAX;  // the project the key is for
constexpr size_t kRowLinesMax = 400, kRowFilesMax = 100;
std::string rowKey(const App& a, int t) {
  const Song& s = a.doc.song();
  if (t < 0 || t >= static_cast<int>(s.tracks.size())) {
    return std::string();
  }
  const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
  std::string k;
  if (tr.mode == TrackMode::Audio && tr.mixer != kNoId) {
    k = "m" + std::to_string(tr.mixer);
  } else if (tr.mode == TrackMode::Instrument && tr.channel != kNoId) {
    k = "c" + std::to_string(tr.channel);
  } else {
    k = "t" + std::to_string(t);
  }
  return (gSetKey.empty() ? a.projectFile() : gSetKey) + "\t" + k;
}
std::vector<std::string> splitLines(const std::string& all) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at < all.size()) {
    size_t nl = all.find('\n', at);
    if (nl == std::string::npos) {
      nl = all.size();
    }
    out.push_back(all.substr(at, nl - at));
    at = nl + 1;
  }
  return out;
}
void loadRowScales(const App& a) {
  if (gRowScalesLoaded) {
    return;
  }
  gRowScalesLoaded = true;
  for (const std::string& line : splitLines(a.settings.getString("clips.rowHeights"))) {
    // "<set>\t<track>\t<per mille>" (checkpoint 55) or "...\t<per mille>\t<time>" (56).
    std::vector<size_t> tabs;
    for (size_t k = 0; k < line.size(); ++k) {
      if (line[k] == '\t') {
        tabs.push_back(k);
      }
    }
    if (tabs.size() < 2) {
      continue;
    }
    const bool stamped = tabs.size() >= 3;
    const size_t vt = stamped ? tabs[tabs.size() - 2] : tabs.back();
    RowScale r;
    r.v = static_cast<float>(std::atol(line.c_str() + vt + 1)) / 1000.0f;  // per mille
    r.at = stamped ? std::atoll(line.c_str() + tabs.back() + 1) : 0;
    if (r.v >= 0.25f && r.v <= 4.0f) {
      gRowScales[line.substr(0, vt)] = r;
    }
  }
}
void storeRowScales(App& a) {
  if (gRowScales.size() > kRowLinesMax) {  // the newest stay, the oldest go first
    std::vector<std::pair<std::string, RowScale>> v(gRowScales.begin(), gRowScales.end());
    std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second.at > y.second.at; });
    v.resize(kRowLinesMax);
    gRowScales = std::map<std::string, RowScale>(v.begin(), v.end());
  }
  std::string all;
  for (const auto& [key, r] : gRowScales) {
    all += (all.empty() ? "" : "\n") + key + "\t" + std::to_string(std::lround(r.v * 1000.0f)) + "\t" + std::to_string(r.at);
  }
  a.settings.set("clips.rowHeights", all);
}
// Moves every line of Set `from` to Set `to` (a Save As, a first save, a Set found again).
bool moveRowScales(const std::string& from, const std::string& to) {
  if (from.empty() || to.empty() || from == to) {
    return false;
  }
  const std::string pre = from + "\t";
  bool moved = false;
  for (auto it = gRowScales.lower_bound(pre); it != gRowScales.end() && it->first.compare(0, pre.size(), pre) == 0;) {
    gRowScales[to + "\t" + it->first.substr(pre.size())] = it->second;
    it = gRowScales.erase(it);
    moved = true;
  }
  return moved;
}
bool hasRowScales(const std::string& set) {
  const std::string pre = set + "\t";
  const auto it = gRowScales.lower_bound(pre);
  return it != gRowScales.end() && it->first.compare(0, pre.size(), pre) == 0;
}
}  // namespace

std::string fileFingerprint(const std::string& path) {
  // FRZ-1118: never on a web or network drive (the whole file, read on the interface thread): a Set
  // there keeps its row heights by name only.
  if (remotefs::isRemote(path)) {
    return std::string();
  }
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return std::string();
  }
  uint64_t h = 1469598103934665603ull;  // FNV-1a, 64 bits
  std::vector<unsigned char> buf(65536);
  size_t n = 0, total = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    for (size_t k = 0; k < n; ++k) {
      h = (h ^ buf[k]) * 1099511628211ull;
    }
    total += n;
  }
  std::fclose(f);
  if (total == 0) {
    return std::string();
  }
  static const char* hex = "0123456789abcdef";
  std::string out(16, '0');
  for (int k = 15; k >= 0; --k) {
    out[static_cast<size_t>(k)] = hex[h & 15u];
    h >>= 4;
  }
  return out + "-" + std::to_string(total);
}

std::string setKey(const App& a) { return gSetKey.empty() ? a.projectFile() : gSetKey; }

void rowHeightsFrame(App& a) {
  const uint64_t ep = a.doc.projectEpoch();
  const std::string& file = a.projectFile();
  if (ep == gSetEpoch && (gSetKey == file || (file.empty() && !gSetKey.empty()))) {
    return;
  }
  loadRowScales(a);
  if (ep == gSetEpoch) {  // the same Set, saved under a new name (a first save, Save As): its rows go along
    if (moveRowScales(gSetKey, file)) {
      storeRowScales(a);
    }
    gSetKey = file;
    return;
  }
  gSetEpoch = ep;
  if (file.empty()) {  // a new Set: a key of its own, never another unsaved Set's
    gSetKey = "unsaved " + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + std::to_string(ep);
    return;
  }
  gSetKey = file;
  if (hasRowScales(file)) {
    return;
  }
  // Renamed or moved outside Gridline: the same bytes as a file last saved here under another name.
  const std::string fp = fileFingerprint(file);
  if (fp.empty()) {
    return;
  }
  for (const std::string& line : splitLines(a.settings.getString("clips.rowHeightFiles"))) {
    const size_t t = line.rfind('\t');
    if (t != std::string::npos && line.compare(t + 1, std::string::npos, fp) == 0 && line.compare(0, t, file) != 0 &&
        hasRowScales(line.substr(0, t))) {
      moveRowScales(line.substr(0, t), file);
      storeRowScales(a);
      return;
    }
  }
}

void rowHeightsSaved(App& a, const std::string& file) {
  rowHeightsFrame(a);  // (a first save or Save As carries the rows over first)
  if (file.empty() || gSetKey != file || !hasRowScales(file)) {
    return;
  }
  const std::string fp = fileFingerprint(file);
  if (fp.empty()) {
    return;
  }
  std::vector<std::string> keep;
  for (const std::string& line : splitLines(a.settings.getString("clips.rowHeightFiles"))) {
    const size_t t = line.rfind('\t');
    if (t != std::string::npos && line.compare(0, t, file) != 0) {
      keep.push_back(line);
    }
  }
  keep.push_back(file + "\t" + fp);  // the newest last
  if (keep.size() > kRowFilesMax) {
    keep.erase(keep.begin(), keep.begin() + static_cast<std::ptrdiff_t>(keep.size() - kRowFilesMax));
  }
  std::string all;
  for (const std::string& l : keep) {
    all += (all.empty() ? "" : "\n") + l;
  }
  a.settings.set("clips.rowHeightFiles", all);
}

float rowScale(const App& a, int track, float fallback) {
  loadRowScales(a);
  if (gRowScales.empty()) {
    return fallback;
  }
  const auto it = gRowScales.find(rowKey(a, track));
  return it != gRowScales.end() ? it->second.v : fallback;
}

void setRowScale(App& a, int track, float scale, bool done) {
  loadRowScales(a);
  rowHeightsFrame(a);
  const std::string k = rowKey(a, track);
  if (k.empty()) {
    return;
  }
  RowScale& r = gRowScales[k];
  r.v = std::clamp(scale, 0.25f, 4.0f);
  r.at = static_cast<long long>(std::time(nullptr));
  if (!done) {
    return;
  }
  storeRowScales(a);
}
bool arrangementClip(const App& a, const Clip& c) { return !active(a) || c.start >= a.doc.song().live.areaEnd; }

void withoutGridClips(const App& a, Song& s, const std::function<void(Song&)>& fn) {
  std::vector<std::pair<size_t, Clip>> grid;  // (index, clip), in order
  const Tick end = active(a) ? s.live.areaEnd : 0;
  if (end > 0)
    for (size_t i = 0; i < s.playlist.size(); ++i)
      if (s.playlist[i].start < end) grid.emplace_back(i, s.playlist[i]);
  if (!grid.empty())
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [end](const Clip& c) { return c.start < end; }), s.playlist.end());
  fn(s);
  for (auto& [i, c] : grid) s.playlist.insert(s.playlist.begin() + static_cast<std::ptrdiff_t>(std::min(i, s.playlist.size())), std::move(c));
}

ImFont* numFont(const App& a) { return active(a) ? a.fontRegular : a.fontMono; }
ImU32 envelopeColour() { return theme::rgb(0xEE5A50); }

namespace {
// Checkpoint 54 (N53-10): a mixer track's level in dB by the engine's fader law (dsp::faderGain, 0.8 is
// 0 dB), as Studio's and Smooth's strips read it. (The strips read 20 log10 of the fader value before.)
double faderDb(float v) {
  const double db = dsp::gainToDb(dsp::faderGain(static_cast<double>(v), 0.8));
  return std::fabs(db) < 0.005 ? 0.0 : db;  // (never "-0.0")
}
}  // namespace

namespace {
// C47: the clip grid's side of Session record and Capture (defined with the grid below).
void launch(App& a, ObjectId clip);
// Checkpoint 48: Session record, as the reference's. It records into a clip of the grid while the other
// tracks' clips go on playing (the engine stays in its clip-launch mode): into the clip the track is
// playing (overdub), else into a new clip in the slot, launched, which grows bar by bar while it
// records (up to a scene's length, then it loops on, overdubbing). Stopping keeps the clip playing.
struct SessionRec {
  bool on = false;
  ObjectId clip = kNoId;
  int track = -1;
  bool grow = false;     // a new clip: it grows while recording, and is cut to whole bars at the stop
  bool started = false;  // the clip played (the notes are placed from then on)
  int launchTries = 0;   // frames the new clip's launch is retried (until the engine has the clip)
  double lastAt = 0;     // the content tick last frame (a smaller one: the clip looped)
  std::vector<std::pair<int, ObjectId>> extras;  // N49-04: the other armed MIDI tracks' new clips (track, clip)
};
SessionRec gRec;
// N47-01 / N48-01 (checkpoint 49): the audio part of a Session record, beside the MIDI part: every armed
// audio track of the grid records its input; each take goes into its track's slot of scene `slot`.
struct AudioSessionRec {
  bool on = false;
  bool pending = false;  // stopped: the takes are still being written (audioTakesCommitted places them)
  int track = -1;        // the first armed audio track (its slot picks the scene)
  int slot = -1;
  int waitFrames = 0;    // N49-02: frames the transport may still take to start (the MIDI clip's launch starts it)
};
AudioSessionRec gARec;
// N48-04: the history's push count when a Session record began: once every part is done, the steps
// since (the scene, the new clip, its notes and length, the takes) become one undo step.
uint64_t gRecPushes0 = 0;
bool gRecSquash = false;
int gRecSquashTries = 0;  // frames left to retry while an edit group is still open (the audio takes' commit)
void squashRecording(App& a) {
  if (!gRecSquash || gRec.on || gARec.on || gARec.pending || a.doc.groupDepth() > 0) return;  // (the takes' group lands first)
  const uint64_t n = a.doc.history().pushCount() - gRecPushes0;
  if (n <= 1 || a.doc.squashSteps(static_cast<size_t>(n), "record clip") || ++gRecSquashTries > 10) gRecSquash = false, gRecSquashTries = 0;
}
// N47-10: MIDI tracks armed with their arm buttons (this session; the reference also arms the selected
// MIDI track by itself). Audio tracks keep their mixer track's arm, as before.
std::set<int> gMidiArmed;
bool midiTrack(const Song& s, int t) {
  return t >= 0 && t < static_cast<int>(s.tracks.size()) && trackmodes::clipAllowed(s, ClipKind::Pattern, t) &&
         trackmodes::modeOf(s, t) != TrackMode::Audio;
}
// K-112c (checkpoint 51): the clip grid's and the mixer band's columns. A folded group's members take
// none (they still play and launch with their scenes); `col` is each track's column (a hidden
// member's is its group's), `n` the columns shown.
struct ColMap {
  std::vector<int> col;
  std::vector<char> hidden, group;
  int n = 0;
};
ColMap colMap(const Song& s, int nCols) {
  ColMap m;
  m.col.assign(static_cast<size_t>(std::max(0, nCols)), 0);
  m.hidden.assign(m.col.size(), 0);
  m.group.assign(m.col.size(), 0);
  int hideTo = -1;
  const std::vector<char> lanes = trackmodes::automationLanes(s, nCols);
  const std::vector<char> gaps = trackmodes::gapTracks(s, nCols);
  for (int t = 0; t < nCols; ++t) {
    if (t <= hideTo || (t < static_cast<int>(lanes.size()) && lanes[static_cast<size_t>(t)]) ||  // N52-10: an automation lane is the Arrangement's only
        (t < static_cast<int>(gaps.size()) && gaps[static_cast<size_t>(t)])) {                    // N53-04: no column for a gap track
      m.hidden[static_cast<size_t>(t)] = 1;
      m.col[static_cast<size_t>(t)] = std::max(0, m.n - 1);
      continue;
    }
    m.col[static_cast<size_t>(t)] = m.n++;
    if (trackmodes::isGroupTrack(s, t)) {
      m.group[static_cast<size_t>(t)] = 1;
      if (s.tracks[static_cast<size_t>(t)].gridFolded) hideTo = std::max(hideTo, t + s.tracks[static_cast<size_t>(t)].groupSize);  // (a group inside an open group: checkpoint 52)
    }
  }
  return m;
}
// The clip's content tick heard now (the transport tick less where the clip's content began).
double recContentTick(const App& a) { return a.engine.position() - static_cast<double>(a.engine.liveOrigin(gRec.track)); }
// Clip and pattern to `bars` bars (in the recording's undo step while it records).
void recSetBars(App& a, int bars) {
  const Clip* c = a.doc.song().clip(gRec.clip);
  const Pattern* p = c ? a.doc.song().pattern(c->pattern) : nullptr;
  if (!p || bars < 1) return;
  const ObjectId pid = p->id, cid = c->id;
  const int spb = p->stepsPerBar;
  const Tick len = static_cast<Tick>(bars) * std::max<Tick>(1, a.doc.song().barTicks());
  auto fn = [&a, pid, cid, spb, bars, len] {
    a.doc.setPatternLength(pid, bars, spb);
    a.doc.editClip(cid, "record clip length", [len](Clip& x) { x.length = len; });
  };
  if (a.recorder.active()) a.recorder.edit(a.doc, fn);
  else fn();
}
int sessionRecordSlotFor(App& a, int t, int k);
int audioRecordSlotFor(App& a, int t, int k);
// Checkpoint 59 (N58-08): a recorded or captured MIDI clip takes its track's name, as the reference names
// its takes (an empty clip made by hand stays unnamed until given one, N47-14).
void nameAfterTrack(App& a, ObjectId clip, int t) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(clip);
  if (!c || c->kind != ClipKind::Pattern || t < 0 || t >= static_cast<int>(s.tracks.size())) return;
  const std::string name = playlistTrackName(s, static_cast<size_t>(t));
  if (const Pattern* p = s.pattern(c->pattern); p && p->name.empty() && !name.empty())
    a.doc.editPattern(p->id, "clip name", [name](Pattern& x) { x.name = name; });
}
bool newMidiClipIn(App& a, int t, int k, ObjectId* made);
void ensureNoteChannel(App& a, int t);
int gridColumns(const Song& s);
Tick sceneTicks(const Song& s);
std::vector<int> slotClips(const Song& s, int t, int k);
void ensureScene(App& a, int k);
int selectedScene();
void selectSlot(App& a, int t, int k);
std::vector<ObjectId> trackChannels(const Song& s, int t);
void focusTrack(App& a, int t);
void foldedSelection(App& a, int g, bool grid);
void clearGridSel();
void gridDragFrame();  // grid2-1118
}  // namespace

int captureNow(App& a) {
  const Song& s = a.doc.song();
  if (active(a)) {
    // Checkpoint 48: Capture in the clip grid, as the reference's: into the clip the selected MIDI track
    // plays (added to its notes), else into a new clip in the selected slot (or the track's first empty
    // one), as long as the phrase in whole bars (up to a scene), which then plays.
    const int t = selectedTrack(a);
    if (!midiTrack(s, t)) {
      a.notify(i18n::tr("Select a MIDI track to capture into"), true);
      return 0;
    }
    if (a.capture.size() == 0) {
      a.notify(i18n::tr("Nothing to capture: play some notes first"));
      return 0;
    }
    const Clip* playing = s.clip(a.engine.liveActive(t));
    if (playing && playing->kind == ClipKind::Pattern) {
      const std::string name = s.pattern(playing->pattern) ? s.pattern(playing->pattern)->name : std::string();
      const int n = a.capture.commit(a.doc, playing->pattern, s.tempo, 0, a.now);
      if (n > 0) a.notify(std::to_string(n) + " " + i18n::tr("notes captured into") + " " + name);
      return n;
    }
    const int k = sessionRecordSlotFor(a, t, -1);
    a.doc.beginGroup("capture");
    ObjectId clip = kNoId;
    if (k >= 0 && newMidiClipIn(a, t, k, &clip)) {
      nameAfterTrack(a, clip, t);  // checkpoint 59 (N58-08)
      const Clip* c = a.doc.song().clip(clip);
      const ObjectId pat = c ? c->pattern : kNoId;
      const int n = pat != kNoId ? a.capture.commit(a.doc, pat, a.doc.song().tempo, 0, a.now) : 0;
      Tick end = 0;
      if (const Pattern* p = a.doc.song().pattern(pat))
        for (const auto& [ch, notes] : p->notes)
          for (const Note& nt : notes) end = std::max(end, nt.start + nt.length);
      const Tick bar = std::max<Tick>(1, a.doc.song().barTicks());
      const int bars = static_cast<int>(std::clamp<Tick>((end + bar - 1) / bar, 1, kSceneBars));
      if (const Pattern* p = a.doc.song().pattern(pat); p && bars != p->bars) {
        a.doc.setPatternLength(pat, bars, p->stepsPerBar);
        a.doc.editClip(clip, "capture", [bars, bar](Clip& x) { x.length = bar * bars; });
      }
      a.doc.endGroup();
      if (n > 0) {
        gRec = {};
        gRec.clip = clip, gRec.track = t, gRec.launchTries = 30;  // it plays (launched once the engine has it)
        a.notify(std::to_string(n) + " " + i18n::tr("notes captured into a new clip"));
      }
      return n;
    }
    a.doc.endGroup();
    a.notify(i18n::tr("No empty slot to capture into"), true);
    return 0;
  }
  const Pattern* p = s.pattern(a.curPattern);
  if (!p) {
    a.notify(i18n::tr("Select a pattern first"), true);
    return 0;
  }
  const std::string name = p->name;
  const int n = a.capture.commit(a.doc, a.curPattern, s.tempo, 0, a.now);
  if (n > 0) a.notify(std::to_string(n) + " " + i18n::tr("notes captured into") + " " + name);
  else a.notify(i18n::tr("Nothing to capture: play some notes first"));
  return n;
}

bool sessionRecording(const App& a) { return active(a) ? gRec.on || gARec.on : a.recordArmed && a.engine.mode() == PlayMode::Pattern; }
ObjectId sessionRecordClip(const App&) { return gRec.on ? gRec.clip : kNoId; }

bool sessionRecordTarget(const App& a, ObjectId& pattern, Tick& loopLength, double& origin) {
  if (!gRec.on || !gRec.started || !a.engine.playing() || a.engine.liveActive(gRec.track) != gRec.clip) return false;
  const Clip* c = a.doc.song().clip(gRec.clip);
  if (!c || c->kind != ClipKind::Pattern || !a.doc.song().pattern(c->pattern)) return false;
  pattern = c->pattern;
  loopLength = std::max<Tick>(1, a.doc.song().clipLength(*c));
  origin = static_cast<double>(a.engine.liveOrigin(gRec.track));
  return true;
}

namespace {
// N49-04: the notes the first armed MIDI track recorded go to every other armed MIDI track's new clip
// (on that track's own instrument), with the same length.
void copyToExtras(App& a) {
  if (gRec.extras.empty()) return;
  const Song& s = a.doc.song();
  const Clip* mc = s.clip(gRec.clip);
  const Pattern* mp = mc ? s.pattern(mc->pattern) : nullptr;
  if (!mp) return;
  std::vector<Note> notes;
  for (const auto& [ch, ns] : mp->notes) notes.insert(notes.end(), ns.begin(), ns.end());
  const int bars = mp->bars;
  const Tick len = s.clipLength(*mc);
  for (const auto& [xt, xc] : gRec.extras) {
    const Clip* c = a.doc.song().clip(xc);
    const Pattern* p = c ? a.doc.song().pattern(c->pattern) : nullptr;
    const Channel* ch = trackmodes::linkedChannel(a.doc.song(), xt);
    if (!p || !ch) continue;
    const ObjectId pid = p->id, chId = ch->id, clipId = xc;
    const int spb = p->stepsPerBar;
    a.doc.setPatternLength(pid, bars, spb);
    a.doc.editSong("record clip", [&notes, pid, chId, clipId, len](Song& x) {
      if (Pattern* xp = x.pattern(pid)) xp->notes[chId] = notes;
      if (Clip* cc = x.clip(clipId)) cc->length = len;
    });
  }
  gRec.extras.clear();
}
}  // namespace

void stopSessionRecord(App& a) {
  if (gARec.on) {  // the takes are written over the next frames, then placed in their slots
    gARec.on = false;
    gARec.pending = true;
  }
  if (!gRec.on) return;
  // A new clip keeps the whole bars it recorded (at least one), as the reference cuts it at the launch grid.
  if (gRec.grow && gRec.started && a.doc.song().clip(gRec.clip)) {
    const Tick bar = std::max<Tick>(1, a.doc.song().barTicks());
    const double at = recContentTick(a);
    const int bars = static_cast<int>(std::clamp<double>(std::ceil(std::max(1.0, at) / static_cast<double>(bar)), 1.0, kSceneBars));
    const Clip* c = a.doc.song().clip(gRec.clip);
    if (c && bars < static_cast<int>(a.doc.song().clipLength(*c) / bar)) recSetBars(a, bars);
  }
  copyToExtras(a);
  gRec.on = false;
  gRec.launchTries = 0;
  squashRecording(a);
}

void startSessionRecord(App& a, int t, int k) {
  const Song& s = a.doc.song();
  gRecPushes0 = a.doc.history().pushCount();
  gRecSquash = true;
  // N48-01: the reference records into the selected scene in all armed tracks: every armed audio track
  // of the grid, and the armed MIDI track (the selected one, else the first armed).
  int audioT = -1;
  if (t >= 0 && t < static_cast<int>(s.tracks.size()) && trackmodes::modeOf(s, t) == TrackMode::Audio && trackArmed(a, t)) audioT = t;
  for (int i = 0; i < gridColumns(s) && audioT < 0; ++i)
    if (trackmodes::modeOf(s, i) == TrackMode::Audio && trackArmed(a, i)) audioT = i;
  int midiT = t >= 0 ? t : selectedTrack(a);
  if (!midiTrack(s, midiT) || !trackArmed(a, midiT)) {
    midiT = -1;
    for (int i = 0; i < gridColumns(s) && midiT < 0; ++i)
      if (midiTrack(s, i) && trackArmed(a, i)) midiT = i;
  }
  if (audioT < 0 && midiT < 0) {
    gRecSquash = false;
    a.notify(i18n::tr("Arm a track to record into, or select a MIDI track"), true);
    return;
  }
  const int scene = k >= 0 ? k : selectedScene();  // the selected scene
  if (!a.engine.playing()) {  // N48-03: from a stopped transport the count-in plays first, as the reference's
    if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);  // grid-1118: over the arrangement
    if (a.countInBars > 0) a.engine.playWithCountIn(static_cast<double>(a.countInBars * std::max<Tick>(1, s.barTicks())));
    else if (audioT >= 0 && midiT < 0) a.engine.play();
    // N49-02: with a MIDI part the new clip's launch starts the transport, so the audio takes and the
    // notes begin on the same tick (a play first made the launch wait for the next bar).
  }
  if (audioT >= 0) {
    // N47-01: the armed audio tracks record their inputs in the clip launch mode; the takes land in
    // their slots of the scene when the recording stops.
    gARec = {};
    gARec.track = audioT, gARec.slot = audioRecordSlotFor(a, audioT, scene), gARec.on = true;
    gARec.waitFrames = 60;
    ensureScene(a, gARec.slot);
  }
  if (midiT < 0) return;
  gRec = {};
  gRec.track = midiT;
  // The clip the track plays (no slot named): the new notes join it.
  if (const Clip* c = a.doc.song().clip(a.engine.liveActive(midiT)); k < 0 && c && c->kind == ClipKind::Pattern && a.engine.playing()) {
    gRec.clip = c->id;
    gRec.on = true;
    return;
  }
  int mk = k >= 0 ? sessionRecordSlotFor(a, midiT, k) : -1;
  if (k < 0 &&!a.doc.song().tracks[static_cast<size_t>(midiT)].locked) {  // the selected scene's slot, or the next empty one below
    mk = std::max(0, scene);
    while (!slotClips(a.doc.song(), midiT, mk).empty()) ++mk;
  }
  ObjectId clip = kNoId;
  if (mk < 0 || !newMidiClipIn(a, midiT, mk, &clip)) {
    a.notify(i18n::tr("No empty slot to record into"), true);
    if (audioT >= 0 && !a.engine.playing() && a.countInBars <= 0) a.engine.play();
    squashRecording(a);
    return;
  }
  nameAfterTrack(a, clip, midiT);  // checkpoint 59 (N58-08): a recorded clip is named after its track
  gRec.clip = clip;
  gRec.grow = true;
  gRec.on = true;
  gRec.launchTries = 30;
  // N49-04: every other armed MIDI track records too: a new clip in its slot of the same scene, which
  // plays beside the first and takes the same notes when the recording stops (the same input reaches
  // every armed track, as the reference's).
  gRec.extras.clear();
  for (int i = 0; i < gridColumns(a.doc.song()); ++i) {
    if (i == midiT || !midiTrack(a.doc.song(), i) || !trackArmed(a, i) || a.doc.song().tracks[static_cast<size_t>(i)].locked) continue;
    if (!slotClips(a.doc.song(), i, mk).empty()) continue;
    ObjectId xc = kNoId;
    if (newMidiClipIn(a, i, mk, &xc)) {
      nameAfterTrack(a, xc, i);
      gRec.extras.push_back({i, xc});
    }
  }
}

void toggleSessionRecord(App& a) {
  if (active(a)) {
    if (gRec.on || gARec.on) stopSessionRecord(a);
    else a.later([&a] { startSessionRecord(a, -1, -1); });
    return;
  }
  // Outside Clips: the record button in pattern mode.
  if (sessionRecording(a)) {
    a.recordArmed = false;
    return;
  }
  a.later([&a] {
    a.engine.setMode(PlayMode::Pattern);
    a.recordArmed = true;
    if (!a.engine.playing()) a.startPlayback();
  });
}

bool sessionRecordAudio(const App& a) { return gARec.on && a.engine.playing(); }

bool audioTakePending(const App&) { return gARec.pending || gARec.on; }

bool audioTakesCommitted(App& a, const std::vector<ObjectId>& clips) {
  if (!gARec.pending && !gARec.on) return false;
  const int t = gARec.track, k = gARec.slot;
  gARec.on = gARec.pending = false;
  const Song& s = a.doc.song();
  const Tick st = sceneTicks(s), bar = std::max<Tick>(1, s.barTicks());
  // Each armed grid track's take goes into its own slot of the scene (when empty), whole bars up to a
  // scene; a take anywhere else (a new take track, a track out of the grid) is dropped.
  ObjectId mine = kNoId;
  a.doc.beginGroup("record clip");
  for (ObjectId id : clips) {
    const Clip* c = a.doc.song().clip(id);
    if (!c) continue;
    const int ct = c->track;
    bool taken = false;  // another clip (not this take, which may sit in the slot already) holds the slot
    for (const int i : slotClips(a.doc.song(), ct, k)) taken = taken || a.doc.song().playlist[static_cast<size_t>(i)].id != id;
    const MixerTrack* mx = ct >= 0 ? trackmodes::linkedMixer(a.doc.song(), ct) : nullptr;
    if (ct < 0 || ct >= gridColumns(a.doc.song()) || trackmodes::modeOf(a.doc.song(), ct) != TrackMode::Audio || taken || !mx || !mx->armed) {
      a.doc.removeClip(id);
      continue;
    }
    const Tick len = std::clamp<Tick>((a.doc.song().clipLength(*c) + bar - 1) / bar * bar, bar, st);
    const Rgb col = a.doc.song().tracks[static_cast<size_t>(ct)].color;
    a.doc.editClip(id, "record clip", [k, st, len](Clip& x) { x.start = static_cast<Tick>(k) * st, x.length = len, x.muted = false; });
    a.doc.editSong("record clip", [&](Song& x) {  // N47-14: the track's colour
      if (const Clip* cc = x.clip(id))
        if (Channel* ch = x.channel(cc->source)) ch->color = col;
    });
    if (ct == t) mine = id;
  }
  a.doc.endGroup();
  if (mine != kNoId) {
    if (!gRec.on) {  // (a MIDI part still recording keeps its own clip to launch)
      selectSlot(a, t, k);
      gRec.clip = mine, gRec.track = t;
      gRec.launchTries = 30;  // it plays (as the reference's recorded clip goes on looping)
    } else {
      launch(a, mine);
    }
    a.notify(i18n::tr("Recorded into the clip slot"));
  }
  squashRecording(a);
  return true;
}

bool trackArmed(const App& a, int t) {
  const Song& s = a.doc.song();
  if (t < 0 || t >= static_cast<int>(s.tracks.size())) return false;
  if (trackmodes::modeOf(s, t) == TrackMode::Audio) {
    const MixerTrack* m = trackmodes::linkedMixer(s, t);
    return m && m->armed;
  }
  return midiTrack(s, t) && (gMidiArmed.count(t) > 0 || t == selectedTrack(const_cast<App&>(a)));
}

bool midiArmedByHand(int t) { return gMidiArmed.count(t) > 0; }

bool instrumentDropped(App& a, ObjectId channel, const std::string& name) {
  if (!active(a)) return false;
  const Song& s = a.doc.song();
  int track = -1;
  for (int t = 0; t < static_cast<int>(s.tracks.size()) && track < 0; ++t) {
    const std::vector<ObjectId> ch = trackChannels(s, t);
    if (std::find(ch.begin(), ch.end(), channel) != ch.end()) track = t;
  }
  // N53-08: the instrument's own mixer track sends to every return (at zero), as every track of a set does.
  if (const Channel* c = s.channel(channel); track >= 0 && c && c->mixerTrack != s.masterId() && trackmodes::missingReturnSends(s, c->mixerTrack)) {
    const ObjectId mix = c->mixerTrack;
    a.doc.editSong("track sends", [mix](Song& x) { trackmodes::addReturnSendsTo(x, mix); });
  }
  if (track >= 0 && !name.empty()) {
    // N47-15: a track still named by its place (or not at all) takes the instrument's name after its
    // number ("1 Analog Synth"); a name the user gave stays. Its instrument and mixer track follow.
    // Checkpoint 54: the number Clips shows it by (lanes and gap tracks take none).
    const std::vector<int> nums = trackmodes::trackNumbers(s, track + 1);
    const int number = track < static_cast<int>(nums.size()) && nums[static_cast<size_t>(track)] > 0 ? nums[static_cast<size_t>(track)] : track + 1;
    const std::string was = s.tracks[static_cast<size_t>(track)].name;
    const std::string def1 = std::to_string(number) + " MIDI", def2 = std::to_string(number) + " Audio";
    // N52-11: a track named after the instrument it had ("1 Analog Synth") takes the new one's name too.
    bool instrumentNamed = false;
    if (const std::string num = std::to_string(number) + " "; was.rfind(num, 0) == 0)
      for (const PluginInfo& pi : pluginCatalog())
        if (pi.kind == PluginKind::Generator && was.compare(num.size(), std::string::npos, pi.name) == 0) instrumentNamed = true;
    if (was.empty() || was == def1 || was == def2 || was == name || instrumentNamed) {
      const std::string to = std::to_string(number) + " " + name;
      a.doc.editSong("name track", [track, to, channel, was, name](Song& x) {
        PlaylistTrack& tr = x.tracks[static_cast<size_t>(track)];
        tr.name = to;
        if (Channel* c = x.channel(channel)) {
          c->name = to;
          for (MixerTrack& m : x.mixer)
            if (m.id == c->mixerTrack && (m.name.empty() || m.name == was || m.name == name || m.name.rfind("Insert ", 0) == 0)) m.name = to;
        }
      });
    }
  }
  // N47-03: the device shows in the device band, as the reference's chain (refs 487, 462, 481); nothing
  // else in the window moves. An installed plugin still opens its own window.
  a.selectChannel(channel);
  if (track >= 0) {
    a.playlist.trackSel = {track};
    focusTrack(a, track);
  }
  layoutui::setRegionVisible(a, "live.device", true, false);
  if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) *f = true;
  if (const Channel* c = a.doc.song().channel(channel); c && isHostedType(c->plugin.type)) a.openChannelWindow(channel);
  return true;
}

const MixerTrack* chosenBus(const App& a) {
  const Song& s = a.doc.song();
  if (s.mixer.empty() || !a.playlist.trackSel.empty() || !a.playlist.selected.empty()) {
    return nullptr;
  }
  if (a.selMixer == s.mixer[0].id) {
    return a.showMaster ? &s.mixer[0] : nullptr;
  }
  for (size_t j : trackmodes::returnTracks(s)) {
    if (j < s.mixer.size() && s.mixer[j].id == a.selMixer) {
      return &s.mixer[j];
    }
  }
  return nullptr;
}

void selectReturn(App& a, ObjectId mixer) {
  a.selMixer = mixer;
  a.playlist.trackSel.clear();  // as Main (N50-10): the return is the chosen track now
  a.playlist.selected.clear();
  trackChosen();  // 0.7.8: its chain in the detail spot
}

void busEffectAdded(App& a, ObjectId mixer) {
  if (!active(a)) {
    return;
  }
  const Song& s = a.doc.song();
  if (!s.mixer.empty() && mixer == s.mixer[0].id) {
    selectMain(a);
  } else {
    selectReturn(a, mixer);
  }
  layoutui::setRegionVisible(a, "live.device", true, false);
  if (bool* f = layoutui::windowOpenFlag(a, "Device chain")) {
    *f = true;
    a.focusWindow = "Device chain";
  }
}

bool clipPlayhead(const App& a, ObjectId pattern, double& tick) {
  if (!active(a)) return false;
  const Song& s = a.doc.song();
  const Clip* best = nullptr;
  int bestTrack = -1;
  for (int t = 0; t < gridColumns(s); ++t) {
    const Clip* c = s.clip(a.engine.liveActive(t));
    if (!c || c->kind != ClipKind::Pattern || c->pattern != pattern) continue;
    if (!best || a.playlist.selected.count(c->id)) best = c, bestTrack = t;
  }
  const Pattern* p = s.pattern(pattern);
  if (!best || !p) return false;
  const double len = static_cast<double>(std::max<Tick>(1, s.clipLength(*best)));
  const double in = std::fmod(std::max(0.0, a.engine.position() - static_cast<double>(a.engine.liveOrigin(bestTrack))), len);
  double pt = static_cast<double>(best->offset) + in;
  const double plen = static_cast<double>(std::max<Tick>(1, s.patternContentLength(*p)));
  if (p->loop && pt >= plen) pt = std::fmod(pt, plen);
  tick = pt;
  return true;
}

void toggleMidiArm(App& a, int t, bool exclusive) {
  if (!midiTrack(a.doc.song(), t)) return;
  const bool on = !gMidiArmed.count(t);
  if (exclusive) gMidiArmed.clear();  // as the reference: one arm at a time unless Cmd is held
  if (on) gMidiArmed.insert(t);
  else gMidiArmed.erase(t);
}

namespace {
void captureFrame(App& a);
}
void sessionRecordFrame(App& a) {
  squashRecording(a);  // (a squash the audio commit's open edit group put off)
  captureFrame(a);     // N48-11
  {  // N50-07: stopping the transport ends Arrangement Record too, as the reference's (the next Play does not record)
    static bool wasPlaying = false;
    const bool playing = a.engine.playing();
    if (wasPlaying && !playing && a.recordArmed && !a.engine.countingIn()) a.recordArmed = false;
    wasPlaying = playing;
  }
  // N49-08: a MIDI take recorded into the Arrangement (F9) takes its track's colour, in the take's own
  // undo step, as a dropped or inserted clip does.
  if (a.recorder.active() && a.recorder.songMode()) {
    const Song& s0 = a.doc.song();
    const Clip* c = s0.clip(a.recorder.takeClip());
    const Pattern* p = c ? s0.pattern(c->pattern) : nullptr;
    if (p && c->track >= 0 && c->track < static_cast<int>(s0.tracks.size())) {
      const Rgb col = s0.tracks[static_cast<size_t>(c->track)].color;
      if (p->color != col) {
        const ObjectId pid = p->id;
        a.recorder.edit(a.doc, [&a, pid, col] { a.doc.editPattern(pid, "take colour", [col](Pattern& x) { x.color = col; }); });
      }
      // N50-06: the take is as long as the recording so far (it ends where recording stops, as the
      // reference's), not its notes' last bar; grown a sixteenth at a time, in the take's undo step.
      const Tick six = std::max<Tick>(1, s0.beatTicks() / 4);
      const Tick upTo = static_cast<Tick>(std::max({0.0, a.recordTick(), a.engine.position()})) - c->start;  // the later of the heard and the played clock
      // N51-10: the clip's own length (the pattern grows by bars under it), up to the sixteenth reached.
      const Tick len = std::max(six, (upTo + six - 1) / six * six);
      if (a.engine.playing() && !a.engine.countingIn() && upTo > 0 && (c->length <= 0 || len > c->length)) {
        const ObjectId cid = c->id;
        a.recorder.edit(a.doc, [&a, cid, len] { a.doc.editClip(cid, "take length", [len](Clip& x) { x.length = std::max(x.length, len); }); });
      }
    }
  }
  const Song& s = a.doc.song();
  if (gRec.launchTries > 0) {  // the new clip plays once the engine has it (the program is rebuilt after the edit)
    if (!s.clip(gRec.clip)) gRec.launchTries = 0;
    else if (a.engine.liveActive(gRec.track) == gRec.clip || a.engine.liveQueued(gRec.track) == gRec.clip) gRec.launchTries = 0;
    else if (--gRec.launchTries < 29) launch(a, gRec.clip);
    for (const auto& [xt, xc] : gRec.extras)  // N49-04: the other armed tracks' clips start with it
      if (a.doc.song().clip(xc) && a.engine.liveActive(xt) != xc && a.engine.liveQueued(xt) != xc) launch(a, xc);
  }
  if (gARec.on && (a.engine.playing() || a.engine.countingIn())) gARec.waitFrames = 0;
  else if (gARec.on && gARec.waitFrames > 0) --gARec.waitFrames;  // N49-02: the launch has not started it yet
  else if (gARec.on)  // the transport stopping ends the audio part (the takes then land)
    gARec.on = false, gARec.pending = true;
  if (!gRec.on) return;
  const Clip* c = s.clip(gRec.clip);
  const ObjectId act = a.engine.liveActive(gRec.track);
  if (!c) {
    gRec.on = false;  // the clip was deleted (or undone)
    return;
  }
  if (!gRec.started) {
    if (act == gRec.clip && a.engine.playing() && !a.engine.countingIn()) gRec.started = true;
    else if (a.engine.countingIn()) gRec.launchTries = std::max(gRec.launchTries, 2);  // N49-01: it records once the count-in is done
    else if (gRec.launchTries == 0 && a.engine.liveQueued(gRec.track) != gRec.clip) gRec.on = false;  // it never played
    return;
  }
  if (!a.engine.playing() || act != gRec.clip) {  // the transport stopped, or the track plays another clip or none
    stopSessionRecord(a);
    return;
  }
  const double at = recContentTick(a);
  const Tick bar = std::max<Tick>(1, s.barTicks());
  if (at < gRec.lastAt - static_cast<double>(bar / 2)) gRec.grow = false;  // it looped: its length stays
  gRec.lastAt = at;
  if (gRec.grow) {  // half a bar before its end, a new clip grows by a bar (up to a scene's length)
    const Tick len = s.clipLength(*c);
    const int bars = static_cast<int>(len / bar);
    if (bars >= kSceneBars) gRec.grow = false;
    else if (at > static_cast<double>(len - bar / 2)) recSetBars(a, bars + 1);
  }
}

// ------------------------------------------------ arrangement record ----
// N48-11: with Arrangement record on, what the clip grid plays is written into the arrangement when
// the transport stops (or the record button goes off): each track's launched clip, one copy per
// pass, from its launch to the next launch or stop, as the reference records session launches.
namespace {
struct CapSeg {
  int track = 0;
  ObjectId clip = kNoId;
  Tick origin = 0, from = 0, to = 0;  // clip-launch clock ticks: the clip's content tick 0, the stretch it played
  Tick off = 0;                       // arrangement tick = clock tick + off
};
// The clip-launch mode runs its own clock (it starts again at 0 when the first launch leaves the
// arrangement's playback), so the capture keeps the arrangement's time itself: it goes on from where
// the arrangement played, frame by frame, and each stretch notes how its clock maps onto it.
struct Capture {
  bool on = false;
  double arrNow = 0;           // the arrangement tick now
  double lastPos = 0;          // the transport last frame
  PlayMode lastMode = PlayMode::Song;
  double liveStart = -1;       // the clip-launch clock when the capture first saw it run
  std::map<int, CapSeg> open;  // by track
  std::vector<CapSeg> done;
} gCap;

Tick capDest(const CapSeg& sg, Tick t) { return t + sg.off; }

// Checkpoint 60 (N59-03): one take is one undo step, as the reference's: the edits made while
// Arrangement record ran (the knob moves' own values), the launches written at the stop, the
// automation recorded with them and the audio takes become one step once all of them are in.
uint64_t gCapPushes0 = 0;
bool gCapSquash = false;
int gCapSquashTries = 0;
void squashCapture(App& a) {
  if (!gCapSquash || gCap.on || a.doc.groupDepth() > 0 || a.audioRecSession || a.audioRecFinishing || automationTakesPending()) return;
  const uint64_t n = a.doc.history().pushCount() - gCapPushes0;
  if (n <= 1 || a.doc.squashSteps(static_cast<size_t>(n), "record into the arrangement") || ++gCapSquashTries > 10) {
    gCapSquash = false, gCapSquashTries = 0;
  }
}

void writeCapture(App& a) {
  std::vector<CapSeg> segs;
  for (const auto& sg : gCap.done)
    if (sg.to > sg.from) segs.push_back(sg);
  if (segs.empty()) return;
  int placed = 0;
  std::map<ObjectId, Clip> launched;  // the launched clips (grid clips: set aside during the edit)
  for (const CapSeg& sg : segs)
    if (const Clip* c = a.doc.song().clip(sg.clip)) launched.emplace(c->id, *c);
  a.doc.editSong("record clips into the arrangement", [&](Song& song) { withoutGridClips(a, song, [&](Song& s) {  // N53-02
    for (CapSeg sg : segs) {
      const auto src = launched.find(sg.clip);
      if (src == launched.end()) continue;
      const Clip& c = src->second;
      const Tick len = std::max<Tick>(1, s.clipLength(c));
      if (c.launchOnce) sg.to = std::min(sg.to, sg.origin + len);  // N53-02: a one-shot plays (and is written) once
      const Tick d0 = capDest(sg, sg.from), d1 = capDest(sg, sg.to);
      if (d1 <= d0) continue;
      const std::set<int> tr{sg.track};
      // The destination is cleared (clips across its edges are cut), then filled pass by pass.
      const std::set<ObjectId> under = chop::isolateRange(s, d0, d1, tr);
      s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& x) { return under.count(x.id) > 0; }),
                       s.playlist.end());
      std::set<ObjectId> before;
      for (const auto& x : s.playlist)
        if (x.track == sg.track) before.insert(x.id);
      Tick o = sg.origin;
      if (o + len <= sg.from) o += (sg.from - o) / len * len;
      // Checkpoint 59 (N58-13): a launch that played several passes is one looping clip (K-114's loop),
      // as the reference records a launch: not one clip per pass. A single pass stays a plain copy.
      const Tick passes = (sg.to - o + len - 1) / len;
      if (passes > 1 && !c.launchOnce) {
        Clip n = c;
        n.id = s.allocateId();
        n.start = capDest(sg, o);
        n.length = len;
        n.launched = false, n.launchOnce = false, n.launchSet = ClipLaunch{};
        n.takeGroup = kNoId, n.take = 0;
        n.group = kNoId;
        TempoMap tm;
        tm.build(s);
        n = withClipLoop(s, tm, n, true);
        n.length = passes * len;
        s.playlist.push_back(n);
        o = sg.to;  // (no passes left to copy)
      }
      for (int guard = 0; o < sg.to && guard < 4096; o += len, ++guard) {
        if (o + len <= sg.from) continue;
        Clip n = c;
        n.id = s.allocateId();
        n.start = capDest(sg, o);
        n.launched = false, n.launchOnce = false, n.launchSet = ClipLaunch{};  // (an arrangement clip plays where it lies)
        n.takeGroup = kNoId, n.take = 0;
        n.group = kNoId;
        s.playlist.push_back(n);
      }
      // Only the part played stays: the passes are cut at the stretch's edges, the rest goes.
      const std::set<ObjectId> inside = chop::isolateRange(s, d0, d1, tr);
      s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(),
                                      [&](const Clip& x) { return x.track == sg.track && !before.count(x.id) && !inside.count(x.id); }),
                       s.playlist.end());
      placed += static_cast<int>(inside.size());
    }
  }); });
  const int launches = static_cast<int>(segs.size());
  std::printf("arrangement record: %d %s from %d %s\n", placed, placed == 1 ? "clip" : "clips", launches, launches == 1 ? "launch" : "launches");
  std::fflush(stdout);
  // Checkpoint 60 (N59-10): "1 clip", the i18n way (both forms translatable).
  if (placed > 0) a.notify(i18n::plural(placed, "%d clip recorded into the arrangement", "%d clips recorded into the arrangement"));
}

void captureFrame(App& a) {
  const bool want = a.recordArmed && a.engine.playing() && !a.engine.countingIn();
  const int nTracks = std::min<int>(static_cast<int>(a.doc.song().tracks.size()), kMaxPlaylistTracks);
  const Tick o = arrangementOrigin(a);
  static bool wasPlaying = false;  // the transport ran last frame
  const bool startedNow = !wasPlaying && a.engine.playing();
  wasPlaying = a.engine.playing();
  squashCapture(a);  // N59-03 (the take before)
  if (want && !gCap.on) {
    gCap = Capture{};
    gCap.on = true;
    gCapPushes0 = a.doc.history().pushCount(), gCapSquash = false;  // N59-03
    gCap.lastPos = a.engine.position();
    gCap.lastMode = a.engine.mode();
    // From the arrangement's play position (a capture begun while the grid plays: the arrangement's start).
    gCap.arrNow = gCap.lastMode == PlayMode::Live ? static_cast<double>(o) : std::max(gCap.lastPos, static_cast<double>(o));
    // N53-02: a transport started with the capture began its clip-launch clock at 0, a frame ago: the
    // launches it plays are recorded from their start (not from where the first frame saw the clock).
    if (startedNow && gCap.lastMode == PlayMode::Live) gCap.liveStart = 0;
  }
  if (!gCap.on) return;
  if (want) {
    const double pos = a.engine.position();
    const PlayMode mode = a.engine.mode();
    const bool sameClock = mode == gCap.lastMode && pos >= gCap.lastPos;
    // A clock that changed this frame: what ends ends where the old clock stood.
    const double endPos = sameClock ? pos : gCap.lastPos;
    if (sameClock) gCap.arrNow += pos - gCap.lastPos;
    const PlayMode wasMode = gCap.lastMode;
    const double prevPos = gCap.lastPos;  // N59-03: where the clock stood last frame
    gCap.lastPos = pos, gCap.lastMode = mode;
    // N53-02: the clip-launch clock starting (again) during the capture starts at 0: what it plays is
    // recorded from its start. A capture begun while it already ran records from that moment on.
    // grid-1118: under the arrangement (song mode) the launches run on the song position itself.
    if (gCap.liveStart < 0) gCap.liveStart = mode == PlayMode::Live && !sameClock && wasMode != PlayMode::Live ? 0.0 : pos;
    const Tick beat = std::max<Tick>(1, a.doc.song().beatTicks());
    for (int t = 0; t < nTracks; ++t) {
      const ObjectId act = mode == PlayMode::Live || mode == PlayMode::Song ? a.engine.liveActive(t) : kNoId;  // grid-1118
      const Tick org = a.engine.liveOrigin(t);
      auto it = gCap.open.find(t);
      // Checkpoint 59 (N58-13): a clip that loops on starts its next pass a whole number of its
      // lengths after its launch: the same launch, one stretch (written as one looping clip).
      bool nextPass = false;
      if (it != gCap.open.end() && act == it->second.clip && org > it->second.origin && sameClock) {
        if (const Clip* lc = a.doc.song().clip(act)) {
          const Tick len = std::max<Tick>(1, a.doc.song().clipLength(*lc));
          nextPass = (org - it->second.origin) % len == 0;
        }
      }
      if (it != gCap.open.end() && !nextPass && (act != it->second.clip || org != it->second.origin || !sameClock)) {
        Tick stopAt = static_cast<Tick>(endPos);
        if (act == kNoId && sameClock && t < static_cast<int>(a.doc.song().tracks.size())) {
          // Checkpoint 60 (N59-03): a clip stopped (by a scene, or its track's stop) stopped on its launch
          // grid, between last frame and this one: it ends there, not where this frame saw the clock
          // (a tick or more later: 385 ticks for a one-bar stretch).
          const Song& song = a.doc.song();
          const Clip* lc = song.clip(it->second.clip);
          const Tick g = launchGridTicks(song, song.tracks[static_cast<size_t>(t)].launch, lc ? song.clipLength(*lc) : 0);
          if (g > 0) {
            const Tick snap = static_cast<Tick>(std::floor(endPos / static_cast<double>(g))) * g;
            if (static_cast<double>(snap) >= std::floor(prevPos)) stopAt = snap;
          }
        }
        it->second.to = act != kNoId && sameClock ? std::max(it->second.from, org) : std::max(it->second.from, stopAt);
        gCap.done.push_back(it->second);
        gCap.open.erase(it);
      }
      if (act != kNoId && !gCap.open.count(t)) {
        CapSeg sg;
        sg.track = t, sg.clip = act, sg.origin = org;
        // This clock's tick 0 in the arrangement, on the beat grid (the clock restarts on a launch).
        const double off = gCap.arrNow - pos;
        sg.off = static_cast<Tick>(std::llround(off / static_cast<double>(beat))) * beat;
        // N53-02 (checkpoint 54): a clip already playing when the capture began is written from the
        // arrangement tick the capture began at (not from the first frame's clock, a few ticks late).
        const Tick here = static_cast<Tick>(std::llround(gCap.arrNow)) - sg.off;
        sg.from = std::max(org, std::min(static_cast<Tick>(gCap.liveStart), here));
        // Checkpoint 59: the rounding to the beat never puts the take before the Arrangement's start
        // (into the clip grid's storage): a beat later then.
        for (int guard = 0; capDest(sg, sg.from) < o && guard < 64; ++guard) sg.off += beat;
        gCap.open[t] = sg;
      }
    }
    return;
  }
  for (auto& [t, sg] : gCap.open) {  // the transport stopped (or recording went off): the open stretches end here
    sg.to = std::max(sg.from, static_cast<Tick>(gCap.lastPos));
    gCap.done.push_back(sg);
  }
  gCap.open.clear();
  gCap.on = false;
  writeCapture(a);
  gCap.done.clear();
  gCapSquash = true, gCapSquashTries = 0;  // N59-03: once the automation and audio takes are in
}
}  // namespace

bool arrangementRecordTick(const App& a, double& tick) {
  if (!active(a) || !a.recordArmed || !a.engine.playing() || a.engine.countingIn()) return false;
  const double pos = a.engine.position();
  const double o = static_cast<double>(arrangementOrigin(a));
  if (!gCap.on) {  // (the capture starts on Clips' next frame: as it will map this moment)
    tick = a.engine.mode() == PlayMode::Live ? o : std::max(pos, o);
    return true;
  }
  if (a.engine.mode() != gCap.lastMode || pos < gCap.lastPos) {  // the clock changed this frame
    tick = gCap.arrNow;
    return true;
  }
  // As the capture maps a stretch of the clip-launch clock (on the beat grid), so the moves line up
  // with the clips recorded with them.
  const double beat = static_cast<double>(std::max<Tick>(1, a.doc.song().beatTicks()));
  const double off = a.engine.mode() == PlayMode::Live ? std::round((gCap.arrNow - gCap.lastPos) / beat) * beat : gCap.arrNow - gCap.lastPos;
  tick = std::max(o, pos + off);
  return true;
}

bool arrangementCapturing(const App& a) { return active(a) && gCap.on; }

const char* gridName(LaunchGrid g) {
  // grid-1118: and whole bars (the 4 beats grid reads as 1 Bar, as before: the default in 4/4)
  static const char* const kNames[] = {i18n::N_("None"),   "1/16", "1/8", "1/4", "1/2", i18n::N_("1 Bar"), i18n::N_("Clip"), i18n::N_("1 Bar"),
                                       i18n::N_("2 Bars"), i18n::N_("4 Bars"), i18n::N_("8 Bars"), "1/32", "1/16T", "1/8T", "1/4T", "1/2T"};
  static_assert(sizeof kNames / sizeof kNames[0] == static_cast<size_t>(LaunchGrid::Count), "a name per launch grid");
  const int i = static_cast<int>(g);
  return i >= 0 && i < static_cast<int>(LaunchGrid::Count) ? i18n::tr(kNames[i]) : "?";
}

bool gridItems(LaunchGrid cur, LaunchGrid& picked, bool combo) {
  // grid-1118: Clips' launch grids, the coarsest first (the 4 beats grid is ticked as 1 Bar; picking 1 Bar
  // sets the bar grid, which follows the meter). `combo`: Selectables (a combo), else MenuItems.
  // grid2-1118: and 1/32 and the triplet grids (1/2T ... 1/16T).
  static const LaunchGrid kOrder[] = {LaunchGrid::Now,     LaunchGrid::EightBars, LaunchGrid::FourBars, LaunchGrid::TwoBars,    LaunchGrid::Bar,
                                      LaunchGrid::TwoBeats, LaunchGrid::Beat,     LaunchGrid::HalfBeat, LaunchGrid::Step,       LaunchGrid::ThirtySecond,
                                      LaunchGrid::HalfT,    LaunchGrid::QuarterT, LaunchGrid::EighthT,  LaunchGrid::SixteenthT};
  bool got = false;
  for (LaunchGrid g : kOrder) {
    const bool on = cur == g || (g == LaunchGrid::Bar && cur == LaunchGrid::FourBeats);
    if (combo ? ImGui::Selectable(gridName(g), on) : ImGui::MenuItem(gridName(g), nullptr, on)) {
      picked = g;
      got = true;
    }
  }
  if (cur == LaunchGrid::ClipLength) {  // set in another layout: shown, and kept until another is picked
    if (combo) {
      ImGui::Selectable(gridName(cur), true);
    } else {
      ImGui::MenuItem(gridName(cur), nullptr, true);
    }
  }
  return got;
}

void selectMain(App& a) {
  // N49-09: Main is selected and its devices show: the Master chain window (820 x 300, centred, in Clips).
  const Song& s = a.doc.song();
  if (s.mixer.empty()) return;
  a.selMixer = s.mixer[0].id;
  a.showMaster = true;
  a.playlist.trackSel.clear();  // N50-10: Main is the selected track now (no track title stays outlined)
  a.playlist.selected.clear();
  a.focusWindow = "Master chain";
  trackChosen();  // 0.7.8: the detail spot shows the device view (Main's chain docks there)
}

void toggleArrangementRecord(App& a) {
  a.recordArmed = !a.recordArmed;
  // N48-11: in Clips, Arrangement record starts the transport (from the arrangement's play position).
  if (a.recordArmed && active(a)) {
    // N49-03: the armed MIDI track (the selected one first) takes the notes: it gets its note-only
    // channel when it has no instrument, and its channel is what the keys play, so the take lands on it.
    const Song& s = a.doc.song();
    int mt = selectedTrack(a);
    if (!midiTrack(s, mt) || !trackArmed(a, mt)) {
      mt = -1;
      for (int i = 0; i < gridColumns(s) && mt < 0; ++i)
        if (midiTrack(s, i) && trackArmed(a, i)) mt = i;
    }
    if (mt >= 0) {
      ensureNoteChannel(a, mt);
      if (const Channel* ch = trackmodes::linkedChannel(a.doc.song(), mt)) a.selectChannel(ch->id);
      else if (const std::vector<ObjectId> chs = trackChannels(a.doc.song(), mt); !chs.empty()) a.selectChannel(chs.front());
    }
  }
  if (a.recordArmed && active(a) && !a.engine.playing()) {
    if (a.engine.mode() == PlayMode::Pattern) a.engine.setMode(PlayMode::Song);
    const Tick o = arrangementOrigin(a);
    if (a.engine.mode() == PlayMode::Song && a.engine.position() < static_cast<double>(o)) a.engine.setPosition(o);
    (void)menuparity::playPressed(a);
  }
}

bool drawMode(const App& a) { return (a.editFocus == App::EditFocus::Piano ? a.piano.tool : a.playlist.tool) == Tool::Draw; }

void setDrawMode(App& a, bool on) { a.piano.tool = a.playlist.tool = on ? Tool::Draw : Tool::Select; }

// ------------------------------------------------------------ main view ----

bool gridView(const App& a) { return a.showLivePads && !a.showPlaylist; }

void showView(App& a, bool grid) {
  a.showLivePads = grid;
  a.showPlaylist = !grid;
  a.focusWindow = grid ? "Live pads" : "Playlist";
}

namespace {
void selectClipChannel(App& a, const Clip& c);  // (with the clip grid, below)
// C47 (C44-23): the track the device band follows when the track has no channel of its own to select
// (an audio track: its mixer track's inserts; an empty MIDI track: nothing yet). Valid while the
// selected channel is still the one there was when the track was chosen.
struct TrackFocus {
  int t = -1;
  ObjectId ch = kNoId;
};
TrackFocus gFocus;
// The channels a track plays, in order: an instrument track's own, else those its pattern clips play
// (an audio track's are its clips' sources, which are not shown as its devices: none).
std::vector<ObjectId> trackChannels(const Song& s, int t) {
  if (const Channel* c = trackmodes::linkedChannel(s, t)) return {c->id};
  std::vector<ObjectId> v;
  if (trackmodes::modeOf(s, t) == TrackMode::Audio) return v;
  for (const Clip& c : s.playlist) {
    if (c.track != t || c.kind != ClipKind::Pattern) continue;
    const Pattern* p = s.pattern(c.pattern);
    if (!p) continue;
    auto take = [&](ObjectId id) {
      if (s.channel(id) && std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
    };
    for (const auto& [ch, n] : p->notes)
      if (!n.empty()) take(ch);
    for (const auto& [ch, st] : p->steps)
      if (std::any_of(st.begin(), st.end(), [](uint8_t x) { return x != 0; })) take(ch);
  }
  return v;
}
// A track chosen (its title, its header or one of its slots): the device band shows its devices.
// Checkpoint 52 (N51-11): a group folded with its hidden track selected selects the group (the device
// band then shows the group's chain, not a track out of sight).
void foldedSelection(App& a, int g, bool grid) {
  const Song& s = a.doc.song();
  if (g < 0 || g >= static_cast<int>(s.tracks.size())) return;
  if (!(grid ? s.tracks[static_cast<size_t>(g)].gridFolded : s.tracks[static_cast<size_t>(g)].folded)) return;
  const int gs = s.tracks[static_cast<size_t>(g)].groupSize;
  const int cur = selectedTrack(a);
  bool hidden = cur > g && cur <= g + gs;
  for (int t : a.playlist.trackSel) hidden = hidden || (t > g && t <= g + gs);
  if (!hidden) return;
  a.playlist.trackSel = {g};
  clearGridSel();
  focusTrack(a, g);
}
void focusTrack(App& a, int t) {
  const std::vector<ObjectId> chans = trackChannels(a.doc.song(), t);
  if (!chans.empty()) {
    if (std::find(chans.begin(), chans.end(), a.selChannel) == chans.end()) a.selectChannel(chans.front());
    gFocus = {};
    return;
  }
  gFocus = {t, a.selChannel};
}
constexpr size_t kArrangementSetsMax = 40;
// N45-10: the view an opened set asks for, held over the first frames (a launch with a file builds the
// layout after the load, and that opens the shell's default windows again).
int gOpenView = -1, gOpenFrames = 0;
// N47-17: a set opened or made new (and the first one at launch) starts with its first track selected,
// as the reference always has a track selected: the device band then shows that track.
bool gPickFirst = true;
bool gOfferOneShots = false;  // checkpoint 55 (N54-01): the opened set's looping short samples are looked for
// Checkpoint 55 (N54-01): a set saved before checkpoint 54 keeps its short unwarped grid samples looping
// (they retrigger at their own length). Nothing is changed unasked: a card offers to make them play
// once, in one undo step; the save stays as it was unless the offer is taken. Offered once per set.
// Checkpoint 56 (N55-07): "Keep looping" is remembered per Set, outside the song (clips.oneShotAnswers, the
// Sets' files, newest last); a Set where a sample already plays once was saved after the switch came (the
// person chose then), so its loops are taken as meant; the card names the samples.
bool oneShotsAnswered(const App& a, const std::string& file) {
  if (file.empty()) {
    return false;
  }
  const std::string all = a.settings.getString("clips.oneShotAnswers");
  size_t at = 0;
  while (at < all.size()) {
    size_t nl = all.find('\n', at);
    if (nl == std::string::npos) {
      nl = all.size();
    }
    if (all.compare(at, nl - at, file) == 0) {
      return true;
    }
    at = nl + 1;
  }
  return false;
}
void rememberOneShotsAnswer(App& a, const std::string& file) {
  if (file.empty() || oneShotsAnswered(a, file)) {
    return;
  }
  std::vector<std::string> v;
  const std::string all = a.settings.getString("clips.oneShotAnswers");
  size_t at = 0;
  while (at < all.size()) {
    size_t nl = all.find('\n', at);
    if (nl == std::string::npos) {
      nl = all.size();
    }
    if (nl > at) {
      v.push_back(all.substr(at, nl - at));
    }
    at = nl + 1;
  }
  v.push_back(file);
  if (v.size() > 200) {
    v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() - 200));
  }
  std::string out;
  for (const std::string& l : v) {
    out += (out.empty() ? "" : "\n") + l;
  }
  a.settings.set("clips.oneShotAnswers", out);
}
}  // namespace

std::string oneShotsCardText(const Song& s, const std::vector<ObjectId>& ids) {
  std::vector<std::string> names;
  for (const Clip& c : s.playlist) {
    if (std::find(ids.begin(), ids.end(), c.id) == ids.end()) {
      continue;
    }
    std::string n = c.name;
    if (n.empty()) {
      if (const Channel* ch = s.channel(c.source)) {
        n = ch->name;
      }
    }
    if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) {
      names.push_back(n);
    }
  }
  std::string list;
  const size_t shown = std::min<size_t>(names.size(), 3);
  for (size_t k = 0; k < shown; ++k) {
    list += (k == 0 ? "" : k + 1 == shown && names.size() <= 3 ? " and " : ", ") + names[k];
  }
  if (names.size() > 3) {
    list += " and " + std::to_string(names.size() - 3) + " more";
  }
  const std::string n = std::to_string(ids.size());
  std::string text = ids.size() == 1 ? "A short sample loops" : n + " short samples loop";
  if (!list.empty()) {
    text += " (" + list + ")";
  }
  return text + (ids.size() == 1 ? "; play it once?" : "; play them once?");
}

bool offerOneShotsWanted(const App& a) {
  const Song& s = a.doc.song();
  if (trackmodes::loopingShortSlotClips(s).empty() || oneShotsAnswered(a, a.projectFile())) {
    return false;
  }
  for (const Clip& c : s.playlist) {  // saved after the switch came: the loops are the person's choice
    if (c.kind == ClipKind::Audio && c.launchOnce && c.start < s.live.areaEnd) {
      return false;
    }
  }
  return true;
}

namespace {
void offerOneShots(App& a) {
  static std::set<std::string> offered;
  const std::vector<ObjectId> ids = trackmodes::loopingShortSlotClips(a.doc.song());
  if (ids.empty() || !offerOneShotsWanted(a)) {
    return;
  }
  const std::string file = a.projectFile();
  const std::string key = file.empty() ? std::to_string(ids.front()) : file;
  if (!offered.insert(key).second) {
    return;
  }
  a.notifyAction(oneShotsCardText(a.doc.song(), ids),
                 {{"Play once", [&a, ids] {
                     int done = 0;
                     a.doc.editSong("play short samples once", [&](Song& s) {
                       for (Clip& c : s.playlist) {
                         if (std::find(ids.begin(), ids.end(), c.id) != ids.end() && !c.launchOnce) {
                           c.launchOnce = true;
                           ++done;
                         }
                       }
                     });
                     a.notify(std::to_string(done) + (done == 1 ? " sample now plays once" : " samples now play once"));
                   }},
                  {"Keep looping", [&a, file] { rememberOneShotsAnswer(a, file); }}},
                 20.0);
}
int gRenameFrames = 0;  // N47-12: frames until the clip band's Name field takes the keyboard
// 0.7.8: the detail view (the clip view or the device view in one spot). gDetailWant: a clip (1) or a
// track (2) chosen by a click, applied once the button is up and no menu is open (the click that
// selects also starts a drag, and the swap rebuilds the dock); gDetailFocus: the keys go with it.
// gLastClip: the view shown last (the spot brought back shows it). gOpenDetail: an opened Set's view.
int gDetailWant = 0;
bool gDetailFocus = false, gLastClip = false;
int gOpenDetail = 0, gOpenDetailFrames = 0;
int gDetailFocusFrames = 0;  // frames left for the shown view's window to take the keys
int gDetailKeysFrame = -100;  // the frame they were handed over (the grid does not take them back just after)
std::vector<std::string> setList(const App& a, const char* key) {
  std::vector<std::string> v;
  const std::string all = a.settings.getString(key);
  size_t p = 0;
  while (p < all.size()) {
    const size_t e = all.find('\n', p);
    const std::string line = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
    if (!line.empty()) v.push_back(line);
    if (e == std::string::npos) break;
    p = e + 1;
  }
  return v;
}
std::vector<std::string> arrangementSets(const App& a) { return setList(a, "clips.arrangementSets"); }
// 0.7.8: the Sets last saved with the clip view / the device view in the detail spot (newest first); a
// Set in neither opens with the spot as the layout last had it.
constexpr const char* kClipViewSets = "clips.clipViewSets";
constexpr const char* kDeviceViewSets = "clips.deviceViewSets";
void putSetList(App& a, const char* key, std::vector<std::string> v, const std::string& file, bool in) {
  v.erase(std::remove(v.begin(), v.end(), file), v.end());
  if (in) v.insert(v.begin(), file);
  if (v.size() > kArrangementSetsMax) v.resize(kArrangementSetsMax);
  std::string all;
  for (const std::string& f : v) all += (all.empty() ? "" : "\n") + f;
  a.settings.set(key, all);
}
}  // namespace

bool detailShown(const App& a) {
  const layout::Shell& sh = layout::shell(layout::Preset::Live);
  const layout::State st = layoutui::regionState(a, layout::Preset::Live);
  return layout::visible(sh, st, "live.clip") || layout::visible(sh, st, "live.device");
}
bool clipViewShown(const App& a) {
  return layout::visible(layout::shell(layout::Preset::Live), layoutui::regionState(a, layout::Preset::Live), "live.clip");
}
void showDetailView(App& a, bool clip, bool focus) {
  layoutui::setRegionVisible(a, clip ? "live.clip" : "live.device", true);  // (the other one goes: one spot)
  layoutui::setRegionVisible(a, clip ? "live.device" : "live.clip", false);  // (also when this one was shown already: a 0.7.7 stack)
  // A view with nothing open in it opens its main part: the notes editor, the device chain.
  if (bool* f = layoutui::windowOpenFlag(a, clip ? "Piano roll" : "Device chain")) *f = true;
  gLastClip = clip;
  if (focus) gDetailFocusFrames = 30;  // (the region is built on the next frames: viewFrame hands the keys over)
}
void swapDetailView(App& a) {
  const bool clip = detailShown(a) ? clipViewShown(a) : gLastClip;
  showDetailView(a, !clip, true);
}
void trackChosen() { gDetailWant = 2, gDetailFocus = false; }
void clipChosen(bool focus) { gDetailWant = 1, gDetailFocus = gDetailFocus || focus; }

void projectOpened(App& a, const std::string& file) {
  // On a launch with a file the shell is not built yet: the view waits for Clips' first frames
  // (viewFrame runs only in Clips, so another layout never sees it).
  const bool now = layoutui::active() == layout::Preset::Live;
  const bool clips = now || a.settings.getString("ui.layout") == "live";
  const std::vector<std::string> v = arrangementSets(a);
  bool grid = file.empty() || std::find(v.begin(), v.end(), file) == v.end();
  // Checkpoint 59: a 0.7.7 development save kept the view in the project (N48-07): it is read once,
  // carried over to this person's list, and dropped from the song (it is never written again).
  const std::string pv = a.doc.song().live.view;
  if (!file.empty() && !pv.empty()) {
    grid = pv == "grid";
    std::vector<std::string> sets = v;
    sets.erase(std::remove(sets.begin(), sets.end(), file), sets.end());
    if (!grid) sets.insert(sets.begin(), file);
    if (sets.size() > kArrangementSetsMax) sets.resize(kArrangementSetsMax);
    std::string all;
    for (const std::string& f : sets) all += (all.empty() ? "" : "\n") + f;
    a.settings.set("clips.arrangementSets", all);
  }
  if (!pv.empty()) a.doc.patchSong([](Song& x) { x.live.view.clear(); });
  if (!clips) {  // K-314 (A-07): another layout; the view above is carried over all the same
    return;
  }
  if (now) showView(a, grid);
  gOpenView = grid ? 1 : 0;
  gOpenFrames = 4;
  {  // 0.7.8: and the detail spot shows the view it was saved with here (a Set never saved here: as it was)
    const std::vector<std::string> cv = setList(a, kClipViewSets), dv = setList(a, kDeviceViewSets);
    gOpenDetail = file.empty() ? 0 : std::find(cv.begin(), cv.end(), file) != cv.end() ? 1 : std::find(dv.begin(), dv.end(), file) != dv.end() ? 2 : 0;
    gOpenDetailFrames = 4;
  }
  gPickFirst = true;
  gFocus = {};  // N52-12: a track chosen in the set before is no choice in this one
  gOfferOneShots = true;
}

void setOpenedNew() { gPickFirst = true; }

void renameClip(App& a, ObjectId clip) {
  const Clip* c = a.doc.song().clip(clip);
  if (!c) return;
  a.playlist.selected = {clip};
  if (c->kind == ClipKind::Pattern) a.selectPattern(c->pattern);
  layoutui::setRegionVisible(a, "live.clip", true, false);
  if (bool* f = layoutui::windowOpenFlag(a, "Piano roll")) *f = true;
  gRenameFrames = 3;  // after the menu closes and the band is built
}

bool renameFocusNow() { return gRenameFrames > 0 && --gRenameFrames == 0; }

int gRenTrack = -1, gRenTrackFrames = 0, gRenTrackIdle = 0;  // checkpoint 49: the track whose grid title is a name field
void renameTrack(App& a, int track) {
  const Song& s = a.doc.song();
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return;
  a.playlist.trackSel = {track};
  if (gridView(a)) {
    gRenTrack = track, gRenTrackFrames = 2, gRenTrackIdle = 0;
    return;
  }
  menus::openPrompt("Rename track", playlistTrackName(s, static_cast<size_t>(track)), [&a, track](const std::string& n) {
    if (track < static_cast<int>(a.doc.song().tracks.size()) && !n.empty())
      trackmodes::renameTrack(a.doc, track, n);  // N52-06: its mixer track follows
  });
}

void projectSaved(App& a, const std::string& file) {
  if (file.empty()) {
    return;
  }
  std::vector<std::string> v = arrangementSets(a);
  const std::vector<std::string> cv = setList(a, kClipViewSets), dv = setList(a, kDeviceViewSets);
  bool arrangement = false, clipView = false, known = true;
  if (active(a)) {
    arrangement = !gridView(a);
    clipView = clipViewShown(a);  // 0.7.8: the detail spot's view (hidden: the one shown last)
    if (!detailShown(a)) clipView = gLastClip;
  } else {  // K-314 (A-07): outside Clips its view is not changed: a Save As keeps the one the Set had
    const std::string was = a.projectFile();
    if (was.empty() || was == file) {
      return;
    }
    arrangement = std::find(v.begin(), v.end(), was) != v.end();
    clipView = std::find(cv.begin(), cv.end(), was) != cv.end();
    known = clipView || std::find(dv.begin(), dv.end(), was) != dv.end();
  }
  putSetList(a, kClipViewSets, cv, file, known && clipView);
  putSetList(a, kDeviceViewSets, dv, file, known && !clipView);
  v.erase(std::remove(v.begin(), v.end(), file), v.end());
  if (arrangement) {
    v.insert(v.begin(), file);
  }
  if (v.size() > kArrangementSetsMax) v.resize(kArrangementSetsMax);
  std::string all;
  for (const std::string& f : v) all += (all.empty() ? "" : "\n") + f;
  a.settings.set("clips.arrangementSets", all);
}

namespace {
// N53-05 (checkpoint 54): an instrument a collaborator added in another layout (Studio's rack) with no
// track to play it from gets a track here, as every instrument in Clips is a track, so the Clips user sees
// and plays it. K-314 B-03: a track shown on this computer only (PlaylistTrack::shownHere): no undo step
// of this person's, nothing written into the collaborator's channel, no track or mixer track for anyone
// else; the collaborator's own Undo still takes the channel away. It becomes the project's once this
// person uses it (clips on it, an edit of it: collab/Session.cpp), and comes back after the shared tracks
// when a collaborator's track takes its place. An opened project, undo and this computer's own edits are
// left as they are.
// Checkpoint 55 (N54-09): an instrument this computer added by a path that made no track for it (a
// script, the collaboration's own "edit" command) gets a real track: one new edit step, not an undo or
// redo. Both are named as Clips names an instrument's ("5 SamCh": its number, then the instrument's name;
// the instrument keeps its own name).
bool playsSomewhere(const Song& s, ObjectId id) {
  if (trackmodes::instrumentTrackOf(s, id) >= 0) {
    return true;
  }
  const int used = trackmodes::usedTrackCount(s);
  for (int t = 0; t < used; ++t) {
    for (ObjectId c : trackChannels(s, t)) {
      if (c == id) {
        return true;
      }
    }
  }
  for (const Clip& c : s.playlist) {
    if (c.kind == ClipKind::Audio && c.source == id) {
      return true;  // (an audio clip's source plays from its clip's track)
    }
  }
  return false;
}

void showHere(App& a, ObjectId id) {
  const Song& x = a.doc.song();
  const Channel* ch = x.channel(id);
  if (!ch || playsSomewhere(x, id)) {
    return;
  }
  const int t = trackmodes::freeTrack(x, trackmodes::usedTrackCount(x));  // after the others
  if (t < 0 || t >= kMaxPlaylistTracks) {
    return;
  }
  a.doc.patchSong([&](Song& y) {
    if (t >= static_cast<int>(y.tracks.size())) {
      y.tracks.resize(static_cast<size_t>(t) + 1);
    }
    const Channel* c = y.channel(id);
    PlaylistTrack& tr = y.tracks[static_cast<size_t>(t)];
    tr = PlaylistTrack{};
    tr.mode = TrackMode::Instrument;
    tr.channel = id;
    tr.color = c ? c->color : tr.color;
    tr.shownHere = true;
    tr.name = std::to_string(trackmodes::shownNumber(y, t)) + " " + (c ? c->name : std::string());
  });
}

void adoptPeerChannels(App& a) {
  static std::set<ObjectId> known, shown;  // shown: collaborators' instruments given a track here only
  static size_t knownPos = 0;
  static uint64_t knownRev = 0, knownMove = 0;
  static bool first = true;
  const Song& s = a.doc.song();
  const size_t pos = a.doc.history().position();
  const uint64_t rev = a.doc.revision(), move = a.doc.moveSerial();
  if (rev == knownRev && !first) {
    return;
  }
  std::set<ObjectId> now;
  for (const Channel& c : s.channels) {
    now.insert(c.id);
  }
  std::vector<ObjectId> fresh;
  const bool peer = pos == knownPos && collabui::active(a);
  const bool own = pos == knownPos + 1 && move == knownMove;  // one new step of this computer's (no undo or redo)
  if (!first && (peer || own)) {
    for (ObjectId id : now) {
      if (!known.count(id)) {
        fresh.push_back(id);
      }
    }
  }
  first = false;
  known = std::move(now), knownPos = pos, knownRev = rev, knownMove = move;
  // K-314 B-03: a track shown here that a collaborator's track displaced (or that became the project's)
  std::vector<ObjectId> again;
  for (auto it = shown.begin(); it != shown.end();) {
    const ObjectId id = *it;
    bool here = false, real = false;
    for (const PlaylistTrack& t : s.tracks) {
      if (t.mode == TrackMode::Instrument && t.channel == id) {
        (t.shownHere ? here : real) = true;
      }
    }
    if (!s.channel(id) || real || !collabui::active(a)) {
      it = shown.erase(it);
      continue;
    }
    if (!here && !playsSomewhere(s, id)) {
      again.push_back(id);
    }
    ++it;
  }
  for (ObjectId id : again) {
    a.later([&a, id] {
      if (active(a) && collabui::active(a)) {
        showHere(a, id);
      }
    });
  }
  if (fresh.empty()) {
    return;
  }
  for (ObjectId id : fresh) {
    const Channel* c = s.channel(id);
    const PluginInfo* pi = c ? findPlugin(c->plugin.type) : nullptr;
    if (!c || playsSomewhere(s, id) || !pi || pi->kind != PluginKind::Generator || c->isLayer()) {
      continue;
    }
    if (peer) {
      shown.insert(id);
      a.later([&a, id] {
        if (active(a)) {
          showHere(a, id);
        }
      });
      continue;
    }
    if (c->plugin.type == "gl.sampler") {
      continue;  // (this computer's samplers come with their clips: a take, a drop)
    }
    // CI-1118: the track joins the step that added the instrument, so one Undo takes both back
    // (in a collaboration the first Undo used to take back only the track, the instrument stayed).
    const uint64_t serial = a.doc.history().stateSerial();
    a.later([&a, id, serial] {
      const Song& x = a.doc.song();
      if (!x.channel(id) || trackmodes::instrumentTrackOf(x, id) >= 0 || !active(a)) {
        return;
      }
      const int t = trackmodes::freeTrack(x, trackmodes::usedTrackCount(x));  // a new track after the others
      if (t < 0) {
        return;
      }
      bool made = false;
      a.doc.amendStep(serial, [&] {
        a.doc.editSong("instrument track", [&](Song& y) {
          if (!trackmodes::makeInstrumentTrack(y, t, id)) {
            return;
          }
          made = true;
          const Channel* ch = y.channel(id);
          y.tracks[static_cast<size_t>(t)].name = std::to_string(trackmodes::shownNumber(y, t)) + " " + (ch ? ch->name : std::string());
          trackmodes::addReturnSendsTo(y, ch && ch->mixerTrack != y.masterId() ? ch->mixerTrack : kNoId);  // N53-08: as a dropped instrument's
        });
      });
      if (made) {
        knownPos = a.doc.history().position();
        knownRev = a.doc.revision();
      }
    });
  }
}
}  // namespace

bool loopOn(const App& a) {
  const Song& s = a.doc.song();
  return s.cycleOn && s.cycleEnd > s.cycleStart;
}

bool recordRange(const App& a, Tick& from, Tick& to) {
  const Song& s = a.doc.song();
  if (active(a)) {
    from = s.cycleStart, to = s.cycleEnd;
  } else {
    from = a.playlist.selStart, to = a.playlist.selEnd;
  }
  return to > from;
}

void setLoopBrace(App& a, Tick from, Tick to, bool on) {
  from = std::max<Tick>(0, from);
  if (to <= from) return;
  const Song& s = a.doc.song();
  if (s.cycleStart == from && s.cycleEnd == to && s.cycleOn == on) return;
  // As Smooth's cycle (R04-10): transport state, no undo step; saved with the Set.
  a.doc.patchSong([from, to, on](Song& x) { x.cycleStart = from, x.cycleEnd = to, x.cycleOn = on; });
}

void toggleLoop(App& a) {
  const Song& s = a.doc.song();
  if (s.cycleEnd > s.cycleStart) {  // the brace stays where it is; Loop turns on or off
    a.doc.patchSong([](Song& x) { x.cycleOn = !x.cycleOn; });
    return;
  }
  // No brace yet: the time selection's, else four bars from the play position.
  const Tick org = arrangementOrigin(a);
  if (a.playlist.selEnd > a.playlist.selStart) {
    setLoopBrace(a, std::max(org, a.playlist.selStart), std::max(org, a.playlist.selEnd), true);
    return;
  }
  const MeterMap mm(s);
  const Tick from = mm.barStartAt(std::max(org, static_cast<Tick>(a.engine.position())));
  Tick to = from;
  for (int k = 0; k < 4; ++k) {
    to = mm.nextBar(to);
  }
  setLoopBrace(a, from, to, true);
}

void loopFromSelection(App& a) {
  const Tick org = arrangementOrigin(a);
  if (a.playlist.selEnd > a.playlist.selStart) {
    setLoopBrace(a, std::max(org, a.playlist.selStart), std::max(org, a.playlist.selEnd), true);
    return;
  }
  // No time selection: the selected clips' span, as the reference's Loop Selection.
  const Song& s = a.doc.song();
  Tick lo = -1, hi = -1;
  for (const Clip& c : s.playlist) {
    if (a.playlist.selected.count(c.id) && c.start >= org) {
      lo = lo < 0 ? c.start : std::min(lo, c.start);
      hi = std::max(hi, c.start + s.clipLength(c));
    }
  }
  if (lo >= 0 && hi > lo) {
    setLoopBrace(a, lo, hi, true);
  } else {
    a.setHint(i18n::tr("Select a time range or clips in the Arrangement first: the loop brace then frames it"));
  }
}

std::vector<ObjectId> sessionExportClips(const App& a, std::string& label) {
  const Song& s = a.doc.song();
  std::vector<ObjectId> ids;
  if (a.engine.playing()) {  // grid-1118: the grid plays over the arrangement (song mode)
    for (int t = 0; t < gridColumns(s) && t < kMaxPlaylistTracks; ++t) {
      const Clip* c = s.clip(a.engine.liveActive(t));
      if (c && c->start < s.live.areaEnd && !c->muted) {
        ids.push_back(c->id);
      }
    }
  }
  if (!ids.empty()) {
    label = "the playing clips";
    return ids;
  }
  const int k = std::max(0, selectedScene());
  for (int t = 0; t < gridColumns(s); ++t) {
    const std::vector<int> v = slotClips(s, t, k);
    if (!v.empty()) {
      const Clip& c = s.playlist[static_cast<size_t>(v.front())];
      if (!c.muted) {
        ids.push_back(c.id);
      }
    }
  }
  label = ids.empty() ? std::string() : "scene " + std::to_string(k + 1);
  return ids;
}

void consolidate(App& a) {
  const Song& s = a.doc.song();
  std::map<int, std::set<ObjectId>> byTrack;
  std::map<int, bool> audio;
  for (const Clip& c : s.playlist) {
    if (a.playlist.selected.count(c.id) && arrangementClip(a, c) && c.kind != ClipKind::Automation) {
      byTrack[c.track].insert(c.id);
      audio[c.track] = audio[c.track] || c.kind == ClipKind::Audio;
    }
  }
  bool any = false;
  for (const auto& [t, ids] : byTrack) {
    any = any || ids.size() >= 2 || audio[t];
  }
  if (!any) {
    a.notify(i18n::tr(byTrack.empty() ? "Select clips in the Arrangement to consolidate them"
                                       : "Select two or more MIDI clips on a track, or audio clips, to consolidate them"),
             true);
    return;
  }
  const std::string dir = renderedDir(a);
  const double sr = a.engine.sampleRate();
  std::set<ObjectId> made;
  a.doc.beginGroup("consolidate clips");
  for (const auto& entry : byTrack) {
    const int t = entry.first;
    const std::set<ObjectId>& ids = entry.second;
    if (!audio[t]) {
      if (ids.size() >= 2) {
        if (const ObjectId nc = ops::mergeClips(a.doc, ids)) {
          made.insert(nc);
        }
      }
      continue;
    }
    ObjectId nid = kNoId;
    // Checkpoint 57 (N56-10): the rendered clip keeps the colour its clips had (an audio clip's sound's,
    // else the track's), as the reference keeps the track's colour; not the render's neutral grey.
    const Song& cur = a.doc.song();
    Rgb keep = t < static_cast<int>(cur.tracks.size()) ? cur.tracks[static_cast<size_t>(t)].color : Rgb{};
    for (const ObjectId id : ids) {
      const Clip* c = cur.clip(id);
      if (const Channel* src = c && c->kind == ClipKind::Audio ? cur.channel(c->source) : nullptr) {
        keep = src->color;
        break;
      }
    }
    a.doc.editSong("consolidate clips", [&](Song& x) {
      nid = consolidateClips(x, ids, dir, sr, true);
      const Clip* nc = nid != kNoId ? x.clip(nid) : nullptr;
      if (Channel* ch = nc ? x.channel(nc->source) : nullptr) {
        ch->color = keep;
      }
    });
    if (nid != kNoId) {
      made.insert(nid);
    }
  }
  a.doc.endGroup();
  if (made.empty()) {
    a.notify(i18n::tr("Nothing to render: the selected clips are silent"), true);
    return;
  }
  a.playlist.selected = made;
}

void transportFrame(App& a) {
  rowHeightsFrame(a);  // N55-06: the open Set's row-height key follows a Save As
  const bool clips = active(a);
  const Song& s = a.doc.song();
  Tick la = 0, lb = 0;
  if (clips) {
    if (loopOn(a)) la = s.cycleStart, lb = s.cycleEnd;
  } else if (a.playlist.selEnd > a.playlist.selStart) {
    la = a.playlist.selStart, lb = a.playlist.selEnd;
  } else if (loopOn(a)) {  // K-314 (A-05): no time selection: the song's saved loop, in every layout (playLoop)
    la = s.cycleStart, lb = s.cycleEnd;
  }
  static Tick sentA = -1, sentB = -1;
  if (la != sentA || lb != sentB) {
    a.engine.setLoopRange(la, lb);
    sentA = la, sentB = lb;
  }
  {  // the Arrangement's end: its last clip's bar end (cached per edit)
    static uint64_t serial = ~uint64_t{0}, epoch = ~uint64_t{0};
    static Tick end = 0;
    if (clips && (serial != a.doc.saveSerial() || epoch != a.doc.projectEpoch())) {
      serial = a.doc.saveSerial(), epoch = a.doc.projectEpoch();
      const Tick org = arrangementOrigin(a);
      Tick last = 0;
      for (const Clip& c : s.playlist) {
        if (c.start >= org && !c.muted) {
          last = std::max(last, c.start + s.clipLength(c));
        }
      }
      const MeterMap mm(s);
      end = last > org ? (mm.barStartAt(last) == last ? last : mm.nextBar(last)) : 0;
    }
    // Checkpoint 57 (N56-05): it plays on past its end in silence, as the reference does (Loop on loops
    // the brace); never into the clip grid (the start above).
    a.engine.setArrangement(clips ? arrangementOrigin(a) : 0, clips ? end : 0, false);
    a.engine.setSessionOverArrangement(clips);  // grid-1118: the clip grid plays over the arrangement
    if (clips) pendingLaunchFrame(a);
  }
  {  // grid2-1118: the person's follow action switch; a launched scene's tempo / time signature, once the
     // engine applied it at the scene's launch point, becomes the song's (one undo step; the scene rows
     // keep their length when the meter changes).
    static bool followSet = false;
    if (!followSet) {
      followSet = true;
      a.engine.setFollowActions(a.settings.getBool("clips.followActions"));
    }
    static uint32_t seen = 0;
    const uint32_t serial = a.engine.sceneChangeSerial();
    if (serial != seen) {
      seen = serial;
      const double bpm = a.engine.sceneTempoSet();
      const int num = a.engine.sceneSigNumSet(), den = a.engine.sceneSigDenSet();
      const bool tempoCh = bpm > 0 && std::fabs(bpm - s.tempo) > 1e-9;
      const bool sigCh = num > 0 && den > 0 && (num != s.timeSigNum || den != s.timeSigDen);
      if (clips && (tempoCh || sigCh)) {
        a.doc.editSong("scene tempo", [&](Song& x) {
          if (sigCh) {
            if (x.live.sceneLen <= 0 && x.live.areaEnd > 0) {
              x.live.sceneLen = trackmodes::sceneTicksOf(x);
            }
            x.timeSigNum = num, x.timeSigDen = den;
          }
          if (tempoCh) {
            x.tempo = bpm;
          }
        });
      }
    }
  }
  // N55-02: the Arrangement's song position never rests in the clip grid's storage before it (Space on an
  // empty Arrangement, "Record it first": both start at its bar 1).
  static int seekWait = 0;
  if (clips && a.engine.mode() == PlayMode::Song && a.engine.position() < static_cast<double>(arrangementOrigin(a)) - 0.5) {
    if (seekWait <= 0) {
      a.engine.setPosition(arrangementOrigin(a));
      seekWait = 3;  // (the audio thread takes it a block later)
    }
    --seekWait;
  } else {
    seekWait = 0;
  }
  // Clips' Punch is its own switch, off unless the person turns it on (clips.punch); the other layouts
  // keep record.punch as it was. K-314 (A-03): entering Clips first keeps their switch as it is now in
  // record.punch (otherwise written only at exit), so leaving Clips restores it, not the value at start.
  static int was = -1;
  if (was != (clips ? 1 : 0)) {
    if (was >= 0) {  // (and leaving it keeps Clips' own, however it was set)
      a.settings.set(clips ? "record.punch" : "clips.punch", a.recPunch);
    }
    if (was >= 0 || clips) a.recPunch = a.settings.getBool(clips ? "clips.punch" : "record.punch");
    was = clips ? 1 : 0;
  }
}

void viewFrame(App& a) {
  sessionRecordFrame(a);
  gridDragFrame();  // grid2-1118: grid clips carried onto the Arrangement
  adoptPeerChannels(a);
  liveio::drawMenus(a);  // checkpoint 55 (gap 2): the In/Out section's From and To menus
  if (gOfferOneShots && !a.projectLoading()) {
    gOfferOneShots = false;
    offerOneShots(a);
  }
  if (gPickFirst && GImGui->OpenPopupStack.Size == 0) {
    gPickFirst = false;
    setDrawMode(a, false);  // N50-03: a set opens in pointer mode (Draw mode is the person's own toggle, B)
    const Song& s = a.doc.song();
    if (a.playlist.trackSel.empty() && a.playlist.selected.empty() && trackmodes::usedTrackCount(s) > 0) {
      a.playlist.trackSel = {0};
      focusTrack(a, 0);
    }
  }
  if (gOutside) {  // N45-10: coming into Clips (a switch, a launch) shows the set in the view it was saved in
    gOutside = false;
    projectOpened(a, a.projectPath);
  }
  {  // the Live way with installed plugins' windows (Settings > Interface)
    PluginWindowPolicy p;
    p.managed = true;
    p.openOnLoad = a.settings.getBool("live.pluginWindowsOnLoad");
    p.several = a.settings.getBool("live.pluginWindowsSeveral");
    p.selectedOnly = a.settings.getBool("live.pluginWindowsSelectedOnly");
    const Song& s = a.doc.song();
    const Channel* ch = s.channel(a.selChannel);
    p.channel = ch ? ch->id : kNoId;
    p.mixer = ch ? ch->mixerTrack : a.selMixer;
    setPluginWindowPolicy(p);
  }
  {  // Round 45: selecting a clip shows the clip band in place of the device band (one detail band at a
     // time, as the reference's detail view turns to the clip view when a clip is selected).
    // Not while a mouse button is held (integ-1117: any button, so a right-press that selects a clip
    // waits for its menu too): the press that selects a clip also starts its drag, and the band
    // swap rebuilds the dock (the arrangement moves under the pointer and the drag was lost). The
    // band turns once the button is released.
    static std::set<ObjectId> lastSel;
    static std::set<int> lastTracks;  // C47: a newly selected track brings its devices to the band
    static int pendingTrack = -1;     // N53-07: the track just selected alone, until the button is up
    if (a.playlist.trackSel != lastTracks) {
      lastTracks = a.playlist.trackSel;
      if (lastTracks.size() == 1) focusTrack(a, *lastTracks.begin());
      pendingTrack = lastTracks.size() == 1 ? *lastTracks.begin() : -1;
    }
    // N53-07: a track title (a group's too) or a slot of another track selected: the band shows that
    // track's devices and the status bar names it, as the reference's detail view follows the track; a
    // clip selected on another track is no longer the band's. (Once the button is up, as below.)
    if (pendingTrack >= 0 && GImGui->OpenPopupStack.Size == 0 && !ImGui::IsAnyMouseDown()) {
      bool other = false;
      for (ObjectId id : a.playlist.selected)
        if (const Clip* c = a.doc.song().clip(id); !c || c->track != pendingTrack) other = true;
      if (other && a.playlist.trackSel == std::set<int>{pendingTrack}) {
        a.playlist.selected.clear();
        layoutui::setRegionVisible(a, "live.device", true);
      }
      pendingTrack = -1;
    }
    // N46-01: not while a menu is open (the band swap closed the clip menu a right-click had just
    // opened); it happens when the menu closes.
    if (a.playlist.selected != lastSel && GImGui->OpenPopupStack.Size == 0 && !ImGui::IsAnyMouseDown()) {
      lastSel = a.playlist.selected;
      if (!lastSel.empty()) layoutui::setRegionVisible(a, "live.clip", true);
      // N46-03: one clip selected (grid or arrangement): the clip band edits its own pattern and channel.
      if (lastSel.size() == 1)
        if (const Clip* c = a.doc.song().clip(*lastSel.begin()); c && c->kind == ClipKind::Pattern) {
          a.selectPattern(c->pattern);
          selectClipChannel(a, *c);
        }
    }
    // 0.7.8: a click on a track (its title, header, mixer strip) or a clip turns the detail spot to that
    // track's devices or that clip, also when the selection did not change (a click on the selected
    // clip after the device view was shown brings the clip view back).
    if (gDetailWant != 0 && GImGui->OpenPopupStack.Size == 0 && !ImGui::IsAnyMouseDown()) {
      const bool clip = gDetailWant == 1;
      if (!clip || !a.playlist.selected.empty()) showDetailView(a, clip, gDetailFocus);
      gDetailWant = 0;
      gDetailFocus = false;
    }
  }
  {  // 0.7.8: the detail spot, one view at a time
    const layout::Shell& sh = layout::shell(layout::Preset::Live);
    const layout::State st = layoutui::regionState(a, layout::Preset::Live);
    const bool clip = layout::visible(sh, st, "live.clip"), dev = layout::visible(sh, st, "live.device");
    // A layout saved while the two could stack (0.7.7) shows both: the one the selection asks for stays.
    if (clip && dev) showDetailView(a, !a.playlist.selected.empty(), false);
    else if (clip || dev) gLastClip = clip;
    if (gOpenDetailFrames > 0 && gOpenDetail > 0 && !a.projectLoading()) {  // an opened Set's view (the spot hidden: it stays hidden)
      --gOpenDetailFrames;
      if ((clip || dev) && clip != (gOpenDetail == 1)) showDetailView(a, gOpenDetail == 1, false);
      gLastClip = gOpenDetail == 1;
    }
    // The keys go to the view shown (Shift+Tab, its button, a double-click on a clip) once it is drawn
    // in the spot: the window in front there (the notes editor, the device chain, another tab).
    if (gDetailFocusFrames > 0) {
      --gDetailFocusFrames;
      const char* region = gLastClip ? "live.clip" : "live.device";
      const ImGuiDockNode* n = layout::visible(sh, st, region) ? ImGui::DockBuilderGetNode(layoutui::regionNode(region)) : nullptr;
      const ImGuiWindow* w = n ? n->VisibleWindow : nullptr;
      if (w && w->WasActive && !w->Hidden) {
        a.focusWindow = w->Name;
        gDetailFocusFrames = 0;
        gDetailKeysFrame = ImGui::GetFrameCount();
      } else if (gDetailFocusFrames == 0) {
        a.focusWindow = gLastClip ? "Piano roll" : "Device chain";
      }
    }
  }
  {  // C44-03: Tab is the view swap. Dear ImGui's own tabbing (always on) moved the keyboard into the
     // next text field of the focused window (the mixer band's volume fields), after which every Tab
     // went to that field; in Clips it only runs while a text field already has the keyboard.
    const ImGuiIO& io = ImGui::GetIO();
    GImGui->ConfigNavEnableTabbing = io.WantTextInput;
    // C44-17: Shift+Tab swaps the clip view and the device view in the detail spot (one at a time;
    // 0.7.8: the keys go with it; the spot hidden comes back with the other view than last time).
    if (!io.WantTextInput && io.KeyShift && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !a.showStage && GImGui->OpenPopupStack.Size == 0 &&
        ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
      swapDetailView(a);
    }
  }
  {  // C44-08: Esc closes the open menu or context menu (the top one; a dialog keeps its own Esc)
    ImGuiContext& g = *GImGui;
    if (g.OpenPopupStack.Size > 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      ImGuiWindow* top = g.OpenPopupStack.back().Window;
      if (!top || !(top->Flags & ImGuiWindowFlags_Modal)) ImGui::ClosePopupToLevel(g.OpenPopupStack.Size - 1, true);
    } else if (g.OpenPopupStack.Size == 0 && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      // N53-06 (checkpoint 54): Esc closes a device's floating editor window when it is the front window
      // (the last one opened or clicked), as the reference's plugin windows close.
      for (int i = g.WindowsFocusOrder.Size - 1; i >= 0; --i) {
        const ImGuiWindow* w = g.WindowsFocusOrder[i];
        if (!w || !w->WasActive || w->Hidden || w->DockIsActive || (w->Flags & ImGuiWindowFlags_ChildWindow)) continue;
        const char* n = w->Name ? std::strstr(w->Name, "###") : nullptr;
        if (n && std::strcmp(n, "###channelsettings") == 0) a.settingsChannel = kNoId;
        else if (n && std::strcmp(n, "###fxeditor") == 0) a.fxEditor = {kNoId, -1};
        break;  // only the front window
      }
    }
  }
  {  // C44-20: song playback starts at the arrangement's start, never inside the clip grid's storage
    // N45-02: Clips has no pattern mode: Space plays the arrangement (or the launched clips). C47: but
    // Session record runs in it (the recorded clip's pattern loops while it records).
    if (a.engine.mode() == PlayMode::Pattern && !a.recordArmed) a.engine.setMode(PlayMode::Song);
    const Tick o = arrangementOrigin(a);
    if (o > 0 && !a.engine.playing() && a.engine.mode() == PlayMode::Song && a.engine.position() < static_cast<double>(o))
      a.engine.setPosition(o);
  }
  static bool wasGrid = false;
  // C44-09: coming back to Clips from another layout brings back the view it was left in (the
  // other layouts open their own main windows, which said nothing about which view Clips showed).
  static int lastFrame = -10;
  static bool everShown = false;
  const int fc = ImGui::GetFrameCount();
  const bool back = everShown && fc - lastFrame > 1;
  lastFrame = fc;
  everShown = true;
  if (gOpenFrames > 0 && gOpenView >= 0) {  // N45-10: an opened set's view
    --gOpenFrames;
    if (gridView(a) != (gOpenView == 1) || a.showLivePads == a.showPlaylist) showView(a, gOpenView == 1);  // (only a change: a menu stays open)
  } else if (back) showView(a, wasGrid);
  else if (a.showLivePads && a.showPlaylist) showView(a, !wasGrid);  // one just opened: it wins
  else if (!a.showLivePads && !a.showPlaylist) showView(a, wasGrid);  // both closed: the last one comes back
  wasGrid = gridView(a);
}

// -------------------------------------------------------- track headers ----

namespace {

struct Cell {
  ImVec2 a, b;
  bool contains(ImVec2 m) const { return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y; }
};

// The parts of a header, left to right: name chip, activator, S, arm, volume (or Back to
// arrangement) and pan (on the second row only).
struct HeaderCells {
  Cell name, act, solo, arm, vol, pan;
  Cell io;  // checkpoint 56: the In/Out column between the name and the mixer controls (empty: none)
  bool hasPan = false;
};

// The name block fills the header's left part in the track's colour; the mixer cells sit in a
// column on its right (activator, S, arm on top, the volume under them when the row is tall
// enough, beside them otherwise, with the pan beside the volume), and a level meter runs down the
// right edge.
constexpr float kHeadMeterW = 10.0f;
constexpr float kNameGlyphW = 13.0f;  // a name's mode glyph (trackui::drawNameGlyph)
// K-383: Clips' track meters (core/ClipsMeter.h): each mixer track's post-fader peak on a top weighted
// 0 to -60 dB scale, green to -6 dB, yellow to 0 dB, red at and over 0 dBFS, with the Clips ballistics
// (instant rise, a 20 dB/s fall, a short held peak line). The strips, the headers and the device
// chain's Out draw this one. One state per key, run once a frame whoever draws it.
struct ClipsMeterState {
  LevelMeter m = clipsmeter::makeMeter();
  int frame = -1;
};
struct ClipsMeterDev {  // the last frame's drawn meters (dev-input "clipsmeters")
  std::string what;
  float levelDb[2], holdDb[2], pos[2], oldPos[2];
};
std::vector<ClipsMeterDev> gClipsMeterDev;
int gClipsMeterDevFrame = -1;

const LevelMeter& clipsMeterRun(uint64_t key, float peakL, float peakR) {
  static std::map<uint64_t, ClipsMeterState> states;
  if (states.size() > 4096) states.clear();  // deleted tracks never pile up
  ClipsMeterState& st = states[key];
  const int frame = ImGui::GetFrameCount();
  if (st.frame != frame) {
    st.m.update(peakL, peakR, ImGui::GetIO().DeltaTime);
    st.frame = frame;
  }
  return st.m;
}

void clipsMeterDraw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const LevelMeter& m, float gap, const char* what) {
  const auto& P = pal();
  const float y6 = clipsmeter::pos(clipsmeter::kYellowDb);
  const float bw = std::max(1.0f, std::floor((p1.x - p0.x - gap) * 0.5f));
  const float h = p1.y - p0.y;
  auto yAt = [&](float f) { return std::round(p1.y - f * h); };
  ClipsMeterDev dev{what ? what : "", {}, {}, {}, {}};
  for (int c = 0; c < 2; ++c) {
    const float x0 = p0.x + static_cast<float>(c) * (bw + gap), x1 = x0 + bw;
    const float db = m.levelDb(c);
    const float v = db > LevelMeter::kFloorDb ? clipsmeter::pos(db) : 0.0f;
    if (v > 0.0f) {
      const float top = yAt(v);
      if (db > 0.0f) {  // an over: the whole bar red
        dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, p1.y), P.meterHigh);
      } else {
        dl->AddRectFilled(ImVec2(x0, std::max(top, yAt(y6))), ImVec2(x1, p1.y), P.meterLow);
        if (v > y6) dl->AddRectFilled(ImVec2(x0, top), ImVec2(x1, yAt(y6)), P.meterMid);
      }
    }
    const float hd = m.holdDb(c);
    if (hd > LevelMeter::kFloorDb) {
      const float hy = std::min(yAt(clipsmeter::pos(hd)), p1.y - 1.0f);
      dl->AddLine(ImVec2(x0, hy + 0.5f), ImVec2(x1, hy + 0.5f), hd > 0.0f ? P.meterHigh : hd > clipsmeter::kYellowDb ? P.meterMid : P.meterLow, 1.0f);
    }
    dev.levelDb[c] = db, dev.holdDb[c] = hd, dev.pos[c] = v;
    dev.oldPos[c] = db > LevelMeter::kFloorDb ? LevelMeter::toPos(db) : 0.0f;  // the shared -60..+6 dB scale it used
  }
  if (what) {
    const int frame = ImGui::GetFrameCount();
    if (gClipsMeterDevFrame != frame) gClipsMeterDevFrame = frame, gClipsMeterDev.clear();
    if (gClipsMeterDev.size() < 512) gClipsMeterDev.push_back(dev);
  }
}

// The meter in an item of its own at the cursor (the strips and the device chain's Out).
void clipsMeterItem(uint64_t key, float peakL, float peakR, ImVec2 size, const char* what) {
  const LevelMeter& m = clipsMeterRun(key, peakL, peakR);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), pal().dark ? pal().field : pal().surface950, 1.0f);
  clipsMeterDraw(dl, p, ImVec2(p.x + size.x, p.y + size.y), m, 1.0f, what);
}

// Checkpoint 57: a header's level meter, at the right edge `right`: the slot, and the bars of `mixer`'s
// level when it has one. Tracks, returns and Main draw the same one (K-383: the Clips meter and its
// ballistics; it drew each frame's raw peak, which flickered).
void headMeter(App& a, ImDrawList* dl, float right, float my0, float my1, ObjectId mixer) {
  const auto& P = pal();
  const float mx0 = right - kHeadMeterW - 3;
  dl->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx0 + kHeadMeterW, my1), P.dark ? P.field : P.surface950, 1.0f);
  if (mixer == kNoId) {
    return;
  }
  const mixmeters::Reading r = mixmeters::get(a, mixer);
  const LevelMeter& m = clipsMeterRun((uint64_t{1} << 62) | mixer, r.peakL, r.peakR);
  const std::string what = "head " + std::to_string(mixer);
  clipsMeterDraw(dl, ImVec2(mx0 + 1, my0), ImVec2(mx0 + kHeadMeterW - 1, my1), m, 1.0f, what.c_str());
}
// HDR-1118: one grid at every height. The controls column (activator, S, arm; the volume under the
// activator and the pan under S and arm) sits at the same x on every row, so the columns line up down
// the arrangement and every name block is as wide as its neighbours'. Two rows from 43 px; one row (the
// name and activator, S and arm on one centred line, the volume and pan left out, as a short track in
// the reference shows them) from 16 px; below that the name alone. `back`: the row's grid plays a
// clip, so a short row keeps a Back to arrangement button left of the activator.
constexpr float kCellH = 16.0f, kCellGap = 3.0f, kCellPad = 4.0f;
constexpr float kCtlMinH = 16.0f;  // shorter rows show the name alone
bool twoRowHeader(float th) { return th >= kCellPad * 2 + kCellH * 2 + kCellGap; }
HeaderCells cellsOf(const HeaderDraw& h, bool back = false) {
  const float th = h.h1.y - h.h0.y;
  const float x1 = h.h1.x - kHeadMeterW - 6;
  const float sq = 18.0f, actW = 38.0f, gap = kCellGap, colW = actW + gap + sq + gap + sq;
  const bool twoRows = twoRowHeader(th);
  const bool ctl = th >= kCtlMinH;
  const float ch = twoRows ? kCellH : std::min(kCellH, th - 4.0f);
  const float y0 = twoRows ? std::round(h.h0.y + kCellPad) : std::round(h.h0.y + (th - ch) * 0.5f);
  HeaderCells c;
  const float rx0 = x1 - colW;
  // (the Back button of a short row: as wide as S and arm together, left of the activator)
  const float backW = !twoRows && back && ctl ? sq * 2 + gap : 0.0f;
  const float nameX1 = std::max(h.h0.x + 30, rx0 - 6 - h.ioW - (backW > 0 ? backW + gap : 0.0f));
  c.name = {ImVec2(h.h0.x, h.h0.y), ImVec2(nameX1, h.h1.y)};  // edge to edge: one line of the panel between tracks
  c.io = {ImVec2(nameX1, h.h0.y), ImVec2(std::max(nameX1, rx0 - 6 - (backW > 0 ? backW + gap : 0.0f)), h.h1.y)};
  if (!ctl) {
    return c;  // (empty cells: nothing to draw or hit)
  }
  c.act = {ImVec2(rx0, y0), ImVec2(rx0 + actW, y0 + ch)};
  c.solo = {ImVec2(c.act.b.x + gap, y0), ImVec2(c.act.b.x + gap + sq, y0 + ch)};
  c.arm = {ImVec2(c.solo.b.x + gap, y0), ImVec2(c.solo.b.x + gap + sq, y0 + ch)};
  if (twoRows) {
    const float v0 = y0 + ch + gap;
    c.vol = {ImVec2(rx0, v0), ImVec2(c.act.b.x, v0 + ch)};
    c.pan = {ImVec2(c.solo.a.x, v0), ImVec2(c.arm.b.x, v0 + ch)};
    c.hasPan = true;
  } else if (backW > 0) {
    c.vol = {ImVec2(rx0 - gap - backW, y0), ImVec2(rx0 - gap, y0 + ch)};
  }
  return c;
}

// C44-25: a track's solo is its own mixer track's solo, so the activators (the tracks' mutes) keep
// their states; a track with no mixer track of its own falls back to muting the others.
const MixerTrack* soloMixer(const Song& s, int track) {
  const MixerTrack* mx = trackmodes::playedMixer(s, track);
  return mx && !s.mixer.empty() && mx->id != s.mixer[0].id ? mx : nullptr;
}

bool soloed(const Song& s, int track) {
  if (const MixerTrack* mx = track >= 0 && track < static_cast<int>(s.tracks.size()) ? soloMixer(s, track) : nullptr) return mx->solo;
  if (track < 0 || track >= static_cast<int>(s.tracks.size()) || s.tracks[static_cast<size_t>(track)].muted || s.tracks.size() < 2) return false;
  for (size_t k = 0; k < s.tracks.size(); ++k)
    if (static_cast<int>(k) != track && !s.tracks[k].muted) return false;
  return true;
}

void square(ImDrawList* dl, const Cell& c, ImU32 fill, bool hot) {
  const auto& P = pal();
  // K-112c (round 31): pressed (hovered with the button down) the face sinks a shade, lit or not.
  const bool down = hot && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  dl->AddRectFilled(c.a, c.b, down ? theme::mix(fill, IM_COL32(0, 0, 0, 255), 0.18f) : fill, theme::kRadiusSm);
  // K-112c: a 1 px rim a shade darker than the face (lit or not), as the refs outline these buttons;
  // hovered, a light one.
  const ImU32 rim = hot ? theme::withAlpha(P.text, 0.45f) : theme::mix(fill, IM_COL32(0, 0, 0, 255), 0.35f);
  dl->AddRect(c.a, c.b, rim, theme::kRadiusSm);
}

void centredText(ImDrawList* dl, ImFont* f, float fs, const Cell& c, ImU32 col, const char* t) {
  theme::centreText(dl, f, fs, c.a, c.b, col, t);  // by the font's ink and capital height (K-112)
}

// The volume (or pan) drag of a header's cell (the linked mixer track's fader or pan).
struct VolDrag {
  ObjectId mixer = kNoId;
  double start = 0;
  float y0 = 0;
  bool pan = false;
} gVol;

// A pan value as the header shows it: C, or 1 to 50 with L or R.
std::string panText(float pan) {
  const int v = static_cast<int>(std::lround(pan * 50.0f));
  if (v == 0) return "C";
  return std::to_string(std::abs(v)) + (v < 0 ? "L" : "R");
}

}  // namespace

namespace {
// K-112c (round 30): Main's title is the brightest block and a return's a light grey, as the dark refs'
// headers and strips (357, 280); in a light theme they turn dark (093).
ImU32 mainFill() { return pal().text; }
ImU32 returnFill() { return theme::mix(pal().text, pal().surface800, 0.42f); }
char returnLetter(const Song& s, int mixerIndex);

float gBusClipY = 1e9f;  // hunt-clips3-1119: the return rows' visible foot (a row cut there takes no pointer below it)
void busRow(App& a, ImDrawList* dl, size_t j, float x0, float x1, float y) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const MixerTrack& mt = s.mixer[j];
  const ImVec2 m = ImGui::GetIO().MousePos;
  const ImVec2 r0(x0 + 1, y), r1(x1 - 1, y + kBusRowH - 2);
  // K-112c (round 31): the name block ends where the tracks' name blocks end and the volume sits in
  // their activator column, so the rows' columns line up (357, 280, 093).
  HeaderDraw hd;
  hd.h0 = ImVec2(x0 + 1, y), hd.h1 = ImVec2(x1, y + 60.0f);  // a two-row header's columns
  hd.ioW = liveio::shown(a) ? ioWidth() : 0.0f;  // checkpoint 56: and their In/Out column (58: sized to the window)
  const HeaderCells hc = cellsOf(hd);
  const float nameX1 = hc.name.b.x;
  // hunt-clips-1119: the press also starts the arrangement's own canvas item (drawn first), which then
  // blocks a plain hover test: the Main and return rows never took a click.
  const bool hov = m.x >= r0.x && m.x < r1.x && m.y >= r0.y && m.y < r1.y && m.y < gBusClipY && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  // hunt-clips2-1119: Main is ringed once chosen (its chain shown), as the clip grid's Main title: a new set
  // falls back to the master as its mixer track without anything clicked.
  const bool sel = a.selMixer == mt.id && (j != 0 || a.showMaster);
  const ImU32 base = j == 0 ? mainFill() : returnFill();
  const ImU32 fill = hov ? theme::mix(base, P.surface700, 0.15f) : base;
  dl->AddRectFilled(r0, r1, P.surface800);  // K-112c (round 32): the row's face, as a track header's, behind the fields
  dl->AddRectFilled(r0, ImVec2(nameX1, r1.y), fill, 0.0f);  // square, as the tracks' title blocks (141, 280)
  if (sel) dl->AddRect(ImVec2(r0.x + 0.75f, r0.y + 0.75f), ImVec2(nameX1 - 0.75f, r1.y - 0.75f), P.hot, 0.0f, 0, 1.5f);
  std::string name = j == 0 ? std::string(i18n::tr("Main")) : mt.name;
  if (const char lt = j == 0 ? 0 : returnLetter(s, static_cast<int>(j))) name = std::string(1, lt) + " " + name;  // as the strips' titles
  // HDR-1118: on the tracks' name column, in their size, cut with an ellipsis as theirs
  const float nfs = theme::kFontCaption + 1, nx = r0.x + 5 + kNameGlyphW;
  dl->PushClipRect(r0, ImVec2(nameX1, r1.y), true);
  theme::textEllipsis(dl, a.fontRegular, nfs, ImVec2(nx, theme::textTop(a.fontRegular, nfs, r0.y, kBusRowH - 2)), nameX1 - nx - 6, theme::contrastText(fill),
                      name.c_str());
  dl->PopClipRect();
  // Volume (dB) and the meter.
  const ImVec2 v0(hc.act.a.x, r0.y + 2), v1(hc.act.b.x, r1.y - 2);
  dl->AddRectFilled(v0, v1, P.dark ? P.field : P.surface950, theme::kRadiusSm);
  char vb[16];
  if (mt.volume > 1e-6f) std::snprintf(vb, sizeof vb, "%.1f", faderDb(mt.volume));
  else std::snprintf(vb, sizeof vb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
  const float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, vb).x;
  dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(v1.x - 4 - tw), theme::textTop(a.fontRegular, theme::kFontLabel, v0.y, v1.y - v0.y)), P.text, vb);
  if (hd.ioW > 0) {  // N55-10: a return's Audio To, Main's Master Out
    liveio::drawMixerBlock(a, dl, mt.id, ImVec2(hc.io.a.x + 5, y + 2), ImVec2(hc.io.b.x - 3, y + kBusRowH - 4), ImGui::IsWindowHovered());
  }
  headMeter(a, dl, hd.h1.x, r0.y + 3, r1.y - 3, mt.id);  // checkpoint 57: as the tracks' (N56 P6-10)
  if (hov) {
    a.setHint(j == 0 ? i18n::tr("Main: the master track (click to select it in the mixer)")
                     : i18n::tr("Return track: other tracks send to it (click to select it in the mixer)"));
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (j == 0) {
        selectMain(a);  // (as the clip grid's Main title: N49-09, its devices show)
      } else {
        selectReturn(a, mt.id);
      }
    }
  }
}
}  // namespace

void drawMainRow(App& a, ImDrawList* dl, float x0, float x1, float y) {
  const auto& P = pal();
  if (a.doc.song().mixer.empty()) return;
  dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(x1, y + kBusRowH), P.surface900);
  dl->AddLine(ImVec2(x0, y - 3.5f), ImVec2(x1, y - 3.5f), P.line);
  busRow(a, dl, 0, x0, x1, y);
}

namespace {
// Returns: mixer tracks an aux send reaches (not the master, not a bus fed by outputs), in mixer order.
// K-112c (C47): a bus (the demo's Drum Bus) is where its members' outputs go, not a return.
std::vector<size_t> returnTracks(const Song& s) { return trackmodes::returnTracks(s); }
// GitHub #58: the returns the session view shows a column and a strip for (hidden ones left out;
// each keeps its letter, so the send knobs still name it).
std::vector<size_t> shownReturns(const Song& s) {
  std::vector<size_t> out;
  for (size_t r : trackmodes::returnTracks(s))
    if (!mixhide::trackHidden(s, r)) out.push_back(r);
  return out;
}

// C47: View > Return tracks off folds their strips, titles and rows away (the sends stay on the tracks).
bool returnsShown(const App& a) { return a.settings.getBool("clips.returnTracks"); }
// A return's letter (A, B, ... by its place among the returns), or 0.
char returnLetter(const Song& s, int mixerIndex) {
  const std::vector<size_t> r = returnTracks(s);
  for (size_t i = 0; i < r.size() && i < 26; ++i)
    if (static_cast<int>(r[i]) == mixerIndex) return static_cast<char>('A' + i);
  return 0;
}
}  // namespace

float busRowsHeight(const App& a) {
  const Song& s = a.doc.song();
  if (s.mixer.empty() || !returnsShown(a)) {
    return 0.0f;
  }
  return 6.0f + kBusRowH * static_cast<float>(shownReturns(s).size());
}

// GitHub #58: "N hidden" at the right end of Main's title while some mixer strips are hidden; a click
// lists them (and Show all). Drawn after the title, whose button lets it take the hover.
static void hiddenChip(App& a, ImDrawList* dl, float x0, float x1, float y0, float h) {
  const std::string hl = mixhideui::hiddenLabel(a.doc.song());
  if (hl.empty()) return;
  const auto& P = pal();
  const float fs = theme::kFontLabel;
  const float tw = std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, hl.c_str()).x);
  const float cw = tw + 10, cx1 = x1 - 3, cx0 = std::max(x0 + 2, cx1 - cw);
  const ImVec2 q0(cx0, y0 + 2), q1(cx1, y0 + h - 2);
  ImGui::SetCursorScreenPos(q0);
  const bool click = ImGui::InvisibleButton("##hiddenchip", ImVec2(std::max(1.0f, q1.x - q0.x), std::max(1.0f, q1.y - q0.y)));
  const bool hov = ImGui::IsItemHovered();
  devMenuItem("mixer hidden"), devNoteRect("mixhidden");
  dl->AddRectFilled(q0, q1, hov ? theme::withAlpha(P.ink, 0.45f) : theme::withAlpha(P.ink, 0.28f), 0.0f);
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(q0.x + 5, theme::textTop(a.fontRegular, fs, q0.y, q1.y - q0.y)), q1.x - q0.x - 8,
                      theme::contrastText(mainFill()), hl.c_str());
  ui::Hint("Tracks hidden from the mixer (they still play): click to show them");
  if (click) ImGui::OpenPopup("##clipshidden");
  if (ImGui::BeginPopup("##clipshidden")) {
    mixhideui::hiddenMenu(a);
    ImGui::EndPopup();
  }
}

float drawBusRows(App& a, ImDrawList* dl, float x0, float x1, float top, float bottom) {
  const Song& s = a.doc.song();
  if (s.mixer.empty()) return 0;
  if (!returnsShown(a)) return 0;
  const std::vector<size_t> rows = shownReturns(s);
  float y = top + 6;  // a gap after the last track
  // hunt-clips3-1119: a row only partly in room is drawn cut at the foot (at 960 x 600 the returns were
  // left out until the arrangement was scrolled).
  dl->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
  gBusClipY = bottom;
  for (size_t j : rows) {
    if (y >= bottom) {
      break;
    }
    busRow(a, dl, j, x0, x1, y);
    y += kBusRowH;
  }
  gBusClipY = 1e9f;
  dl->PopClipRect();
  return std::min(y, bottom) - top;
}

namespace {
HeaderPart drawHeaderBody(App& a, ImDrawList* dl, const HeaderDraw& h);
// The track's grid plays a clip (its header offers Back to arrangement in place of the volume).
// grid-1118: the grid plays over the song, so a track is on the grid while a clip of its own has taken it.
bool onGridTrack(const App& a, int track) { return a.engine.arrangementOverridden(track) || a.engine.liveActive(track) != kNoId; }
}  // namespace

namespace {
float gIoW = liveio::kHeaderW;
}  // namespace
float headerWidth(const App& a, float viewW) {
  const float base = std::round(std::clamp(viewW * 0.19f, 180.0f, 240.0f));
  gIoW = std::round(std::clamp(viewW * 0.11f, 100.0f, 170.0f));
  return base + (liveio::shown(a) ? gIoW : 0.0f);
}
float ioWidth() { return gIoW; }

HeaderPart drawHeader(App& a, ImDrawList* dl, const HeaderDraw& h) {
  const Song& s = a.doc.song();
  if (h.lane || !liveio::shown(a) || h.track < 0 || h.track >= static_cast<int>(s.tracks.size())) return drawHeaderBody(a, dl, h);
  // Checkpoint 55 (gap 2), 56 (N55-10): the In/Out column right of the track's name, before its mixer
  // controls, as the reference's I/O section sits.
  const auto& P = pal();
  HeaderDraw body = h;
  body.ioW = ioWidth();  // checkpoint 58: sized to the window
  const HeaderPart part = drawHeaderBody(a, dl, body);
  const HeaderCells c = cellsOf(body, onGridTrack(a, h.track));
  const ImVec2 i0 = c.io.a, i1 = c.io.b;
  dl->AddRectFilled(i0, i1, h.selected ? P.surface700 : P.surface800);
  dl->AddLine(ImVec2(i0.x + 0.5f, i0.y), ImVec2(i0.x + 0.5f, i1.y), P.dark ? P.ink : P.line);
  const bool over = liveio::drawBlock(a, dl, h.track, ImVec2(i0.x + 5, i0.y + 3), ImVec2(i1.x - 3, i1.y - 3), h.hovered);
  if (h.lit) {  // checkpoint 57 (N56 P6-6): the frame round the whole header, the I/O column too
    dl->AddRect(h.h0, h.h1, theme::withAlpha(P.hot, 0.7f), 0, 0, 1.5f);
  } else if (h.selected) {
    dl->AddRect(h.h0, h.h1, theme::withAlpha(P.hot, 0.45f));
  }
  return over ? HeaderPart::Io : part;
}

namespace {
HeaderPart drawHeaderBody(App& a, ImDrawList* dl, const HeaderDraw& h) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  if (h.track < 0 || h.track >= static_cast<int>(s.tracks.size())) return HeaderPart::None;
  const PlaylistTrack& t = s.tracks[static_cast<size_t>(h.track)];
  const bool onGrid = !h.lane && onGridTrack(a, h.track);
  const HeaderCells c = cellsOf(h, onGrid);
  const bool ctl = c.act.b.x > c.act.a.x;  // HDR-1118: a row too short for controls shows its name alone
  const ImVec2 m = h.mouse;
  if (h.lane) {
    // Checkpoint 54 (N53-03): an automation lane is part of its track, as the reference's: a quieter
    // row with the parameter's name, indented under the track; no number, activator, solo or arm.
    const ImU32 face = h.selected ? P.surface700 : h.hovered ? theme::mix(P.surface900, P.surface800, 0.5f) : P.surface900;
    dl->AddRectFilled(h.h0, h.h1, face);
    const ImU32 tc = theme::content(t.color);
    dl->AddRectFilled(ImVec2(h.h0.x + 8, h.h0.y), ImVec2(h.h0.x + 11, h.h1.y), theme::withAlpha(tc, 0.85f));  // its track's colour
    std::string label = playlistTrackName(s, static_cast<size_t>(h.track));
    for (const Clip& k : s.playlist)  // the parameter's own name ("Volume"), whatever the track is called now
      if (k.track == h.track && k.kind == ClipKind::Automation)
        if (const AutomationClip* ac = s.automation(k.source)) {
          const std::string full = paramDisplayName(s, ParamId{ac->owner, ac->index});
          const size_t cut = full.rfind(" \xE2\x80\xBA ");
          label = cut == std::string::npos ? full : full.substr(cut + 5);
          break;
        }
    const float fs = theme::kFontCaption + 1;
    const float ty = theme::textTop(a.fontRegular, fs, h.h0.y + 2, std::min(20.0f, h.h1.y - h.h0.y - 2));
    dl->PushClipRect(h.h0, h.h1, true);
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(h.h0.x + 18, ty), h.h1.x - h.h0.x - 26, P.textDim, label.c_str());
    dl->PopClipRect();
    const bool in = h.hovered && m.x >= h.h0.x && m.x < h.h1.x && m.y >= h.h0.y && m.y < h.h1.y;
    return in ? HeaderPart::Name : HeaderPart::None;
  }
  // The row: a raised face, lit while selected or moving.
  const ImU32 face = h.lit || h.selected ? P.surface700 : h.hovered ? theme::mix(P.surface800, P.surface700, 0.5f) : P.surface800;
  dl->AddRectFilled(h.h0, h.h1, face);
  // Name block in the track's colour: number and name at its top, with Studio's mode glyph for linked tracks.
  const ImU32 tc = theme::content(t.color);
  const ImU32 chip = t.muted ? theme::mix(tc, P.surface800, 0.55f) : tc;
  dl->AddRectFilled(c.name.a, c.name.b, chip, 0.0f);  // K-112c (round 28): square, as every ref's title block
  const ImU32 ink = theme::contrastText(chip);
  // HDR-1118: the name on the controls' first line (its middle on theirs); a row with no controls
  // centres it, a size smaller where the row is shorter than the name.
  const float th = h.h1.y - h.h0.y;
  const float fs = !ctl && th < 15.0f ? theme::kFontLabel : theme::kFontCaption + 1;
  const float ty = ctl ? theme::textTop(a.fontRegular, fs, c.act.a.y, c.act.b.y - c.act.a.y) : theme::textTop(a.fontRegular, fs, h.h0.y, th);
  // K-112c (round 30): the name alone, as every ref (357, 280, 141, 093); the number is on the activator.
  const std::string label = playlistTrackName(s, static_cast<size_t>(h.track));
  dl->PushClipRect(c.name.a, c.name.b, true);
  float nx = c.name.a.x + 5;
  // K-112c (round 37): the name's ink, not a faint grey on the colour. Checkpoint 57 (N56 P6-5): every
  // name starts at one edge: a track with no glyph keeps the glyph's room.
  const float glyph = trackmodes::modeOf(s, h.track) != TrackMode::Unassigned ? trackui::drawNameGlyph(a, dl, h.track, ImVec2(nx, ty), theme::withAlpha(ink, 0.8f)) : 0.0f;
  nx += glyph > 0 ? glyph : kNameGlyphW;
  const bool group = trackmodes::isGroupTrack(s, h.track);
  theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(nx, ty), c.name.b.x - nx - (group ? 24 : 6), ink, label.c_str());  // regular weight (280, 141, 142)
  dl->PopClipRect();
  // Activator (the track's mute, numbered), solo, arm.
  const bool on = !t.muted;
  char num[16];
  std::snprintf(num, sizeof num, "%d", h.number > 0 ? h.number : h.track + 1);  // N53-04: as Clips counts
  const bool solo = soloed(s, h.track);
  if (ctl) {
    square(dl, c.act, on ? P.hot : P.surface900, h.hovered && c.act.contains(m));
    centredText(dl, a.fontRegular, theme::kFontLabel, c.act, on ? theme::contrastText(P.hot) : P.textFaint, num);
    square(dl, c.solo, solo ? P.solo : P.surface900, h.hovered && c.solo.contains(m));
    centredText(dl, a.fontBold, theme::kFontLabel, c.solo, solo ? theme::contrastText(P.solo) : P.textDim, "S");
  }
  // N45-05: the volume, pan and meter are the mixer track the track plays through (its strip's: an
  // instrument's, or the bus its sounds share); arm is the audio track's own.
  const MixerTrack* mx = trackmodes::playedMixer(s, h.track);
  const MixerTrack* lm = trackmodes::linkedMixer(s, h.track);
  const bool audio = trackmodes::modeOf(s, h.track) == TrackMode::Audio;
  const bool midiT = !audio && midiTrack(s, h.track);
  const bool armed = (audio && lm && lm->armed) || (midiT && midiArmedByHand(h.track));
  const bool autoArm = midiT && !armed && trackArmed(a, h.track);  // N47-10: the selected MIDI track, armed by itself
  if (!group && ctl) {  // checkpoint 52 (N51-11): a group records nothing (no arm, as in the grid)
    square(dl, c.arm, armed ? theme::withAlpha(P.arm, 0.85f) : autoArm ? theme::withAlpha(P.arm, 0.45f) : P.surface900, (audio || midiT) && h.hovered && c.arm.contains(m));
    const ImVec2 ac((c.arm.a.x + c.arm.b.x) * 0.5f, (c.arm.a.y + c.arm.b.y) * 0.5f);
    dl->AddCircleFilled(ac, std::min(3.5f, (c.arm.b.y - c.arm.a.y) * 0.25f), armed || autoArm ? theme::contrastText(P.arm) : audio || midiT ? P.text : theme::withAlpha(P.textDim, 0.45f), 12);  // K-112c (round 19): a neutral dot until armed (142, 157, 143)
  }
  // Volume (the linked mixer track's fader) or, while the grid plays a clip here, Back to arrangement.
  const bool hasVol = c.vol.b.x > c.vol.a.x;  // HDR-1118: a one-row header shows no volume (Back only)
  const float vcx = (c.vol.a.x + c.vol.b.x) * 0.5f, vcy = (c.vol.a.y + c.vol.b.y) * 0.5f;
  if (onGrid && hasVol) {
    square(dl, c.vol, P.hot, h.hovered && c.vol.contains(m));
    ui::drawIconFit(dl, ui::Icon::Undo, ImVec2(vcx - 6, vcy - 5), ImVec2(vcx + 6, vcy + 5), theme::contrastText(P.hot));
  } else if (mx && hasVol) {
    // Checkpoint 58 (N57-14): a track with no mixer track of its own (a MIDI track with no instrument)
    // shows no volume or pan fields, as its grid strip shows none (it showed dashes here).
    dl->AddRectFilled(c.vol.a, c.vol.b, P.dark ? P.field : P.surface950, theme::kRadiusSm);
    if (h.hovered && c.vol.contains(m)) dl->AddRect(c.vol.a, c.vol.b, theme::withAlpha(P.text, 0.35f), theme::kRadiusSm);
    {
      char vb[16];
      const float g = mx->volume;
      if (g > 1e-6f) std::snprintf(vb, sizeof vb, "%.1f", faderDb(g));
      else std::snprintf(vb, sizeof vb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
      const float vw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, vb).x;
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(c.vol.b.x - 5 - vw), theme::textTop(a.fontRegular, theme::kFontLabel, c.vol.a.y, c.vol.b.y - c.vol.a.y)),
                  P.text, vb);
    }
  }
  if (c.hasPan && mx) {  // the pan field (the linked mixer track's pan)
    dl->AddRectFilled(c.pan.a, c.pan.b, P.dark ? P.field : P.surface950, theme::kRadiusSm);
    if (h.hovered && c.pan.contains(m)) dl->AddRect(c.pan.a, c.pan.b, theme::withAlpha(P.text, 0.35f), theme::kRadiusSm);
    centredText(dl, a.fontRegular, theme::kFontLabel, c.pan, P.text, panText(mx->pan).c_str());
  }
  // The level meter down the right edge (the linked mixer track's). Checkpoint 57 (N56 P6-10): the
  // same slot on every row (a track with no mixer track, the returns and Main), so the edge is even.
  headMeter(a, dl, h.h1.x, h.h0.y + 3, h.h1.y - 3, mx ? mx->id : kNoId);
  if (h.lit) {  // checkpoint 57 (N56 P6-6): the frame round the whole header, over the name block too
    dl->AddRect(h.h0, h.h1, theme::withAlpha(P.hot, 0.7f), 0, 0, 1.5f);
  } else if (h.selected) {
    dl->AddRect(h.h0, h.h1, theme::withAlpha(P.hot, 0.45f));
  }
  // The volume drag in progress (moves the mixer track's volume, one undo step per drag).
  if (gVol.mixer != kNoId) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && gVol.pan) {
      const double dy = static_cast<double>(gVol.y0 - ImGui::GetIO().MousePos.y);
      a.doc.setParam(mixerParam(gVol.mixer, MixerParam::Pan), std::clamp(gVol.start + dy * (ImGui::GetIO().KeyShift ? 0.002 : 0.01), -1.0, 1.0), true);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const double dy = static_cast<double>(gVol.y0 - ImGui::GetIO().MousePos.y);
      const double db0 = gVol.start > 1e-6 ? faderDb(static_cast<float>(gVol.start)) : -60.0;
      const double db = std::clamp(db0 + dy * (ImGui::GetIO().KeyShift ? 0.05 : 0.25), -60.0, faderDb(1.25f));
      a.doc.setParam(mixerParam(gVol.mixer, MixerParam::Volume), db <= -59.9 ? 0.0 : dsp::faderValueForDb(db, 0.8), true);
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else {
      a.doc.endGesture();
      gVol = VolDrag{};
    }
  }
  // Checkpoint 51: a group's ringed fold button at the name block's right end (150).
  const float fcx = c.name.b.x - 12, fcy = std::round(ctl ? (c.act.a.y + c.act.b.y) * 0.5f : (h.h0.y + h.h1.y) * 0.5f);
  const bool onFold = group && h.hovered && std::fabs(m.x - fcx) <= 8 && std::fabs(m.y - fcy) <= 8;
  if (group) {
    const ImU32 fi = theme::withAlpha(ink, onFold ? 1.0f : 0.8f);
    // HDR-1118: the ring the size of the controls' line (inside a row too short for controls)
    const float fr = std::min(6.5f, (ctl ? c.act.b.y - c.act.a.y : th) * 0.5f - 1.0f);
    dl->AddCircle(ImVec2(fcx, fcy), fr, fi, 20, 1.2f);
    const float u = fr / 6.5f;
    if (t.folded) {
      dl->AddTriangleFilled(ImVec2(fcx - 1.5f * u, fcy - 3 * u), ImVec2(fcx - 1.5f * u, fcy + 3 * u), ImVec2(fcx + 2.5f * u, fcy), fi);
    } else {
      dl->AddTriangleFilled(ImVec2(fcx - 3 * u, fcy - 1.5f * u), ImVec2(fcx + 3 * u, fcy - 1.5f * u), ImVec2(fcx, fcy + 2.5f * u), fi);
    }
  }
  if (!h.hovered) return HeaderPart::None;
  if (onFold) return HeaderPart::Fold;
  if (c.act.contains(m)) return HeaderPart::Activator;
  if (c.solo.contains(m)) return HeaderPart::Solo;
  if (c.arm.contains(m)) return (audio && !group) || midiTrack(s, h.track) ? HeaderPart::Arm : HeaderPart::None;  // N47-10: MIDI tracks arm too
  if (c.vol.contains(m)) return onGrid ? HeaderPart::BackToArrangement : mx ? HeaderPart::Volume : HeaderPart::None;
  if (c.hasPan && c.pan.contains(m)) return mx ? HeaderPart::Pan : HeaderPart::None;
  if (c.name.contains(m)) return HeaderPart::Name;
  return HeaderPart::None;
}
}  // namespace

bool clickHeader(App& a, int track, HeaderPart part, bool right) {
  const Song& s = a.doc.song();
  if (track < 0 || track >= static_cast<int>(s.tracks.size())) return false;
  switch (part) {
    case HeaderPart::Io:
      return true;  // checkpoint 55: the In/Out column acted on the click itself
    case HeaderPart::Fold:
      if (right) return false;
      a.doc.editPlaylistTrack(track, s.tracks[static_cast<size_t>(track)].folded ? "unfold group" : "fold group", [](PlaylistTrack& t) { t.folded = !t.folded; });
      foldedSelection(a, track, false);
      return true;
    case HeaderPart::Activator:
      if (!right) {
        if (trackmodes::isGroupTrack(s, track)) {  // checkpoint 52 (N51-02): a group's activator silences all it plays
          const bool to = !s.tracks[static_cast<size_t>(track)].muted;
          a.doc.editSong(to ? "deactivate group" : "activate group", [track, to](Song& x) {
            PlaylistTrack& g = x.tracks[static_cast<size_t>(track)];
            g.muted = to;
            if (MixerTrack* m = x.mixerTrack(g.mixer)) m->muted = to;
          });
          return true;
        }
        a.doc.editPlaylistTrack(track, "mute playlist track", [](PlaylistTrack& t) { t.muted = !t.muted; });
        return true;
      }
      [[fallthrough]];  // right-click: solo, as Studio's switch
    case HeaderPart::Solo: {
      if (const MixerTrack* mx = soloMixer(s, track)) {
        ops::soloMixerTrack(a.doc, mx->id);
        return true;
      }
      const bool solo = soloed(s, track);
      a.doc.editSong(solo ? "unsolo playlist track" : "solo playlist track", [track, solo](Song& x) {
        for (size_t k = 0; k < x.tracks.size(); ++k) x.tracks[k].muted = solo ? false : static_cast<int>(k) != track;
      });
      return true;
    }
    case HeaderPart::Arm: {
      // N49-15: arming is exclusive for every track, MIDI and audio alike (Cmd keeps the others), as
      // the reference's Exclusive Arm.
      const ImGuiIO& io = ImGui::GetIO();
      const bool exclusive = !(io.KeySuper || io.KeyCtrl);
      auto otherAudioArmed = [&] {
        for (int i = 0; i < static_cast<int>(s.tracks.size()); ++i)
          if (i != track && trackmodes::modeOf(s, i) == TrackMode::Audio)
            if (const MixerTrack* m = trackmodes::linkedMixer(s, i); m && m->armed) return true;
        return false;
      };
      auto disarmOthers = [track](Song& x) {
        for (int i = 0; i < static_cast<int>(x.tracks.size()); ++i)
          if (i != track && trackmodes::modeOf(x, i) == TrackMode::Audio)
            if (const MixerTrack* m = trackmodes::linkedMixer(x, i); m && m->armed) trackmodes::setArmed(x, i, false);
      };
      if (trackmodes::modeOf(s, track) != TrackMode::Audio) {  // N47-10: a MIDI track's arm
        const bool on = !midiArmedByHand(track);
        toggleMidiArm(a, track, exclusive);
        if (on && exclusive && otherAudioArmed()) a.doc.editSong("disarm track", disarmOthers);
        return true;
      }
      const MixerTrack* mx = trackmodes::linkedMixer(s, track);
      const bool to = !(mx && mx->armed);
      a.doc.editSong(to ? "arm track" : "disarm track", [track, to, exclusive, disarmOthers](Song& x) {
        if (to && exclusive) disarmOthers(x);
        trackmodes::setArmed(x, track, to);
      });
      if (to && exclusive) gMidiArmed.clear();
      return true;
    }
    case HeaderPart::Volume:
      if (right) return false;
      if (const MixerTrack* mx = trackmodes::playedMixer(s, track)) {
        gVol.mixer = mx->id;
        gVol.start = mx->volume;
        gVol.y0 = ImGui::GetIO().MousePos.y;
      }
      return true;
    case HeaderPart::Pan:
      if (const MixerTrack* mx = trackmodes::playedMixer(s, track)) {
        if (right || ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) {  // back to the centre
          a.doc.setParam(mixerParam(mx->id, MixerParam::Pan), 0.0, false);
          return true;
        }
        gVol.mixer = mx->id;
        gVol.start = mx->pan;
        gVol.y0 = ImGui::GetIO().MousePos.y;
        gVol.pan = true;
      }
      return true;
    case HeaderPart::BackToArrangement:
      a.engine.backToArrangement(track);  // grid-1118: this track plays its arrangement clips again
      return true;
    default: return false;
  }
}

const char* headerHint(HeaderPart part) {
  switch (part) {
    case HeaderPart::Fold: return i18n::tr("Fold: shows or hides the group's tracks");
    case HeaderPart::Activator: return i18n::tr("Track on / off: click mutes or unmutes it, right-click solos it");
    case HeaderPart::Solo: return i18n::tr("Solo: only this track plays (click again for all)");
    case HeaderPart::Arm: return i18n::tr("Record arm: the track's mixer track records its input");
    case HeaderPart::Volume: return i18n::tr("Volume of the track's mixer track (dB): drag up or down, Shift for fine steps");
    case HeaderPart::Pan: return i18n::tr("Pan of the track's mixer track: drag up or down (Shift: fine), right-click centres it");
    case HeaderPart::BackToArrangement: return i18n::tr("Back to arrangement: the track stops its grid clip and plays the arrangement again");
    case HeaderPart::Name: return i18n::tr("Double-click the name to rename the track \xC2\xB7 right-click: track menu \xC2\xB7 drag to move it");
    default: return "";
  }
}

// ----------------------------------------------------------- status line ----

namespace {
// N49-10: the selected clip's track first (the status bar named the header selected before it).
int statusTrack(App& a) {
  const Song& s = a.doc.song();
  if (a.playlist.selected.size() == 1)
    if (const Clip* c = s.clip(*a.playlist.selected.begin()); c && c->track >= 0 && c->track < static_cast<int>(s.tracks.size())) return c->track;
  if (!a.playlist.trackSel.empty()) {
    const int t = *a.playlist.trackSel.begin();
    if (t >= 0 && t < static_cast<int>(s.tracks.size())) return t;
  }
  if (const Channel* c = s.channel(a.selChannel)) return trackmodes::instrumentTrackOf(s, c->id);
  return -1;
}

std::string selectedTrackName(App& a) {
  const Song& s = a.doc.song();
  if (const Clip* c = a.playlist.selected.size() == 1 ? s.clip(*a.playlist.selected.begin()) : nullptr;
      c && c->track >= 0 && c->track < static_cast<int>(s.tracks.size()))
    return std::to_string(trackmodes::shownNumber(s, c->track)) + ". " + playlistTrackName(s, static_cast<size_t>(c->track));
  if (!a.playlist.trackSel.empty()) {
    const int t = *a.playlist.trackSel.begin();
    if (t >= 0 && t < static_cast<int>(s.tracks.size())) return std::to_string(trackmodes::shownNumber(s, t)) + ". " + playlistTrackName(s, static_cast<size_t>(t));
  }
  if (const Channel* c = s.channel(a.selChannel)) {
    const int t = trackmodes::instrumentTrackOf(s, c->id);
    return t >= 0 ? std::to_string(trackmodes::shownNumber(s, t)) + ". " + playlistTrackName(s, static_cast<size_t>(t)) : c->name;
  }
  return std::string();
}
}  // namespace

void drawStatusBar(App& a) {
  const auto& P = pal();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = theme::kStatusBarH;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.chrome));
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  const float fs = theme::kFontCaption;
  const float ty = theme::textTop(a.fontRegular, fs, wp.y, h);
  // Right: the band toggles (lit while shown), then what is selected.
  const layout::Shell& sh = layout::shell(layout::Preset::Live);
  const layout::State st = layoutui::regionState(a, layout::Preset::Live);
  float rx = wp.x + W - 4;
  struct Tg {
    const char* id;
    const char* label;
    const char* region;
    const char* hint;
    const char* part;  // the window a band with nothing open in it opens
  };
  // K-297: the mixer band has its toggle here too, in the corner (as the refs keep it); before, it could
  // only be shown or hidden from the View menu.
  const Tg tg[] = {{"##sb_mix", i18n::N_("Mixer"), "live.mixer", i18n::N_("Mixer band: the tracks' levels, pans and sends under the main view"), "Mixer"},
                   {"##sb_dev", i18n::N_("Devices"), "live.device", i18n::N_("Device band: the selected track's instrument and the inserts of its mixer track, in the same place as the clip band (Shift+Tab switches)"), "Device chain"},
                   {"##sb_clip", i18n::N_("Clip"), "live.clip", i18n::N_("Clip band: the selected clip's properties and editors, in the same place as the device band (Shift+Tab switches)"), "Piano roll"}};
  for (const Tg& t : tg) {
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(layoutui::regionNode(t.region));
    const bool on = layout::visible(sh, st, t.region) && node && node->Windows.Size > 0;  // shown, with something in it
    const char* text = i18n::tr(t.label);
    const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, text).x + 18;
    rx -= tw;
    const ImVec2 bp(rx, wp.y + 4);
    ImGui::SetCursorScreenPos(bp);
    const bool click = ImGui::InvisibleButton(t.id, ImVec2(tw, h - 7));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(bp, ImVec2(bp.x + tw, bp.y + h - 7), on ? P.hot : hov ? P.surface700 : P.surface800, theme::kRadiusSm);
    dl->AddText(a.fontRegular, fs, ImVec2(bp.x + 9, ty), on ? theme::contrastText(P.hot) : P.textDim, text);
    ui::Hint(t.hint);
    devNoteRect(t.id + 2);  // (sdlat targets "sb_mix", "sb_dev", "sb_clip")
    if (click) {
      const bool detail = t.region != std::string("live.mixer");
      if (on) {
        layoutui::setRegionVisible(a, t.region, false);
      } else if (detail) {
        // 0.7.8: Clip and Devices switch the one detail spot (as Shift+Tab does) and give it the keys.
        showDetailView(a, t.region == std::string("live.clip"), true);
      } else {
        layoutui::setRegionVisible(a, t.region, true, false);
        // A band with nothing open in it opens its main part (the mixer).
        if (bool* f = layoutui::windowOpenFlag(a, t.part)) *f = true;
      }
    }
    rx -= 4;
  }
  rx -= theme::kS3;
  std::string sel = selectedTrackName(a);
  {  // N49-10: the device of that track (its instrument), none for an empty MIDI track's note-only channel
    const Song& s = a.doc.song();
    const int t = statusTrack(a);
    const Channel* c = t >= 0 ? trackmodes::linkedChannel(s, t) : s.channel(a.selChannel);
    const bool notesOnly = c && c->plugin.type == "gl.midiout" && c->mixerTrack == kNoId;
    if (c && !notesOnly)
      if (const PluginInfo* pi = findPlugin(c->plugin.type)) sel += (sel.empty() ? "" : " \xC2\xB7 ") + pi->name;
  }
  {  // grid2-1118: the edit grid of the view in front (Cmd+1 .. 5 change it)
    const std::string gl = gridLabel(a, gridView(a) ? 1 : 0);
    const float gw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, gl.c_str()).x;
    if (gw < W * 0.2f) {
      rx -= gw;
      dl->AddText(a.fontRegular, fs, ImVec2(rx, ty), P.textFaint, gl.c_str());
      rx -= theme::kS3;
    }
  }
  if (!sel.empty()) {
    const float sw = std::min(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, sel.c_str()).x, W * 0.3f);
    rx -= sw;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(rx, ty), sw + 1, P.textDim, sel.c_str());
  }
  if (a.doc.modified()) {
    rx -= theme::kS3;
    dl->AddCircleFilled(ImVec2(rx + 3, ty + fs * 0.5f + 1), 3.0f, P.textDim, 12);
  }
  // Far left (as the refs have it): the button that shows or hides the info corner, lit while shown.
  float lx = wp.x + 4;
  {
    const float bw = h - 7;
    const bool on = layout::visible(sh, st, "live.info");
    const ImVec2 bp(lx, wp.y + 4);
    ImGui::SetCursorScreenPos(bp);
    const bool click = ImGui::InvisibleButton("##sb_info", ImVec2(bw, bw));
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bw), on ? P.hot : hov ? P.surface700 : P.surface800, theme::kRadiusSm);
    ui::drawIconFitC(dl, ui::Icon::Info, ImVec2(bp.x + bw * 0.5f, bp.y + bw * 0.5f), ImVec2(bw - 8, bw - 8), on ? theme::contrastText(P.hot) : P.textDim);
    ui::Hint(i18n::tr("Info: shows or hides the corner that names what you point at and says what it does"));
    if (click) layoutui::setRegionVisible(a, "live.info", !on);
    lx += bw + theme::kS3;
  }
  // Left: the view and what it is for.
  const bool grid = gridView(a);
  const std::string view = grid ? i18n::tr("Clip grid") : i18n::tr("Arrangement");
  const char* what = grid ? i18n::tr("click a slot to select it, its triangle launches the clip, a scene's triangle launches the row")
                          : i18n::tr("the Grid switch at the top shows the clip grid");
  // K-112c (round 19): the message sits in a long recessed field from the info button to the right
  // end's items, as every ref's status bar (072, 093, 198, 276, 280).
  {
    const ImVec2 f0(lx - theme::kS1, wp.y + 4), f1(rx - theme::kS2, wp.y + h - 3);
    if (f1.x - f0.x > 60) {
      dl->AddRectFilled(f0, f1, P.dark ? P.field : P.surface950, 2.0f);
      dl->AddRect(f0, f1, theme::mix(P.dark ? P.field : P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), 2.0f);
    }
    lx += theme::kS1;
  }
  const float room = rx - lx - theme::kS4;
  const float vw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, view.c_str()).x;
  dl->AddText(a.fontRegular, fs, ImVec2(lx, ty), P.text, view.c_str());
  if (room > vw + 40) {
    const std::string rest = std::string(" \xC2\xB7 ") + what;
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(lx + vw, ty), room - vw, P.textDim, rest.c_str());
  }
  ImGui::End();
}

// ------------------------------------------------------------ info corner ----

void infoCorner(App& a) {
  const auto& P = pal();
  static std::string last;
  if (!a.hint.empty()) last = a.hint;
  std::string title, body;
  if (last.empty()) {
    title = i18n::tr("Info");
    body = i18n::tr("Point at anything: its name and what it does show here.");
  } else {
    // "Subject   (help)" or "Subject: help" -> a title and its text.
    size_t cut = last.find("   (");
    size_t skip = 4;
    if (cut == std::string::npos) cut = last.find(": "), skip = 2;
    // hunt-clips2-1119: a list of actions ("Double-click the name to rename the track · right-click: track
    // menu") has no subject: its first colon is inside the list (the title read "... · right-click").
    if (skip == 2 && cut != std::string::npos && last.rfind(" \xC2\xB7 ", cut) != std::string::npos) {
      cut = std::string::npos;
    }
    // hunt-clips-1119: "Subject (what: it does)": the bracket is the text ("Audio processing load (click"
    // was the title of the top bar's CPU readout, its text "the Performance panel)").
    bool bracket = skip == 4;
    if (skip == 2 && cut != std::string::npos && !last.empty() && last.back() == ')') {
      const size_t open = last.rfind(" (", cut);
      if (open != std::string::npos && open > 0 && last.find(')', open) == last.size() - 1) {
        cut = open, skip = 2, bracket = true;
      }
    }
    if (cut != std::string::npos && cut > 0 && cut < 60) {
      title = last.substr(0, cut);
      body = last.substr(cut + skip);
      if (bracket && !body.empty() && body.back() == ')') {
        body.pop_back();
      }
    } else {
      body = last;
    }
  }
  ImGui::Dummy(ImVec2(0, 2));
  if (!title.empty()) {
    ImGui::PushFont(a.fontBold, theme::kFontCaption + 1);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2));
  }
  if (!body.empty()) {
    if (!body.empty()) body[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(body[0])));
    ImGui::PushFont(a.fontRegular, theme::kFontCaption + 1);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::mix(P.textDim, P.text, 0.6f)));  // K-112c (round 19): nearer the text colour (072, 093)
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(body.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }
}

// ------------------------------------------------------------ device band ----

namespace {

// K-112: a device's on / off lamp, as the refs draw it: a dot in the accent colour while on, an
// empty ring while off.
void deviceLamp(ImDrawList* dl, ImVec2 c, bool on) {
  const auto& P = pal();
  c = ImVec2(std::round(c.x), std::round(c.y));
  if (on) {  // K-112c: the lit lamp with a thin dark rim, as the refs draw it
    dl->AddCircleFilled(c, 5.0f, P.accent, 20);
    dl->AddCircle(c, 5.0f, IM_COL32(16, 18, 22, 150), 20, 1.0f);
  }
  else dl->AddCircle(c, 4.5f, P.textFaint, 20, 1.2f);
}

// One device of the chain: the instrument (slot -1) or an insert of the mixer track.
struct Device {
  PluginTarget target;
  const PluginInfo* info = nullptr;
  std::string name, sub;
  bool enabled = true;
  int knobs = 0, cols = 0;
  float visW = 0;       // the compact view's width (DeviceViews), 0 without one
  bool hosted = false;  // an installed plugin: its own window, never redrawn here
  bool above = false;   // the picture spans the card's top, one row of knobs under it
  // 0.7.8: a sampler channel: its card holds the Audio clip window's band form (the chip row, or the
  // tab bar when the band is tall enough), as the window shows it everywhere else; no knobs.
  bool form = false;
  float formW = 0;  // the card's width (from the chain's)
  std::vector<devview::Main> mains;  // its main controls (DeviceViews); empty: its first parameters
  // The k-th knob's parameter and label.
  int param(int k) const { return mains.empty() ? k : mains[static_cast<size_t>(k)].param; }
  std::string label(int k) const {
    const int p = param(k);
    if (!mains.empty() && !mains[static_cast<size_t>(k)].label.empty()) return mains[static_cast<size_t>(k)].label;
    return info && p >= 0 && p < static_cast<int>(info->params.size()) ? info->params[static_cast<size_t>(p)].name : std::string();
  }
};

// Checkpoint 58 (N57-10): 72 px knob cells (60 cut 5 of an instrument's 15 labels: "Unison voi..."), and a
// narrower Out box (its note is a hint now).
constexpr float kCardTitleH = 20.0f, kCardPad = 8.0f, kKnobCellW = 72.0f, kFoldedW = 22.0f, kOutW = 168.0f;
// 0.7.8: the frame the sampler's band form had the keys (the band's own keys leave them to it).
int gFormKeys = -100;
int gFoldNoted = -1;  // the frame the first card's fold arrow was noted (dev "sdlat devfold")
// Checkpoint 60 (N59-09): the knob cells' width this frame: kKnobCellW, wider when the band has room to
// spare (at 1920 the chain left 775 px empty while long names were cut).
float gKnobCellW = kKnobCellW;
constexpr float kKnobCellMaxW = 100.0f;
constexpr float kHostedRowsW = 236.0f, kHostedShutW = 132.0f;
constexpr int kAllParamsMax = 64;  // an installed plugin with more starts with an empty panel (Configure fills it)

// What the band remembers per device (by target key; moves keep it with the device).
struct BandUi {
  uint64_t selected = 0;        // the selected device (Delete removes it, P switches its A/B)
  std::set<uint64_t> folded;    // shown as a narrow strip
  std::set<uint64_t> shut;      // an installed plugin's parameter panel is folded away
  std::set<uint64_t> configuring;
  std::map<uint64_t, int> seen;  // Configure: the last touched parameter when it was last looked at
  uint64_t rowSel = 0;           // Configure: the selected panel entry (target key) ...
  int rowSelParam = -1;          // ... and its parameter (Delete removes it)
};
BandUi& bandUi() {
  static BandUi u;
  return u;
}

// After moveSlot(owner, from, to) the devices' memory follows them.
void followMove(ObjectId owner, int from, int to) {
  BandUi& u = bandUi();
  auto moved = [&](int sl) {
    if (sl == from) return to;
    if (from < to && sl > from && sl <= to) return sl - 1;
    if (from > to && sl >= to && sl < from) return sl + 1;
    return sl;
  };
  auto remap = [&](uint64_t k) {
    for (int sl = 0; sl < kEffectSlots; ++sl)
      if (PluginTarget{owner, sl}.key() == k) return PluginTarget{owner, moved(sl)}.key();
    return k;
  };
  for (std::set<uint64_t>* s : {&u.folded, &u.shut, &u.configuring}) {
    std::set<uint64_t> n;
    for (uint64_t k : *s) n.insert(remap(k));
    *s = std::move(n);
  }
  u.selected = remap(u.selected);
  u.rowSel = remap(u.rowSel);
}

// A parameter name cut to fit under its knob (the whole name shows in the knob's tooltip and hint).
std::string fitText(ImFont* f, float fs, const std::string& name, float w) {
  if (f->CalcTextSizeA(fs, 1e9f, 0, name.c_str()).x <= w) return name;
  std::string s = name;
  while (s.size() > 1 && f->CalcTextSizeA(fs, 1e9f, 0, (s + "\xE2\x80\xA6").c_str()).x > w) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s + "\xE2\x80\xA6";
}
std::string fitLabel(App& a, const std::string& name, float w) { return fitText(a.fontRegular, theme::kFontLabel, name, w); }

// The devices whose picture reads best wide: transfer curves and sweeps go above their knobs.
bool pictureAbove(const PluginInfo* info) {
  if (!info) return false;
  const std::string& ty = info->type;
  return ty == "gl.comp" || ty == "gl.clipper" || ty == "gl.dist" || ty == "gl.flanger" || ty == "gl.phaser" || ty == "gl.chorus";
}

// A band knob: its name above the dial and its value under it (the band's own order; the rack
// panels' knobs keep theirs). The dial is the panels' (drag, wheel, menu, typing a value).
float bandKnobRadius() { return (pui::knobCellHeight(pui::KnobSmall) - 32) * 0.5f; }
float bandKnobH() { return bandKnobRadius() * 2 + 30; }
void bandKnob(App& a, const PluginTarget& t, int param, ImVec2 pos, const std::string& label, float cell) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ParamId pid = t.pid(param);
  const auto d = describeParam(s, pid);
  const auto v = getParam(s, pid);
  if (!d || !v) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  std::string full = label.empty() ? d->name : label;
  // Checkpoint 58 (N57-10): a name too long for its cell takes the usual short words first
  // ("Vel to volume", "Osc 1 PW"), and is cut only if it still does not fit (the hint names it in full).
  if (a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, full.c_str()).x > cell - 4) {
    static const std::pair<const char*, const char*> kShort[] = {
        {"Velocity", "Vel"}, {"velocity", "vel"}, {"Unison", "Uni"}, {"pulse width", "PW"}, {"Pulse width", "PW"},
        {"modulation", "mod"}, {"Modulation", "Mod"}, {"Frequency", "Freq"}, {"frequency", "freq"}, {"Resonance", "Res"},
        {"Envelope", "Env"}, {"envelope", "env"}, {"Amount", "Amt"}, {"amount", "amt"},
        {"Threshold", "Thresh"}, {"threshold", "thresh"}};  // checkpoint 59 (N58-09): "Comp thresh..."
    for (const auto& [l, sh] : kShort) {
      const size_t at = full.find(l);
      if (at != std::string::npos) {
        full.replace(at, std::strlen(l), sh);
      }
    }
    // Checkpoint 59 (N58-09): a note in brackets goes before the name is cut ("Release (legato)":
    // "Release"; the hint keeps it).
    const size_t br = full.find(" (");
    if (br != std::string::npos && br > 0 && a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, full.c_str()).x > cell - 4) {
      full.erase(br);
    }
  }
  const std::string lbl = fitLabel(a, full, cell - 4);
  const ImVec2 ls = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl.c_str());
  dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(pos.x + (cell - ls.x) * 0.5f), pos.y), P.textDim, lbl.c_str());
  const float r = bandKnobRadius();
  const ImVec2 keep = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(ImVec2(std::round(pos.x + cell * 0.5f - r), pos.y + 14));
  pui::dial(a, t, param, r);
  const bool act = ImGui::IsItemActive();
  ImGui::SetCursorScreenPos(keep);
  const std::string vs = fitText(a.fontRegular, 11.0f, d->format(*v), cell - 2);
  const ImVec2 vz = a.fontRegular->CalcTextSizeA(11.0f, 1e9f, 0, vs.c_str());
  dl->AddText(a.fontRegular, 11.0f, ImVec2(std::round(pos.x + (cell - vz.x) * 0.5f), pos.y + 14 + r * 2 + 2), act ? P.paper : P.text, vs.c_str());
}

// Text reading upwards from `o` (a folded device's strip): drawn flat, then its vertices turned.
void upwardText(ImDrawList* dl, ImFont* f, float fs, ImVec2 o, ImU32 col, const std::string& text) {
  const int v0 = dl->VtxBuffer.Size;
  dl->PushClipRectFullScreen();
  dl->AddText(f, fs, o, col, text.c_str());
  dl->PopClipRect();
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - o.x, dy = v.pos.y - o.y;
    v.pos = ImVec2(o.x + dy, o.y - dx);
  }
}

// The installed plugin's panel entries: its chosen list, else every parameter up to kAllParamsMax.
std::vector<int> panelParams(const PluginState* st, const PluginInfo* info) {
  std::vector<int> out;
  const int n = info ? static_cast<int>(info->params.size()) : 0;
  if (st && !st->shown.empty()) {
    for (int k : st->shown)
      if (k >= 0 && k < n) out.push_back(k);
    return out;
  }
  if (n <= kAllParamsMax)
    for (int k = 0; k < n; ++k) out.push_back(k);
  return out;
}

// Writes the panel's list (one undo step; saved with the project and shared with collaborators).
void setShown(App& a, const PluginTarget& t, const std::vector<int>& v) {
  if (t.isChannel()) {
    a.doc.editChannel(t.owner, "device parameters", [&](Channel& c) { c.plugin.shown = v; });
  } else {
    a.doc.editMixerTrack(t.owner, "device parameters", [&](MixerTrack& m) { m.slots[static_cast<size_t>(t.slot)].plugin.shown = v; });
  }
}

float cardWidth(const Device& d) {
  const BandUi& u = bandUi();
  if (u.folded.count(d.target.key())) return kFoldedW;
  if (d.form) return d.formW;
  if (d.hosted) return u.shut.count(d.target.key()) ? kHostedShutW : kHostedRowsW;
  const float cell = gKnobCellW;
  if (d.above && d.visW > 0) return kCardPad * 2 + std::max(d.visW + 40, static_cast<float>(std::max(2, d.cols)) * cell);
  return kCardPad * 2 + (d.visW > 0 ? d.visW + 8 : 0.0f) + static_cast<float>(std::max(d.visW > 0 ? 1 : 3, d.cols)) * cell;
}

// The device's window: the floating one, as in Studio (the instrument and effect editors open there
// in Clips). A sampler's settings show in its card (the Audio clip window's band form) unless asked
// for in a window (`insWindow`).
bool inWindow(App& a, const BandPick& pick, const Device& d) {
  if (d.hosted) return pluginWindowShown(a, d.target);
  if (d.target.isChannel()) return (!d.form || pick.insWindow) && a.settingsChannel == d.target.owner;
  return a.fxEditor.first == d.target.owner && a.fxEditor.second == d.target.slot;
}
void toggleWindow(App& a, BandPick& pick, const Device& d) {
  if (d.hosted) {
    togglePluginWindow(a, d.target);
    return;
  }
  const bool shown = inWindow(a, pick, d);
  if (d.target.isChannel()) {
    if (d.form) pick.insWindow = !shown;
    a.settingsChannel = shown ? kNoId : d.target.owner;
    if (!shown) a.focusWindow = d.form ? "Audio clip" : "###channelsettings";
  } else {
    a.fxEditor = shown ? std::pair<ObjectId, int>{kNoId, -1} : std::pair<ObjectId, int>{d.target.owner, d.target.slot};
    if (!shown) a.focusWindow = "###fxeditor";
  }
}

// A small glyph button of a device's title bar.
bool titleIcon(ImDrawList* dl, const char* id, ui::Icon ic, ImVec2 c0, float w, bool lit, const char* hint) {
  const auto& P = pal();
  ImGui::SetCursorScreenPos(c0);
  const bool click = ImGui::InvisibleButton(id, ImVec2(w, kCardTitleH - 4));
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 c1(c0.x + w, c0.y + kCardTitleH - 4);
  // K-112c: each title bar button sits in a thin ring (filled while hovered), as the refs draw them.
  const ImVec2 c(std::round((c0.x + c1.x) * 0.5f), std::round((c0.y + c1.y) * 0.5f));
  const float rr = 6.5f;
  if (hov) dl->AddCircleFilled(c, rr, P.surface600, 20);
  dl->AddCircle(c, rr, lit ? P.hot : theme::withAlpha(P.textDim, 0.75f), 20, 1.0f);
  ui::drawIcon(dl, ic, c, 7.0f, lit ? P.hot : hov ? P.text : P.textDim);
  ui::Hint(hint);
  return click;
}

// Dragging a device by its title moves it among the inserts; with Alt held at the drop it is copied.
struct DevDrag {
  ObjectId owner = kNoId;
  int slot = -1;
};
constexpr const char* kDevPayload = "GL_LIVEDEV";
constexpr const char* kRowPayload = "GL_LIVEPRM";

void dropOnInsert(App& a, ObjectId owner, int to, int browserSlot = -1) {
  if (!ImGui::BeginDragDropTarget()) return;
  if (browserSlot >= 0) {
    // hunt-clips-1119: "Drop effects here" takes an effect (or a file an effect loads) dragged from the
    // browser too, into that free insert, as a mixer slot does: it only took a device moved in the band.
    browserui::DropPlace place;
    place.mixer = owner;
    place.slot = browserSlot;
    browserui::acceptPluginDrop(a, browse::DropTarget::MixerSlot, place);
    browserui::acceptFileDrop(a, browse::DropTarget::MixerSlot, place);
  }
  if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kDevPayload)) {
    DevDrag dd;
    std::memcpy(&dd, pl->Data, sizeof dd);
    const Song& s = a.doc.song();
    const MixerTrack* mt = s.mixerTrack(owner);
    if (dd.owner == owner && mt && dd.slot >= 0 && dd.slot < kEffectSlots && to >= 0 && to < kEffectSlots) {
      if (ImGui::GetIO().KeyAlt) {  // a copy, in the first free slot, then moved to the drop
        int freeSlot = -1;
        for (int k = 0; k < kEffectSlots && freeSlot < 0; ++k)
          if (mt->slots[static_cast<size_t>(k)].plugin.type.empty()) freeSlot = k;
        if (freeSlot >= 0) {
          const PluginState copy = mt->slots[static_cast<size_t>(dd.slot)].plugin;
          a.doc.beginGroup("copy effect");
          if (a.doc.setSlotPlugin(owner, freeSlot, copy.type)) {
            setPluginState(a, PluginTarget{owner, freeSlot}, copy, "copy effect");
            if (to != freeSlot) {
              a.doc.moveSlot(owner, freeSlot, to);
              followMove(owner, freeSlot, to);
            }
          }
          a.doc.endGroup();
        } else {
          a.notify(i18n::tr("No free insert on this mixer track for the copy"), true);
        }
      } else if (dd.slot != to) {
        a.doc.moveSlot(owner, dd.slot, to);
        followMove(owner, dd.slot, to);
      }
    }
  }
  ImGui::EndDragDropTarget();
}

// K-112: the band's device clipboard: an effect's whole state (its type, parameters and data).
PluginState devClip;

void copyDevice(App& a, const Device& d) {
  if (d.target.isChannel()) return;
  if (const PluginState* st = d.target.state(a.doc.song())) devClip = *st;
}

// A copied effect goes right after `d` (after the instrument: the first insert) on the same mixer
// track, through the first free insert, as one undo step.
void pasteDevice(App& a, const Device& d) {
  if (devClip.type.empty()) return;
  const Song& s = a.doc.song();
  ObjectId owner = d.target.owner;
  int after = d.target.slot;
  if (d.target.isChannel()) {
    const Channel* c = s.channel(d.target.owner);
    if (!c || c->mixerTrack == kNoId) return;
    owner = c->mixerTrack;
    after = -1;
  }
  const MixerTrack* mt = s.mixerTrack(owner);
  if (!mt) return;
  int freeSlot = -1;
  for (int k = 0; k < kEffectSlots && freeSlot < 0; ++k)
    if (mt->slots[static_cast<size_t>(k)].plugin.type.empty()) freeSlot = k;
  if (freeSlot < 0) {
    a.notify(i18n::tr("No free insert on this mixer track for the pasted effect"), true);
    return;
  }
  const int to = freeSlot > after ? after + 1 : after;  // (erase-then-insert: past a freed earlier slot, one less)
  const PluginState copy = devClip;
  a.doc.beginGroup("paste effect");
  if (a.doc.setSlotPlugin(owner, freeSlot, copy.type)) {
    setPluginState(a, PluginTarget{owner, freeSlot}, copy, "paste effect");
    if (to != freeSlot && to >= 0) {
      a.doc.moveSlot(owner, freeSlot, to);
      followMove(owner, freeSlot, to);
    }
  }
  a.doc.endGroup();
}

// The device's menu (right-click on its title, or its menu button), grouped as the refs group a
// device's menu: cut, copy and paste; then removing and folding it and how its editor shows; then
// compare; then the plugin menu every window has (presets, state, replace, rename...).
void deviceMenu(App& a, BandPick& pick, const Device& d) {
  BandUi& u = bandUi();
  const uint64_t key = d.target.key();
  const bool folded = u.folded.count(key) > 0;
  const bool fx = !d.target.isChannel();
  // Checkpoint 59 (N58-06): an effect is cut, copied and duplicated; an instrument (one per track) is
  // not, so those rows are not shown for it. A copied effect pastes after any device, its own included.
  if (fx && ImGui::MenuItem(i18n::tr("Cut"), keys::label("rack.cut"))) {
    copyDevice(a, d);
    a.doc.setSlotPlugin(d.target.owner, d.target.slot, "");
  }
  if (fx && ImGui::MenuItem(i18n::tr("Copy"), keys::label("rack.copy"))) copyDevice(a, d);
  if (ImGui::MenuItem(i18n::tr("Paste"), keys::label("rack.paste"), false, !devClip.type.empty())) pasteDevice(a, d);
  if (fx && ImGui::MenuItem(i18n::tr("Duplicate"))) {
    const PluginState keep = devClip;
    copyDevice(a, d);
    pasteDevice(a, d);
    devClip = keep;
  }
  ImGui::Separator();
  if (fx && ImGui::MenuItem(i18n::tr("Delete"), "Del")) a.doc.setSlotPlugin(d.target.owner, d.target.slot, "");
  if (ImGui::MenuItem(folded ? i18n::tr("Unfold") : i18n::tr("Fold"))) {
    if (folded) u.folded.erase(key);
    else u.folded.insert(key);
  }
  if (ImGui::MenuItem(d.hosted ? i18n::tr("Plugin window") : i18n::tr("Open in a window"), nullptr, inWindow(a, pick, d))) toggleWindow(a, pick, d);
  if (d.hosted && ImGui::MenuItem(i18n::tr("Configure"), nullptr, u.configuring.count(key) > 0)) {
    if (!u.configuring.erase(key)) {
      u.configuring.insert(key);
      u.seen[key] = hostedLastTouched(d.target);
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(pui::compareOnB(d.target) ? i18n::tr("Compare: switch to A") : i18n::tr("Compare: switch to B"), "P")) pui::compareFlip(a, d.target);
  if (ImGui::MenuItem(i18n::tr("Compare: copy to the other side"))) pui::compareStore(a, d.target);
  ImGui::Separator();
  pluginWindowMenu(a, d.target.owner, d.target.slot, true);
  if (d.target.isChannel()) {  // K-254: the instrument's notes in the current clip to audio
    ImGui::Separator();
    renderui::channelMenu(a, d.target.owner);
  }
}

}  // namespace

void devDeviceMenu(App& a) {
  const Song& s = a.doc.song();
  const Channel* ch = s.channel(a.selChannel);
  if (!ch && !s.channels.empty()) ch = &s.channels.front();
  if (!ch) return;
  Device d;
  d.target = PluginTarget{ch->id, -1};
  d.info = findPlugin(ch->plugin.type);
  d.hosted = isHostedType(ch->plugin.type);
  d.name = d.info ? d.info->name : ch->name;
  if (const MixerTrack* mt = s.mixerTrack(ch->mixerTrack))
    for (int k = 0; k < kEffectSlots; ++k) {
      const EffectSlot& sl = mt->slots[static_cast<size_t>(k)];
      if (sl.plugin.type.empty()) continue;
      d.target = PluginTarget{mt->id, k};
      d.info = findPlugin(sl.plugin.type);
      d.hosted = isHostedType(sl.plugin.type);
      d.name = d.info ? d.info->name : sl.plugin.type;
      d.enabled = sl.enabled;
      break;
    }
  static BandPick pick;
  deviceMenu(a, pick, d);
}

namespace {

// A built-in's factory presets, from its title bar.
void presetPopup(App& a, const Device& d) {
  const PluginState* st = d.target.state(a.doc.song());
  if (!st || !d.info) return;
  if (d.info->presets.empty()) ImGui::TextDisabled("%s", i18n::tr("No presets"));
  for (size_t i = 0; i < d.info->presets.size(); ++i)
    if (ImGui::MenuItem(d.info->presets[i].name.c_str())) {
      PluginState ns = presetStateKeepingData(*d.info, i, st->data);
      ns.shown = st->shown;
      setPluginState(a, d.target, ns, "load preset");
      pui::presetLoaded(d.target, d.info->type, d.info->presets[i].name);
    }
}

// The installed plugin's parameter panel: Gridline's own rows (never the plugin's art).
void hostedRows(App& a, const Device& d, ImVec2 b0, ImVec2 b1) {
  const auto& P = pal();
  BandUi& u = bandUi();
  const uint64_t key = d.target.key();
  const PluginState* st = d.target.state(a.doc.song());
  std::vector<int> rows = panelParams(st, d.info);
  const bool configuring = u.configuring.count(key) > 0;
  const int touched = hostedLastTouched(d.target);
  if (configuring && touched >= 0 && touched != u.seen[key]) {  // a control touched in its window joins the panel
    u.seen[key] = touched;
    if (std::find(rows.begin(), rows.end(), touched) == rows.end()) {
      rows.push_back(touched);
      setShown(a, d.target, rows);
    }
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = b1.x - b0.x;
  if (configuring) {
    dl->AddRect(ImVec2(b0.x - 2, b0.y - 2), ImVec2(b1.x + 2, b1.y + 2), theme::withAlpha(P.hot, 0.8f), theme::kRadiusSm);
    dl->AddText(a.fontRegular, theme::kFontLabel, b0, P.hot, i18n::tr("Touch a control in its window to add it"));
    b0.y += 16;
  }
  // Outside Configure the control touched last shows too, for now, after the chosen ones.
  const bool temp = !configuring && touched >= 0 && std::find(rows.begin(), rows.end(), touched) == rows.end() && d.info &&
                    touched < static_cast<int>(d.info->params.size());
  if (rows.empty() && !temp) {
    // K-112c: an empty panel says how to fill it, centred in the card as the refs show it.
    const bool any = d.info && !d.info->params.empty();
    const char* msg = configuring ? "" : any ? i18n::tr("Click Configure, then move a control in the plugin's window to add it here")
                                             : i18n::tr("No parameters");
    if (*msg) {
      const float fs = theme::kFontLabel, ww = std::max(40.0f, w - 16);
      const ImVec2 ts = a.fontRegular->CalcTextSizeA(fs, 1e9f, ww, msg);
      const ImVec4 clip(b0.x, b0.y, b1.x, b1.y);
      // centred line by line: each wrapped line placed on its own
      float y = std::round(b0.y + std::max(2.0f, (b1.y - b0.y - ts.y) * 0.5f));
      const char* t = msg;
      const char* end = msg + std::strlen(msg);
      while (t < end) {
        const char* e = a.fontRegular->CalcWordWrapPosition(fs, t, end, ww);
        if (e == t) e = t + 1;
        const ImVec2 ls = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, t, e);
        dl->AddText(a.fontRegular, fs, ImVec2(std::round(b0.x + (w - ls.x) * 0.5f), y), P.textDim, t, e, 0, &clip);
        y += fs + 3;
        t = e;
        while (t < end && *t == ' ') ++t;
      }
    }
    return;
  }
  ImGui::SetCursorScreenPos(b0);
  ImGui::BeginChild("##rows", ImVec2(w, std::max(20.0f, b1.y - b0.y)), ImGuiChildFlags_None);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  const float gripW = configuring ? 12.0f : 0.0f, rw = ImGui::GetContentRegionAvail().x - gripW - 2;
  int moveFrom = -1, moveTo = -1;
  for (size_t r = 0; r < rows.size() + (temp ? 1 : 0); ++r) {
    const bool isTemp = r == rows.size();
    const int k = isTemp ? touched : rows[r];
    ImGui::PushID(static_cast<int>(r));
    const ImVec2 rp = ImGui::GetCursorScreenPos();
    if (configuring) {  // a grip to drag the entry (and select it: Delete removes it)
      ImGui::InvisibleButton("##grip", ImVec2(gripW, 22));
      const bool sel = u.rowSel == key && u.rowSelParam == k;
      if (ImGui::IsItemClicked()) u.rowSel = key, u.rowSelParam = k;
      ui::drawIcon(ImGui::GetWindowDrawList(), ui::Icon::GripDots, ImVec2(rp.x + gripW * 0.5f, rp.y + 11), 8.0f, sel ? P.hot : P.textFaint);
      ui::Hint("Drag to reorder; click, then Delete, to remove it from the panel");
      if (ImGui::BeginDragDropSource()) {
        const int from = static_cast<int>(r);
        ImGui::SetDragDropPayload(kRowPayload, &from, sizeof from);
        ImGui::TextUnformatted(d.info ? d.info->params[static_cast<size_t>(k)].name.c_str() : "");
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kRowPayload)) {
          std::memcpy(&moveFrom, pl->Data, sizeof moveFrom);
          moveTo = static_cast<int>(r);
        }
        ImGui::EndDragDropTarget();
      }
      ImGui::SetCursorScreenPos(ImVec2(rp.x + gripW + 2, rp.y));
    }
    hostedParamRow(a, d.target, k, rw);
    if (isTemp) ImGui::GetWindowDrawList()->AddRect(ImVec2(rp.x + gripW + 2, rp.y), ImVec2(rp.x + gripW + 2 + rw, rp.y + 22), theme::withAlpha(P.textFaint, 0.7f),
                                                    4.0f);
    ImGui::PopID();
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveFrom < static_cast<int>(rows.size()) && moveTo < static_cast<int>(rows.size())) {
    const int v = rows[static_cast<size_t>(moveFrom)];
    rows.erase(rows.begin() + moveFrom);
    rows.insert(rows.begin() + moveTo, v);
    setShown(a, d.target, rows);
  }
}

// Draws a device card at `p`, `h` tall: title bar, then its picture and main controls (a built-in)
// or its parameter panel (an installed plugin). A folded device is a narrow strip with its name.
void drawCard(App& a, ImDrawList* dl, BandPick& pick, const Device& d, ImVec2 p, float h) {
  const auto& P = pal();
  BandUi& u = bandUi();
  const uint64_t key = d.target.key();
  const float w = cardWidth(d);
  const ImVec2 p1(p.x + w, p.y + h);
  const bool selected = u.selected == key, folded = u.folded.count(key) > 0;
  const bool canBypass = !d.target.isChannel();
  dl->AddRectFilled(p, p1, P.surface800, theme::kRadiusLg);
  if (folded) {  // a strip: lamp, then the name reading upwards; a double-click (or click) unfolds it
    dl->AddRectFilled(p, p1, P.surface700, theme::kRadiusLg);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 2, p.y + 2));
    if (ImGui::InvisibleButton("##lamp", ImVec2(w - 4, 16)) && canBypass)
      a.doc.setParam(slotEnableParam(d.target.owner, d.target.slot), d.enabled ? 0.0 : 1.0, false);
    ui::Hint(canBypass ? "Turn the effect on or off (the mixer slot's switch)" : "The track's instrument");
    deviceLamp(dl, ImVec2(p.x + w * 0.5f, p.y + 10), d.enabled);
    // K-379: the fold arrow under the lamp, where the open card has it: a click unfolds it in place
    if (titleIcon(dl, "##fold", ui::Icon::Right, ImVec2(p.x + (w - 16) * 0.5f, p.y + 20), 16, false, "Unfold the device"))
      u.folded.erase(key);
    if (gFoldNoted != ImGui::GetFrameCount()) devNoteRect("devfold"), gFoldNoted = ImGui::GetFrameCount();  // (the first card's: an sdlat target)
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 38));
    ImGui::InvisibleButton("##strip", ImVec2(w, std::max(10.0f, h - 38)));
    if (ImGui::IsItemClicked()) u.selected = key;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) u.folded.erase(key);
    ui::Hint("Double-click to unfold the device");
    if (ImGui::BeginPopupContextItem("##menu")) {
      deviceMenu(a, pick, d);
      ImGui::EndPopup();
    }
    if (!d.target.isChannel()) dropOnInsert(a, d.target.owner, d.target.slot);
    const float fs = theme::kFontCaption;
    const std::string nm = fitText(a.fontBold, fs, d.name, h - 50);
    upwardText(dl, a.fontBold, fs, ImVec2(std::round(p.x + (w - fs) * 0.5f), p1.y - 8), d.enabled ? P.text : P.textDim, nm);
    if (selected) dl->AddRect(p, p1, P.hot, theme::kRadiusLg, 1.5f, 0);
    return;
  }
  dl->AddRectFilled(p, ImVec2(p1.x, p.y + kCardTitleH), selected ? theme::mix(P.surface700, P.surface600, 0.6f) : P.surface700, theme::kRadiusLg,
                    ImDrawFlags_RoundCornersTop);
  // Title bar, left to right: the activator, the fold arrow (a built-in: the card folds to a strip in
  // place, as a double-click on its name does) or the panel triangle (an installed plugin: its
  // parameter panel folds away in place), the name; at the right the window button (the device's own
  // editor in its window, as everywhere else in Gridline), Configure (installed plugins) or presets
  // (built-ins), and the menu. K-379: the arrow opened a half-screen copy of the editor above the band.
  const float ty0 = p.y + 2;
  ImGui::SetCursorScreenPos(ImVec2(p.x + 2, ty0));
  if (ImGui::InvisibleButton("##lamp", ImVec2(14, kCardTitleH - 4)) && canBypass)
    a.doc.setParam(slotEnableParam(d.target.owner, d.target.slot), d.enabled ? 0.0 : 1.0, false);
  ui::Hint(canBypass ? "Turn the effect on or off (the mixer slot's switch)" : "The track's instrument");
  deviceLamp(dl, ImVec2(p.x + 9, p.y + kCardTitleH * 0.5f), d.enabled);
  if (d.hosted) {
    const bool open = !u.shut.count(key);
    if (titleIcon(dl, "##panel", open ? ui::Icon::ChevronDown : ui::Icon::Right, ImVec2(p.x + 16, ty0), 14, false,
                  open ? "Fold the parameter panel away" : "Show the parameter panel")) {
      if (open) u.shut.insert(key);
      else u.shut.erase(key);
    }
  } else {
    if (titleIcon(dl, "##fold", ui::Icon::ChevronDown, ImVec2(p.x + 16, ty0), 14, false, "Fold the device to a narrow strip"))
      u.folded.insert(key);
    if (gFoldNoted != ImGui::GetFrameCount()) devNoteRect("devfold"), gFoldNoted = ImGui::GetFrameCount();  // (the first card's: an sdlat target)
  }
  // Right end: menu, then (to its left) the window button and Configure / presets. K-112: an installed
  // plugin's window button sits left of its name, after the panel triangle, and Configure is a word
  // at the right end (an icon when the card is too narrow for it), as the refs draw a plugin's card.
  const float menuX = p1.x - 18, winX = d.hosted ? p.x + 31 : menuX - 16;
  float thirdX = d.hosted ? menuX - 16 : winX - 16;
  if (titleIcon(dl, "##menubtn", ui::Icon::More, ImVec2(menuX, ty0), 15, false, "The device's menu")) ImGui::OpenPopup("##menu");
  if (titleIcon(dl, "##window", ui::Icon::WindowPopout, ImVec2(winX, ty0), 15, inWindow(a, pick, d),
                d.hosted ? "Show or hide the plugin's own window" : "Open the device's editor in a window"))
    toggleWindow(a, pick, d);
  {  // the first built-in card's window button (an sdlat target)
    static int noted = -1;
    if (!d.hosted && noted != ImGui::GetFrameCount()) devNoteRect("devwindow"), noted = ImGui::GetFrameCount();
  }
  if (d.hosted) {
    const bool conf = u.configuring.count(key) > 0;
    const char* cw = i18n::tr("Configure");
    const float cfs = theme::kFontCaption;
    const float cwW = std::round(a.fontRegular->CalcTextSizeA(cfs, 1e9f, 0, cw).x + 12);
    const bool word = menuX - 4 - cwW - (p.x + 48) >= 48;  // room for the word and a readable name
    bool hit = false;
    const char* chint = conf ? "Done: stop adding controls to the panel" : "Configure: controls touched in the plugin's window join the panel";
    if (word) {
      thirdX = menuX - 4 - cwW;
      const ImVec2 c0(thirdX, ty0 + 1), c1(thirdX + cwW, ty0 + kCardTitleH - 5);
      ImGui::SetCursorScreenPos(c0);
      hit = ImGui::InvisibleButton("##configure", ImVec2(c1.x - c0.x, c1.y - c0.y));
      const bool hov = ImGui::IsItemHovered();
      dl->AddRectFilled(c0, c1, conf ? P.hot : hov ? P.surface700 : P.surface800, 2.0f);  // K-112c (round 26): one hover step, as the other Live buttons
      if (!conf) dl->AddRect(c0, c1, P.dark ? P.lineStrong : P.line, 2.0f);
      theme::centreText(dl, a.fontRegular, cfs, c0, c1, conf ? theme::contrastText(P.hot) : P.text, cw);
      ui::Hint(chint);
    } else {
      hit = titleIcon(dl, "##configure", ui::Icon::Settings, ImVec2(thirdX, ty0), 15, conf, chint);
    }
    if (hit) {
      if (!u.configuring.erase(key)) {
        u.configuring.insert(key);
        u.seen[key] = hostedLastTouched(d.target);
        u.shut.erase(key);
        if (!pluginWindowShown(a, d.target)) togglePluginWindow(a, d.target);
      }
    }
  } else if (titleIcon(dl, "##presets", ui::Icon::ListBullets, ImVec2(thirdX, ty0), 15, false, "Presets")) {
    ImGui::OpenPopup("##presetlist");
  }
  if (ImGui::BeginPopup("##presetlist")) {
    presetPopup(a, d);
    ImGui::EndPopup();
  }
  // K-112c: hot-swap (another device in its place) and save (its preset to a file), left of the
  // right-end buttons as the refs draw them, while the card still leaves the name room.
  if (thirdX - 32 - (d.hosted ? p.x + 48 : p.x + 32) >= 56) {
    thirdX -= 32;
    if (titleIcon(dl, "##swap", ui::Icon::SwapArrows, ImVec2(thirdX, ty0), 15, false, "Hot-swap: put another device in its place"))
      ImGui::OpenPopup("##swaplist");
    devNoteRect("devswap");
    if (titleIcon(dl, "##save", ui::Icon::Save, ImVec2(thirdX + 16, ty0), 15, false, "Save its settings as a preset file"))
      savePresetDialog(a, d.target.owner, d.target.slot);
    devNoteRect("devsave");
  }
  if (ImGui::BeginPopup("##swaplist")) {
    swapDeviceItems(a, d.target.owner, d.target.slot);
    ImGui::EndPopup();
  }
  // The name: a click selects the device, a double-click folds it; dragged, it moves (Alt: copies).
  const float nameX = d.hosted ? p.x + 48 : p.x + 32, nameW = std::max(10.0f, thirdX - 2 - nameX);
  ImGui::SetCursorScreenPos(ImVec2(nameX, p.y + 1));
  ImGui::InvisibleButton("##title", ImVec2(nameW, kCardTitleH - 2));
  if (ImGui::IsItemClicked()) u.selected = key;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) u.folded.insert(key);
  ui::Hint(d.target.isChannel() ? "Click to select, double-click to fold"
                                : "Click to select (Delete removes it), double-click to fold; drag to move it, with Alt to copy it");
  if (!d.target.isChannel() && ImGui::BeginDragDropSource()) {
    const DevDrag dd{d.target.owner, d.target.slot};
    ImGui::SetDragDropPayload(kDevPayload, &dd, sizeof dd);
    ImGui::TextUnformatted(d.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (!d.target.isChannel()) dropOnInsert(a, d.target.owner, d.target.slot);
  if (ImGui::BeginPopupContextItem("##menu")) {  // also the menu button's popup (same id)
    deviceMenu(a, pick, d);
    ImGui::EndPopup();
  }
  const float fs = theme::kFontCaption + 1;
  const float ty = theme::textTop(a.fontBold, fs, p.y, kCardTitleH);
  dl->PushClipRect(ImVec2(nameX, p.y), ImVec2(nameX + nameW, p.y + kCardTitleH), true);
  // (a long name ends in an ellipsis clear of the buttons after it, not cut through a letter)
  dl->AddText(a.fontBold, fs, ImVec2(nameX + 1, ty), d.enabled ? P.text : P.textDim, fitText(a.fontBold, fs, d.name, nameW - 6).c_str());
  dl->PopClipRect();
  if (selected) dl->AddRect(p, p1, P.hot, theme::kRadiusLg, 1.5f, 0);
  const ImVec2 b0(p.x + kCardPad, p.y + kCardTitleH + 4);
  if (d.form) {  // a sampler: the Audio clip window's band form fills the card under its title
    const ImVec2 f0(p.x + 1, p.y + kCardTitleH), f1(p1.x - 1, p1.y - 2);
    if (f1.y - f0.y < 40 || f1.x - f0.x < 100) return;
    ImGui::SetCursorScreenPos(f0);
    ImGui::BeginChild("##smpform", ImVec2(f1.x - f0.x, f1.y - f0.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    clipeditor::drawDeviceBand(a, d.target.owner, ImGui::GetContentRegionAvail());
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) gFormKeys = ImGui::GetFrameCount();
    ImGui::EndChild();
    return;
  }
  if (d.hosted) {
    if (!u.shut.count(key)) hostedRows(a, d, b0, ImVec2(p1.x - kCardPad, p1.y - 6));
    else dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(b0.x, b0.y + 2), P.textFaint, d.sub.c_str());
    return;
  }
  float kx = b0.x;
  const float cell = gKnobCellW, cellH = bandKnobH();
  float ky = b0.y;
  if (d.above && d.visW > 0) {  // the picture across the card's top, the knobs in a row under it
    const ImVec2 v0(b0.x, b0.y + 2), v1(p1.x - kCardPad, std::max(b0.y + 24, p1.y - 6 - cellH));
    if (devview::draw(a, d.target, v0, v1) && !inWindow(a, pick, d)) toggleWindow(a, pick, d);  // its editor, in its window
    ky = v1.y + 4;
    kx = b0.x + std::max(0.0f, (p1.x - kCardPad - b0.x - static_cast<float>(d.cols) * cell) * 0.5f);
  } else if (d.visW > 0) {  // the device's picture, then its main knobs beside it
    const ImVec2 v0(b0.x, b0.y + 2), v1(b0.x + d.visW, p1.y - 8);
    if (devview::draw(a, d.target, v0, v1) && !inWindow(a, pick, d)) toggleWindow(a, pick, d);  // double-clicked where no handle took it
    kx += d.visW + 8;
  }
  for (int k = 0; k < d.knobs; ++k) {
    const int r = k / std::max(1, d.cols), col = k % std::max(1, d.cols);
    ImGui::PushID(k);
    bandKnob(a, d.target, d.param(k), ImVec2(kx + static_cast<float>(col) * cell, ky + static_cast<float>(r) * (cellH + 2)), d.label(k), cell);
    ImGui::PopID();
  }
}

// Which knobs a device shows: its main controls (DeviceViews), else its first parameters (the main
// ones come first in most plugins), as many as fit.
void pickKnobs(App& a, Device& d, float bandH) {
  if (d.form) {  // (the band form has the sampler's controls)
    d.knobs = d.cols = 0;
    return;
  }
  if (!d.hosted) d.mains = devview::mainParams(a, d.target);
  const int n = !d.mains.empty() ? static_cast<int>(d.mains.size()) : d.info ? static_cast<int>(d.info->params.size()) : 0;
  const float cellH = bandKnobH() + 2;
  d.above = d.visW > 0 && pictureAbove(d.info);
  const int rows = d.above ? 1 : std::max(1, static_cast<int>((bandH - kCardTitleH - 4) / cellH));
  const int maxCols = d.above ? 4 : d.visW > 0 ? (d.target.isChannel() ? 3 : 2) : d.target.isChannel() ? 5 : 4;
  d.knobs = std::min(n, rows * maxCols);
  d.cols = std::min(maxCols, std::max(1, (d.knobs + rows - 1) / rows));
}

std::string slotLabel(int slot) { return std::string(i18n::tr("Insert")) + " " + std::to_string(slot + 1); }

}  // namespace

namespace {
int gBandAsked = -1;  // dev "liveband" (devBandAsk): the script frame it was asked at
}  // namespace
void devBandAsk(int frame) { gBandAsked = frame; }

void devDetailReport(App& a, int frame) {
  const layout::Shell& sh = layout::shell(layout::Preset::Live);
  const layout::State st = layoutui::regionState(a, layout::Preset::Live);
  auto node = [](const char* region) { return ImGui::DockBuilderGetNode(layoutui::regionNode(region)); };
  auto shown = [&](const char* region) { return layout::visible(sh, st, region); };
  auto lit = [&](const char* region) {  // as the status bar lights its button
    const ImGuiDockNode* n = node(region);
    return shown(region) && n && n->Windows.Size > 0;
  };
  const bool clip = shown("live.clip"), dev = shown("live.device");
  const ImGuiDockNode* n = clip ? node("live.clip") : dev ? node("live.device") : nullptr;
  const ImGuiWindow* nav = GImGui->NavWindow ? GImGui->NavWindow->RootWindow : nullptr;
  std::printf("livedetail frame %d: clip %d device %d rect %.0f %.0f %.0f %.0f lit clip %d device %d grid %d focus \"%s\"\n", frame, clip ? 1 : 0,
              dev ? 1 : 0, n ? static_cast<double>(n->Pos.x) : 0.0, n ? static_cast<double>(n->Pos.y) : 0.0, n ? static_cast<double>(n->Size.x) : 0.0,
              n ? static_cast<double>(n->Size.y) : 0.0, lit("live.clip") ? 1 : 0, lit("live.device") ? 1 : 0, gridView(a) ? 1 : 0,
              nav ? nav->Name : "-");
  std::fflush(stdout);
}

void deviceBand(App& a, BandPick& pick) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  BandUi& u = bandUi();
  // A sampler's settings window closed: its card shows them again.
  if (pick.insWindow && a.settingsChannel == kNoId) pick.insWindow = false;
  const ObjectId chId = a.settingsChannel != kNoId ? a.settingsChannel : a.selChannel;
  const Channel* ch = s.channel(chId);
  const MixerTrack* mt = ch ? s.mixerTrack(ch->mixerTrack) : s.mixerTrack(a.selMixer);
  // C47 (C44-23): a chosen track with no channel of its own: its mixer track's inserts (an audio track),
  // or nothing yet (an empty MIDI track), not the master's.
  const bool trackFocus = gFocus.t >= 0 && gFocus.t < trackmodes::usedTrackCount(s) && a.selChannel == gFocus.ch && a.settingsChannel == kNoId && pick.kind != 1;
  if (trackFocus) {
    ch = nullptr;
    mt = trackmodes::playedMixer(s, gFocus.t);
  }
  // N52-12: the chain is the selected track's (a clip or a title chosen), as the status bar names it:
  // an audio or group track's inserts with no instrument (the sampler playing its clips is not one of
  // its devices), an instrument track's instrument and inserts, whatever channel was chosen before.
  if (pick.kind != 1 && a.settingsChannel == kNoId) {
    int selT = -1;
    if (a.playlist.selected.size() == 1)
      if (const Clip* sc = s.clip(*a.playlist.selected.begin())) selT = sc->track;
    if (selT < 0 && !a.playlist.trackSel.empty()) selT = *a.playlist.trackSel.begin();
    if (selT >= 0 && selT < trackmodes::usedTrackCount(s)) {
      if (trackmodes::modeOf(s, selT) == TrackMode::Audio) {
        ch = nullptr;
        mt = trackmodes::linkedMixer(s, selT);
      } else if (const Channel* lc = trackmodes::linkedChannel(s, selT); lc && lc != ch) {
        ch = lc;
        mt = s.mixerTrack(lc->mixerTrack);
      }
    }
  }
  // hunt-clips4-1119: Main or a return chosen (its title or row): the chain is its inserts, not the
  // instrument chosen before (their effects showed nowhere in the band).
  const MixerTrack* bus = chosenBus(a);
  if (bus && pick.kind != 1 && a.settingsChannel == kNoId) {
    ch = nullptr;
    mt = bus;
  }
  // An effect opened on another mixer track (from the mixer): the band shows that track's inserts.
  if (pick.kind == 1 && pick.fxTrack != kNoId && (!mt || mt->id != pick.fxTrack))
    if (const MixerTrack* fxt = s.mixerTrack(pick.fxTrack)) {
      mt = fxt;
      if (ch && ch->mixerTrack != fxt->id) ch = nullptr;
    }
  // N48-13: the note-only channel a new clip gives an empty MIDI track (silent, on no mixer track) is no
  // device: the band still asks for an instrument, as the reference's.
  const bool notesOnly = ch && ch->plugin.type == "gl.midiout" && ch->mixerTrack == kNoId && pick.kind != 1;
  if (notesOnly) ch = nullptr, mt = nullptr;
  if (!ch && !mt) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    if (trackFocus || notesOnly)
      ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 60.0f)), "No instrument yet",
                     "Drag an instrument from the browser onto this track: it and its effects show here");
    else
      ui::EmptyState(ImGui::GetWindowDrawList(), p0, ImVec2(p0.x + av.x, p0.y + std::max(av.y, 60.0f)), "No device",
                     "Select a track: its instrument and the inserts of its mixer track show here");
    ImGui::Dummy(ImVec2(av.x, std::max(av.y, 60.0f)));
    return;
  }
  // A device opened elsewhere (the mixer, the rack, View) is selected and unfolded; its editor opens in
  // its floating window, as in Studio. A sampler's settings show in its card (the band form), on the
  // tab they were asked for.
  if (pick.open && pick.kind != 0 && pick.kind != 1) pick.open = false;  // the master chain has its own window
  if (pick.open) {
    pick.open = false;
    const PluginTarget t = pick.kind == 0 && ch ? PluginTarget{ch->id, -1} : PluginTarget{pick.fxTrack, pick.slot};
    const PluginState* st = t.state(s);
    if (st && !isHostedType(st->type)) {
      u.selected = t.key();
      u.folded.erase(t.key());
      if (t.isChannel() && clipeditor::takesChannel(a, t.owner))
        clipeditor::deviceBandTab(a.channelTab == 1 ? clipwin::TabChannel : clipwin::entryTab(clipwin::Entry::ChannelSettings));
    }
  }
  // What the chain is (the track, then where the inserts live): the bracket label's hint.
  std::string chainHint = bus && !ch ? (bus == &s.mixer[0] ? std::string(i18n::tr("Main")) : bus->name) : selectedTrackName(a);
  if (chainHint.empty() && ch) chainHint = ch->name;
  {
    const size_t mtIndex = mt ? static_cast<size_t>(mt - s.mixer.data()) : 0;
    chainHint += std::string(chainHint.empty() ? "" : ": ") +
                 (mt && mtIndex > 0 ? std::string(i18n::tr("chain: the instrument, then the inserts of mixer track")) + " " + std::to_string(mtIndex) +
                                          " (" + i18n::tr("the same slots the mixer shows") + ")"
                                    : std::string(i18n::tr("chain: the instrument, then the master's inserts")));
  }
  std::vector<Device> devs;
  if (ch) {
    Device d;
    d.target = PluginTarget{ch->id, -1};
    d.info = findPlugin(ch->plugin.type);
    d.hosted = isHostedType(ch->plugin.type);
    d.name = d.info ? d.info->name : ch->name;
    d.sub = i18n::tr("instrument");
    d.form = !d.hosted && clipeditor::takesChannel(a, ch->id);
    d.visW = d.hosted || d.form ? 0.0f : devview::width(a, d.target);
    devs.push_back(d);
  }
  int firstFree = -1;
  if (mt)
    for (int k = 0; k < kEffectSlots; ++k) {
      const EffectSlot& sl = mt->slots[static_cast<size_t>(k)];
      const PluginInfo* fx = findPlugin(sl.plugin.type);
      const bool hosted = isHostedType(sl.plugin.type);
      if (!fx && !hosted) {
        if (firstFree < 0) firstFree = k;
        continue;
      }
      Device d;
      d.target = PluginTarget{mt->id, k};
      d.info = fx;
      d.hosted = hosted;
      d.name = fx ? fx->name : sl.plugin.type;
      d.sub = slotLabel(k);
      d.enabled = sl.enabled;
      d.visW = hosted ? 0.0f : devview::width(a, d.target);
      devs.push_back(d);
    }
  // Forget a selected device that is gone.
  {
    std::set<uint64_t> live;
    for (const Device& d : devs) live.insert(d.target.key());
    if (!live.count(u.selected)) u.selected = 0;
  }
  if (gBandAsked >= 0) {  // K-379 (dev "liveband"): each card as the band shows it
    const int asked = std::exchange(gBandAsked, -1);
    std::printf("liveband frame %d: devices %zu\n", asked, devs.size());
    for (const Device& d : devs) {
      const uint64_t k = d.target.key();
      std::printf("  liveband \"%s\" %s folded %d window %d shut %d width %.0f\n", d.name.c_str(), d.hosted ? "hosted" : d.form ? "sampler" : "builtin",
                  u.folded.count(k) ? 1 : 0, inWindow(a, pick, d) ? 1 : 0, u.shut.count(k) ? 1 : 0, static_cast<double>(cardWidth(d)));
    }
    std::fflush(stdout);
  }
  const float bandH = std::max(80.0f, ImGui::GetContentRegionAvail().y - 2);
  // The knobs fit the shortest card: the inserts' (under their bracket label).
  const float knobH = bandH - 16.0f;
  for (Device& d : devs) pickKnobs(a, d, knobH);

  // No scroll bar (its height would cost a row of knobs): the wheel scrolls the devices sideways.
  ImGui::BeginChild("##chain", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = ImGui::GetContentRegionAvail().y;
  const float chainW = ImGui::GetContentRegionAvail().x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float x0 = p.x;
  float bracket0 = -1;
  // 0.7.8: a sampler's band form takes about 60 % of the chain (the inserts and Out after it).
  for (Device& d : devs)
    if (d.form) d.formW = std::floor(std::clamp(chainW * 0.6f, 640.0f, 900.0f));
  {  // N59-09: the room the chain leaves (at the plain cell width) widens the knob cells, up to kKnobCellMaxW
    gKnobCellW = kKnobCellW;
    float used = 12.0f + 120.0f + 6.0f + 12.0f + kOutW + 14.0f + 8.0f;  // the bracket, the smallest drop zone, Out
    int cols = 0;
    for (const Device& d : devs) {
      used += cardWidth(d) + 12.0f;
      if (!d.hosted && !d.form && !bandUi().folded.count(d.target.key())) cols += std::max(d.visW > 0 ? 1 : 3, d.cols);
    }
    if (cols > 0 && chainW > used) gKnobCellW = std::min(kKnobCellMaxW, kKnobCellW + std::floor((chainW - used) / static_cast<float>(cols)));
  }
  // K-112c (round 26): with a bracket in the band every card starts under its top line, so the
  // instrument's title row lines up with the inserts' (it sat 4 px higher).
  const bool bracketed = std::any_of(devs.begin(), devs.end(), [](const Device& x) { return !x.target.isChannel(); });
  for (size_t i = 0; i < devs.size(); ++i) {
    const Device& d = devs[i];
    if (!d.target.isChannel() && bracket0 < 0) {
      // The inserts sit inside a bracket: they are the mixer track's own slots.
      p.x += 6;
      bracket0 = p.x;
      p.x += 6;
    } else if (i > 0) {
      p.x += 8;
    }
    ImGui::PushID(static_cast<int>(i));
    drawCard(a, dl, pick, d, ImVec2(p.x, p.y + (bracketed ? 4.0f : 0.0f)), h - (bracket0 >= 0 ? 16.0f : bracketed ? 4.0f : 0.0f));
    p.x += cardWidth(d);
    ImGui::PopID();
    if (d.target.isChannel() && i + 1 < devs.size()) {  // signal flow arrow into the inserts
      ui::drawIconFit(dl, ui::Icon::Play, ImVec2(p.x + 6, p.y + h * 0.5f - 5), ImVec2(p.x + 14, p.y + h * 0.5f + 5), P.textFaint);
      p.x += 12;
    }
  }
  // The drop zone: the next free insert of the mixer track (a device dragged here moves to it). It
  // takes the room left before Out, so the chain always ends in a place to drop the next device.
  if (mt && firstFree >= 0) {
    if (bracket0 < 0) {
      p.x += 6;
      bracket0 = p.x;
      p.x += 6;
    } else {
      p.x += 8;
    }
    const float outW = 12.0f + kOutW + 14.0f;  // the meter, Out and the bracket's end
    // Checkpoint 58 (N57-10): no wider than a few devices (it filled a 1920 window with 1048 px of nothing).
    const float zw = std::clamp(x0 + chainW - p.x - outW, 120.0f, 240.0f);
    const ImVec2 z0(p.x, p.y + 4), z1(p.x + zw, p.y + h - 12);
    ImGui::SetCursorScreenPos(z0);
    ImGui::InvisibleButton("##dropzone", ImVec2(z1.x - z0.x, z1.y - z0.y));
    {  // past the last device: a move goes to the end of the used inserts
      int lastUsed = -1;
      for (int k = 0; k < kEffectSlots; ++k)
        if (!mt->slots[static_cast<size_t>(k)].plugin.type.empty()) lastUsed = k;
      dropOnInsert(a, mt->id, std::max(0, lastUsed), firstFree);
    }
    const bool dragging = ImGui::GetDragDropPayload() != nullptr;
    dl->AddRect(z0, z1, theme::withAlpha(dragging ? P.hot : P.textFaint, dragging ? 0.8f : 0.35f), theme::kRadiusLg, 1.0f, 0);
    const std::string dz = std::string(i18n::tr("Drop effects here")) + "\n" + slotLabel(firstFree);
    const ImVec2 ts = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, dz.c_str());
    // K-112c (round 33): centred in the part of the zone in view (the band scrolls), and only when it fits
    // there whole: it was centred on the whole zone and cut by the band's edge ("Drop ef").
    const float vx0 = std::max(z0.x, dl->GetClipRectMin().x + 20), vx1 = std::min(z1.x, dl->GetClipRectMax().x - 20);
    if (vx1 - vx0 >= ts.x + 16) {  // each line centred on its own
      float ly = std::round((z0.y + z1.y - ts.y) * 0.5f);
      for (size_t b = 0; b <= dz.size();) {
        const size_t e = std::min(dz.find('\n', b), dz.size());
        const float lw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, dz.c_str() + b, dz.c_str() + e).x;
        dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((vx0 + vx1 - lw) * 0.5f), ly), P.textFaint, dz.c_str() + b, dz.c_str() + e);
        ly += ts.y / static_cast<float>(1 + std::count(dz.begin(), dz.end(), '\n'));
        b = e + 1;
      }
    }
    p.x = z1.x;
  }
  if (bracket0 >= 0 && mt) {
    // Dashed bracket around the inserts, named after the mixer track.
    const float bx0 = bracket0, bx1 = p.x + 6, by0 = p.y, by1 = p.y + h - 4;
    const ImU32 bc = theme::withAlpha(P.hot, 0.7f);
    for (float x = bx0; x < bx1; x += 7) {
      dl->AddLine(ImVec2(x, by0), ImVec2(std::min(x + 4, bx1), by0), bc);
      dl->AddLine(ImVec2(x, by1), ImVec2(std::min(x + 4, bx1), by1), bc);
    }
    for (float y = by0; y < by1; y += 7) {
      dl->AddLine(ImVec2(bx0, y), ImVec2(bx0, std::min(y + 4, by1)), bc);
      dl->AddLine(ImVec2(bx1, y), ImVec2(bx1, std::min(y + 4, by1)), bc);
    }
    const size_t mtIndex = static_cast<size_t>(mt - s.mixer.data());
    // N48-20 (checkpoint 49): the chain's owner by its Clips name (Main, or the track's), no Studio terms.
    const std::string lbl = mtIndex == 0 ? std::string(i18n::tr("Main")) : !mt->name.empty() ? mt->name : std::string(i18n::tr("Track")) + " " + std::to_string(mtIndex);
    const ImVec2 ls = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lbl.c_str());
    dl->AddRectFilled(ImVec2(bx0 + 10, by1 - ls.y * 0.5f - 1), ImVec2(bx0 + 16 + ls.x, by1 + ls.y * 0.5f), P.surface900);
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(bx0 + 13, by1 - ls.y * 0.5f - 1), bc, lbl.c_str());
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(bx0 + 10, by1 - ls.y * 0.5f - 1), ImVec2(bx0 + 16 + ls.x, by1 + ls.y * 0.5f)))
      a.setHint(chainHint);
    p.x = bx1 + 8;
  }
  // Out: where the track goes, after a level meter of the chain's output.
  if (mt) {
    const mixmeters::Reading r = mixmeters::get(a, mt->id);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 4));
    clipsMeterItem((uint64_t{2} << 62) | mt->id, r.peakL, r.peakR, ImVec2(6, h - 12), nullptr);  // K-383
    ui::Hint("The chain's output level (the mixer track's meter)");
    p.x += 12;
    const float ow = kOutW;
    const ImVec2 o0(p.x, p.y), o1(p.x + ow, p.y + h - 4);
    dl->AddRectFilled(o0, o1, P.surface800, theme::kRadiusLg);
    dl->AddText(a.fontBold, theme::kFontCaption, ImVec2(o0.x + 8, o0.y + 6), P.text, i18n::tr("Out"));
    // Checkpoint 52 (N51-11): the output is where it plays (Main, or its group); the rest are sends.
    const ObjectId outId = trackmodes::outputOf(*mt);
    const MixerTrack* outM = s.mixerTrack(outId);
    std::string to = outM && outM != &s.mixer[0] ? std::string(i18n::tr("to")) + "\xC2\xA0" + outM->name : std::string(i18n::tr("to Main"));
    for (const Send& sd : mt->sends)
      if (const MixerTrack* dst = s.mixerTrack(sd.to); dst && dst != &s.mixer[0] && sd.to != outId) {
        char lv[16];
        // K-112: no-break spaces keep a level with its % sign and "send" with the track's name when the line wraps.
        std::snprintf(lv, sizeof lv, "\xC2\xA0%d\xC2\xA0%%", static_cast<int>(std::lround(sd.level * 100)));
        to += std::string(" \xC2\xB7 ") + i18n::tr("send") + "\xC2\xA0" + dst->name + lv;
      }
    const ImVec4 clip(o0.x + 8, o0.y, o1.x - 8, o1.y - 4);
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(o0.x + 8, o0.y + 24), P.textDim, to.c_str(), nullptr, ow - 16, &clip);
    // Checkpoint 58 (N57-11): what an edit here does is the box's hint (the Info corner), not a paragraph in it.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(o0, o1)) {
      a.setHint(i18n::tr("Out: where the chain plays. Adding, moving or turning off a device here edits the mixer track: the mixer and every other layout show the change."));
    }
    p.x = o1.x;
  }
  ImGui::SetCursorScreenPos(ImVec2(x0, p.y));
  ImGui::Dummy(ImVec2(p.x - x0 + 4, h - 4));
  // Keys while the band has focus: Delete removes the selected insert (or, in Configure, the
  // selected panel entry); P switches the selected device's A/B compare.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
      gFormKeys < ImGui::GetFrameCount() - 1) {  // (a sampler's band form with the keys keeps them)
    const ImGuiIO& io = ImGui::GetIO();
    const bool plain = !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift;
    const Device* sel = nullptr;
    for (const Device& d : devs)
      if (d.target.key() == u.selected) sel = &d;
    if (plain && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
      const Device* rowDev = nullptr;
      for (const Device& d : devs)
        if (d.target.key() == u.rowSel && u.configuring.count(u.rowSel)) rowDev = &d;
      if (rowDev && u.rowSelParam >= 0) {
        std::vector<int> rows = panelParams(rowDev->target.state(s), rowDev->info);
        rows.erase(std::remove(rows.begin(), rows.end(), u.rowSelParam), rows.end());
        if (rows.empty()) rows.push_back(-1);  // an emptied panel stays empty (no list at all means every parameter)
        setShown(a, rowDev->target, rows);
        u.rowSelParam = -1;
      } else if (sel && !sel->target.isChannel()) {
        a.doc.setSlotPlugin(sel->target.owner, sel->target.slot, "");
        u.selected = 0;
      }
    } else if (plain && sel && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
      pui::compareFlip(a, sel->target);
    } else if (sel && io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {  // the device menu's cut, copy and paste
      if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        copyDevice(a, *sel);
      } else if (ImGui::IsKeyPressed(ImGuiKey_X, false) && !sel->target.isChannel()) {
        copyDevice(a, *sel);
        a.doc.setSlotPlugin(sel->target.owner, sel->target.slot, "");
        u.selected = 0;
      } else if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        pasteDevice(a, *sel);
      }
    }
  }
  // A plain wheel scrolls the chain sideways (over a plugin's parameter panel, the panel scrolls).
  if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0 && !ImGui::IsAnyItemActive())
    ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * 60.0f);
  {  // More devices past an edge: the edge fades and a small arrow says the wheel scrolls there.
    const ImVec2 w0 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const ImU32 bg = P.surface900, clear = theme::withAlpha(P.surface900, 0.0f);
    ImDrawList* fdl = ImGui::GetWindowDrawList();
    auto edge = [&](bool right) {
      const float x0e = right ? w0.x + ws.x - 28 : w0.x, x1e = x0e + 28;
      fdl->AddRectFilledMultiColor(ImVec2(x0e, w0.y), ImVec2(x1e, w0.y + ws.y), right ? clear : bg, right ? bg : clear, right ? bg : clear, right ? clear : bg);
      ui::drawIcon(fdl, right ? ui::Icon::Right : ui::Icon::Left, ImVec2(right ? x1e - 9 : x0e + 9, w0.y + ws.y * 0.5f), 9.0f, P.textDim);
    };
    if (ImGui::GetScrollX() < ImGui::GetScrollMaxX() - 1) edge(true);
    if (ImGui::GetScrollX() > 1) edge(false);
  }
  ImGui::EndChild();
}

// ------------------------------------------------------------ clip column ----

namespace {

// The playlist track whose launch settings the clip column shows: the selected channel's track,
// else the first selected header.
int clipTrack(App& a) {
  const Song& s = a.doc.song();
  // C44-10: the selected clip's track first (the Launch section fills as soon as a clip is selected).
  if (a.playlist.selected.size() == 1)
    if (const Clip* c = s.clip(*a.playlist.selected.begin()); c && c->track >= 0 && c->track < static_cast<int>(s.tracks.size())) return c->track;
  if (const Channel* c = s.channel(a.selChannel))
    if (const int t = trackmodes::instrumentTrackOf(s, c->id); t >= 0) return t;
  if (!a.playlist.trackSel.empty()) {
    const int t = *a.playlist.trackSel.begin();
    if (t >= 0 && t < static_cast<int>(s.tracks.size())) return t;
  }
  return -1;
}

// K-112c (round 21): one field of the column with its label above it, as the refs' clip view sets
// its fields (038, 041, 112): `cols` fields share the row side by side, `col` is this one's place.
// The cursor is left under the label, at the field's left; returns the field's width.
float columnField(App& a, const char* label, int col, int cols) {
  static ImVec2 row;
  static float rowW = 0;
  if (col == 0) row = ImGui::GetCursorScreenPos(), rowW = ImGui::GetContentRegionAvail().x;
  constexpr float kGap = 6.0f, kLabelH = 15.0f;
  const float w = std::floor((rowW - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
  const float x = row.x + static_cast<float>(col) * (w + kGap);
  ImGui::GetWindowDrawList()->AddText(a.fontRegular, theme::kFontLabel + 1, ImVec2(x, row.y), pal().textDim, label);
  ImGui::SetCursorScreenPos(ImVec2(x, row.y + kLabelH));
  return w;
}

// A section's title bar; `fill` (a colour) fills the bar, as the clip's title bar in its own colour.
// With a `foldId` the bar folds its section: a click on it shows or hides the rows under it (a
// chevron at its right: down while open, left while folded; remembered for the session). Returns
// whether the section is open.
bool columnHeading(App& a, const char* title, ImU32 chip, ImU32 fill = 0, const char* foldId = nullptr) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x, h = 20.0f;
  bool open = true, hov = false;
  if (foldId) {
    const ImGuiID id = ImGui::GetID(foldId);
    ImGuiStorage* st = ImGui::GetStateStorage();
    open = st->GetBool(id, true);
    if (ImGui::InvisibleButton(foldId, ImVec2(w, h))) st->SetBool(id, open = !open);
    hov = ImGui::IsItemHovered();
    ui::Hint(open ? "Click to fold this section" : "Click to show this section");
    ImGui::SetCursorScreenPos(p);
  }
  // A clip's title band, square as the layout's other bars, in the clip's full colour as 038, 034 and 112
  // draw it (K-112c round 34; a 42 % tint read as a muddy brown beside the clip in the arrangement).
  const ImU32 bg = fill ? fill : P.surface700;
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? theme::mix(bg, P.text, 0.08f) : bg, 0.0f);
  float tx = p.x + 6;
  // K-112c (round 35): no swatch on a band that is the colour itself (it showed as an empty check box), as 038.
  if (!fill && chip) {
    dl->AddRectFilled(ImVec2(tx, p.y + 6), ImVec2(tx + 8, p.y + 14), chip, 1.0f);
    tx += 13;
  }
  const ImU32 shown = bg;
  const ImU32 fg = fill ? theme::contrastText(shown) : P.text;
  const float right = foldId ? 20.0f : 6.0f;
  theme::textEllipsis(dl, a.fontBold, theme::kFontCaption, ImVec2(tx, theme::textTop(a.fontBold, theme::kFontCaption, p.y, h)), p.x + w - right - tx,
                      fg, title);
  if (foldId)
    ui::drawIcon(dl, open ? ui::Icon::ChevronDown : ui::Icon::ChevronLeft, ImVec2(p.x + w - 10, std::round(p.y + h * 0.5f)), 9,
                 theme::withAlpha(fg, hov ? 1.0f : 0.75f));
  ImGui::Dummy(ImVec2(w, h + 2));
  return open;
}

}  // namespace

bool columnSection(App& a, const char* title, ImU32 fill, const char* foldId) { return columnHeading(a, title, 0, fill, foldId); }
float columnRow(App& a, const char* label, int col, int cols) { return columnField(a, label, col, cols); }

void valueBox(App& a, const char* text, float w) {
  const auto& P = pal();
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + w, p.y + h);
  ImGui::Dummy(ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, q, P.field, theme::kRadiusSm);
  dl->AddRect(p, q, ImGui::GetColorU32(ImGuiCol_Border), theme::kRadiusSm, 1.0f, 0);
  const float fs = ImGui::GetFontSize();
  dl->PushClipRect(p, q, true);
  // Checkpoint 57 (N56-09): centred, as the drag fields beside it (Gain, Pitch) centre theirs.
  const float tw = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0, text).x;
  const float tx = std::max(p.x + ImGui::GetStyle().FramePadding.x, std::round(p.x + (w - tw) * 0.5f));
  dl->AddText(ImGui::GetFont(), fs, ImVec2(tx, theme::textTop(ImGui::GetFont(), fs, p.y, h)), P.text, text);
  dl->PopClipRect();
  (void)a;
}

// Checkpoint 48: a clip position as the reference's clip values read it, bars.beats.sixteenths from
// 1.1.1, in a box as valueBox (`duration`: a length, from 0.0.0). A vertical drag moves it a beat per
// 6 px (Shift: a sixteenth), the wheel a beat. N48-12: a click picks the field, and then the Up and
// Down arrows step it; a double-click types it ("2.3.1", "2.3" or "2"; Enter sets it, Escape keeps
// it). `v` changes within [lo, hi]. Returns true when it changed this frame.
bool positionField(App& a, const char* id, Tick& v, Tick lo, Tick hi, float w, const char* hint, bool duration) {
  const Song& s = a.doc.song();
  const Tick bar = std::max<Tick>(1, s.barTicks()), beat = std::max<Tick>(1, s.beatTicks()), six = std::max<Tick>(1, beat / 4);
  const long long one = duration ? 0 : 1;
  char text[96];  // (room for three 64-bit numbers: GCC -Wformat-truncation)
  const Tick c = std::max<Tick>(0, v);
  // Checkpoint 57 (N56-09): spaced as the transport's position ("1 . 1 . 1").
  std::snprintf(text, sizeof text, "%lld . %lld . %lld", static_cast<long long>(c / bar) + one, static_cast<long long>(c % bar / beat) + one,
                static_cast<long long>(c % beat / six) + one);
  const ImGuiID gid = ImGui::GetID(id);
  static ImGuiID typing = 0, picked = 0;
  static bool focusNext = false;
  static int typeGrace = 0;  // N50-04: frames the new field has to take the keyboard before "not active" means it was left
  static char buf[96] = {};  // (holds the field text: GCC -Wformat-truncation)
  Tick nv = v;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  if (typing == gid) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(w);
    if (focusNext || typeGrace > 0) ImGui::SetKeyboardFocusHere(), focusNext = false;
    const bool enter = ImGui::InputText("##type", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    const bool active = ImGui::IsItemActive() || ImGui::IsItemActivated();
    if (active) typeGrace = 0;
    else if (typeGrace > 0) --typeGrace;
    const bool gone = !enter && !active && typeGrace == 0;
    ImGui::PopID();
    if (enter) {
      long long f[3] = {one, one, one};
      int n = 0;
      for (const char* q = buf; *q && n < 3;) {
        while (*q && !std::isdigit(static_cast<unsigned char>(*q)) && *q != '-') ++q;
        if (!*q) break;
        char* e = nullptr;
        f[n++] = std::strtoll(q, &e, 10);
        q = e;
      }
      if (n > 0) nv = static_cast<Tick>((f[0] - one) * bar + (f[1] - one) * beat + (f[2] - one) * six);
      typing = 0;
    } else if (gone) {
      typing = 0;  // clicked away or Escape: the value stays
    }
  } else {
    valueBox(a, text, w);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    ui::Hint(hint);
    const Tick step = ImGui::GetIO().KeyShift ? six : beat;
    static float acc = 0;
    if (ImGui::IsItemActivated()) acc = 0, picked = gid;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      typing = gid, focusNext = true, typeGrace = 4;
      std::snprintf(buf, sizeof buf, "%s", text);
    } else if (ImGui::IsItemActive()) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
      acc -= ImGui::GetIO().MouseDelta.y;
      const int n = static_cast<int>(acc / 6.0f);
      acc -= static_cast<float>(n) * 6.0f;
      nv += step * n;
    } else if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
      nv += step * (ImGui::GetIO().MouseWheel > 0 ? 1 : -1);
    }
    if (picked == gid) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered()) picked = 0;  // a click elsewhere lets go
      else if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nv += step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nv -= step;
      }
      if (picked == gid) {
        const ImVec2 q(p.x + w, p.y + ImGui::GetFrameHeight());
        ImGui::GetWindowDrawList()->AddRect(p, q, pal().accent, theme::kRadiusSm, 0, 1.0f);  // the picked field
      }
    }
  }
  nv = std::clamp(nv, lo, std::max(lo, hi));
  if (nv == v) return false;
  v = nv;
  return true;
}

ObjectId clipRegion(const App& a, ObjectId pattern, Tick& start, Tick& end) {
  if (!active(a) || a.playlist.selected.size() != 1) return kNoId;
  const Song& s = a.doc.song();
  const Clip* c = s.clip(*a.playlist.selected.begin());
  if (!c || c->kind != ClipKind::Pattern || c->pattern != pattern) return kNoId;
  start = c->offset, end = c->offset + s.clipLength(*c);
  if (c->loop && c->loopLength > 0) start = c->loopStart, end = c->loopStart + c->loopLength;  // N50-05: with Loop on the brace is the loop
  return c->id;
}

void setClipRegion(App& a, ObjectId clip, Tick start, Tick end, const char* label, uint64_t mergeKey) {
  const Tick len = std::max<Tick>(1, end - start);
  start = std::max<Tick>(0, start);
  a.doc.editClip(clip, label, [start, len](Clip& x) {
    // N50-05: with Loop on (and a loop of its own) the brace is the loop: Position and Length.
    if (x.loop && x.loopLength > 0) x.loopStart = start, x.loopLength = len;
    else x.offset = start, x.length = len;
  }, mergeKey);
}

void columnScrollEdges() {
  // More rows past an edge: the edge fades and a small arrow says the wheel scrolls there.
  const auto& P = pal();
  const ImVec2 w0 = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  const ImU32 bg = P.surface900, clear = theme::withAlpha(P.surface900, 0.0f);
  ImDrawList* fdl = ImGui::GetWindowDrawList();
  // K-112c (round 26): the arrow sits in a solid strip at the very edge, past a short fade, so it
  // never lands on a field's text (it crossed "Next 4 beats" at 100 % and the labels at 150 %).
  auto edge = [&](bool bottom) {
    const float y0 = bottom ? w0.y + ws.y - 24 : w0.y, y1 = y0 + 24, kSolid = 12.0f;
    const float s0 = bottom ? y1 - kSolid : y0, s1 = s0 + kSolid;   // the solid strip
    const float f0 = bottom ? y0 : s1, f1 = bottom ? s0 : y1;       // the fade
    fdl->AddRectFilledMultiColor(ImVec2(w0.x, f0), ImVec2(w0.x + ws.x, f1), bottom ? clear : bg, bottom ? clear : bg, bottom ? bg : clear,
                                 bottom ? bg : clear);
    fdl->AddRectFilled(ImVec2(w0.x, s0), ImVec2(w0.x + ws.x, s1), bg);
    ui::drawIcon(fdl, bottom ? ui::Icon::ChevronDown : ui::Icon::ChevronUp, ImVec2(w0.x + ws.x * 0.5f, std::round((s0 + s1) * 0.5f)), 9.0f, P.textDim);
  };
  if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 1) edge(true);
  if (ImGui::GetScrollY() > 1) edge(false);
}

ObjectId focusedAudioClip(App& a) {
  if (a.playlist.selected.size() != 1) return kNoId;
  const Clip* c = a.doc.song().clip(*a.playlist.selected.begin());
  return c && c->kind == ClipKind::Audio && a.doc.song().channel(c->source) ? c->id : kNoId;
}

float clipColumn(App& a) {
  constexpr float kW = kClipColumnW;
  const ImVec2 p0 = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
  if (av.x < kW + 360.0f || av.y < 80.0f) return 0.0f;  // a narrow or short window keeps its whole area for the notes
  const auto& P = pal();
  const Song& s = a.doc.song();
  const Pattern* pat = s.pattern(a.curPattern);
  const int track = clipTrack(a);
  // A short band (1280 x 800) cannot show every row: the sections fold, and the column scrolls
  // with the wheel (a fade and an arrow at an edge say more is there). Checkpoint 58 (N57-06): a wide
  // band (1440 and up) shows the Clip and Launch sections side by side, as the audio clip view's Clip
  // and Sample, so nothing scrolls.
  const bool twoCols = gridView(a) && clipViewTwoColumns(av.x);
  auto beginColumn = [&](const char* cid) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface900));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));  // round 22: 8 px in from the panel, as the device band's cards below
    ImGui::BeginChild(cid, ImVec2(kW - 2, av.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  };
  beginColumn("##clipcol");
  // The clip: the pattern being edited (its name, length and meter are the pattern's own).
  // Its title bar: the clip's name in the clip's colour (the pattern's), or "Clip" when there is none.
  const bool clipOpen = pat ? columnHeading(a, pat->name.empty() ? i18n::tr("Clip") : pat->name.c_str(), 0, theme::content(pat->color), "##foldclip")
                            : columnHeading(a, i18n::tr("Clip"), 0, 0, "##foldclip");
  if (!clipOpen) {
    // folded: only the title bar
  } else if (!pat) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
    ImGui::TextWrapped("%s", i18n::tr("No pattern selected"));
    ImGui::PopStyleColor();
  } else {
    ImGui::PushID(static_cast<int>(pat->id));
    const float nameW = columnField(a, i18n::tr("Name"), 0, 1);
    static ObjectId nameFor = kNoId;
    static char name[128] = {};
    if (nameFor != pat->id || ImGui::GetActiveID() != ImGui::GetID("##name")) {  // follows the pattern unless being typed in
      std::snprintf(name, sizeof name, "%s", pat->name.c_str());
      nameFor = pat->id;
    }
    ImGui::SetNextItemWidth(nameW);
    if (renameFocusNow()) ImGui::SetKeyboardFocusHere();  // N47-12: the clip menu's Rename
    ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivatedAfterEdit() && pat->name != name) a.doc.renamePattern(pat->id, name);
    ui::Hint("The clip's name (every copy of this clip shows it)");
    // Checkpoint 48: the clip's Start and End, as the reference's first row (456, 454): where the clip
    // starts in its notes and where it ends; a drag moves either (the other stays). For the one clip
    // selected (grid or arrangement) that plays this pattern.
    const Clip* sel = nullptr;
    if (a.playlist.selected.size() == 1)
      if (const Clip* c = s.clip(*a.playlist.selected.begin()); c && c->kind == ClipKind::Pattern && c->pattern == pat->id) sel = c;
    if (sel) {
      const ObjectId cid = sel->id;
      const Tick six = std::max<Tick>(1, s.beatTicks() / 4);
      Tick start = sel->offset, end = sel->offset + s.clipLength(*sel);
      const float sw = columnField(a, i18n::tr("Start"), 0, 2);
      // N48-12: Start, End and Length are one region, the brace over the notes (the editor drags its ends).
      if (positionField(a, "##cstart", start, 0, end - six, sw,
                        "Where the clip starts in its notes (the brace's left end): drag, wheel or arrows, double-click to type")) {
        setClipRegion(a, cid, start, end, "clip start", ImGui::GetID("##cstart"));
      }
      const float ew = columnField(a, i18n::tr("End"), 1, 2);
      const Tick maxEnd = start + 512 * std::max<Tick>(1, s.barTicks());
      if (positionField(a, "##cend", end, start + six, maxEnd, ew,
                        "Where the clip ends (a grid clip starts again from its start there): drag, wheel or arrows, double-click to type")) {
        setClipRegion(a, cid, start, end, "clip end", ImGui::GetID("##cend"));
      }
      Tick len = end - start;
      const float lw = columnField(a, i18n::tr("Length"), 0, 2);
      if (positionField(a, "##clen", len, six, maxEnd - start, lw, "The clip's length (bars. beats. sixteenths): End moves with it", true))
        setClipRegion(a, cid, start, start + len, "clip end", ImGui::GetID("##cend"));
      // N50-05: Loop, and the loop's Position and Length inside the clip, as the reference's (046, 047):
      // with Loop on the clip repeats [Position, Position + Length) and the note editor's brace is the loop.
      // hunt-clips-1119: a clip looped by an edge drag with the loop key repeats its notes' own length
      // (loopLength 0): that is a loop too, shown lit with that window (it read as off, its Position and
      // Length hidden, and a click "turned on" a loop as long as the clip: the repeats were gone).
      const LoopWindow lwin = clipLoopWindow(s, *sel);
      const bool looping = sel->loop && lwin.len > 0;
      // Checkpoint 58 (N57-08): the same Loop toggle as the audio clip view's, beside Length (no label over it).
      const float loopW = columnField(a, "", 1, 2);
      if (ui::TextButton(i18n::tr("Loop"), looping, loopW)) {
        const Tick ls = start, ll = std::max<Tick>(six, end - start);
        a.doc.editClip(cid, looping ? "clip loop off" : "clip loop on", [looping, ls, ll](Clip& x) {
          if (looping) x.loop = false, x.loopStart = 0, x.loopLength = 0;
          else x.loop = true, x.loopStart = ls, x.loopLength = ll;  // the loop starts as the clip's region
        });
      }
      ui::Hint("Loop: the clip repeats its loop (Position and Length) for as long as it plays");
      if (looping) {
        Tick lpos = lwin.from, llen = lwin.len;
        const float pw = columnField(a, i18n::tr("Position"), 0, 2);
        if (positionField(a, "##clpos", lpos, 0, std::max<Tick>(0, maxEnd - llen), pw, "Where the loop starts in the clip's notes: drag, wheel or arrows, double-click to type"))
          a.doc.editClip(cid, "clip loop position", [lpos, llen](Clip& x) { x.loopStart = std::max<Tick>(0, lpos), x.loopLength = llen; }, ImGui::GetID("##clpos"));
        const float plw = columnField(a, i18n::tr("Length"), 1, 2);
        if (positionField(a, "##cllen", llen, six, maxEnd, plw, "The loop's length (bars. beats. sixteenths)", true))
          a.doc.editClip(cid, "clip loop length", [lpos, llen, six](Clip& x) { x.loopStart = lpos, x.loopLength = std::max<Tick>(six, llen); }, ImGui::GetID("##cllen"));
      }
    }
    const float lenW = sel ? 0.0f : columnField(a, i18n::tr("Length"), 0, 2);
    if (!sel) {
      const float bw = ImGui::GetFrameHeight();
      char len[32];
      std::snprintf(len, sizeof len, "%d %s", pat->bars, pat->bars == 1 ? i18n::tr("bar") : i18n::tr("bars"));
      // Checkpoint 48: the notes' length; a clip that played the whole pattern goes on playing all of it
      // (before, a longer pattern still played only its first bars in a grid clip).
      auto setBars = [&a, &s](const Pattern& pp, int bars) {
        const Tick oldLen = s.patternContentLength(pp);
        std::vector<ObjectId> follow;
        for (const Clip& c : s.playlist)
          if (c.kind == ClipKind::Pattern && c.pattern == pp.id && c.offset == 0 && c.length == oldLen) follow.push_back(c.id);
        const ObjectId pid = pp.id;
        a.doc.beginGroup("pattern length");
        a.doc.setPatternLength(pid, bars, pp.stepsPerBar);
        if (const Pattern* np = a.doc.song().pattern(pid)) {
          const Tick newLen = a.doc.song().patternContentLength(*np);
          for (ObjectId id : follow) a.doc.editClip(id, "pattern length", [newLen](Clip& x) { x.length = newLen; });
        }
        a.doc.endGroup();
      };
      if (ImGui::Button("-", ImVec2(bw, 0)) && pat->bars > 1) setBars(*pat, pat->bars - 1);
      ui::Hint("One bar shorter");
      ImGui::SameLine(0, 2);
      const ImVec2 lp = ImGui::GetCursorScreenPos();
      const float lw = std::max(20.0f, lenW - bw * 2 - 4);
      ImGui::Dummy(ImVec2(lw, bw));
      ImGui::GetWindowDrawList()->AddRectFilled(lp, ImVec2(lp.x + lw, lp.y + bw), P.field, theme::kRadiusSm);
      ImGui::GetWindowDrawList()->AddRect(lp, ImVec2(lp.x + lw, lp.y + bw), ImGui::GetColorU32(ImGuiCol_Border), theme::kRadiusSm, 1.0f, 0);  // round 23: the fields' rim
      theme::centreText(ImGui::GetWindowDrawList(), a.fontRegular, theme::kFontLabel + 1, lp, ImVec2(lp.x + lw, lp.y + bw), P.text, len);
      ImGui::SameLine(0, 2);
      if (ImGui::Button("+", ImVec2(bw, 0)) && pat->bars < 512) setBars(*pat, pat->bars + 1);
      ui::Hint("One bar longer");
    }
    const float meterW = columnField(a, i18n::tr("Signature"), sel ? 0 : 1, 2);  // N50-05: a row of its own under Loop  // checkpoint 48: the reference's word (456)
    {
      char m[32];
      if (pat->ownMeter()) std::snprintf(m, sizeof m, "%d / %d", pat->meterNum, pat->meterDen);
      else std::snprintf(m, sizeof m, "%d / %d", s.timeSigNum, s.timeSigDen);  // N46-10: numbers, as the reference
      valueBox(a, m, meterW);  // K-112c (round 23): boxed, as every ref's clip values (038, 041, 045, 112)
      ui::Hint(pat->ownMeter() ? "The pattern's own meter (change it from the piano roll's toolbar)"
                               : "The song's meter (give the pattern its own from the piano roll's toolbar)");
    }
    ImGui::PopID();
  }
  if (twoCols) {
    columnScrollEdges();
    ImGui::EndChild();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x + kW - 1.5f, p0.y), ImVec2(p0.x + kW - 1.5f, p0.y + av.y), P.line);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + kW, p0.y));
    beginColumn("##clipcol2");
  } else {
    ImGui::Dummy(ImVec2(0, 4));
  }
  // Launch: how the clip's track starts and ends its clips (the same settings as the track's menu).
  // N46-10: only for the grid's clips; an arrangement clip plays where it sits, so the reference
  // shows no launch settings for it.
  const bool launchOpen = gridView(a) && columnHeading(a, i18n::tr("Launch"), 0, 0, "##foldlaunch");
  if (!gridView(a) || !launchOpen) {
    // folded: only the title bar
  } else if (track < 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textFaint));
    ImGui::TextWrapped("%s", i18n::tr("Select a track to set how its clips launch"));
    ImGui::PopStyleColor();
  } else {
    if (a.playlist.selected.size() == 1 && clipLaunchBox(a, *a.playlist.selected.begin(), false)) {  // grid2-1118: the clip's own
      columnHeading(a, i18n::tr("Track launch"), 0, 0, nullptr);
      ImGui::Dummy(ImVec2(0, 2));
    }
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(track)];
    auto edit = [&](const char* label, auto fn) { a.doc.editPlaylistTrack(track, label, fn); };
    ImGui::PushID(1000 + track);
    const float wpad = columnField(a, i18n::tr("Press"), 0, 2);
    if (ui::BeginCombo("##pad", padModeName(t.pad), wpad)) {
      for (int i = 0; i < static_cast<int>(PadMode::Count); ++i)
        if (ImGui::Selectable(padModeName(static_cast<PadMode>(i)), t.pad == static_cast<PadMode>(i)))
          edit("change pad mode", [i](PlaylistTrack& x) { x.pad = static_cast<PadMode>(i); });
      ImGui::EndCombo();
    }
    ui::Hint("What a press on a clip does");
    const float wfollow = columnField(a, i18n::tr("At its end"), 1, 2);
    if (ui::BeginCombo("##follow", followModeName(t.follow), wfollow)) {
      for (int i = 0; i < static_cast<int>(FollowMode::Count); ++i)
        if (ImGui::Selectable(followModeName(static_cast<FollowMode>(i)), t.follow == static_cast<FollowMode>(i)))
          edit("change clip end action", [i](PlaylistTrack& x) { x.follow = static_cast<FollowMode>(i); });
      ImGui::EndCombo();
    }
    ui::Hint("What happens when a launched clip reaches its end");
    const float wlaunch = columnField(a, i18n::tr("Starts on"), 0, 1);  // full width: its grids read in full
    // Checkpoint 59 (N58-12): the control bar's words ("1 Bar", not "Next 4 beats").
    if (ui::BeginCombo("##launch", gridName(t.launch), wlaunch)) {
      LaunchGrid g = t.launch;
      if (gridItems(t.launch, g, true)) {
        edit("change launch grid", [g](PlaylistTrack& x) { x.launch = g; });
      }
      ImGui::EndCombo();
    }
    ui::Hint("The grid a launched clip waits for before it starts");
    const float wphase = columnField(a, i18n::tr("In phase"), 0, 1);
    if (ui::BeginCombo("##phase", t.syncStart == LaunchGrid::Now ? i18n::tr("Off") : gridName(t.syncStart), wphase)) {
      if (ImGui::Selectable(i18n::tr("Off"), t.syncStart == LaunchGrid::Now)) edit("change phase", [](PlaylistTrack& x) { x.syncStart = LaunchGrid::Now; });
      for (int i = 1; i < kLaunchGridsBasic; ++i) {
        if (static_cast<LaunchGrid>(i) == LaunchGrid::Bar) {
          continue;  // (the 4 beats grid is listed as 1 Bar already)
        }
        if (ImGui::Selectable(gridName(static_cast<LaunchGrid>(i)), t.syncStart == static_cast<LaunchGrid>(i)))
          edit("change phase", [i](PlaylistTrack& x) { x.syncStart = static_cast<LaunchGrid>(i); });
      }
      ImGui::EndCombo();
    }
    ui::Hint("Off: a launched clip starts from its start. A grid: it starts in phase with the song on that grid");
    ImGui::PopID();
  }
  columnScrollEdges();
  ImGui::EndChild();
  const float colsW = twoCols ? 2 * kW : kW;
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x + colsW - 1.5f, p0.y), ImVec2(p0.x + colsW - 1.5f, p0.y + av.y), P.line);
  ImGui::SetCursorScreenPos(p0);
  return colsW;
}

bool clipViewTwoColumns(float bandW) { return bandW >= kClipColumnW * 2 + 560.0f; }

// ---- grid2-1118: the edit grid keys (Cmd+1 .. 5) and the global quantisation keys (Cmd+6 .. 0) ----
namespace {
struct EditGridState {
  bool on = false;       // a key set it (else the editor's own snap menu rules)
  bool fixed = false;    // fixed size, else adaptive to the zoom
  int density = 0;       // adaptive: narrower (+) or wider (-) lines
  int size = 7;          // fixed: kSizes index (1/16)
  bool triplet = false;
  bool snap = true;
  Snap menuSnap = Snap::Line;  // the snap menu's value when a key took over
  bool menuKnown = false;
  Tick shown = 0;        // the grid last worked out (shown in the status bar)
};
EditGridState gEdit[2];
// The fixed sizes, coarsest first: 8, 4, 2, 1 bars, then 1/2 .. 1/32 of a whole note.
Tick fixedSize(const Song& s, int i) {
  const Tick bar = std::max<Tick>(1, s.barTicks()), whole = std::max<Tick>(1, static_cast<Tick>(s.ppq) * 4);
  switch (i) {
    case 0: return bar * 8;
    case 1: return bar * 4;
    case 2: return bar * 2;
    case 3: return bar;
    default: return std::max<Tick>(1, whole >> (i - 3));
  }
}
constexpr int kFixedSizes = 9;
int editorOf() { return keys::focusContext() == keys::Context::PianoRoll ? 1 : 0; }
}  // namespace

void editGrid(const App& a, int which, Snap snap, double pxPerTick, Tick& visGrid, SnapGrid& snapG, SnapGrid& effGrid) {
  EditGridState& e = gEdit[std::clamp(which, 0, 1)];
  if (!active(a) || !e.on) {
    e.shown = effGrid;
    return;
  }
  if (!e.menuKnown) {
    e.menuSnap = snap, e.menuKnown = true;
  }
  if (snap != e.menuSnap) {  // a pick in the snap menu: the menu's grid again
    e = EditGridState{};
    e.shown = effGrid;
    return;
  }
  const Song& s = a.doc.song();
  Tick g = 0;
  if (e.fixed) {
    g = fixedSize(s, std::clamp(e.size, 0, kFixedSizes - 1));
  } else {
    const double minPx = 16.0 * std::pow(2.0, -static_cast<double>(e.density));
    g = fixedSize(s, 0);
    for (int i = kFixedSizes - 1; i >= 0; --i) {
      if (static_cast<double>(fixedSize(s, i)) * pxPerTick >= minPx) {
        g = fixedSize(s, i);
        break;
      }
    }
  }
  if (e.triplet) {
    g = std::max<Tick>(1, g * 2 / 3);
  }
  visGrid = g;
  effGrid = SnapGrid(g);
  const bool alt = ImGui::GetIO().KeyAlt;  // Alt inverts snapping, as before
  snapG = SnapGrid(e.snap != alt ? g : 1);
  e.shown = g;
}

std::string gridLabel(const App& a, int which) {
  const EditGridState& e = gEdit[std::clamp(which, 0, 1)];
  const Song& s = a.doc.song();
  const Tick g = e.shown;
  if (g <= 1) {
    return i18n::tr("Grid off");
  }
  const Tick bar = std::max<Tick>(1, s.barTicks()), whole = std::max<Tick>(1, static_cast<Tick>(s.ppq) * 4);
  const Tick base = e.on && e.triplet ? g * 3 / 2 : g;
  std::string n;
  if (base >= bar && base % bar == 0) {
    n = base == bar ? i18n::tr("1 Bar") : std::to_string(base / bar) + i18n::tr(" Bars");
  } else if (whole % base == 0) {
    n = "1/" + std::to_string(whole / base);
  } else {
    n = std::to_string(g) + i18n::tr(" ticks");
  }
  if (e.on && e.triplet) {
    n += "T";
  }
  std::string out = std::string(i18n::tr("Grid ")) + n;
  if (e.on) {
    out += e.fixed ? i18n::tr(" (fixed)") : i18n::tr(" (adaptive)");
    if (!e.snap) {
      out += i18n::tr(", snap off");
    }
  }
  return out;
}

void gridKeys(App& a) {
  struct Q {
    const char* id;
    LaunchGrid g;
  };
  static const Q kQ[] = {{"clips.quant16", LaunchGrid::Step},
                         {"clips.quant8", LaunchGrid::HalfBeat},
                         {"clips.quant4", LaunchGrid::Beat},
                         {"clips.quantBar", LaunchGrid::Bar},
                         {"clips.quantNone", LaunchGrid::Now}};
  for (const Q& q : kQ) {
    if (keys::pressed(q.id)) {
      const LaunchGrid g = q.g;
      a.doc.editSong("change launch grid", [g](Song& x) {
        for (PlaylistTrack& t : x.tracks) {
          t.launch = g;
        }
      });
      a.notify(std::string(i18n::tr("Global quantisation: ")) + gridName(g));
    }
  }
  const bool narrower = keys::pressed("clips.gridNarrower"), wider = keys::pressed("clips.gridWider");
  const bool triplet = keys::pressed("clips.gridTriplet"), snap = keys::pressed("clips.gridSnap"), fixed = keys::pressed("clips.gridFixed");
  if (!narrower && !wider && !triplet && !snap && !fixed) {
    return;
  }
  const int which = editorOf();
  EditGridState& e = gEdit[which];
  if (!e.on) {  // a key takes over from the snap menu: adaptive, as the menu's line grid looks now
    const Tick was = e.shown;
    e = EditGridState{};
    e.on = true;
    for (int i = 0; i < kFixedSizes; ++i) {
      if (fixedSize(a.doc.song(), i) == was) {
        e.size = i;
      }
    }
  }
  if (narrower) {
    if (e.fixed) {
      e.size = std::min(kFixedSizes - 1, e.size + 1);
    } else {
      e.density = std::min(3, e.density + 1);
    }
  }
  if (wider) {
    if (e.fixed) {
      e.size = std::max(0, e.size - 1);
    } else {
      e.density = std::max(-3, e.density - 1);
    }
  }
  if (triplet) {
    e.triplet = !e.triplet;
  }
  if (snap) {
    e.snap = !e.snap;
  }
  if (fixed) {
    if (!e.fixed) {  // the size shown now stays
      const Tick g = e.triplet ? e.shown * 3 / 2 : e.shown;
      for (int i = 0; i < kFixedSizes; ++i) {
        if (fixedSize(a.doc.song(), i) == g) {
          e.size = i;
        }
      }
    }
    e.fixed = !e.fixed;
  }
}


// grid2-1118: a grid clip's own launch settings (Clip::launchSet), over its track's: launch mode, launch
// quantisation ("Global": the track's grid, which the control bar sets on every track), legato, and its
// follow actions (A and B with their chances, after a time or the clip's length). Each change is one
// undo step. Nothing for an arrangement clip (it plays where it sits).
const char* followActionName(FollowAction f) {
  static const char* const kNames[] = {i18n::N_("None"), i18n::N_("Stop"),  i18n::N_("Play again"), i18n::N_("Previous"), i18n::N_("Next"),
                                       i18n::N_("First"), i18n::N_("Last"), i18n::N_("Any"),        i18n::N_("Other"),    i18n::N_("Jump")};
  const int i = static_cast<int>(f);
  return i >= 0 && i < static_cast<int>(FollowAction::Count) ? i18n::tr(kNames[i]) : "";
}
namespace {
bool followCombo(const char* id, FollowAction& f, float w) {
  bool got = false;
  if (ui::BeginCombo(id, followActionName(f), w)) {
    for (int i = 0; i < static_cast<int>(FollowAction::Count); ++i) {
      if (ImGui::Selectable(followActionName(static_cast<FollowAction>(i)), f == static_cast<FollowAction>(i))) {
        f = static_cast<FollowAction>(i);
        got = true;
      }
    }
    ImGui::EndCombo();
  }
  return got;
}
bool percentField(const char* id, uint8_t& v, float w) {
  int x = v;
  ImGui::SetNextItemWidth(w);
  if (ImGui::DragInt(id, &x, 0.5f, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp) && x != v) {
    v = static_cast<uint8_t>(std::clamp(x, 0, 100));
    return true;
  }
  return false;
}
bool sceneIndexField(const char* id, int& v, float w) {  // a Jump target: the scene's number (1 up)
  int x = v + 1;
  ImGui::SetNextItemWidth(w);
  if (ImGui::DragInt(id, &x, 0.2f, 1, 999, "%d", ImGuiSliderFlags_AlwaysClamp) && x - 1 != v) {
    v = std::clamp(x, 1, 999) - 1;
    return true;
  }
  return false;
}
}  // namespace

// The follow action fields (a clip's or a scene's): On, A / B with their chances and Jump targets, the time.
bool followFields(App& a, FollowSpec& f, bool clip, Tick clipLen) {
  bool ch = false;
  bool on = f.on;
  if (ImGui::Checkbox(i18n::tr("Follow action"), &on)) {
    f.on = on;
    ch = true;
  }
  ui::Hint(clip ? "Follow actions: after the follow time the clip goes on to another clip of its group (the run of filled slots it is in)"
                : "Scene follow actions: after the follow time another scene launches");
  if (!f.on) {
    return ch;
  }
  const float wa = columnField(a, i18n::tr("Action A"), 0, 2);
  ch = followCombo("##fa", f.a, wa) || ch;
  const float wb = columnField(a, i18n::tr("Action B"), 1, 2);
  ch = followCombo("##fb", f.b, wb) || ch;
  const float wca = columnField(a, i18n::tr("Chance A"), 0, 2);
  ch = percentField("##fca", f.chanceA, wca) || ch;
  ui::Hint("How likely action A is (against B's chance)");
  const float wcb = columnField(a, i18n::tr("Chance B"), 1, 2);
  ch = percentField("##fcb", f.chanceB, wcb) || ch;
  if (f.a == FollowAction::Jump || f.b == FollowAction::Jump) {
    if (f.a == FollowAction::Jump) {
      const float wj = columnField(a, i18n::tr("A jumps to"), 0, 2);
      ch = sceneIndexField("##fja", f.jumpA, wj) || ch;
    }
    if (f.b == FollowAction::Jump) {
      const float wj = columnField(a, i18n::tr("B jumps to"), f.a == FollowAction::Jump ? 1 : 0, 2);
      ch = sceneIndexField("##fjb", f.jumpB, wj) || ch;
    }
    ui::Hint("The scene (its number) a Jump goes to");
  }
  if (clip) {
    bool linked = f.linked;
    if (ImGui::Checkbox(i18n::tr("Linked to the clip's length"), &linked)) {
      f.linked = linked;
      ch = true;
    }
    ui::Hint("Linked: the follow action comes after the clip has played this many times");
  }
  if (clip && f.linked) {
    const float wl = columnField(a, i18n::tr("Times played"), 0, 2);
    int n = f.loops;
    ImGui::SetNextItemWidth(wl);
    if (ImGui::DragInt("##floops", &n, 0.1f, 1, 999, "%d", ImGuiSliderFlags_AlwaysClamp) && n != f.loops) {
      f.loops = std::clamp(n, 1, 999);
      ch = true;
    }
    (void)clipLen;
  } else {
    const float wt = columnField(a, i18n::tr("Follow time"), 0, 1);
    const Song& s = a.doc.song();
    Tick t = f.time > 0 ? f.time : std::max<Tick>(1, s.barTicks());
    if (positionField(a, "##ftime", t, std::max<Tick>(1, s.ppq / 4), std::max<Tick>(1, s.barTicks()) * 999, wt,
                      "The follow action's time (bars. beats. sixteenths) from the start", true)) {
      f.time = t;
      ch = true;
    }
  }
  return ch;
}

bool clipLaunchBox(App& a, ObjectId id, bool heading) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c || c->start >= s.live.areaEnd || !gridView(a)) {
    return false;
  }
  if (heading && !columnSection(a, i18n::tr("Launch"), 0, "##foldalaunch")) {
    return true;
  }
  return clipLaunchParts(a, id, kLaunchPartLaunch | kLaunchPartFollow);
}

bool clipLaunchParts(App& a, ObjectId id, int parts) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c || c->start >= s.live.areaEnd) {
    return false;
  }
  const ClipLaunch cur = c->launchSet;
  ClipLaunch to = cur;
  const PlaylistTrack* tr = c->track >= 0 && c->track < static_cast<int>(s.tracks.size()) ? &s.tracks[static_cast<size_t>(c->track)] : nullptr;
  ImGui::PushID(static_cast<int>(id) + 0x6C00000);
  if (parts & kLaunchPartLaunch) {
    const float wm = columnField(a, i18n::tr("Launch mode"), 0, 2);
    const std::string trackMode = std::string(i18n::tr("Track's")) + (tr ? std::string(" (") + i18n::tr(padModeName(tr->pad)) + ")" : std::string());
    if (ui::BeginCombo("##cmode", to.mode < 0 ? trackMode.c_str() : i18n::tr(padModeName(static_cast<PadMode>(to.mode))), wm)) {
      if (ImGui::Selectable(trackMode.c_str(), to.mode < 0)) {
        to.mode = -1;
      }
      for (int i = 0; i < static_cast<int>(PadMode::Count); ++i) {
        if (ImGui::Selectable(i18n::tr(padModeName(static_cast<PadMode>(i))), to.mode == i)) {
          to.mode = static_cast<int8_t>(i);
        }
      }
      ImGui::EndCombo();
    }
    ui::Hint("What a press on this clip does: Trigger starts it, Gate plays it while held, Toggle starts and stops it, Repeat retriggers it on its grid while held");
    const float wq = columnField(a, i18n::tr("Quantisation"), 1, 2);
    if (ui::BeginCombo("##cquant", to.quant < 0 ? i18n::tr("Global") : gridName(static_cast<LaunchGrid>(to.quant)), wq)) {
      if (ImGui::Selectable(i18n::tr("Global"), to.quant < 0)) {
        to.quant = -1;
      }
      LaunchGrid g = to.quant < 0 ? LaunchGrid::Count : static_cast<LaunchGrid>(to.quant);
      if (gridItems(g, g, true)) {
        to.quant = static_cast<int8_t>(g);
      }
      ImGui::EndCombo();
    }
    ui::Hint("The grid this clip waits for when launched (Global: its track's, set for every track in the control bar)");
    bool legato = to.legato;
    if (ImGui::Checkbox(i18n::tr("Legato"), &legato)) {
      to.legato = legato;
    }
    ui::Hint("Legato: launched while another clip of its track plays, it takes over at that clip's play position");
  }
  FollowSpec f = to.follow;
  if ((parts & kLaunchPartFollow) && followFields(a, f, true, s.clipLength(*c))) {
    to.follow = f;
  }
  if (parts & kLaunchPartFollow) {
    bool all = a.engine.followActions();
    // hunt-clips2-1119: a label that fits the Launch column (it was cut off mid-word); the hint says the rest.
    if (ImGui::Checkbox(i18n::tr("All follow actions"), &all)) {
      a.engine.setFollowActions(all);
      a.settings.set("clips.followActions", all);
    }
    ui::Hint("Follow actions on for every clip and scene (off: none of them runs)");
  }
  ImGui::PopID();
  if (!(to == cur)) {
    a.doc.editClip(id, "clip launch settings", [to](Clip& x) { x.launchSet = to; });
  }
  ImGui::Dummy(ImVec2(0, 4));
  return true;
}

std::string clipLaunchSummary(const App& a, ObjectId id, bool follow) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c) {
    return std::string();
  }
  const ClipLaunch& l = c->launchSet;
  if (follow) {
    const FollowSpec& f = l.follow;
    if (!f.on) {
      return i18n::tr("off");
    }
    std::string out = followActionName(f.a);
    if (f.b != FollowAction::None && f.chanceB > 0) {
      out += std::string(" / ") + followActionName(f.b);
    }
    if (f.linked) {
      out += ", " + std::to_string(f.loops) + "x";
    }
    return out;
  }
  const PlaylistTrack* tr = c->track >= 0 && c->track < static_cast<int>(s.tracks.size()) ? &s.tracks[static_cast<size_t>(c->track)] : nullptr;
  const char* mode = l.mode >= 0 ? padModeName(static_cast<PadMode>(l.mode)) : tr ? padModeName(tr->pad) : "";
  std::string out = i18n::tr(mode);
  out += ", ";
  out += l.quant < 0 ? i18n::tr("Global") : gridName(static_cast<LaunchGrid>(l.quant));
  return out;
}

// -------------------------------------------------------------- clip grid ----

namespace {

// The tracks the grid shows as columns: every track up to the last one in use (clips, a link or a name).
// K-112c (round 39): the arrangement's rows in Clips are the same tracks (core/TrackModes.h).
int gridColumns(const Song& s) { return trackmodes::usedTrackCount(s); }

Rgb clipColour(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Pattern) {
    if (const Pattern* p = s.pattern(c.pattern)) return p->color;
  } else if (c.kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c.source)) return ch->color;
  } else if (const AutomationClip* au = s.automation(c.source)) {
    return au->color;
  }
  return 0x50565E;
}

std::string clipLabel(const Song& s, const Clip& c) {
  if (c.kind == ClipKind::Pattern) {
    if (const Pattern* p = s.pattern(c.pattern)) return p->name;
  } else if (c.kind == ClipKind::Audio) {
    if (const Channel* ch = s.channel(c.source)) return ch->name;
  } else if (const AutomationClip* au = s.automation(c.source)) {
    return au->name;
  }
  return std::string();
}

// K-112c (round 44): a clip picked in the grid brings its channel along, so the piano roll's channel
// picker and the clip band name the same part: a pattern clip keeps the selected channel when it
// plays in the pattern, else takes the pattern's first channel that does; an audio clip its own.
void selectClipChannel(App& a, const Clip& c) {
  const Song& s = a.doc.song();
  if (c.kind == ClipKind::Audio) {
    if (s.channel(c.source)) a.selectChannel(c.source);
    return;
  }
  const Pattern* p = c.kind == ClipKind::Pattern ? s.pattern(c.pattern) : nullptr;
  if (!p) return;
  auto plays = [&](ObjectId ch) {
    if (auto it = p->notes.find(ch); it != p->notes.end() && !it->second.empty()) return true;
    if (auto it = p->steps.find(ch); it != p->steps.end())
      for (uint8_t v : it->second)
        if (v) return true;
    return false;
  };
  if (plays(a.selChannel)) return;
  for (const Channel& ch : s.channels)
    if (plays(ch.id)) {
      a.selectChannel(ch.id);
      return;
    }
  // N46-02: an empty clip: its track's instrument (or note-only channel), so its notes can be drawn.
  if (const Channel* lc = trackmodes::linkedChannel(s, c.track)) a.selectChannel(lc->id);
}

// grid-1118: the clip grid plays over the arrangement (song mode: the arrangement plays on in the tracks
// the grid has not taken; AudioEngine::setSessionOverArrangement, set every Clips frame).
void launch(App& a, ObjectId clip) {
  if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
  a.engine.launchClip(clip);
}
// grid2-1118: a scene launched as one by the engine (its clips on their own launch settings, its tempo,
// time signature and follow actions); a row past the stored scenes stops the tracks that keep their stop
// button in it.
void launchSceneRow(App& a, int k, int nScenes) {
  if (a.engine.mode() != PlayMode::Song) {
    a.engine.setMode(PlayMode::Song);
  }
  if (k < nScenes) {
    a.engine.launchScene(k);
    return;
  }
  const Song& s = a.doc.song();
  for (int t = 0; t < static_cast<int>(s.tracks.size()) && t < kMaxPlaylistTracks; ++t) {
    if (trackmodes::hasStopButton(s, t, k)) {
      a.engine.stopLiveTrack(t);
    }
  }
}


bool gGridAsked = false;  // K-287 (dev): the next drawn clip grid prints its slots
float gGridScrollX = 0.0f;  // C44-28: the clip grid's horizontal scroll (the mixer band under it follows)

// ---- K-112c (C44-11, C44-12, C44-13, C44-14, C44-15): the grid's own selection and slot edits ----
// The selected slot (track, scene); a press on a clip selects it without launching it (its launch
// triangle launches), and the grid's keys work on the selected slot. Every edit is a document edit
// (one undo step, collaborators get it); the live area grows (the arrangement after it moves on,
// out of sight in Clips) when a clip lands in a row past its end.
struct GridSel {
  int t = -1, k = -1;
};
GridSel gSel;
// grid2-1118: the further selected slots (a rubber band, Shift-click range, Cmd-click), beside gSel (the
// keyboard slot); the slot a Shift-click range counts from.
std::set<std::pair<int, int>> gMulti;
GridSel gAnchor;
void clearGridSel() {
  gSel = {};
  gMulti.clear();
}
int selectedScene() { return gSel.k; }
// Checkpoint 60 (N59-07): a scene picked by a click on its launch cell (it launches too), as the
// reference selects a scene: Cmd+R renames it while nothing else was selected since.
int gSceneSel = -1;
bool sceneSelValid(const App& a) {
  return gSceneSel >= 0 && gSel.t < 0 && gSel.k < 0 && a.playlist.selected.empty() && a.playlist.trackSel.empty();
}
int gSelFocus = 0;  // C44-12: frames the grid keeps the keyboard after a selection (the clip band appearing takes it)
std::vector<Clip> gClipboard;     // a slot's clips, copied (start relative to the scene's start)

Tick sceneTicks(const Song& s) { return trackmodes::sceneTicksOf(s); }  // grid2-1118: a frozen length when the meter changed

// Playlist indices of the live clips on track t starting in scene k.
std::vector<int> slotClips(const Song& s, int t, int k) {
  std::vector<int> out;
  const Tick st = sceneTicks(s), a0 = static_cast<Tick>(k) * st, a1 = a0 + st;
  if (t < 0 || k < 0) return out;
  for (int ci : liveClips(s, t)) {
    const Clip& c = s.playlist[static_cast<size_t>(ci)];
    if (c.start >= a0 && c.start < a1) out.push_back(ci);
  }
  return out;
}

// The live area reaches the end of scene k at least.
void ensureScene(App& a, int k) { ops::extendLiveArea(a.doc, static_cast<Tick>(k + 1) * sceneTicks(a.doc.song())); }

void selectSlot(App& a, int t, int k) {
  gSel = {t, k};
  gMulti.clear();  // grid2-1118: a plain selection is one slot
  gAnchor = {t, k};
  gSelFocus = 3;
  a.playlist.trackSel = {t};  // C47: as the reference, a slot's track is the selected track
  const Song& s = a.doc.song();
  const std::vector<int> in = slotClips(s, t, k);
  if (in.empty()) {
    a.playlist.selected.clear();  // an empty slot: the clip band empties (C44-26)
    trackChosen();                // 0.7.8: and the detail spot shows the track's devices
    return;
  }
  const Clip& c = s.playlist[static_cast<size_t>(in.front())];
  a.playlist.selected = {c.id};  // the clip band shows the clip (its notes, or an audio clip's sample)
  clipChosen();                  // 0.7.8: in the detail spot, also when it was the clip selected already
  if (c.kind == ClipKind::Pattern) a.selectPattern(c.pattern);
  selectClipChannel(a, c);
}

void copySlot(App& a, int t, int k) {
  const Song& s = a.doc.song();
  gClipboard.clear();
  const Tick at = static_cast<Tick>(k) * sceneTicks(s);
  for (int ci : slotClips(s, t, k)) {
    Clip c = s.playlist[static_cast<size_t>(ci)];
    c.start -= at;
    gClipboard.push_back(c);
  }
}

bool deleteSlot(App& a, int t, int k, const char* label = "delete clip") {
  const std::vector<int> in = slotClips(a.doc.song(), t, k);
  if (in.empty() || a.doc.song().tracks[static_cast<size_t>(t)].locked) return false;
  std::vector<ObjectId> ids;
  for (int ci : in) ids.push_back(a.doc.song().playlist[static_cast<size_t>(ci)].id);
  a.doc.beginGroup(label);
  for (ObjectId id : ids) a.doc.removeClip(id);
  a.doc.endGroup();
  a.playlist.selected.clear();
  return true;
}

// Puts copies of `clips` (starts relative to the scene) into slot (t, k), replacing what is there.
// Returns false when the track does not take them.
bool placeInSlot(App& a, const std::vector<Clip>& clips, int t, int k, const char* label) {
  const Song& s = a.doc.song();
  if (clips.empty() || t < 0 || t >= static_cast<int>(s.tracks.size()) || k < 0 || s.tracks[static_cast<size_t>(t)].locked) return false;
  for (const Clip& c : clips)
    // (hunt-clips-1119: nor a MIDI clip into an audio track's slot, or audio into a MIDI track's: N50-02)
    if (!trackmodes::clipAllowed(s, c.kind, t) || !clipFitsTrack(s, c.kind, t) || (c.kind != ClipKind::Automation && trackmodes::isGroupTrack(s, t))) {
      return false;  // checkpoint 52: never onto a group
    }
  a.doc.beginGroup(label);
  ensureScene(a, k);
  std::set<ObjectId> old;
  for (int ci : slotClips(a.doc.song(), t, k)) old.insert(a.doc.song().playlist[static_cast<size_t>(ci)].id);
  const Tick at = static_cast<Tick>(k) * sceneTicks(a.doc.song());
  std::set<ObjectId> added;
  a.doc.editSong(label, [&](Song& x) {
    x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(), [&](const Clip& c) { return old.count(c.id) > 0; }), x.playlist.end());
    for (Clip c : clips) {
      c.id = x.allocateId();
      c.start = at + std::max<Tick>(0, c.start);
      c.track = t;
      c.takeGroup = kNoId, c.take = 0;
      x.playlist.push_back(c);
      added.insert(c.id);
    }
  });
  a.doc.endGroup();
  selectSlot(a, t, k);
  return true;
}

// Inserts an empty scene under scene k (every clip of a later scene moves down one row).
void insertScene(App& a, int k) {
  const Tick st = sceneTicks(a.doc.song());
  a.doc.editSong("insert scene", [&](Song& x) { trackmodes::insertScene(x, k, st); });  // checkpoint 59: names move with their scenes
}

// grid-1118: launches of clips just made (a captured scene): the engine takes a clip once the program
// that holds it is in, so each launch is sent again every frame until its track plays or queues it.
struct PendingLaunch {
  ObjectId clip = kNoId;
  int track = -1;
  bool legato = false;
  int frames = 90;
};
std::vector<PendingLaunch> gPending;
void pendingLaunchFrame(App& a) {
  for (size_t i = 0; i < gPending.size();) {
    PendingLaunch& p = gPending[i];
    if (a.engine.liveActive(p.track) == p.clip || a.engine.liveQueued(p.track) == p.clip || --p.frames <= 0 || !a.doc.song().clip(p.clip)) {
      gPending.erase(gPending.begin() + static_cast<std::ptrdiff_t>(i));
      continue;
    }
    if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
    a.engine.launchClip(p.clip, p.legato);
    ++i;
  }
}

// grid-1118: Capture and insert scene, as the reference's: a new scene under the selected one (else under
// the last scene in use) holding a copy of every clip playing now, launched at once in the playing clips'
// place, so playback goes on without a break. One undo step.
void captureSceneNow(App& a) {
  const Song& s0 = a.doc.song();
  const Tick st = sceneTicks(s0);
  const int nScenes = s0.live.areaEnd > 0 ? static_cast<int>((s0.live.areaEnd + st - 1) / st) : 0;
  const int k = gSel.k >= 0 ? gSel.k : gSceneSel >= 0 ? gSceneSel : std::max(0, nScenes - 1);
  struct Take {
    int t;
    Clip c;
  };
  std::vector<Take> takes;
  for (int t = 0; t < static_cast<int>(s0.tracks.size()) && t < kMaxPlaylistTracks; ++t) {
    const Clip* c = s0.clip(a.engine.liveActive(t));
    if (c && c->start < s0.live.areaEnd && c->track == t) {
      Clip x = *c;
      x.start = c->start % st;  // its place inside its scene
      takes.push_back({t, x});
    }
  }
  if (takes.empty()) {
    a.notify(i18n::tr("Nothing is playing in the clip grid to capture"));
    return;
  }
  a.doc.beginGroup("capture scene");
  insertScene(a, k);
  ensureScene(a, k + 1);
  const Tick at = static_cast<Tick>(k + 1) * sceneTicks(a.doc.song());
  std::vector<std::pair<int, ObjectId>> made;
  a.doc.editSong("capture scene", [&](Song& x) {
    for (const Take& tk : takes) {
      Clip c = tk.c;
      c.id = x.allocateId();
      c.start = at + c.start;
      c.track = tk.t;
      c.takeGroup = kNoId, c.take = 0;
      x.playlist.push_back(c);
      made.emplace_back(tk.t, c.id);
    }
  });
  a.doc.endGroup();
  for (const auto& [t, id] : made) gPending.push_back({id, t, true, 90});
  clearGridSel();
  a.playlist.selected.clear();
  gSceneSel = k + 1;
}

// Duplicates slot (t, k) into the slot under it (a scene is inserted when that one holds a clip).
void duplicateSlot(App& a, int t, int k) {
  if (slotClips(a.doc.song(), t, k).empty()) return;
  std::vector<Clip> keep = gClipboard;
  copySlot(a, t, k);
  std::vector<Clip> src = gClipboard;
  gClipboard = std::move(keep);
  a.doc.beginGroup("duplicate clip");
  if (!slotClips(a.doc.song(), t, k + 1).empty()) insertScene(a, k);
  placeInSlot(a, src, t, k + 1, "duplicate clip");
  a.doc.endGroup();
}


// ---- grid2-1118: several slots selected (a rubber band, Shift-click range, Cmd-click) and their edits ----
bool slotSelected(int t, int k) { return (gSel.t == t && gSel.k == k) || gMulti.count({t, k}) > 0; }
std::vector<std::pair<int, int>> selectedSlots() {
  std::set<std::pair<int, int>> all = gMulti;
  if (gSel.t >= 0 && gSel.k >= 0) {
    all.insert({gSel.t, gSel.k});
  }
  return {all.begin(), all.end()};
}
// The clip band and the arrangement's selection: every clip of the selected slots.
void syncMultiSel(App& a) {
  const Song& s = a.doc.song();
  a.playlist.selected.clear();
  std::set<int> tracks;
  for (const auto& [t, k] : selectedSlots()) {
    tracks.insert(t);
    for (int ci : slotClips(s, t, k)) {
      a.playlist.selected.insert(s.playlist[static_cast<size_t>(ci)].id);
    }
  }
  if (!tracks.empty()) {
    a.playlist.trackSel = tracks;
  }
}
void extendSelection(App& a, int t, int k) {  // Shift-click: the rectangle from the anchor
  const GridSel from = gAnchor.t >= 0 ? gAnchor : gSel.t >= 0 ? gSel : GridSel{t, k};
  gMulti.clear();
  for (int u = std::min(from.t, t); u <= std::max(from.t, t); ++u) {
    for (int j = std::min(from.k, k); j <= std::max(from.k, k); ++j) {
      gMulti.insert({u, j});
    }
  }
  gSel = {t, k};
  gAnchor = from;
  gSelFocus = 3;
  syncMultiSel(a);
}
void toggleSelection(App& a, int t, int k) {  // Cmd-click: adds or removes the slot
  if (gSel.t >= 0 && gSel.k >= 0) {
    gMulti.insert({gSel.t, gSel.k});
  }
  if (gMulti.count({t, k})) {
    gMulti.erase({t, k});
    gSel = gMulti.empty() ? GridSel{} : GridSel{gMulti.begin()->first, gMulti.begin()->second};
  } else {
    gMulti.insert({t, k});
    gSel = {t, k};
  }
  gAnchor = gSel;
  gSelFocus = 3;
  syncMultiSel(a);
}

// Several slots copied: each clip with its slot's place from the block's top-left slot.
struct BoardClip {
  int dt = 0, dk = 0;
  Clip c;
};
std::vector<BoardClip> gBoard;
std::vector<BoardClip> boardOf(const Song& s, const std::vector<std::pair<int, int>>& sel) {
  std::vector<BoardClip> out;
  if (sel.empty()) {
    return out;
  }
  int t0 = sel.front().first, k0 = sel.front().second;
  for (const auto& [t, k] : sel) {
    t0 = std::min(t0, t), k0 = std::min(k0, k);
  }
  const Tick st = sceneTicks(s);
  for (const auto& [t, k] : sel) {
    for (int ci : slotClips(s, t, k)) {
      BoardClip b{t - t0, k - k0, s.playlist[static_cast<size_t>(ci)]};
      b.c.start -= static_cast<Tick>(k) * st;
      out.push_back(b);
    }
  }
  return out;
}
// Copy: one slot as before; several: the board (paste lands the block with its top-left slot there).
void copySlots(App& a) {
  const std::vector<std::pair<int, int>> sel = selectedSlots();
  if (sel.size() <= 1) {
    gBoard.clear();
    if (!sel.empty()) {
      copySlot(a, sel.front().first, sel.front().second);
    }
    return;
  }
  gClipboard.clear();
  gBoard = boardOf(a.doc.song(), sel);
}
bool deleteSlots(App& a, const char* label) {
  const std::vector<std::pair<int, int>> sel = selectedSlots();
  if (sel.size() <= 1) {
    return !sel.empty() && deleteSlot(a, sel.front().first, sel.front().second, label);
  }
  const Song& s = a.doc.song();
  std::vector<ObjectId> ids;
  for (const auto& [t, k] : sel) {
    if (t < static_cast<int>(s.tracks.size()) && !s.tracks[static_cast<size_t>(t)].locked) {
      for (int ci : slotClips(s, t, k)) {
        ids.push_back(s.playlist[static_cast<size_t>(ci)].id);
      }
    }
  }
  if (ids.empty()) {
    return false;
  }
  a.doc.beginGroup(label);
  for (ObjectId id : ids) {
    a.doc.removeClip(id);
  }
  a.doc.endGroup();
  a.playlist.selected.clear();
  return true;
}
// The board's block pasted with its top-left slot at (t, k): each slot replaced (one undo step); the
// slots it fills are selected.
void pasteBoardAt(App& a, const std::vector<BoardClip>& board, int t, int k, const char* label) {
  std::map<std::pair<int, int>, std::vector<Clip>> bySlot;
  for (const BoardClip& b : board) {
    bySlot[{t + b.dt, k + b.dk}].push_back(b.c);
  }
  a.doc.beginGroup(label);
  std::set<std::pair<int, int>> done;
  for (const auto& [at, clips] : bySlot) {
    if (placeInSlot(a, clips, at.first, at.second, label)) {
      done.insert(at);
    }
  }
  a.doc.endGroup();
  if (done.empty()) {
    a.notify(i18n::tr("The clips do not fit there"), true);  // hunt-clips-1119: say why nothing was pasted
  }
  if (!done.empty()) {
    gMulti = done;
    gSel = {done.begin()->first, done.begin()->second};
    gAnchor = gSel;
    syncMultiSel(a);
  }
}
void pasteSlots(App& a, int t, int k) {
  if (!gBoard.empty()) {
    pasteBoardAt(a, gBoard, t, k, "paste clips");
  } else if (!gClipboard.empty()) {
    if (!placeInSlot(a, gClipboard, t, k, "paste clip")) {
      a.notify(i18n::tr("The clips do not fit there"), true);  // hunt-clips-1119: say why nothing was pasted
    }
  }
}
// Duplicate: one slot as before; several: the block is copied into new scenes under it.
void duplicateSlots(App& a) {
  const std::vector<std::pair<int, int>> sel = selectedSlots();
  if (sel.size() <= 1) {
    if (!sel.empty()) {
      duplicateSlot(a, sel.front().first, sel.front().second);
    }
    return;
  }
  int t0 = sel.front().first, k0 = sel.front().second, k1 = k0;
  for (const auto& [t, k] : sel) {
    t0 = std::min(t0, t), k0 = std::min(k0, k), k1 = std::max(k1, k);
  }
  const std::vector<BoardClip> board = boardOf(a.doc.song(), sel);
  if (board.empty()) {
    return;
  }
  a.doc.beginGroup("duplicate clips");
  for (int j = k0; j <= k1; ++j) {
    insertScene(a, k1);
  }
  pasteBoardAt(a, board, t0, k1 + 1, "duplicate clips");
  a.doc.endGroup();
}
// The selected slots moved (or copied, Alt) by dt tracks and dk scenes: their clips land in the slots
// there, whose clips are replaced. False when a slot would leave the grid or a track does not take it.
bool moveSlots(App& a, int dt, int dk, bool copy) {
  const Song& s = a.doc.song();
  const std::vector<std::pair<int, int>> sel = selectedSlots();
  const int nTracks = static_cast<int>(s.tracks.size());
  std::set<ObjectId> ids;
  std::set<std::pair<int, int>> to;
  for (const auto& [t, k] : sel) {
    const int nt = t + dt, nk = k + dk;
    if (nt < 0 || nt >= nTracks || nk < 0 || s.tracks[static_cast<size_t>(nt)].locked || (!copy && s.tracks[static_cast<size_t>(t)].locked)) {
      return false;
    }
    for (int ci : slotClips(s, t, k)) {
      const Clip& c = s.playlist[static_cast<size_t>(ci)];
      // (hunt-clips-1119: the kinds a track takes, as a single slot's drag: N50-02)
      if (!trackmodes::clipAllowed(s, c.kind, nt) || !clipFitsTrack(s, c.kind, nt) || (c.kind != ClipKind::Automation && trackmodes::isGroupTrack(s, nt))) {
        return false;
      }
      ids.insert(c.id);
    }
    to.insert({nt, nk});
  }
  if (ids.empty() || (dt == 0 && dk == 0)) {
    return false;
  }
  if (copy) {
    int t0 = sel.front().first, k0 = sel.front().second;
    for (const auto& [t, k] : sel) {
      t0 = std::min(t0, t), k0 = std::min(k0, k);
    }
    pasteBoardAt(a, boardOf(s, sel), t0 + dt, k0 + dk, "copy clips");
    return true;
  }
  std::set<ObjectId> gone;
  int kMax = 0;
  for (const auto& [nt, nk] : to) {
    kMax = std::max(kMax, nk);
    for (int ci : slotClips(s, nt, nk)) {
      if (!ids.count(s.playlist[static_cast<size_t>(ci)].id)) {
        gone.insert(s.playlist[static_cast<size_t>(ci)].id);
      }
    }
  }
  const Tick d = static_cast<Tick>(dk) * sceneTicks(s);
  a.doc.beginGroup("move clips");
  ensureScene(a, kMax);
  for (ObjectId id : gone) {
    a.doc.removeClip(id);
  }
  a.doc.editClips("move clips", [&](std::vector<Clip>& cs) {
    for (Clip& c : cs) {
      if (ids.count(c.id)) {
        c.start += d, c.track += dt;
      }
    }
  });
  a.doc.endGroup();
  gMulti = to;
  gSel = {to.begin()->first, to.begin()->second};
  gAnchor = gSel;
  syncMultiSel(a);
  return true;
}
// Cmd+E: the selected slots' stop buttons go (or come back: all of them, when the first had none).
void toggleStopButtons(App& a) {
  const std::vector<std::pair<int, int>> sel = selectedSlots();
  if (sel.empty()) {
    return;
  }
  const bool to = !trackmodes::hasStopButton(a.doc.song(), sel.front().first, sel.front().second);
  a.doc.editSong(to ? "add stop button" : "remove stop button", [&](Song& x) {
    for (const auto& [t, k] : sel) {
      trackmodes::setStopButton(x, t, k, to);
    }
  });
}
// Cmd+A: every slot with a clip.
void selectAllSlots(App& a) {
  const Song& s = a.doc.song();
  gMulti.clear();
  const Tick st = sceneTicks(s);
  for (const Clip& c : s.playlist) {
    if (c.start < s.live.areaEnd && c.track >= 0 && c.track < static_cast<int>(s.tracks.size())) {
      gMulti.insert({c.track, static_cast<int>(c.start / st)});
    }
  }
  if (gMulti.empty()) {
    return;
  }
  gSel = {gMulti.begin()->first, gMulti.begin()->second};
  gAnchor = gSel;
  gSelFocus = 3;
  syncMultiSel(a);
}

// ---- grid2-1118: grid clips dragged onto the Arrangement ----
// A slot drag that leaves the grid upward (or Tab during it) shows the Arrangement and carries the
// clips there as a drag (GL_GRIDCLIPS): dropped, copies land at the drop position, the slots' columns
// on the tracks from the one under the pointer, the scenes one after another (each scene's row as long
// as its longest clip), as one undo step.
struct GridDragClip {
  int dt = 0;
  Tick off = 0;
  Clip c;
};
std::vector<GridDragClip> gGridDrag;
int gGridDragGone = 0;
bool gSlotPressed = false;  // hunt-clips-1119: a press on a slot is held (its drag may go on to the Arrangement)
void beginGridDrag(App& a) {
  const Song& s = a.doc.song();
  const std::vector<std::pair<int, int>> sel = selectedSlots();
  gGridDrag.clear();
  if (sel.empty()) {
    return;
  }
  int t0 = sel.front().first, k0 = sel.front().second, k1 = k0;
  for (const auto& [t, k] : sel) {
    t0 = std::min(t0, t), k0 = std::min(k0, k), k1 = std::max(k1, k);
  }
  const Tick st = sceneTicks(s);
  Tick off = 0;
  for (int k = k0; k <= k1; ++k) {
    Tick rowLen = 0;
    for (const auto& [t, kk] : sel) {
      if (kk != k) {
        continue;
      }
      for (int ci : slotClips(s, t, k)) {
        GridDragClip g{t - t0, off, s.playlist[static_cast<size_t>(ci)]};
        g.c.start -= static_cast<Tick>(k) * st;
        g.off += g.c.start;
        rowLen = std::max(rowLen, g.c.start + s.clipLength(g.c));
        gGridDrag.push_back(g);
      }
    }
    if (rowLen > 0) {
      off += rowLen;
    }
  }
  gGridDragGone = 0;
  if (!gGridDrag.empty()) {
    showView(a, false);
  }
}
void gridDragFrame() {
  if (gGridDrag.empty()) {
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (++gGridDragGone > 2) {  // (the Arrangement takes it on the release frame)
      gGridDrag.clear();
    }
    return;
  }
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
    const int n = static_cast<int>(gGridDrag.size());
    ImGui::SetDragDropPayload("GL_GRIDCLIPS", &n, sizeof n);
    ImGui::TextUnformatted(n == 1 ? i18n::tr("1 clip: drop it on the Arrangement") : i18n::tr("Clips: drop them on the Arrangement"));
    ImGui::EndDragDropSource();
  }
}

// A new empty MIDI clip (one bar) in an empty slot of a track that takes notes (`made`: its id).
bool newMidiClipIn(App& a, int t, int k, ObjectId* made) {
  const Song& s = a.doc.song();
  if (t < 0 || t >= static_cast<int>(s.tracks.size()) || s.tracks[static_cast<size_t>(t)].locked || !trackmodes::clipAllowed(s, ClipKind::Pattern, t) ||
      trackmodes::modeOf(s, t) == TrackMode::Audio ||  // an audio track's slot takes no MIDI clip
      !slotClips(s, t, k).empty())
    return false;
  a.doc.beginGroup("insert MIDI clip");
  ensureScene(a, k);
  ensureNoteChannel(a, t);
  const ObjectId p = a.doc.addPattern("");  // N47-14: an empty clip has no name until given one (refs 455, 531)
  ObjectId id = kNoId;
  if (p != kNoId) {
    const Rgb col = a.doc.song().tracks[static_cast<size_t>(t)].color;  // N46-02: the clip takes the track's colour
    a.doc.editSong("pattern colour", [&](Song& x) {
      if (Pattern* pp = x.pattern(p)) pp->color = col;
    });
    id = a.doc.addClip(p, t, static_cast<Tick>(k) * sceneTicks(a.doc.song()), std::max<Tick>(1, a.doc.song().barTicks()));
  }
  a.doc.endGroup();
  if (id == kNoId) return false;
  if (const Channel* ch = trackmodes::linkedChannel(a.doc.song(), t)) a.selectChannel(ch->id);
  else if (const std::vector<ObjectId> chs = trackChannels(a.doc.song(), t); !chs.empty()) a.selectChannel(chs.front());
  a.selectPattern(p);
  selectSlot(a, t, k);
  layoutui::setRegionVisible(a, "live.clip", true);  // it opens in the clip band
  if (made) *made = id;
  return true;
}

void ensureNoteChannel(App& a, int t) {
  // N46-02: notes need a channel. A MIDI track with no instrument and no notes yet gets a note-only
  // channel (MIDI Out, silent, on Main: no strip of its own), linked as its instrument, so the clip
  // opens ready to draw; an instrument dropped on the track later takes that channel's place and
  // keeps the notes (the track's ReplaceTrackInstrument drop).
  if (!trackmodes::linkedChannel(a.doc.song(), t) && trackChannels(a.doc.song(), t).empty()) {
    const PlaylistTrack& tr0 = a.doc.song().tracks[static_cast<size_t>(t)];
    const Rgb col = tr0.color;
    const ObjectId nc = a.doc.addChannel(playlistTrackName(a.doc.song(), static_cast<size_t>(t)), "gl.midiout");
    if (nc != kNoId)
      a.doc.editSong("link MIDI track", [&](Song& x) {
        if (Channel* c = x.channel(nc)) c->color = col, c->mixerTrack = kNoId;
        PlaylistTrack& tr = x.tracks[static_cast<size_t>(t)];
        tr.mode = TrackMode::Instrument, tr.channel = nc, tr.mixer = kNoId;
      });
  }
}
bool newMidiClip(App& a, int t, int k) { return newMidiClipIn(a, t, k, nullptr); }

// The clip's own edits at the head (run next frame: the grid's slot lists hold playlist indices) of its menu (C44-15), then the arrangement's clip menu.
void slotEditItems(App& a, int t, int k, bool hasClip) {
  if (hasClip) {  // grid2-1118: on every selected slot
    if (ImGui::MenuItem(i18n::tr("Cut"))) {
      a.later([&a] { copySlots(a), deleteSlots(a, "cut clips"); });
    }
    if (ImGui::MenuItem(i18n::tr("Copy"), keys::label("playlist.copy"))) {
      copySlots(a);
    }
  }
  if (ImGui::MenuItem(i18n::tr("Paste"), keys::label("playlist.paste"), false, !gClipboard.empty() || !gBoard.empty())) {
    a.later([&a, t, k] { pasteSlots(a, t, k); });
  }
  if (hasClip) {
    if (ImGui::MenuItem(i18n::tr("Duplicate"), keys::label("playlist.duplicate"))) {
      a.later([&a] { duplicateSlots(a); });
    }
    if (ImGui::MenuItem(i18n::tr("Delete"), keys::label("playlist.delete"))) {
      a.later([&a] { deleteSlots(a, "delete clips"); });
    }
  }
  {  // grid2-1118: the slot's stop button (Cmd+E)
    const bool has = trackmodes::hasStopButton(a.doc.song(), t, k);
    if (ImGui::MenuItem(has ? i18n::tr("Remove stop button") : i18n::tr("Add stop button"), keys::shownText("Ctrl+E"))) {
      a.later([&a] { toggleStopButtons(a); });
    }
  }
  if (!hasClip) {
    const bool midi = trackmodes::clipAllowed(a.doc.song(), ClipKind::Pattern, t) && trackmodes::modeOf(a.doc.song(), t) != TrackMode::Audio;
    if (ImGui::MenuItem(i18n::tr("Insert MIDI clip"), nullptr, false, midi)) a.later([&a, t, k] { newMidiClip(a, t, k); });
  }
  if (ImGui::MenuItem(i18n::tr("Insert scene"), keys::label("playlist.insertSpace"))) a.later([&a, k] { insertScene(a, k); });
}

// Checkpoint 59 (N58-03): a scene's menu (a right-click on its launch cell), as the reference's: launch,
// rename and colour it, insert, duplicate or delete it. Each edit is one undo step; the name and colour
// are kept with the Set (Song::live.scenes). `stored`: the scene holds clips of the live area.
void renameScene(App& a, int k) {
  const SceneInfo* info = trackmodes::sceneInfo(a.doc.song(), k);
  menus::openPrompt("Rename scene", info ? info->name : std::string(), [&a, k](const std::string& n) {
    a.doc.editSong("rename scene", [&](Song& x) { trackmodes::setSceneName(x, k, n); });
  });
}

// grid2-1118: scene k's settings changed by `fn` (its name and colour stay).
void editScene(Song& x, int k, const std::function<void(SceneInfo&)>& fn) {
  const SceneInfo* si = trackmodes::sceneInfo(x, k);
  SceneInfo info = si ? *si : SceneInfo{};
  fn(info);
  trackmodes::setSceneSettings(x, k, info);
}

void sceneMenuItems(App& a, int k, bool stored) {
  const Song& s = a.doc.song();
  const SceneInfo* info = trackmodes::sceneInfo(s, k);
  const std::string label = info && !info->name.empty() ? info->name : "Scene " + std::to_string(k + 1);
  ImGui::TextDisabled("%s", label.c_str());
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Rename..."), keys::label("edit.rename"))) renameScene(a, k);  // N59-07: and Cmd+R on a selected scene
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {
    uint32_t col = 0;
    if (menus::colorGrid(&col)) a.doc.editSong("scene colour", [&](Song& x) { trackmodes::setSceneColor(x, k, col); });
    if (ImGui::MenuItem(i18n::tr("No colour"), nullptr, false, info && info->color != 0))
      a.doc.editSong("scene colour", [&](Song& x) { trackmodes::setSceneColor(x, k, 0); });
    ImGui::EndMenu();
  }
  // grid2-1118: the scene's own tempo and time signature (set when it launches) and its follow actions.
  {
    const double bpm = info ? trackmodes::sceneTempo(*info) : 0.0;
    int num = 0, den = 0;
    const bool sig = info && trackmodes::sceneSignature(*info, num, den);
    char cur[48];
    std::snprintf(cur, sizeof cur, "%.2f", bpm);
    if (ImGui::MenuItem(i18n::tr("Tempo..."), bpm > 0 ? cur : nullptr)) {
      menus::openPrompt("Scene tempo (BPM; empty: none)", bpm > 0 ? std::string(cur) : std::string(), [&a, k](const std::string& v) {
        const double t = std::atof(v.c_str());
        if (!v.empty() && (t < 10 || t > 999)) {
          a.notify(i18n::tr("A scene's tempo is between 10 and 999 BPM"), true);
          return;
        }
        a.doc.editSong("scene tempo", [&](Song& x) { editScene(x, k, [&](SceneInfo& si) { si.tempo = v.empty() ? 0.0 : t; }); });
      });
    }
    char sg[24];
    std::snprintf(sg, sizeof sg, "%d/%d", num, den);
    if (ImGui::MenuItem(i18n::tr("Time signature..."), sig ? sg : nullptr)) {
      menus::openPrompt("Scene time signature (as 3/4; empty: none)", sig ? std::string(sg) : std::string(), [&a, k](const std::string& v) {
        int n = 0, d = 0;
        if (!v.empty() && (std::sscanf(v.c_str(), "%d/%d", &n, &d) != 2 || n < 1 || n > 99 || (d != 1 && d != 2 && d != 4 && d != 8 && d != 16))) {
          a.notify(i18n::tr("A time signature is written as 3/4 (1 to 99 over 1, 2, 4, 8 or 16)"), true);
          return;
        }
        a.doc.editSong("scene time signature", [&](Song& x) {
          editScene(x, k, [&](SceneInfo& si) { si.sigNum = v.empty() ? 0 : n, si.sigDen = v.empty() ? 0 : d; });
        });
      });
    }
    if (ImGui::BeginMenu(i18n::tr("Follow action"))) {
      FollowSpec f = info ? info->follow : FollowSpec{};
      const FollowSpec f0 = f;
      if (ImGui::MenuItem(i18n::tr("On"), nullptr, f.on)) {
        f.on = !f.on;
      }
      if (ImGui::BeginMenu(i18n::tr("Action A"))) {
        for (int i = 0; i < static_cast<int>(FollowAction::Count); ++i) {
          if (ImGui::MenuItem(followActionName(static_cast<FollowAction>(i)), nullptr, f.a == static_cast<FollowAction>(i))) {
            f.a = static_cast<FollowAction>(i), f.on = true;
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(i18n::tr("Action B"))) {
        for (int i = 0; i < static_cast<int>(FollowAction::Count); ++i) {
          if (ImGui::MenuItem(followActionName(static_cast<FollowAction>(i)), nullptr, f.b == static_cast<FollowAction>(i))) {
            f.b = static_cast<FollowAction>(i), f.on = true;
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(i18n::tr("Chance A"))) {
        for (int c : {100, 75, 50, 25, 0}) {
          const std::string l = std::to_string(c) + "%";
          if (ImGui::MenuItem(l.c_str(), nullptr, f.chanceA == c && f.chanceB == 100 - c)) {
            f.chanceA = static_cast<uint8_t>(c), f.chanceB = static_cast<uint8_t>(100 - c);
          }
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(i18n::tr("Follow time"))) {
        const Tick bar = std::max<Tick>(1, s.barTicks());
        for (int n : {1, 2, 4, 8, 16}) {
          const std::string l = std::to_string(n) + (n == 1 ? i18n::tr(" bar") : i18n::tr(" bars"));
          const Tick t = bar * n;
          if (ImGui::MenuItem(l.c_str(), nullptr, (f.time > 0 ? f.time : bar) == t)) {
            f.time = n == 1 ? 0 : t;
          }
        }
        ImGui::EndMenu();
      }
      if ((f.a == FollowAction::Jump || f.b == FollowAction::Jump) && ImGui::MenuItem(i18n::tr("Jump to scene..."))) {
        const int j = f.a == FollowAction::Jump ? f.jumpA : f.jumpB;
        menus::openPrompt("Jump to scene (its number)", std::to_string(j + 1), [&a, k](const std::string& v) {
          const int n = std::atoi(v.c_str());
          if (n < 1) {
            return;
          }
          a.doc.editSong("scene follow action", [&](Song& x) {
            editScene(x, k, [&](SceneInfo& si) {
              if (si.follow.a == FollowAction::Jump) {
                si.follow.jumpA = n - 1;
              }
              if (si.follow.b == FollowAction::Jump) {
                si.follow.jumpB = n - 1;
              }
            });
          });
        });
      }
      ImGui::EndMenu();
      if (!(f == f0)) {
        a.doc.editSong("scene follow action", [&](Song& x) { editScene(x, k, [&](SceneInfo& si) { si.follow = f; }); });
      }
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Insert scene"), keys::label("playlist.insertSpace"))) a.later([&a, k] { insertScene(a, k); });
  // N59-06: any scene: one past the stored ones copies its name and colour (and moves the later ones down).
  if (ImGui::MenuItem(i18n::tr("Duplicate scene"), nullptr, false, stored || k < static_cast<int>(s.live.scenes.size()))) {
    a.later([&a, k] {
      const Tick st = sceneTicks(a.doc.song());
      a.doc.editSong("duplicate scene", [&](Song& x) { trackmodes::duplicateScene(x, k, st); });
    });
  }
  if (ImGui::MenuItem(i18n::tr("Delete scene"), nullptr, false, stored || info)) {
    a.later([&a, k] {
      const Tick st = sceneTicks(a.doc.song());
      a.doc.editSong("delete scene", [&](Song& x) {
        if (!trackmodes::deleteScene(x, k, st)) {  // a row past the stored scenes: its name and colour only
          trackmodes::setSceneName(x, k, "");
          trackmodes::setSceneColor(x, k, 0);
        }
      });
    });
  }
}

}  // namespace

bool slotDragHeld() {
  // (the button held: the grid is not drawn once the drag went to the Arrangement, so its press flag stays)
  return (gSlotPressed || !gGridDrag.empty()) && ImGui::IsMouseDown(ImGuiMouseButton_Left);
}

bool dropGridClips(App& a, int track, Tick at) {
  if (!active(a)) {
    return false;
  }
  const ImGuiPayload* peek = ImGui::GetDragDropPayload();
  if (!peek || !peek->IsDataType("GL_GRIDCLIPS")) {
    return false;
  }
  if (!ImGui::AcceptDragDropPayload("GL_GRIDCLIPS")) {
    return true;
  }
  const std::vector<GridDragClip> drag = std::move(gGridDrag);
  gGridDrag.clear();
  const Song& s = a.doc.song();
  const Tick from = std::max(at, arrangementOrigin(a));
  std::set<ObjectId> placed;
  int refused = 0;
  a.doc.editSong("place clips in arrangement", [&](Song& x) {
    for (const GridDragClip& g : drag) {
      const int tr = track + g.dt;
      // hunt-clips-1119: a MIDI clip never lands on an audio track (nor audio on a MIDI track), as a slot
      // dragged in the grid (N50-02): grid columns dropped one track to the right put MIDI clips on the
      // audio track beside them and audio clips on a spare track past the set's last one.
      if (tr < 0 || tr >= static_cast<int>(x.tracks.size()) || x.tracks[static_cast<size_t>(tr)].locked || !trackmodes::clipAllowed(x, g.c.kind, tr) ||
          !clipFitsTrack(x, g.c.kind, tr) || (g.c.kind != ClipKind::Automation && trackmodes::isGroupTrack(x, tr))) {
        ++refused;
        continue;
      }
      Clip c = g.c;
      c.id = x.allocateId();
      c.start = from + g.off;
      c.track = tr;
      c.takeGroup = kNoId, c.take = 0;
      c.launched = false, c.launchSet = ClipLaunch{};  // (an arrangement clip plays where it lies)
      x.playlist.push_back(c);
      placed.insert(c.id);
    }
  });
  (void)s;
  if (!placed.empty()) {
    a.playlist.selected = placed;
  }
  if (refused > 0) {
    a.notify(i18n::tr("Some clips did not fit the tracks under them and were left out"), true);
  }
  return true;
}

void devClipsMeters(App& a, int frame) {
  std::printf("clipsmeters frame %d: drawn %d playing %d meters %zu\n", frame, gClipsMeterDevFrame, a.engine.playing() ? 1 : 0, gClipsMeterDev.size());
  for (const ClipsMeterDev& d : gClipsMeterDev)
    std::printf("  meter %s levelDb %.1f %.1f holdDb %.1f %.1f pos %.3f %.3f oldpos %.3f %.3f\n", d.what.c_str(), static_cast<double>(d.levelDb[0]),
                static_cast<double>(d.levelDb[1]), static_cast<double>(d.holdDb[0]), static_cast<double>(d.holdDb[1]), static_cast<double>(d.pos[0]),
                static_cast<double>(d.pos[1]), static_cast<double>(d.oldPos[0]), static_cast<double>(d.oldPos[1]));
  std::fflush(stdout);
}

void devGrid2State(App& a, int frame) {
  const Song& s = a.doc.song();
  std::printf("grid2 frame %d: scene %d tempo %.2f sig %d/%d songTempo %.2f songSig %d/%d sceneLen %lld follow %d sel %zu view %s\n", frame,
              a.engine.liveScene(), a.engine.sceneTempoSet(), a.engine.sceneSigNumSet(), a.engine.sceneSigDenSet(), s.tempo, s.timeSigNum,
              s.timeSigDen, static_cast<long long>(s.live.sceneLen), a.engine.followActions() ? 1 : 0, selectedSlots().size(),
              gridView(a) ? "grid" : "arrangement");
  std::printf("  editgrid arrangement \"%s\" clip \"%s\"\n", gridLabel(a, 0).c_str(), gridLabel(a, 1).c_str());
  // hunt-clips2-1119: Main chosen (its title and row ringed), as the ring is drawn
  std::printf("  mainchosen %d\n", !s.mixer.empty() && a.selMixer == s.mixer[0].id && a.showMaster ? 1 : 0);
  for (const auto& [t, k] : selectedSlots()) {
    std::printf("  selslot %d %d\n", t, k);
  }
  for (size_t t = 0; t < s.tracks.size(); ++t) {
    std::string ns;
    for (int k : s.tracks[t].noStop) {
      ns += (ns.empty() ? "" : ",") + std::to_string(k);
    }
    std::printf("  gtrack %zu launch %d nostop %s\n", t, static_cast<int>(s.tracks[t].launch), ns.empty() ? "-" : ns.c_str());
  }
  for (const Clip& c : s.playlist) {
    if (c.launchSet.isDefault()) {
      continue;
    }
    const FollowSpec& f = c.launchSet.follow;
    std::printf("  gclip %u mode %d quant %d legato %d follow %d a %d b %d chance %d %d time %lld linked %d\n", static_cast<unsigned>(c.id), c.launchSet.mode,
                c.launchSet.quant, c.launchSet.legato ? 1 : 0, f.on ? 1 : 0, static_cast<int>(f.a), static_cast<int>(f.b), f.chanceA, f.chanceB,
                static_cast<long long>(f.time), f.linked ? 1 : 0);
  }
  for (size_t k = 0; k < s.live.scenes.size(); ++k) {
    const SceneInfo& sc = s.live.scenes[k];
    std::printf("  gscene %zu name \"%s\" tempo %.2f sig %d/%d follow %d\n", k, sc.name.c_str(), sc.tempo, sc.sigNum, sc.sigDen, sc.follow.on ? 1 : 0);
  }
  std::fflush(stdout);
}

bool renameSelectedScene(App& a) {
  if (!active(a) || !sceneSelValid(a)) return false;
  renameScene(a, gSceneSel);
  return true;
}

void devGridAsk() { gGridAsked = true; }
namespace {
float gSceneColX = 0;  // the scene column's centre, last drawn (dev grid report)
}
namespace {
// Checkpoint 48: the slot Session record or Capture makes its clip in on track t: slot k when it is
// empty, else (k < 0) the selected slot of the track when empty, else the track's first empty slot
// at or after it. -1 when the track takes no MIDI clip.
int sessionRecordSlotFor(App& a, int t, int k) {
  const Song& s = a.doc.song();
  if (!midiTrack(s, t) || s.tracks[static_cast<size_t>(t)].locked) return -1;
  if (k >= 0) return slotClips(s, t, k).empty() ? k : -1;
  k = gSel.t == t && gSel.k >= 0 ? gSel.k : 0;
  while (!slotClips(s, t, k).empty()) ++k;
  return k;
}
// N47-01: an audio track's slot to record into: k when empty, else the selected (or first) empty one.
int audioRecordSlotFor(App& a, int t, int k) {
  const Song& s = a.doc.song();
  if (k < 0) k = gSel.t == t && gSel.k >= 0 ? gSel.k : 0;
  while (!slotClips(s, t, k).empty()) ++k;
  return k;
}
}  // namespace
int selectedTrack(App& a) {
  const Song& s = a.doc.song();
  const int n = gridColumns(s);
  if (!a.playlist.trackSel.empty() && *a.playlist.trackSel.begin() < n) return *a.playlist.trackSel.begin();
  if (gSel.t >= 0 && gSel.t < n) return gSel.t;
  for (ObjectId id : a.playlist.selected)
    if (const Clip* c = s.clip(id); c && c->track < n) return c->track;
  return -1;
}
void newReturnTrack(App& a) {
  a.later([&a] { a.doc.editSong("new return track", [](Song& s) { trackmodes::addReturnTrack(s, "Return"); }); });
}

void toggleDetail(App& a) {
  const layout::Shell& sh = layout::shell(layout::Preset::Live);
  const layout::State st = layoutui::regionState(a, layout::Preset::Live);
  const bool clip = layout::visible(sh, st, "live.clip"), dev = layout::visible(sh, st, "live.device");
  if (clip || dev) {
    layoutui::setRegionVisible(a, "live.clip", false);
    layoutui::setRegionVisible(a, "live.device", false);
  } else {
    showDetailView(a, gLastClip, false);  // 0.7.8: the spot comes back with the view it last showed
  }
}

int newTrack(App& a, bool audio) { return newTrackAfter(a, audio, active(a) ? selectedTrack(a) : -1); }
int newTrackAfter(App& a, bool audio, int after) {
  const int t = audio ? trackmodes::addAudioTrack(a.doc, after) : trackmodes::addMidiTrack(a.doc, after);
  if (t >= 0) {
    a.playlist.trackSel = {t};
    if (gSel.t >= 0) gSel.t = t;
  }
  return t;
}
bool clipFitsTrack(const Song& s, ClipKind kind, int track) {
  if (kind == ClipKind::Automation) return true;
  if (trackmodes::isGroupTrack(s, track)) return false;
  if (trackmodes::modeOf(s, track) == TrackMode::Audio) return kind == ClipKind::Audio;
  return kind == ClipKind::Pattern;
}
void groupTracks(App& a, int track) {
  std::vector<int> sel(a.playlist.trackSel.begin(), a.playlist.trackSel.end());
  if (track >= 0 && !a.playlist.trackSel.count(track)) sel = {track};
  if (sel.empty() && selectedTrack(a) >= 0) sel = {selectedTrack(a)};
  a.later([&a, sel] {
    std::string why;
    const int g = trackmodes::groupTracks(a.doc, sel, &why);
    if (g < 0) {
      a.notify(i18n::tr(why.empty() ? "Select the tracks to group" : why.c_str()));
      return;
    }
    a.playlist.trackSel = {g};
    gSel = {};
    focusTrack(a, g);
  });
}
// Checkpoint 52 (N51-03, N51-07): the selected tracks (or the one clicked when it is not among them).
std::vector<int> tracksFor(App& a, int track) {
  std::vector<int> sel(a.playlist.trackSel.begin(), a.playlist.trackSel.end());
  if (track >= 0 && !a.playlist.trackSel.count(track)) sel = {track};
  if (sel.empty() && selectedTrack(a) >= 0) sel = {selectedTrack(a)};
  return sel;
}
void deleteTracks(App& a, int track) {
  const std::vector<int> sel = tracksFor(a, track);
  a.later([&a, sel] {
    std::string why;
    if (trackmodes::deleteTracks(a.doc, sel, &why) <= 0) return a.notify(i18n::tr(why.empty() ? "Select the tracks to delete" : why.c_str()));
    const int n = trackmodes::usedTrackCount(a.doc.song());
    a.playlist.trackSel = {std::clamp(sel.empty() ? 0 : sel.front(), 0, std::max(0, n - 1))};
    gSel = {};
  });
}
void duplicateTracks(App& a, int track, bool withContent) {
  const std::vector<int> sel = tracksFor(a, track);
  a.later([&a, sel, withContent] {
    std::string why;
    const int t = trackmodes::duplicateTracks(a.doc, sel, &why, withContent);
    if (t < 0) return a.notify(i18n::tr(why.empty() ? "Select the tracks to duplicate" : why.c_str()));
    a.playlist.trackSel = {t};
    gSel = {};
    focusTrack(a, t);
  });
}
void ungroupTracks(App& a, int track) {
  int t = track >= 0 ? track : !a.playlist.trackSel.empty() ? *a.playlist.trackSel.begin() : selectedTrack(a);
  // N52-07: as the reference, Ungroup acts on a selected group track only (a member selected: nothing).
  if (!trackmodes::isGroupTrack(a.doc.song(), t)) {
    a.notify(i18n::tr("Select a group track to ungroup"));
    return;
  }
  a.later([&a, t] {
    std::string why;
    if (!trackmodes::ungroupTracks(a.doc, t, &why)) return a.notify(i18n::tr(why.c_str()));
    a.playlist.trackSel = {t};
    gSel = {};
  });
}
namespace {
bool gAutoLanes = true;
}
void toggleAutomationLanes(App& a) {
  gAutoLanes = !gAutoLanes;
  a.notify(i18n::tr(gAutoLanes ? "Automation lanes shown (A hides them)" : "Automation lanes hidden (A shows them)"));
}
bool automationLanesShown() { return gAutoLanes; }
void gridCaptureScene(App& a) {
  a.later([&a] { captureSceneNow(a); });
}
void backToArrangement(App& a) {
  a.engine.backToArrangement();
  if (a.engine.mode() != PlayMode::Song) a.engine.setMode(PlayMode::Song);
}
void gridInsertScene(App& a) {
  const int k = gSel.k >= 0 ? gSel.k : std::max(0, static_cast<int>((a.doc.song().live.areaEnd + sceneTicks(a.doc.song()) - 1) / sceneTicks(a.doc.song())) - 1);
  a.later([&a, k] { insertScene(a, k); });
}
void gridInsertMidiClip(App& a) {
  const Song& s = a.doc.song();
  if (!gridView(a)) {
    // N49-07: in the Arrangement an empty MIDI clip over the time selection, else a bar at the insert
    // marker, on the selected track (the time selection's first track first).
    int t = !a.playlist.selTracks.empty() && a.playlist.selEnd > a.playlist.selStart ? *a.playlist.selTracks.begin()
            : !a.playlist.trackSel.empty()                                         ? *a.playlist.trackSel.begin()
                                                                                   : selectedTrack(a);
    if (!midiTrack(s, t)) {
      a.notify(i18n::tr("Select a MIDI track first"));
      return;
    }
    const Tick bar = std::max<Tick>(1, s.barTicks()), o = arrangementOrigin(a);
    Tick at = std::max(o, static_cast<Tick>(a.engine.position())), len = bar;
    if (a.playlist.selEnd > a.playlist.selStart) at = std::max(o, a.playlist.selStart), len = std::max<Tick>(1, a.playlist.selEnd - at);
    a.later([&a, t, at, len, bar] {
      if (a.doc.song().tracks[static_cast<size_t>(t)].locked) return;
      a.doc.beginGroup("insert MIDI clip");
      ensureNoteChannel(a, t);
      const int bars = static_cast<int>(std::max<Tick>(1, (len + bar - 1) / bar));
      const ObjectId p = a.doc.addPattern("", bars);
      ObjectId id = kNoId;
      if (p != kNoId) {
        const Rgb col = a.doc.song().tracks[static_cast<size_t>(t)].color;
        a.doc.editSong("pattern colour", [&](Song& x) {
          if (Pattern* pp = x.pattern(p)) pp->color = col;
        });
        id = a.doc.addClip(p, t, at, len);
      }
      a.doc.endGroup();
      if (id == kNoId) return;
      if (const Channel* ch = trackmodes::linkedChannel(a.doc.song(), t)) a.selectChannel(ch->id);
      a.selectPattern(p);
      a.playlist.selected = {id};
      // hunt-clips2-1119: the time selection made the clip: it is the clip now (the selection stayed
      // drawn over the new clip).
      a.playlist.selStart = a.playlist.selEnd = 0;
      a.playlist.selTracks.clear();
      layoutui::setRegionVisible(a, "live.clip", true);
    });
    return;
  }
  int t = gSel.t, k = gSel.k;
  if (t < 0 && !a.playlist.trackSel.empty()) t = *a.playlist.trackSel.begin();
  if (t < 0 || t >= static_cast<int>(s.tracks.size())) {
    a.notify(i18n::tr("Select a slot of a MIDI track first"));
    return;
  }
  if (k < 0 || !slotClips(s, t, k).empty()) {  // the track's first empty slot
    k = 0;
    while (!slotClips(s, t, k).empty()) ++k;
  }
  a.later([&a, t, k] {
    if (!newMidiClip(a, t, k)) a.notify(i18n::tr("MIDI clips go on MIDI tracks"));
  });
}

namespace {

// K-112c (round 38): the session mixer's strips, shared by the clip grid's foot and the Clips mixer band.
struct StripRow {
  float y0 = 0, h = 0;     // the strips' top and height
  bool sendsRow = false;  // the sends row across their top
};
// N45-03: one send per return, as the reference's strips (every track has a knob for every return,
// a new track or return included): (send index or -1 when the track has no send there yet, the
// return's mixer index). A mixer track never sends to itself.
constexpr int kMaxStripSends = 6;
// Checkpoint 56 (N55-01): a send is an aux send, never the track's output: while the track plays into a
// return, that return's knob shows the kept aux send (or none), greyed out (`routed`). Checkpoint 57
// (N56-01): the kept send is in Song::keptSends, not in the track's sends (it does not play); an aux send
// that is in the sends (a hand-made song's second route there) plays, and its knob is live.
struct StripSend {
  int k = -1;      // the aux send's index (-1: none yet)
  int dest = -1;   // the return's mixer index
  bool routed = false;  // the track's output is this return: the send is off (its level kept)
};
std::vector<StripSend> sendsOf(const Song& s, const MixerTrack* mx) {
  std::vector<StripSend> v;
  if (!mx) return v;
  const ObjectId out = trackmodes::outputOf(*mx);
  for (size_t j : returnTracks(s)) {
    if (s.mixer[j].id == mx->id) continue;
    const int k = trackmodes::auxSendIndex(*mx, s.mixer[j].id);
    v.push_back({k, static_cast<int>(j), s.mixer[j].id == out && k < 0});
    if (v.size() >= static_cast<size_t>(kMaxStripSends)) break;
  }
  return v;
}
// K-112c (round 28): the sends get their own row across the top of every strip, above the volume
// and the meter, with "Sends" at the right and the knobs staggered, each with its return's letter
// in its open foot (143, 141, 112). The row is kept on every strip (blank where a track sends
// nowhere) so the faders line up, and dropped when the strips are too short for it.
// C47: three knobs to a row, a second row from the fourth return (up to six on the strip).
constexpr float kSendsH = 34.0f;
// Checkpoint 58 (N57-03): the least strip height that shows the In/Out rows (From and To, labelled).
constexpr float kStripIoMinH = 220.0f;
float sendsHeight(const Song& s) {
  const int n = std::min<int>(static_cast<int>(returnTracks(s).size()), kMaxStripSends);
  if (n == 0) return 0.0f;
  const int per = std::min(n, 3), rowsN = (n + 2) / 3;
  return kSendsH + 8.0f * static_cast<float>(per - 2 > 0 ? per - 2 : 0) + 30.0f * static_cast<float>(rowsN - 1);
}
bool anySends(const Song& s) { return !returnTracks(s).empty(); }
// Checkpoint 55 (N54-10): a strip's send knob made real (a track of an older set, which has none): the
// send to `to` at `level`, and the track's other return sends at zero, as a new track has them, in one
// step. A track with no output yet first sends to Main (its output; the send must not become it).
void addStripSend(Song& x, ObjectId from, ObjectId to, float level) {
  MixerTrack* t = x.mixerTrack(from);
  if (!t) {
    return;
  }
  if (trackmodes::outputOf(*t) == kNoId) {
    t->sends.insert(t->sends.begin(), Send{x.masterId(), 1.0f, false});
  }
  bool has = false;
  for (const Send& sd : t->sends) {
    has = has || (sd.to == to && !sd.sidechain);
  }
  if (!has) {
    t->sends.push_back({to, level, false});
  }
  trackmodes::addReturnSendsTo(x, from);
}
// Checkpoint 57 (N56-02): a send that is off while the track plays into its return, drawn dimmed as the
// reference draws it: a faint cap and rail, no value arc, a faint pointer at the kept level. Clips' strip
// only (ui::Knob and the plugin editors are left as they are). `p` is the knob's top left.
void offSendKnob(ImDrawList* dl, ImVec2 p, float r, float norm) {
  const auto& P = pal();
  const ImVec2 c(p.x + r, p.y + r);
  const float a0 = 3.14159265f * 0.75f, a1 = 3.14159265f * 2.25f, th = 2.0f;
  dl->AddCircleFilled(c, r - th - 1.0f, theme::withAlpha(P.surface800, 0.45f), 32);
  dl->PathArcTo(c, r - th * 0.5f, a0, a1, 32);
  dl->PathStroke(theme::withAlpha(P.dark ? P.field : P.line, 0.55f), 0, th);
  const float an = a0 + std::clamp(norm, 0.0f, 1.0f) * (a1 - a0);
  const ImVec2 dir(std::cos(an), std::sin(an));
  dl->PathLineTo(ImVec2(c.x + dir.x * r * 0.15f, c.y + dir.y * r * 0.15f));
  dl->PathLineTo(ImVec2(c.x + dir.x * (r - th - 3.0f), c.y + dir.y * (r - th - 3.0f)));
  dl->PathStroke(theme::withAlpha(P.textFaint, 0.6f), 0, 2.0f);
}
// A send knob for a send the track does not have yet: drawn at zero; touching it adds the send (to
// the return, at zero) under the same widget id, so the drag goes on in the real knob next frame.
void missingSendKnob(App& a, ObjectId from, int dest, float r) {
  double v = 0.0;
  const ParamDesc d{"Send level", "%", 0.0, 1.25, 1.0, ParamKind::Continuous, ParamScale::Linear, {}};
  bool released = false;
  const bool ch = ui::Knob("##send", &v, d, r, nullptr, false, &released);
  const Song& s = a.doc.song();
  const MixerTrack* fm = s.mixerTrack(from);
  if (ch && fm && from != s.masterId() && dest >= 0 && dest < static_cast<int>(s.mixer.size()) && canRoute(s, from, s.mixer[static_cast<size_t>(dest)].id)) {
    const ObjectId to = s.mixer[static_cast<size_t>(dest)].id;
    const float lv = static_cast<float>(std::clamp(v, 0.0, 1.25));
    a.doc.editSong("add send", [&](Song& x) { addStripSend(x, from, to, lv); });
  }
}
void sessionStrip(App& a, ImDrawList* dl, const StripRow& g, int t, const MixerTrack* mx, float x, float w, bool master, char letter = 0) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const ImVec2 m = ImGui::GetIO().MousePos;
  const float my0 = g.y0, mh = g.h;
  const bool sendsRow = g.sendsRow;
  ImGui::PushID(master ? 9999 : letter ? 8000 + letter : 2000 + t);
  if (mx) {
    // K-112c (round 32): collaboration, as Studio's mixer: a peer's pointer on this mixer track (in any
    // layout) lands on this strip, and a peer's change to it flashes its outline.
    canchor::publishRect(canchor::MixerStrip, mx->id, 0, ImVec2(x, my0), ImVec2(x + w, my0 + mh));
    if (ImU32 fc = 0; const float fl = collabui::flash(a, "mixer/#" + std::to_string(mx->id), &fc))
      dl->AddRect(ImVec2(x + 1, my0 + 1), ImVec2(x + w - 1, my0 + mh - 1), theme::withAlpha(fc, fl), 0.0f, 0, 2.0f);
  }
  // GitHub #58: the track's mixer strip hidden (or in a collapsed group): a quiet stand-in in its
  // column (the clip grid keeps its columns), a click shows it; narrow: the meter, the activator
  // and S only.
  const int mxIndex = mx ? s.mixerIndex(mx->id) : -1;
  const bool slim = mx && !master && mxIndex > 0 && mx->narrow;
  auto stripMenu = [&] {  // a right-click on the strip's free space: the mixer track's menu
    if (!mx || master || mxIndex <= 0) return;
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(x, my0), ImVec2(x + w, my0 + mh)) && !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right))
      ImGui::OpenPopup("##clipsmixmenu");
    if (ImGui::BeginPopup("##clipsmixmenu")) {
      if (const int mi = a.doc.song().mixerIndex(mx->id); mi > 0) mixerTrackMenu(a, static_cast<size_t>(mi));
      ImGui::EndPopup();
    }
  };
  if (mx && !master && mxIndex > 0 && mixhide::trackHidden(s, static_cast<size_t>(mxIndex))) {
    const bool byGroup = !mx->hidden;
    const ImVec2 c(std::round(x + w * 0.5f), std::round(my0 + mh * 0.42f));
    ImGui::SetCursorScreenPos(ImVec2(x, my0));
    const bool click = ImGui::InvisibleButton("##hiddenstrip", ImVec2(std::max(1.0f, w), std::max(1.0f, mh)));
    devNoteRect("clipshiddenstrip");  // (tests: the stand-in keeps the track's column)
    const bool hov = ImGui::IsItemHovered();
    if (hov) dl->AddRectFilled(ImVec2(x + 2, my0 + 2), ImVec2(x + w - 2, my0 + mh - 2), theme::withAlpha(P.surface700, 0.5f));
    ui::drawIcon(dl, ui::Icon::EyeOff, c, 14, hov ? P.text : P.textFaint);
    const char* lab = byGroup ? i18n::tr("In a collapsed group") : i18n::tr("Hidden");
    theme::centreText(dl, a.fontRegular, theme::kFontLabel, ImVec2(x + 2, c.y + 12), ImVec2(x + w - 2, c.y + 28), hov ? P.text : P.textDim, lab);
    if (byGroup) ui::Hint("This track's mixer strip is in a collapsed group (it still plays): click to expand the group");
    else ui::Hint("This track's mixer strip is hidden (it still plays): click to show it");
    if (click) {
      if (byGroup) mixhide::setGroupFolded(a.doc, mx->group, false);
      else mixhide::setHidden(a.doc, {mx->id}, false);
    }
    const bool trk = t >= 0 && t < static_cast<int>(s.tracks.size());
    dl->AddRectFilled(ImVec2(x, std::round(my0 + mh - 3.0f)), ImVec2(x + w, std::round(my0 + mh)), trk ? theme::content(s.tracks[static_cast<size_t>(t)].color) : theme::content(mx->color));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##clipsmixmenu");
    if (ImGui::BeginPopup("##clipsmixmenu")) {
      if (const int mi = a.doc.song().mixerIndex(mx->id); mi > 0) mixerTrackMenu(a, static_cast<size_t>(mi));
      ImGui::EndPopup();
    }
    ImGui::PopID();
    return;
  }
  // The sends row (see above), then two columns. Left, top to bottom: the peak and the volume in
  // dB, pan, then the activator, S and arm stacked. Right: the fader beside a meter with its dB
  // scale, the rest of the strip's height.
  // Checkpoint 55 (gap 2): the In/Out rows across the top of every strip while shown (blank on Main and
  // the returns, so the faders line up), when the strips are tall enough for them.
  // Checkpoint 56: the labelled rows on tall strips; the compact three rows (names in the hints) on lower ones.
  // Checkpoint 57 (N56-07): at the default height the labelled From and To (kMidH), not the bare rows.
  const float ioH = !liveio::shown(a) ? 0.0f : mh >= 360.0f ? liveio::kStripH : mh >= kStripIoMinH ? liveio::kMidH + 8.0f : 0.0f;
  if (ioH > 0 && !master && !letter && t >= 0) {
    liveio::drawBlock(a, dl, t, ImVec2(x + 3, my0 + 4), ImVec2(x + w - 3, my0 + ioH - 4), ImGui::IsWindowHovered());
  } else if (ioH > 0 && mx) {  // checkpoint 56 (N55-10): a return's Audio To, Main's Cue Out and Master Out
    liveio::drawMixerBlock(a, dl, mx->id, ImVec2(x + 3, my0 + 4), ImVec2(x + w - 3, my0 + ioH - 4), ImGui::IsWindowHovered());
  }
  const float bottom = my0 + mh, top = my0 + 4 + ioH + (sendsRow ? sendsHeight(s) : 0.0f);
  const float colL = std::min(44.0f, std::floor(w * 0.48f));  // the left column's width
  const float rx = x + colL + 4;                               // the right column
  if (sendsRow && mx && !master && !slim) {
    const float sy = my0 + 4 + ioH;
    const auto sends = sendsOf(s, mx);
    if (!sends.empty()) {
      const char* lab = i18n::tr("Sends");
      const float lw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, lab).x;
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(x + w - 4 - lw), std::round(sy)), P.textDim, lab);
    }
    constexpr float kR = 10.0f;
    for (size_t i = 0; i < sends.size(); ++i) {
      const int k = sends[i].k, dest = sends[i].dest;
      const float col = static_cast<float>(i % 3), rowN = static_cast<float>(i / 3);
      const ImVec2 kp(x + 4 + col * (2 * kR + 6), sy + col * 8.0f + rowN * 30.0f);
      ImGui::SetCursorScreenPos(kp);
      ImGui::PushID(1000 + dest);  // by return (not by send), so a knob keeps its id when its send is made
      if (sends[i].routed) {
        // N55-01: the track plays into this return: its send there is off, greyed, its level kept.
        // Checkpoint 57 (N56-02): drawn dimmed, as the reference's: no value arc, faint body and pointer.
        const KeptSend* kept = trackmodes::keptSend(s, mx->id, s.mixer[static_cast<size_t>(dest)].id);
        offSendKnob(dl, kp, kR, kept ? kept->level / 1.25f : 0.0f);
        ImGui::InvisibleButton("##send", ImVec2(2 * kR, 2 * kR));
      } else if (k >= 0) {
        ui::ParamKnob("##send", sendParam(mx->id, dest), kR, nullptr, false);
      } else {
        missingSendKnob(a, mx->id, dest, kR);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        a.setHint(sends[i].routed ? std::string(i18n::tr("Send to")) + " " + s.mixer[static_cast<size_t>(dest)].name + ": " +
                                        i18n::tr("off while the track plays into it (Audio To); its level is kept")
                                  : std::string(i18n::tr("Send to")) + " " + s.mixer[static_cast<size_t>(dest)].name);
      }
      if (const char lt = returnLetter(s, dest)) {
        // Checkpoint 59 (N58-10): the return's letter beside the knob's lower right, clear of its ring and
        // value arc (it sat on the ring's foot), in the gap before the next knob.
        const char ls[2] = {lt, 0};
        const float fz = 9.0f;
        dl->AddText(a.fontRegular, fz, ImVec2(std::round(kp.x + 2 * kR), std::round(kp.y + 2 * kR - fz + 1)), P.textDim, ls);
      }
      ImGui::PopID();
    }
  }
  // The peak since it was last cleared (click it to clear), then the volume (dB) under it, each
  // value centred, as the refs' strips (357, 280, 170, 143).
  // K-112c (round 44): a short strip (a small window or a large interface scale) keeps what fits,
  // from the most used down: the volume field at the top, the fader and meter, the activator; then
  // S, arm, the peak and pan as the height allows. Nothing is drawn over anything else.
  const bool compact = mh < 126.0f;
  // Volume (dB). K-112c (round 44): a track with no mixer track yet has none (empty02, 069: an empty
  // MIDI track's strip is bare but for its meter and buttons).
  const float volTop = compact ? top : top + 18;
  if (mx && !slim) {
    const ImVec2 v0(x + 2, volTop), v1(x + colL, volTop + 16);
    dl->AddRectFilled(v0, v1, P.dark ? P.field : P.surface950, theme::kRadiusSm);
    char vb[16];
    if (mx->volume > 1e-6f) std::snprintf(vb, sizeof vb, "%.1f", faderDb(mx->volume));
    else std::snprintf(vb, sizeof vb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
    const float tw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, vb).x;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((v0.x + v1.x - tw) * 0.5f), theme::textTop(a.fontRegular, theme::kFontLabel, volTop, 16)),
                P.text, vb);
  }
  // The left column from the bottom up: arm, S, the activator, then pan.
  // K-112c (round 28): the strip ends in a bar of the track's colour (143, 146, 280, 141), so the
  // controls stand on it with a gap.
  constexpr float kFootH = 3.0f, kFootGap = 4.0f;
  const float base = bottom - kFootH - kFootGap;
  float armY = base - 15, soloY = armY - 17, actY = soloY - 21;
  float panY = actY - 27, panR = 11.0f;
  // In a short strip only the buttons that fit under the volume field are drawn, the activator
  // first, then S, then arm (all three stay in the arrangement's headers).
  const float minY = volTop + (mx ? 20.0f : 0.0f);
  int nb = 3;
  if (compact) {
    const float room = base - minY;
    nb = room >= 53 ? 3 : room >= 36 ? 2 : room >= 19 ? 1 : 0;
    if (nb == 2) soloY = base - 15, actY = soloY - 21;
    if (nb == 1) actY = base - 19;
  }
  // Checkpoint 59 (N58-04): a short strip (In/Out shown at 1440) keeps Pan above the buttons when all
  // three fit, a size smaller when the full knob does not (Pan is a first-class control).
  if (compact && nb == 3) panY = actY - 27;
  if (panY < minY && actY - 21 >= minY) panY = actY - 21, panR = 9.0f;
  const bool showPan = !slim && nb == 3 && panY >= minY, fitSolo = nb >= 2, fitArm = nb >= 3 && !trackmodes::isGroupTrack(a.doc.song(), t);  // checkpoint 51: a group records nothing
  // Checkpoint 60 (N59-04): when Pan finds no room above the buttons (1024 x 640 with In/Out shown), it
  // takes the peak field's row instead, as a small knob: Pan is used far more than the peak readout.
  const bool panInPeak = mx && !compact && !showPan && !slim;
  if (mx && !compact && !panInPeak && !slim) {
    static std::map<ObjectId, float> held;  // per mixer track, linear
    float& pk = held[mx->id];
    pk = std::max(pk, mixmeters::get(a, mx->id).peak());
    const ImVec2 k0(x + 2, top), k1(x + colL, top + 16);
    ImGui::SetCursorScreenPos(k0);
    if (ImGui::InvisibleButton("##peak", ImVec2(k1.x - k0.x, k1.y - k0.y))) pk = 0;
    ui::Hint("The highest level since this was last clicked (click to clear)");
    // K-112c (round 33): a filled capsule over the square volume field, as 357 and 170 draw them (round 32
    // squared it by mistake; the refs' peak is round, the volume square).
    const float kr = (k1.y - k0.y) * 0.5f;
    dl->AddRectFilled(k0, k1, P.dark ? P.field : P.surface950, kr);
    dl->AddRect(k0, k1, pk >= 1.0f ? P.arm : theme::mix(P.dark ? P.field : P.surface950, IM_COL32(0, 0, 0, 255), 0.35f), kr, 0, 1.0f);
    char pb[16];
    if (pk > 1e-6f) std::snprintf(pb, sizeof pb, "%.1f", static_cast<double>(20.0f * std::log10(pk)));
    else std::snprintf(pb, sizeof pb, "-\xE2\x88\x9E");  // minus infinity, as the refs write it
    const float pw = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, 0, pb).x;
    dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round((k0.x + k1.x - pw) * 0.5f), theme::textTop(a.fontRegular, theme::kFontLabel, top, 16)),
                pk >= 1.0f ? P.arm : P.textDim, pb);
  }
  // Fader and meter of the mixer track, with the scale on the right.
  const float fy = top, fh = std::max(24.0f, bottom - kFootH - kFootGap - 1 - top);
  if (mx) {
    if (!slim) {  // GitHub #58: a narrow strip keeps only its meter
      ImGui::SetCursorScreenPos(ImVec2(rx, fy));
      ui::setFaderPointer(true);  // K-112c: a pointer beside the meter, as the refs' strips
      ui::ParamFader("##fader", mixerParam(mx->id, MixerParam::Volume), ImVec2(14, fh));
      ui::setFaderPointer(false);
    }
    const mixmeters::Reading r = mixmeters::get(a, mx->id);
    ImGui::SetCursorScreenPos(ImVec2(rx + 16, fy));
    const std::string what = "strip " + std::to_string(mx->id);
    clipsMeterItem(mx->id, r.peakL, r.peakR, ImVec2(10, fh), what.c_str());
    // The meter's dB scale (K-383: 0 dB at the top, -60 dB at the bottom, top weighted, as the meter
    // draws it: clipsmeter::pos): a tick and a number every 12 dB, the numbers only where there is room
    // for them (and never two numbers on top of each other).
    const float fsz = 9.0f, sx = rx + 28;
    const int step = fh >= 90 ? clipsmeter::kTickStepDb : 2 * clipsmeter::kTickStepDb;
    float lastLabel = -1e9f;
    for (int db = 0; db >= static_cast<int>(clipsmeter::kFloorDb); db -= step) {
      const float ty = std::round(fy + fh - clipsmeter::pos(static_cast<float>(db)) * fh) + 0.5f;
      dl->AddLine(ImVec2(sx, ty), ImVec2(sx + 3, ty), P.textFaint);
      if (fh >= 48 && sx + 4 + 10 <= x + w + 2 && ty - lastLabel >= fsz) {
        lastLabel = ty;
        const std::string n = std::to_string(-db);
        dl->AddText(a.fontRegular, fsz, ImVec2(sx + 4, std::round(ty - fsz * 0.5f)), P.textFaint, n.c_str());
      }
    }
    // Pan: the mixer's own knob (undo, automation, links as there), as large as the refs' (143, 146, 280).
    const float kc = x + 2 + (colL - 2) * 0.5f;  // the left column's centre
    if (showPan) {
      ImGui::SetCursorScreenPos(ImVec2(kc - panR, panY));
      ui::ParamKnob("##pan", mixerParam(mx->id, MixerParam::Pan), panR, nullptr, true);
      ui::Hint("Pan");
    } else if (panInPeak) {  // N59-04: in the peak field's row
      ImGui::SetCursorScreenPos(ImVec2(kc - 8.0f, top));
      ui::ParamKnob("##pan", mixerParam(mx->id, MixerParam::Pan), 8.0f, nullptr, true);
      ui::Hint("Pan");
    }
  } else {
    // K-112c (round 44): no mixer track yet: a dotted, unlit meter where the fader and meter stand
    // (empty02, 069), and a hint saying how the track gets one.
    // A track whose clips play into several mixer tracks shows their loudest level there instead
    // (read only: each has its own fader in the mixer).
    std::vector<ObjectId> outs = t >= 0 ? trackmodes::playedMixers(s, t) : std::vector<ObjectId>{};
    if (outs.size() == 1 && outs[0] == s.masterId()) outs.clear();  // (a note-only MIDI track: nothing of its own)
    const float mx0 = rx + 16, my1 = fy + fh;
    if (outs.empty()) {
      for (float yy = fy + 2; yy + 2 <= my1; yy += 5) dl->AddRectFilled(ImVec2(mx0 + 3, yy), ImVec2(mx0 + 6, yy + 2), theme::withAlpha(P.textFaint, 0.55f));
    } else {
      float pl = 0, pr = 0;
      for (ObjectId id : outs) {
        const mixmeters::Reading r = mixmeters::get(a, id);
        pl = std::max(pl, r.peakL), pr = std::max(pr, r.peakR);
      }
      ImGui::SetCursorScreenPos(ImVec2(mx0, fy));
      clipsMeterItem((uint64_t{3} << 62) | static_cast<uint64_t>(t + 1), pl, pr, ImVec2(10, fh), "strip outs");
    }
    ImGui::SetCursorScreenPos(ImVec2(rx, fy));
    ImGui::InvisibleButton("##nomixer", ImVec2(std::max(1.0f, x + w - rx), std::max(1.0f, fh)));
    ui::Hint(outs.empty() ? "No mixer track yet: put an instrument or an audio clip on this track to give it one"
                          : "This track plays into several mixer tracks: the loudest of them (each has its fader in the mixer)");
  }
  // Activator (number), S, arm, stacked: the same controls as the arrangement's headers.
  const float bx0 = x + 2, bx1 = x + colL;
  if (letter && nb >= 1) {  // K-112c (round 19): a return's activator is its letter (143, 141, 280)
    const float cxl = std::round((bx0 + bx1) * 0.5f);
    const Cell ca{ImVec2(cxl - 10, actY), ImVec2(cxl + 10, actY + 18)};  // K-112c (round 28): square, as S is wide
    const bool on = mx && !mx->muted;
    square(dl, ca, on ? P.hot : P.surface900, ImGui::IsWindowHovered() && ca.contains(m));
    const char ls[2] = {letter, 0};
    centredText(dl, a.fontRegular, theme::kFontLabel, ca, on ? theme::contrastText(P.hot) : P.textFaint, ls);
    ImGui::SetCursorScreenPos(ca.a);
    if (ImGui::InvisibleButton("##ract", ImVec2(ca.b.x - ca.a.x, ca.b.y - ca.a.y)) && mx)
      a.doc.setParam(mixerParam(mx->id, MixerParam::Mute), mx->muted ? 0.0 : 1.0, false);
    ui::Hint("Return track: turn it on or off");
  } else if (!master && !letter && nb >= 1) {
    const Song& ss = a.doc.song();
    const PlaylistTrack& tr = ss.tracks[static_cast<size_t>(t)];
    const bool hovAny = ImGui::IsWindowHovered();
    const float cxm = std::round((bx0 + bx1) * 0.5f);
    const Cell ca{ImVec2(cxm - 10, actY), ImVec2(cxm + 10, actY + 18)};  // K-112c (round 28): square, as S is wide (143, 146, 280, 141)
    const Cell cs{ImVec2(cxm - 10, soloY), ImVec2(cxm + 10, soloY + 14)};
    const Cell cr{ImVec2(cxm - 10, armY), ImVec2(cxm + 10, armY + 14)};
    const std::string numS = std::to_string(trackmodes::shownNumber(ss, t));  // N54-04: as Clips counts (after any layout's moves)
    const char* num = numS.c_str();
    square(dl, ca, !tr.muted ? P.hot : P.surface900, hovAny && ca.contains(m));
    centredText(dl, a.fontRegular, theme::kFontLabel, ca, !tr.muted ? theme::contrastText(P.hot) : P.textFaint, num);
    const bool so = soloed(ss, t);
    if (fitSolo) {
      square(dl, cs, so ? P.solo : P.surface900, hovAny && cs.contains(m));
      centredText(dl, a.fontBold, theme::kFontLabel, cs, so ? theme::contrastText(P.solo) : P.textDim, "S");
    }
    const bool audio = trackmodes::modeOf(ss, t) == TrackMode::Audio;
    bool armed = audio && mx && mx->armed;
    // Checkpoint 48: the selected MIDI track is armed for notes (its empty slots record), drawn lit softer.
    const bool midiT = !audio && midiTrack(ss, t), handArm = midiT && midiArmedByHand(t);
    const bool autoArm = midiT && !handArm && trackArmed(a, t);
    if (handArm) armed = true;  // N47-10: a MIDI track's own arm
    if (fitArm) square(dl, cr, armed ? theme::withAlpha(P.arm, 0.85f) : autoArm ? theme::withAlpha(P.arm, 0.45f) : P.surface900, (audio || midiT) && hovAny && cr.contains(m));
    if (fitArm) dl->AddCircleFilled(ImVec2((cr.a.x + cr.b.x) * 0.5f, (cr.a.y + cr.b.y) * 0.5f), 3.5f, armed || autoArm ? theme::contrastText(P.arm) : audio ? P.text : theme::withAlpha(P.textDim, 0.45f), 12);  // K-112c (round 19): a neutral dot until armed (142, 157, 143)
    ImGui::SetCursorScreenPos(ca.a);
    if (ImGui::InvisibleButton("##act", ImVec2(ca.b.x - ca.a.x, ca.b.y - ca.a.y))) clickHeader(a, t, HeaderPart::Activator, false);
    ui::Hint(headerHint(HeaderPart::Activator));
    if (fitSolo) {
      ImGui::SetCursorScreenPos(cs.a);
      if (ImGui::InvisibleButton("##solo", ImVec2(cs.b.x - cs.a.x, cs.b.y - cs.a.y))) clickHeader(a, t, HeaderPart::Solo, false);
      ui::Hint(headerHint(HeaderPart::Solo));
    }
    if (fitArm) {
      ImGui::SetCursorScreenPos(cr.a);
      if (ImGui::InvisibleButton("##arm", ImVec2(cr.b.x - cr.a.x, cr.b.y - cr.a.y)) && (audio || midiT)) clickHeader(a, t, HeaderPart::Arm, false);
      ui::Hint(audio ? headerHint(HeaderPart::Arm) : i18n::tr("Record arm: an armed MIDI track records notes into its empty slots (the selected one is armed by itself; Cmd arms several)"));
    }
  }
  // K-112c (round 32): Main's strip no longer draws a "Main" box where the tracks have their
  // activator: it looked like a button and did nothing (its title already names it).
  {  // the colour bar along the foot
    const bool trk = !master && !letter && t >= 0 && t < static_cast<int>(s.tracks.size());
    const ImU32 cc = trk ? theme::content(s.tracks[static_cast<size_t>(t)].color) : mx ? theme::content(mx->color) : P.surface600;
    dl->AddRectFilled(ImVec2(x, std::round(bottom - kFootH)), ImVec2(x + w, std::round(bottom)), cc);
  }
  // hunt-clips4-1119: a plugin dragged from the browser onto a strip goes where its title would put it
  // (a track's instrument, an effect on its mixer track); on a return or Main, an effect into its first
  // free slot. The browser's hint offers the strips, and only the titles took the drop (Main none).
  if (ImGui::GetDragDropPayload() && (t >= 0 || mx) && ImGui::BeginDragDropTargetCustom(ImRect(x, my0, x + w, bottom), ImGui::GetID("##stripdrop"))) {
    browserui::DropPlace place;
    if (t >= 0 && !master && !letter) {
      place.track = t;
      browserui::acceptPluginDrop(a, browse::DropTarget::Playlist, place);
    } else if (mx) {
      place.mixer = mx->id;
      if (browserui::acceptPluginDrop(a, browse::DropTarget::MixerTrack, place)) {
        busEffectAdded(a, mx->id);
      }
    }
    ImGui::EndDragDropTarget();
  }
  stripMenu();  // GitHub #58
  ImGui::PopID();
}

}  // namespace

void drawClipGrid(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImGui::BeginChild("##clipgrid", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  if (gDetailFocusFrames > 0 || (gDetailWant == 1 && gDetailFocus) || ImGui::GetFrameCount() - gDetailKeysFrame < 8)
    gSelFocus = 0;  // 0.7.8: a double-click gives the keys to the clip view
  if (gSelFocus > 0) {  // C44-12: the keys stay on the grid while the clip band opens for the selected clip
    --gSelFocus;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) ImGui::SetWindowFocus();
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float dpp = pixelgrid::frameScale();  // K-276: every cell edge on a device pixel, at any interface scale
  const ImVec2 o(livegrid::snap(ImGui::GetCursorScreenPos().x, dpp), livegrid::snap(ImGui::GetCursorScreenPos().y, dpp));
  const ImVec2 av = ImGui::GetContentRegionAvail();
  const int nCols = gridColumns(s);
  const ColMap cm = colMap(s, nCols);  // checkpoint 51: folded groups' members take no column
  auto vc = [&](int t) { return t < nCols ? cm.col[static_cast<size_t>(t)] : cm.n + (t - nCols); };
  auto hiddenCol = [&](int t) { return t >= 0 && t < nCols && cm.hidden[static_cast<size_t>(t)]; };
  auto groupCol = [&](int t) { return t >= 0 && t < nCols && cm.group[static_cast<size_t>(t)]; };
  // Track columns keep one width whatever the window's (spare width stays free between the last
  // column and Main, which sits at the right edge). K-112c (round 19): one gap everywhere: kGap
  // between columns, between slot rows, under the titles and above the stop row (18 px cells).
  // (LiveGrid.h holds the geometry, so a test measures the gaps.)
  using livegrid::kPad, livegrid::kTitleH, livegrid::kGap, livegrid::kSlotH, livegrid::kCellH;
  constexpr float kMainW = 140, colW = livegrid::kColW;
  // hunt-clips2-1119: the scenes' frozen length (grid2-1118), as every slot edit uses: after a scene set 3/4 the
  // grid re-cut the live area by the new meter's bars and showed clips in the wrong rows.
  const Tick sceneT = sceneTicks(s);
  // Slots: the live area's clips by scene.
  int nScenes = s.live.areaEnd > 0 ? static_cast<int>((s.live.areaEnd + sceneT - 1) / sceneT) : 0;
  std::vector<std::vector<std::vector<int>>> slot(static_cast<size_t>(nCols));
  for (int t = 0; t < nCols; ++t) {
    slot[static_cast<size_t>(t)].resize(static_cast<size_t>(std::max(nScenes, 0)));
    for (int ci : liveClips(s, t)) {
      const int k = static_cast<int>(s.playlist[static_cast<size_t>(ci)].start / sceneT);
      if (k >= 0 && k < nScenes) slot[static_cast<size_t>(t)][static_cast<size_t>(k)].push_back(ci);
    }
  }
  // The mixer at the foot of the grid: compact strips (volume, fader and meter, pan and sends,
  // then the activator, S and arm), 110 to 260 px (round 24: about 45 % of the panel, so the sends show at 1440 as in 143, 112, 093). The slots fill the panel with
  // empty rows down to the mixer (round 25: 276, 355), every scene in use always shown.
  // K-112c (round 44): with the mixer band open under the grid (the Mixer shown in Clips) the strips
  // are drawn there only, not twice; the slots take the grid's whole height.
  const bool footMixer = !(a.showMixer && !a.showStage);
  float mixH = footMixer ? std::clamp(av.y * 0.45f, 110.0f, 260.0f) : 0.0f;
  // Checkpoint 58 (N57-03): on a short window the strips take the height their sends row (and the In/Out
  // rows, while shown: View > In/Out, Cmd+Alt+I) need, as long as the grid keeps its titles, two scene
  // rows and the stop row (the rows scroll). What still does not fit says so (stripHiddenNote).
  if (footMixer) {
    const float gridMin = kPad * 2 + kTitleH + kGap + 3 * kSlotH;
    const float want = (liveio::shown(a) ? kStripIoMinH : 131.0f) + (anySends(s) ? sendsHeight(s) : 0.0f);
    mixH = std::max(mixH, std::min(want, av.y - gridMin));
  }
  const int rowsFit = std::max(1, static_cast<int>((av.y - kPad * 2 - kTitleH - kGap - mixH - kSlotH) / kSlotH));  // and the stop row
  // N46-12 (C44-23 part 3): a set shows its scenes, and at least the reference new set's eight; the
  // panel under them stays clear down to the stop row (no longer as many rows as the window holds).
  // Checkpoint 58 (N57-05): and empty slot rows down to the stop row, as the reference's (no dead band
  // under the last scene on a tall window); they are drawn, not stored.
  const int rows = std::max({nScenes, 8, rowsFit});
  const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(a.now) * 8.0f);
  static ObjectId held = kNoId;
  static std::pair<int, int> dragFrom{-1, -1};  // K-287: the slot (track, scene) a press started on
  static bool keepMulti = false;                 // grid2-1118: the press kept a multi-slot selection
  static bool banding = false;                   // grid2-1118: a rubber band from an empty slot
  static ImVec2 bandFrom;
  static int titleDrag = -1;                     // K-112c (round 39): a column dragged by its title (moves the track)
  const ImVec2 m = ImGui::GetIO().MousePos;
  // N45-04: only the scene rows scroll up and down: the titles stay at the top and the stop row and
  // the mixer under them at the foot, as the reference's Session view (101, 143). `oyPin` is the
  // grid's origin with the vertical scroll taken out; the rows keep o.y.
  const float scrollY = ImGui::GetScrollY();
  const float oyPin = o.y + scrollY;
  const float y0 = livegrid::titleY(oyPin, dpp), yT1 = livegrid::titleBottom(oyPin, dpp), titleH = yT1 - y0;
  const pixelgrid::Span stopSpan = livegrid::row(oyPin, rowsFit, dpp);  // the stop row, pinned under the rows that fit
  const float slotsY0 = yT1 + kGap, slotsY1 = stopSpan.x0 - kGap;      // where scene rows show
  auto rowShown = [&](const pixelgrid::Span& rs) { return rs.x1 > slotsY0 && rs.x0 < slotsY1; };
  const float fs = theme::kFontCaption + 1;
  auto column = [&](int t) { return livegrid::columnX(o.x, vc(t), dpp); };
  // C44-19: Main (the scenes and Stop all) stands pinned at the window's right edge and the returns
  // beside it, as 101, 112 and 357; the track columns scroll under them (clipped at the returns), and
  // the content is as wide as the tracks, a free column and the pinned part, so the last track
  // scrolls into view next to them. (K-112c round 36: the shift is whole columns' worth of snapped
  // space, so each return keeps the K-276 column span and gaps.)
  const float scrollX = ImGui::GetScrollX();
  gGridScrollX = scrollX;
  const float visX0 = o.x + scrollX;
  const float mainX = livegrid::snap(visX0 + av.x - kPad - kMainW + 4, dpp);
  float retShift = 0.0f;
  int nFit = 0;
  {
    const std::vector<size_t> rets = shownReturns(s);
    nFit = returnsShown(a) ? std::min<int>(static_cast<int>(rets.size()), 26) : 0;
    if (nFit > 0) retShift = livegrid::snap(mainX - column(nCols + nFit), dpp);
  }
  auto retColumn = [&](int i) { return livegrid::column(o.x + retShift, cm.n + i, dpp); };
  const float pinX0 = nFit > 0 ? retColumn(0).x0 : mainX;  // the pinned part's left edge
  const float trackClipX1 = pinX0 - kGap;                    // track columns end here
  // K-112c (round 43): the free column between the last track and the returns (or Main), where
  // files and instruments dropped make new tracks (101, 069, 375, empty02).
  const float freeX0 = column(nCols), freeX1 = trackClipX1;
  // N47-18: a column cut by the pinned part to a sliver (under half its width) is not shown at all:
  // the last whole column ends where the returns begin (455, 457).
  float gridClipX1 = trackClipX1;
  for (int t = 0; t < nCols; ++t) {
    const pixelgrid::Span cs = livegrid::column(o.x, vc(t), dpp);
    if (cs.x0 < trackClipX1 && cs.x1 > trackClipX1 && trackClipX1 - cs.x0 < cs.w() * 0.5f) gridClipX1 = cs.x0;
  }
  ImGui::PushClipRect(ImVec2(visX0, -1e6f), ImVec2(std::max(visX0, gridClipX1), 1e6f), true);  // the track columns (popped after the stop row)
  for (int t = 0; t < nCols; ++t) {
    if (hiddenCol(t)) continue;
    const PlaylistTrack& tr = s.tracks[static_cast<size_t>(t)];
    const pixelgrid::Span cs = livegrid::column(o.x, vc(t), dpp);
    const float x = cs.x0, w = cs.w();  // K-112c: columns kGap apart, square slots, as the refs
    ImGui::PushID(t);
    // Title: the track's colour and name (a click selects the track, as a header does).
    const ImU32 tc = theme::content(tr.color);
    const bool sel = a.playlist.trackSel.count(t) > 0;
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, yT1), tc, 0.0f);
    // K-112c (round 32): a peer's pointer on this track's header (arrangement, any layout) lands on its title here.
    canchor::publishRect(canchor::PlaylistHeader, static_cast<uint32_t>(t), 0, ImVec2(x, y0), ImVec2(x + w, yT1));
    if (sel) {  // K-276: the ring stroked wholly inside the title
      const float in = pixelgrid::strokeIn(1.5f);
      dl->AddRect(ImVec2(x + in, y0 + in), ImVec2(x + w - in, yT1 - in), P.text, 0.0f, 1.5f, 0);
    }
    const std::string name = playlistTrackName(s, static_cast<size_t>(t));  // K-112c (round 30): the name alone (357, 093, 143)
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), w - (groupCol(t) ? 26 : 10), theme::contrastText(tc), name.c_str());
    const bool onFold = groupCol(t) && m.x >= x + w - 18 && m.y >= y0 && m.y < yT1;
    if (groupCol(t)) {  // checkpoint 51: the group's fold button at the title's right end
      const float fx = x + w - 16, cy = std::round((y0 + yT1) * 0.5f);
      const ImU32 ink = theme::withAlpha(theme::contrastText(tc), onFold ? 1.0f : 0.8f);
      // A ringed fold button (149, 172): pointing right while folded, down while open.
      dl->AddCircle(ImVec2(fx + 7.5f, cy), 6.5f, ink, 20, 1.2f);
      if (tr.gridFolded) dl->AddTriangleFilled(ImVec2(fx + 6, cy - 3), ImVec2(fx + 6, cy + 3), ImVec2(fx + 10, cy), ink);
      else dl->AddTriangleFilled(ImVec2(fx + 4.5f, cy - 1.5f), ImVec2(fx + 10.5f, cy - 1.5f), ImVec2(fx + 7.5f, cy + 2.5f), ink);
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y0));
    if (gRenTrack == t) {  // checkpoint 49: Rename: the title is a name field until Enter, Escape or a click away
      static char nbuf[128];
      if (gRenTrackFrames > 0) {
        std::snprintf(nbuf, sizeof nbuf, "%s", name.c_str());
        if (--gRenTrackFrames == 0) ImGui::SetKeyboardFocusHere();
      }
      ImGui::SetNextItemWidth(w);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, std::max(0.0f, (titleH - ImGui::GetFontSize()) * 0.5f)));
      ImGui::InputText("##trname", nbuf, sizeof nbuf, ImGuiInputTextFlags_AutoSelectAll);
      ImGui::PopStyleVar();
      const bool done = ImGui::IsItemDeactivated();
      gRenTrackIdle = ImGui::IsItemActive() || gRenTrackFrames > 0 ? 0 : gRenTrackIdle + 1;
      if (done && !ImGui::IsKeyPressed(ImGuiKey_Escape) && name != nbuf && nbuf[0]) {
        const std::string nn = nbuf;
        trackmodes::renameTrack(a.doc, t, nn);  // N52-06: its mixer track follows
      }
      if (done || gRenTrackIdle > 3) gRenTrack = -1;
      ImGui::SetCursorScreenPos(ImVec2(x, y0));
      ImGui::Dummy(ImVec2(w, titleH));
    } else if (ImGui::InvisibleButton("##title", ImVec2(w, titleH))) {
      if (onFold) {
        const bool f = !tr.gridFolded;  // S01: the grid folds on its own, as the reference's views
        a.later([&a, t, f] {
          a.doc.editPlaylistTrack(t, f ? "fold group" : "unfold group", [f](PlaylistTrack& x) { x.gridFolded = f; });
          foldedSelection(a, t, true);
        });
      } else {
        // Checkpoint 51: Shift adds the tracks between (from the first selected), Cmd adds or takes
        // one, as the reference's titles (the selection Cmd+G groups).
        const ImGuiIO& io = ImGui::GetIO();
        static int anchor = -1;  // S06: Shift ranges from the last title clicked, as the reference's
        if (io.KeyShift && !a.playlist.trackSel.empty()) {
          const int from = a.playlist.trackSel.count(anchor) ? anchor : *a.playlist.trackSel.begin();
          a.playlist.trackSel.clear();
          for (int u = std::min(from, t); u <= std::max(from, t); ++u)
            if (!hiddenCol(u)) a.playlist.trackSel.insert(u);
          if (a.playlist.trackSel.empty()) a.playlist.trackSel = {t};
          anchor = from;
        } else if ((io.KeySuper || io.KeyCtrl) && !a.playlist.trackSel.empty()) {
          if (!a.playlist.trackSel.erase(t)) a.playlist.trackSel.insert(t);
          anchor = t;
        } else {
          a.playlist.trackSel = {t};
          anchor = t;
        }
        gSel = {};  // checkpoint 52 (N51-07): the title has the keys (Delete, Cmd+D), not a slot
        focusTrack(a, t);
        trackChosen();  // 0.7.8: the detail spot shows the track's devices
      }
    }
    if (ImGui::BeginPopupContextItem("##trlive")) {
      playlistTrackMenu(a, t);  // the track's whole menu (its launch settings under Live settings)
      ImGui::EndPopup();
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) titleDrag = t;  // K-112c (round 39)
    ui::Hint(onFold          ? "Fold: shows or hides the group's tracks"
             : groupCol(t) ? "The group: click selects it (its devices show below), drag to move it with its tracks, right-click: its menu"
                           : "The track: click selects it (its devices show below), drag to move it, right-click: the track's menu");
    if (groupCol(t)) {  // checkpoint 51: a group's slots launch its tracks' clips of the scene
      const int g1 = std::min(nCols - 1, t + tr.groupSize);
      ImGui::PushClipRect(ImVec2(visX0, slotsY0), ImVec2(std::max(visX0, trackClipX1), std::max(slotsY0, slotsY1)), true);
      for (int k = 0; k < rows; ++k) {
        const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
        if (!rowShown(rs)) continue;
        const ImVec2 q0(x, rs.x0), q1(x + w, rs.x1);
        std::vector<const Clip*> mem;
        bool any = false, anyOn = false, anyQueued = false;
        for (int u = t + 1; u <= g1; ++u) {
          const std::vector<int>* in = k < nScenes ? &slot[static_cast<size_t>(u)][static_cast<size_t>(k)] : nullptr;
          const Clip* c = in && !in->empty() ? &s.playlist[static_cast<size_t>(in->front())] : nullptr;
          mem.push_back(c);
          any = any || c;
          anyOn = anyOn || (c && a.engine.liveActive(u) == c->id);
          anyQueued = anyQueued || (c && a.engine.liveQueued(u) == c->id);
        }
        ImGui::PushID(k);
        ImGui::SetCursorScreenPos(q0);
        ImGui::InvisibleButton("##gslot", ImVec2(w, q1.y - q0.y));
        const bool hov = ImGui::IsItemHovered(), onButton = m.x < q0.x + 16.5f;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && onButton)
          for (int u = t + 1; u <= g1; ++u) {
            const Clip* c = mem[static_cast<size_t>(u - t - 1)];
            if (c) launch(a, c->id);
            else a.engine.stopLiveTrack(u);
          }
        const float cyy = std::round((q0.y + q1.y) * 0.5f);
        if (any) {
          // As the reference's group slot (149, 172): a plain cell a shade up from an empty slot, its
          // launch button, and a small hatched square in the group's colour at the right end.
          const ImU32 base = P.surface600;
          dl->AddRectFilled(q0, q1, hov && !onButton ? theme::mix(base, P.paper, 0.08f) : base, 0.0f);
          const float ri = pixelgrid::strokeIn(2.0f);
          if (anyOn) dl->AddRect(ImVec2(q0.x + ri, q0.y + ri), ImVec2(q1.x - ri, q1.y - ri), P.launch, 0.0f, 0, 2.0f);
          else if (anyQueued) dl->AddRect(ImVec2(q0.x + ri, q0.y + ri), ImVec2(q1.x - ri, q1.y - ri), theme::withAlpha(P.launch, 0.35f + 0.65f * pulse), 0.0f, 0, 2.0f);
          {
            const float hs = std::round(q1.y - q0.y - 6), hx1 = std::round(q1.x - 4), hx0 = hx1 - hs, hy0 = std::round(q0.y + 3);
            const ImU32 gc = theme::content(tr.color);
            dl->AddRectFilled(ImVec2(hx0, hy0), ImVec2(hx1, hy0 + hs), theme::mix(gc, base, 0.55f), 0.0f);
            dl->PushClipRect(ImVec2(hx0, hy0), ImVec2(hx1, hy0 + hs), true);
            for (float d = -hs; d < hs; d += 4.0f) dl->AddLine(ImVec2(hx0 + d, hy0 + hs), ImVec2(hx0 + d + hs, hy0), gc, 1.6f);
            dl->PopClipRect();
          }
          const ImU32 ink = theme::contrastText(base);
          ui::drawIconFit(dl, ui::Icon::Play, ImVec2(q0.x + 6, cyy - 4.5f), ImVec2(q0.x + 13, cyy + 4.5f),
                          anyOn ? P.launch : theme::withAlpha(ink, hov && onButton ? 1.0f : 0.6f));
          dl->AddLine(ImVec2(q0.x + 16.5f, q0.y), ImVec2(q0.x + 16.5f, q1.y), theme::withAlpha(IM_COL32(16, 18, 22, 255), 0.35f));
          ui::Hint(onButton ? "Launch: plays the group's clips in this scene (its tracks with an empty slot stop)" : "Group slot: the clips of the group's tracks in this scene");
        } else {
          dl->AddRectFilled(q0, q1, hov ? P.surface700 : P.surface800, 0.0f);
          dl->AddRectFilled(ImVec2(q0.x + 6, cyy - 3), ImVec2(q0.x + 12, cyy + 3), hov && onButton ? P.text : hov ? P.textDim : P.textFaint);
          ui::Hint(onButton ? "Stop: stops the group's tracks" : "Group slot: none of the group's tracks has a clip in this scene");
        }
        if (gSel.t == t && gSel.k == k) dl->AddRect(q0, q1, P.text, 0.0f, 0, 1.5f);  // S11: the selected (keyboard) slot
        ImGui::PopID();
      }
      ImGui::PopClipRect();
      ImGui::PopID();
      continue;
    }
    const ObjectId act = a.engine.liveActive(t), que = a.engine.liveQueued(t);
    const bool autoArmed = trackArmed(a, t) && !s.tracks[static_cast<size_t>(t)].locked;  // N47-10: its empty slots record
    ImGui::PushClipRect(ImVec2(visX0, slotsY0), ImVec2(std::max(visX0, trackClipX1), std::max(slotsY0, slotsY1)), true);  // N45-04
    for (int k = 0; k < rows; ++k) {
      const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
      if (!rowShown(rs)) continue;
      const float sy = rs.x0;
      const ImVec2 q0(x, sy), q1(x + w, rs.x1);
      ImGui::PushID(k);
      ImGui::SetCursorScreenPos(q0);
      ImGui::InvisibleButton("##slot", ImVec2(w, q1.y - q0.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      const bool hov = ImGui::IsItemHovered(), pressed = ImGui::IsItemActive();  // K-112c (round 31): a press sinks the slot
      // C44-11: the launch triangle (or an empty slot's stop square) has a cell of its own at the left;
      // a press anywhere else selects the slot, opens its menu or starts a drag, and launches nothing.
      const bool onButton = m.x < q0.x + 16.5f;
      const std::vector<int>* in = k < nScenes ? &slot[static_cast<size_t>(t)][static_cast<size_t>(k)] : nullptr;
      if (ImGui::IsItemActivated() || (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        // grid2-1118: Shift-click selects the range, Cmd-click adds or removes the slot; a press on a slot
        // of a multi-slot selection keeps it (a drag moves them all; a click alone selects that slot).
        const ImGuiIO& kio = ImGui::GetIO();
        const bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        keepMulti = false;
        if (!right && !onButton && kio.KeyShift) {
          extendSelection(a, t, k);
        } else if (!right && !onButton && (kio.KeyCtrl || kio.KeySuper)) {
          toggleSelection(a, t, k);
        } else if (!gMulti.empty() && slotSelected(t, k) && !onButton && in && !in->empty()) {
          keepMulti = !right;
          gSel = {t, k};
        } else {
          selectSlot(a, t, k);
        }
        if (!right && !onButton && !kio.KeyShift && !kio.KeyCtrl && !kio.KeySuper && (!in || in->empty())) {
          bandFrom = m;  // an empty slot's body: a rubber band
          banding = true;
        }
        if (right) {
          ImGui::OpenPopup("##slotmenu");
        }
      }
      const bool selSlot = slotSelected(t, k);
      if (in && !in->empty()) {
        const Clip& c = s.playlist[static_cast<size_t>(in->front())];
        const bool recording = gRec.on && gRec.clip == c.id;  // checkpoint 48: Session record runs into it
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          if (onButton && recording) {
            stopSessionRecord(a);  // as the reference: its launch button ends the recording, the clip plays on
          } else if (onButton) {
            launch(a, c.id);
            held = c.id;
          }
          dragFrom = {t, k};  // K-287: dragged onto another slot, the clip moves there
        }
        if (ImGui::BeginPopup("##slotmenu")) {  // the clip's menu: its edits, then the arrangement's clip menu
          slotEditItems(a, t, k, true);
          ImGui::Separator();
          gClipMenuFromSlot = true;  // N46-08: one edit block (the slot's)
          devArrangementClipMenu(a, c.id);
          gClipMenuFromSlot = false;
          ImGui::EndPopup();
        }
        const bool on = act == c.id, waiting = que == c.id;
        // C44-12: a deactivated clip (its activator key) is drawn grey.
        const ImU32 base = c.muted ? theme::mix(theme::content(clipColour(s, c)), P.surface700, 0.75f) : theme::content(clipColour(s, c));
        const ImU32 fill = pressed && !on ? theme::mix(base, IM_COL32(0, 0, 0, 255), 0.14f) : hov && !on ? theme::mix(base, P.paper, 0.12f) : base;
        dl->AddRectFilled(q0, q1, fill, 0.0f);
        // K-276: the 2 px ring stroked wholly inside the cell (centred on its edge it reached into the gap).
        const float ri = pixelgrid::strokeIn(2.0f);
        const ImVec2 r0(q0.x + ri, q0.y + ri), r1(q1.x - ri, q1.y - ri);
        if (on) dl->AddRect(r0, r1, recording ? P.arm : P.launch, 0.0f, 2.0f, 0);  // checkpoint 48: red while it records
        // K-112c (round 32): a collaborator's change to the clip flashes its slot, as the arrangement's clips.
        if (ImU32 fc = 0; const float fl = collabui::flash(a, "playlist/#" + std::to_string(c.id), &fc))
          dl->AddRect(r0, r1, theme::withAlpha(fc, fl), 0.0f, 0, 2.0f);
        if (waiting) dl->AddRect(r0, r1, theme::withAlpha(P.launch, 0.35f + 0.65f * pulse), 0.0f, 2.0f, 0);
        if (selSlot && !on && !waiting) dl->AddRect(r0, r1, P.text, 0.0f, 0, 1.5f);  // the selected slot
        const ImU32 ink = theme::contrastText(fill);
        const float cyy = (q0.y + q1.y) * 0.5f;
        // K-112c: the launch triangle in a cell of its own (a divider), drawn soft, as the refs do.
        // N46-11: a playing clip's triangle turns the launch green (a queued one blinks it), as the reference.
        const ImU32 triCol = on        ? P.launch
                             : waiting ? theme::withAlpha(P.launch, 0.35f + 0.65f * pulse)
                                       : theme::withAlpha(ink, hov && onButton ? 1.0f : 0.6f);
        if (on || waiting || recording)  // the green reads on any clip colour
          dl->AddRectFilled(ImVec2(q0.x + 2.0f, q0.y + 2.0f), ImVec2(q0.x + 16.0f, q1.y - 2.0f), theme::mix(fill, IM_COL32(0, 0, 0, 255), 0.55f));
        if (recording)  // a recording clip's button is the record dot, as the reference's
          dl->AddCircleFilled(ImVec2(q0.x + 9.0f, cyy), 4.0f, P.arm, 16);
        else
          ui::drawIconFit(dl, ui::Icon::Play, ImVec2(q0.x + 6, cyy - 4.5f), ImVec2(q0.x + 13, cyy + 4.5f), triCol);
        dl->AddLine(ImVec2(q0.x + 16.5f, q0.y), ImVec2(q0.x + 16.5f, q1.y), theme::withAlpha(IM_COL32(16, 18, 22, 255), 0.35f));
        std::string lbl = clipLabel(s, c);
        if (in->size() > 1) lbl += " +" + std::to_string(in->size() - 1);
        const bool follows = c.launchSet.follow.on;  // grid2-1118: a clip with follow actions has a mark at its right end
        theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(q0.x + 19, theme::textTop(a.fontRegular, theme::kFontCaption, cyy, 0)), w - (follows ? 36 : 24), ink, lbl.c_str());
        if (follows) {
          const float fx = std::round(q1.x - 13), fy = std::round(cyy);
          dl->AddTriangleFilled(ImVec2(fx, fy - 4), ImVec2(fx + 7, fy), ImVec2(fx, fy + 4), theme::withAlpha(ink, 0.75f));
          dl->AddLine(ImVec2(fx - 3, fy - 4), ImVec2(fx - 3, fy + 4), theme::withAlpha(ink, 0.75f), 1.5f);
        }
        ui::Hint(onButton && recording ? "Recording: click to stop recording (the clip plays on)"
                 : onButton            ? "Launch: plays the clip at the track's launch grid (Enter launches the selected slot)"
                                       : "Clip: click selects it, drag moves it, double-click shows it in the clip band, right-click: its menu");
        if (hov && !onButton && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) clipChosen(true);  // 0.7.8: shown with the keys (once the button is up)
      } else {
        // An empty slot's stop square stops the track; the rest of it selects the slot (a
        // double-click there makes a MIDI clip on a MIDI track, C44-13).
        // Checkpoint 48: the selected MIDI track is armed for notes (the reference arms the selected
        // track for its keyboard): its empty slots show a record button, which records a new clip there.
        const bool recButton = autoArmed;
        const bool stopButton = trackmodes::hasStopButton(s, t, k);  // grid2-1118: removable (Cmd+E)
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && onButton) {
          if (gARec.on && k == gARec.slot && recButton) {
            stopSessionRecord(a);  // N48-02: its button ends the recording
          } else if (recButton) {
            a.later([&a, t, k] { startSessionRecord(a, t, k); });
          } else if (stopButton) {
            a.engine.stopLiveTrack(t);
          }
        }
        if (hov && !onButton && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.later([&a, t, k] { newMidiClip(a, t, k); });
        if (ImGui::BeginPopup("##slotmenu")) {  // C44-14: Paste, Insert MIDI clip, Insert scene
          slotEditItems(a, t, k, false);
          ImGui::EndPopup();
        }
        dl->AddRectFilled(q0, q1, pressed ? P.surface600 : hov ? P.surface700 : P.surface800, 0.0f);
        if (gARec.on && k == gARec.slot && trackmodes::modeOf(s, t) == TrackMode::Audio && trackArmed(a, t)) {
          // N48-02: the take recording here shows as a clip in the track's colour with the red record
          // button from the start, as the reference's (it lands as a clip when the recording stops).
          dl->AddRectFilled(q0, q1, theme::content(tr.color), 0.0f);
          const float ri = pixelgrid::strokeIn(2.0f);
          dl->AddRect(ImVec2(q0.x + ri, q0.y + ri), ImVec2(q1.x - ri, q1.y - ri), P.arm, 0.0f, 0, 2.0f);
          const float ry = std::round((q0.y + q1.y) * 0.5f);
          dl->AddRectFilled(ImVec2(q0.x + 2.0f, q0.y + 2.0f), ImVec2(q0.x + 16.0f, q1.y - 2.0f), theme::mix(theme::content(tr.color), IM_COL32(0, 0, 0, 255), 0.55f));
          dl->AddCircleFilled(ImVec2(q0.x + 9.0f, ry), 4.0f, P.arm, 16);
          theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(q0.x + 19, theme::textTop(a.fontRegular, theme::kFontCaption, ry, 0)), w - 24,
                              theme::contrastText(theme::content(tr.color)), i18n::tr("Recording"));
          ImGui::PopID();
          continue;
        }
        if (selSlot) {
          const float ri = pixelgrid::strokeIn(1.5f);
          dl->AddRect(ImVec2(q0.x + ri, q0.y + ri), ImVec2(q1.x - ri, q1.y - ri), P.textDim, 0.0f, 0, 1.5f);
        }
        const float cyy = std::round((q0.y + q1.y) * 0.5f);
        // The stop square on the launch triangle's centre line (both centred 9 px in), so they line up down a column.
        if (recButton) {
          dl->AddCircleFilled(ImVec2(q0.x + 9.0f, cyy), 3.5f, hov && onButton ? P.arm : theme::withAlpha(P.arm, hov ? 0.7f : 0.45f), 16);
        } else if (stopButton) {
          dl->AddRectFilled(ImVec2(q0.x + 6, cyy - 3), ImVec2(q0.x + 12, cyy + 3), hov && onButton ? P.text : hov ? P.textDim : P.textFaint);
        }
        ui::Hint(onButton && recButton ? "Record: records a new clip here (the track is armed)"
                 : onButton && !stopButton ? keys::withKeys(i18n::N_("No stop button: launching this slot or its scene leaves the track playing (%s adds it back)"), {keys::fixedKey("Cmd+E")})
                 : onButton            ? keys::withKeys(i18n::N_("Stop: stops the track's clip (%s removes the button)"), {keys::fixedKey("Cmd+E")})
                                       : "Empty slot: click selects it, double-click makes a MIDI clip, right-click: paste or insert");
      }
      ImGui::PopID();
    }
    ImGui::PopClipRect();
    ImGui::PopID();
  }
  ImGui::PopClipRect();  // (the track columns' clip, C44-19)
  // Main column: scenes (launch every track's slot in the row, stop the others), Stop all.
  {
    const float x = mainX, w = pixelgrid::whole(kMainW - 8, dpp);
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, yT1), mainFill(), 0.0f);  // K-112c: square, as the track titles
    dl->AddText(a.fontRegular, fs, ImVec2(x + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), theme::contrastText(mainFill()), i18n::tr("Main"));
    {  // N49-09: a click (or right-click) on Main's title selects it: its chain opens
      ImGui::SetCursorScreenPos(ImVec2(x, y0));
      ImGui::SetNextItemAllowOverlap();  // GitHub #58: the hidden count sits on it
      ImGui::InvisibleButton("##maintitle", ImVec2(w, std::max(1.0f, titleH)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) selectMain(a);
      ui::Hint("Main: click to show its devices");
      if (!s.mixer.empty() && a.selMixer == s.mixer[0].id && a.showMaster) {
        const float in = pixelgrid::strokeIn(1.5f);
        dl->AddRect(ImVec2(x + in, y0 + in), ImVec2(x + w - in, yT1 - in), P.hot, 0.0f, 0, 1.5f);
      }
    }
    hiddenChip(a, dl, x, x + w, y0, titleH);  // GitHub #58: the mixer's hidden strips
    ImGui::PushClipRect(ImVec2(x, slotsY0), ImVec2(x + w, std::max(slotsY0, slotsY1)), true);  // N45-04
    for (int k = 0; k < rows; ++k) {
      const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
      if (!rowShown(rs)) continue;
      const float sy = rs.x0;
      const ImVec2 q0(x, sy), q1(x + w, rs.x1);
      gSceneColX = x + w * 0.5f;  // (the dev grid report's scene column)
      ImGui::PushID(1000 + k);
      ImGui::SetCursorScreenPos(q0);
      if (ImGui::InvisibleButton("##scene", ImVec2(w, q1.y - q0.y))) {
        // N59-07: the click selects the scene (the slot and clip selections go) as well as launching it.
        clearGridSel();
        a.playlist.selected.clear();
        a.playlist.trackSel.clear();
        gSceneSel = k;
        launchSceneRow(a, k, nScenes);
      }
      const bool hov = ImGui::IsItemHovered();
      if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##scenemenu");  // checkpoint 59 (N58-03)
      if (ImGui::BeginPopup("##scenemenu")) {
        sceneMenuItems(a, k, k < nScenes);
        ImGui::EndPopup();
      }
      // K-112c: square scene cells, the launch triangle at the left and the scene's number at the
      // right end, as every ref draws the scene column. K-112c (round 43): in every row, an empty
      // set's too (empty01, empty02: eight numbered scenes before any clip). A row past the live
      // area is a scene of empty slots: launching it stops every track, as an empty slot does. It
      // is drawn, not stored: the song gains nothing until a clip lands in the row.
      const SceneInfo* scene = trackmodes::sceneInfo(s, k);  // checkpoint 59 (N58-03): its own colour and name
      dl->AddRectFilled(q0, q1, hov ? P.surface700 : P.surface800, 0.0f);
      if (scene && scene->color != 0) dl->AddRectFilled(q0, q1, theme::withAlpha(theme::rgb(scene->color), hov ? 0.55f : 0.42f), 0.0f);
      const float cyy = (q0.y + q1.y) * 0.5f;
      ui::drawIconFit(dl, ui::Icon::Play, ImVec2(q0.x + 6, cyy - 4.5f), ImVec2(q0.x + 13, cyy + 4.5f), hov ? P.text : P.textDim);
      const std::string sn = std::to_string(k + 1);
      const float nw = numFont(a)->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, sn.c_str()).x;
      dl->AddText(numFont(a), theme::kFontCaption, ImVec2(std::round(q1.x - 7 - nw), theme::textTop(numFont(a), theme::kFontCaption, cyy, 0)), P.text, sn.c_str());
      float setW = 0;  // grid2-1118: the scene's own tempo and time signature, before its number
      if (scene) {
        const double sb = scene->tempo;
        const bool sgOn = scene->sigNum > 0 && scene->sigDen > 0;
        if (sb > 0 || sgOn) {
          char tx[40];
          if (sb > 0 && sgOn) {
            std::snprintf(tx, sizeof tx, "%.0f %d/%d", sb, scene->sigNum, scene->sigDen);
          } else if (sb > 0) {
            std::snprintf(tx, sizeof tx, "%.0f", sb);
          } else {
            std::snprintf(tx, sizeof tx, "%d/%d", scene->sigNum, scene->sigDen);
          }
          const float tw = numFont(a)->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, tx).x;
          if (q1.x - q0.x > tw + nw + 40) {
            setW = tw + 8;
            dl->AddText(numFont(a), theme::kFontCaption, ImVec2(std::round(q1.x - 7 - nw - 8 - tw), theme::textTop(numFont(a), theme::kFontCaption, cyy, 0)), P.textDim, tx);
          }
        }
      }
      if (scene && !scene->name.empty()) {  // the name between the launch triangle and the number, cut to fit
        const float nx0 = q0.x + 19, nx1 = std::round(q1.x - 7 - nw - 6 - setW);
        if (nx1 - nx0 > 12) {
          ImGui::PushClipRect(ImVec2(nx0, q0.y), ImVec2(nx1, q1.y), true);
          const std::string shown = fitText(a.fontRegular, theme::kFontCaption, scene->name, nx1 - nx0);
          dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(nx0, theme::textTop(a.fontRegular, theme::kFontCaption, cyy, 0)), P.text, shown.c_str());
          ImGui::PopClipRect();
        }
      }
      if (gSceneSel == k && sceneSelValid(a)) {  // N59-07: the selected scene
        const float in = pixelgrid::strokeIn(1.5f);
        dl->AddRect(ImVec2(q0.x + in, q0.y + in), ImVec2(q1.x - in, q1.y - in), P.hot, 0.0f, 0, 1.5f);
      }
      ui::Hint(keys::withKeys(i18n::N_("Scene: launches every track's clip in this row (tracks with an empty slot stop) and selects it (%s renames it); right-click to rename, colour, insert, duplicate or delete it"), {keys::cmdKey("edit.rename", "Cmd+R")}));
      ImGui::PopID();
    }
    ImGui::PopClipRect();
  }
  float gridBottom = stopSpan.x0;
  if (gGridAsked) {  // K-287 (dev): each slot's centre and its first clip (0: empty)
    gGridAsked = false;
    std::printf("livegrid cols %d scenes %d rows %d\n", nCols, nScenes, rows);
    for (int t = 0; t < nCols; ++t)  // K-112c (round 39): each column's title: its track's name and centre
      if (!hiddenCol(t)) std::printf("livegrid title %d \"%s\" %.0f %.0f\n", t, playlistTrackName(s, static_cast<size_t>(t)).c_str(),
                  static_cast<double>(column(t) + (colW - 4) * 0.5f), static_cast<double>((y0 + yT1) * 0.5f));
    for (int k = 0; k < rows; ++k)  // K-112c (round 43): every slot row's centre, and the free column's
      std::printf("livegrid row %d y %.1f\n", k, static_cast<double>((livegrid::row(o.y, k, dpp).x0 + livegrid::row(o.y, k, dpp).x1) * 0.5f));
    if (gSceneColX > 0) std::printf("livegrid scene x %.0f\n", static_cast<double>(gSceneColX));  // checkpoint 59
    if (freeX1 - freeX0 >= 40.0f) std::printf("livegrid free x %.1f w %.1f\n", static_cast<double>((freeX0 + freeX1) * 0.5f), static_cast<double>(freeX1 - freeX0));
    for (int t = 0; t < nCols; ++t)
      for (int k = 0; k < nScenes && !hiddenCol(t) && !groupCol(t); ++k) {
        const livegrid::Cell q = livegrid::slot(o.x, o.y, vc(t), k, dpp);
        const std::vector<int>& in = slot[static_cast<size_t>(t)][static_cast<size_t>(k)];
        std::printf("  slot %d %d x %.1f y %.1f clip %u\n", t, k, static_cast<double>((q.x0 + q.x1) * 0.5f), static_cast<double>((q.y0 + q.y1) * 0.5f),
                    in.empty() ? 0u : static_cast<unsigned>(s.playlist[static_cast<size_t>(in.front())].id));
      }
    std::fflush(stdout);
  }
  // The stop row under the slots: a stop button per track (lit while the track plays a grid clip),
  // and Stop all under the scenes. K-112c (round 42): always, as a new set's grid in the refs (no
  // live area yet is a grid of empty slots, not a message).
  {
    const float sy = stopSpan.x0, sy1 = stopSpan.x1;  // the rows' own gap above it (N45-04: pinned)
    ImGui::PushClipRect(ImVec2(visX0, -1e6f), ImVec2(std::max(visX0, gridClipX1), 1e6f), true);  // C44-19
    for (int t = 0; t < nCols; ++t) {
      if (hiddenCol(t)) continue;
      const pixelgrid::Span cs = livegrid::column(o.x, vc(t), dpp);
      const float x = cs.x0, w = cs.w();
      const ImVec2 q0(x, sy), q1(x + w, sy1);
      // Checkpoint 51: a group's stop button stops its tracks.
      const int u1 = groupCol(t) ? std::min(nCols - 1, t + s.tracks[static_cast<size_t>(t)].groupSize) : t;
      ImGui::PushID(3000 + t);
      ImGui::SetCursorScreenPos(q0);
      if (ImGui::InvisibleButton("##stoptrack", ImVec2(w, q1.y - q0.y)))
        for (int u = t; u <= u1; ++u) a.engine.stopLiveTrack(u);
      const bool hov = ImGui::IsItemHovered();
      ui::Hint(groupCol(t) ? "Stop: the group's tracks stop" : "Stop: this track's grid clip stops");
      ImGui::PopID();
      bool live = false, stopping = false;
      for (int u = t; u <= u1; ++u) {
        live = live || a.engine.liveActive(u) != kNoId;
        stopping = stopping || a.engine.liveStopping(u);  // grid-1118: a stop waiting for its grid line blinks
      }
      // K-112c: the stop row is a row of cells like the slots, its square at the left under the
      // slots' squares (the refs line the stop buttons up with the launch column).
      dl->AddRectFilled(q0, q1, hov ? P.surface700 : P.surface800, 0.0f);
      const float cyy = (q0.y + q1.y) * 0.5f;
      dl->AddRectFilled(ImVec2(q0.x + 6, cyy - 4), ImVec2(q0.x + 14, cyy + 4),
                        stopping ? theme::withAlpha(P.text, 0.3f + 0.7f * pulse) : live || hov ? P.text : P.textDim, 1.0f);
      // C44-18: the playing clip's progress, a pie filling round once per pass of the clip (counted
      // from the frame the track's clip started, on the transport's free-running position).
      static std::map<int, std::pair<ObjectId, double>> started;
      const ObjectId act = live ? a.engine.liveActive(t) : kNoId;
      auto it = started.find(t);
      if (act == kNoId) {
        if (it != started.end()) started.erase(it);
      } else {
        if (it == started.end() || it->second.first != act) it = started.insert_or_assign(t, std::make_pair(act, a.engine.position())).first;
        const Clip* pc = s.clip(act);
        const double len = pc ? static_cast<double>(std::max<Tick>(1, s.clipLength(*pc))) : 1.0;
        const double f = std::fmod(std::max(0.0, a.engine.position() - it->second.second), len) / len;
        const ImVec2 pcn(q0.x + 25.0f, cyy);
        const float r = 5.0f;
        dl->AddCircle(pcn, r, P.launch, 20, 1.2f);
        if (f > 0.001) {
          dl->PathLineTo(pcn);
          dl->PathArcTo(pcn, r, -IM_PI * 0.5f, -IM_PI * 0.5f + static_cast<float>(f) * 2.0f * IM_PI, 20);
          dl->PathFillConvex(P.launch);
        }
      }
    }
    ImGui::PopClipRect();
    const float x = mainX, w = pixelgrid::whole(kMainW - 8, dpp);
    const ImVec2 q0(x, sy), q1(x + w, sy1);
    ImGui::SetCursorScreenPos(q0);
    if (ImGui::InvisibleButton("##stopall", ImVec2(w, q1.y - q0.y))) a.engine.stopAllLive();
    const bool hov = ImGui::IsItemHovered();
    ui::Hint("Stop all: every track's grid clip stops");
    // K-112c: Stop all is a ringed square at the left of the scene column's stop cell (no word, as the refs).
    dl->AddRectFilled(q0, q1, hov ? P.surface700 : P.surface800, 0.0f);
    const float cyy = (q0.y + q1.y) * 0.5f;
    dl->AddRectFilled(ImVec2(q0.x + 5, cyy - 5), ImVec2(q0.x + 15, cyy + 5), hov ? P.text : P.textDim, 1.0f);
    dl->AddRect(ImVec2(q0.x + 4.5f, cyy - 5.5f), ImVec2(q0.x + 15.5f, cyy + 5.5f), IM_COL32(16, 18, 22, 200), 1.5f);
    gridBottom = livegrid::rowY(oyPin, rowsFit + 1, dpp);
  }
  // K-112c (round 43): the free column, and files or instruments dropped onto the grid. The free
  // column is a quiet panel with its hint, as the refs' (101, 069, 375, empty02); what drops there
  // makes new tracks after the shown ones. A slot (or a title, or the stop cell: the column's first
  // empty slot) puts the files in that track's slots, one per scene from there, through the same
  // drop as the arrangement's (one undo step, collaborators see it). Dropped audio opens nothing.
  {
    const float fy0 = y0, fy1 = stopSpan.x1;
    const bool freeCol = freeX1 - std::max(freeX0, visX0) >= 40.0f;
    if (freeCol) {
      dl->AddRectFilled(ImVec2(freeX0, fy0), ImVec2(freeX1, fy1), theme::withAlpha(P.surface800, 0.55f), 0.0f);
      const char* ht = i18n::tr("Drop files and instruments here");
      const float wrapW = freeX1 - freeX0 - 16.0f;  // a narrow column wraps it, a wide one keeps one line (101 / empty02)
      const ImVec2 ts = a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, wrapW, ht);
      const float midY = (livegrid::rowY(oyPin, 0, dpp) + livegrid::row(oyPin, std::min(rows, 8) - 1, dpp).x1) * 0.5f;
      // Each wrapped line centred on its own, as the refs set the hint.
      const char* line = ht;
      const char* htEnd = ht + std::strlen(ht);
      float ly = std::round(midY - ts.y * 0.5f);
      const float lh = a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, "Ag").y;
      // Round 45: a column narrower than the longest word shows no hint (no word is cut in two).
      float widest = 0.0f;
      for (const char* w = ht; w < htEnd;) {
        const char* we = w;
        while (we < htEnd && *we != ' ') ++we;
        widest = std::max(widest, a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, w, we).x);
        w = we < htEnd ? we + 1 : we;
      }
      if (widest > wrapW) line = htEnd;
      while (line < htEnd) {
        const char* end = a.fontRegular->CalcWordWrapPosition(theme::kFontCaption, line, htEnd, wrapW);
        if (end <= line) end = htEnd;
        const float lw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0.0f, line, end).x;
        dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(std::round((freeX0 + freeX1 - lw) * 0.5f), ly), P.textFaint, line, end);
        ly += lh + 2;
        line = end;
        while (*line == ' ') ++line;
      }
    }
    const float dx1 = std::min(trackClipX1, freeCol ? freeX1 : column(nCols) - kGap);
    if (nCols > 0 && dx1 > column(0) && ImGui::BeginDragDropTargetCustom(ImRect(column(0), fy0, dx1, fy1), ImGui::GetID("##griddrop"))) {
      int tt = -1;
      for (int t = 0; t < nCols && tt < 0; ++t) {
        if (hiddenCol(t)) continue;
        const pixelgrid::Span cs = livegrid::column(o.x, vc(t), dpp);
        if (m.x >= cs.x0 && m.x < cs.x1 + kGap && m.x < trackClipX1) tt = t;
      }
      const bool onGroup = tt >= 0 && groupCol(tt);  // S09: a group takes effects (its bus), never clips
      const bool inFree = tt < 0 && freeCol && m.x >= freeX0;
      int tk = -1;
      for (int k = 0; k < rows && tk < 0; ++k) {
        const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
        if (m.y >= rs.x0 - kGap && m.y < rs.x1 && m.y >= slotsY0 && m.y < slotsY1) tk = k;
      }
      if (tt >= 0 && tk < 0 && !onGroup) {  // the title or the stop cell: the column's first empty slot
        tk = 0;
        while (tk < nScenes && !slot[static_cast<size_t>(tt)][static_cast<size_t>(tk)].empty()) ++tk;
      }
      if (inFree) tt = nCols, tk = std::max(tk, 0);
      if (tt >= 0) {
        browserui::DropPlace place;
        place.track = tt;
        place.tick = static_cast<Tick>(tk) * sceneT;
        place.slotTicks = sceneT;
        place.newTracks = inFree;
        browserui::DropFeedback fb;
        fb.highlight = false;
        if (onGroup) {
          const ImGuiPayload* pl = ImGui::GetDragDropPayload();
          if (pl && (pl->IsDataType("GL_FILE") || pl->IsDataType("GL_ITEM") || pl->IsDataType("GL_FILES"))) {
            ImGui::SetTooltip("%s", i18n::tr("A group track holds no clips: put them on one of its tracks"));
            ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
          }
          place.tick = 0;
        } else if (browserui::acceptFileDrop(a, browse::DropTarget::Playlist, place, &fb) && !inFree) {
          // hunt-clips2-1119: the slot a file is dropped in is the selected one (the selection stayed on
          // the slot chosen before). After the drop's own work (queued before this).
          a.later([&a, tt, tk] {
            if (!slotClips(a.doc.song(), tt, tk).empty()) {
              selectSlot(a, tt, tk);
            }
          });
        }
        browserui::DropFeedback pfb;
        pfb.highlight = false;
        browserui::acceptPluginDrop(a, browse::DropTarget::Playlist, place, &pfb);
        // Where it lands: the slots the files go to (in the free column, the new tracks' first
        // columns), or the whole column an instrument goes to.
        const float ri = pixelgrid::strokeIn(2.0f);
        dl->PushClipRect(ImVec2(column(0), fy0), ImVec2(std::max(dx1, column(0) + 1), fy1), true);
        if (const browserui::HoverPlan* hp = fb.hovering ? browserui::hoverPlan(browse::DropTarget::Playlist) : nullptr) {
          dl->PushClipRect(ImVec2(column(0), slotsY0), ImVec2(std::max(dx1, column(0) + 1), std::max(slotsY0, slotsY1)), true);  // N45-04
          for (const xdrop::Step& sp : hp->plan.steps) {
            if (sp.action != browse::DropAction::AudioClip && sp.action != browse::DropAction::MidiPatternClip) continue;
            const int k = static_cast<int>(sp.tick / sceneT);
            const livegrid::Cell q = livegrid::slot(o.x, o.y, sp.track, k, dpp);
            const ImU32 col = theme::rgb(sp.action == browse::DropAction::AudioClip ? theme::role::kPaint : theme::role::kDraw);
            dl->AddRectFilled(ImVec2(q.x0, q.y0), ImVec2(q.x1, q.y1), theme::withAlpha(col, 0.28f), 0.0f);
            dl->AddRect(ImVec2(q.x0 + ri, q.y0 + ri), ImVec2(q.x1 - ri, q.y1 - ri), theme::withAlpha(col, 0.95f), 0.0f, 0, 2.0f);
            if (sp.item < hp->items.size())
              theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(q.x0 + 19, theme::textTop(a.fontRegular, theme::kFontCaption, (q.y0 + q.y1) * 0.5f, 0)),
                                  q.x1 - q.x0 - 24, P.text, xdrop::displayName(hp->items[sp.item]).c_str());
          }
          dl->PopClipRect();
        } else if (pfb.hovering) {
          const pixelgrid::Span cs = livegrid::column(o.x, vc(tt), dpp);
          const ImU32 hot = theme::rgb(theme::role::kDraw);
          dl->AddRectFilled(ImVec2(cs.x0, fy0), ImVec2(cs.x1, fy1), theme::withAlpha(hot, 0.16f));
          dl->AddRect(ImVec2(cs.x0 + ri, fy0 + ri), ImVec2(cs.x1 - ri, fy1 - ri), theme::withAlpha(hot, 0.95f), 0.0f, 0, 2.0f);
        }
        dl->PopClipRect();
      }
      ImGui::EndDragDropTarget();
    }
  }
  // K-287: a slot's clips dragged onto an empty slot of the live area move there (that track, that
  // scene, the same place inside the scene): one undo step, as a move in the arrangement.
  // grid2-1118: a rubber band from an empty slot selects the slots it touches.
  if (banding) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      banding = false;
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
      const float bx0 = std::min(bandFrom.x, m.x), bx1 = std::max(bandFrom.x, m.x), by0 = std::min(bandFrom.y, m.y), by1 = std::max(bandFrom.y, m.y);
      dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), theme::withAlpha(P.hot, 0.12f));
      dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), theme::withAlpha(P.hot, 0.8f), 0.0f, 0, 1.0f);
      std::set<std::pair<int, int>> band;
      for (int t = 0; t < nCols; ++t) {
        if (hiddenCol(t) || groupCol(t)) {
          continue;
        }
        const pixelgrid::Span cs = livegrid::column(o.x, vc(t), dpp);
        if (cs.x1 <= bx0 || cs.x0 >= bx1) {
          continue;
        }
        for (int k = 0; k < rows; ++k) {
          const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
          if (rs.x1 > by0 && rs.x0 < by1) {
            band.insert({t, k});
          }
        }
      }
      if (!band.empty() && band != gMulti) {
        gMulti = band;
        gSel = {band.begin()->first, band.begin()->second};
        syncMultiSel(a);
      }
    }
  }
  gSlotPressed = dragFrom.first >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (dragFrom.first >= 0) {
    const auto [ft, fk] = dragFrom;
    // grid2-1118: a slot of a multi-slot selection drags them all.
    const bool multi = !gMulti.empty() && slotSelected(ft, fk) && selectedSlots().size() > 1;
    int tt = -1, tk = -1;
    for (int t = 0; t < nCols && tt < 0; ++t) {
      if (hiddenCol(t) || groupCol(t)) continue;
      const pixelgrid::Span cs = livegrid::column(o.x, vc(t), dpp);
      if (m.x >= cs.x0 && m.x < cs.x1 && m.x < trackClipX1) tt = t;
    }
    // C44-05: any row is a target, the empty ones past the live area too (it grows to take the clip).
    for (int k = 0; k < rows && tk < 0; ++k) {
      const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
      if (m.y >= rs.x0 && m.y < rs.x1 && m.y >= slotsY0 && m.y < slotsY1) tk = k;
    }
    const bool srcOk = ft < nCols && fk < nScenes && !slot[static_cast<size_t>(ft)][static_cast<size_t>(fk)].empty();
    // grid-1118: a full slot takes the dragged clip too (its clip is replaced), as the reference's; Alt copies.
    const bool copyDrag = ImGui::GetIO().KeyAlt;
    bool ok = srcOk && tt >= 0 && tk >= 0 && (tt != ft || tk != fk) && !s.tracks[static_cast<size_t>(tt)].locked &&
              (copyDrag || !s.tracks[static_cast<size_t>(ft)].locked);
    if (ok)
      for (int ci : slot[static_cast<size_t>(ft)][static_cast<size_t>(fk)]) ok = ok && trackmodes::clipAllowed(s, s.playlist[static_cast<size_t>(ci)].kind, tt) && clipFitsTrack(s, s.playlist[static_cast<size_t>(ci)].kind, tt);  // N50-02
    const bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f);
    if (multi) {
      ok = srcOk && tt >= 0 && tk >= 0 && (tt != ft || tk != fk);
    }
    // grid2-1118: up out of the grid (over the titles' top) or Tab: the clips go on to the Arrangement.
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragging && srcOk && (m.y < y0 - 2.0f || ImGui::IsKeyPressed(ImGuiKey_Tab, false))) {
      if (!multi) {
        gMulti.clear();
        gSel = {ft, fk};
      }
      beginGridDrag(a);
      dragFrom = {-1, -1};
      keepMulti = false;
    } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (dragging && ok && multi) {  // the slots they would land in
        const float ri = pixelgrid::strokeIn(2.0f);
        for (const auto& [st0, sk0] : selectedSlots()) {
          const int nt = st0 + (tt - ft), nk = sk0 + (tk - fk);
          if (nt < 0 || nt >= nCols || nk < 0 || nk >= rows || hiddenCol(nt)) {
            continue;
          }
          const livegrid::Cell q = livegrid::slot(o.x, o.y, vc(nt), nk, dpp);
          dl->AddRect(ImVec2(q.x0 + ri, q.y0 + ri), ImVec2(q.x1 - ri, q.y1 - ri), P.hot, 0.0f, 0, 2.0f);
        }
        ImGui::SetMouseCursor(copyDrag ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);
      } else if (dragging && ok) {  // the slot it would land in
        const livegrid::Cell q = livegrid::slot(o.x, o.y, tt, tk, dpp);
        const float ri = pixelgrid::strokeIn(2.0f);
        dl->AddRect(ImVec2(q.x0 + ri, q.y0 + ri), ImVec2(q.x1 - ri, q.y1 - ri), P.hot, 0.0f, 0, 2.0f);
        ImGui::SetMouseCursor(copyDrag ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);
        if (copyDrag) {  // a plus at the target's launch cell: a copy lands there
          const float cx = q.x0 + 9.0f, cy = std::round((q.y0 + q.y1) * 0.5f);
          dl->AddRectFilled(ImVec2(cx - 4, cy - 0.75f), ImVec2(cx + 4, cy + 0.75f), P.hot);
          dl->AddRectFilled(ImVec2(cx - 0.75f, cy - 4), ImVec2(cx + 0.75f, cy + 4), P.hot);
        }
      }
    } else {
      const bool moved = ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= 16.0f;
      if (keepMulti && !moved && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        selectSlot(a, ft, fk);  // a click alone on a slot of the selection selects that one
      }
      keepMulti = false;
      if (multi && ok && moved && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const int mt = tt - ft, mk = tk - fk;
        a.later([&a, mt, mk, copyDrag] {
          if (!moveSlots(a, mt, mk, copyDrag)) {
            a.notify(i18n::tr("The clips do not fit there"), true);
          }
        });
      } else if (ok && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && moved) {
        if (copyDrag) {  // grid-1118: Alt+drag copies the slot (one undo step)
          std::vector<Clip> keep = gClipboard;
          copySlot(a, ft, fk);
          const std::vector<Clip> src = gClipboard;
          gClipboard = std::move(keep);
          placeInSlot(a, src, tt, tk, "copy clips");
        } else {
          std::set<ObjectId> ids, gone;
          for (int ci : slot[static_cast<size_t>(ft)][static_cast<size_t>(fk)]) ids.insert(s.playlist[static_cast<size_t>(ci)].id);
          if (tk < nScenes)
            for (int ci : slot[static_cast<size_t>(tt)][static_cast<size_t>(tk)]) gone.insert(s.playlist[static_cast<size_t>(ci)].id);
          const Tick dt = static_cast<Tick>(tk - fk) * sceneT;
          a.doc.beginGroup("move clips");
          ensureScene(a, tk);
          for (ObjectId id : gone) a.doc.removeClip(id);  // replaced
          a.doc.editClips("move clips", [&](std::vector<Clip>& cs) {
            for (Clip& c : cs)
              if (ids.count(c.id)) c.start += dt, c.track = tt;
          });
          a.doc.endGroup();
          selectSlot(a, tt, tk);
        }
      }
      dragFrom = {-1, -1};
    }
  }
  // K-112c (round 39): a title dragged along the row moves its track, as dragging an arrangement
  // header does (the same document edit: the arrangement's rows follow, and collaborators get it).
  if (titleDrag >= nCols) titleDrag = -1;
  if (titleDrag >= 0) {
    int before = nCols;  // checkpoint 51: the first shown column whose middle is right of the pointer
    for (int t = 0; t < nCols; ++t)
      if (!hiddenCol(t) && m.x < column(t) + colW * 0.5f) {
        before = t;
        break;
      }
    const float lx = before < nCols ? column(before) - 3 : column(nCols - 1) + colW - 3;
    const pixelgrid::Span ls = pixelgrid::line(lx, dpp);
    dl->AddRectFilled(ImVec2(ls.x0 - 1, y0), ImVec2(ls.x1 + 1, yT1), P.hot);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (before != titleDrag && before != titleDrag + 1) {
        std::vector<int> now;
        std::vector<int> moved{titleDrag};
        if (groupCol(titleDrag))
          for (int u = titleDrag + 1; u <= std::min(nCols - 1, titleDrag + s.tracks[static_cast<size_t>(titleDrag)].groupSize); ++u) moved.push_back(u);
        a.doc.beginGroup("move tracks");
        moved = trackmodes::withLanes(s, moved);  // N53-03: its lanes go with it (nothing lands between them)
        if (trackorder::movePlaylistTracks(a.doc, moved, trackmodes::pastLanes(s, before), &now) && !now.empty()) {
          a.playlist.trackSel = {now.front()};
          trackmodes::syncGroupRouting(a.doc);  // N51-06: in or out of a group, the routing follows (the same undo step)
        }
        a.doc.endGroup();
      }
      titleDrag = -1;
    }
  }
  if (held != kNoId && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    a.engine.releaseClip(held);
    held = kNoId;
  }
  // ---- mixer controls under the columns (the linked mixer track of each column) ----
  const float my0 = std::max(gridBottom + 8, oyPin + av.y - kPad - mixH);
  if (footMixer) dl->AddLine(ImVec2(visX0 + kPad, my0 - 4), ImVec2(mainX + kMainW - 8, my0 - 4), P.line);
  const float mh = footMixer ? std::max(100.0f, oyPin + av.y - kPad - my0) : 0.0f;
  const StripRow srow{my0, mh, anySends(s) && mh >= 131.0f + sendsHeight(s)};  // room for the sends row, the fields and the pan above the buttons  // room for the row, the fields and the pan above the buttons
  auto strip = [&](int t, const MixerTrack* mx, float x, float w, bool master, char letter = 0) {
    if (footMixer) sessionStrip(a, dl, srow, t, mx, x, w, master, letter);
  };
  ImGui::PushClipRect(ImVec2(visX0, -1e6f), ImVec2(std::max(visX0, gridClipX1), 1e6f), true);  // C44-19
  for (int t = 0; t < nCols; ++t)
    if (!hiddenCol(t)) strip(t, trackmodes::playedMixer(s, t), column(t), colW - 4, false);
  ImGui::PopClipRect();
  // K-112c (round 19): the return tracks as strips between the tracks and Main, each titled in the
  // grid's title row with its letter and name and no slots, as every ref's mixer (143, 141, 280, 093).
  // (Where they stand: retShift / nFit above.)
  int nRet = 0;
  {
    const std::vector<size_t> rets = shownReturns(s);
    for (size_t i = 0; i < rets.size() && i < 26; ++i) {
      const pixelgrid::Span cs = retColumn(static_cast<int>(i));  // K-276: as a track's title
      const float x = cs.x0;
      if (static_cast<int>(i) >= nFit) break;  // only the returns that fit before Main
      const MixerTrack& rt = s.mixer[rets[i]];
      const char lt = returnLetter(s, static_cast<int>(rets[i]));
      dl->AddRectFilled(ImVec2(x, y0), ImVec2(cs.x1, yT1), returnFill(), 0.0f);
      const std::string nm = std::string(1, lt) + " " + rt.name;
      theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), cs.w() - 10, theme::contrastText(returnFill()), nm.c_str());
      ImGui::SetCursorScreenPos(ImVec2(x, y0));
      ImGui::PushID(static_cast<int>(8100 + i));
      if (ImGui::InvisibleButton("##rtitle", ImVec2(cs.w(), titleH))) {
        selectReturn(a, rt.id);
      }
      ImGui::PopID();
      ui::Hint("Return track: other tracks send to it (click to select it in the mixer)");
      strip(-1, &rt, x, colW - 4, false, lt);
      ++nRet;
    }
  }
  // K-112c: a 1 px divider between strips (and before Main), as the refs' mixers draw them.
  for (int t = 1; footMixer && t <= nCols + nRet; ++t) {
    if (hiddenCol(t)) continue;
    // A track's left edge, a return's (shifted) left edge, or Main's; plus the last track's right edge
    // when the returns stand apart from it.
    const float ex = t == nCols + nRet ? mainX : t >= nCols ? retColumn(t - nCols).x0 : column(t);
    const pixelgrid::Span lx = pixelgrid::line(ex - 3, dpp);  // one crisp device column
    if (t >= nCols || (ex - 3 > visX0 && ex - 3 < trackClipX1)) dl->AddRectFilled(ImVec2(lx.x0, my0 + 2), ImVec2(lx.x1, my0 + mh - 2), P.line);
    if (t == nCols && nRet > 0 && column(nCols) - 3 < trackClipX1) {
      const pixelgrid::Span lt = pixelgrid::line(column(nCols) - 3, dpp);
      dl->AddRectFilled(ImVec2(lt.x0, my0 + 2), ImVec2(lt.x1, my0 + mh - 2), P.line);
    }
  }
  if (!s.mixer.empty()) strip(-1, &s.mixer[0], mainX, kMainW - 8, true);
  // Checkpoint 58 (N57-03): what the strips leave out for lack of height says so, in the free column.
  if (footMixer) {
    const bool sendsHidden = anySends(s) && !srow.sendsRow, ioHidden = liveio::shown(a) && mh < kStripIoMinH;
    const float nx0 = std::max(freeX0, visX0) + 8, nx1 = std::min(freeX1, trackClipX1) - 8;
    if ((sendsHidden || ioHidden) && nx1 - nx0 >= 60.0f) {
      const char* note = sendsHidden && ioHidden ? i18n::tr("Sends and In/Out hidden: the window is too short")
                         : sendsHidden           ? i18n::tr("Sends hidden: the window is too short")
                                                 : i18n::tr("In/Out hidden: the window is too short");
      const float ww = nx1 - nx0;
      const ImVec2 sz = a.fontRegular->CalcTextSizeA(theme::kFontLabel, 1e9f, ww, note);
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(nx0 + std::max(0.0f, (ww - sz.x) * 0.5f)), std::round(my0 + 8)), P.textFaint, note,
                  nullptr, ww);
    }
  }
  // C44-12: the grid's keys, on the selected slot while the grid has the keyboard: the arrows move
  // the selection, Enter launches its clip (an empty slot: stops the track), Delete, copy, cut, paste
  // and duplicate edit it, the activator key mutes its clip and Insert scene adds a row under it.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() && nCols > 0) {
    if ((gSel.t < 0 || gSel.k < 0) && !a.playlist.trackSel.empty() && (keys::pressed("playlist.delete") || keys::pressed("playlist.duplicate"))) {
      // Checkpoint 52 (N51-07): Delete and Cmd+D on the selected track titles, as the reference's.
      if (keys::pressed("playlist.delete")) deleteTracks(a, -1);
      else duplicateTracks(a, -1);
    } else if ((gSel.t < 0 || gSel.k < 0) && sceneSelValid(a)) {
      // grid-1118: a selected scene, as the reference's scene column: Up / Down select the scene above or
      // below, Enter launches it and selects the next one (scenes fire top to bottom), Left goes to the
      // last track's slot of the row.
      const ImGuiIO& io = ImGui::GetIO();
      const bool plain = !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift;
      int k = gSceneSel;
      if (plain && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) k = std::max(0, k - 1);
      if (plain && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) k = std::min(rows - 1, k + 1);
      if (k != gSceneSel) {
        gSceneSel = k;
        const pixelgrid::Span rs = livegrid::row(o.y, k, dpp);
        if (rs.x0 < slotsY0) ImGui::SetScrollY(std::max(0.0f, scrollY - (slotsY0 - rs.x0)));
        if (rs.x1 > slotsY1) ImGui::SetScrollY(scrollY + (rs.x1 - slotsY1));
      }
      if (plain && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        int t = nCols - 1;
        while (t > 0 && hiddenCol(t)) --t;
        selectSlot(a, t, gSceneSel);
      } else if (plain && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        const int kk = gSceneSel;
        launchSceneRow(a, kk, nScenes);
        gSceneSel = std::min(rows - 1, kk + 1);  // select next scene on launch
      }
    } else if (gSel.t < 0 || gSel.k < 0) {
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
          ImGui::IsKeyPressed(ImGuiKey_RightArrow))
        selectSlot(a, 0, 0);
    } else {
      const ImGuiIO& io = ImGui::GetIO();
      const bool plain = !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift;
      int nt = gSel.t, nk = gSel.k;
      if (plain && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nk = std::max(0, nk - 1);
      if (plain && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nk = std::min(rows - 1, nk + 1);
      if (plain && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nt = std::max(0, nt - 1);
      bool toScene = false;  // grid-1118: Right from the last track goes on to the row's scene
      if (plain && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        int r = nt + 1;
        while (r < nCols && hiddenCol(r)) ++r;
        if (r >= nCols) toScene = true;
        nt = std::min(nCols - 1, nt + 1);
      }
      nt = std::min(nt, nCols - 1);
      {  // checkpoint 51: past folded tracks (S11: a group's slots are reached too: Enter launches them)
        const int dir = nt < gSel.t ? -1 : 1;
        while (nt >= 0 && nt < nCols && hiddenCol(nt)) nt += dir;
        if (nt < 0 || nt >= nCols) nt = gSel.t;
      }
      if (toScene) {
        const int sk = gSel.k;
        clearGridSel();
        a.playlist.selected.clear();
        a.playlist.trackSel.clear();
        gSceneSel = sk;
      } else if (nt != gSel.t || nk != gSel.k) {
        selectSlot(a, nt, nk);
        const pixelgrid::Span rs = livegrid::row(o.y, nk, dpp);  // N45-04: the selected row scrolls into view
        if (rs.x0 < slotsY0) ImGui::SetScrollY(std::max(0.0f, scrollY - (slotsY0 - rs.x0)));
        if (rs.x1 > slotsY1) ImGui::SetScrollY(scrollY + (rs.x1 - slotsY1));
        // N46-05: and its column clear of the pinned returns and Main.
        const pixelgrid::Span cs = livegrid::column(o.x, vc(nt), dpp);
        if (cs.x1 > trackClipX1) ImGui::SetScrollX(scrollX + (cs.x1 - trackClipX1));
        if (cs.x0 < visX0 + kPad) ImGui::SetScrollX(std::max(0.0f, scrollX - (visX0 + kPad - cs.x0)));
      }
      const int t = std::max(0, gSel.t), k = std::max(0, gSel.k);
      const std::vector<int> here = toScene ? std::vector<int>{} : slotClips(s, t, k);
      static ObjectId keyHeld = kNoId;  // grid-1118: Enter held on a Gate or Repeat clip: its release lets go
      if (keyHeld != kNoId && !ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter)) {
        a.engine.releaseClip(keyHeld);
        keyHeld = kNoId;
      }
      const bool cmdEnter = (io.KeyCtrl || io.KeySuper) && !io.KeyShift && !io.KeyAlt &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
      if (toScene) {
        // (the scene has the keys from the next frame)
      } else if (cmdEnter) {  // grid-1118: Cmd+Enter stops the selected slot's track, as the reference's
        if (groupCol(t)) {
          for (int u = t; u <= std::min(nCols - 1, t + s.tracks[static_cast<size_t>(t)].groupSize); ++u) a.engine.stopLiveTrack(u);
        } else {
          a.engine.stopLiveTrack(t);
        }
      } else if (plain && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && groupCol(t)) {
        for (int u = t + 1; u <= std::min(nCols - 1, t + s.tracks[static_cast<size_t>(t)].groupSize); ++u) {
          const std::vector<int> in = slotClips(s, u, k);
          if (!in.empty()) launch(a, s.playlist[static_cast<size_t>(in.front())].id);
          else a.engine.stopLiveTrack(u);
        }
      } else if (plain && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        if (!here.empty()) {
          keyHeld = s.playlist[static_cast<size_t>(here.front())].id;
          launch(a, keyHeld);
        } else {
          a.engine.stopLiveTrack(t);
        }
      }
      const bool cmd = io.KeyCtrl || io.KeySuper;
      if (!toScene) {  // (no slot edits once the selection moved to the scene)
        // grid2-1118: the edits work on every selected slot (a paste lands the copied block at this one).
        const bool many = selectedSlots().size() > 1;
        if (keys::pressed("playlist.delete")) {
          a.later([&a] { deleteSlots(a, "delete clips"); });
        }
        if (keys::pressed("playlist.copy")) {
          copySlots(a);
        }
        if (cmd && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_X, false) && (!here.empty() || many)) {
          a.later([&a] { copySlots(a), deleteSlots(a, "cut clips"); });
        }
        if (keys::pressed("playlist.paste") && (!gClipboard.empty() || !gBoard.empty()) && !groupCol(t)) {
          a.later([&a, t, k] { pasteSlots(a, t, k); });
        }
        if (keys::pressed("playlist.duplicate") && !groupCol(t)) {
          a.later([&a] { duplicateSlots(a); });
        }
        // grid2-1118: Cmd+E adds or removes the selected slots' stop buttons; Cmd+A selects every clip.
        if (cmd && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_E, false)) {
          a.later([&a] { toggleStopButtons(a); });
        }
        if (cmd && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
          selectAllSlots(a);
        }
        if (keys::pressed("playlist.insertSpace")) a.later([&a, k] { insertScene(a, k); });
        // Checkpoint 56 (N55-05): Cmd+Shift+M is the global Insert MIDI clip command (clips.insertMidiClip), which
        // takes this slot; a second handler here made the clip first and the command then said it could not.
        if (keys::pressed("playlist.mute") && !here.empty()) {
          const ObjectId id = s.playlist[static_cast<size_t>(here.front())].id;
          const bool to = !s.playlist[static_cast<size_t>(here.front())].muted;
          a.later([&a, id, to] { a.doc.editClip(id, to ? "deactivate clip" : "activate clip", [to](Clip& c) { c.muted = to; }); });
        }
      }
    }
  }
  ImGui::SetCursorScreenPos(o);
  // C44-19: as wide as the tracks, a free column and the pinned returns and Main.
  // N45-04: as tall as the window plus the scene rows that do not fit (they scroll under the titles).
  ImGui::Dummy(ImVec2(column(nCols) + colW - o.x + (mainX + kMainW + kPad - 4 - pinX0), av.y - 2 + static_cast<float>(std::max(0, rows - rowsFit)) * kSlotH));
  ImGui::EndChild();
}

void drawMixerBand(App& a) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  // C44-28: under the clip grid the band is the foot of the grid's own columns: no title row of its
  // own (the grid's titles name them) and no scroller of its own (it follows the grid's).
  const bool underGrid = gridView(a);
  if (underGrid) ImGui::SetNextWindowScroll(ImVec2(gGridScrollX, -1.0f));
  ImGui::BeginChild("##clipsmixer", ImVec2(0, 0), ImGuiChildFlags_None,
                    underGrid ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float dpp = pixelgrid::frameScale();
  const ImVec2 o(livegrid::snap(ImGui::GetCursorScreenPos().x, dpp), livegrid::snap(ImGui::GetCursorScreenPos().y, dpp));
  const ImVec2 av = ImGui::GetContentRegionAvail();
  const int nCols = gridColumns(s);
  const ColMap cm = colMap(s, nCols);  // checkpoint 51: as the grid's columns
  auto vc = [&](int t) { return t < nCols ? cm.col[static_cast<size_t>(t)] : cm.n + (t - nCols); };
  using livegrid::kPad, livegrid::kGap;
  constexpr float kMainW = 140, colW = livegrid::kColW;
  const float fs = theme::kFontCaption + 1;
  // The titles, then the strips: the clip grid's columns, returns beside Main at the right (357).
  const float y0 = livegrid::titleY(o.y, dpp), yT1 = livegrid::titleBottom(o.y, dpp), titleH = yT1 - y0;
  auto column = [&](int t) { return livegrid::column(o.x, vc(t), dpp); };
  // C44-19: Main and the returns pinned at the right edge, the tracks scrolling under them (as the grid).
  const float visX0 = o.x + ImGui::GetScrollX();
  const float mainX = livegrid::snap(visX0 + av.x - kPad - kMainW + 4, dpp);
  const float my0 = underGrid ? livegrid::snap(o.y + kPad, dpp) : livegrid::snap(yT1 + kGap + 2, dpp);
  const float mh = std::max(100.0f, o.y + av.y - kPad - my0);
  const StripRow srow{my0, mh, anySends(s) && mh >= 131.0f + sendsHeight(s)};
  // A title: the fill and name of a column (a click selects its mixer track; a selected one is ringed).
  auto title = [&](float x0, float x1, ImU32 fill, const std::string& name, const MixerTrack* mx, int id) {
    if (underGrid) return;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, yT1), fill, 0.0f);
    if (mx && a.selMixer == mx->id && (mx->id != s.masterId() || a.showMaster)) {  // (hunt-clips2-1119: as Main's title)
      const float in = pixelgrid::strokeIn(1.5f);
      dl->AddRect(ImVec2(x0 + in, y0 + in), ImVec2(x1 - in, yT1 - in), P.hot, 0.0f, 0, 1.5f);
    }
    theme::textEllipsis(dl, a.fontRegular, fs, ImVec2(x0 + 6, theme::textTop(a.fontRegular, fs, y0, titleH)), x1 - x0 - 10,
                        theme::contrastText(fill), name.c_str());
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::PushID(id);
    ImGui::SetNextItemAllowOverlap();  // GitHub #58: the hidden count sits on Main's
    ImGui::InvisibleButton("##mtitle", ImVec2(x1 - x0, titleH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if ((ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) && mx) {
      if (!s.mixer.empty() && mx->id == s.mixer[0].id) {
        selectMain(a);  // N49-09
      } else {
        selectReturn(a, mx->id);
      }
    }
    ImGui::PopID();
    if (mx) ui::Hint("Click to select this mixer track");
  };
  // The returns, right-aligned against Main (as the clip grid, round 36).
  const std::vector<size_t> rets = shownReturns(s);
  const int nFit = returnsShown(a) ? std::min<int>(static_cast<int>(rets.size()), 26) : 0;
  const float retShift = nFit > 0 ? livegrid::snap(mainX - column(nCols + nFit).x0, dpp) : 0.0f;
  const float pinX0 = nFit > 0 ? livegrid::column(o.x + retShift, cm.n, dpp).x0 : mainX;
  const float trackClipX1 = pinX0 - kGap;
  float bandClipX1 = trackClipX1;  // checkpoint 52: N47-18's rule here too (a strip cut to a sliver is not shown)
  for (int t = 0; t < nCols; ++t) {
    if (cm.hidden[static_cast<size_t>(t)]) continue;
    const pixelgrid::Span cs = column(t);
    if (cs.x0 < trackClipX1 && cs.x1 > trackClipX1 && trackClipX1 - cs.x0 < cs.w() * 0.5f) bandClipX1 = cs.x0;
  }
  ImGui::PushClipRect(ImVec2(visX0, -1e6f), ImVec2(std::max(visX0, bandClipX1), 1e6f), true);
  for (int t = 0; t < nCols; ++t) {
    if (cm.hidden[static_cast<size_t>(t)]) continue;
    const pixelgrid::Span cs = column(t);
    const MixerTrack* mx = trackmodes::playedMixer(s, t);
    title(cs.x0, cs.x1, theme::content(s.tracks[static_cast<size_t>(t)].color), playlistTrackName(s, static_cast<size_t>(t)), mx, 3000 + t);
    sessionStrip(a, dl, srow, t, mx, cs.x0, colW - 4, false);
  }
  for (int t = 1; t <= nCols; ++t) {  // the dividers between the tracks' strips
    if (t < nCols && cm.hidden[static_cast<size_t>(t)]) continue;
    const pixelgrid::Span lx = pixelgrid::line(column(t).x0 - 3, dpp);
    dl->AddRectFilled(ImVec2(lx.x0, my0 + 2), ImVec2(lx.x1, my0 + mh - 2), P.line);
  }
  ImGui::PopClipRect();
  for (int i = 0; i < nFit; ++i) {
    const pixelgrid::Span cs = livegrid::column(o.x + retShift, cm.n + i, dpp);
    const MixerTrack& rt = s.mixer[rets[static_cast<size_t>(i)]];
    const char lt = returnLetter(s, static_cast<int>(rets[i]));
    title(cs.x0, cs.x1, returnFill(), std::string(1, lt) + " " + rt.name, &rt, 3500 + i);
    sessionStrip(a, dl, srow, -1, &rt, cs.x0, colW - 4, false, lt);
  }
  // 1 px dividers between the strips, as the clip grid's foot.
  auto divider = [&](float ex) {
    const pixelgrid::Span lx = pixelgrid::line(ex - 3, dpp);
    dl->AddRectFilled(ImVec2(lx.x0, my0 + 2), ImVec2(lx.x1, my0 + mh - 2), P.line);
  };
  for (int i = 1; i < nFit; ++i) divider(livegrid::column(o.x + retShift, cm.n + i, dpp).x0);
  if (nFit > 0) divider(livegrid::column(o.x + retShift, cm.n, dpp).x0);
  divider(mainX);
  if (!s.mixer.empty()) {
    const pixelgrid::Span ms{mainX, livegrid::snap(mainX + kMainW - 8, dpp)};
    title(ms.x0, ms.x1, mainFill(), i18n::tr("Main"), &s.mixer[0], 3999);
    if (!underGrid) hiddenChip(a, dl, ms.x0, ms.x1, y0, titleH);

    sessionStrip(a, dl, srow, -1, &s.mixer[0], mainX, kMainW - 8, true);
  }
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(column(nCols).x0 + kGap - o.x + (mainX + kMainW + kPad - 4 - pinX0), std::max(av.y - 2, my0 + mh - o.y)));
  ImGui::EndChild();
}

namespace {
bool gCentreAsked = false;
}

void devCentreAsk() { gCentreAsked = true; }

void devCentreFrame(App& a) {
  if (!gCentreAsked) return;
  gCentreAsked = false;
  const struct { const char* name; ImFont* f; } fonts[] = {{"regular", a.fontRegular}, {"bold", a.fontBold}, {"mono", a.fontMono}};
  std::printf("CENTRE BEGIN\n");
  for (const auto& fo : fonts)
    for (float sz : {theme::kFontLabel, theme::kFontCaption, theme::kFontBody}) {
      ImFontBaked* b = fo.f ? fo.f->GetFontBaked(sz) : nullptr;
      const ImFontGlyph* cap = b ? b->FindGlyph('H') : nullptr;
      if (!cap) continue;
      for (float h : {15.0f, 16.0f, 18.0f, 20.0f, 22.0f, 24.0f}) {
        const float older = std::round(0.0f + (h - sz) * 0.5f) - 1, plain = std::round((h - sz) * 0.5f), now = theme::textTop(fo.f, sz, 0.0f, h);
        const float mid = (cap->Y0 + cap->Y1) * 0.5f;  // the cap band's middle from the pen
        std::printf("CENTRE %s %.0f box %.0f cap %.1f..%.1f older %+.1f plain %+.1f now %+.1f\n", fo.name, sz, h, cap->Y0, cap->Y1,
                    older + mid - h * 0.5f, plain + mid - h * 0.5f, now + mid - h * 0.5f);
      }
    }
  std::printf("CENTRE END\n");
  std::fflush(stdout);
}

namespace surface {
std::vector<int> columns(const App& a) {
  const Song& s = a.doc.song();
  const int n = gridColumns(s);
  const ColMap m = colMap(s, n);
  std::vector<int> out;
  for (int t = 0; t < n; ++t)
    if (!m.hidden[static_cast<size_t>(t)]) out.push_back(t);
  return out;
}
int scenes(const App& a) { return trackmodes::sceneCount(a.doc.song()); }
ObjectId slotClip(const App& a, int t, int k) {
  const Song& s = a.doc.song();
  const std::vector<int> in = slotClips(s, t, k);
  return in.empty() ? kNoId : s.playlist[static_cast<size_t>(in.front())].id;
}
void launchClip(App& a, ObjectId clip) { launch(a, clip); }
void launchScene(App& a, int k) { launchSceneRow(a, k, scenes(a)); }
void selectSlot(App& a, int t, int k) { livelook::selectSlot(a, t, k); }
bool deleteSlot(App& a, int t, int k) { return livelook::deleteSlot(a, t, k); }
void duplicateSlot(App& a, int t, int k) { livelook::duplicateSlot(a, t, k); }
}  // namespace surface

}  // namespace gl::livelook
