// Gridline: playlist clips rendered to audio one at a time (K-250, K-251). See ClipRender.h.
#include "ClipRender.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include "../core/PluginInfo.h"
#include "../core/TrackReceipt.h"
#include "../packs/edison/edison.h"
#include "ClipOps.h"
#include "Export.h"
#include "Sample.h"

namespace fs = std::filesystem;

namespace gl {

namespace ed = packs::edison;

Song clipAloneSong(const Song& song, ObjectId clip) {
  Song s = stemSong(song, song.masterId(), false);  // every mixer track as it is, the master neutral
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& x) { return x.id != clip && x.kind != ClipKind::Automation; }),
                   s.playlist.end());
  for (auto& t : s.tracks) t.muted = false;
  for (auto& x : s.playlist)
    if (x.id == clip) x.muted = false;
  s.live.areaEnd = 0;  // the arrangement only
  return s;
}

RenderResult renderClipAlone(const Song& song, ObjectId clip, int sampleRate, std::function<void(double)> progress,
                             const std::atomic<bool>* cancel) {
  RenderResult none;
  const Clip* k = song.clip(clip);
  if (!k || k->kind == ClipKind::Automation) {
    none.error = "no clip to render";
    return none;
  }
  const Tick len = song.clipLength(*k);
  if (len <= 0) {
    none.error = "the clip is empty";
    return none;
  }
  RenderJob j;
  j.sampleRate = sampleRate;
  j.scope = RenderScope::Range;
  j.start = k->start;
  j.end = k->start + len;
  j.tail = TailMode::Cut;
  j.arrangementOnly = true;
  j.progress = std::move(progress);
  j.cancel = cancel;
  return renderAudio(clipAloneSong(song, clip), j);
}

std::vector<ObjectId> renderableClips(const Song& song, const std::set<ObjectId>& clips) {
  std::vector<const Clip*> v;
  for (const auto& c : song.playlist)
    if (clips.count(c.id) && c.kind != ClipKind::Automation) v.push_back(&c);
  std::stable_sort(v.begin(), v.end(), [](const Clip* a, const Clip* b) { return a->start != b->start ? a->start < b->start : a->track < b->track; });
  std::vector<ObjectId> out;
  out.reserve(v.size());
  for (const Clip* c : v) out.push_back(c->id);
  return out;
}

std::string clipRenderName(const Song& song, ObjectId clip) {
  const Clip* c = song.clip(clip);
  if (!c) return "Clip";
  if (c->kind == ClipKind::Audio)
    if (const Channel* ch = song.channel(c->source); ch && !ch->name.empty()) return ch->name;
  if (c->kind == ClipKind::Pattern)
    if (const Pattern* p = song.pattern(c->pattern); p && !p->name.empty()) return p->name;
  return "Clip";
}

std::vector<std::string> batchFileNames(const Song& song, const std::vector<ObjectId>& clips, const std::set<std::string>& taken) {
  std::set<std::string> used = taken;
  std::vector<std::string> out;
  out.reserve(clips.size());
  for (ObjectId id : clips) {
    const std::string base = cleanFileName(clipRenderName(song, id));
    std::string n = base;
    for (int k = 2; used.count(n); ++k) n = base + " " + std::to_string(k);
    used.insert(n);
    out.push_back(n);
  }
  return out;
}

bool clipRenderOrigin(const Song& song, ObjectId clip, int64_t frames, int sampleRate, receipt::Origin& og) {
  const Clip* k = song.clip(clip);
  if (!k) return false;
  og = receipt::Origin{};
  if (k->kind == ClipKind::Audio) {  // J-219: the clip's part of its file
    const Channel* ch = song.channel(k->source);
    if (!ch || frames <= 0) return false;
    og.source = ch->plugin.data.substr(0, ch->plugin.data.find('\n'));
    if (og.source.empty()) return false;
    og.start = std::max(0.0, k->audioStart);
    og.end = k->audioEnd > og.start ? k->audioEnd : -1;
    if (og.end < 0)
      if (SamplePtr sp = sampleCache().get(og.source)) og.end = sp->seconds();
    if (og.end > 0) og.speed = (og.end - og.start) / (static_cast<double>(frames) / sampleRate);
    else og.rangeKnown = false;
    og.reverse = k->reverse;
    og.kind = "clip";
    return true;
  }
  // A pattern clip: the files its instruments read (which parts is not known).
  og.rangeKnown = false;
  og.kind = "render";
  std::set<std::string> seen;
  std::vector<ObjectId> chans;
  if (const Pattern* p = song.pattern(k->pattern)) {
    for (const auto& [id, st] : p->steps) chans.push_back(id);
    for (const auto& [id, n] : p->notes) chans.push_back(id);
  }
  for (ObjectId id : chans)
    if (const Channel* ch = song.channel(id); ch && !ch->plugin.data.empty() && ch->plugin.data.front() == '/' && seen.insert(ch->plugin.data).second)
      og.sources.push_back(ch->plugin.data);
  if (og.sources.empty()) return false;
  og.source = og.sources.front();
  return true;
}

