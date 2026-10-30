// Gridline — "platter" pack tests (J-159, J-163): a Platter pass recorded as automation.
// A scripted live performance on both decks and the mixer (mouse scratch, jog scratch,
// a throw, hot cues, cue, loops, beat jumps, a seek, rim nudges, crossfader cuts, EQ
// kills, pitch moves) is played through the audio engine while Gridline records.
// J-163: the pass becomes automation, one lane per control (the decks' state: position,
// play, wheel touch, mouse hand, loop, ...; the hands' travel; the controls that moved):
// song mode (J-179): a new pattern holding the clips, placed once where the pass was played
// (or, as the setup chooses, grouped playlist clips); pattern mode: the pass pattern's
// clips; no notes. A project with both forms plays each where it is. Played back
// unedited it must give the same audio as the live pass, bit for bit, at 64, 256 and 1024
// sample buffers and across buffer sizes; played from the middle (playback started in it,
// a trimmed clip) the decks pick it up on the pass's own step grid and match the full
// replay after a short settle; a clip cut in two or moved later plays the same. Edits
// change the sound from where they are. Also: each control's value stream step by step,
// a second pass (the first untouched, one undo step), loop recording and punch, save /
// load of the exact positions, and J-159 data (start note, command keys) still replaying.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../Check.h"
#include "core/AudioBuffer.h"
#include "core/Document.h"
#include "core/PatternAutomation.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "core/AutomationEdit.h"
#include "core/MenuOps.h"
#include "core/NoteTools.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Sample.h"
#include "packs/platter/platter.h"
#include "packs/seltmann/seltmann.h"

using namespace gl;
namespace pl = gl::packs::platter;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100;
constexpr double kPi = 3.14159265358979323846;

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_platter_perf_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

std::string writeTrack(const std::string& name, double seconds, double bpm) {
  const auto n = static_cast<size_t>(seconds * kSr);
  AudioBuffer b(2, n);
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < 2; ++c) {
      const double t = static_cast<double>(i) / kSr;
      const double beat = 60.0 / bpm;
      const double ph = std::fmod(t, beat);
      double x = 0.25 * std::sin(2 * kPi * 220 * t + c) + 0.12 * std::sin(2 * kPi * 331 * t) + 0.08 * std::sin(2 * kPi * 1250 * t + 0.5 * c);
      x += 0.4 * std::exp(-ph * 60.0) * std::sin(2 * kPi * 90 * ph);
      b.data[static_cast<size_t>(c) * n + i] = static_cast<float>(x);
    }
  const std::string path = (tmpDir() / name).string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}

pl::DeckModel flatDeck(const std::string& path, double bpm) {
  SamplePtr src = sampleCache().get(path);
  pl::DeckModel dm = pl::deckModel(*src, gl::packs::seltmann::flatGrid(*src, bpm), true);
  dm.cue = 0;
  return dm;
}

// ------------------------------------------------------------------ the rig
// A project with one Platter setup (deck A 120 BPM, deck B 100 BPM, DDJ-200 as a whole),
// armed, recording as automation; an 8 bar pattern (16 s at 120 BPM).
struct Rig {
  Document doc{makeEmptySong()};
  ObjectId ch = kNoId, pat = kNoId;
  ProgramBuilder builder;
  std::unique_ptr<AudioEngine> eng;
  int64_t pos = 0;  // samples since play

  Rig(const std::string& a, const std::string& b, bool keyLock) {
    doc.setParam(songParam(SongParam::Tempo), 120.0, false);
    ch = doc.addChannel("Decks", pl::kType);
    pat = doc.song().patterns[0].id;
    doc.editPattern(pat, "", [](Pattern& p) { p.bars = 8; });
    pl::Model m;
    pl::loadTrack(m, 0, a, flatDeck(a, 120));
    pl::loadTrack(m, 1, b, flatDeck(b, 100));
    m.map = pl::profileMapping("ddj200", pl::kSideBoth);
    m.asked = true;
    m.armed = true;
    m.recordAs = pl::RecAutomation;
    doc.editChannel(ch, "", [&](Channel& c) {
      c.plugin.data = pl::formatData(a, m);
      for (int k = 0; k < pl::kDecks; ++k) c.plugin.params[static_cast<size_t>(pl::deckParam(k, pl::DKeyLock))] = keyLock ? 1.f : 0.f;
    });
    eng = std::make_unique<AudioEngine>(kSr);
    rebuild();
  }
  void rebuild() { eng->setProgram(builder.build(doc.song())); }
  Generator* gen() const {
    auto rt = builder.channelRuntime(ch);
    return rt ? rt->gen.get() : nullptr;
  }
};

// One timed action of the performance: `fn(gen, offset in the block)`.
struct Act {
  int64_t at = 0;
  std::function<void(Generator*, int)> fn;
};

// The scripted performance (sample times from the start of play).
std::vector<Act> performance() {
  std::vector<Act> v;
  auto midi = [&](int64_t at, uint8_t s, uint8_t d1, uint8_t d2) {
    v.push_back({at, [=](Generator* g, int o) { g->midi(s, d1, d2, o); }});
  };
  auto cmd = [&](int64_t at, uint8_t kind, uint8_t a, double val, int deck) {
    v.push_back({at, [=](Generator* g, int) {
                   pl::Cmd c;
                   c.kind = kind;
                   c.a = a;
                   c.v = val;
                   c.deck = static_cast<uint8_t>(deck);
                   pl::command(g, c);
                 }});
  };
  auto button = [&](int64_t at, int ctl, int deck, int64_t hold) {
    cmd(at, pl::CmdButton, static_cast<uint8_t>(ctl), 1, deck);
    cmd(at + hold, pl::CmdButton, static_cast<uint8_t>(ctl), 0, deck);
  };
  // both decks play, the crossfader towards A
  midi(100, 0x90, 0x0B, 0x7F), midi(600, 0x90, 0x0B, 0x00);
  midi(300, 0x91, 0x0B, 0x7F), midi(700, 0x91, 0x0B, 0x00);
  midi(2000, 0xB6, 0x1F, 0x20);
  // hot cue 1 on A (set)
  midi(30000, 0x97, 0x00, 0x7F), midi(31000, 0x97, 0x00, 0x00);
  // EQ kill: A's low
  midi(40000, 0xB0, 0x0F, 0x00);
  // jog scratch on A: touch, back and forth, release
  midi(52000, 0x90, 0x36, 0x7F);
  for (int k = 0; k < 140; ++k) midi(52300 + k * 300, 0xB0, 0x22, (k / 12) % 2 ? 0x3A : 0x46);
  midi(96000, 0x90, 0x36, 0x00);
  // mouse scratch on A, then a throw
  cmd(100000, pl::CmdHandDown, 0, 0, 0);
  for (int k = 0; k < 120; ++k) cmd(100256 + k * 256, pl::CmdHandMove, 0, 0.3 * std::sin(k * 0.11), 0);
  for (int k = 0; k < 12; ++k) cmd(131000 + k * 256, pl::CmdHandMove, 0, 0.3 * std::sin(119 * 0.11) + 0.08 * (k + 1), 0);
  cmd(134200, pl::CmdHandUp, 0, 0, 0);
  // J-197: quick flicks on A's jog (a report every millisecond, 10 ticks out and back in
  // 2 x 12 ms, six times; the speed-up of the jog tracker is in the pass and its replay)
  midi(137000, 0x90, 0x36, 0x7F);
  {
    int sent = 0;
    for (int k = 0; k < 150; ++k) {
      const double t = k * 0.001, T = 0.012;
      const int c = static_cast<int>(t / (2 * T));
      const double u = t - c * 2 * T;
      const int want = c >= 6 ? 0 : static_cast<int>(std::floor(10 * (u < T ? u / T : (2 * T - u) / T) + 1e-9));
      if (want != sent) midi(137300 + k * 44, 0xB0, 0x22, static_cast<uint8_t>(0x40 + want - sent)), sent = want;
    }
  }
  midi(144300, 0x90, 0x36, 0x00);
  // hot cue 1 (jump)
  midi(150000, 0x97, 0x00, 0x7F), midi(151000, 0x97, 0x00, 0x00);
  // B: cue held, then released
  midi(160000, 0x91, 0x0C, 0x7F), midi(175000, 0x91, 0x0C, 0x00);
  midi(176000, 0x91, 0x0B, 0x7F), midi(176500, 0x91, 0x0B, 0x00);
  // A: loops, beat jumps
  button(180000, pl::CtlLoopAuto, 0, 400);
  button(200000, pl::CtlLoopHalve, 0, 400);
  button(212000, pl::CtlLoopDouble, 0, 400);
  button(225000, pl::CtlLoopAuto, 0, 400);
  button(235000, pl::CtlJumpBack, 0, 300);
  button(245000, pl::CtlJumpFwd, 0, 300);
  // crossfader cuts
  for (int k = 0; k < 14; ++k) midi(255000 + k * 1500, 0xB6, 0x1F, k % 2 ? 0x7F : 0x00);
  // pitch moves on A
  for (int k = 0; k < 40; ++k) midi(280000 + k * 1000, 0xB0, 0x00, static_cast<uint8_t>(0x40 + static_cast<int>(16 * std::sin(k * 0.2))));
  // EQ back, B's filter
  midi(325000, 0xB0, 0x0F, 0x40);
  midi(326000, 0xB6, 0x18, 0x20), midi(340000, 0xB6, 0x18, 0x40);
  // a seek on A (the record position lane)
  cmd(350000, pl::CmdSeek, 0, 50000.0, 0);
  // rim nudges on B
  for (int k = 0; k < 20; ++k) midi(360000 + k * 400, 0xB1, 0x21, k < 10 ? 0x43 : 0x3D);
  std::stable_sort(v.begin(), v.end(), [](const Act& x, const Act& y) { return x.at < y.at; });
  return v;
}

