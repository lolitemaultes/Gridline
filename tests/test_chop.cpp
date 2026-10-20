// Gridline — PL-13: chop / stutter / region editing in the playlist.
// Slicing exactness (no gaps or overlaps, sample-accurate audio windows),
// transient detection on test signals, stutter results (audio and pattern),
// chop presets, time selection operations, one undo step per action and the
// project / FLP round trip of chopped arrangements. Synthetic songs only.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Automation.h"
#include "core/Document.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Chop.h"
#include "engine/ClipOps.h"
#include "engine/Render.h"
#include "engine/Sample.h"
#include "packs/edison/edison.h"

namespace fs = std::filesystem;
using namespace gl;
namespace ed = gl::packs::edison;

namespace {

std::string gDir;
constexpr double kSr = 44100;

std::string writeTestWav(const std::string& name, double secs, const std::function<float(double)>& f) {
  ed::Audio a = ed::makeAudio(kSr, 2, static_cast<int64_t>(std::llround(secs * kSr)));
  for (int64_t i = 0; i < a.frames(); ++i) a.L[static_cast<size_t>(i)] = a.R[static_cast<size_t>(i)] = f(static_cast<double>(i) / kSr);
  std::string p = gDir + "/" + name + ".wav";
  ed::writeWavFile(p, ed::makeContent(a), 32);
  return p;
}

ObjectId addAudioClip(Song& s, const std::string& path, Tick start, Tick length, int track = 0) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = fs::path(path).stem().string();
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = track;
  c.start = start;
  c.length = length;
  s.playlist.push_back(c);
  return c.id;
}

ObjectId addPatternClip(Song& s, const std::vector<Note>& notes, Tick start, Tick length, int track = 0) {
  if (s.channels.empty()) {
    Channel ch;
    ch.id = s.allocateId();
    ch.name = "Synth";
    ch.plugin = defaultPluginState("gl.sampler");
    ch.mixerTrack = s.masterId();
    s.channels.push_back(ch);
  }
  Pattern p;
  p.id = s.allocateId();
  p.name = "Riff";
  p.bars = 4;
  p.notes[s.channels[0].id] = notes;
  s.patterns.push_back(p);
  Clip c;
  c.id = s.allocateId();
  c.pattern = p.id;
  c.track = track;
  c.start = start;
  c.length = length;
  s.playlist.push_back(c);
  return c.id;
}