namespace {

ed::Audio toAudio(const AudioBuffer& b, int rate) {
  ed::Audio a = ed::makeAudio(rate, 2, static_cast<int64_t>(b.frames));
  if (b.channels == 0) return a;
  std::copy(b.plane(0), b.plane(0) + b.frames, a.L.begin());
  const float* r = b.plane(b.channels > 1 ? 1 : 0);
  std::copy(r, r + b.frames, a.R.begin());
  return a;
}

// One clip's render written into `dir` (32-bit float, named after the clip and its audio).
std::string writeRender(const Song& song, ObjectId clip, const std::string& dir, int rate) {
  RenderResult r = renderClipAlone(song, clip, rate);
  if (!r.error.empty() || r.cancelled || r.audio.frames == 0) return std::string();
  const ed::Audio au = toAudio(r.audio, rate);
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  const std::string path = ed::renderPath(dir, cleanFileName(clipRenderName(song, clip) + " rendered"), ed::audioHash(au));
  if (!ed::writeWavFile(path, ed::makeContent(au), 32)) return std::string();
  receipt::Origin og;
  if (clipRenderOrigin(song, clip, static_cast<int64_t>(r.audio.frames), rate, og)) receipt::writeOrigin(path, og);
  return path;
}

}  // namespace

ObjectId renderClipInPlace(Song& song, ObjectId clip, const std::string& dir, int sampleRate) {
  const Clip* k = song.clip(clip);
  if (!k || k->kind == ClipKind::Automation) return kNoId;
  const Clip old = *k;
  const Tick len = song.clipLength(old);
  const std::string name = clipRenderName(song, clip);
  const std::string path = writeRender(song, clip, dir, sampleRate);
  if (path.empty()) return kNoId;
  Channel ch;
  ch.id = song.allocateId();
  ch.name = name + " (rendered)";
  ch.color = 0x8A9BA8;
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = path;
  ch.mixerTrack = song.masterId();  // its mixer track's effects are in the render
  song.channels.push_back(ch);
  song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) { return c.id == clip; }), song.playlist.end());
  Clip c;
  c.id = song.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.track = std::max(0, old.track);
  c.start = old.start;
  c.length = len;
  c.muted = old.muted;
  c.group = old.group;
  song.playlist.push_back(c);
  return c.id;
}

std::vector<ObjectId> renderClipsInPlace(Song& song, const std::set<ObjectId>& clips, const std::string& dir, int sampleRate) {
  std::vector<ObjectId> out;
  // Every render reads the song as it was (a clip rendered first never sounds in the next one's render).
  const Song before = song;
  for (ObjectId id : renderableClips(before, clips)) {
    const std::string path = writeRender(before, id, dir, sampleRate);
    if (path.empty()) continue;
    const Clip* k = song.clip(id);
    if (!k) continue;
    const Clip old = *k;
    Channel ch;
    ch.id = song.allocateId();
    ch.name = clipRenderName(before, id) + " (rendered)";
    ch.color = 0x8A9BA8;
    ch.plugin = defaultPluginState("gl.sampler");
    ch.plugin.data = path;
    ch.mixerTrack = song.masterId();
    song.channels.push_back(ch);
    song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) { return c.id == id; }), song.playlist.end());
    Clip c;
    c.id = song.allocateId();
    c.kind = ClipKind::Audio;
    c.source = ch.id;
    c.track = std::max(0, old.track);
    c.start = old.start;
    c.length = before.clipLength(old);
    c.muted = old.muted;
    c.group = old.group;
    song.playlist.push_back(c);
    out.push_back(c.id);
  }
  return out;
}

std::vector<ClipFile> renderClipsToFiles(const Song& song, const std::set<ObjectId>& clips, const std::string& dir, int sampleRate, int bits,
                                         std::string* err, std::function<void(double)> progress, const std::atomic<bool>* cancel) {
  std::vector<ClipFile> out;
  const std::vector<ObjectId> ids = renderableClips(song, clips);
  if (ids.empty()) {
    if (err) *err = "no clips to render";
    return out;
  }
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  std::set<std::string> taken;  // the folder's own WAV files are never overwritten
  for (const auto& e : fs::directory_iterator(fs::u8path(dir), ec))
    if (e.path().extension() == ".wav") taken.insert(e.path().stem().u8string());
  const std::vector<std::string> names = batchFileNames(song, ids, taken);
  const double n = static_cast<double>(ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    if (cancel && cancel->load()) break;
    const double base = static_cast<double>(i) / n;
    RenderResult r = renderClipAlone(song, ids[i], sampleRate, progress ? [&progress, base, n](double p) { progress(base + p / n); } : std::function<void(double)>{}, cancel);
    if (r.cancelled || (cancel && cancel->load())) break;
    if (!r.error.empty() || r.audio.frames == 0) {
      if (err && err->empty()) *err = r.error.empty() ? std::string("nothing to render") : r.error;
      continue;
    }
    const std::string path = (fs::u8path(dir) / fs::u8path(names[i] + ".wav")).u8string();
    AudioFileOptions o;
    o.bits = bits;
    o.cancel = cancel;
    std::string e;
    if (!writeAudioFile(path, r.audio, sampleRate, o, &e)) {
      if (err && err->empty()) *err = e;
      continue;
    }
    receipt::Origin og;
    if (clipRenderOrigin(song, ids[i], static_cast<int64_t>(r.audio.frames), sampleRate, og)) receipt::writeOrigin(path, og);
    out.push_back({ids[i], path});
  }
  if (progress) progress(1.0);
  return out;
}

}  // namespace gl
