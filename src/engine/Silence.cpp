// Gridline: finding the silence in audio, and taking it out (K-253). See Silence.h.
#include "Silence.h"
#include <algorithm>
#include <cmath>
#include "../core/Automation.h"
#include "ClipOps.h"
#include "ClipSource.h"
#include <filesystem>
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "KerfMarks.h"
#include "Sample.h"

namespace gl::silence {

namespace ed = packs::edison;

namespace {

constexpr double kWindowSec = 0.010;   // level windows
constexpr double kClickSec = 0.015;    // shorter sounds are clicks
constexpr double kBreathSec = 1.0;     // a breath is shorter than this

struct Levels {
  int64_t win = 1;           // frames per window
  std::vector<double> db;    // per window
};

Levels levels(const float* L, const float* R, int64_t n, double rate) {
  Levels lv;
  lv.win = std::max<int64_t>(1, static_cast<int64_t>(std::llround(kWindowSec * rate)));
  const int64_t nw = (n + lv.win - 1) / lv.win;
  lv.db.resize(static_cast<size_t>(std::max<int64_t>(0, nw)));
  for (int64_t w = 0; w < nw; ++w) {
    const int64_t a = w * lv.win, b = std::min(n, a + lv.win);
    double sl = 0, sr = 0;
    for (int64_t i = a; i < b; ++i) {
      sl += static_cast<double>(L[i]) * L[i];
      sr += static_cast<double>(R[i]) * R[i];
    }
    const double ms = std::max(sl, sr) / static_cast<double>(std::max<int64_t>(1, b - a));
    lv.db[static_cast<size_t>(w)] = ms > 1e-20 ? 10.0 * std::log10(ms) : -200.0;
  }
  return lv;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return -200.0;
  const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

double thresholdOf(const Levels& lv, const Options& o) {
  if (!o.autoThreshold) return o.thresholdDb;
  if (lv.db.empty()) return -50.0;
  // The noise floor (the quietest tenth) and the sound (the loudest twentieth): a third of the
  // way up from the floor, at least 6 dB and at most 18 dB over it, whatever share is silence.
  const double floor = percentile(lv.db, 0.10), loud = percentile(lv.db, 0.95);
  const double over = std::clamp((loud - floor) / 3.0, 6.0, 18.0);
  return std::clamp(floor + over, -80.0, -30.0);
}

std::vector<Span> spansOf(const float* L, const float* R, int64_t n, double rate, const Options& o) {
  std::vector<Span> out;
  if (n <= 0 || rate <= 0) return out;
  const Levels lv = levels(L, R, n, rate);
  const double thr = thresholdOf(lv, o);
  const double loud = percentile(lv.db, 0.95);
  struct Part {
    int64_t w0, w1;  // windows [w0, w1)
    double peak;
  };
  std::vector<Part> parts;
  const int64_t nw = static_cast<int64_t>(lv.db.size());
  for (int64_t w = 0; w < nw;) {
    if (lv.db[static_cast<size_t>(w)] < thr) {
      ++w;
      continue;
    }
    Part p{w, w, -200.0};
    while (w < nw && lv.db[static_cast<size_t>(w)] >= thr) p.peak = std::max(p.peak, lv.db[static_cast<size_t>(w)]), ++w;
    p.w1 = w;
    parts.push_back(p);
  }
  const double win = static_cast<double>(lv.win) / rate;
  std::vector<Span> raw;
  for (size_t k = 0; k < parts.size(); ++k) {
    const Part& p = parts[k];
    const double secs = static_cast<double>(p.w1 - p.w0) * win;
    if (secs < kClickSec) continue;  // a click
    if (!o.keepBreaths && secs < kBreathSec && p.peak < loud - o.breathDb) continue;  // a breath
    raw.push_back({p.w0 * lv.win, std::min(n, p.w1 * lv.win)});
  }
  if (raw.empty()) return out;
  // Pauses shorter than the shortest gap stay; then the margins.
  const auto gap = static_cast<int64_t>(std::llround(std::max(0.0, o.minGapMs) / 1000.0 * rate));
  const auto margin = static_cast<int64_t>(std::llround(std::max(0.0, o.marginMs) / 1000.0 * rate));
  std::vector<Span> merged;
  for (const Span& s : raw) {
    if (!merged.empty() && s.a - merged.back().b < gap) merged.back().b = s.b;
    else merged.push_back(s);
  }
  for (Span s : merged) {
    s.a = std::max<int64_t>(0, s.a - margin);
    s.b = std::min(n, s.b + margin);
    if (!out.empty() && s.a <= out.back().b) out.back().b = std::max(out.back().b, s.b);
    else out.push_back(s);
  }
  return out;
}

}  // namespace

double thresholdFor(const float* L, const float* R, int64_t n, double rate, const Options& o) {
  if (n <= 0 || rate <= 0) return o.autoThreshold ? -50.0 : o.thresholdDb;
  return thresholdOf(levels(L, R, n, rate), o);
}

std::vector<Span> soundSpans(const float* L, const float* R, int64_t n, double rate, const Options& o) { return spansOf(L, R, n, rate, o); }

Span trimSpan(const float* L, const float* R, int64_t n, double rate, const Options& o) {
  const std::vector<Span> s = spansOf(L, R, n, rate, o);
  if (s.empty()) return {};
  return {s.front().a, s.back().b};
}

std::vector<Span> soundSpans(const ed::Audio& a, const Options& o) { return soundSpans(a.L.data(), a.R.data(), a.frames(), a.rate, o); }
Span trimSpan(const ed::Audio& a, const Options& o) { return trimSpan(a.L.data(), a.R.data(), a.frames(), a.rate, o); }

int64_t mapFrame(const std::vector<Span>& keep, int64_t f) {
  int64_t out = 0;
  for (const Span& s : keep) {
    if (f < s.a) return out;
    if (f < s.b) return out + (f - s.a);
    out += s.len();
  }
  return out;
}

int64_t keptFrames(const std::vector<Span>& keep) {
  int64_t n = 0;
  for (const Span& s : keep) n += s.len();
  return n;
}

// ------------------------------------------------------------------ audio

namespace {
int64_t fadeFrames(const ed::Content& c, const Options& o) {
  return std::max<int64_t>(0, static_cast<int64_t>(std::llround(std::max(0.0, o.fadeMs) / 1000.0 * c.rate())));
}
}  // namespace

ed::Content trimEnds(const ed::Content& c, const Options& o, Span* kept) {
  const ed::Audio& a = c.a();
  const Span s = trimSpan(a, o);
  if (kept) *kept = s;
  if (s.len() <= 0) return c;  // all silence: left as it is
  if (s.a == 0 && s.b == a.frames()) return c;
  ed::Content out = ed::cropTo(c, {s.a, s.b});
  const int64_t f = std::min(fadeFrames(c, o), s.len() / 2);
  if (f > 0 && s.a > 0) out = ed::fadeIn(out, {0, f});
  if (f > 0 && s.b < a.frames()) out = ed::fadeOut(out, {out.frames() - f, out.frames()});
  return out;
}

ed::Content removeSilence(const ed::Content& c, const Options& o, std::vector<Span>* keepOut) {
  const ed::Audio& a = c.a();
  const std::vector<Span> keep = soundSpans(a, o);
  if (keepOut) *keepOut = keep;
  if (keep.empty()) return c;
  if (keep.size() == 1 && keep[0].a == 0 && keep[0].b == a.frames()) return c;
  // The gaps, from the last one (earlier positions stay valid).
  std::vector<Span> gaps;
  int64_t at = 0;
  for (const Span& s : keep) {
    if (s.a > at) gaps.push_back({at, s.a});
    at = s.b;
  }
  if (at < a.frames()) gaps.push_back({at, a.frames()});
  ed::Content out = c;
  for (auto it = gaps.rbegin(); it != gaps.rend(); ++it) out = ed::deleteRange(out, {it->a, it->b});
  // Fades at every cut inside the audio: out at a part's end, in at the next one's start.
  const int64_t f0 = fadeFrames(c, o);
  int64_t pos = 0;
  for (size_t k = 0; k < keep.size(); ++k) {
    const Span& s = keep[k];
    const int64_t f = std::min(f0, s.len() / 2);
    if (f > 0 && s.a > 0) out = ed::fadeIn(out, {pos, pos + f});
    if (f > 0 && s.b < a.frames()) out = ed::fadeOut(out, {pos + s.len() - f, pos + s.len()});
    pos += s.len();
  }
  return out;
}

// ------------------------------------------------------------------ playlist clips

const char* clipProblem(const Song& song, const Clip& c) {
  if (c.kind != ClipKind::Audio) return "Only audio clips have silence to take out";
  if (c.loop) return "The clip loops: turn its loop off first";
  ClipSource src;
  double a = 0, b = 0;
  if (!clipWindow(song, c, &src, a, b) || !src.sample) return "The clip has no audio yet";
  return "";
}

namespace {

// What the clip plays, in its own direction, from its start to its end (or the end of its audio).
struct Played {
  std::vector<float> L, R;
  double rate = 0;
  double src0 = 0;  // source seconds (the clip's direction) at frame 0
};

bool played(const Song& song, const Clip& c, const TempoMap& map, Played& p) {
  ClipSource src;
  double a = 0, b = 0;
  if (!clipWindow(song, c, &src, a, b) || !src.sample) return false;
  const Channel* ch = song.channel(c.source);
  if (!ch) return false;
  const Sample& smp = *src.sample;
  const double end = std::min(b, clipSourceAt(song, c, map, static_cast<double>(c.start + song.clipLength(c))));
  const double s0 = clipSourceAt(song, c, map, static_cast<double>(c.start));
  if (end <= s0) return false;
  p.rate = smp.sampleRate;
  const auto nf = static_cast<int64_t>(smp.frames());
  const auto f0 = static_cast<int64_t>(std::floor(s0 * p.rate)), f1 = std::min(nf, static_cast<int64_t>(std::ceil(end * p.rate)));
  if (f1 <= f0) return false;
  const bool rev = clipPlaysReversed(*ch, c);
  p.L.resize(static_cast<size_t>(f1 - f0));
  p.R.resize(p.L.size());
  for (int64_t i = f0; i < f1; ++i) {
    const int64_t k = rev ? nf - 1 - i : i;  // a reversed clip's direction runs back through the file
    const bool in = k >= 0 && k < nf;
    p.L[static_cast<size_t>(i - f0)] = in ? smp.left[static_cast<size_t>(k)] : 0.0f;
    p.R[static_cast<size_t>(i - f0)] = in ? smp.right[static_cast<size_t>(k)] : 0.0f;
  }
  p.src0 = static_cast<double>(f0) / p.rate;
  return true;
}

// The song tick where the clip reaches source second `s`: the last tick at or before it (`down`)
// or the first tick at or after it.
Tick tickAt(const Song& song, const Clip& c, const TempoMap& map, double s, bool down) {
  Tick lo = c.start, hi = c.start + song.clipLength(c);
  // first tick with source >= s
  while (lo < hi) {
    const Tick mid = lo + (hi - lo) / 2;
    if (clipSourceAt(song, c, map, static_cast<double>(mid)) >= s) hi = mid;
    else lo = mid + 1;
  }
  if (down && lo > c.start && clipSourceAt(song, c, map, static_cast<double>(lo)) > s + 1e-9) --lo;
  return lo;
}

// The clip's kept parts as tick spans [t0, t1) inside it (sorted, apart).
std::vector<std::pair<Tick, Tick>> keptTicks(const Song& song, const Clip& c, const Options& o) {
  std::vector<std::pair<Tick, Tick>> out;
  TempoMap map;
  map.build(song);
  Played p;
  if (!played(song, c, map, p)) return out;
  const std::vector<Span> sp = soundSpans(p.L.data(), p.R.data(), static_cast<int64_t>(p.L.size()), p.rate, o);
  const Tick end = c.start + song.clipLength(c);
  const auto n = static_cast<int64_t>(p.L.size());
  for (const Span& s : sp) {
    const Tick t0 = s.a <= 0 ? c.start : tickAt(song, c, map, p.src0 + static_cast<double>(s.a) / p.rate, true);
    const Tick t1 = s.b >= n ? end : std::min(end, tickAt(song, c, map, p.src0 + static_cast<double>(s.b) / p.rate, false));
    if (t1 <= t0) continue;
    if (!out.empty() && t0 <= out.back().second) out.back().second = std::max(out.back().second, t1);
    else out.push_back({t0, t1});
  }
  return out;
}

// The clip `from` takes the id `to` (the part that stays keeps the clip's own id).
void takeId(Song& song, ObjectId from, ObjectId to) {
  if (from == to) return;
  song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) { return c.id == to; }), song.playlist.end());
  if (Clip* c = song.clip(from)) c->id = to;
}

