// Gridline — audio clip workflow (J-8). See ClipOps.h.
#include "ClipOps.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include "../core/PluginParams.h"
#include "../packs/edison/edison.h"
#include "Render.h"
#include "Sample.h"

namespace fs = std::filesystem;

namespace gl {

namespace ed = packs::edison;

namespace {

double par(const Channel& ch, int k, double d) {
  return ch.plugin.type == "gl.sampler" && k < static_cast<int>(ch.plugin.params.size())
             ? static_cast<double>(ch.plugin.params[static_cast<size_t>(k)])
             : d;
}

// Playback mapping of an audio clip (same as the engine's compile step).
struct ClipMap {
  double speed = 1;       // source seconds per playback second
  double secPerTick = 0;  // > 0: tempo-synced
};

ClipMap clipMap(const Song& song, const Channel& ch, const Clip& c) {
  ClipMap m;
  const double cents = par(ch, pp::SmpPitchShift, 0) + c.pitch;
  const bool keepPitch = par(ch, pp::SmpStretchMode, 0) >= 0.5;
  const double tf = std::max(0.01, c.stretch * par(ch, pp::SmpTimeStretch, 1));
  if (c.syncBpm > 0)
    m.secPerTick = 60.0 / (c.syncBpm * song.ppq) / tf;
  else
    m.speed = (keepPitch ? 1.0 : std::pow(2.0, cents / 1200.0)) / tf;
  return m;
}

// Source window (seconds) of an audio clip.
bool sourceWindow(const Song& song, const Clip& c, SamplePtr& smp, double& a, double& b) {
  const Channel* ch = song.channel(c.source);
  if (c.kind != ClipKind::Audio || !ch) return false;
  smp = sampleCache().get(ch->plugin.data);
  if (!smp || smp->frames() < 2 || smp->sampleRate <= 0) return false;
  a = std::max(0.0, c.audioStart);
  b = c.audioEnd > a ? std::min(c.audioEnd, smp->seconds()) : smp->seconds();
  return b > a;
}

ObjectId addSamplerChannel(Song& song, const std::string& name, const std::string& path, ObjectId mixer, Rgb color) {
  Channel ch;
  ch.id = song.allocateId();
  ch.name = name;
  ch.color = color;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = mixer != kNoId && song.mixerTrack(mixer) ? mixer : song.masterId();
  song.channels.push_back(ch);
  return ch.id;
}

bool writeAudio(const std::string& dir, const std::string& name, const ed::Audio& a, std::string& path) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  path = ed::renderPath(dir, cleanFileName(name), ed::audioHash(a));
  return ed::writeWavFile(path, ed::makeContent(a), 32);
}

}  // namespace

std::string cleanFileName(const std::string& s) {
  std::string o;
  for (char ch : s) {
    const unsigned char u = static_cast<unsigned char>(ch);
    o += (std::isalnum(u) || ch == ' ' || ch == '-' || ch == '_' || u >= 0x80) ? ch : '_';
  }
  while (!o.empty() && o.back() == ' ') o.pop_back();
  return o.empty() ? std::string("audio") : o;
}

double clipSourceAt(const Song& song, const Clip& c, const TempoMap& map, double tick) {
  const Channel* ch = song.channel(c.source);
  if (!ch) return c.audioStart;
  const ClipMap m = clipMap(song, *ch, c);
  if (m.secPerTick > 0) return c.audioStart + (tick - static_cast<double>(c.start)) * m.secPerTick;
  return c.audioStart + (map.secondsAt(tick) - map.secondsAt(static_cast<double>(c.start))) * m.speed;
}

Tick audioClipNaturalLength(const Song& song, const Clip& c) {
  SamplePtr smp;
  double a = 0, b = 0;
  if (!sourceWindow(song, c, smp, a, b)) return 0;
  const ClipMap m = clipMap(song, *song.channel(c.source), c);
  if (m.secPerTick > 0) return std::max<Tick>(1, static_cast<Tick>(std::llround((b - a) / m.secPerTick)));
  TempoMap map;
  map.build(song);
  const double s0 = map.secondsAt(static_cast<double>(c.start));
  const double end = map.tickAtSeconds(s0 + (b - a) / std::max(1e-6, m.speed));
  return std::max<Tick>(1, static_cast<Tick>(std::llround(end - static_cast<double>(c.start))));
}

