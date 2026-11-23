// Gridline: render MIDI to audio (K-254). See SourceRender.h.
#include "SourceRender.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include "../core/Automation.h"
#include "../core/I18n.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "../packs/edison/edison.h"
#include "Chop.h"
#include "ClipOps.h"
#include "ClipRender.h"
#include "Export.h"
#include "Freeze.h"
#include "LoopFile.h"
#include "Sample.h"
#include "SampledInstrument.h"
#include "Slicing.h"

namespace fs = std::filesystem;

namespace gl::srcrender {

namespace ed = packs::edison;

Through defaultThrough(Kind k) { return k == Kind::Pattern || k == Kind::Clip ? Through::Mixer : Through::ChannelFx; }

Options defaultOptions(const Source& s) {
  Options o;
  o.through = defaultThrough(s.kind);
  if (isOneShot(s)) {
    o.tail = TailMode::Leave;
    o.oneShotTrim = true;
  }
  return o;
}

namespace {

bool hasSound(const Pattern& p, ObjectId ch) {
  if (auto it = p.steps.find(ch); it != p.steps.end())
    for (uint8_t v : it->second)
      if (v > 0) return true;
  if (auto it = p.notes.find(ch); it != p.notes.end())
    for (const Note& n : it->second)
      if (!n.muted) return true;
  return false;
}

// The channels a source plays (Pattern: every channel with steps or notes in it).
std::vector<ObjectId> sourceChannels(const Song& s, const Source& src) {
  if (src.kind == Kind::Channel || src.kind == Kind::Notes) return src.channels;
  const Pattern* p = nullptr;
  if (src.kind == Kind::Pattern) p = s.pattern(src.pattern);
  if (src.kind == Kind::Clip)
    if (const Clip* c = s.clip(src.clip); c && c->kind == ClipKind::Pattern) p = s.pattern(c->pattern);
  std::vector<ObjectId> out;
  if (!p) {
    if (const Clip* c = s.clip(src.clip); c && c->kind == ClipKind::Audio) out.push_back(c->source);
    return out;
  }
  for (const auto& ch : s.channels)
    if (hasSound(*p, ch.id)) out.push_back(ch.id);
  return out;
}

// The selected notes of a Notes source (valid indices only), in time order.
std::vector<Note> selectedNotes(const Song& s, const Source& src) {
  std::vector<Note> out;
  const Pattern* p = s.pattern(src.pattern);
  if (!p || src.channels.empty()) return out;
  auto it = p->notes.find(src.channels[0]);
  if (it == p->notes.end()) return out;
  std::set<int> seen;
  for (int i : src.notes)
    if (i >= 0 && i < static_cast<int>(it->second.size()) && seen.insert(i).second) out.push_back(it->second[static_cast<size_t>(i)]);
  std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; });
  return out;
}

Tick notesSpan(const std::vector<Note>& ns) {
  if (ns.empty()) return 0;
  Tick a = ns.front().start, b = a;
  for (const Note& n : ns) b = std::max(b, n.start + std::max<Tick>(1, n.length));
  return b - a;
}

// Every channel a layer plays through comes along (one level, as a freeze).
std::set<ObjectId> withLayers(const Song& s, const std::vector<ObjectId>& chans) {
  std::set<ObjectId> keep(chans.begin(), chans.end());
  for (ObjectId id : chans)
    if (const Channel* c = s.channel(id))
      for (ObjectId k : c->layer) keep.insert(k);
  return keep;
}

// Only `keep` sounds; channels of `bypass` go straight to the master (their volume and pan stay).
void soloChannels(Song& x, const std::set<ObjectId>& keep, bool bypass, bool dry) {
  const ObjectId master = x.masterId();
  for (auto& c : x.channels) {
    if (!keep.count(c.id)) {
      c.muted = true;
      continue;
    }
    c.muted = false;
    if (bypass) {
      c.mixerTrack = master;
      c.auxOuts.clear();
    }
    if (dry) {
      c.noteFx.clear();
      c.echo = ChannelEcho{};
    }
  }
}

}  // namespace

