// Gridline — disk recording into the playlist (J-8). See AudioRecord.h.
#include "AudioRecord.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include "../core/Automation.h"
#include "../core/TrackModes.h"
#include "../packs/edison/edison.h"
#include "ArmedRing.h"
#include "ClipOps.h"

namespace fs = std::filesystem;

namespace gl {

using packs::edison::CaptureRing;

namespace {

const ArmedRing* ringFor(const ArmRings& rings, ObjectId mixer) {
  for (const auto& [id, r] : rings) {
    if (id == mixer) return r.get();
  }
  return nullptr;
}

// Ticks per frame around `frame` of the ring (0: unknown).
double tpfAt(const ArmedRing& ring, size_t frame) {
  double a = 0, b = 0;
  if (frame > 0 && ring.tickAt(frame - 1, a) && ring.tickAt(frame, b) && b > a) return b - a;
  if (ring.tickAt(frame, a) && ring.tickAt(frame + 1, b) && b > a) return b - a;
  return 0;
}

}  // namespace

// ------------------------------------------------------------------ AudioRecorder

void AudioRecorder::begin() {
  takes_.clear();
  open_.clear();
}

void AudioRecorder::readInto(const CaptureRing& ring, Open& o, size_t until) {
  if (bufL_.empty()) {
    bufL_.resize(16384);
    bufR_.resize(16384);
    bufP_.resize(16384);
  }
  RecordedTake& t = takes_[o.take];
  while (o.pos < until) {
    size_t want = std::min(bufL_.size(), until - o.pos);
    size_t got = ring.read(o.pos, want, bufL_.data(), bufR_.data(), bufP_.data());
    if (got == 0) break;
    t.L.insert(t.L.end(), bufL_.begin(), bufL_.begin() + static_cast<std::ptrdiff_t>(got));
    t.R.insert(t.R.end(), bufR_.begin(), bufR_.begin() + static_cast<std::ptrdiff_t>(got));
  }
}

void AudioRecorder::pump(AudioEngine& engine, const Rings& rings) {
  // Every boundary event is queued before the frames after it are written, so
  // frames written before this snapshot are classified by the events popped
  // below. Later frames wait for the next pump.
  std::map<ObjectId, std::pair<const CaptureRing*, size_t>> snap;
  for (const auto& [id, r] : rings)
    if (r) snap[id] = {r, r->written()};
  auto ringOf = [&](ObjectId m) -> const CaptureRing* {
    auto it = snap.find(m);
    return it == snap.end() ? nullptr : it->second.first;
  };
  AudioEngine::RecEvent e;
  while (engine.popRecEvent(e)) {
    const CaptureRing* ring = ringOf(e.mixer);
    auto it = std::find_if(open_.begin(), open_.end(), [&](const Open& o) { return o.mixer == e.mixer; });
    if (it != open_.end()) {  // End (or a Start without one): close the open take here
      if (ring) readInto(*ring, *it, e.ringPos);
      RecordedTake& done = takes_[it->take];
      done.done = true;
      done.armEnd = e.armPos;  // K-123 (0.7.7)
      done.punchOut = e.kind == AudioEngine::RecEvent::End && e.punch;
      open_.erase(it);
    }
    if (e.kind == AudioEngine::RecEvent::Start) {
      RecordedTake t;
      t.mixer = e.mixer;
      t.startTick = e.tick;
      t.armStart = e.armPos;  // K-123 (0.7.7)
      t.punchIn = e.punch;
      if (t.punchIn) t.punchInTick = e.tick;
      int count = 0, lastPass = -1;
      double lastStart = -1;
      for (const auto& k : takes_) {
        if (k.mixer == e.mixer) ++count, lastPass = k.pass, lastStart = k.startTick;
      }
      // K-123 (0.7.7) quick punch: a punch further on is the same pass; one at or before the last
      // one (the loop came round) is the next pass.
      t.pass = quickPunch_ && lastPass >= 0 ? (e.tick > lastStart ? lastPass : lastPass + 1) : count;
      takes_.push_back(std::move(t));
      open_.push_back({e.mixer, takes_.size() - 1, e.ringPos});
    }
  }
  for (auto& o : open_)
    if (auto it = snap.find(o.mixer); it != snap.end()) readInto(*it->second.first, o, it->second.second);
  // K-123 (0.7.7): the punch crossfades' audio, once the ring holds the frames around the punch.
  for (RecordedTake& t : takes_) {
    if (t.punchIn && !t.padInDone) {
      const ArmedRing* ring = ringFor(armRings_, t.mixer);
      if (!ring || ring->written() > t.armStart || t.done) {
        padIn(t);
        t.padInDone = true;
      }
    }
    if (t.done && t.punchOut && !t.padOutDone) t.padOutDone = padOut(t, false);
  }
}

ArmRings armRingsOf(const Program& p) {
  ArmRings out;
  for (const auto& m : p.mixer) {
    if (m.armed && m.armRing) out.push_back({m.id, m.armRing});
  }
  return out;
}

AudioRecorder::Rings ringsOf(const Program& p) {
  AudioRecorder::Rings out;
  for (const auto& m : p.mixer) {
    if (m.armed && m.rt && m.rt->rec) out.push_back({m.id, m.rt->rec.get()});
  }
  return out;
}

bool AudioRecorder::open() const {
  if (!open_.empty()) return true;
  for (const RecordedTake& t : takes_) {
    if (t.done && t.punchOut && !t.padOutDone) return true;  // K-123 (0.7.7): waiting for the audio after the punch out
  }
  return false;
}

std::vector<RecordedTake> AudioRecorder::release() {
  for (RecordedTake& t : takes_) {
    if (t.punchIn && !t.padInDone) {
      padIn(t);
      t.padInDone = true;
    }
    if (t.punchOut && !t.padOutDone) t.padOutDone = padOut(t, true);
  }
  open_.clear();
  std::vector<RecordedTake> out;
  out.swap(takes_);
  return out;
}

double AudioRecorder::seconds(double sampleRate) const {
  int64_t m = 0;
  for (const auto& t : takes_) m = std::max(m, t.frames());
  return sampleRate > 0 ? static_cast<double>(m) / sampleRate : 0.0;
}

// ------------------------------------------------------------------ commit

float takePeak(const RecordedTake& t) {
  float pk = 0;
  for (float v : t.L) pk = std::max(pk, std::fabs(v));
  for (float v : t.R) pk = std::max(pk, std::fabs(v));
  return pk;
}

int dropSilentTakes(std::vector<RecordedTake>& takes, float thresholdDb) {
  const float lin = std::pow(10.0f, thresholdDb / 20.0f);
  const size_t before = takes.size();
  takes.erase(std::remove_if(takes.begin(), takes.end(), [&](const RecordedTake& t) { return takePeak(t) < lin; }), takes.end());
  return static_cast<int>(before - takes.size());
}

std::string takeBaseName(const Song& song, ObjectId mixer, TakeCommitOptions::Naming naming) {
  const MixerTrack* mt = song.mixerTrack(mixer);
  const int mi = song.mixerIndex(mixer);
  if (naming == TakeCommitOptions::Naming::Input && mt && mt->input >= 0) {
    const std::string a = "In " + std::to_string(mt->input + 1);
    return mt->inputStereo ? a + " - In " + std::to_string(mt->input + 2) : a;
  }
  return mi >= 0 ? mixerTrackName(song, static_cast<size_t>(mi)) : std::string("Input");
}

int recordTrackFor(const Song& song, ObjectId mixer) {
  // PL-08: the audio track linked to the mixer track takes its recordings.
  if (const int t = trackmodes::recordTrack(song, mixer); t >= 0) return t;
  // Otherwise insert N → track N; instrument tracks take no audio clips, so
  // the first track below that is not one.
  int i = std::max(0, song.mixerIndex(mixer) - 1);
  while (i + 1 < kMaxPlaylistTracks && !trackmodes::clipAllowed(song, ClipKind::Audio, i)) ++i;
  return i;
}

TakePlacement takePlacement(const Song& song, const std::vector<RecordedTake>& takes, size_t i, const TakeCommitOptions& o) {
  TakePlacement r;
  if (i >= takes.size()) return r;
  const RecordedTake& t = takes[i];
  // Take group: the mixer track recorded in more than one pass (K-123 0.7.7: the punches of one
  // quick punch pass are pieces of one take, not passes).
  std::set<int> passes;
  int firstPass = t.pass, lastPass = t.pass;
  for (const auto& k : takes) {
    if (k.frames() == 0 || k.mixer != t.mixer) continue;
    passes.insert(k.pass);
    firstPass = std::min(firstPass, k.pass), lastPass = std::max(lastPass, k.pass);
  }
  r.grouped = passes.size() > 1;
  const MixerTrack* mt = song.mixerTrack(t.mixer);
  // Output taps (J-31) are always stereo; inputs follow the track's input pair.
  r.stereo = mt ? (mt->inputStereo || mt->recPoint != 0) : true;
  r.name = takeBaseName(song, t.mixer, o.naming) + (r.grouped ? " take " + std::to_string(t.pass - firstPass + 1) : std::string(" rec"));
  r.color = mt && mt->color != 0x50565E ? mt->color : 0xC8724E;
  // The track's lane (later passes stack below).
  // K-123: as lanes every pass stays on the track; otherwise later passes stack below.
  r.track = std::clamp(recordTrackFor(song, t.mixer) + (r.grouped && !o.lanes ? t.pass - firstPass : 0), 0, kMaxPlaylistTracks - 1);
  if (r.grouped) {
    r.lane = o.lanes;
    r.take = t.pass - firstPass;
    r.muted = o.muteEarlierTakes && t.pass != lastPass;
  }
  return r;
}

TakeSpan takeSpan(const TempoMap& map, double startTick, int64_t frames, double sampleRate) {
  const double sr = sampleRate > 0 ? sampleRate : 44100.0;
  TakeSpan r;
  // Clip on the tick at or before the take start; the gap becomes leading silence
  // so the audio lands on the exact sample.
  r.start = static_cast<Tick>(std::floor(startTick + 1e-9));
  const double t0Sec = map.secondsAt(static_cast<double>(r.start));
  r.pad = std::max<int64_t>(0, std::llround((map.secondsAt(startTick) - t0Sec) * sr));
  const double durSec = static_cast<double>(r.pad + frames) / sr;
  const double endTick = map.tickAtSeconds(t0Sec + durSec);
  r.length = std::max<Tick>(1, static_cast<Tick>(std::ceil(endTick - static_cast<double>(r.start) - 1e-6)));
  return r;
}

// ------------------------------------------------------------------ K-123 (0.7.7) punches

namespace {

// The crossfades of one punched take clip against the audio already on its track (see
// TakeCommitOptions::punchFade).
void punchCrossfade(Song& song, const TempoMap& map, ObjectId clipId, const RecordedTake& t, double fade, const std::vector<ObjectId>& fresh) {
  Clip* c = song.clip(clipId);
  if (!c) return;
  const Tick ts = c->start, te = c->start + std::max<Tick>(1, c->length);
  const int track = c->track;
  auto secs = [&](Tick a, Tick b) { return map.secondsAt(static_cast<double>(b)) - map.secondsAt(static_cast<double>(a)); };
  // Where the old audio stops and starts again: half the crossfade past each punch point (whole ticks).
  Tick cutA = ts, cutB = te;
  if (t.punchIn && t.punchInTick >= 0) {
    cutA = static_cast<Tick>(std::ceil(map.tickAtSeconds(map.secondsAt(t.punchInTick) + fade * 0.5) - 1e-6));
    cutA = std::clamp<Tick>(cutA, ts + 1, te);
  }
  if (t.punchOut && t.punchOutTick >= 0) {
    cutB = static_cast<Tick>(std::floor(map.tickAtSeconds(map.secondsAt(t.punchOutTick) - fade * 0.5) + 1e-6));
    cutB = std::clamp<Tick>(cutB, ts, te - 1);
  }
  const double inLen = t.punchIn ? secs(ts, cutA) : 0.0, outLen = t.punchOut ? secs(cutB, te) : 0.0;
  // The old audio on the track under the take (not takes in lanes: the comp looks after those).
  std::vector<ObjectId> old;
  for (const Clip& o : song.playlist) {
    if (o.track == track && o.kind == ClipKind::Audio && o.takeGroup == kNoId && o.id != clipId &&
        std::find(fresh.begin(), fresh.end(), o.id) == fresh.end() && o.start < te && o.start + song.clipLength(o) > ts) {
      old.push_back(o.id);
    }
  }
  if (cutB > cutA) {
    for (ObjectId id : old) {
      ObjectId mid = id;
      const Clip* o = song.clip(id);
      if (!o) continue;
      if (o->start < cutA) {
        if (o->start + song.clipLength(*o) <= cutA) {  // ends inside the punch-in crossfade
          if (Clip* left = song.clip(id); left && t.punchIn && left->start + song.clipLength(*left) > ts) {
            left->fadeOut = secs(ts, left->start + song.clipLength(*left));
            left->fadeOutManual = true;
          }
          continue;
        }
        mid = splitClipAt(song, id, cutA);
        if (Clip* left = song.clip(id); left && t.punchIn) {
          left->fadeOut = inLen;
          left->fadeOutManual = true;
          left->fadeOutShape = 0, left->fadeOutTension = 0;
        }
      }
      if (mid == kNoId) continue;
      ObjectId right = kNoId;
      if (const Clip* m = song.clip(mid); m && m->start + song.clipLength(*m) > cutB) right = m->start >= cutB ? mid : splitClipAt(song, mid, cutB);
      if (right != kNoId && t.punchOut) {
        if (Clip* rc = song.clip(right)) {
          rc->fadeIn = outLen;
          rc->fadeInManual = true;
          rc->fadeInShape = 0, rc->fadeInTension = 0;
        }
      }
      if (right != mid) {  // the part between the cuts goes (undo brings it back)
        song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& x) { return x.id == mid; }), song.playlist.end());
      }
    }
  }
  c = song.clip(clipId);
  if (!c) return;
  if (t.punchIn) {
    c->fadeIn = inLen;
    c->fadeInManual = true;
  }
  if (t.punchOut) {
    c->fadeOut = outLen;
    c->fadeOutManual = true;
  }
}

}  // namespace

