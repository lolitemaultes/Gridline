#include "Render.h"
#include <algorithm>
#include <cmath>
#include <set>
#include "LiveRender.h"
#include "Resample.h"
#include "../core/MixerOps.h"

namespace gl {

AudioBuffer renderSong(const Song& source, const RenderOptions& opt) {
  // Live area (performance mode): launched clips play from the song start (J-32).
  Song song = source;
  const Tick start = opt.patternMode ? 0 : prepareOfflineLive(song);
  ProgramBuilder builder;
  builder.setSampleRate(opt.sampleRate);
  builder.setPdc(opt.pdc);
  auto prog = builder.build(song);
  Tick end = opt.patternMode ? (prog->pattern(opt.pattern) ? prog->pattern(opt.pattern)->length : 0) : prog->songEnd;
  if (end <= start) return AudioBuffer(static_cast<size_t>(opt.channels), 0);
  end += opt.tail;
  // Song mode follows the tempo map (tempo automation); patterns play at the song tempo.
  const TempoMap& map = opt.patternMode ? prog->flatMap : prog->tempoMap;
  const int64_t total =
      static_cast<int64_t>(std::ceil(map.samplesBetween(static_cast<double>(start), static_cast<double>(end), opt.sampleRate))) + 1;

  AudioEngine eng(opt.sampleRate);
  eng.setProgram(prog);
  eng.setLooping(false);
  eng.setOffline(true);
  eng.setFixedBlock(AudioEngine::kOfflineBlock);  // J-30: identical output for any opt.blockSize
  if (opt.patternMode) {
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(opt.pattern);
  } else {
    eng.setMode(PlayMode::Song);
    if (start > 0) eng.setPosition(start);
  }
  eng.play();

  eng.setThreads(opt.threads);

  // Plugin delay compensation: the master output lags the transport by the
  // program's output latency; render that much longer and drop the start so
  // the file begins on time (sample-aligned with a latency-free render).
  const int64_t lat = std::max(0, prog->outputLatency);
  const int64_t span = total + lat;
  AudioBuffer out(static_cast<size_t>(opt.channels), static_cast<size_t>(total));
  std::vector<float> L(static_cast<size_t>(span)), R(static_cast<size_t>(span));
  int bs = std::clamp(opt.blockSize, 1, AudioEngine::kMaxBlock);
  int64_t reported = 0;
  for (int64_t pos = 0; pos < span; pos += bs) {
    if (opt.cancel && opt.cancel->load(std::memory_order_relaxed)) break;
    int n = static_cast<int>(std::min<int64_t>(bs, span - pos));
    eng.process(L.data() + pos, R.data() + pos, n);
    if (opt.progress && pos - reported >= opt.sampleRate / 4) {  // four times per second of audio
      reported = pos;
      opt.progress(static_cast<double>(pos) / static_cast<double>(span));
    }
  }
  if (opt.progress) opt.progress(1.0);
  std::copy(L.begin() + lat, L.end(), out.plane(0));
  if (opt.channels > 1) std::copy(R.begin() + lat, R.end(), out.plane(1));
  return out;
}

// ---------------------------------------------------------------- export render

const char* renderInterpName(RenderInterp q) {
  switch (q) {
    case RenderInterp::Linear: return "Realtime (band-limited sinc)";  // J-87: was linear
    case RenderInterp::Hermite: return "Hermite (4-point)";
    case RenderInterp::Sinc16: return "Sinc 16-point";
    case RenderInterp::Sinc32: return "Sinc 32-point";
    case RenderInterp::Sinc64: return "Sinc 64-point";
    case RenderInterp::Sinc128: return "Sinc 128-point";
    case RenderInterp::Sinc256: return "Sinc 256-point";
    case RenderInterp::Sinc512: return "Sinc 512-point";
    default: return "?";
  }
}

namespace {

void neutralMaster(MixerTrack& m) {
  m.volume = 0.8f;  // unity on the fader law
  m.pan = 0;
  m.stereoSep = 0;
  m.invertPolarity = false;
  m.swapLR = false;
  m.eqGain = {0.0f, 0.0f, 0.0f};
  for (auto& s : m.slots) s.enabled = false;
}

}  // namespace

Song prepareRenderSong(const Song& song, const RenderJob& job) {
  Song s = song;
  for (size_t i = 0; i < s.mixer.size(); ++i) {
    MixerTrack& m = s.mixer[i];
    const bool master = i == 0;
    if ((master && !job.masterEffects) || (!master && !job.insertEffects))
      for (auto& sl : m.slots) sl.enabled = false;
    if (master && !job.masterEffects) m.eqGain = {0.0f, 0.0f, 0.0f};
  }
  if (!job.polyphonyLimits)
    for (auto& c : s.channels) c.polyphony = 0;
  return s;
}

bool renderScopeTicks(const Song& song, const RenderJob& job, Tick& start, Tick& end) {
  start = end = 0;
  switch (job.scope) {
    case RenderScope::Pattern:
      if (const Pattern* p = song.pattern(job.pattern)) end = song.patternContentLength(*p);
      break;
    case RenderScope::Range:
      start = std::max<Tick>(0, job.start);
      end = job.end;
      break;
    default: {
      Song s = song;
      start = prepareOfflineLive(s);
      end = s.lengthTicks();
      break;
    }
  }
  return end > start;
}

int64_t scopeFrames(const Song& song, const RenderJob& job, Tick start, Tick end, int sampleRate) {
  TempoMap map;
  if (job.scope == RenderScope::Pattern)
    map.setConstant(song.tempo, song.ppq);
  else
    map.build(song);
  return static_cast<int64_t>(std::ceil(map.samplesBetween(static_cast<double>(start), static_cast<double>(end), sampleRate) - 1e-9));
}

RenderResult renderAudio(const Song& source, const RenderJob& job) {
  RenderResult res;
  res.sampleRate = job.sampleRate;
  Song song = prepareRenderSong(source, job);
  // Live area (performance mode): launched clips play from the song start (J-32).
  const Tick liveStart = job.scope == RenderScope::Song ? prepareOfflineLive(song) : 0;
  const int os = job.oversample >= 4 ? 4 : job.oversample >= 2 ? 2 : 1;
  const int sr = job.sampleRate * os;
  if (job.sampleRate < 8000 || job.sampleRate > 384000) {
    res.error = "unsupported sample rate";
    return res;
  }
  ProgramBuilder builder;
  builder.setSampleRate(sr);
  builder.setPdc(job.pdc);
  auto prog = builder.build(song);
  Tick A = 0, B = 0;
  if (job.scope == RenderScope::Pattern) {
    const ProgPattern* pp = prog->pattern(job.pattern);
    B = pp ? pp->length : 0;
  } else if (job.scope == RenderScope::Range) {
    A = std::max<Tick>(0, job.start);
    B = job.end;
  } else {
    A = liveStart;
    B = prog->songEnd;
  }
  res.start = A;
  res.end = B;
  if (B <= A) {
    res.error = job.scope == RenderScope::Pattern ? "the pattern is empty" : job.scope == RenderScope::Range ? "the range is empty" : "the playlist is empty";
    return res;
  }
  const TempoMap& map = job.scope == RenderScope::Pattern ? prog->flatMap : prog->tempoMap;
  // The engine's own wrap point: the first sample at or after B.
  const int64_t pass = static_cast<int64_t>(std::ceil(map.samplesBetween(static_cast<double>(A), static_cast<double>(B), sr) - 1e-9));
  const int loops = std::clamp(job.loops, 1, 64);
  const int64_t body = pass * loops;
  const int64_t lat = std::max(0, prog->outputLatency);
  const bool tails = job.tail != TailMode::Cut;
  const int64_t tailCap = tails ? static_cast<int64_t>(std::max(0.0, job.maxTailSeconds) * sr) : 0;

  AudioEngine eng(sr);
  eng.setProgram(prog);
  eng.setOffline(true);
  // J-30: fixed engine blocks (identical output for any job.blockSize); the
  // stop at the end of the body lands exactly on its sample.
  eng.setFixedBlock(AudioEngine::kOfflineBlock);
  eng.addBlockBarrier(body);
  eng.setInterpolation(static_cast<int>(job.interp));
  eng.setThreads(job.threads);
  if (job.scope == RenderScope::Pattern) {
    eng.setMode(PlayMode::Pattern);
    eng.setPattern(job.pattern);
  } else {
    eng.setMode(PlayMode::Song);
    eng.setLoopRange(A, B);
    eng.setPosition(A);
  }
  eng.setLooping(true);  // passes wrap at B exactly; the transport stops after the last one
  eng.play();

  std::vector<float> L, R;
  L.reserve(static_cast<size_t>(body + lat + tailCap));
  R.reserve(static_cast<size_t>(body + lat + tailCap));
  const int bs = std::clamp(job.blockSize, 1, AudioEngine::kMaxBlock);
  const float silent = static_cast<float>(std::pow(10.0, job.silenceDb / 20.0));
  const int64_t hold = sr;  // a tail ends after one second below the silence level
  int64_t pos = 0, lastLoud = -1;
  const int64_t estimate = body + lat + (tails ? std::min<int64_t>(tailCap, 4 * sr) : 0);
  auto step = [&](int64_t n) {
    L.resize(static_cast<size_t>(pos + n));
    R.resize(static_cast<size_t>(pos + n));
    eng.process(L.data() + pos, R.data() + pos, static_cast<int>(n));
    for (int64_t i = pos; i < pos + n; ++i)
      if (std::fabs(L[static_cast<size_t>(i)]) > silent || std::fabs(R[static_cast<size_t>(i)]) > silent) lastLoud = i;
    pos += n;
    if (job.progress) job.progress(std::min(1.0, static_cast<double>(pos) / static_cast<double>(std::max<int64_t>(1, estimate))));
    return !(job.cancel && job.cancel->load());
  };
  bool ok = true;
  while (ok && pos < body) ok = step(std::min<int64_t>(bs, body - pos));
  eng.stopToStart();  // releases every note at the scope end; effects ring on
  while (ok && pos < body + lat) ok = step(std::min<int64_t>(bs, body + lat - pos));
  if (tails)
    while (ok && pos < body + lat + tailCap && (lastLoud < 0 ? false : pos - 1 - lastLoud < hold)) ok = step(bs);
  if (!ok) {
    res.cancelled = true;
    return res;
  }
  // Output frames (after dropping the PDC latency).
  int64_t frames = body;
  if (job.tail == TailMode::Leave) frames = std::max(body, std::min(pos - lat, lastLoud + 1 - lat));
  std::vector<float> oL(static_cast<size_t>(frames)), oR(static_cast<size_t>(frames));
  for (int64_t i = 0; i < frames; ++i) {
    oL[static_cast<size_t>(i)] = L[static_cast<size_t>(i + lat)];
    oR[static_cast<size_t>(i)] = R[static_cast<size_t>(i + lat)];
  }
  if (job.tail == TailMode::Wrap && body > 0) {
    const int64_t end = std::min(pos - lat, lastLoud + 1 - lat);
    for (int64_t i = body; i < end; ++i) {
      oL[static_cast<size_t>(i % body)] += L[static_cast<size_t>(i + lat)];
      oR[static_cast<size_t>(i % body)] += R[static_cast<size_t>(i + lat)];
    }
  }
  if (os > 1) {  // decimate to the output rate (the body stays sample-exact)
    const int64_t basePass = static_cast<int64_t>(std::ceil(map.samplesBetween(static_cast<double>(A), static_cast<double>(B), job.sampleRate) - 1e-9));
    const int64_t baseBody = basePass * loops;
    std::vector<float> dL = resampleChannel(oL.data(), oL.size(), sr, job.sampleRate, 48);
    std::vector<float> dR = resampleChannel(oR.data(), oR.size(), sr, job.sampleRate, 48);
    const int64_t want = job.tail == TailMode::Leave ? std::max<int64_t>(baseBody, static_cast<int64_t>(dL.size())) : baseBody;
    dL.resize(static_cast<size_t>(want), 0.0f);
    dR.resize(static_cast<size_t>(want), 0.0f);
    oL.swap(dL);
    oR.swap(dR);
    res.bodyFrames = baseBody;
  } else {
    res.bodyFrames = body;
  }
  res.audio.resize(2, oL.size());
  std::copy(oL.begin(), oL.end(), res.audio.plane(0));
  std::copy(oR.begin(), oR.end(), res.audio.plane(1));
  if (job.progress) job.progress(1.0);
  return res;
}

// ---------------------------------------------------------------- stems / tracks

std::vector<ObjectId> usedInserts(const Song& song) {
  std::set<ObjectId> fed;
  for (const auto& c : song.channels) {
    if (c.mixerTrack != kNoId) fed.insert(c.mixerTrack);
    for (ObjectId a : c.auxOuts)
      if (a != kNoId) fed.insert(a);
  }
  for (const auto& m : song.mixer) {
    for (const auto& s : m.sends)
      if (!s.sidechain) fed.insert(s.to);
    if (m.hasInput()) fed.insert(m.id);
  }
  std::vector<ObjectId> out;
  for (size_t i = 1; i < song.mixer.size(); ++i)
    if (fed.count(song.mixer[i].id)) out.push_back(song.mixer[i].id);
  return out;
}

Song stemSong(const Song& song, ObjectId track, bool masterFx) {
  // The mixer's own stem reduction (J-12, core/MixerOps): the track sent
  // straight to a neutral master, its sources kept, everything else muted.
  Song s = mixops::stemSong(song, track);
  if (s.mixer.empty()) return s;
  for (auto& m : s.mixer) m.solo = false;
  const bool full = track == s.masterId() || !s.mixerTrack(track);
  if (full) {
    if (!masterFx) neutralMaster(s.mixer[0]);
    return s;
  }
  if (masterFx) {  // stems through the master chain: the master as it is
    s.mixer[0] = song.mixer[0];
    s.mixer[0].solo = false;
    s.modLinks = song.modLinks;
  }
  return s;
}

Song playlistTrackSong(const Song& song, int track) { return playlistTracksSong(song, {track}); }

Song playlistTracksSong(const Song& song, const std::vector<int>& tracks) {
  Song s = song;
  // Other tracks' notes and audio go; automation keeps playing everywhere.
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(),
                                  [&](const Clip& c) {
                                    return c.kind != ClipKind::Automation &&
                                           std::find(tracks.begin(), tracks.end(), c.track) == tracks.end();
                                  }),
                   s.playlist.end());
  return s;
}