bool renderable(const Song& s, const Source& src, std::string* why) {
  auto no = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  switch (src.kind) {
    case Kind::Channel: {
      const Pattern* p = s.pattern(src.pattern);
      if (!p) return no(i18n::N_("No pattern to render"));
      if (src.channels.empty()) return no(i18n::N_("No channel to render"));
      bool any = false;
      for (ObjectId id : src.channels) {
        if (!s.channel(id)) return no(i18n::N_("No channel to render"));
        any = any || hasSound(*p, id);
      }
      return any || no(i18n::N_("These rows have no steps or notes in this pattern"));
    }
    case Kind::Pattern: {
      const Pattern* p = s.pattern(src.pattern);
      if (!p) return no(i18n::N_("No pattern to render"));
      for (const auto& c : s.channels)
        if (hasSound(*p, c.id)) return true;
      return no(i18n::N_("The pattern is empty"));
    }
    case Kind::Clip: {
      const Clip* c = s.clip(src.clip);
      if (!c || c->kind == ClipKind::Automation) return no(i18n::N_("No clip to render"));
      if (s.clipLength(*c) <= 0) return no(i18n::N_("The clip is empty"));
      return true;
    }
    case Kind::Notes:
      if (src.channels.empty() || !s.channel(src.channels[0])) return no(i18n::N_("No channel to render"));
      if (selectedNotes(s, src).empty()) return no(i18n::N_("No notes selected"));
      return true;
    default:
      return no(i18n::N_("Nothing to render"));
  }
}

Song sourceSong(const Song& s, const Source& src, Through through) {
  const std::vector<ObjectId> chans = sourceChannels(s, src);
  const bool single = (src.kind == Kind::Channel || src.kind == Kind::Notes) && chans.size() == 1;
  Song x;
  if (src.kind == Kind::Clip) {
    x = clipAloneSong(s, src.clip);  // K-250: the clip alone over its span, the master neutral
    if (through != Through::Mixer) soloChannels(x, withLayers(x, chans), true, through == Through::Dry);
    else
      for (ObjectId id : withLayers(x, chans))
        if (Channel* c = x.channel(id)) c->muted = false;
    return x;
  }
  if (single && through != Through::Mixer) {
    x = freeze::renderSong(s, freeze::channelUnit(chans[0]));  // the channel at unity and centre, as a freeze
    if (through == Through::Dry)
      if (Channel* c = x.channel(chans[0])) {
        c->noteFx.clear();
        c->echo = ChannelEcho{};
      }
  } else {
    x = stemSong(s, s.masterId(), false);  // every mixer track as it is, the master neutral
    soloChannels(x, withLayers(x, chans), through != Through::Mixer, through == Through::Dry);
  }
  if (src.kind == Kind::Notes) {
    // The selected notes alone, from their first start, in a pattern of their own at the song start.
    const std::vector<Note> ns = selectedNotes(s, src);
    const Pattern* sp = s.pattern(src.pattern);
    Pattern p;
    p.id = x.allocateId();
    p.name = "one-shot";
    if (sp) p.meterNum = sp->meterNum, p.meterDen = sp->meterDen, p.stepsPerBar = sp->stepsPerBar;
    const Tick first = ns.empty() ? 0 : ns.front().start;
    for (Note n : ns) {
      n.start -= first;
      n.muted = false;
      p.notes[chans[0]].push_back(n);
    }
    const Tick span = notesSpan(ns);
    const Tick bar = std::max<Tick>(1, x.patternBarTicks(p));
    p.bars = static_cast<int>(std::clamp<Tick>((span + bar - 1) / bar, 1, kMaxPatternBars));
    x.patterns.push_back(p);
    x.playlist.erase(std::remove_if(x.playlist.begin(), x.playlist.end(), [](const Clip& c) { return c.kind != ClipKind::Automation; }),
                     x.playlist.end());
    Clip c;
    c.id = x.allocateId();
    c.pattern = p.id;
    c.track = 0;
    c.start = 0;
    c.length = span;
    x.playlist.push_back(c);
    if (x.tracks.empty()) x.tracks.resize(1);
    for (auto& t : x.tracks) t.muted = false;
    x.live.areaEnd = 0;
  }
  return x;
}

RenderJob sourceJob(const Song& s, const Source& src, const Options& o) {
  RenderJob j;
  j.sampleRate = o.sampleRate;
  j.loops = std::clamp(o.loops, 1, 64);
  j.tail = o.tail;
  j.maxTailSeconds = o.maxTailSeconds;
  j.blockSize = o.blockSize;
  j.progress = o.progress;
  j.cancel = o.cancel;
  switch (src.kind) {
    case Kind::Clip:
      if (const Clip* c = s.clip(src.clip)) {
        j.scope = RenderScope::Range;
        j.start = c->start;
        j.end = c->start + s.clipLength(*c);
        j.arrangementOnly = true;
      }
      break;
    case Kind::Notes:
      j.scope = RenderScope::Range;
      j.start = 0;
      j.end = notesSpan(selectedNotes(s, src));
      j.arrangementOnly = true;
      break;
    default:
      j.scope = RenderScope::Pattern;
      j.pattern = src.pattern;
      break;
  }
  return j;
}