constexpr int64_t kPass = 380000;  // samples recorded

// Renders `frames` in blocks of `block`: the actions due in each block; the capture drained
// into `pb`; the step states after each block into `trace`.
void play(Rig& r, int64_t frames, int block, const std::vector<Act>* acts, std::vector<float>& L, std::vector<float>& R,
          pl::PerformanceBuilder* pb = nullptr, bool* done = nullptr, std::vector<pl::StepState>* trace = nullptr) {
  std::vector<float> bl(static_cast<size_t>(block)), br(static_cast<size_t>(block));
  std::vector<pl::CaptureEvent> evs(4096);
  size_t ai = 0;
  if (acts)
    while (ai < acts->size() && (*acts)[ai].at < r.pos) ++ai;
  for (int64_t k = 0; k < frames; k += block) {
    const int n = static_cast<int>(std::min<int64_t>(block, frames - k));
    Generator* g = r.gen();
    if (acts && g)
      for (; ai < acts->size() && (*acts)[ai].at < r.pos + n; ++ai) (*acts)[ai].fn(g, static_cast<int>((*acts)[ai].at - r.pos));
    r.eng->process(bl.data(), br.data(), n);
    L.insert(L.end(), bl.begin(), bl.begin() + n);
    R.insert(R.end(), br.begin(), br.begin() + n);
    r.pos += n;
    if (g && pb)
      for (int c; (c = pl::drainCapture(g, evs.data(), static_cast<int>(evs.size()))) > 0;)
        for (int i = 0; i < c; ++i)
          if (pb->feed(evs[static_cast<size_t>(i)]) && done) *done = true;
    if (g && trace) {
      pl::StepState s;
      pl::stepState(g, s);
      trace->push_back(s);
    }
  }
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
  double m = 0;
  for (size_t i = from; i < std::min({to, a.size(), b.size()}); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
  return m;
}
double peak(const std::vector<float>& a, size_t from, size_t to) {
  double m = 0;
  for (size_t i = from; i < std::min(to, a.size()); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i])));
  return m;
}
std::string dbfs(double x) {
  char b[32];
  if (x <= 0) std::snprintf(b, sizeof b, "-inf dBFS (bit-identical)");
  else std::snprintf(b, sizeof b, "%.1f dBFS", 20 * std::log10(x));
  return b;
}

// J-159's form (projects recorded with it): the performance into the pass's pattern as a
// start note, command key notes and lanes (song mode: through a clip).
ObjectId store(Rig& r, const pl::Performance& perf, bool song) {
  Generator* g = r.gen();
  pl::Mirror mr;
  if (g && pl::mirror(g, mr))
    for (int k = 0; k < pl::PCount; ++k)
      if (mr.seq[static_cast<size_t>(k)] != 0) r.doc.setParam(channelPluginParam(r.ch, k), mr.value[static_cast<size_t>(k)], false);
  r.doc.beginGroup("Record a deck performance");
  const ObjectId target = pl::passPattern(r.doc, r.eng->mode() == PlayMode::Song ? r.pat : r.pat, r.ch, song);
  const Tick len = static_cast<Tick>(std::ceil(perf.endTick - perf.startTick));
  const double origin = song ? static_cast<double>(pl::clipOrigin(r.doc, target, r.ch, static_cast<Tick>(std::floor(perf.startTick)), len)) : 0.0;
  pl::writePerformance(r.doc, target, r.ch, origin, perf, song);
  r.doc.endGroup();
  return target;
}

// J-163: what the editor does when a pass ends: the controller's last values into the
// project, the pass as automation (song mode: clips; pattern mode: the pass pattern's lanes).
// J-179: song mode writes one pass pattern placed on the playlist (`clips`: the setup's
// "Playlist clips" choice, J-163's grouped playlist clips). `pattern`: the pattern holding
// the pass (pattern mode: the pass pattern; song mode: the new pass pattern, kNoId for clips).
pl::PassWritten storePass(Rig& r, const pl::Performance& perf, bool song, ObjectId* pattern = nullptr, bool clips = false) {
  Generator* g = r.gen();
  pl::Mirror mr;
  if (g && pl::mirror(g, mr))
    for (int k = 0; k < pl::PCount; ++k)
      if (mr.seq[static_cast<size_t>(k)] != 0) r.doc.setParam(channelPluginParam(r.ch, k), mr.value[static_cast<size_t>(k)], false);
  r.doc.beginGroup("Record a deck performance");
  const ObjectId target = song ? kNoId : pl::passPattern(r.doc, r.pat, r.ch, false);
  const pl::PassWritten w = pl::writePass(r.doc, r.ch, perf, target, clips);
  r.doc.endGroup();
  if (pattern) *pattern = song ? w.pattern : target;
  return w;
}
bool inPass(const pl::PassWritten& w, ObjectId clip) { return std::find(w.clips.begin(), w.clips.end(), clip) != w.clips.end(); }

// The parameters a written pass holds lanes for (clips or the pattern's lanes).
std::vector<int> passParams(const Rig& r, const pl::PassWritten& w, ObjectId pattern) {
  std::vector<int> out;
  const Song& s = r.doc.song();
  for (ObjectId id : w.autos)
    if (const AutomationClip* a = s.automation(id)) out.push_back(static_cast<int>(a->index - kChannelPluginBase));
  if (const Pattern* p = s.pattern(pattern))
    for (const auto& l : p->automation)
      if (l.owner == r.ch) out.push_back(static_cast<int>(l.index - kChannelPluginBase));
  return out;
}
size_t channelNotes(const Rig& r) {
  size_t n = 0;
  for (const auto& p : r.doc.song().patterns)
    if (auto it = p.notes.find(r.ch); it != p.notes.end()) n += it->second.size();
  return n;
}
// The pass's clip of `param` (song mode: the pass pattern's, or the playlist's).
const AutomationClip* passClip(const Rig& r, const pl::PassWritten& w, int param) {
  for (ObjectId id : w.autos)
    if (const AutomationClip* a = r.doc.song().automation(id); a && a->index == kChannelPluginBase + static_cast<uint32_t>(param)) return a;
  if (const Pattern* p = r.doc.song().pattern(w.pattern)) return findPatternLane(*p, r.ch, kChannelPluginBase + static_cast<uint32_t>(param));
  return nullptr;
}
size_t firstDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
  for (size_t i = from; i < std::min({to, a.size(), b.size()}); ++i)
    if (a[i] != b[i]) return i;
  return to;
}

// ------------------------------------------------------------------ the null test
struct PassResult {
  pl::Performance perf;
  std::vector<float> L, R;
  std::vector<pl::StepState> trace;
  bool ok = false;
};

PassResult livePass(Rig& r, int block, bool song, Tick from, bool trace) {
  PassResult out;
  const auto acts = performance();
  r.eng->setMode(song ? PlayMode::Song : PlayMode::Pattern);
  r.eng->setPattern(r.pat);
  r.eng->setPosition(from);
  r.eng->setRecording(true);
  r.eng->play();
  r.pos = 0;
  pl::PerformanceBuilder pb;
  bool done = false;
  play(r, kPass, block, &acts, out.L, out.R, &pb, &done, trace ? &out.trace : nullptr);
  r.eng->setRecording(false);
  std::vector<float> tl, tr;
  play(r, 4096, block, nullptr, tl, tr, &pb, &done);
  r.eng->stop();
  out.perf = pb.perf;
  out.ok = done && !pb.lost;
  return out;
}

PassResult replay(Rig& r, int block, Tick from, bool trace) {
  PassResult out;
  r.rebuild();
  // the decks run on while stopped (and drift away from where the pass began)
  std::vector<float> tl, tr;
  r.pos = 0;
  play(r, 5000, block, nullptr, tl, tr);
  r.eng->setPosition(from);
  r.eng->play();
  r.pos = 0;
  play(r, kPass, block, nullptr, out.L, out.R, nullptr, nullptr, trace ? &out.trace : nullptr);
  r.eng->stop();
  out.ok = true;
  return out;
}