void removeClip(Song& song, ObjectId id) {
  song.playlist.erase(std::remove_if(song.playlist.begin(), song.playlist.end(), [&](const Clip& c) { return c.id == id; }), song.playlist.end());
}

}  // namespace

bool trimClip(Song& song, ObjectId id, const Options& o) {
  const Clip* c = song.clip(id);
  if (!c || *clipProblem(song, *c)) return false;
  const Clip orig = *c;
  const auto kept = keptTicks(song, orig, o);
  if (kept.empty()) return false;  // all silence: left as it is
  const Tick end = orig.start + song.clipLength(orig);
  const Tick t0 = kept.front().first, t1 = kept.back().second;
  if (t0 <= orig.start && t1 >= end) return false;
  const double fade = std::max(0.0, o.fadeMs) / 1000.0;
  if (t1 < end) {
    const ObjectId r = splitClipAt(song, id, t1);
    if (r != kNoId) removeClip(song, r);
    if (Clip* k = song.clip(id)) k->fadeOut = fade, k->fadeOutManual = true;
  }
  if (t0 > orig.start) {
    const ObjectId r = splitClipAt(song, id, t0);
    if (r != kNoId) {
      removeClip(song, id);
      takeId(song, r, id);
      if (Clip* k = song.clip(id)) k->fadeIn = fade, k->fadeInManual = true;
    }
  }
  return true;
}