int64_t oneShotTrim(AudioBuffer& a, double endDb) {
  const size_t n = a.frames;
  if (n == 0 || a.channels == 0) return 0;
  const float startThr = static_cast<float>(std::pow(10.0, -90.0 / 20.0));
  const float endThr = static_cast<float>(std::pow(10.0, endDb / 20.0));
  auto over = [&](size_t i, float thr) {
    for (size_t c = 0; c < a.channels; ++c)
      if (std::fabs(a.at(c, i)) > thr) return true;
    return false;
  };
  size_t first = 0;
  while (first < n && !over(first, startThr)) ++first;
  if (first >= n) {  // silence: a very short stretch of it
    a.resize(a.channels, std::min<size_t>(n, 64));
    return 0;
  }
  size_t last = n - 1;
  while (last > first && !over(last, endThr)) --last;
  const size_t end = std::min(n, last + 1);
  AudioBuffer b(a.channels, end - first);
  for (size_t c = 0; c < a.channels; ++c) std::copy(a.plane(c) + first, a.plane(c) + end, b.plane(c));
  if (end < n) {  // a short fade into the cut
    const size_t f = std::min<size_t>(b.frames / 4, 256);
    for (size_t c = 0; c < b.channels && f > 0; ++c)
      for (size_t i = 0; i < f; ++i) b.at(c, b.frames - f + i) *= static_cast<float>(f - i) / static_cast<float>(f + 1);
  }
  a = std::move(b);
  return static_cast<int64_t>(first);
}

namespace {

// Onset ticks of the source (from the span's start), for one pass.
void onsets(const Song& s, const Source& src, std::vector<Tick>& grid, std::vector<Tick>& exact) {
  grid.clear(), exact.clear();
  if (src.kind == Kind::Clip) {
    const Clip* c = s.clip(src.clip);
    if (!c) return;
    exact.push_back(0);
    for (Tick t : chop::patternOnsets(s, *c)) exact.push_back(t - c->start);
    const Pattern* p = c->kind == ClipKind::Pattern ? s.pattern(c->pattern) : nullptr;
    const Tick step = p ? std::max<Tick>(1, s.ticksPerStep(*p)) : std::max<Tick>(1, s.beatTicks() / 4);
    for (Tick t : exact) grid.push_back(t / step * step);
  } else if (src.kind == Kind::Notes) {
    const std::vector<Note> ns = selectedNotes(s, src);
    if (ns.empty()) return;
    for (const Note& n : ns) exact.push_back(n.start - ns.front().start);
    grid = exact;
  } else {
    const Pattern* p = s.pattern(src.pattern);
    if (!p) return;
    const Tick step = std::max<Tick>(1, s.ticksPerStep(*p));
    for (ObjectId ch : sourceChannels(s, src)) {
      if (auto it = p->steps.find(ch); it != p->steps.end())
        for (size_t i = 0; i < it->second.size(); ++i)
          if (it->second[i] > 0) exact.push_back(static_cast<Tick>(i) * step);
      if (auto it = p->notes.find(ch); it != p->notes.end())
        for (const Note& n : it->second)
          if (!n.muted) exact.push_back(n.start);
    }
    for (Tick t : exact) grid.push_back(t / step * step);
  }
  for (auto* v : {&grid, &exact}) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
  }
}

}  // namespace

Tick sourceSpan(const Song& s, const Source& src) {
  switch (src.kind) {
    case Kind::Clip:
      if (const Clip* c = s.clip(src.clip)) return s.clipLength(*c);
      return 0;
    case Kind::Notes:
      return notesSpan(selectedNotes(s, src));
    default:
      if (const Pattern* p = s.pattern(src.pattern)) return s.patternContentLength(*p);
      return 0;
  }
}

Tick sourceSongStart(const Song& s, const Source& src) {
  if (src.kind == Kind::Clip) {
    const Clip* c = s.clip(src.clip);
    return c ? c->start : 0;
  }
  Tick best = -1;
  for (const auto& c : s.playlist)
    if (c.kind == ClipKind::Pattern && c.pattern == src.pattern && (best < 0 || c.start < best)) best = c.start;
  if (best < 0) return 0;
  if (src.kind == Kind::Notes) {
    const std::vector<Note> ns = selectedNotes(s, src);
    if (!ns.empty()) best += ns.front().start;
  }
  return best;
}

int sourceHitCount(const Song& s, const Source& src) {
  std::vector<Tick> grid, exact;
  onsets(s, src, grid, exact);
  return static_cast<int>(grid.size());
}

