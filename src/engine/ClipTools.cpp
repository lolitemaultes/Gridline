// Gridline: Loupe's audio tools on playlist audio clips. See ClipTools.h.
#include "ClipTools.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "../core/ClipSources.h"
#include "../core/Document.h"
#include "../core/I18n.h"
#include "../core/PluginParams.h"
#include "../core/TimeWarp.h"
#include "../core/TrackReceipt.h"
#include "ClipAudioEdit.h"
#include "ClipOps.h"
#include "ClipSource.h"

namespace gl::cliptools {

namespace fs = std::filesystem;

namespace {

std::string firstLine(const std::string& d) { return d.substr(0, d.find('\n')); }

void turn(ed::Audio& a) {
  std::reverse(a.L.begin(), a.L.end());
  std::reverse(a.R.begin(), a.R.end());
}

// The target's frames for a file read at `rate` (the clip's frames are the decoded sample's).
void scaleTo(const Target& t, double rate, int64_t n, int64_t& a, int64_t& b, ed::Range& part) {
  a = t.a, b = t.b, part = t.part;
  if (t.rate > 0 && std::fabs(rate - t.rate) > 0.5) {
    const double k = rate / t.rate;
    a = static_cast<int64_t>(std::llround(static_cast<double>(t.a) * k));
    b = static_cast<int64_t>(std::llround(static_cast<double>(t.b) * k));
    part = {static_cast<int64_t>(std::llround(static_cast<double>(t.part.a) * k)), static_cast<int64_t>(std::llround(static_cast<double>(t.part.b) * k))};
  }
  a = std::clamp<int64_t>(a, 0, n);
  b = std::clamp<int64_t>(b, a, n);
  part.a = std::clamp<int64_t>(part.a, 0, b - a);
  part.b = std::clamp<int64_t>(part.b, part.a, b - a);
}

bool isSamplerChannel(const Channel& ch) { return ch.plugin.type == "gl.sampler"; }

// The text before and after the name in an edited copy's name ("%s (edited)", translated or not).
bool editedParts(const char* fmt, std::string& pre, std::string& post) {
  const std::string f = fmt;
  const size_t at = f.find("%s");
  if (at == std::string::npos) return false;
  pre = f.substr(0, at);
  post = f.substr(at + 2);
  return !(pre.empty() && post.empty());
}

// The name inside an edited copy's name, in the interface language or English. False: not one.
bool stripEdited(const std::string& name, std::string& base) {
  for (const char* fmt : {i18n::tr("%s (edited)"), "%s (edited)"}) {
    std::string pre, post;
    if (!editedParts(fmt, pre, post) || name.size() <= pre.size() + post.size()) continue;
    if (name.compare(0, pre.size(), pre) == 0 && name.compare(name.size() - post.size(), post.size(), post) == 0) {
      base = name.substr(pre.size(), name.size() - pre.size() - post.size());
      return true;
    }
  }
  return false;
}

}  // namespace

bool target(const Song& s, ObjectId clip, double t0, double t1, Target& t, std::string* why) {
  t = Target{};
  const Clip* c = s.clip(clip);
  const Channel* ch = c && c->kind == ClipKind::Audio ? s.channel(c->source) : nullptr;
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (!ch) return fail("Not an audio clip");
  const std::string file = firstLine(ch->plugin.data);
  std::error_code ec;
  if (file.empty() || !fs::is_regular_file(fs::u8path(file), ec)) return fail("The clip's audio file is missing");
  const ClipSource src = clipSource(*ch);
  if (!src.ok()) return fail("The clip's audio could not be read");
  const double c0 = static_cast<double>(c->start), c1 = static_cast<double>(c->start + s.clipLength(*c));
  int64_t a = 0, b = 0;
  if (!clipedit::clipRangeFrames(s, clip, c0, c1, a, b)) return fail("The clip plays none of its audio");
  t.clip = clip;
  t.channel = ch->id;
  t.file = file;
  t.name = ch->name.empty() ? fs::u8path(file).stem().u8string() : uneditedName(ch->name);
  t.a = a, t.b = b;
  t.rate = src.sample->sampleRate;
  t.reversed = clipPlaysReversed(*ch, *c);
  if (t1 > t0) {
    int64_t sa = 0, sb = 0;
    if (!clipedit::clipRangeFrames(s, clip, std::max(t0, c0), std::min(t1, c1), sa, sb)) return fail("The selection holds none of the clip's audio");
    sa = std::clamp(sa, a, b), sb = std::clamp(sb, a, b);
    if (sb <= sa) return fail("The selection holds none of the clip's audio");
    t.part = t.reversed ? ed::Range{b - sb, b - sa} : ed::Range{sa - a, sb - a};
    if (t.part.a == 0 && t.part.b == b - a) t.part = {};
  }
  return true;
}

bool target(const Song& s, ObjectId clip, Target& t, std::string* why) { return target(s, clip, 0, 0, t, why); }

ed::AudioPtr windowAudio(const Target& t, std::string* why) {
  ed::Content full;
  std::string err;
  if (!ed::loadContent(t.file, full, &err) || full.frames() < 1) {
    if (why) *why = err.empty() ? std::string("The audio could not be read") : err;
    return nullptr;
  }
  int64_t a = 0, b = 0;
  ed::Range part;
  scaleTo(t, full.rate(), full.frames(), a, b, part);
  ed::Audio w = ed::extract(full.a(), {a, b});
  if (t.reversed) turn(w);
  return std::make_shared<const ed::Audio>(std::move(w));
}

ed::ToolParams paramsFor(const ed::ToolParams& p, const Target& first, const Target& t) {
  ed::ToolParams q = p;
  if (p.kind == ed::ToolKind::DrumStretch && t.clip != first.clip) {
    // The same tempo change for every loop: each one's new length from its own length.
    const auto len = [](const Target& x) { return static_cast<double>(x.part.empty() ? x.b - x.a : x.part.len()); };
    const double rate = first.rate > 0 ? first.rate : 44100.0;
    const double f = static_cast<double>(ed::drumStretchLength(p.drum, rate)) / std::max(1.0, len(first));
    const double rt = t.rate > 0 ? t.rate : rate;
    const double newSec = len(t) * f / rt;
    q.drum.bars = newSec * p.drum.targetBpm / (60.0 * std::max(1, p.drum.beatsPerBar));
  }
  return q;
}

Result render(const Target& t, const ed::ToolParams& p, const std::string& dir, ed::Progress* prog) {
  Result r;
  r.clip = t.clip;
  r.channel = t.channel;
  r.kind = p.kind;
  ed::Content full;
  std::string err;
  if (!ed::loadContent(t.file, full, &err) || full.frames() < 1) {
    r.error = err.empty() ? std::string("The audio could not be read") : err;
    return r;
  }
  int64_t a = 0, b = 0;
  ed::Range part;
  scaleTo(t, full.rate(), full.frames(), a, b, part);
  if (b - a < 2) {
    r.error = "The clip plays none of its audio";
    return r;
  }
  ed::Audio win = ed::extract(full.a(), {a, b});
  if (t.reversed) turn(win);
  ed::ToolParams tp = p;
  if (!part.empty())
    for (auto& q : tp.brush) q.f0 += part.a, q.f1 += part.a;  // (the brush is given in the part's frames)
  const ed::Content out = ed::processContent(ed::makeContent(win), part, tp, prog);
  if (ed::stopped(prog)) {
    r.error = "Stopped";
    return r;
  }
  ed::Audio o = out.a();
  if (t.reversed) turn(o);
  r.rate = o.rate;
  r.oldFrames = b - a;
  r.part = ed::changesLength(p.kind);
  ed::Content file;
  if (!r.part) {
    // The whole file, the clip's part replaced (same length, so the clip keeps every setting).
    ed::Audio f = full.a();
    if (o.channels == 2 && f.channels == 1) f.channels = 2;  // (a mono file keeps R equal to L)
    const int64_t n = std::min(o.frames(), b - a);
    for (int ch = 0; ch < 2; ++ch) std::copy(o.ch(ch), o.ch(ch) + n, f.ch(ch) + a);
    if (f.channels == 1) f.R = f.L;
    r.newFrames = b - a;
    file = full;
    file.audio = std::make_shared<const ed::Audio>(std::move(f));
  } else {
    r.newFrames = o.frames();
    file = ed::makeContent(std::move(o));
  }
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  r.path = ed::renderPath(dir, cleanFileName(t.name + " " + ed::toolName(p.kind)), ed::audioHash(file.a()));
  if (!fs::is_regular_file(fs::u8path(r.path), ec) && !ed::writeWavFile(r.path, file, 32, &err)) {
    r.error = err.empty() ? std::string("The new file could not be written") : err;
    r.path.clear();
    return r;
  }
  receipt::Origin og;
  if (!receipt::readOrigin(r.path, og)) {  // J-219: Track Receipt maps the new file back to the source
    og.source = t.file;
    og.kind = "edit";
    if (r.part) {
      og.start = static_cast<double>(a) / full.rate();
      og.end = static_cast<double>(b) / full.rate();
      og.speed = r.newFrames > 0 ? static_cast<double>(b - a) / static_cast<double>(r.newFrames) : 1.0;
      og.reverse = false;
    }
    receipt::writeOrigin(r.path, og);
  }
  return r;
}

bool apply(Song& s, const Result& r) {
  if (r.path.empty()) return false;
  Clip* c = s.clip(r.clip);
  const int idx = s.channelIndex(r.channel);
  if (!c || c->kind != ClipKind::Audio || c->source != r.channel || idx < 0) return false;
  // A channel only this clip plays changes in place; one shared with other clips (or playing notes)
  // is copied, so the others keep the original.
  const bool alone = ops::clipSourceUses(s, ClipKind::Audio, r.channel) <= 1 && !ops::channelHasPatternData(s, r.channel);
  ObjectId chId = r.channel;
  if (!alone) {
    Channel copy = s.channels[static_cast<size_t>(idx)];
    copy.id = s.allocateId();
    copy.layer.clear();
    copy.name = editedName(copy.name.empty() ? fs::u8path(firstLine(copy.plugin.data)).stem().u8string() : copy.name);
    chId = copy.id;
    s.channels.insert(s.channels.begin() + idx + 1, copy);
    c = s.clip(r.clip);
    c->source = chId;
  }
  Channel& ch = *s.channel(chId);
  const size_t nl = ch.plugin.data.find('\n');  // (a slicer's region names stay after the path)
  ch.plugin.data = nl == std::string::npos ? r.path : r.path + ch.plugin.data.substr(nl);
  if (!r.part) return true;
  // The clip's part on its own: the clip plays all of the new file.
  if (isSamplerChannel(ch)) {
    if (ch.plugin.params.size() > static_cast<size_t>(pp::SmpStart)) ch.plugin.params[pp::SmpStart] = 0.0f;
    if (ch.plugin.params.size() > static_cast<size_t>(pp::SmpEnd)) ch.plugin.params[pp::SmpEnd] = 1.0f;
  }
  const double k = r.kind == ed::ToolKind::DrumStretch && r.oldFrames > 0 ? static_cast<double>(r.newFrames) / static_cast<double>(r.oldFrames) : 1.0;
  const bool reversed = clipPlaysReversed(ch, *c);
  if (!c->warp.empty() && !reversed && r.rate > 0) {
    // The markers move into the new file (scaled with a drum loop's new length).
    const double a0 = c->audioStart, span = static_cast<double>(r.oldFrames) / r.rate;
    const std::vector<WarpPoint> fw = warpPoints(c->warp, false, 0.0);
    const double t0 = warpTimeAt(fw, a0);
    std::vector<WarpMarker> warp;
    for (const WarpMarker& m : c->warp)
      if (m.at > a0 && m.at < a0 + span) warp.push_back({m.id, (m.at - a0) * k, (m.to - t0) * k});
    if (!warp.empty()) {
      warp.push_back({0, 0.0, 0.0});
      warp.push_back({0, span * k, (warpTimeAt(fw, a0 + span) - t0) * k});
    }
    normalizeWarp(warp, static_cast<double>(r.newFrames) / r.rate);
    c->warp = std::move(warp);
  } else {
    c->warp.clear();
  }
  c->audioStart = 0;
  c->audioEnd = 0;
  if (c->loop) {
    c->offset = static_cast<Tick>(std::llround(static_cast<double>(c->offset) * k));
    c->length = std::max<Tick>(1, static_cast<Tick>(std::llround(static_cast<double>(c->length) * k)));
  } else if (const Tick n = audioClipNaturalLength(s, *c); n > 0) {
    c = s.clip(r.clip);
    c->length = r.kind == ed::ToolKind::DrumStretch ? n : std::max(c->length, n);
  }
  return true;
}

std::string editedName(const std::string& name) {
  std::string base;
  if (stripEdited(name, base)) return name;
  std::string pre, post;
  if (!editedParts(i18n::tr("%s (edited)"), pre, post)) editedParts("%s (edited)", pre, post);
  return pre + name + post;
}

std::string uneditedName(const std::string& name) {
  std::string base;
  return stripEdited(name, base) ? base : name;
}

std::string undoLabel(ed::ToolKind k, int clips) {
  return std::string(ed::toolName(k)) + (clips == 1 ? std::string(" clip") : " " + std::to_string(clips) + " clips");
}

int processClips(Document& d, const std::set<ObjectId>& clips, const ed::ToolParams& p, const std::string& dir, double t0, double t1, std::string* why) {
  std::vector<Target> ts;
  for (const Clip& c : d.song().playlist) {
    if (!clips.count(c.id) || c.kind != ClipKind::Audio) continue;
    Target t;
    if (target(d.song(), c.id, t0, t1, t, why)) ts.push_back(t);
  }
  std::vector<Result> rs;
  for (const Target& t : ts) {
    Result r = render(t, paramsFor(p, ts.front(), t), dir);
    if (r.path.empty()) {
      if (why) *why = r.error;
      continue;
    }
    rs.push_back(r);
  }
  if (rs.empty()) return 0;
  int n = 0;
  d.editSong(undoLabel(p.kind, static_cast<int>(rs.size())), [&](Song& x) {
    for (const Result& r : rs) n += apply(x, r) ? 1 : 0;
  });
  return n;
}

}  // namespace gl::cliptools