std::vector<const Clip*> piecesOf(const Song& s, const std::vector<ObjectId>& ids) {
  std::vector<const Clip*> v;
  for (ObjectId id : ids)
    if (const Clip* c = s.clip(id)) v.push_back(c);
  std::sort(v.begin(), v.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
  return v;
}

// Pieces tile [a, b) exactly; audio pieces start where the original read at that tick.
bool tiles(const Song& s, const Song& orig, const Clip& o, const std::vector<const Clip*>& v, Tick a, Tick b) {
  if (v.empty() || v.front()->start != a) return false;
  TempoMap map;
  map.build(orig);
  Tick t = a;
  for (const Clip* c : v) {
    if (c->start != t || s.clipLength(*c) <= 0) return false;
    if (o.kind == ClipKind::Audio && std::fabs(c->audioStart - clipSourceAt(orig, o, map, static_cast<double>(c->start))) > 1e-9) return false;
    if (o.kind == ClipKind::Pattern && c->offset != o.offset + (c->start - o.start)) return false;
    t += s.clipLength(*c);
  }
  return t == b;
}

double maxDiff(const AudioBuffer& x, const AudioBuffer& y, size_t from, size_t to, const std::vector<size_t>& skip, size_t radius) {
  double err = 0;
  for (size_t i = from; i < std::min({to, x.frames, y.frames}); ++i) {
    bool near = false;
    for (size_t k : skip) near = near || (i + radius >= k && i <= k + radius);
    if (!near) err = std::max(err, static_cast<double>(std::fabs(x.at(0, i) - y.at(0, i))));
  }
  return err;
}

// ======================================================================== slicing

void testSlicing() {
  const std::string tone = writeTestWav("tone", 3.0, [](double t) { return 0.3f * static_cast<float>(std::sin(2 * M_PI * 220 * t)); });
  // Grid: an off-grid audio clip cut at the song's grid lines.
  Song s = makeEmptySong();
  s.tempo = 120;
  const ObjectId au = addAudioClip(s, tone, 50, 300);
  const Song orig = s;
  auto ids = chop::sliceAtGrid(s, {au}, 96);
  auto v = piecesOf(s, ids);
  expect(v.size() == 4 && v[1]->start == 96 && v[2]->start == 192 && v[3]->start == 288, "grid: cuts on the grid lines");
  expect(tiles(s, orig, *orig.clip(au), v, 50, 350), "grid: pieces tile the clip, audio sample-accurate");
  // Equal parts: 301 ticks into 4 (lengths differ by at most one tick).
  Song e = makeEmptySong();
  e.tempo = 120;
  const ObjectId au2 = addAudioClip(e, tone, 10, 301);
  const ObjectId pc = addPatternClip(e, {Note(0, 24, 60, 100), Note(96, 24, 62, 100)}, 0, 384, 1);
  const Song eo = e;
  ids = chop::sliceEqual(e, {au2, pc}, 4);
  std::vector<ObjectId> a1, p1;
  for (ObjectId id : ids) (e.clip(id)->kind == ClipKind::Audio ? a1 : p1).push_back(id);
  auto va = piecesOf(e, a1), vp = piecesOf(e, p1);
  bool even = va.size() == 4;
  for (const Clip* c : va) even = even && (c->length == 75 || c->length == 76);
  expect(even && tiles(e, eo, *eo.clip(au2), va, 10, 311), "equal parts: 4 audio pieces, no gaps or overlaps");
  expect(vp.size() == 4 && tiles(e, eo, *eo.clip(pc), vp, 0, 384), "equal parts: pattern pieces keep their content offsets");
  for (int n : {2, 3, 5, 7, 16}) {
    Song q = eo;
    auto r = piecesOf(q, chop::sliceEqual(q, {au2}, n));
    expect(static_cast<int>(r.size()) == n && tiles(q, eo, *eo.clip(au2), r, 10, 311), ("equal parts: exact tiling into " + std::to_string(n)).c_str());
  }
  // Sample-accurate: the sliced clip plays what the whole clip played (away from the 2 ms edge declicks).
  {
    RenderOptions ro;
    ro.sampleRate = static_cast<int>(kSr);
    Song w = makeEmptySong();
    w.tempo = 120;
    const ObjectId c = addAudioClip(w, tone, 0, 384);
    const AudioBuffer before = renderSong(w, ro);
    auto r = piecesOf(w, chop::sliceEqual(w, {c}, 7));
    const AudioBuffer after = renderSong(w, ro);
    std::vector<size_t> cuts;
    for (const Clip* k : r) cuts.push_back(static_cast<size_t>(std::llround(static_cast<double>(k->start) / 192.0 * kSr)));
    const double err = maxDiff(before, after, 0, static_cast<size_t>(1.99 * kSr), cuts, static_cast<size_t>(0.003 * kSr));
    expect(r.size() == 7 && err < 1e-5, "equal parts: the pieces play the same audio as the whole clip");
  }
  // Transients: clicks at irregular times are found where they are.
  const double at[] = {0.0, 0.31, 0.72, 1.18, 1.5};
  const std::string clicks = writeTestWav("clicks", 2.0, [&](double t) {
    for (double c : at) {
      const double ph = t - c;
      if (ph >= 0 && ph < 0.03) return 0.8f * static_cast<float>(std::exp(-ph * 150) * std::sin(2 * M_PI * 900 * t));
    }
    return 0.0f;
  });
  Song tr = makeEmptySong();
  tr.tempo = 120;
  const ObjectId ck = addAudioClip(tr, clicks, 0, 384);
  const Song tro = tr;
  auto tv = piecesOf(tr, chop::sliceAtTransients(tr, {ck}, 0.5));
  bool onClicks = tv.size() == 5;
  for (size_t i = 0; onClicks && i < tv.size(); ++i) onClicks = std::fabs(static_cast<double>(tv[i]->start) - at[i] * 192.0) <= 2.0;
  expect(onClicks, "transients: one slice per click, cut at the clicks");
  expect(tiles(tr, tro, *tro.clip(ck), tv, 0, 384), "transients: the slices tile the clip");
  // Pattern clips: at the note starts (steps included).
  Song pt = makeEmptySong();
  const ObjectId pk = addPatternClip(pt, {Note(0, 24, 60, 100), Note(40, 24, 62, 100), Note(200, 24, 64, 100)}, 96, 384);
  pt.patterns.back().steps[pt.channels[0].id] = std::vector<uint8_t>(16, 0);
  pt.patterns.back().steps[pt.channels[0].id][12] = 100;  // step 12 = tick 288
  pt.clip(pk)->offset = 0;
  auto pv = piecesOf(pt, chop::sliceAtTransients(pt, {pk}, 0.5));
  expect(pv.size() == 4 && pv[1]->start == 96 + 40 && pv[2]->start == 96 + 200 && pv[3]->start == 96 + 288,
         "pattern transients: cuts at the note and step starts");
}

// ======================================================================== stutter

void testPlan() {
  chop::StutterParams p;
  p.rate = 16;  // 24 ticks at 96 PPQ
  auto st = chop::plan(p, 384, 96);
  bool ok = st.size() == 16;
  for (size_t i = 0; i < st.size(); ++i) ok = ok && st[i].at == static_cast<Tick>(i) * 24 && st[i].slot == 24 && st[i].play == 24 && st[i].src == 0;
  expect(ok, "plan: 1/16 repeats tile a bar");
  p.triplet = true;
  st = chop::plan(p, 96, 96);
  expect(st.size() == 6 && st[0].slot == 16, "plan: triplet sixteenths");
  p = chop::StutterParams{};
  p.rate = 4, p.endRate = 32;
  st = chop::plan(p, 384, 96);
  Tick sum = 0;
  bool shrinking = true;
  for (size_t i = 0; i < st.size(); ++i) sum += st[i].slot, shrinking = shrinking && (i == 0 || i + 1 == st.size() || st[i].slot <= st[i - 1].slot);
  expect(sum == 384 && shrinking && st.front().slot == 96 && st[st.size() - 2].slot <= 16, "plan: an end rate rolls the slices shorter, still tiling");
  p = chop::StutterParams{};
  p.rate = 8, p.decay = 0.5, p.pitchStart = 0, p.pitchEnd = 12, p.reverse = 2, p.gate = 0.5;
  st = chop::plan(p, 384, 96);
  expect(st.size() == 8 && std::fabs(st[3].gain - 0.125) < 1e-12 && std::fabs(st[7].semis - 12) < 1e-12 && !st[0].reverse && st[1].reverse &&
             st[2].play == 24,
         "plan: decay, pitch ramp, every-other reverse, gate");
  p.decay = -0.5;
  st = chop::plan(p, 384, 96);
  expect(std::fabs(st.back().gain - 1.0) < 1e-12 && std::fabs(st[6].gain - 0.5) < 1e-12, "plan: negative decay builds up");
  p = chop::presetParams(chop::Preset::HalfTime);
  st = chop::plan(p, 384, 96);
  expect(st.size() == 16 && std::fabs(st[5].src - 5 * 24 * 0.5) < 1e-9 && std::fabs(st[5].speed - 0.5) < 1e-12, "plan: half-time plays on at half speed");
  p = chop::presetParams(chop::Preset::TapeStop);
  st = chop::plan(p, 384, 96);
  bool slowing = true;
  for (size_t i = 1; i < st.size(); ++i) slowing = slowing && st[i].speed < st[i - 1].speed && st[i].src > st[i - 1].src;
  expect(slowing && st.back().speed < 0.2, "plan: tape stop slows down while playing on");
  for (int i = 0; i < chop::kPresetCount; ++i) {
    const auto pr = static_cast<chop::Preset>(i);
    const auto q = chop::plan(chop::presetParams(pr), 384, 96);
    Tick tot = 0;
    for (const auto& x : q) tot += x.slot;
    expect(!q.empty() && tot == 384 && *chop::presetName(pr) && *chop::presetHint(pr), (std::string("preset tiles: ") + chop::presetName(pr)).c_str());
  }
}

void testStutterAudio() {
  const std::string tone = writeTestWav("tone2", 3.0, [](double t) { return 0.3f * static_cast<float>(std::sin(2 * M_PI * 330 * t)); });
  Song s = makeEmptySong();
  s.tempo = 120;
  const ObjectId c = addAudioClip(s, tone, 0, 4 * 96);
  s.clip(c)->gain = 0.8f;
  const Song orig = s;
  TempoMap map;
  map.build(orig);
  chop::StutterParams p;
  p.rate = 8, p.decay = 0.25, p.pitchEnd = 12, p.reverse = 2, p.gate = 0.75, p.fadeMs = 3;
  auto ids = chop::stutter(s, {c}, 96, 192, p);
  auto v = piecesOf(s, ids);
  expect(v.size() == 2, "audio stutter: one clip per slice (1/8 over a beat)");
  // The region is cut out of the original: the head and tail keep playing around it.
  const Clip* head = s.clip(c);
  expect(head && head->start == 0 && head->length == 96, "audio stutter: the head ends at the region");
  bool tail = false;
  for (const auto& k : s.playlist)
    if (k.start == 192 && k.length == 192 && std::fabs(k.audioStart - clipSourceAt(orig, *orig.clip(c), map, 192)) < 1e-9) tail = true;
  expect(tail, "audio stutter: the tail continues at its own audio");
  const double src0 = clipSourceAt(orig, *orig.clip(c), map, 96);
  if (v.size() == 2) {
    expect(v[0]->start == 96 && v[0]->length == 36 && v[1]->start == 144 && v[1]->length == 36, "audio stutter: slots and gate");
    expect(std::fabs(v[0]->audioStart - src0) < 1e-9 && !v[0]->reverse, "audio stutter: the first slice plays the region start");
    expect(std::fabs(v[1]->gain - 0.8f * 0.75f) < 1e-6 && std::fabs(v[1]->pitch - 1200.0f) < 1e-3, "audio stutter: decay and pitch on the clip");
    // Reverse: the same source window read backwards.
    SamplePtr smp = sampleCache().get(tone);
    const double D = static_cast<double>(smp->frames() - 1) / smp->sampleRate;
    Clip fwd = *v[1];
    fwd.reverse = false, fwd.audioStart = src0, fwd.audioEnd = 0;
    const double s1 = clipSourceAt(s, fwd, map, static_cast<double>(fwd.start + fwd.length));
    expect(v[1]->reverse && std::fabs(v[1]->audioStart - (D - s1)) < 1e-9 && std::fabs(v[1]->audioEnd - (D - src0)) < 1e-9,
           "audio stutter: a reversed slice reads its window backwards");
    expect(v[0]->fadeInManual && std::fabs(v[0]->fadeIn - 0.003) < 1e-12 && std::fabs(v[0]->fadeOut - 0.003) < 1e-12, "audio stutter: slice fades");
  }
  // Render: a beat repeat of the first eighth sounds like it, twice.
  {
    Song r = orig;
    chop::StutterParams br = chop::presetParams(chop::Preset::BeatRepeat);
    br.fadeMs = 0;
    chop::stutter(r, {c}, 96, 192, br);
    RenderOptions ro;
    ro.sampleRate = static_cast<int>(kSr);
    const AudioBuffer out = renderSong(r, ro), ref = renderSong(orig, ro);
    const size_t q0 = static_cast<size_t>(0.5 * kSr), q = static_cast<size_t>(0.25 * kSr);
    double err = 0;
    for (size_t i = static_cast<size_t>(0.004 * kSr); i + static_cast<size_t>(0.004 * kSr) < q; ++i)
      err = std::max({err, static_cast<double>(std::fabs(out.at(0, q0 + i) - ref.at(0, q0 + i))),
                      static_cast<double>(std::fabs(out.at(0, q0 + q + i) - ref.at(0, q0 + i)))});
    expect(out.frames > q0 + 2 * q && err < 1e-4, "audio stutter: beat repeat replays the region's first eighth");
  }
  // Tape stop: plays on, every slice slower (stretch up), reading forward.
  {
    Song t = orig;
    auto tv = piecesOf(t, chop::stutter(t, {c}, 0, 384, chop::presetParams(chop::Preset::TapeStop)));
    bool ok = tv.size() == 32;
    for (size_t i = 1; ok && i < tv.size(); ++i) ok = tv[i]->stretch > tv[i - 1]->stretch && tv[i]->audioStart > tv[i - 1]->audioStart;
    expect(ok, "tape stop: slices slow down and play on");
  }
  // Half-time: slice k starts at half the content position.
  {
    Song h = orig;
    auto hv = piecesOf(h, chop::stutter(h, {c}, 0, 384, chop::presetParams(chop::Preset::HalfTime)));
    bool ok = hv.size() == 16;
    for (size_t i = 0; ok && i < hv.size(); ++i)
      ok = std::fabs(hv[i]->audioStart - clipSourceAt(orig, *orig.clip(c), map, static_cast<double>(i) * 12.0)) < 1e-9 &&
           std::fabs(hv[i]->stretch - 2.0) < 1e-12;
    expect(ok, "half-time: every slice plays half a slice of audio at half speed");
  }
}

void testStutterPattern() {
  Song s = makeEmptySong();
  std::vector<Note> notes;
  for (int i = 0; i < 16; ++i) notes.push_back(Note(i * 24, 12, 60 + i, 100));
  const ObjectId c = addPatternClip(s, notes, 0, 4 * 96);
  const ObjectId oldPat = s.clip(c)->pattern;
  chop::StutterParams p;
  p.rate = 16, p.decay = 0.5, p.pitchEnd = 3;
  auto ids = chop::stutter(s, {c}, 96, 192, p);
  expect(ids.size() == 1, "pattern stutter: one clip for the region");
  const Clip* k = ids.empty() ? nullptr : s.clip(ids[0]);
  const Pattern* np = k ? s.pattern(k->pattern) : nullptr;
  expect(k && k->start == 96 && k->length == 96 && k->offset == 0 && np && k->pattern != oldPat, "pattern stutter: a new pattern over the region");
  if (np) {
    const auto& ns = np->notes.begin()->second;
    // Repeats of the region's first note (pitch 64), quieter each time, pitch ramped 0..3.
    bool ok = ns.size() == 4;
    for (size_t i = 0; ok && i < ns.size(); ++i)
      ok = ns[i].start == static_cast<Tick>(i) * 24 && ns[i].pitch == 64 + static_cast<int>(std::lround(3.0 * static_cast<double>(i) / 3.0)) &&
           ns[i].velocity == static_cast<int>(std::lround(100 * std::pow(0.5, static_cast<double>(i)))) && ns[i].length == 12;
    expect(ok, "pattern stutter: note repeats with decay and pitch ramp");
  }
  expect(s.pattern(oldPat)->notes.begin()->second.size() == 16, "pattern stutter: the original pattern is untouched");
  // Reverse + gate: a note in the first half of its slice lands in the second half.
  Song r = makeEmptySong();
  const ObjectId rc = addPatternClip(r, {Note(0, 6, 60, 100)}, 0, 96);
  chop::StutterParams rp;
  rp.rate = 16, rp.reverse = 1;
  auto rids = chop::stutter(r, {rc}, 0, 96, rp);
  const Pattern* rpn = rids.empty() ? nullptr : r.pattern(r.clip(rids[0])->pattern);
  expect(rpn && rpn->notes.begin()->second.size() == 4 && rpn->notes.begin()->second[0].start == 18 && rpn->notes.begin()->second[1].start == 42,
         "pattern stutter: reversed slices mirror the notes");
  // Half-time on a pattern: notes twice as far apart and twice as long.
  Song h = makeEmptySong();
  const ObjectId hc = addPatternClip(h, {Note(0, 12, 60, 100), Note(24, 12, 62, 100), Note(48, 12, 64, 100)}, 0, 96);
  auto hids = chop::stutter(h, {hc}, 0, 96, chop::presetParams(chop::Preset::HalfTime));
  const Pattern* hp = hids.empty() ? nullptr : h.pattern(h.clip(hids[0])->pattern);
  expect(hp && hp->notes.begin()->second.size() == 2 && hp->notes.begin()->second[1].start == 48 && hp->notes.begin()->second[0].length == 24,
         "pattern half-time: notes spread to twice the distance");
}

// ======================================================================== regions

void testRegions() {
  const std::string tone = writeTestWav("tone3", 3.0, [](double t) { return 0.3f * static_cast<float>(std::sin(2 * M_PI * 110 * t)); });
  Song s = makeEmptySong();
  s.tempo = 120;
  const ObjectId pc = addPatternClip(s, {Note(0, 24, 60, 100), Note(120, 24, 62, 100)}, 0, 384, 0);
  const ObjectId ac = addAudioClip(s, tone, 0, 384, 1);
  const Song orig = s;
  TempoMap map;
  map.build(orig);
  // Duplicate [96, 192) → [192, 288): the destination is cleared first.
  Song d = s;
  auto ids = chop::duplicateRange(d, 96, 192);
  expect(ids.size() == 2, "duplicate time: one copy per track");
  int pieces = 0;
  bool exact = true;
  for (const auto& c : d.playlist) {
    if (c.track == 0) {
      ++pieces;
      if (c.start == 192) exact = exact && c.offset == 96 && c.length == 96;
      if (c.start == 288) exact = exact && c.offset == 288;
    }
    if (c.track == 1 && c.start == 192) exact = exact && std::fabs(c.audioStart - clipSourceAt(orig, *orig.clip(ac), map, 96)) < 1e-9;
  }
  expect(pieces == 4 && exact, "duplicate time: the copy plays the source content; the rest stays in place");
  // Insert space on track 1 only.
  Song ins = s;
  chop::insertSpace(ins, 96, 48, {1});
  bool okIns = ins.clip(pc)->start == 0 && ins.clip(pc)->length == 384;
  int right = 0;
  for (const auto& c : ins.playlist)
    if (c.track == 1 && c.start == 144) right++, okIns = okIns && std::fabs(c.audioStart - clipSourceAt(orig, *orig.clip(ac), map, 96)) < 1e-9;
  expect(okIns && right == 1 && ins.clip(ac)->length == 96, "insert space: cuts across the point, later audio moves right");
  // Delete space: the spanning audio clip loses exactly its middle.
  Song del = s;
  del.markers.push_back(Marker{});
  del.markers.back().pos = 300;
  chop::deleteSpace(del, 96, 192);
  int on1 = 0;
  bool okDel = del.clip(ac)->length == 96;
  for (const auto& c : del.playlist)
    if (c.track == 1 && c.start == 96) ++on1, okDel = okDel && c.length == 192 && std::fabs(c.audioStart - clipSourceAt(orig, *orig.clip(ac), map, 192)) < 1e-9;
  for (const auto& c : del.playlist)
    if (c.track == 0 && c.start == 96) okDel = okDel && c.offset == 192;
  expect(on1 == 1 && okDel && del.markers.back().pos == 204, "delete space: sample-accurate cut, the rest and markers move left");
  // Consolidate the time selection: rendered audio of [96, 192) on a free track.
  Song con = makeEmptySong();
  con.tempo = 120;
  const ObjectId cc = addAudioClip(con, tone, 0, 384, 0);
  RenderOptions ro;
  ro.sampleRate = static_cast<int>(kSr);
  const AudioBuffer ref = renderSong(con, ro);
  const ObjectId nc = chop::consolidateRange(con, 96, 192, {}, gDir + "/Rendered", kSr);
  const Clip* k = nc ? con.clip(nc) : nullptr;
  expect(k && k->start == 96 && k->length == 96 && k->track == 1 && k->kind == ClipKind::Audio, "consolidate: one audio clip over the selection");
  if (k) {
    con.clip(cc)->muted = true;
    const AudioBuffer out = renderSong(con, ro);
    const double err = maxDiff(ref, out, static_cast<size_t>(0.505 * kSr), static_cast<size_t>(0.995 * kSr), {}, 0);
    expect(err < 1e-3, "consolidate: the rendered clip sounds like the selection");
  }
  // Chop it: a slicer channel, one marker and one note per slice.
  Song ch = makeEmptySong();
  ch.tempo = 120;
  addAudioClip(ch, tone, 0, 384, 0);
  const auto r = chop::chopRangeToSlicer(ch, 0, 384, {}, 48, 0.5, gDir + "/Rendered", kSr);
  const Channel* sl = ch.channel(r.channel);
  const Pattern* sp = ch.pattern(r.pattern);
  expect(r.slices == 8 && sl && sl->plugin.type == "gl.slicer" && sl->plugin.params[pp::SlMode] == static_cast<float>(pp::SlModeManual),
         "chop it: a slicer channel with manual markers");
  bool markers = sl != nullptr;
  for (int i = 0; markers && i < 8; ++i) markers = std::fabs(sl->plugin.params[static_cast<size_t>(pp::slMarker(i))] - i / 8.0) < 1e-6;
  expect(markers && sl->plugin.params[static_cast<size_t>(pp::slMarker(8))] < 0, "chop it: markers on the grid");
  bool notes = sp && sp->notes.count(r.channel) && sp->notes.at(r.channel).size() == 8;
  for (int i = 0; notes && i < 8; ++i) {
    const Note& n = sp->notes.at(r.channel)[static_cast<size_t>(i)];
    notes = n.start == i * 48 && n.length == 48 && n.pitch == 60 + i;
  }
  expect(notes && ch.clip(r.clip) && ch.clip(r.clip)->track == 1, "chop it: a pattern plays the slices in order on a free track");
  {
    const AudioBuffer out = renderSong(ch, ro);
    double e = 0;
    for (size_t i = 0; i < out.frames && i < static_cast<size_t>(1.9 * kSr); ++i) e += static_cast<double>(out.at(0, i)) * out.at(0, i);
    expect(e > 100.0, "chop it: the slices sound");
  }
}

// ======================================================================== undo / round trips

Song chopped(const std::string& tone) {
  Song s = makeEmptySong();
  s.tempo = 120;
  const ObjectId ac = addAudioClip(s, tone, 0, 768, 0);
  std::vector<Note> notes;
  for (int i = 0; i < 16; ++i) notes.push_back(Note(i * 24, 12, 60 + i % 5, 100));
  const ObjectId pc = addPatternClip(s, notes, 0, 384, 1);
  chop::sliceEqual(s, {ac}, 4);
  chop::StutterParams p = chop::presetParams(chop::Preset::BeatRepeat);
  p.reverse = 2, p.decay = 0.3, p.pitchEnd = 5;  // per-clip gain, pitch (FL @60) and reverse (FL @72 bit 0)
  chop::stutter(s, chop::clipsInRange(s, 192, 384, {0}), 192, 384, p);
  chop::stutter(s, {pc}, 96, 288, chop::presetParams(chop::Preset::TripletStutter));
  chop::sliceAtGrid(s, chop::clipsInRange(s, 0, 96, {1}), 48);
  return s;
}

void testUndoAndRoundTrip() {
  const std::string tone = writeTestWav("tone4", 4.0, [](double t) { return 0.3f * static_cast<float>(std::sin(2 * M_PI * 440 * t)); });
  // One undo step per action.
  Document d;
  {
    Song s = makeEmptySong();
    s.tempo = 120;
    addAudioClip(s, tone, 0, 384, 0);
    addPatternClip(s, {Note(0, 24, 60, 100), Note(96, 24, 62, 100)}, 0, 384, 1);
    d.reset(s);
  }
  const Song before = d.song();
  struct Act {
    const char* name;
    std::function<void(Song&)> fn;
  };
  const std::vector<Act> acts = {
      {"slice at grid", [](Song& x) { chop::sliceAtGrid(x, chop::clipsInRange(x, 0, 384), 96); }},
      {"slice equal", [](Song& x) { chop::sliceEqual(x, chop::clipsInRange(x, 0, 384), 3); }},
      {"slice transients", [](Song& x) { chop::sliceAtTransients(x, chop::clipsInRange(x, 0, 384), 0.5); }},
      {"stutter", [](Song& x) { chop::stutter(x, chop::clipsInRange(x, 0, 384), 96, 192, chop::presetParams(chop::Preset::ReverseBuild)); }},
      {"duplicate time", [](Song& x) { chop::duplicateRange(x, 0, 96); }},
      {"insert space", [](Song& x) { chop::insertSpace(x, 96, 96); }},
      {"delete space", [](Song& x) { chop::deleteSpace(x, 96, 192); }},
  };
  for (const auto& act : acts) {
    const size_t pos = d.history().position();
    d.editSong(act.name, act.fn);
    const bool one = d.history().position() == pos + 1 && !(d.song() == before);
    d.undo();
    expect(one && d.song() == before, (std::string("undo: one step for ") + act.name).c_str());
  }
  // Project round trip of a chopped arrangement.
  const Song s = chopped(tone);
  Song back;
  std::string err;
  expect(songFromJson(songToJson(s), back, &err), "project: chopped arrangement loads");
  expect(back.playlist == s.playlist && back.patterns == s.patterns, "project: every chopped clip and stutter pattern is kept");
  // FLP round trip: chopped results are ordinary clips.
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(s, bytes, rep, eo), "flp: exported");
  Song imp;
  flp::Report ir;
  expect(importFlpBytes(bytes, imp, ir, &err), "flp: re-imported");
  auto sorted = [](const Song& x, ClipKind k) {
    std::vector<Clip> v;
    for (const auto& c : x.playlist)
      if (c.kind == k) v.push_back(c);
    std::sort(v.begin(), v.end(), [](const Clip& a, const Clip& b) { return a.track != b.track ? a.track < b.track : a.start < b.start; });
    return v;
  };
  const auto ea = sorted(s, ClipKind::Audio), ia = sorted(imp, ClipKind::Audio);
  bool audioOk = ea.size() == ia.size() && !ea.empty();
  for (size_t i = 0; audioOk && i < ea.size(); ++i)
    audioOk = ea[i].start == ia[i].start && s.clipLength(ea[i]) == imp.clipLength(ia[i]) && ea[i].track == ia[i].track &&
              std::fabs(ea[i].audioStart - ia[i].audioStart) < 1e-6 && std::fabs(ea[i].gain - ia[i].gain) < 1e-6 &&
              std::fabs(ea[i].fadeIn - ia[i].fadeIn) < 1e-6 && std::fabs(ea[i].fadeOut - ia[i].fadeOut) < 1e-6 &&
              ea[i].reverse == ia[i].reverse && std::fabs(ea[i].pitch - ia[i].pitch) < 0.01f &&
              std::fabs(ea[i].stretch - ia[i].stretch) < 1e-9;
  expect(audioOk, "flp: audio slices and stutter clips keep position, length, source window, gain, fades, pitch, reverse");
  const auto ep = sorted(s, ClipKind::Pattern), ip = sorted(imp, ClipKind::Pattern);
  bool patOk = ep.size() == ip.size() && !ep.empty();
  for (size_t i = 0; patOk && i < ep.size(); ++i) {
    patOk = ep[i].start == ip[i].start && s.clipLength(ep[i]) == imp.clipLength(ip[i]) && ep[i].offset == ip[i].offset;
    const Pattern *a = s.pattern(ep[i].pattern), *b = imp.pattern(ip[i].pattern);
    size_t na = 0, nb = 0;
    if (a)
      for (const auto& [ch, ns] : a->notes) na += ns.size();
    if (b)
      for (const auto& [ch, ns] : b->notes) nb += ns.size();
    patOk = patOk && a && b && na == nb;
  }
  expect(patOk, "flp: pattern pieces and stutter patterns keep offsets and notes");
}

}  // namespace

int main() {
  gDir = test::hermeticTestEnv("chop");
  testSlicing();
  testPlan();
  testStutterAudio();
  testStutterPattern();
  testRegions();
  testUndoAndRoundTrip();
  return finish();
}