Output render(const Song& s, const Source& src, const Options& o) {
  Output out;
  out.sampleRate = o.sampleRate;
  out.ppq = s.ppq;
  out.name = sourceName(s, src);
  std::string why;
  if (!renderable(s, src, &why)) {
    out.error = why;
    return out;
  }
  const std::vector<ObjectId> chans = sourceChannels(s, src);
  const Song x = sourceSong(s, src, o.through);
  const RenderJob job = sourceJob(s, src, o);
  RenderResult r = renderAudio(x, job);
  if (!r.error.empty()) {
    out.error = r.error;
    return out;
  }
  out.cancelled = r.cancelled;
  out.audio = std::move(r.audio);
  out.bodyFrames = r.bodyFrames;
  out.songTicks = src.kind == Kind::Clip;
  out.start = src.kind == Kind::Clip ? job.start : 0;
  out.oneShot = isOneShot(src) || o.oneShotTrim;
  if (src.kind == Kind::Notes) {
    const std::vector<Note> ns = selectedNotes(s, src);
    if (!ns.empty()) out.start = ns.front().start;
  }
  const Tick span = sourceSpan(s, src);
  const int loops = std::clamp(o.loops, 1, 64);
  out.length = span * loops;
  {
    TempoMap map;
    map.build(s);
    out.tempo = src.kind == Kind::Clip ? map.bpmAt(static_cast<double>(job.start)) : s.tempo;
  }
  out.keepsStrip = chans.size() == 1 && (src.kind == Kind::Channel || src.kind == Kind::Notes) && o.through != Through::Mixer;
  out.stripOf = chans.size() == 1 ? chans[0] : kNoId;
  if (src.kind == Kind::Notes) {
    const std::vector<Note> ns = selectedNotes(s, src);
    if (!ns.empty()) out.rootNote = ns.front().pitch;
  }
  // Onsets in frames: the engine's own tick to sample mapping over the job's scope.
  std::vector<Tick> grid, exact;
  onsets(s, src, grid, exact);
  const Tick base = job.scope == RenderScope::Range ? job.start : 0;
  auto frameOf = [&](Tick t) { return scopeFrames(x, job, base, base + t, o.sampleRate); };
  for (int k = 0; k < loops; ++k) {
    for (Tick t : grid) out.hits.push_back({t + span * k, frameOf(t + span * k)});
    for (Tick t : exact) out.notes.push_back({t + span * k, frameOf(t + span * k)});
  }
  const Tick beat = std::max<Tick>(1, s.beatTicks());
  for (Tick t = 0; t < out.length; t += beat) out.beats.push_back({t, frameOf(t)});
  if (o.oneShotTrim && !out.cancelled) {
    out.trimmedFrom = oneShotTrim(out.audio, o.trimEndDb);
    for (auto* v : {&out.hits, &out.notes, &out.beats}) {
      for (Hit& h : *v) h.frame = std::max<int64_t>(0, h.frame - out.trimmedFrom);
      v->erase(std::unique(v->begin(), v->end(), [](const Hit& a, const Hit& b) { return a.frame == b.frame; }), v->end());
    }
    out.bodyFrames = std::clamp<int64_t>(out.bodyFrames - out.trimmedFrom, 0, static_cast<int64_t>(out.audio.frames));
  }
  return out;
}

// ---- names ----

std::string sourceName(const Song& s, const Source& src) {
  const Pattern* p = s.pattern(src.pattern);
  const std::string pn = p ? p->name : std::string("Pattern");
  const Channel* ch = src.channels.empty() ? nullptr : s.channel(src.channels[0]);
  const std::string cn = ch ? ch->name : std::string("Channel");
  switch (src.kind) {
    case Kind::Channel:
      if (src.channels.size() > 1) return pn + " (" + std::to_string(src.channels.size()) + " rows)";
      return cn + " sequence (" + pn + ")";
    case Kind::Pattern:
      return pn;
    case Kind::Clip: {
      const Clip* c = s.clip(src.clip);
      if (!c || c->kind != ClipKind::Pattern) return clipRenderName(s, src.clip);
      const Pattern* cp = s.pattern(c->pattern);
      std::vector<const Clip*> same;
      for (const auto& k : s.playlist)
        if (k.kind == ClipKind::Pattern && k.pattern == c->pattern) same.push_back(&k);
      std::stable_sort(same.begin(), same.end(), [](const Clip* a, const Clip* b) { return a->start != b->start ? a->start < b->start : a->track < b->track; });
      const int n = static_cast<int>(std::find(same.begin(), same.end(), c) - same.begin()) + 1;
      return (cp ? cp->name : std::string("Pattern")) + " clip " + std::to_string(n);
    }
    case Kind::Notes: {
      const std::vector<Note> ns = selectedNotes(s, src);
      return cn + " one-shot " + sampled::noteLabel(ns.empty() ? 60 : ns.front().pitch);
    }
    default:
      return "Render";
  }
}

const char* destName(Dest d) {
  switch (d) {
    case Dest::AudioClip: return i18n::N_("Audio clip");
    case Dest::Replace: return i18n::N_("Replace in place");
    case Dest::Slicer: return i18n::N_("Slicer channel");
    case Dest::OneShot: return i18n::N_("One-shot sampler");
    case Dest::DrumPad: return i18n::N_("Drum pad");
    case Dest::Loupe: return i18n::N_("Loupe");
    case Dest::Library: return i18n::N_("Library");
    case Dest::File: return i18n::N_("File...");
    default: return "";
  }
}