void testLegacy(const std::string& a, const std::string& b, int liveBlock, int replayBlock, bool song, bool keyLock) {
  Rig r(a, b, keyLock);
  const Tick from = song ? 2 * r.doc.song().barTicks() : 0;
  if (song) {
    r.doc.addClip(r.pat, 0, 0);
    r.rebuild();
  }
  const PassResult live = livePass(r, liveBlock, song, from, false);
  char name[200];
  std::snprintf(name, sizeof name, "J-159 data, %s, %s, live %d / replay %d%s", song ? "song mode from bar 3" : "pattern mode",
                keyLock ? "key lock" : "no key lock", liveBlock, replayBlock, "");
  expect(live.ok, (std::string("a pass is captured as a performance: ") + name).c_str());
  if (!live.ok) return;
  expect(peak(live.L, 0, kPass) > 0.05, "the live pass is audible");
  const ObjectId target = store(r, live.perf, song);
  const Pattern* p = r.doc.song().pattern(target);
  size_t notes = 0;
  std::vector<int> lanes;
  if (p) {
    if (auto it = p->notes.find(r.ch); it != p->notes.end()) notes = it->second.size();
    for (const auto& l : p->automation)
      if (l.owner == r.ch) lanes.push_back(static_cast<int>(l.index - kChannelPluginBase));
  }
  std::printf("# %s: %zu notes, %zu lanes in the pattern\n", name, notes, lanes.size());
  auto has = [&](int param) { return std::find(lanes.begin(), lanes.end(), param) != lanes.end(); };
  // start, play A, play B, hot cue (set, jump), cue B, play B, auto loop (twice), halve,
  // double, jump back, jump forward, the wheel touch (twice), the mouse hand
  expect(target == r.pat && notes == 16,"J-159 data: the start note and a note per button press");
  expect(has(pl::PXfader) && has(pl::deckParam(0, pl::DEqLow)) && has(pl::deckParam(0, pl::DPitch)) && has(pl::deckParam(1, pl::DFilter)) &&
             has(pl::deckParam(0, pl::DPos)) && has(pl::deckParam(0, pl::DMouse)) && has(pl::deckParam(0, pl::DWheel)) &&
             has(pl::deckParam(1, pl::DNudge)) && lanes.size() == 8,
         "J-159 data lanes: crossfader, A's low EQ and pitch, B's filter; A's seek, mouse and wheel travel, B's nudge");
  const PassResult rep = replay(r, replayBlock, from, false);
  const double dl = maxDiff(live.L, rep.L, 0, kPass), dr = maxDiff(live.R, rep.R, 0, kPass);
  const double d = std::max(dl, dr);
  std::printf("# %s: replay vs live, largest difference %s over %lld samples\n", name, dbfs(d).c_str(), static_cast<long long>(kPass));
  if (d > 0) {
    size_t first = 0;
    for (; first < static_cast<size_t>(kPass); ++first)
      if (live.L[first] != rep.L[first] || live.R[first] != rep.R[first]) break;
    std::printf("# first different sample %zu\n", first);
  }
  expect(d == 0, (std::string("J-159 data still replays bit for bit: ") + name).c_str());
  if (song || keyLock || liveBlock != 256 || replayBlock != 256) return;
  // editing the data changes the sound from the edit on (and nothing before it)
  const ParamId eq = channelPluginParam(r.ch, pl::deckParam(0, pl::DEqLow));
  const PatternAutomation* lane = findPatternLane(*r.doc.song().pattern(target), eq.owner, eq.index);
  expect(lane && lane->points.size() >= 2, "A's low EQ lane is there to edit");
  if (!lane) return;
  const std::vector<AutoPoint> orig = lane->points;
  std::vector<AutoPoint> pts = orig;
  for (auto& q : pts) q.value = std::max(q.value, 0.5f);  // no kill
  r.doc.editPattern(target, "", [&](Pattern& x) { setPatternLane(x, eq.owner, eq.index, pts); });
  const PassResult ed = replay(r, 256, from, false);
  size_t first = 0;
  for (; first < static_cast<size_t>(kPass); ++first)
    if (live.L[first] != ed.L[first] || live.R[first] != ed.R[first]) break;
  std::printf("# edited EQ lane: the first changed sample is %zu (the kill was at 40000)\n", first);
  expect(first >= 40000 && first < 40000 + 2 * pl::kStep && maxDiff(live.L, ed.L, 40000, 325000) > 0.01,
         "an edited lane changes the replay from where the edit is, the same before it");
  // deleting the mouse hand's note: the scratch is gone
  r.doc.editPattern(target, "", [&](Pattern& x) {
    auto& ns = x.notes[r.ch];
    ns.erase(std::remove_if(ns.begin(), ns.end(), [](const Note& n) { return n.pitch == pl::cmdKeyPitch(0, pl::KeyMouse); }), ns.end());
    setPatternLane(x, eq.owner, eq.index, orig);
  });
  const PassResult nm = replay(r, 256, from, false);
  {
    size_t f2 = 0;
    while (f2 < nm.L.size() && nm.L[f2] == live.L[f2]) ++f2;
    std::printf("# without the mouse note: %g before, %g during, first change %zu\n", maxDiff(live.L, nm.L, 0, 100000), maxDiff(live.L, nm.L, 100000, 150000), f2);
  }
  expect(maxDiff(live.L, nm.L, 0, 99800) == 0 && maxDiff(live.L, nm.L, 100000, 150000) > 0.01,  // (the hand went down on the step after the block the mouse came in)
         "without the mouse hand's note the mouse scratch is gone (the same until it)");
}

// Each control's value and both decks, step by step (32 sample blocks: one per step).
void testStreams(const std::string& a, const std::string& b) {
  Rig r(a, b, false);
  const PassResult live = livePass(r, pl::kStep, false, 0, true);
  expect(live.ok, "streams: a pass is captured");
  if (!live.ok) return;
  storePass(r, live.perf, false);
  const PassResult rep = replay(r, pl::kStep, 0, true);
  const size_t n = std::min(live.trace.size(), rep.trace.size());
  int badParam = -1;
  size_t badStep = 0;
  bool posOk = true;
  size_t moved = 0;
  std::vector<bool> changed(static_cast<size_t>(pl::PCount), false);
  size_t driven = 0;
  for (size_t i = 0; i < n; ++i) {
    driven += rep.trace[i].driven[0] && rep.trace[i].driven[1] ? 1 : 0;
    for (int k = 0; k < pl::PCount; ++k) {
      if (pl::perfParam(k)) continue;
      const auto kk = static_cast<size_t>(k);
      if (i > 0 && live.trace[i].p[kk] != live.trace[i - 1].p[kk]) changed[kk] = true;
      if (live.trace[i].p[kk] != rep.trace[i].p[kk] && badParam < 0) badParam = k, badStep = i;
    }
    for (int d = 0; d < pl::kDecks; ++d)
      if (live.trace[i].pos[d] != rep.trace[i].pos[d] || live.trace[i].vel[d] != rep.trace[i].vel[d]) {
        if (posOk) std::printf("# deck %d differs first at step %zu: pos %.9f / %.9f, speed %.9f / %.9f\n", d, i, live.trace[i].pos[d],
                               rep.trace[i].pos[d], live.trace[i].vel[d], rep.trace[i].vel[d]);
        posOk = false;
      }
  }
  for (bool c : changed) moved += c ? 1 : 0;
  if (badParam >= 0) std::printf("# parameter %d differs first at step %zu\n", badParam, badStep);
  std::printf("# streams: %zu steps compared, %zu controls moved, both decks driven on %zu\n", n, moved, driven);
  expect(driven + 2 >= n, "streams: the pass's state lanes drive both decks all along");
  expect(n >= static_cast<size_t>(kPass / pl::kStep) && moved >= 4, "streams: every step of the pass compared, the moved controls among them");
  expect(badParam < 0, "streams: every control's value is the same on every step (live and replayed)");
  expect(posOk, "streams: both decks' read position and speed are the same on every step");
  expect(maxDiff(live.L, rep.L, 0, kPass) == 0, "streams: the audio is the same");
}