std::vector<ObjectId> removeClipSilence(Song& song, ObjectId id, const Options& o) {
  std::vector<ObjectId> out;
  const Clip* c = song.clip(id);
  if (!c || *clipProblem(song, *c)) return out;
  const Clip orig = *c;
  const auto kept = keptTicks(song, orig, o);
  const Tick end = orig.start + song.clipLength(orig);
  if (kept.empty() || (kept.size() == 1 && kept[0].first <= orig.start && kept[0].second >= end)) return out;
  // Cut at every edge of a kept part inside the clip; the pieces in between are silence.
  std::vector<Tick> cuts;
  for (const auto& [a, b] : kept) {
    if (a > orig.start) cuts.push_back(a);
    if (b < end) cuts.push_back(b);
  }
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  std::vector<ObjectId> pieces{id};
  ObjectId cur = id;
  for (const Tick t : cuts) {
    const ObjectId r = splitClipAt(song, cur, t);
    if (r == kNoId) continue;
    pieces.push_back(r);
    cur = r;
  }
  const double fade = std::max(0.0, o.fadeMs) / 1000.0;
  auto isKept = [&](Tick s) {
    for (const auto& [a, b] : kept)
      if (s >= a && s < b) return true;
    return false;
  };
  std::vector<ObjectId> keepIds;
  for (const ObjectId p : pieces) {
    const Clip* k = song.clip(p);
    if (!k) continue;
    if (!isKept(k->start)) {
      removeClip(song, p);
      continue;
    }
    keepIds.push_back(p);
  }
  if (keepIds.empty()) return out;
  // The first part keeps the clip's id; fades at every cut, the clip's own fades at its own ends.
  if (keepIds.front() != id) {
    takeId(song, keepIds.front(), id);
    keepIds.front() = id;
  }
  for (const ObjectId p : keepIds) {
    Clip* k = song.clip(p);
    if (!k) continue;
    const Tick ke = k->start + song.clipLength(*k);
    if (k->start > orig.start) k->fadeIn = fade, k->fadeInManual = true, k->fadeInShape = 0, k->fadeInTension = 0;
    else k->fadeIn = orig.fadeIn, k->fadeInManual = orig.fadeInManual, k->fadeInShape = orig.fadeInShape, k->fadeInTension = orig.fadeInTension;
    if (ke < end) k->fadeOut = fade, k->fadeOutManual = true, k->fadeOutShape = 0, k->fadeOutTension = 0;
    else k->fadeOut = orig.fadeOut, k->fadeOutManual = orig.fadeOutManual, k->fadeOutShape = orig.fadeOutShape, k->fadeOutTension = orig.fadeOutTension;
  }
  if (o.ripple) {  // the parts move together from the clip's start; later clips on the track follow
    Tick at = orig.start;
    for (const ObjectId p : keepIds) {
      Clip* k = song.clip(p);
      if (!k) continue;
      k->start = at;
      at += song.clipLength(*k);
    }
    const Tick removed = end - at;
    if (removed > 0)
      for (auto& x : song.playlist)
        if (x.track == orig.track && x.start >= end && std::find(keepIds.begin(), keepIds.end(), x.id) == keepIds.end()) x.start -= removed;
  }
  return keepIds;
}