const char* sliceAtName(SliceAt a) {
  switch (a) {
    case SliceAt::Hits: return i18n::N_("Each step hit");
    case SliceAt::Notes: return i18n::N_("Notes");
    case SliceAt::Beats: return i18n::N_("Beats");
    case SliceAt::Transients: return i18n::N_("Transients");
    default: return "";
  }
}

const char* throughName(Through t) {
  switch (t) {
    case Through::Dry: return i18n::N_("Dry");
    case Through::ChannelFx: return i18n::N_("Channel effects");
    case Through::Mixer: return i18n::N_("Mixer track");
    default: return "";
  }
}

// ---- place ----

std::vector<int64_t> slicePoints(const Output& out, SliceAt at, double sensitivity) {
  std::vector<int64_t> v{0};
  const int64_t n = static_cast<int64_t>(out.audio.frames);
  if (at == SliceAt::Transients) {
    Sample smp;
    smp.sampleRate = out.sampleRate;
    if (out.audio.channels > 0) {
      smp.left.assign(out.audio.plane(0), out.audio.plane(0) + n);
      const float* r = out.audio.plane(out.audio.channels > 1 ? 1 : 0);
      smp.right.assign(r, r + n);
    }
    for (int64_t f : computeSlices(smp, 0, std::clamp(sensitivity, 0.0, 1.0)))
      if (f > v.back() && f < n) v.push_back(f);
  } else {
    const std::vector<Hit>& h = at == SliceAt::Beats ? out.beats : at == SliceAt::Notes ? out.notes : out.hits;
    for (const Hit& x : h)
      if (x.frame > v.back() && x.frame < n) v.push_back(x.frame);
  }
  if (static_cast<int>(v.size()) > pp::kMaxSlices) v.resize(static_cast<size_t>(pp::kMaxSlices));
  return v;
}

ObjectId drumPadChannel(const Song& s, ObjectId preferred) {
  if (const Channel* c = s.channel(preferred); c && c->plugin.type == "gl.pads") return c->id;
  for (const auto& c : s.channels)
    if (c.plugin.type == "gl.pads") return c.id;
  return kNoId;
}

int freePad(const Song& s, ObjectId drumChannel) {
  const Channel* c = s.channel(drumChannel);
  if (!c || c->plugin.type != "gl.pads") return -1;
  for (int k = 0; k < pp::kPadsAll; ++k)
    if (padSamplePath(c->plugin.data, k, 0) == "none") return k;
  return -1;
}

namespace {
WavMeta renderMeta(const Output& out) {
  WavMeta meta;
  meta.loop = !out.oneShot;
  meta.tempo = out.tempo;
  meta.beats = static_cast<int>(std::lround(static_cast<double>(out.length) / std::max(1, out.ppq)));
  return meta;
}
}  // namespace

std::string writeRender(const Output& out, const std::string& dir, std::string* err) {
  if (out.audio.frames == 0) {
    if (err) *err = "nothing rendered";
    return std::string();
  }
  ed::Audio au = ed::makeAudio(out.sampleRate, 2, static_cast<int64_t>(out.audio.frames));
  if (out.audio.channels > 0) {
    std::copy(out.audio.plane(0), out.audio.plane(0) + out.audio.frames, au.L.begin());
    const float* r = out.audio.plane(out.audio.channels > 1 ? 1 : 0);
    std::copy(r, r + out.audio.frames, au.R.begin());
  }
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  const std::string path = ed::renderPath(dir, cleanFileName(out.name), ed::audioHash(au));
  if (fs::exists(fs::u8path(path), ec)) {  // the same render made before (undo, redo, again)
    loopfile::Info li;
    if (!loopfile::read(path, li) || li.loop == out.oneShot) {  // tagged the other way: the tag changes, the audio stays
      if (!writeWavFile(path, out.audio, out.sampleRate, 32, renderMeta(out), err)) return std::string();
    }
    return path;
  }
  if (!writeWavFile(path, out.audio, out.sampleRate, 32, renderMeta(out), err)) return std::string();
  return path;
}

bool tagsAsLoop(const Song& s, const Output& out) {
  if (out.audio.frames == 0 || out.sampleRate <= 0 || !(out.tempo > 0)) return false;
  const double ppq = std::max(1, out.ppq);
  const double barTicks = static_cast<double>(std::max<Tick>(1, s.barTicks()));
  const double secPerTick = 60.0 / (out.tempo * ppq);
  const double barSec = barTicks * secPerTick;
  const double len = static_cast<double>(out.audio.frames) / out.sampleRate;
  const double bars = std::round(len / barSec);
  if (bars < 1 || std::fabs(len - bars * barSec) > 0.001) return false;
  // Where the audio ends on the bars (in ticks, so a tempo change before the span does not count):
  // its start (a one-shot trim moves it) plus its length.
  const double endTick = static_cast<double>(out.start) + (static_cast<double>(out.trimmedFrom) / out.sampleRate + len) / secPerTick;
  const double off = endTick - std::round(endTick / barTicks) * barTicks;
  return std::fabs(off) * secPerTick <= 0.001;
}