ObjectId splitClipAt(Song& song, ObjectId id, Tick at) {
  Clip* c = song.clip(id);
  if (!c) return kNoId;
  const Tick len = song.clipLength(*c);
  if (at <= c->start || at >= c->start + len) return kNoId;
  Clip right = *c;
  right.id = song.allocateId();
  right.start = at;
  right.length = c->start + len - at;
  if (c->kind == ClipKind::Audio) {
    TempoMap map;
    map.build(song);
    right.audioStart = std::max(0.0, clipSourceAt(song, *c, map, static_cast<double>(at)));
    right.fadeIn = 0;
  } else {
    right.offset = c->offset + (at - c->start);
  }
  c = song.clip(id);  // (allocateId does not move clips, but keep it obviously valid)
  c->length = at - c->start;
  if (c->kind == ClipKind::Audio) c->fadeOut = 0;
  song.playlist.push_back(right);
  return right.id;
}

std::vector<ObjectId> sliceClipAtGrid(Song& song, ObjectId id, Tick grid) {
  const Clip* c = song.clip(id);
  if (!c || grid <= 0) return {};
  std::vector<ObjectId> out{id};
  const Tick end = c->start + song.clipLength(*c);
  ObjectId cur = id;
  for (Tick t = (c->start / grid + 1) * grid; t < end; t += grid) {
    ObjectId r = splitClipAt(song, cur, t);
    if (r == kNoId) break;
    out.push_back(r);
    cur = r;
  }
  return out;
}

std::vector<ObjectId> sliceClipAtTransients(Song& song, ObjectId id, double sensitivity) {
  const Clip* c = song.clip(id);
  SamplePtr smp;
  double a = 0, b = 0;
  if (!c || !sourceWindow(song, *c, smp, a, b)) return {};
  ed::Audio au = ed::fromSample(*smp);
  const auto r0 = static_cast<int64_t>(std::llround(a * smp->sampleRate));
  const auto r1 = std::min<int64_t>(au.frames(), static_cast<int64_t>(std::llround(b * smp->sampleRate)));
  std::vector<int64_t> on = ed::detectOnsets(au, {r0, r1}, sensitivity);
  // Onsets → song ticks (the clip's mapping is monotonic: invert by bisection).
  TempoMap map;
  map.build(song);
  const Clip base = *c;
  const Tick len = song.clipLength(base);
  const bool rev = base.reverse != (par(*song.channel(base.source), pp::SmpReverse, 0) >= 0.5);
  std::vector<Tick> cuts;
  for (int64_t f : on) {
    double src = static_cast<double>(f) / smp->sampleRate;
    if (rev) src = smp->seconds() - src;  // reversed clips read from the end
    double lo = static_cast<double>(base.start), hi = static_cast<double>(base.start + len);
    for (int k = 0; k < 48; ++k) {
      double mid = 0.5 * (lo + hi);
      (clipSourceAt(song, base, map, mid) < src ? lo : hi) = mid;
    }
    Tick t = static_cast<Tick>(std::llround(hi));
    if (t > base.start && t < base.start + len) cuts.push_back(t);
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  std::vector<ObjectId> out{id};
  ObjectId cur = id;
  for (Tick t : cuts) {
    ObjectId r = splitClipAt(song, cur, t);
    if (r == kNoId) continue;
    out.push_back(r);
    cur = r;
  }
  return out;
}

int fitClipToTempo(Song& song, ObjectId id) {
  Clip* c = song.clip(id);
  SamplePtr smp;
  double a = 0, b = 0;
  if (!c || !sourceWindow(song, *c, smp, a, b)) return 0;
  TempoMap map;
  map.build(song);
  const double bpm = map.bpmAt(static_cast<double>(c->start));
  const double secs = b - a;
  // Beats in the audio at the song tempo (quarter-note beats of the tempo).
  const int beats = std::max(1, static_cast<int>(std::llround(secs * bpm / 60.0)));
  const double tfCh = std::max(0.01, par(*song.channel(c->source), pp::SmpTimeStretch, 1));
  c->stretch = 1.0;
  c->syncBpm = beats * 60.0 / secs;
  c->length = std::max<Tick>(1, static_cast<Tick>(std::llround(beats * song.ppq * tfCh)));
  return beats;
}

float normalizeClip(Song& song, ObjectId id, double targetDb) {
  Clip* c = song.clip(id);
  SamplePtr smp;
  double a = 0, b = 0;
  if (!c || !sourceWindow(song, *c, smp, a, b)) return 0.f;
  const size_t i0 = static_cast<size_t>(std::llround(a * smp->sampleRate));
  const size_t i1 = std::min(smp->frames(), static_cast<size_t>(std::llround(b * smp->sampleRate)));
  float peak = 0;
  for (size_t i = i0; i < i1; ++i) peak = std::max({peak, std::fabs(smp->left[i]), std::fabs(smp->right[i])});
  if (peak <= 1e-9f) return 0.f;
  c->gain = static_cast<float>(std::pow(10.0, targetDb / 20.0) / peak);
  return c->gain;
}

bool setClipStretch(Song& song, ObjectId id, double timeFactor, bool keepPitch) {
  Clip* c = song.clip(id);
  if (!c || c->kind != ClipKind::Audio || !(timeFactor > 0)) return false;
  Channel* ch = song.channel(c->source);
  if (!ch) return false;
  if (ch->plugin.type == "gl.sampler" && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode))
    ch->plugin.params[pp::SmpStretchMode] = keepPitch ? 1.0f : 0.0f;
  c = song.clip(id);
  c->stretch = std::clamp(timeFactor, 0.0625, 16.0);
  if (Tick n = audioClipNaturalLength(song, *c); n > 0) song.clip(id)->length = n;
  return true;
}