std::vector<TrackSet> playlistTrackGroups(const Song& song) {
  std::vector<TrackSet> out;
  const std::vector<int> used = usedPlaylistTracks(song);
  const int n = static_cast<int>(song.tracks.size());
  for (int h = 0; h < n; ++h) {
    const int size = song.tracks[static_cast<size_t>(h)].groupSize;
    if (size <= 0) continue;
    TrackSet set;
    set.name = playlistTrackName(song, static_cast<size_t>(h));
    for (int t = h; t <= std::min(n - 1, h + size); ++t) set.tracks.push_back(t);
    const bool sounds = std::any_of(set.tracks.begin(), set.tracks.end(), [&](int t) {
      return std::find(used.begin(), used.end(), t) != used.end();
    });
    if (sounds) out.push_back(std::move(set));
    h += size;  // nested groups render with their parent
  }
  return out;
}

std::vector<int> usedPlaylistTracks(const Song& song) {
  std::set<int> used;
  for (const auto& c : song.playlist) {
    if (c.kind == ClipKind::Automation || c.muted || c.start < song.live.areaEnd) continue;
    if (c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted) continue;
    used.insert(c.track);
  }
  return {used.begin(), used.end()};
}

namespace {

// Parts of a split render share the full song's scope so they line up.
RenderJob partJob(const Song& song, const RenderJob& job, size_t k, size_t count) {
  RenderJob j = job;
  if (job.scope == RenderScope::Song) {
    Tick a = 0, b = 0;
    renderScopeTicks(prepareRenderSong(song, job), job, a, b);
    j.scope = RenderScope::Range;
    j.start = a;
    j.end = b;
  }
  if (job.progress) {
    auto outer = job.progress;
    j.progress = [outer, k, count](double f) { outer((static_cast<double>(k) + f) / static_cast<double>(std::max<size_t>(1, count))); };
  }
  return j;
}

}  // namespace