bool expectsLoop(const Song& s, const Source& src, const Options& o) {
  if (isOneShot(src) || o.oneShotTrim || o.tail == TailMode::Leave) return false;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  const Tick span = sourceSpan(s, src) * std::clamp(o.loops, 1, 64);
  const Tick start = src.kind == Kind::Clip ? sourceSongStart(s, src) : 0;
  return span > 0 && span % bar == 0 && start % bar == 0;
}

namespace {

int freeTrackAt(const Song& s, int from, Tick a, Tick b) {
  for (int t = std::max(0, from); t < kMaxPlaylistTracks; ++t) {
    bool used = false;
    for (const auto& c : s.playlist) used = used || (c.track == t && c.start < b && c.start + s.clipLength(c) > a);
    if (!used) return t;
  }
  return std::max(0, from);
}

void ensureTrack(Song& s, int t) {
  if (t >= 0 && static_cast<int>(s.tracks.size()) <= t) s.tracks.resize(static_cast<size_t>(t) + 1);
}

// A new channel playing `path`, after the source's last channel in the rack (or at the end).
ObjectId addChannel(Song& s, const std::string& type, const std::string& name, const std::string& path, const Output& out,
                    const std::vector<ObjectId>& after) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.plugin = defaultPluginState(type);
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  ch.color = 0x8A9BA8;
  if (const Channel* src = after.empty() ? nullptr : s.channel(after.front())) ch.color = src->color;
  if (const Channel* src = s.channel(out.stripOf); src && out.keepsStrip) {  // pre-fader: through the source's own strip
    ch.mixerTrack = src->mixerTrack;
    ch.gain = src->gain;
    ch.pan = src->pan;
  }
  int at = -1;
  for (ObjectId id : after) at = std::max(at, s.channelIndex(id));
  at = at < 0 ? static_cast<int>(s.channels.size()) : at + 1;
  s.channels.insert(s.channels.begin() + at, ch);
  return ch.id;
}

// Ticks the rendered audio lasts from `start` (song ticks when songTicks, else at the pattern tempo).
Tick audioTicks(const Song& s, const Output& out, Tick start, int64_t frames) {
  TempoMap map;
  if (out.songTicks) map.build(s);
  else map.setConstant(out.tempo, s.ppq);
  const double t0 = static_cast<double>(out.songTicks ? start : 0);
  return std::max<Tick>(1, static_cast<Tick>(std::llround(map.tickAfterSamples(t0, static_cast<double>(frames), out.sampleRate) - t0)));
}

void muteSource(Song& s, const Source& src) {
  switch (src.kind) {
    case Kind::Channel:
      for (ObjectId id : src.channels)
        if (Channel* c = s.channel(id)) c->muted = true;
      break;
    case Kind::Notes:
      if (Pattern* p = s.pattern(src.pattern); p && !src.channels.empty())
        if (auto it = p->notes.find(src.channels[0]); it != p->notes.end())
          for (int i : src.notes)
            if (i >= 0 && i < static_cast<int>(it->second.size())) it->second[static_cast<size_t>(i)].muted = true;
      break;
    case Kind::Pattern:
      for (auto& c : s.playlist)
        if (c.kind == ClipKind::Pattern && c.pattern == src.pattern) c.muted = true;
      break;
    case Kind::Clip:
      if (Clip* c = s.clip(src.clip)) c->muted = true;
      break;
    default:
      break;
  }
}

Clip audioClipOf(Song& s, ObjectId channel, int track, Tick start, Tick length) {
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = channel;
  c.track = std::max(0, track);
  c.start = start;
  c.length = length;
  ensureTrack(s, c.track);
  return c;
}