// A second pass over a pattern that holds a performance goes into a new pattern.
void testSecondPass(const std::string& a, const std::string& b) {
  Rig r(a, b, false);
  PassResult one = livePass(r, 256, false, 0, false);
  ObjectId p1 = kNoId, p2 = kNoId;
  storePass(r, one.perf, false, &p1);
  const Pattern before = *r.doc.song().pattern(p1);
  PassResult two = livePass(r, 256, false, 0, false);
  expect(one.ok && two.ok, "two passes captured");
  const size_t undoBefore = r.doc.history().position();
  storePass(r, two.perf, false, &p2);
  const Pattern* q1 = r.doc.song().pattern(p1);
  const Pattern* q2 = r.doc.song().pattern(p2);
  expect(p2 != p1 && q1 && q2, "the second pass records into a new pattern");
  expect(q1 && *q1 == before, "the first pattern is untouched");
  expect(q2 && q2->name == "Decks take 2" && pl::holdsPerformance(*q2, r.ch), "the new pattern is named after the setup and holds the second pass");
  expect(r.doc.history().position() == undoBefore + 1, "a pass is one undo step (new pattern included)");
  r.doc.undo();
  expect(r.doc.song().pattern(p2) == nullptr && r.doc.song().pattern(p1) && *r.doc.song().pattern(p1) == before,
         "undo removes the second pass and its pattern");
  // the start states: one per performance still in the project
  std::string path;
  pl::Model m;
  pl::parseData(r.doc.song().channel(r.ch)->plugin.data, path, m);
  expect(m.starts.size() == 1, "the setup keeps the start state of the pass in the project (its position lanes name it)");
  // model text round trip (exact)
  pl::Model back;
  std::string p0;
  expect(pl::parseData(pl::formatData(path, m), p0, back) && back == m && back.starts.size() == 1 && back.starts[0].v == m.starts[0].v,
         "start states survive the model text exactly");
  // a project save / load keeps the sub-tick positions
  const std::string proj = (tmpDir() / "perf.gridline.json").string();
  std::string err;
  expect(saveProject(proj, r.doc.song(), &err), "a project with a performance saves");
  Song t;
  expect(loadProject(proj, t, &err), "... and loads");
  const Pattern* lp = t.pattern(p1);
  bool same = lp && lp->notes == before.notes && lp->automation.size() == before.automation.size();
  size_t fracs = 0;
  if (same)
    for (size_t i = 0; i < before.automation.size(); ++i) {
      same = same && lp->automation[i].points == before.automation[i].points;
      for (const auto& q : before.automation[i].points) fracs += q.frac != 0 ? 1 : 0;
    }
  for (const auto& [c, ns] : before.notes)
    for (const auto& nn : ns) fracs += nn.frac != 0 || nn.endFrac != 0 ? 1 : 0;
  size_t fines = 0;
  for (const auto& l : before.automation)
    for (const auto& q : l.points) fines += q.fine != 0 ? 1 : 0;
  std::printf("# %zu sub-tick positions, %zu exact values in the pattern\n", fracs, fines);
  expect(same && fracs > 0 && fines > 0, "save / load: lane points come back with their fractions and exact values");
}

// Loop recording (pattern mode, a 2 bar pattern): every loop pass is a performance of its own,
// the first in the pattern, the next ones in new patterns (a pass never merges into another);
// each replays exactly what its loop played live, the one that starts on the wrap included.
void testLoop(const std::string& a, const std::string& b, int block) {
  Rig r(a, b, false);
  r.doc.editPattern(r.pat, "", [](Pattern& p) { p.bars = 2; });
  r.rebuild();
  const int64_t loop = 176400;  // 2 bars at 120 BPM
  const auto acts = performance();
  r.eng->setMode(PlayMode::Pattern);
  r.eng->setPattern(r.pat);
  r.eng->setPosition(0);
  r.eng->setRecording(true);
  r.eng->play();
  r.pos = 0;
  std::vector<float> L, R;
  std::vector<pl::Performance> perfs;
  {
    std::vector<float> bl(static_cast<size_t>(block)), br(static_cast<size_t>(block));
    std::vector<pl::CaptureEvent> evs(4096);
    pl::PerformanceBuilder pb;
    size_t ai = 0;
    const int64_t total = 2 * loop + 30000;
    for (int64_t k = 0; k < total; k += block) {
      const int n = static_cast<int>(std::min<int64_t>(block, total - k));
      Generator* g = r.gen();
      for (; ai < acts.size() && acts[ai].at < r.pos + n; ++ai) acts[ai].fn(g, static_cast<int>(acts[ai].at - r.pos));
      if (k + n >= total) r.eng->setRecording(false);
      r.eng->process(bl.data(), br.data(), n);
      L.insert(L.end(), bl.begin(), bl.begin() + n);
      R.insert(R.end(), br.begin(), br.begin() + n);
      r.pos += n;
      for (int c; (c = pl::drainCapture(g, evs.data(), static_cast<int>(evs.size()))) > 0;)
        for (int i = 0; i < c; ++i)
          if (pb.feed(evs[static_cast<size_t>(i)])) perfs.push_back(pb.perf);
    }
    std::vector<float> tl(static_cast<size_t>(block)), tr(static_cast<size_t>(block));
    for (int k = 0; k < 8; ++k) {
      r.eng->process(tl.data(), tr.data(), block);
      for (int c; (c = pl::drainCapture(r.gen(), evs.data(), static_cast<int>(evs.size()))) > 0;)
        for (int i = 0; i < c; ++i)
          if (pb.feed(evs[static_cast<size_t>(i)])) perfs.push_back(pb.perf);
    }
    r.eng->stop();
  }
  char name[64];
  std::snprintf(name, sizeof name, " (%d sample blocks)", block);
  expect(perfs.size() == 3 && perfs[0].startTick == 0 && perfs[1].startTick == 0 && perfs[2].startTick == 0,
         (std::string("loop recording: a performance per loop pass, each from the loop start") + name).c_str());
  if (perfs.size() < 2) return;
  std::vector<ObjectId> pats;
  for (const auto& pf : perfs) {
    ObjectId p = kNoId;
    storePass(r, pf, false, &p);
    pats.push_back(p);
  }
  expect(pats.size() == 3 && pats[0] == r.pat && pats[1] != pats[0] && pats[2] != pats[1] && pats[2] != pats[0],
         "loop recording: the first pass in the pattern, each next one in a new pattern");
  for (size_t k = 0; k < 2; ++k) {
    r.eng->setPattern(pats[k]);
    const PassResult rep = replay(r, block == 256 ? 1024 : 256, 0, false);
    const size_t from = k * static_cast<size_t>(loop);
    double d = 0;
    for (size_t i = 0; i < static_cast<size_t>(loop); ++i)
      d = std::max({d, static_cast<double>(std::fabs(rep.L[i] - L[from + i])), static_cast<double>(std::fabs(rep.R[i] - R[from + i]))});
    std::printf("# loop pass %zu replayed: largest difference %s\n", k + 1, dbfs(d).c_str());
    expect(d == 0, (std::string("loop recording: pass ") + std::to_string(k + 1) + " replays its loop bit for bit" + name).c_str());
  }
}

// J-175: song mode, an empty playlist (the song is one bar long), no time selection. Gridline
// records on past the song end (the transport does not loop under a recording, as for audio):
// the whole performance is one pass, from the song start to where it stopped, and replays bit
// for bit once the song loops again. With the song looping, each wrap would end the pass and
// start another one (the J-175 failure: a pass split at the song end).
void testSongEnd(const std::string& a, const std::string& b, int block) {
  for (const bool looping : {false, true}) {
    Rig r(a, b, false);
    const Tick bar = r.doc.song().barTicks();
    const std::shared_ptr<const Program> prog = r.eng->program();
    expect(prog && prog->songLoopEnd == bar, "song end: an empty song ends after one bar");
    const int64_t barS = 88200;  // (a bar at 120 BPM)
    const auto acts = performance();
    r.eng->setMode(PlayMode::Song);
    r.eng->setLooping(looping);  // (what Gridline does while it records in song mode without a selection)
    r.eng->setPosition(0);
    r.eng->setRecording(true);
    r.eng->play();
    r.pos = 0;
    std::vector<float> L, R;
    std::vector<pl::Performance> perfs;
    bool lost = false;
    {
      std::vector<float> bl(static_cast<size_t>(block)), br(static_cast<size_t>(block));
      std::vector<pl::CaptureEvent> evs(4096);
      pl::PerformanceBuilder pb;
      size_t ai = 0;
      for (int64_t k = 0; k < kPass + 8 * block; k += block) {
        const int n = static_cast<int>(std::min<int64_t>(block, kPass + 8 * block - k));
        Generator* g = r.gen();
        for (; ai < acts.size() && acts[ai].at < r.pos + n; ++ai) acts[ai].fn(g, static_cast<int>(acts[ai].at - r.pos));
        if (k >= kPass) r.eng->setRecording(false);  // (Stop: the tail drains the pass)
        r.eng->process(bl.data(), br.data(), n);
        L.insert(L.end(), bl.begin(), bl.begin() + n);
        R.insert(R.end(), br.begin(), br.begin() + n);
        r.pos += n;
        for (int c; (c = pl::drainCapture(g, evs.data(), static_cast<int>(evs.size()))) > 0;)
          for (int i = 0; i < c; ++i)
            if (pb.feed(evs[static_cast<size_t>(i)])) perfs.push_back(pb.perf);
      }
      lost = pb.lost;
      r.eng->stop();
    }
    char name[96];
    std::snprintf(name, sizeof name, " (%d sample blocks)", block);
    if (looping) {
      // the cause: a song that loops under the pass splits it at every wrap (a bar each)
      const size_t want = static_cast<size_t>((kPass + barS - 1) / barS);
      expect(perfs.size() == want && perfs[0].startTick == 0 && perfs[0].endTick == static_cast<double>(bar),
             (std::string("song end, looping: the song wraps under the pass, a pass per bar") + name).c_str());
      continue;
    }
    std::printf("# song end%s: %zu pass(es), ticks %.3f to %.3f, the song ends at %lld\n", name, perfs.size(), perfs.empty() ? 0.0 : perfs[0].startTick,
                perfs.empty() ? 0.0 : perfs[0].endTick, static_cast<long long>(bar));
    expect(perfs.size() == 1 && !lost, (std::string("song end: song mode records one pass past the song end, not split there") + name).c_str());
    if (perfs.size() != 1) continue;
    const pl::Performance& perf = perfs[0];
    expect(perf.startTick == 0 && perf.endTick > 4.0 * static_cast<double>(bar),
           (std::string("song end: the pass runs from the song start through the whole performance (4+ bars)") + name).c_str());
    const pl::PassWritten w = storePass(r, perf, true);
    expect(!w.clips.empty() && r.doc.song().lengthTicks() > 4 * bar, "song end: the pass's clips reach past the old song end");
    // played back with the song looping again (it is as long as the pass now)
    r.eng->setLooping(true);
    const PassResult rep = replay(r, block == 256 ? 1024 : 256, 0, false);
    const double d = std::max(maxDiff(L, rep.L, 0, static_cast<size_t>(kPass)), maxDiff(R, rep.R, 0, static_cast<size_t>(kPass)));
    std::printf("# song end%s replayed: largest difference %s\n", name, dbfs(d).c_str());
    expect(d == 0 && peak(L, static_cast<size_t>(barS), static_cast<size_t>(kPass)) > 0.05,
           (std::string("song end: the pass replays bit for bit, past the old song end too") + name).c_str());
  }
}