std::string renderClipStretch(Song& song, ObjectId id, double timeFactor, double semitones, int mode, const std::string& dir) {
  Clip* c = song.clip(id);
  SamplePtr smp;
  double a = 0, b = 0;
  if (!c || !sourceWindow(song, *c, smp, a, b) || !(timeFactor > 0)) return "";
  ed::Audio src = ed::fromSample(*smp);
  const auto f0 = static_cast<int64_t>(std::llround(a * smp->sampleRate));
  const auto f1 = std::min<int64_t>(src.frames(), static_cast<int64_t>(std::llround(b * smp->sampleRate)));
  ed::Audio win = ed::makeAudio(src.rate, src.channels, f1 - f0);
  std::copy(src.L.begin() + f0, src.L.begin() + f1, win.L.begin());
  std::copy(src.R.begin() + f0, src.R.begin() + f1, win.R.begin());
  const auto sm = static_cast<ed::Stretch>(std::clamp(mode, 0, static_cast<int>(ed::Stretch::Count) - 1));
  ed::Audio out = ed::stretchAudio(win, timeFactor, semitones, sm, 1);
  if (out.frames() == 0) return "";
  const Channel old = *song.channel(c->source);
  std::string path;
  if (!writeAudio(dir, old.name + " stretched", out, path)) return "";
  ObjectId ch = addSamplerChannel(song, old.name + " (stretched)", path, old.mixerTrack, old.color);
  c = song.clip(id);
  c->source = ch;
  c->audioStart = 0;
  c->audioEnd = 0;
  c->stretch = 1.0;
  c->syncBpm = 0;
  c->pitch = 0;
  if (Tick n = audioClipNaturalLength(song, *c); n > 0) song.clip(id)->length = n;
  return path;
}

