// Gridline - J-219: Track Receipt measurement (see TrackReceiptPass.h).
#include "TrackReceiptPass.h"
#include "../core/Meter.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <tuple>
#include <optional>
#include "../core/Automation.h"
#include "../core/Params.h"
#include "../core/ProjectInterop.h"
#include "../core/Zip.h"
#include "../host/Backend.h"
#include "Render.h"
#include "Sample.h"
#include "SampleTap.h"

namespace gl::receipt {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char* kAudioExt[] = {".wav", ".wave", ".flac", ".mp3", ".ogg", ".oga", ".wv", ".aif", ".aiff", ".aifc", ".caf", ".m4a", ".w64"};

// Built-ins that read a file as a whole instead of playing it (wavetables,
// impulse responses, resynthesis): the tap sees nothing, the whole file counts.
bool analysesFile(const std::string& type) {
  return type == "gl.gms" || type == "gl.harmor" || type == "gl.convolver" || type == "gl.plasma" || type == "gl.amprack";
}

// K-132: a parameter set to silence (a mute on, a volume at zero) can still be heard once its
// automation runs: an automation clip placed in the playlist, a pattern's automation, a controller
// or modulation link. True when one of them gives it a value that is not silent (a link: any value).
bool automationHears(const Song& s, ParamId id, bool (*silentAt)(double)) {
  const std::optional<ParamDesc> d = describeParam(s, id);
  if (!d) return false;
  const auto clipHears = [&](const AutomationClip& a) {
    for (const AutoPoint& p : a.points) {
      const double n = std::clamp(static_cast<double>(a.min) + static_cast<double>(p.value) * static_cast<double>(a.max - a.min), 0.0, 1.0);
      if (!silentAt(d->fromNormalized(n))) return true;
    }
    return false;
  };
  for (const AutomationLane& l : automationLanes(s))
    if (l.owner == id.owner && l.index == id.index)
      for (const auto& in : l.inst)
        if (in.clip && clipHears(*in.clip)) return true;
  for (const Pattern& p : s.patterns)
    for (const PatternAutomation& a : p.automation)
      if (a.owner == id.owner && a.index == id.index && clipHears(a)) return true;
  for (const ControllerLink& l : s.links)
    if (l.owner == id.owner && l.index == id.index) return true;
  for (const ModLink& l : s.modLinks)
    if (l.enabled && l.owner == id.owner && l.index == id.index) return true;
  return false;
}
bool mutedAt(double v) { return v >= 0.5; }
bool zeroAt(double v) { return v <= 0; }

std::string pluginName(const std::string& type) {
  const PluginInfo* p = findPlugin(type);
  if (p && !p->name.empty()) return p->name;
  return type;
}

// File names in a run of text: everything up to an audio extension, from
// the start of a path ("/", "X:\") or the start of the run.
void namesInText(const std::string& run, std::vector<std::string>& out) {
  const std::string lo = lower(run);
  for (const char* ext : kAudioExt) {
    const std::string e = ext;
    for (size_t at = lo.find(e); at != std::string::npos; at = lo.find(e, at + 1)) {
      const size_t end = at + e.size();
      if (end < lo.size() && std::isalnum(static_cast<unsigned char>(lo[end]))) continue;  // ".wavx"
      size_t start = 0;
      // the last separator that ends a previous item
      for (size_t i = at; i > 0; --i) {
        const char c = run[i - 1];
        if (c == '"' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?' || c == '\t' || c == '=' || c == ';') {
          start = i;
          break;
        }
      }
      std::string cand = run.substr(start, end - start);
      // a path inside: from its root ("X:\" or "X:/" on Windows, else the first "/")
      size_t root = std::string::npos;
      for (size_t k = cand.find(':'); k != std::string::npos; k = cand.find(':', k + 1)) {
        const bool slash = k + 1 < cand.size() && (cand[k + 1] == '\\' || cand[k + 1] == '/');
        const bool drive = k > 0 && std::isalpha(static_cast<unsigned char>(cand[k - 1])) &&
                           (k == 1 || !std::isalnum(static_cast<unsigned char>(cand[k - 2])));  // not "file:/"
        if (slash && drive) {
          root = k - 1;
          break;
        }
      }
      if (root != std::string::npos) cand = cand.substr(root);
      else if ((root = cand.find('/')) != std::string::npos) cand = cand.substr(root);
      while (!cand.empty() && (cand.front() == ' ' || cand.front() == '\'')) cand.erase(cand.begin());
      if (cand.size() > e.size() && cand.size() < 1024) out.push_back(cand);
    }
  }
}

void scanBytes(const std::string& b, std::vector<std::string>& out, int depth) {
  // UTF-8 / ASCII runs
  std::string run;
  auto flush = [&]() {
    if (run.size() >= 5) namesInText(run, out);
    run.clear();
  };
  for (unsigned char c : b) {
    if (c >= 0x20 && c != 0x7F) run += static_cast<char>(c);
    else flush();
  }
  flush();
  // UTF-16 (little endian) runs, ASCII range
  for (size_t off = 0; off < 2; ++off) {
    run.clear();
    for (size_t i = off; i + 1 < b.size(); i += 2) {
      const auto lo = static_cast<unsigned char>(b[i]), hi = static_cast<unsigned char>(b[i + 1]);
      if (hi == 0 && lo >= 0x20 && lo < 0x7F) run += static_cast<char>(lo);
      else flush();
    }
    flush();
  }
  // zlib blocks (compressed plugin states)
  if (depth >= 2) return;
  int tries = 0;
  for (size_t i = 0; i + 2 < b.size() && tries < 64; ++i) {
    const auto c0 = static_cast<unsigned char>(b[i]), c1 = static_cast<unsigned char>(b[i + 1]);
    if (c0 != 0x78 || (c1 != 0x01 && c1 != 0x5E && c1 != 0x9C && c1 != 0xDA)) continue;
    ++tries;
    std::vector<uint8_t> inflated;
    if (inflateRaw(reinterpret_cast<const uint8_t*>(b.data()) + i + 2, b.size() - i - 2, inflated) && inflated.size() >= 16 &&
        inflated.size() < (64u << 20))
      scanBytes(std::string(inflated.begin(), inflated.end()), out, depth + 1);
  }
}

}  // namespace