Placed placeReplace(Song& s, const Source& src, const Output& out, const std::string& path) {
  Placed r;
  const std::vector<ObjectId> chans = sourceChannels(s, src);
  if (src.kind == Kind::Clip) {  // the clip becomes an audio clip of its sound (same track, start, length, mute)
    const Clip* k = s.clip(src.clip);
    if (!k) return r;
    const Clip old = *k;
    r.channel = addChannel(s, "gl.sampler", out.name + " (rendered)", path, out, {});
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return c.id == old.id; }), s.playlist.end());
    Clip c = audioClipOf(s, r.channel, old.track, old.start, s.clipLength(old));
    c.muted = old.muted;
    c.group = old.group;
    s.playlist.push_back(c);
    r.clip = c.id;
    r.clips = {c.id};
    return r;
  }
  if (src.kind == Kind::Pattern) {  // every clip of the pattern becomes an audio clip playing the render (looped)
    const Tick span = sourceSpan(s, src);
    std::vector<Clip> olds;
    for (const auto& c : s.playlist)
      if (c.kind == ClipKind::Pattern && c.pattern == src.pattern) olds.push_back(c);
    if (olds.empty()) {
      r.error = i18n::N_("The pattern has no clips in the playlist");
      return r;
    }
    r.channel = addChannel(s, "gl.sampler", out.name + " (rendered)", path, out, {});
    TempoMap map;
    map.setConstant(out.tempo, s.ppq);
    const double spanSec = map.secondsAt(static_cast<double>(span));
    s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return c.kind == ClipKind::Pattern && c.pattern == src.pattern; }),
                     s.playlist.end());
    for (const Clip& old : olds) {
      Clip c = audioClipOf(s, r.channel, old.track, old.start, s.clipLength(old));
      c.muted = old.muted;
      c.group = old.group;
      c.audioStart = 0;
      c.audioEnd = spanSec;  // the pattern's span (the tail is not repeated)
      c.offset = span > 0 ? old.offset % span : 0;
      c.loop = c.length > span || c.offset > 0;
      s.playlist.push_back(c);
      r.clips.push_back(c.id);
    }
    r.clip = r.clips.front();
    return r;
  }
  // Rows and notes: a sampler channel plays the render where they were; their steps and notes leave.
  Pattern* p = s.pattern(src.pattern);
  if (!p) return r;
  r.channel = addChannel(s, "gl.sampler", out.name, path, out, chans);
  Channel* nc = s.channel(r.channel);
  auto& prm = nc->plugin.params;
  if (prm.size() <= static_cast<size_t>(pp::SmpOneShot)) prm.resize(static_cast<size_t>(pp::SmpCount), 0.0f);
  prm[pp::SmpRoot] = static_cast<float>(out.rootNote);
  prm[pp::SmpOneShot] = 1.0f;
  Tick at = 0;
  if (src.kind == Kind::Notes) {
    const std::vector<Note> ns = selectedNotes(s, src);
    at = ns.empty() ? 0 : ns.front().start;
    if (auto it = p->notes.find(src.channels[0]); it != p->notes.end()) {
      std::set<int> drop(src.notes.begin(), src.notes.end());
      std::vector<Note> keep;
      for (size_t i = 0; i < it->second.size(); ++i)
        if (!drop.count(static_cast<int>(i))) keep.push_back(it->second[i]);
      it->second = std::move(keep);
      if (it->second.empty()) p->notes.erase(it);
    }
  } else {
    for (ObjectId id : src.channels) {
      p->steps.erase(id);
      p->stepProps.erase(id);
      p->notes.erase(id);
    }
  }
  p->notes[r.channel].push_back(Note(at, std::max<Tick>(1, out.length), out.rootNote, 100));
  return r;
}