void AudioRecorder::setPunchPads(ArmRings rings, double fadeSeconds, double sampleRate) {
  armRings_ = std::move(rings);
  padHalf_ = std::max(0.0, fadeSeconds) * (sampleRate > 0 ? sampleRate : 44100.0) * 0.5;
}

void AudioRecorder::padIn(RecordedTake& t) {
  const ArmedRing* ring = ringFor(armRings_, t.mixer);
  if (!ring || padHalf_ <= 0 || t.armStart == 0) return;
  size_t first = 0, end = 0;
  const double tpf = tpfAt(*ring, t.armStart - 1);
  // The transport played on into the punch: the frames before it are of the same stretch.
  if (tpf <= 0 || !ring->runOf(t.armStart - 1, first, end) || end <= t.armStart) return;  // (a loop wrap starts a new run)
  const double target = std::floor(t.startTick - padHalf_ * tpf + 1e-9);  // a whole tick
  auto want = static_cast<size_t>(std::max(0.0, std::floor((t.startTick - target) / tpf + 1e-6)));
  want = std::min(want, t.armStart - first);
  if (want == 0) return;
  std::vector<float> l(want), r(want);
  if (ring->read(t.armStart - want, want, l.data(), r.data()) != want) return;
  double at = 0;
  t.L.insert(t.L.begin(), l.begin(), l.end());
  t.R.insert(t.R.begin(), r.begin(), r.end());
  t.startTick = ring->tickAt(t.armStart - want, at) ? at : t.startTick - static_cast<double>(want) * tpf;
  t.padBefore = static_cast<int64_t>(want);
}