// Punch in / out (song mode): the pass runs exactly over the punch range (bars `in` to
// `in` + 2) and replays it exactly. Bar 2 starts inside a jog scratch, bar 3 is quiet.
void testPunch(const std::string& a, const std::string& b, int block, int in) {
  Rig r(a, b, false);
  r.doc.addClip(r.pat, 0, 0);
  r.rebuild();
  const Tick bar = r.doc.song().barTicks();
  const Tick pin = in * bar, pout = (in + 2) * bar;
  const size_t s0 = static_cast<size_t>(in) * 88200, len = 2 * 88200;  // (88200 samples a bar at 120 BPM)
  r.eng->setPunch(static_cast<double>(pin), static_cast<double>(pout));
  const PassResult live = livePass(r, block, true, 0, false);
  r.eng->setPunch(0, 0);
  char name[64];
  std::snprintf(name, sizeof name, " (bar %d, %d sample blocks)", in + 1, block);
  expect(live.ok && live.perf.startTick == static_cast<double>(pin) && live.perf.endTick == static_cast<double>(pout),
         (std::string("punch: the pass runs from the punch in to the punch out, exactly") + name).c_str());
  if (!live.ok) return;
  storePass(r, live.perf, true);
  const PassResult rep = replay(r, block == 256 ? 1024 : 256, pin, false);
  double d = 0;
  size_t first = len;
  for (size_t i = 0; i < len; ++i) {
    const double di = std::max(std::fabs(static_cast<double>(rep.L[i] - live.L[s0 + i])), std::fabs(static_cast<double>(rep.R[i] - live.R[s0 + i])));
    if (di > 0 && first == len) first = i;
    d = std::max(d, di);
  }
  std::printf("# punch%s replayed: largest difference %s (first at %zu)\n", name, dbfs(d).c_str(), first);
  expect(d == 0 && peak(live.L, s0, s0 + len) > 0.05, (std::string("punch: the punched pass replays bit for bit") + name).c_str());
}