Placed placeSlicer(Song& s, const Source& src, const Output& out, const std::string& path, const PlaceOptions& o) {
  Placed r;
  const std::vector<int64_t> cuts = slicePoints(out, o.sliceAt, o.sensitivity);
  const int64_t total = std::max<int64_t>(1, static_cast<int64_t>(out.audio.frames));
  const bool intoPattern = src.kind == Kind::Channel || src.kind == Kind::Notes;
  const std::vector<ObjectId> after = intoPattern ? src.channels : std::vector<ObjectId>{};
  r.channel = addChannel(s, "gl.slicer", out.name + " (chopped)", path, out, after);
  Channel* c = s.channel(r.channel);
  auto& prm = c->plugin.params;
  if (prm.size() < static_cast<size_t>(pp::SlCount)) prm.resize(static_cast<size_t>(pp::SlCount), 0.0f);
  prm[pp::SlMode] = static_cast<float>(pp::SlModeManual);
  for (int k = 0; k < pp::kMaxSlices; ++k) prm[static_cast<size_t>(pp::slMarker(k))] = -1.0f;
  const int baseNote = static_cast<int>(std::lround(prm[pp::SlBaseNote]));
  // Slice k starts at its frame; its note at the matching tick of the span.
  std::vector<Tick> ticks;
  for (int64_t f : cuts) ticks.push_back(f == 0 ? 0 : audioTicks(s, out, out.start, f + out.trimmedFrom));
  const Tick endTick = std::max<Tick>(out.length, ticks.empty() ? 1 : ticks.back() + 1);
  Pattern* p = nullptr;
  Tick base = 0;
  if (intoPattern) {
    p = s.pattern(src.pattern);
    if (src.kind == Kind::Notes) {
      const std::vector<Note> ns = selectedNotes(s, src);
      base = ns.empty() ? 0 : ns.front().start;
    }
  } else {
    Pattern np;
    np.id = s.allocateId();
    np.name = out.name + " chops";
    np.color = 0x8A9BA8;
    if (const Pattern* sp = s.pattern(src.kind == Kind::Clip && s.clip(src.clip) ? s.clip(src.clip)->pattern : src.pattern))
      np.meterNum = sp->meterNum, np.meterDen = sp->meterDen, np.stepsPerBar = sp->stepsPerBar, np.color = sp->color;
    const Tick bar = std::max<Tick>(1, s.patternBarTicks(np));
    np.bars = static_cast<int>(std::clamp<Tick>((endTick + bar - 1) / bar, 1, kMaxPatternBars));
    s.patterns.push_back(np);
    p = &s.patterns.back();
    r.pattern = np.id;
  }
  if (!p) return r;
  for (size_t k = 0; k < cuts.size(); ++k) {
    prm[static_cast<size_t>(pp::slMarker(static_cast<int>(k)))] = static_cast<float>(std::clamp(static_cast<double>(cuts[k]) / static_cast<double>(total), 0.0, 1.0));
    const Tick e = k + 1 < ticks.size() ? ticks[k + 1] : endTick;
    p->notes[r.channel].push_back(Note(base + ticks[k], std::max<Tick>(1, e - ticks[k]), std::clamp(baseNote + static_cast<int>(k), 0, 127), 100));
  }
  r.slices = static_cast<int>(cuts.size());
  if (!intoPattern) {  // the chops pattern plays where the source did, on a free track below it
    const Tick at = sourceSongStart(s, src);
    const Clip* sc = src.kind == Kind::Clip ? s.clip(src.clip) : nullptr;
    Clip pc;
    pc.id = s.allocateId();
    pc.pattern = r.pattern;
    pc.start = at;
    pc.length = endTick;
    pc.track = freeTrackAt(s, sc ? sc->track + 1 : 0, at, at + endTick);
    ensureTrack(s, pc.track);
    s.playlist.push_back(pc);
    r.clip = pc.id;
  }
  return r;
}

}  // namespace

Placed place(Song& s, const Source& src, const Output& out, const std::string& path, const PlaceOptions& o) {
  Placed r;
  if (!editsSong(o.dest)) return r;
  if (path.empty()) {
    r.error = i18n::N_("Nothing rendered");
    return r;
  }
  switch (o.dest) {
    case Dest::AudioClip: {
      const Tick at = sourceSongStart(s, src);
      const Tick len = audioTicks(s, out, at, static_cast<int64_t>(out.audio.frames));
      const Clip* sc = src.kind == Kind::Clip ? s.clip(src.clip) : nullptr;
      const int from = sc ? sc->track + 1 : 0;
      r.channel = addChannel(s, "gl.sampler", out.name + " (rendered)", path, out, {});
      Clip c = audioClipOf(s, r.channel, freeTrackAt(s, from, at, at + len), at, len);
      s.playlist.push_back(c);
      r.clip = c.id;
      break;
    }
    case Dest::Replace:
      return placeReplace(s, src, out, path);
    case Dest::Slicer:
      r = placeSlicer(s, src, out, path, o);
      break;
    case Dest::OneShot: {
      const std::vector<ObjectId> after = src.kind == Kind::Channel || src.kind == Kind::Notes ? src.channels : std::vector<ObjectId>{};
      r.channel = addChannel(s, "gl.sampler", out.name, path, out, after);
      auto& prm = s.channel(r.channel)->plugin.params;
      if (prm.size() < static_cast<size_t>(pp::SmpCount)) prm.resize(static_cast<size_t>(pp::SmpCount), 0.0f);
      prm[pp::SmpRoot] = static_cast<float>(out.rootNote);
      prm[pp::SmpOneShot] = 1.0f;
      break;
    }
    case Dest::DrumPad: {
      const ObjectId dc = drumPadChannel(s, o.drumChannel);
      const int pad = freePad(s, dc);
      if (pad < 0) {
        r.error = dc == kNoId ? i18n::N_("There is no drum pad channel") : i18n::N_("Every pad of the drum pad channel holds a sound");
        return r;
      }
      Channel* c = s.channel(dc);
      c->plugin.data = setPadSamplePath(c->plugin.data, pad, 0, path);
      c->plugin.data = padDataSet(c->plugin.data, "N" + std::to_string(pad + 1), out.name);
      r.channel = dc;
      r.pad = pad;
      break;
    }
    default:
      return r;
  }
  if (o.muteSource && r.error.empty()) muteSource(s, src);
  return r;
}

}  // namespace gl::srcrender