namespace {
// Renders `keep` clips over [a, b) with a neutral master. Empty on failure.
bool renderSpan(const Song& song, const std::set<ObjectId>& keep, Tick a, Tick b, double sr, ed::Audio& out) {
  Song r = song;
  std::vector<Clip> kept;
  for (const auto& c : r.playlist)
    if (keep.count(c.id) && !c.muted) {
      Clip k = c;
      k.start -= a;
      kept.push_back(k);
    }
  if (kept.empty()) return false;
  r.playlist = std::move(kept);
  for (auto& t : r.tracks) t.muted = false;
  r.live.areaEnd = 0;
  if (!r.mixer.empty()) {  // master: unity, no effects
    MixerTrack& m = r.mixer[0];
    m.volume = 0.8f;
    m.pan = 0;
    m.stereoSep = 0;
    m.muted = false;
    m.eqGain = {0.0f, 0.0f, 0.0f};
    for (auto& s : m.slots) s.enabled = false;
  }
  RenderOptions opt;
  opt.sampleRate = static_cast<int>(sr);
  AudioBuffer buf = renderSong(r, opt);
  TempoMap map;
  map.build(r);
  const auto n = static_cast<int64_t>(std::llround(map.secondsAt(static_cast<double>(b - a)) * sr));
  if (n <= 0) return false;
  out = ed::makeAudio(sr, 2, n);
  for (int64_t i = 0; i < n && static_cast<size_t>(i) < buf.frames; ++i) {
    out.L[static_cast<size_t>(i)] = buf.at(0, static_cast<size_t>(i));
    out.R[static_cast<size_t>(i)] = buf.channels > 1 ? buf.at(1, static_cast<size_t>(i)) : buf.at(0, static_cast<size_t>(i));
  }
  return true;
}
}  // namespace

ObjectId consolidateClips(Song& song, const std::set<ObjectId>& clips, const std::string& dir, double sr, bool replace) {
  Tick a = 0, b = 0;
  int track = -1;
  std::string name;
  bool first = true;
  for (const auto& c : song.playlist) {
    if (!clips.count(c.id) || c.muted || c.kind == ClipKind::Automation) continue;
    const Tick e = c.start + song.clipLength(c);
    if (first || c.start < a) {
      a = c.start;
      track = c.track;
      name = c.kind == ClipKind::Audio && song.channel(c.source) ? song.channel(c.source)->name
             : song.pattern(c.pattern)                           ? song.pattern(c.pattern)->name
                                                                 : std::string("Clip");
    }
    b = first ? e : std::max(b, e);
    first = false;
  }
  if (first || b <= a) return kNoId;
  ed::Audio au;
  if (!renderSpan(song, clips, a, b, sr, au)) return kNoId;
  std::string path;
  if (!writeAudio(dir, name + " consolidated", au, path)) return kNoId;
  ObjectId ch = addSamplerChannel(song, name + " (rendered)", path, kNoId, 0x8A9BA8);
  if (replace) song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) {
                                     return clips.count(c.id) && c.kind != ClipKind::Automation;
                                   }),
                                   song.playlist.end());
  Clip c;
  c.id = song.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch;
  c.track = std::max(0, track);
  c.start = a;
  c.length = b - a;
  song.playlist.push_back(c);
  return c.id;
}

ObjectId bounceTrack(Song& song, int track, const std::string& dir, double sr) {
  std::set<ObjectId> ids;
  for (const auto& c : song.playlist)
    if (c.track == track && !c.muted && c.kind != ClipKind::Automation) ids.insert(c.id);
  if (ids.empty()) return kNoId;
  ObjectId id = consolidateClips(song, ids, dir, sr, false);
  if (id == kNoId) return kNoId;
  // First empty track below the source.
  int dest = -1;
  for (int t = track + 1; t < kMaxPlaylistTracks && dest < 0; ++t) {
    bool used = false;
    for (const auto& c : song.playlist) used = used || (c.track == t && c.id != id);
    if (!used) dest = t;
  }
  if (dest < 0) dest = track;
  if (static_cast<int>(song.tracks.size()) <= dest) song.tracks.resize(static_cast<size_t>(dest) + 1);
  for (auto& c : song.playlist) {
    if (ids.count(c.id)) c.muted = true;
    if (c.id == id) c.track = dest;
  }
  if (song.tracks[static_cast<size_t>(track)].name.empty() == false)
    song.tracks[static_cast<size_t>(dest)].name = song.tracks[static_cast<size_t>(track)].name + " (bounced)";
  return id;
}

}  // namespace gl