// ------------------------------------------------------------------ J-163: passes as automation
// The unedited pass, written as automation, replays the live pass bit for bit.
void testPassNull(const std::string& a, const std::string& b, int liveBlock, int replayBlock, bool song, bool keyLock, bool clips = false) {
  Rig r(a, b, keyLock);
  const Tick bar = r.doc.song().barTicks();
  const Tick from = song ? 2 * bar : 0;
  if (song) {
    r.doc.addClip(r.pat, 0, 0);  // (the backing: track 0 is taken)
    r.rebuild();
  }
  const PassResult live = livePass(r, liveBlock, song, from, false);
  char name[200];
  std::snprintf(name, sizeof name, "%s, %s, live %d / replay %d", song ? (clips ? "song mode from bar 3, playlist clips" : "song mode from bar 3")
                                                                       : "pattern mode",
                keyLock ? "key lock" : "no key lock", liveBlock, replayBlock);
  expect(live.ok, (std::string("a pass is captured: ") + name).c_str());
  if (!live.ok) return;
  ObjectId pattern = kNoId;
  const pl::PassWritten w = storePass(r, live.perf, song, &pattern, clips);
  if (song) {
    // one undo step: undo takes the whole pass away (its pattern, clips and placed clip), redo brings it back
    const Song with = r.doc.song();
    r.doc.undo();
    const Song& s = r.doc.song();
    bool gone = s.pattern(w.pattern) == nullptr;
    for (ObjectId id : w.clips) gone = gone && s.clip(id) == nullptr;
    for (ObjectId id : w.autos) gone = gone && s.automation(id) == nullptr;
    r.doc.redo();
    expect(gone && r.doc.song().playlist == with.playlist && r.doc.song().patterns == with.patterns && r.doc.song().automations == with.automations,
           (std::string("a song pass is one undo step (undo removes all of it, redo restores it): ") + name).c_str());
  }
  const std::vector<int> params = passParams(r, w, pattern);
  auto has = [&](int deck, int dp) { return std::find(params.begin(), params.end(), pl::deckParam(deck, dp)) != params.end(); };
  auto hasP = [&](int p) { return std::find(params.begin(), params.end(), p) != params.end(); };
  {
    size_t pa = 0, pb = 0;
    for (const auto& c : live.perf.clips) {
      if (c.param == pl::PTrackA) pa = c.points.size();
      if (c.param == pl::deckParam(1, pl::DTrack)) pb = c.points.size();
    }
    std::printf("# %s: %zu lanes, %zu notes; position points A %zu, B %zu (%zu steps)\n", name, params.size(), channelNotes(r), pa, pb,
                static_cast<size_t>(kPass / pl::kStep));
  }
  expect(channelNotes(r) == 0, "a pass writes no notes (no piano roll)");
  expect(has(0, pl::DTrack) && has(1, pl::DTrack) && has(0, pl::DPlay) && has(1, pl::DPlay) && has(0, pl::DTouch) && has(0, pl::DWheel) &&
             has(0, pl::DHand) && has(0, pl::DMouse) && has(1, pl::DNudge) && has(0, pl::DLoop) && has(0, pl::DLoopIn) && has(0, pl::DLoopOut),
         "state lanes: both decks' position and play; A's wheel touch and travel, mouse hand and travel, loop; B's nudge");
  expect(hasP(pl::PXfader) && has(0, pl::DEqLow) && has(0, pl::DPitch) && has(1, pl::DFilter), "control lanes: crossfader, A's low EQ and pitch, B's filter");
  expect(!has(0, pl::DPos) && !has(1, pl::DTouch) && !has(0, pl::DReverse), "no lane for what the pass did not use (no seek lane: the position holds seeks)");
  if (song && !clips) {
    // J-179: one new pattern holds the pass's clips; one clip of it on the playlist where the
    // pass was played, as long as the pass, on a free track (track 0 holds the backing)
    const Song& s = r.doc.song();
    const Pattern* pp = s.pattern(w.pattern);
    const Clip* c = w.clips.size() == 1 ? s.clip(w.clips[0]) : nullptr;
    const Tick len = static_cast<Tick>(std::ceil(live.perf.endTick - static_cast<double>(from)));
    size_t placed = 0;
    for (const auto& x : s.playlist) placed += x.kind == ClipKind::Pattern && x.pattern == w.pattern ? 1 : 0;
    std::printf("# %s: pass pattern \"%s\", %zu clips, placed on track %d at %lld, %lld ticks (pass %.3f to %.3f)\n", name, pp ? pp->name.c_str() : "?",
                pp ? pp->automation.size() : 0, c ? c->track : -1, c ? static_cast<long long>(c->start) : -1LL,
                c ? static_cast<long long>(s.clipLength(*c)) : -1LL, live.perf.startTick, live.perf.endTick);
    expect(pp && pp->name == "Decks take 1" && w.pattern != r.pat && w.autos.empty() && s.automations.empty(),
           "song mode: the pass goes into a new pattern \"<setup> take N\" (no playlist automation clips)");
    expect(c && c->kind == ClipKind::Pattern && c->pattern == w.pattern && c->start == from && s.clipLength(*c) == len && c->track == 1 &&
               c->offset == 0 && placed == 1 && w.firstTrack == 1,
           "song mode: one clip of the pass pattern, where the pass was played, as long as the pass, on the first free track");
    expect(pp && static_cast<Tick>(pp->bars) * bar >= len && static_cast<Tick>(pp->bars - 1) * bar < len, "song mode: the pass pattern's bars cover the pass");
    const AutomationClip* pos = passClip(r, w, pl::PTrackA);
    const AutomationClip* wheel = passClip(r, w, pl::deckParam(0, pl::DWheel));
    bool named = pp && !pp->automation.empty() && static_cast<int>(pp->automation.size()) == w.lanes;
    if (pp)
      for (const auto& l : pp->automation) named = named && !l.name.empty() && l.flCurves && l.owner == r.ch;
    expect(named && pos && pos->name == "Decks A position" && wheel && wheel->name == "Decks A wheel" && wheel->color == 0x4FA3D9 &&
               pos->max - pos->min < 1.0f && !pos->points.empty() && pos->points.front().pos == 0,
           "the pass pattern's clips are named and coloured, start on the pattern's first tick; the position clip is zoomed on the record's travel");
  } else if (song) {
    const Song& s = r.doc.song();
    bool together = w.group != kNoId && !w.clips.empty();
    int t0 = 1 << 30, t1 = -1;
    for (ObjectId id : w.clips) {
      const Clip* c = s.clip(id);
      together = together && c && c->group == w.group && c->start == from && c->kind == ClipKind::Automation;
      if (c) t0 = std::min(t0, c->track), t1 = std::max(t1, c->track);
    }
    expect(together && t1 - t0 + 1 == static_cast<int>(w.clips.size()) && t0 >= 1,
           "song mode: one automation clip per lane where the pass was played, on adjacent free tracks, in one clip group");
    const AutomationClip* pos = passClip(r, w, pl::PTrackA);
    const AutomationClip* wheel = passClip(r, w, pl::deckParam(0, pl::DWheel));
    expect(pos && pos->name == "Decks A position" && wheel && wheel->name == "Decks A wheel" && pos->max - pos->min < 1.0f,
           "clips are named after the setup and the control; the position clip is zoomed on the record's travel");
  } else {
    expect(pattern == r.pat && w.clips.empty(), "pattern mode: the lanes go into the (empty) pattern");
    // J-173: the pattern's lanes are named, coloured automation clips, as the song mode's are
    const Pattern* pp = r.doc.song().pattern(pattern);
    const PatternAutomation* pos = pp ? findPatternLane(*pp, r.ch, kChannelPluginBase + static_cast<uint32_t>(pl::PTrackA)) : nullptr;
    const PatternAutomation* wheel = pp ? findPatternLane(*pp, r.ch, kChannelPluginBase + static_cast<uint32_t>(pl::deckParam(0, pl::DWheel))) : nullptr;
    bool named = pp && !pp->automation.empty();
    if (pp)
      for (const auto& l : pp->automation) named = named && !l.name.empty() && l.flCurves && l.id == kNoId;
    expect(named && pos && pos->name == "Decks A position" && wheel && wheel->name == "Decks A wheel" && wheel->color == 0x4FA3D9 &&
               pos->max - pos->min < 1.0f,
           "J-173 pattern mode: the pass pattern holds named, coloured clips; the position clip is zoomed on the record's travel");
  }
  const PassResult rep = replay(r, replayBlock, from, false);
  const double d = std::max(maxDiff(live.L, rep.L, 0, kPass), maxDiff(live.R, rep.R, 0, kPass));
  std::printf("# %s: replay vs live, largest difference %s (first at %zu)\n", name, dbfs(d).c_str(), firstDiff(live.L, rep.L, 0, kPass));
  expect(d == 0, (std::string("the unedited pass replays the live pass bit for bit: ") + name).c_str());
  if (song) {
    // saved and loaded (a project with a pass in either form: J-163's grouped playlist clips
    // are what earlier projects hold), it plays the same
    const std::string proj = (tmpDir() / "songpass.gridline.json").string();
    std::string err;
    Song loaded;
    const bool io = saveProject(proj, r.doc.song(), &err) && loadProject(proj, loaded, &err);
    expect(io, (std::string("a project with a song pass saves and loads: ") + name).c_str());
    if (io) {
      r.doc.reset(loaded);
      const PassResult back = replay(r, replayBlock, from, false);
      const double db = std::max(maxDiff(live.L, back.L, 0, kPass), maxDiff(live.R, back.R, 0, kPass));
      std::printf("# %s: saved, loaded and replayed: %s\n", name, dbfs(db).c_str());
      expect(db == 0, (std::string("saved and loaded, the pass still replays bit for bit: ") + name).c_str());
    }
  }
  if (song || keyLock || liveBlock != 256 || replayBlock != 256) return;
  // ---- edits change the sound from where they are (pattern mode)
  auto lanePts = [&](int param) {
    const PatternAutomation* l = findPatternLane(*r.doc.song().pattern(pattern), r.ch, kChannelPluginBase + static_cast<uint32_t>(param));
    return l ? l->points : std::vector<AutoPoint>{};
  };
  auto setPts = [&](int param, std::vector<AutoPoint> pts) {
    r.doc.editPattern(pattern, "", [&](Pattern& x) { setPatternLane(x, r.ch, kChannelPluginBase + static_cast<uint32_t>(param), std::move(pts)); });
  };
  // the low EQ lane without its kill
  const int eq = pl::deckParam(0, pl::DEqLow);
  const std::vector<AutoPoint> eqOrig = lanePts(eq);
  {
    std::vector<AutoPoint> pts = eqOrig;
    for (auto& q : pts) q.value = std::max(q.value, 0.5f);
    setPts(eq, pts);
    const PassResult ed = replay(r, 256, from, false);
    const size_t first = firstDiff(live.L, ed.L, 0, kPass);
    std::printf("# edited EQ lane: the first changed sample is %zu (the kill was at 40000)\n", first);
    expect(first >= 40000 && first < 40000 + 2 * pl::kStep && maxDiff(live.L, ed.L, 40000, 325000) > 0.01,
           "an edited control lane changes the replay from the edit on (the same before it)");
    setPts(eq, eqOrig);
  }
  // the position lane: a jump added at 1 s (on deck A)
  const std::vector<AutoPoint> posOrig = lanePts(pl::PTrackA);
  {
    const double t = 44100.0 * 384.0 / 88200.0;  // ticks at 1 s
    const double v = autoedit::valueAt(posOrig, t);
    std::vector<AutoPoint> pts = posOrig;
    AutoPoint p0, p1;
    p0.pos = static_cast<Tick>(t), p0.value = static_cast<float>(v), p0.shape = AutoShape::Curve;
    p1 = p0;
    p1.value = static_cast<float>(v + 0.02), p1.shape = AutoShape::Hold;
    autoedit::addPoint(pts, p0);
    autoedit::addPoint(pts, p1);
    setPts(pl::PTrackA, pts);
    const PassResult ed = replay(r, 256, from, false);
    const size_t first = firstDiff(live.L, ed.L, 0, kPass);
    const size_t at = static_cast<size_t>(static_cast<double>(p0.pos) * 88200.0 / 384.0);
    std::printf("# a jump drawn into A's position lane at sample %zu: the first changed sample is %zu\n", at, first);
    expect(first + pl::kStep >= at && first <= at + 2 * pl::kStep && maxDiff(live.L, ed.L, at, at + 4000) > 0.01,
           "a jump drawn into the position lane: the record jumps there (the same before it)");
    setPts(pl::PTrackA, posOrig);
  }
  // the mouse hand's lane deleted: the mouse scratch is gone
  {
    r.doc.editPattern(pattern, "", [&](Pattern& x) {
      x.automation.erase(std::remove_if(x.automation.begin(), x.automation.end(),
                                        [&](const PatternAutomation& l) {
                                          return l.owner == r.ch && l.index == kChannelPluginBase + static_cast<uint32_t>(pl::deckParam(0, pl::DHand));
                                        }),
                         x.automation.end());
    });
    const PassResult nm = replay(r, 256, from, false);
    std::printf("# without the mouse hand lane: %g before, %g during, first change %zu\n", maxDiff(live.L, nm.L, 0, 100000),
                maxDiff(live.L, nm.L, 100000, 150000), firstDiff(live.L, nm.L, 0, kPass));
    expect(maxDiff(live.L, nm.L, 0, 99800) == 0 && maxDiff(live.L, nm.L, 100000, 150000) > 0.01,
           "without the mouse hand's lane the mouse scratch is gone (the same until it)");
  }
}