bool AudioRecorder::padOut(RecordedTake& t, bool last) {
  const ArmedRing* ring = ringFor(armRings_, t.mixer);
  if (!ring || t.armEnd == 0) return true;
  const size_t e0 = t.armEnd;
  const double tpf = tpfAt(*ring, e0 - 1);
  if (t.punchOutTick < 0) {
    double q = 0;
    double l = 0;
    if (ring->tickAt(e0, q)) t.punchOutTick = q;
    else if (ring->tickAt(e0 - 1, l) && tpf > 0) t.punchOutTick = l + tpf;
    else if (!last) return false;  // the frame after the take is not written yet
  }
  if (padHalf_ <= 0 || tpf <= 0 || t.punchOutTick < 0) return true;
  size_t first = 0, end = 0;
  if (!ring->runOf(e0 - 1, first, end)) return true;
  const double target = std::ceil(t.punchOutTick + padHalf_ * tpf - 1e-9);  // a whole tick
  const auto want = static_cast<size_t>(std::max(0.0, std::floor((target - t.punchOutTick) / tpf + 1e-6)));
  const bool runGoesOn = end >= ring->written();  // the stretch may still grow
  if (end < e0 + want && runGoesOn && !last) return false;  // not played that far yet
  const size_t n = std::min(want, end > e0 ? end - e0 : size_t{0});
  if (n == 0) return true;
  std::vector<float> l(n), r(n);
  if (ring->read(e0, n, l.data(), r.data()) != n) return true;
  t.L.insert(t.L.end(), l.begin(), l.end());
  t.R.insert(t.R.end(), r.begin(), r.end());
  t.padAfter = static_cast<int64_t>(n);
  return true;
}