// ------------------------------------------------------------------ channel samples

bool channelHasSample(const Channel& ch) {
  const PluginInfo* pi = findPlugin(ch.plugin.type);
  if (!pi || !pi->usesSample || ch.plugin.type == "gl.synth") return false;  // (a custom wave is one cycle)
  const std::string& d = ch.plugin.data;
  return !d.empty() && d[0] != '{' && (d.find('\n') == std::string::npos || kerfmarks::hasNames(d));
}

std::string applyToChannel(Song& song, ObjectId id, bool remove, const Options& o, const std::string& dir, std::string* why) {
  Channel* ch = song.channel(id);
  if (!ch || !channelHasSample(*ch)) {
    if (why) *why = "The channel plays no sample";
    return "";
  }
  const std::string path = kerfmarks::pathOf(ch->plugin.data);
  SamplePtr smp = sampleCache().get(path);
  if (!smp || smp->frames() < 2) {
    if (why) *why = "The sample cannot be read";
    return "";
  }
  const ed::Content before = ed::makeContent(ed::fromSample(*smp));
  std::vector<Span> keep;
  ed::Content after;
  if (remove) {
    after = removeSilence(before, o, &keep);
  } else {
    Span k;
    after = trimEnds(before, o, &k);
    if (k.len() > 0) keep = {k};
  }
  if (keep.empty()) {
    if (why) *why = "The sample is all silence";
    return "";
  }
  if (after.audio == before.audio) {
    if (why) *why = remove ? "No silence to remove" : "No silence at the ends";
    return "";
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  const std::string stem = std::filesystem::u8path(path).stem().u8string() + (remove ? " silence removed" : " trimmed");
  const std::string out = ed::renderPath(dir, cleanFileName(stem), ed::audioHash(after.a()));
  if (!ed::writeWavFile(out, after, 32)) {
    if (why) *why = "The new file could not be written";
    return "";
  }
  // Positions (shares of the sample) move with the audio.
  const double n0 = static_cast<double>(before.frames()), n1 = static_cast<double>(std::max<int64_t>(1, after.frames()));
  auto move = [&](float v) { return static_cast<float>(static_cast<double>(mapFrame(keep, std::llround(static_cast<double>(v) * n0))) / n1); };
  auto& P = ch->plugin.params;
  auto at = [&](int k) -> float* { return k >= 0 && static_cast<size_t>(k) < P.size() ? &P[static_cast<size_t>(k)] : nullptr; };
  if (ch->plugin.type == "gl.sampler") {
    for (int k : {static_cast<int>(pp::SmpStart), static_cast<int>(pp::SmpLoopStart)})
      if (float* v = at(k)) *v = std::clamp(move(*v), 0.0f, 1.0f);
    for (int k : {static_cast<int>(pp::SmpEnd), static_cast<int>(pp::SmpLoopEnd)})
      if (float* v = at(k); v && *v < 1.0f) *v = std::clamp(move(*v), 0.0f, 1.0f);
  } else if (ch->plugin.type == "gl.slicer") {
    for (int k = 0; k < pp::kMaxSlices; ++k) {
      if (float* v = at(pp::slMarker(k)); v && *v >= 0) *v = std::clamp(move(*v), 0.0f, 1.0f);
      if (float* v = at(pp::layerPid(1, pp::slMarker(k))); v && *v >= 0) *v = std::clamp(move(*v), 0.0f, 1.0f);
      for (int layer = 0; layer < 2; ++layer)
        if (float* v = at(pp::layerPid(layer, pp::slParam2(k, pp::SliceEnd))); v && *v >= 0) *v = std::clamp(move(*v), 0.0f, 1.0f);
    }
  }
  const size_t nl = ch->plugin.data.find('\n');
  ch->plugin.data = nl == std::string::npos ? out : out + ch->plugin.data.substr(nl);
  return out;
}

}  // namespace gl::silence