bool isAudioName(const std::string& path) {
  const std::string lo = lower(path);
  for (const char* ext : kAudioExt) {
    const std::string e = ext;
    if (lo.size() > e.size() && lo.compare(lo.size() - e.size(), e.size(), e) == 0) return true;
  }
  return false;
}

std::vector<std::string> chunkFileNames(const std::string& data) {
  std::vector<std::string> out;
  scanBytes(data, out, 0);
  // base64 segments ("vst3s1:<b64>:<b64>", "clap1:<b64>", ...)
  size_t at = 0;
  while (at <= data.size()) {
    size_t next = data.find(':', at);
    if (next == std::string::npos) next = data.size();
    const std::string seg = data.substr(at, next - at);
    std::string bytes;
    if (seg.size() >= 16 && host::base64Decode(seg, bytes)) scanBytes(bytes, out, 0);
    at = next + 1;
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<FileRef> fileRefs(const Song& song) {
  std::vector<FileRef> refs;
  std::set<std::string> seen;
  auto add = [&](FileRef r) {
    if (r.path.empty() || r.path.rfind("builtin:", 0) == 0 || r.path.rfind("tone:", 0) == 0) return;
    if (!isAudioName(r.path)) return;
    const std::string key = r.path + "|" + std::to_string(r.owner) + "|" + std::to_string(r.slot) + (r.hosted ? "|h" : "");
    if (seen.insert(key).second) refs.push_back(std::move(r));
  };
  auto scanState = [&](const PluginState& st, ObjectId owner, bool channel, int slot) {
    if (st.empty()) return;
    const bool hosted = isHostedType(st.type);
    const std::string name = pluginName(st.type);
    if (!hosted) {
      PluginState copy = st;
      forEachStateFileRef(copy, [&](std::string& p) { add(FileRef{p, owner, channel, slot, false, name}); });
      // Text states with a file on later lines (zone maps, programs): every line.
      if (!st.data.empty() && st.data.front() != '{' && st.data.front() != '[' && st.data.find('\n') != std::string::npos) {
        std::vector<std::string> names;
        size_t at = 0;
        while (at < st.data.size()) {
          size_t nl = st.data.find('\n', at);
          if (nl == std::string::npos) nl = st.data.size();
          namesInText(st.data.substr(at, nl - at), names);
          at = nl + 1;
        }
        for (const auto& p : names) add(FileRef{p, owner, channel, slot, false, name});
      }
    } else {
      for (const auto& p : chunkFileNames(st.data)) add(FileRef{p, owner, channel, slot, true, name});
    }
    // A stand-in keeps the third-party plugin's saved state: files it names count as used inside it.
    if (!st.original.empty())
      for (const auto& p : chunkFileNames(st.original)) add(FileRef{p, owner, channel, slot, true, name + " (stand-in)"});
  };
  for (const Channel& c : song.channels) {
    scanState(c.plugin, c.id, true, -1);
    for (size_t i = 0; i < c.noteFx.size(); ++i) scanState(c.noteFx[i], c.id, true, static_cast<int>(i));
  }
  for (const MixerTrack& m : song.mixer)
    for (size_t i = 0; i < m.slots.size(); ++i) scanState(m.slots[i].plugin, m.id, false, static_cast<int>(i));
  // Bare names (from chunks): the project's file of that name, when there is one.
  std::map<std::string, std::string> byName;
  for (const FileRef& r : refs)
    if (r.path.find('/') != std::string::npos || r.path.find('\\') != std::string::npos) byName[lower(baseName(r.path))] = r.path;
  for (FileRef& r : refs) {
    if (r.path.find('/') == std::string::npos && r.path.find('\\') == std::string::npos) {
      auto it = byName.find(lower(r.path));
      if (it != byName.end()) r.path = it->second;
    } else if (r.path.find('\\') != std::string::npos && !fs::exists(fs::path(r.path))) {
      // A Windows path saved on another computer: the project's file of that name.
      auto it = byName.find(lower(baseName(r.path)));
      if (it != byName.end() && it->second != r.path) r.path = it->second;
    }
  }
  return refs;
}

bool writeRenderOrigin(const Song& song, const std::string& path) {
  Origin o;
  o.rangeKnown = false;
  o.kind = "render";
  std::set<std::string> seen;
  for (const FileRef& r : fileRefs(song))
    if (r.path != path && seen.insert(r.path).second) o.sources.push_back(r.path);
  if (o.sources.empty()) return false;
  o.source = o.sources.front();
  return writeOrigin(path, o);
}

namespace {

std::string channelWhere(const Song& s, const Channel& c) {
  return "Channel " + std::to_string(s.channelIndex(c.id) + 1) + " '" + c.name + "' (" + pluginName(c.plugin.type) + ")";
}
// The bars a clip covers are kept in Use::bar0 / bar1 (clips on one track merge into one use).
std::string clipWhere(const Song& s, const Clip& c) {
  return "Playlist " + playlistTrackName(s, static_cast<size_t>(std::max(0, c.track))) + ", audio clips";
}
double clipBar(const Song& s, Tick t) { return static_cast<double>(MeterMap(s).barIndex(std::max<Tick>(0, t))) + 1; }  // K-179

// Uses of one file from one place (every clip on a playlist track, say) become one.
void mergeUses(std::vector<Use>& uses) {
  std::vector<Use> out;
  std::map<std::tuple<std::string, std::string, bool, bool, std::string>, size_t> at;
  for (Use& u : uses) {
    const auto key = std::make_tuple(u.file, u.where, u.muted, u.estimated, u.note);
    auto it = at.find(key);
    if (it == at.end()) {
      at[key] = out.size();
      out.push_back(std::move(u));
      continue;
    }
    Use& x = out[it->second];
    x.plays += u.plays;
    x.ranges.insert(x.ranges.end(), u.ranges.begin(), u.ranges.end());
    x.ranges = unite(x.ranges);
    if (u.bar0 > 0) x.bar0 = x.bar0 > 0 ? std::min(x.bar0, u.bar0) : u.bar0;
    x.bar1 = std::max(x.bar1, u.bar1);
  }
  uses = std::move(out);
}
std::string mixerWhere(const Song& s, const MixerTrack& m, int slot) {
  const int mi = s.mixerIndex(m.id);
  std::string w = "Mixer " + mixerTrackName(s, static_cast<size_t>(std::max(0, mi)));
  if (slot >= 0 && static_cast<size_t>(slot) < m.slots.size()) w += ", slot " + std::to_string(slot + 1) + " (" + pluginName(m.slots[static_cast<size_t>(slot)].plugin.type) + ")";
  return w;
}

bool hasNotes(const Pattern& p, ObjectId ch) {
  auto st = p.steps.find(ch);
  if (st != p.steps.end())
    for (uint8_t v : st->second)
      if (v) return true;
  auto nt = p.notes.find(ch);
  return nt != p.notes.end() && !nt->second.empty();
}

}  // namespace

Measurement scan(const Song& song, bool decode) {
  Measurement m;
  std::set<std::string> seen;
  for (const FileRef& r : fileRefs(song)) {
    if (!seen.insert(r.path).second) continue;
    FileIn f;
    f.path = r.path;
    std::error_code ec;
    f.exists = fs::path(r.path).is_absolute() && fs::is_regular_file(r.path, ec);
    if (f.exists && decode)
      if (SamplePtr s = sampleCache().get(r.path)) f.seconds = s->seconds();
    f.chain = originChain(r.path);
    f.derived = !f.chain.empty();
    m.files.push_back(f);
  }
  return m;
}

Measurement measure(const Song& song0, const PassOptions& o) {
  Measurement m = scan(song0, true);
  const std::vector<FileRef> refs = fileRefs(song0);
  std::map<std::string, int> fileIndex;
  for (size_t i = 0; i < m.files.size(); ++i) fileIndex[m.files[i].path] = static_cast<int>(i);
  // The song as the pass plays it: what is muted or silent is taken out (and listed).
  Song song = song0;
  // K-132: silent as set and kept so by its automation (a muted channel whose mute is automated
  // off later is heard there).
  const auto trackMuted = [&](const MixerTrack& t) { return t.muted && !automationHears(song0, mixerParam(t.id, MixerParam::Mute), mutedAt); };
  const auto trackZero = [&](const MixerTrack& t) { return t.volume <= 0 && !automationHears(song0, mixerParam(t.id, MixerParam::Volume), zeroAt); };
  std::set<ObjectId> silentCh;
  for (Channel& c : song.channels) {
    std::string why;
    const MixerTrack* mt = song.mixerTrack(c.mixerTrack);
    if (c.muted && !automationHears(song0, channelParam(c.id, ChannelParam::Mute), mutedAt)) why = "muted channel";
    else if (c.gain <= 0 && !automationHears(song0, channelParam(c.id, ChannelParam::Volume), zeroAt)) why = "channel volume at zero";
    else if (mt && trackMuted(*mt)) why = "muted mixer track";
    else if (mt && trackZero(*mt)) why = "mixer track volume at zero";
    if (why.empty()) continue;
    silentCh.insert(c.id);
    for (const FileRef& r : refs)
      if (r.channel && r.owner == c.id) {
        Use u;
        u.file = r.path, u.owner = c.id, u.where = channelWhere(song0, c), u.muted = true, u.note = why;
        m.uses.push_back(u);
      }
    c.plugin = PluginState{};
    c.noteFx.clear();
  }
  for (MixerTrack& t : song.mixer) {
    if (!(trackMuted(t) || trackZero(t))) continue;
    for (const FileRef& r : refs)
      if (!r.channel && r.owner == t.id) {
        Use u;
        u.file = r.path, u.owner = t.id, u.where = mixerWhere(song0, t, r.slot), u.muted = true, u.note = "muted mixer track";
        m.uses.push_back(u);
      }
  }
  // Third-party plugins are never run here: what they play is not measurable
  // (their uses are estimated below), and loading them would only slow the pass.
  for (Channel& c : song.channels) {
    if (isHostedType(c.plugin.type)) c.plugin = PluginState{};
    c.noteFx.erase(std::remove_if(c.noteFx.begin(), c.noteFx.end(), [](const PluginState& p) { return isHostedType(p.type); }),
                   c.noteFx.end());
  }
  for (MixerTrack& t : song.mixer)
    for (auto& sl : t.slots)
      if (isHostedType(sl.plugin.type)) sl.enabled = false;
  std::vector<Clip> kept;
  for (const Clip& c : song.playlist) {
    std::string why;
    const bool trackMuted = c.track >= 0 && static_cast<size_t>(c.track) < song.tracks.size() && song.tracks[static_cast<size_t>(c.track)].muted;
    if (c.kind == ClipKind::Audio) {
      if (c.muted) why = "muted clip";
      else if (trackMuted) why = "muted playlist track";
      else if (c.gain <= 0) why = "clip volume at zero";
      else if (silentCh.count(c.source)) why = "its channel is muted or silent";
      if (!why.empty()) {
        if (const Channel* ch = song0.channel(c.source))
          for (const FileRef& r : refs)
            if (r.channel && r.owner == ch->id && !r.hosted) {
              Use u;
              u.file = r.path, u.owner = c.id, u.where = clipWhere(song0, c), u.muted = true, u.note = why;
              u.bar0 = clipBar(song0, c.start), u.bar1 = clipBar(song0, c.start + std::max<Tick>(0, song0.clipLength(c) - 1));
              m.uses.push_back(u);
            }
        continue;
      }
    } else if (c.muted || trackMuted) {
      continue;  // muted pattern / automation clips do not play (their channels' uses are what counts)
    }
    kept.push_back(c);
  }
  song.playlist = kept;
  // Which channels get notes (for uses Gridline cannot measure).
  std::set<ObjectId> playing;
  const bool patternMode = song0.playlist.empty();
  std::set<ObjectId> usedPatterns;
  for (const Clip& c : song.playlist)
    if (c.kind == ClipKind::Pattern) usedPatterns.insert(c.pattern);
  for (const Pattern& p : song.patterns)
    if (patternMode || usedPatterns.count(p.id))
      for (const Channel& c : song.channels)
        if (hasNotes(p, c.id)) playing.insert(c.id);
  for (const Clip& c : song.playlist)
    if (c.kind == ClipKind::Audio) playing.insert(c.source);
  // Watch every file and play the project.
  std::vector<tap::WatchFile> watch;
  for (const FileIn& f : m.files) watch.push_back(tap::WatchFile{f.path, f.seconds});
  tap::Pass pass(watch);
  if (!pass.begin()) {
    m.warnings.push_back("Another Track Receipt is measuring right now; try again in a moment.");
    return m;
  }
  std::vector<RenderJob> jobs;
  RenderJob base;
  base.sampleRate = o.sampleRate;
  base.tail = TailMode::Leave;
  base.maxTailSeconds = 10;
  base.threads = 1;
  base.cancel = o.cancel;
  if (!patternMode) {
    m.mode = "song";
    jobs.push_back(base);
  } else {
    m.mode = "patterns";
    for (const Pattern& p : song.patterns) {
      bool any = false;
      for (const Channel& c : song.channels) any = any || hasNotes(p, c.id);
      if (!any) continue;
      RenderJob j = base;
      j.scope = RenderScope::Pattern;
      j.pattern = p.id;
      jobs.push_back(j);
    }
  }
  for (size_t k = 0; k < jobs.size(); ++k) {
    RenderJob& j = jobs[k];
    const double n = static_cast<double>(jobs.size());
    j.progress = [&o, k, n](double f) {
      if (o.progress) o.progress((static_cast<double>(k) + f) / n);
    };
    RenderResult rr = renderAudio(song, j);
    if (!rr.error.empty() && rr.error.find("empty") == std::string::npos) m.warnings.push_back("Render: " + rr.error);
    if (rr.cancelled || (o.cancel && o.cancel->load())) {
      m.cancelled = true;
      break;
    }
  }
  pass.end();
  if (pass.pairsFull()) m.warnings.push_back("Very many sample uses: some were not told apart (the totals are still complete).");
  // Uses measured.
  const MeterMap mm0(song0);
  std::set<std::pair<std::string, ObjectId>> measured;
  for (const tap::Use& u : pass.uses()) {
    if (u.file < 0 || static_cast<size_t>(u.file) >= m.files.size()) continue;
    Use x;
    x.file = m.files[static_cast<size_t>(u.file)].path;
    x.owner = u.owner;
    x.plays = u.plays;
    for (const tap::Range& r : u.ranges)
      if (r.b - r.a >= kMinHeard) x.ranges.push_back(Range{r.a, r.b});  // see kMinHeard
    if (u.tick1 >= u.tick0) {
      x.bar0 = static_cast<double>(mm0.barIndex(static_cast<Tick>(std::max(0.0, u.tick0)))) + 1;  // K-179: meter map bars
      x.bar1 = static_cast<double>(mm0.barIndex(static_cast<Tick>(std::max(0.0, u.tick1)))) + 1;
      if (m.mode != "song") x.bar0 = x.bar1 = 0;
    }
    if (const Clip* c = song0.clip(u.owner)) {
      x.where = clipWhere(song0, *c);
      measured.insert({x.file, c->source});
    } else if (const Channel* ch = song0.channel(u.owner)) {
      x.where = channelWhere(song0, *ch);
    } else if (const MixerTrack* mt = song0.mixerTrack(u.owner)) {
      x.where = mixerWhere(song0, *mt, -1);
    } else {
      x.where = "Preview";
    }
    if (x.ranges.empty()) continue;
    measured.insert({x.file, u.owner});
    m.uses.push_back(x);
  }
  // Uses Gridline cannot measure: inside third-party plugins, and built-ins that analyse a file.
  for (const FileRef& r : refs) {
    if (measured.count({r.path, r.owner})) continue;
    if (r.channel && (silentCh.count(r.owner) || !playing.count(r.owner))) continue;
    if (!r.channel) {
      const MixerTrack* t = song0.mixerTrack(r.owner);
      if (!t || trackMuted(*t) || trackZero(*t)) continue;
    }
    std::string type;
    if (r.channel) {
      if (const Channel* c = song0.channel(r.owner)) type = r.slot >= 0 && static_cast<size_t>(r.slot) < c->noteFx.size() ? c->noteFx[static_cast<size_t>(r.slot)].type : c->plugin.type;
    } else if (const MixerTrack* t = song0.mixerTrack(r.owner); t && r.slot >= 0 && static_cast<size_t>(r.slot) < t->slots.size()) {
      if (!t->slots[static_cast<size_t>(r.slot)].enabled) continue;
      type = t->slots[static_cast<size_t>(r.slot)].plugin.type;
    }
    if (!r.hosted && !analysesFile(type)) continue;  // a built-in that streams the file but never played it
    Use u;
    u.file = r.path;
    u.owner = r.owner;
    u.estimated = true;
    const FileIn& f = m.files[static_cast<size_t>(fileIndex[r.path])];
    double len = f.seconds;
    if (type == "gl.convolver") len = std::min(len, 60.0);  // impulse responses are read up to 60 s
    if (len > 0) u.ranges.push_back(Range{0, len});
    if (r.channel) {
      const Channel* c = song0.channel(r.owner);
      u.where = (r.hosted ? "Used inside " + r.plugin + " on " : "Read whole by " + r.plugin + " on ") + (c ? "'" + c->name + "'" : std::string("a channel"));
    } else {
      const MixerTrack* t = song0.mixerTrack(r.owner);
      u.where = (r.hosted ? "Used inside " + r.plugin + " on " : "Read whole by " + r.plugin + " on ") + (t ? mixerWhere(song0, *t, -1) : std::string("the mixer"));
    }
    u.note = r.hosted ? "played part unknown" : "the whole file is read";
    if (!f.exists) u.note += "; file not on this computer";
    m.uses.push_back(u);
  }
  mergeUses(m.uses);
  return m;
}

}  // namespace gl::receipt