// Played from the middle: playback started inside the pass (a steady part, a scratch), a
// trimmed clip start, a clip cut in two, the clips moved later, a copy after it (song mode;
// J-179: the pass pattern's clip, `clips`: J-163's grouped playlist clips).
void testPickUp(const std::string& a, const std::string& b, bool clips) {
  Rig r(a, b, false);
  const Tick bar = r.doc.song().barTicks();  // (88200 samples)
  const Tick from = 2 * bar;
  r.doc.addClip(r.pat, 0, 0);
  r.doc.editClips("", [&](std::vector<Clip>& v) { v.back().length = 16 * bar; });
  r.rebuild();
  const PassResult live = livePass(r, 256, true, from, false);
  expect(live.ok, "pick up: a pass is captured");
  if (!live.ok) return;
  const pl::PassWritten w = storePass(r, live.perf, true, nullptr, clips);
  std::printf("# pick up: %s\n", clips ? "the pass as playlist clips" : "the pass as a pattern clip");
  expect(clips ? w.clips.size() > 1 : w.clips.size() == 1 && w.pattern != kNoId, "pick up: the pass is written in the form asked for");
  const PassResult full = replay(r, 256, from, false);
  expect(maxDiff(live.L, full.L, 0, kPass) == 0, "pick up: the full replay is the live pass");
  const double spt = 88200.0 / static_cast<double>(bar);  // samples per tick
  // Playback from `tick` (inside the pass): the decks pick it up within a step; compared with
  // the full replay from there: the samples until it matches to -96 dBFS for good, and the
  // largest difference after `settle` samples.
  auto midStart = [&](Tick tick, size_t span, const char* what, size_t settle, double tol, Tick base = -1) {
    const PassResult mid = replay(r, 256, tick, false);
    const size_t off = static_cast<size_t>(static_cast<double>(tick - (base < 0 ? from : base)) * spt);
    std::vector<float> ref(full.L.begin() + static_cast<long>(off), full.L.end());
    size_t lastBad = 0;
    double after = 0;
    for (size_t i = 0; i < std::min(span, ref.size()); ++i) {
      const double di = std::fabs(static_cast<double>(mid.L[i] - ref[i]));
      if (di > 1.6e-5) lastBad = i + 1;
      if (i >= settle) after = std::max(after, di);
    }
    std::printf("# playback from %s: matches the full replay after %zu samples (%.1f ms); after %zu samples the largest difference is %s\n", what,
                lastBad, 1000.0 * static_cast<double>(lastBad) / kSr, settle, dbfs(after).c_str());
    return std::make_pair(lastBad, after <= tol);
  };
  // a steady part (both decks playing, A's low EQ killed): 0.75 s into the pass. The lanes hold
  // the record's place to 1/256 frame, so the pick-up is that close; the decks then run on as
  // they did (the filters' and smoothers' memory of the sound before is not in the lanes).
  {
    const auto res = midStart(from + 144, 28000, "a steady part", 32 + 128, 0.001);
    expect(res.first <= 2048 && res.second,
           "playback started in a steady part: the decks pick the pass up within a step and a crossfade (-60 dBFS), then match it (-96 dBFS) within 50 ms");
  }
  // playback started on a tick whose step falls between two samples: still picked up
  {
    const auto res = midStart(from + 268, 28000, "between two samples", 32 + 128, 0.05);
    expect(res.second, "playback started between two samples of the pass's step grid: the decks still pick the pass up (within a step)");
  }
  // inside the jog scratch (1.4 s in): the scratch goes on from the lanes (the jog filter runs
  // over the hand's last steps first); it has settled once the hand lets go
  {
    const Tick t = from + 272;  // (62475 samples: on a whole sample)
    const auto res = midStart(t, static_cast<size_t>(200000 - 62475), "inside a jog scratch", static_cast<size_t>(118000 - 62475), 0.002);
    expect(res.second && res.first <= static_cast<size_t>(118000 - 62475),
           "playback started inside a scratch: the pass plays on from there and has settled (-96 dBFS) once the hand has let go");
  }
  // a trimmed clip start: the decks are free before it, the pass's from it
  {
    const Tick cutAt = from + 144;
    r.doc.editClips("", [&](std::vector<Clip>& v) {
      for (auto& c : v)
        if (inPass(w, c.id)) c.offset += cutAt - c.start, c.length -= cutAt - c.start, c.start = cutAt;
    });
    const PassResult tr = replay(r, 256, from, false);
    const size_t off = static_cast<size_t>(144 * spt);
    size_t lastBad = 0;
    for (size_t i = off; i < off + 28000; ++i)
      if (std::fabs(tr.L[i] - full.L[i]) > 1.6e-5f) lastBad = i + 1 - off;
    std::printf("# trimmed clip start: the deck follows the pass %zu samples after the clip's start\n", lastBad);
    expect(lastBad <= 2048, "a trimmed clip start: the pass is picked up where the clip starts (matching it within 50 ms)");
    r.doc.undo();
  }
  // the clips cut in two (in a scratch): the same, sample for sample
  {
    const Tick cut = from + static_cast<Tick>(70000.0 / spt);
    const auto right = ops::sliceAt(r.doc, cut, -1);
    const PassResult cutRep = replay(r, 256, from, false);
    const double d = maxDiff(full.L, cutRep.L, 0, kPass);
    std::printf("# clips cut in two (%zu cuts): %s against the uncut replay\n", right.size(), dbfs(d).c_str());
    expect(right.size() >= w.clips.size() && d == 0, "the pass's clips cut in two (in a scratch) play the same, bit for bit");
    r.doc.undo();
  }
  // the clips moved two bars later: the pass plays the same from its new place
  {
    r.doc.editClips("", [&](std::vector<Clip>& v) {
      for (auto& c : v)
        if (inPass(w, c.id)) c.start += 2 * bar;
    });
    const PassResult mv = replay(r, 256, from + 2 * bar, false);
    const double d = maxDiff(full.L, mv.L, 0, kPass);
    std::printf("# clips moved two bars later: %s against the replay in place\n", dbfs(d).c_str());
    expect(d == 0, "the pass's clips moved later play the same from their new place, bit for bit");
    r.doc.undo();
  }
  // a copy after the pass (Duplicate, then moved onto the next bar: the decks keep in time with
  // the project, so a pass plays the same where it lands on the beat as it was played): played
  // from its start it is the pass, bit for bit; played from inside it the decks pick it up as
  // they do in the original
  {
    const std::set<ObjectId> sel(w.clips.begin(), w.clips.end());
    const std::vector<ObjectId> copies = ops::duplicateClips(r.doc, sel);
    const Clip* c0 = copies.empty() ? nullptr : r.doc.song().clip(copies.front());
    const Tick at = c0 ? (c0->start + bar - 1) / bar * bar : from;
    if (c0) {
      const Tick shift = at - c0->start;
      const std::set<ObjectId> cs(copies.begin(), copies.end());
      r.doc.editClips("", [&](std::vector<Clip>& v) {
        for (auto& c : v)
          if (cs.count(c.id)) c.start += shift;
      });
    }
    const PassResult dup = replay(r, 256, at, false);
    const double d = maxDiff(full.L, dup.L, 0, kPass);
    std::printf("# a copy of the pass at tick %lld: %s against the replay of the original\n", static_cast<long long>(at), dbfs(d).c_str());
    expect(copies.size() == w.clips.size() && at > from && d == 0, "a copy of the pass's clips plays the pass from its start, bit for bit");
    const auto res = midStart(at + 144, 28000, "inside the copy (a steady part)", 32 + 128, 0.001, at);
    expect(res.first <= 2048 && res.second, "playback started inside the copy: the decks pick the pass up as in the original");
    r.doc.undo();  // (the move, then the copy)
    if (!copies.empty()) r.doc.undo();
  }
  // outside the pass the decks are free: the hands play them
  {
    pl::StepState st;
    r.rebuild();
    r.eng->setPosition(from + 6 * bar);
    r.eng->play();
    std::vector<float> l, rr;
    r.pos = 0;
    play(r, 4096, 256, nullptr, l, rr);
    pl::stepState(r.gen(), st);
    r.eng->stop();
    expect(!st.driven[0] && !st.driven[1], "after the pass's clips the decks are free");
  }
}

