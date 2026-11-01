// Gridline — disk recording into the playlist (J-8). See AudioRecord.h.
#include "AudioRecord.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include "../core/Automation.h"
#include "../core/TrackModes.h"
#include "../packs/edison/edison.h"
#include "ClipOps.h"

namespace fs = std::filesystem;

namespace gl {

using packs::edison::CaptureRing;

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
      takes_[it->take].done = true;
      open_.erase(it);
    }
    if (e.kind == AudioEngine::RecEvent::Start) {
      RecordedTake t;
      t.mixer = e.mixer;
      t.startTick = e.tick;
      for (const auto& k : takes_) t.pass += k.mixer == e.mixer;
      takes_.push_back(std::move(t));
      open_.push_back({e.mixer, takes_.size() - 1, e.ringPos});
    }
  }
  for (auto& o : open_)
    if (auto it = snap.find(o.mixer); it != snap.end()) readInto(*it->second.first, o, it->second.second);
}

AudioRecorder::Rings ringsOf(const Program& p) {
  AudioRecorder::Rings out;
  for (const auto& m : p.mixer)
    if (m.armed && m.rt && m.rt->rec) out.push_back({m.id, m.rt->rec.get()});
  return out;
}

bool AudioRecorder::open() const { return !open_.empty(); }

std::vector<RecordedTake> AudioRecorder::release() {
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
  // Take group: the mixer track recorded in more than one pass.
  int n = 0, firstPass = t.pass, lastPass = t.pass;
  for (const auto& k : takes) {
    if (k.frames() == 0 || k.mixer != t.mixer) continue;
    ++n;
    firstPass = std::min(firstPass, k.pass), lastPass = std::max(lastPass, k.pass);
  }
  r.grouped = n > 1;
  const MixerTrack* mt = song.mixerTrack(t.mixer);
  // Output taps (J-31) are always stereo; inputs follow the track's input pair.
  r.stereo = mt ? (mt->inputStereo || mt->recPoint != 0) : true;
  r.name = takeBaseName(song, t.mixer, o.naming) + (r.grouped ? " take " + std::to_string(t.pass - firstPass + 1) : std::string(" rec"));
  r.color = mt && mt->color != 0x50565E ? mt->color : 0xC8724E;
  // The track's lane (later passes stack below).
  r.track = std::clamp(recordTrackFor(song, t.mixer) + (r.grouped ? t.pass - firstPass : 0), 0, kMaxPlaylistTracks - 1);
  if (r.grouped) {
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
    if (o.nameTracks && song.tracks[static_cast<size_t>(track)].name.empty()) song.tracks[static_cast<size_t>(track)].name = name;
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
    }
    song.playlist.push_back(c);
    res.clips.push_back(c.id);
    res.takeIndex.push_back(ti);
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