std::vector<RecordedTake> recoverLastPass(const ArmRings& rings, double sampleRate, double minSeconds) {
  std::vector<RecordedTake> out;
  const double sr = sampleRate > 0 ? sampleRate : 44100.0;
  const auto minFrames = static_cast<size_t>(std::max(1.0, minSeconds * sr));
  for (const auto& [id, ring] : rings) {
    size_t first = 0, end = 0;
    double tick = 0;
    if (!ring || !ring->lastRun(minFrames, first, end) || !ring->tickAt(first, tick)) continue;
    RecordedTake t;
    t.mixer = id;
    t.startTick = tick;
    t.done = true;
    t.L.resize(end - first);
    t.R.resize(end - first);
    if (ring->read(first, end - first, t.L.data(), t.R.data()) != end - first) continue;
    t.armStart = first;
    t.armEnd = end;
    out.push_back(std::move(t));
  }
  return out;
}

TakeCommitResult commitTakes(Song& song, const std::vector<RecordedTake>& takes, const TakeCommitOptions& o) {
  TakeCommitResult res;
  if (takes.empty()) return res;
  std::error_code ec;
  fs::create_directories(o.dir, ec);
  TempoMap map;
  map.build(song);
  const double sr = o.sampleRate > 0 ? o.sampleRate : 44100.0;
  // Placements first (from the song as it was before any take is placed).
  std::vector<TakePlacement> place(takes.size());
  for (size_t i = 0; i < takes.size(); ++i)
    if (takes[i].frames() > 0) place[i] = takePlacement(song, takes, i, o);
  // One take group id per grouped mixer track.
  std::map<ObjectId, ObjectId> group;
  for (size_t i = 0; i < takes.size(); ++i)
    if (takes[i].frames() > 0 && place[i].grouped && !group.count(takes[i].mixer)) group[takes[i].mixer] = song.allocateId();

  for (size_t ti = 0; ti < takes.size(); ++ti) {
    const RecordedTake& t = takes[ti];
    if (t.frames() == 0) continue;
    const TakePlacement& pl = place[ti];
    const bool stereo = pl.stereo;
    const TakeSpan span = takeSpan(map, t.startTick, t.frames(), sr);
    const int64_t pad = span.pad;
    packs::edison::Audio a = packs::edison::makeAudio(sr, stereo ? 2 : 1, pad + t.frames());
    for (int64_t i = 0; i < t.frames(); ++i) {
      const size_t k = static_cast<size_t>(i);
      a.L[static_cast<size_t>(pad + i)] = t.L[k];
      a.R[static_cast<size_t>(pad + i)] = stereo ? t.R[k] : t.L[k];
    }
    const std::string& name = pl.name;
    const std::string path = packs::edison::renderPath(o.dir, cleanFileName(name), packs::edison::audioHash(a));
    std::string err;
    if (!packs::edison::writeWavFile(path, packs::edison::makeContent(a), o.bits == 24 || o.bits == 16 ? o.bits : 32, &err)) {
      res.error = err.empty() ? "could not write " + path : err;
      continue;
    }
    res.files.push_back(path);
    // Channel: plays the file through the recorded mixer track.
    Channel ch;
    ch.id = song.allocateId();
    ch.name = name;
    ch.color = pl.color;
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = path;
    ch.mixerTrack = t.mixer != kNoId && song.mixerTrack(t.mixer) ? t.mixer : song.masterId();
    song.channels.push_back(ch);
    res.channels.push_back(ch.id);
    // Clip on the track's lane (later passes stack below).
    const int track = pl.track;
    if (static_cast<int>(song.tracks.size()) <= track) song.tracks.resize(static_cast<size_t>(track) + 1);
    if (o.nameTracks && song.tracks[static_cast<size_t>(track)].name.empty())  // K-123: a lane track takes the base name ("Guitar")
      song.tracks[static_cast<size_t>(track)].name = pl.lane ? takeBaseName(song, t.mixer, o.naming) : name;
    Clip c;
    c.id = song.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch.id;
    c.track = track;
    c.start = span.start;
    c.length = span.length;
    if (auto g = group.find(t.mixer); g != group.end()) {
      c.takeGroup = g->second;
      c.take = pl.take;
      c.muted = pl.muted;
      if (o.lanes) song.tracks[static_cast<size_t>(track)].takeLanes = true;  // K-123: the new takes show
    }
    song.playlist.push_back(c);
    res.clips.push_back(c.id);
    res.takeIndex.push_back(ti);
  }
  // K-123 (0.7.7): punch crossfades, once every take of the session is placed.
  if (o.punchFade > 0) {
    for (size_t k = 0; k < res.clips.size(); ++k) {
      const RecordedTake& t = takes[res.takeIndex[k]];
      if (t.punchIn || t.punchOut) punchCrossfade(song, map, res.clips[k], t, o.punchFade, res.clips);
    }
  }
  return res;
}

std::vector<int> takesInGroup(const Song& song, ObjectId takeGroup) {
  std::set<int> s;
  for (const auto& c : song.playlist)
    if (takeGroup != kNoId && c.takeGroup == takeGroup) s.insert(c.take);
  return {s.begin(), s.end()};
}

bool compTakes(Song& song, ObjectId takeGroup, Tick a, Tick b, int take) {
  if (takeGroup == kNoId || b <= a) return false;
  // Split every clip of the group at the range edges.
  for (Tick at : {a, b}) {
    std::vector<ObjectId> crossing;
    for (const auto& c : song.playlist)
      if (c.takeGroup == takeGroup && at > c.start && at < c.start + song.clipLength(c)) crossing.push_back(c.id);
    for (ObjectId id : crossing) splitClipAt(song, id, at);
  }
  bool any = false;
  for (auto& c : song.playlist) {
    if (c.takeGroup != takeGroup) continue;
    const Tick e = c.start + song.clipLength(c);
    if (c.start >= a && e <= b) {
      c.muted = c.take != take;
      any = true;
    }
  }
  return any;
}

}  // namespace gl