// J-179: a project holding both forms (a pass as J-163's playlist clips, from an earlier
// version or the "Playlist clips" choice, and a pass as a pattern clip): each plays exactly
// where it is placed, the other one's lanes (on the same controls) do not reach into it.
void testMixed(const std::string& a, const std::string& b) {
  Rig r(a, b, false);
  const Tick bar = r.doc.song().barTicks();
  const Tick from = 2 * bar, later = from + 8 * bar;
  r.doc.addClip(r.pat, 0, 0);
  r.doc.editClips("", [&](std::vector<Clip>& v) { v.back().length = 16 * bar; });
  r.rebuild();
  const PassResult live = livePass(r, 256, true, from, false);
  expect(live.ok, "mixed: a pass is captured");
  if (!live.ok) return;
  const pl::PassWritten old = storePass(r, live.perf, true, nullptr, true);
  r.doc.editClips("", [&](std::vector<Clip>& v) {
    for (auto& c : v)
      if (inPass(old, c.id)) c.start += later - from;
  });
  const pl::PassWritten now = storePass(r, live.perf, true);
  expect(old.clips.size() > 1 && old.autos.size() == old.clips.size() && now.clips.size() == 1 && now.pattern != kNoId,
         "mixed: one pass as playlist clips (moved 8 bars later), the same pass as a pattern clip where it was played");
  const PassResult a1 = replay(r, 256, from, false);
  const PassResult a2 = replay(r, 1024, later, false);
  const double d1 = std::max(maxDiff(live.L, a1.L, 0, kPass), maxDiff(live.R, a1.R, 0, kPass));
  const double d2 = std::max(maxDiff(live.L, a2.L, 0, kPass), maxDiff(live.R, a2.R, 0, kPass));
  std::printf("# mixed: the pattern clip's pass %s, the playlist clips' pass %s\n", dbfs(d1).c_str(), dbfs(d2).c_str());
  expect(d1 == 0, "mixed: the pass pattern's clip replays bit for bit beside the playlist clips of another pass");
  expect(d2 == 0, "mixed: the playlist clips (J-163's form) replay bit for bit beside a pass pattern");
}

// A state lane an earlier pass plays over a new pass's time, the new one does not hold: the
// new pass gets its own (off) lane, so the earlier state does not carry into it.
void testOverlap(const std::string& a, const std::string& b) {
  pl::Performance one;
  one.startTick = 0, one.endTick = 3000, one.decks = 1;
  one.scale[0] = pl::PosScale(44100 * 12, kSr);
  one.clips.push_back(pl::PerfLane{pl::PTrackA, {pl::PerfPoint{0, 0.5, 0, AutoShape::Curve}, pl::PerfPoint{3000, 0.6, 0, AutoShape::Curve}}});
  one.clips.push_back(pl::PerfLane{pl::deckParam(0, pl::DLoop), {pl::PerfPoint{0, 1.0}}});
  pl::Performance two = one;
  two.startTick = 1000, two.endTick = 2000;
  two.clips.pop_back();
  for (auto& l : two.clips)
    for (auto& q : l.points) q.tick += 1000;
  for (const bool clips : {true, false}) {
    Rig q(a, b, false);
    const pl::PassWritten w1 = pl::writePass(q.doc, q.ch, one, kNoId, clips);
    const pl::PassWritten w2 = pl::writePass(q.doc, q.ch, two, kNoId, clips);
    const AutomationClip* loop2 = passClip(q, w2, pl::deckParam(0, pl::DLoop));
    const char* form = clips ? " (playlist clips)" : " (pattern)";
    expect(w1.lanes == 2 && w2.lanes == 2 && loop2 && loop2->points.size() == 1 && loop2->points[0].value == 0.0f,
           (std::string("a later pass over an earlier one's loop gets its own loop lane (off)") + form).c_str());
    if (clips) {
      expect(w1.clips.size() == 2 && w2.clips.size() == 2 && w2.firstTrack >= w1.firstTrack + 2 && w2.group != w1.group,
             "the second pass goes on tracks of its own, in a group of its own (playlist clips)");
    } else {
      const Clip* c1 = w1.clips.size() == 1 ? q.doc.song().clip(w1.clips[0]) : nullptr;
      const Clip* c2 = w2.clips.size() == 1 ? q.doc.song().clip(w2.clips[0]) : nullptr;
      const Pattern* p1 = q.doc.song().pattern(w1.pattern);
      const Pattern* p2 = q.doc.song().pattern(w2.pattern);
      expect(c1 && c2 && p1 && p2 && w1.pattern != w2.pattern && c2->track != c1->track && c2->start == 1000 && q.doc.song().clipLength(*c2) == 1000 &&
                 p1->name == "Decks take 1" && p2->name == "Decks take 2" && q.doc.song().automations.empty(),
             "the second pass is a pattern of its own (\"take 2\"), its clip on a track of its own");
    }
  }
}

// Projects from before J-163: a setup's parameter list from J-159 (56 values) gets the
// state lanes' defaults; the J-159 start states keep their layout.
void testOldParams() {
  PluginState s = defaultPluginState(pl::kType);
  s.params.resize(pl::kLegacyPCount);
  normalizePluginState(s);
  bool defaults = static_cast<int>(s.params.size()) == pl::PCount;
  for (int p = pl::PTrackA; p < pl::PCount && defaults; ++p) {
    const auto& d = findPlugin(pl::kType)->params[static_cast<size_t>(p)];
    defaults = s.params[static_cast<size_t>(p)] == static_cast<float>(d.def);
  }
  expect(defaults, "a J-159 parameter list is completed with the state lanes' defaults");
  // J-179: the song pass form is saved in the model (one pattern unless "Playlist clips")
  {
    pl::Model m, back;
    const std::string plain = pl::modelToText(m);
    m.songClips = true;
    const std::string withClips = pl::modelToText(m);
    pl::modelFromText(plain, back);  // (an empty model reads as "no model": the fields still come back)
    const bool plainOk = !back.songClips;
    pl::modelFromText(withClips, back);
    expect(plain.find("songpass") == std::string::npos && plainOk && back.songClips && pl::modelToText(back).find("songpass clips\n") != std::string::npos,
           "the setup's song pass form: one pattern by default (not written), Playlist clips saved and loaded");
  }
  pl::PosScale sc(529200, kSr);
  bool exact = true;
  for (double x : {-88201.0, -1.5, 0.0, 1.0 / 256, 12345.67578125, 529199.99609375, 529200.0 + 88200.0})
    exact = exact && sc.toPos(sc.toN(x)) == x;
  expect(exact, "a position lane's scale maps the read head's 1/256 frame grid to 0..1 and back exactly");
}

// Takes still work: recorded as a take, a pass is a take note.
void testTakeMode(const std::string& a, const std::string& b) {
  Rig r(a, b, false);
  r.doc.editChannel(r.ch, "", [](Channel& c) {
    std::string p;
    pl::Model m;
    pl::parseData(c.plugin.data, p, m);
    m.recordAs = pl::RecTake;
    c.plugin.data = pl::formatData(p, m);
  });
  r.rebuild();
  const auto acts = performance();
  r.eng->setMode(PlayMode::Pattern);
  r.eng->setPattern(r.pat);
  r.eng->setRecording(true);
  r.eng->play();
  r.pos = 0;
  std::vector<float> L, R;
  std::vector<pl::CaptureEvent> evs(4096);
  pl::TakeBuilder tb;
  pl::PerformanceBuilder pb;
  bool take = false, perf = false;
  std::vector<float> bl(256), br(256);
  for (int64_t k = 0; k < 100000; k += 256) {
    r.eng->process(bl.data(), br.data(), 256);
    if (k > 90000) r.eng->setRecording(false);
    for (int c; (c = pl::drainCapture(r.gen(), evs.data(), static_cast<int>(evs.size()))) > 0;)
      for (int i = 0; i < c; ++i) {
        take = tb.feed(evs[static_cast<size_t>(i)]) || take;
        perf = pb.feed(evs[static_cast<size_t>(i)]) || perf;
      }
  }
  (void)acts;
  expect(take && !perf && tb.take.steps() > 0, "record as take: the pass is a take (no performance)");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  auto want = [&](const char* n) { return only.empty() || only == n; };
  const std::string a = writeTrack("perf120.wav", 12, 120), b = writeTrack("perf100.wav", 12, 100);
  if (want("null")) {
    testPassNull(a, b, 256, 256, false, false);
    testPassNull(a, b, 64, 64, false, false);
    testPassNull(a, b, 1024, 1024, false, false);
    testPassNull(a, b, 256, 1024, false, false);
    testPassNull(a, b, 1024, 64, false, false);
    testPassNull(a, b, 64, 256, true, false);
    testPassNull(a, b, 256, 256, true, true);
    testPassNull(a, b, 256, 1024, true, false, true);  // (the "Playlist clips" choice: J-163's form)
  }
  if (want("mixed")) testMixed(a, b);
  if (want("legacy")) {
    testLegacy(a, b, 256, 256, false, false);
    testLegacy(a, b, 1024, 64, false, false);
    testLegacy(a, b, 64, 256, true, false);
  }
  if (want("pickup")) {
    testPickUp(a, b, false);
    testPickUp(a, b, true);
  }
  if (want("overlap")) testOverlap(a, b);
  if (want("old")) testOldParams();
  if (want("streams")) testStreams(a, b);
  if (want("second")) testSecondPass(a, b);
  if (want("take")) testTakeMode(a, b);
  if (want("punch")) {
    for (int in : {1, 2}) {
      testPunch(a, b, 256, in);
      testPunch(a, b, 1024, in);
    }
  }
  if (want("songend")) {
    testSongEnd(a, b, 256);
    testSongEnd(a, b, 1024);
  }
  if (want("loop")) {
    testLoop(a, b, 256);
    testLoop(a, b, 1024);
  }
  return finish();
}