int renderStems(const Song& song, const RenderJob& job, const std::vector<ObjectId>& tracks, bool masterFx, const PartSink& sink) {
  int done = 0;
  for (size_t k = 0; k < tracks.size(); ++k) {
    const int idx = song.mixerIndex(tracks[k]);
    if (idx < 0) continue;
    SplitPart part;
    part.mixer = tracks[k];
    part.index = idx;
    part.name = mixerTrackName(song, static_cast<size_t>(idx));
    RenderResult r = renderAudio(stemSong(song, tracks[k], masterFx), partJob(song, job, k, tracks.size()));
    if (r.cancelled) break;
    ++done;
    if (!sink(part, std::move(r))) break;
  }
  return done;
}

int renderPlaylistTracks(const Song& song, const RenderJob& job, const std::vector<int>& tracks, const PartSink& sink) {
  std::vector<TrackSet> sets;
  for (int t : tracks) sets.push_back({playlistTrackName(song, static_cast<size_t>(std::max(0, t))), {t}});
  return renderPlaylistTrackSets(song, job, sets, sink);
}

int renderPlaylistTrackSets(const Song& song, const RenderJob& job, const std::vector<TrackSet>& sets, const PartSink& sink) {
  int done = 0;
  for (size_t k = 0; k < sets.size(); ++k) {
    if (sets[k].tracks.empty()) continue;
    SplitPart part;
    part.track = sets[k].tracks.front();
    part.index = part.track + 1;
    part.name = sets[k].name;
    RenderResult r = renderAudio(playlistTracksSong(song, sets[k].tracks), partJob(song, job, k, sets.size()));
    if (r.cancelled) break;
    ++done;
    if (!sink(part, std::move(r))) break;
  }
  return done;
}

}  // namespace gl
