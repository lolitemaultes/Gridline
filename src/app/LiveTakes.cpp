// Gridline — J-217: audio being recorded into the playlist, drawn live. See LiveTakes.h.
#include "LiveTakes.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include "../core/Meter.h"
#include "App.h"
#include "DevSignal.h"

namespace gl::livetake {

namespace {

// One take being summarised, in the recorder's take order.
struct Feed {
  ObjectId mixer = kNoId;
  int pass = 0;
  double startTick = 0;
  bool stereo = true;
  bool started = false;
  size_t fed = 0;   // take frames summarised
  int64_t pad = 0;  // leading silence of the clip's file
  double sampleRate = 44100;
  std::unique_ptr<PeakSummary> peaks = std::make_unique<PeakSummary>();
};

struct State {
  std::vector<Feed> feeds;
  std::vector<View> views;
  int closedFrames = 0;  // consecutive frames with every take closed and nothing new
  int64_t lastTotal = -1;
  std::map<std::string, Handoff> handoffs;  // committed file → its live summary
  TempoMap map;
  uint64_t mapRev = ~0ull;
  bool watch = false;  // dev: print the last live frame before the commit
};
State& st() {
  static State s;
  return s;
}

TakeCommitOptions placeOptions(const App& a) {
  TakeCommitOptions o;  // as processAudioRecording commits (muteEarlierTakes: default)
  o.sampleRate = a.engine.sampleRate();
  o.naming = a.settings.getNumber("record.naming") > 0.5 ? TakeCommitOptions::Naming::Input : TakeCommitOptions::Naming::Insert;
  o.nameTracks = a.settings.getBool("record.nameTracks");
  return o;
}

}  // namespace

std::vector<View>& views() { return st().views; }

void reset() {
  State& s = st();
  s.feeds.clear();
  s.views.clear();
  s.closedFrames = 0;
  s.lastTotal = -1;
}

bool settled() { return st().closedFrames >= 2; }

void update(App& a) {
  State& s = st();
  if (!a.audioRecSession) {
    if (!s.feeds.empty() || !s.views.empty()) reset();
    return;
  }
  const std::vector<RecordedTake>& takes = a.audioRec.takes();
  // A new session (begin() forgot the takes): start over.
  bool same = s.feeds.size() <= takes.size();
  for (size_t i = 0; same && i < s.feeds.size(); ++i)
    same = s.feeds[i].mixer == takes[i].mixer && s.feeds[i].pass == takes[i].pass && s.feeds[i].startTick == takes[i].startTick;
  if (!same) reset();
  const Song& song = a.doc.song();
  if (s.mapRev != a.doc.revision()) {
    s.map.build(song);
    s.mapRev = a.doc.revision();
  }
  const TakeCommitOptions o = placeOptions(a);
  const double sr = o.sampleRate > 0 ? o.sampleRate : 44100.0;
  int64_t total = 0;
  for (size_t i = 0; i < takes.size(); ++i) {
    const RecordedTake& t = takes[i];
    if (i == s.feeds.size()) {
      Feed f;
      f.mixer = t.mixer, f.pass = t.pass, f.startTick = t.startTick;
      s.feeds.push_back(std::move(f));
    }
    Feed& f = s.feeds[i];
    if (!f.started) {
      // Latched at the take's start: the clip's file is stereo or mono from its first frame.
      const MixerTrack* mt = song.mixerTrack(t.mixer);
      f.stereo = mt ? (mt->inputStereo || mt->recPoint != 0) : true;
      f.pad = takeSpan(s.map, t.startTick, 0, sr).pad;
      f.sampleRate = sr;
      f.peaks->addSilence(static_cast<size_t>(f.pad));
      f.started = true;
    }
    // Only the frames drained since the last frame (the take's vectors only grow).
    const size_t n = static_cast<size_t>(t.frames());
    if (n > f.fed) {
      const float* L = t.L.data() + f.fed;
      const float* R = (f.stereo ? t.R.data() : t.L.data()) + f.fed;
      f.peaks->add(L, R, n - f.fed);
      f.fed = n;
    }
    total += t.frames();
  }
  // The clips as commitTakes() will place them.
  static std::vector<View> prev;  // (the rectangles drawn last frame stay with their take)
  prev.swap(s.views);
  s.views.clear();
  for (size_t i = 0; i < takes.size(); ++i) {
    const RecordedTake& t = takes[i];
    if (t.frames() == 0) continue;
    View v;
    v.take = i;
    v.mixer = t.mixer;
    v.place = takePlacement(song, takes, i, o);
    v.span = takeSpan(s.map, t.startTick, t.frames(), sr);
    v.fileFrames = s.feeds[i].pad + t.frames();
    v.sampleRate = sr;
    v.done = t.done;
    v.peaks = s.feeds[i].peaks.get();
    for (const View& p : prev)
      if (p.take == i) v.drawn = p.drawn, v.x0 = p.x0, v.x1 = p.x1, v.y0 = p.y0, v.y1 = p.y1;
    s.views.push_back(std::move(v));
  }
  const bool closed = !a.audioRec.open();
  s.closedFrames = closed && total == s.lastTotal ? s.closedFrames + 1 : closed ? 1 : 0;
  s.lastTotal = total;
  static size_t passesReported = 0;  // dev checks (waitfor): "livetake-clips:<n>", n live clips at once
  if (s.views.size() < passesReported) passesReported = 0;
  for (; passesReported < s.views.size(); ++passesReported) devsig::reach("livetake-clips:" + std::to_string(passesReported + 1));
  // Dev checks ("liverec watch"): the last live frame, as the takes are about to be committed.
  if (s.watch && settled()) {
    std::printf("livetakes final:\n");
    devTrace(a, -1);
  }
}

void handoff(const std::vector<RecordedTake>& takes, const TakeCommitResult& r) {
  State& s = st();
  if (s.handoffs.size() > 64) s.handoffs.clear();  // never adopted (clips deleted before they drew)
  for (size_t k = 0; k < r.clips.size() && k < r.files.size() && k < r.takeIndex.size(); ++k) {
    if (r.takeIndex[k] >= takes.size()) continue;
    const RecordedTake& t = takes[r.takeIndex[k]];
    for (Feed& f : s.feeds) {
      if (f.mixer != t.mixer || f.pass != t.pass || !f.started || f.fed != static_cast<size_t>(t.frames())) continue;
      Handoff h;
      h.peaks = std::move(*f.peaks);
      h.frames = f.pad + t.frames();
      h.sampleRate = f.sampleRate;
      s.handoffs[r.files[k]] = std::move(h);
      break;
    }
  }
}

const Handoff* pending(const std::string& path) {
  const State& s = st();
  auto it = s.handoffs.find(path);
  return it == s.handoffs.end() ? nullptr : &it->second;
}

const PeakSummary& samplePeaks(const SamplePtr& smp) {
  static std::unordered_map<const Sample*, std::pair<SamplePtr, PeakSummary>> cache;
  auto it = cache.find(smp.get());
  if (it != cache.end()) return it->second.second;
  if (cache.size() > 256) cache.clear();
  PeakSummary p;
  State& s = st();
  if (auto h = s.handoffs.find(smp->path); h != s.handoffs.end() && h->second.frames == static_cast<int64_t>(smp->frames())) {
    p = std::move(h->second.peaks);  // a take just recorded: its live summary, no rescan
    s.handoffs.erase(h);
  } else {
    p.add(smp->left.data(), smp->right.data(), smp->frames());
  }
  return (cache[smp.get()] = {smp, std::move(p)}).second;
}

void devTrace(App& a, int frame) {
  const State& s = st();
  std::printf("livetakes frame %d: session %d finishing %d views %zu settled %d\n", frame, a.audioRecSession ? 1 : 0,
              a.audioRecFinishing ? 1 : 0, s.views.size(), settled() ? 1 : 0);
  for (const View& v : s.views) {
    // The incremental summary against a full rescan of what the take holds now.
    const RecordedTake& t = a.audioRec.takes()[v.take];
    const Feed& f = s.feeds[v.take];
    PeakSummary full;
    full.addSilence(static_cast<size_t>(f.pad));
    full.add(t.L.data(), (f.stereo ? t.R.data() : t.L.data()), static_cast<size_t>(t.frames()));
    float lo = 0, hi = 0;
    v.peaks->range(0, v.peaks->bins(), lo, hi);
    std::printf("  live take %zu mixer %u pass %d track %d start %lld length %lld pad %lld frames %lld bins %zu bin %zu lo %.4f hi %.4f "
                "sum %016" PRIx64 " rescan %d done %d muted %d stereo %d drawn %d x %.1f %.1f y %.1f %.1f name \"%s\"\n",
                v.take, static_cast<unsigned>(v.mixer), t.pass, v.place.track, static_cast<long long>(v.span.start),
                static_cast<long long>(v.span.length), static_cast<long long>(v.span.pad), static_cast<long long>(v.fileFrames),
                v.peaks->bins(), v.peaks->binFrames(), static_cast<double>(lo), static_cast<double>(hi), v.peaks->checksum(),
                full.checksum() == v.peaks->checksum() ? 1 : 0, v.done ? 1 : 0, v.place.muted ? 1 : 0, f.stereo ? 1 : 0, v.drawn ? 1 : 0,
                static_cast<double>(v.x0), static_cast<double>(v.x1), static_cast<double>(v.y0), static_cast<double>(v.y1),
                v.place.name.c_str());
    // The amplitude envelope (dev checks of the signal): the peak of every whole quarter second
    // after the latency pad, at most 64. A fixed window, not a share of the take: on a slow machine
    // the take is longer and a share of it would span both amplitudes every time.
    std::printf("    env");
    const size_t nb = v.peaks->bins(), bf = std::max<size_t>(1, v.peaks->binFrames());
    const size_t win = std::max<size_t>(1, static_cast<size_t>(std::max(1, a.audioSampleRate)) / 4 / bf);
    for (size_t b = (static_cast<size_t>(std::max<int64_t>(0, f.pad)) + bf - 1) / bf, k = 0; b + win <= nb && k < 64; b += win, ++k) {
      float l = 0, h = 0;
      v.peaks->range(b, b + win, l, h);
      std::printf(" %.3f", static_cast<double>(std::max(h, -l)));
    }
    std::printf("\n");
  }
  std::fflush(stdout);
}

void devClipPeaks(App& a, int frame) {
  const Song& s = a.doc.song();
  std::printf("clippeaks frame %d:\n", frame);
  for (const Clip& c : s.playlist) {
    if (c.kind != ClipKind::Audio) continue;
    const Channel* ch = s.channel(c.source);
    if (!ch || ch->plugin.data.empty()) continue;
    SamplePtr smp = sampleCache().get(ch->plugin.data);  // (dev: waits for the decode)
    if (!smp) continue;
    const PeakSummary& drawn = samplePeaks(smp);
    PeakSummary file;
    file.add(smp->left.data(), smp->right.data(), smp->frames());
    std::printf("  clip %u track %d start %lld length %lld muted %d take %d frames %zu bins %zu sum %016" PRIx64 " file %016" PRIx64
                " name \"%s\"\n",
                static_cast<unsigned>(c.id), c.track, static_cast<long long>(c.start), static_cast<long long>(c.length), c.muted ? 1 : 0, c.take,
                smp->frames(), drawn.bins(), drawn.checksum(), file.checksum(), ch->name.c_str());
  }
  std::fflush(stdout);
}

// --dev-input "liverec ..." (J-217 checks; the input is the synthetic one, GRIDLINE_SYNTH_INPUT):
//   liverec arm <mixer index> [mono]  · input 1 (- 2) of the insert, armed for disk recording
//   liverec select <bar0> <bar1>       · the playlist time selection (punch / loop range), 0 0 clears it
//   liverec start                      · song mode, record button armed (audio), the transport starts
//   liverec stop                       · the transport stops
//   liverec trace                      · live takes (devTrace) · liverec clips: committed clips (devClipPeaks)
//   signal livetake-clips:<n>          · n live clips drawn at once (for waitfor: a loop's passes)
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame) {
  if (cmd != "liverec" || args.empty()) return false;
  const std::string& op = args[0];
  if (op == "arm" && args.size() >= 2) {
    const int mi = std::atoi(args[1].c_str());
    const bool mono = args.size() >= 3 && args[2] == "mono";
    if (mi > 0 && static_cast<size_t>(mi) < a.doc.song().mixer.size())
      a.doc.editMixerTrack(a.doc.song().mixer[static_cast<size_t>(mi)].id, "arm track for recording", [&](MixerTrack& m) {
        m.input = 0;
        m.inputStereo = !mono;
        m.armed = true;
      });
  } else if (op == "select" && args.size() >= 3) {
    const MeterMap mm(a.doc.song());  // K-193: bar lines of the song's meters
    a.playlist.selStart = mm.barStart(std::atoi(args[1].c_str()));
    a.playlist.selEnd = mm.barStart(std::atoi(args[2].c_str()));
  } else if (op == "start") {
    a.engine.setMode(PlayMode::Song);
    a.recordArmed = true;
    a.recAudio = true;
    a.startPlayback();
  } else if (op == "stop") {
    a.engine.stop();
  } else if (op == "watch") {
    st().watch = true;
  } else if (op == "trace") {
    devTrace(a, frame);
  } else if (op == "clips") {
    devClipPeaks(a, frame);
  }
  return true;
}

}  // namespace gl::livetake
