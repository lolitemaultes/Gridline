// Gridline — complete export. See ExportJob.h.
#include "ExportJob.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <new>
#include <set>
#include "../core/Arrangements.h"
#include "../core/Automation.h"
#include "../core/Crash.h"
#include "../core/Meter.h"
#include "../core/MidiFile.h"
#include "Freeze.h"
#include "Loudness.h"
#include "TrackReceiptPass.h"

namespace fs = std::filesystem;

namespace gl {
namespace {
bool hasFrozen(const Song& s) {
  for (const auto& c : s.channels)
    if (c.freeze.frozen()) return true;
  for (const auto& m : s.mixer)
    if (m.freeze.frozen()) return true;
  return false;
}
}  // namespace


MidiFileData exportScopeMidi(const Song& song, const RenderJob& job) {
  switch (job.scope) {
    case RenderScope::Pattern: return patternToMidi(song, job.pattern);
    case RenderScope::Range: return songRangeToMidi(song, job.start, job.end);
    default: return songToMidi(song);
  }
}

std::vector<WavMarker> scopeMarkers(const Song& song, const RenderJob& job, Tick start, Tick end) {
  std::vector<WavMarker> out;
  if (job.scope == RenderScope::Pattern) return out;
  TempoMap map;
  map.build(song);
  for (const auto& m : song.markers) {
    // K-179: a meter change is a cue too, named by its meter ("6/8", or "Verse (6/8)").
    std::string name = m.name;
    if (m.isMeter()) {
      const std::string meter = std::to_string(m.timeSigNum) + "/" + std::to_string(m.timeSigDen);
      name = name.empty() || name == meter ? meter : name + " (" + meter + ")";
    }
    if (m.pos < start || m.pos >= end || name.empty()) continue;
    out.push_back({static_cast<int64_t>(std::llround(map.samplesBetween(static_cast<double>(start), static_cast<double>(m.pos), job.sampleRate))),
                   name});
  }
  return out;
}

namespace {

struct Writer {
  const Song& song;
  const ExportSettings& s;
  ExportOutcome& out;
  const std::atomic<bool>* cancel;

  bool first = true;
  std::set<std::string> taken;  // folder-relative stems written so far (lower case: case-blind file systems)

  // A stem unique in its folder: the name itself, else "name 2", "name 3"...
  std::string unique(const std::string& sub, const std::string& stem) {
    auto key = [&](const std::string& n) {
      std::string k = sub + "/" + n;
      for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return k;
    };
    std::string n = stem;
    for (int k = 2; taken.count(key(n)); ++k) n = stem + " " + std::to_string(k);
    taken.insert(key(n));
    return n;
  }

  void write(const std::string& stem0, RenderResult&& r, const std::string& sub = {}) {
    const std::string stem = unique(sub, stem0);
    fs::path dir = fs::path(s.folder);
    if (!sub.empty()) {
      dir /= sub;
      std::error_code ec;
      fs::create_directories(dir, ec);
    }
    AudioBuffer buf = applyChannelMode(r.audio, s.channels);
    if (s.normalize && s.normalizeLoudness) {  // RN-09: loudness target, peak ceiling
      const LoudnessNormalization n = normalizeLoudness(buf, s.render.sampleRate, s.normalizeLufs, std::min(0.0, s.normalizeDb));
      if (first) out.ceilingLimited = n.ceilingLimited;
    } else if (s.normalize) {
      normalizeBuffer(buf, s.normalizeDb);
    }
    if (first) out.lufs = integratedLoudness(buf, s.render.sampleRate);
    first = false;
    WavMeta meta;
    if (s.markers) meta.markers = scopeMarkers(song, s.render, r.start, r.end);
    if (s.tempoInfo && s.render.scope != RenderScope::Range) {
      TempoMap map;
      map.build(song);
      meta.tempo = s.render.scope == RenderScope::Pattern ? song.tempo : map.bpmAt(0);
      meta.beats = static_cast<int>((r.end - r.start) * std::max(1, s.render.loops) / std::max<Tick>(1, song.ppq));
      // K-179: the meter where the render starts (the chunk holds one meter); a pattern's own meter.
      const MeterSeg g = MeterMap(song).segAt(r.start);
      meta.meterNum = g.num;
      meta.meterDen = g.den;
      if (s.render.scope == RenderScope::Pattern)
        if (const Pattern* p = song.pattern(s.render.pattern)) meta.meterNum = song.patternMeterNum(*p), meta.meterDen = song.patternMeterDen(*p);
      meta.loop = s.render.tail == TailMode::Wrap;
      // K-109: a loop export also carries the sampler loop ('smpl': the whole file loops), so
      // samplers that read it loop it seamlessly as they load it.
      if (meta.loop && buf.frames > 1) meta.loopStart = 0, meta.loopEnd = static_cast<int64_t>(buf.frames);
    }
    for (ExportFormat f : s.formats) {
      AudioFileOptions o;
      o.format = f;
      o.dither = s.dither;
      o.bits = f == ExportFormat::Flac ? s.flacBits : s.wavBits;
      o.bitrate = f == ExportFormat::Mp3 ? s.mp3Bitrate : f == ExportFormat::Ogg ? s.oggBitrate : f == ExportFormat::Opus ? s.opusBitrate : s.m4aBitrate;
      o.cancel = cancel;
      if (f == ExportFormat::Wav) o.meta = meta;
      const std::string path = (dir / (stem + exportFormatExt(f))).string();
      std::string err;
      if (writeAudioFile(path, buf, s.render.sampleRate, o, &err)) {
        out.files.push_back(path);
        try {  // K-307: the sidecar is a courtesy; nothing in it may stop the export
          receipt::writeRenderOrigin(song, path);  // J-219: a later use of this render maps back (Track Receipt)
        } catch (...) {
        }
      } else {
        out.errors.push_back(fs::path(path).filename().string() + ": " + err);
      }
    }
  }
};

}  // namespace

namespace {

void runExportInto(ExportOutcome& out, const Song& song, const ExportSettings& s, const std::function<void(double, const std::string&)>& progress,
                   const std::atomic<bool>* cancel);

void runExportOne(ExportOutcome& out, const Song& song, const ExportSettings& s, const std::function<void(double, const std::string&)>& progress,
                  const std::atomic<bool>* cancel) {
  std::error_code ec;
  fs::create_directories(s.folder, ec);
  if (ec) {
    out.errors.push_back("cannot create folder " + s.folder);
    return;
  }
  Writer w{song, s, out, cancel, true, {}};
  const std::string base = safeFileName(s.baseName);
  const bool audio = !s.formats.empty();
  // Work units for the progress bar: the mix + each part.
  std::vector<ObjectId> stems;
  std::vector<int> tracks;
  if (audio && s.splitMixer) stems = s.stemTracks.empty() ? usedInserts(song) : s.stemTracks;
  if (audio && s.splitPlaylist) tracks = usedPlaylistTracks(song);
  std::vector<TrackSet> groups;  // J-31: track groups
  if (audio && s.splitPlaylistGroups) groups = playlistTrackGroups(song);
  const bool mix = audio && (!(s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups) || s.stemsIncludeMaster);
  const double units = (mix ? 1.0 : 0.0) + static_cast<double>(stems.size()) + static_cast<double>(tracks.size()) +
                       static_cast<double>(groups.size());
  double done = 0;
  auto report = [&](double f, const std::string& what) {
    if (progress) progress(units > 0 ? std::min(1.0, (done + f) / units) : 1.0, what);
  };
  RenderJob job = s.render;
  job.cancel = cancel;
  if (mix) {
    job.progress = [&](double f) { report(f, "Rendering " + base); };
    // K-87: frozen units play their renders (no plugins load for them) when the file is at the
    // export's rate; otherwise, and for a pattern, they play live.
    Song played;
    const bool frozenView = job.scope != RenderScope::Pattern && hasFrozen(song);
    if (frozenView) {
      Song copy = song;
      for (auto& c : copy.channels)
        if (c.freeze.frozen() && c.freeze.sampleRate != job.sampleRate) c.freeze = {};
      for (auto& m : copy.mixer)
        if (m.freeze.frozen() && m.freeze.sampleRate != job.sampleRate) m.freeze = {};
      played = freeze::exportView(copy, job.frozen);
    }
    RenderResult r = renderAudio(frozenView ? played : song, job);
    job.frozen = {};
    if (r.cancelled) {
      out.cancelled = true;
      return;
    }
    if (!r.error.empty()) {
      out.errors.push_back("Nothing to render: " + r.error);
      return;
    }
    out.frames = static_cast<int64_t>(r.audio.frames);
    out.peak = r.audio.peak();
    w.write(base, std::move(r));
    done += 1;
  }
  std::map<char, std::string> tok = {{'P', s.projectTitle.empty() ? base : s.projectTitle},
                                     {'S', s.render.scope == RenderScope::Pattern ? "Pattern" : s.render.scope == RenderScope::Range ? "Range" : "Song"},
                                     {'R', std::to_string(s.render.sampleRate)}};
  // K-344: several kinds of split in one export each get their own subfolder (one kind alone: as before).
  const int kinds = (stems.empty() ? 0 : 1) + (tracks.empty() ? 0 : 1) + (groups.empty() ? 0 : 1);
  std::string sub;
  auto subFolder = [&](const std::string& name) { return kinds > 1 ? safeFileName(name) : std::string(); };
  auto partSink = [&](const SplitPart& p, RenderResult&& r) {
    if (out.frames == 0) {
      out.frames = static_cast<int64_t>(r.audio.frames);
      out.peak = r.audio.peak();
    }
    char num[8];
    std::snprintf(num, sizeof num, "%02d", p.index);
    auto t = tok;
    t['N'] = num;
    t['T'] = p.name;
    t['F'] = s.formats.empty() ? "" : exportFormatName(s.formats[0]);
    w.write(expandNamePattern(s.partPattern, t), std::move(r), sub);
    done += 1;
    return !(cancel && cancel->load());
  };
  if (!stems.empty()) {
    RenderJob sj = job;
    const double base0 = done;
    sj.progress = [&](double f) { report(f * static_cast<double>(stems.size()) - (done - base0), "Rendering mixer tracks"); };
    sub = subFolder(s.mixerFolder);
    renderStems(song, sj, stems, s.stemsMasterFx, partSink);
  }
  if (!tracks.empty() && !(cancel && cancel->load())) {
    RenderJob tj = job;
    const double base0 = done;
    tj.progress = [&](double f) { report(f * static_cast<double>(tracks.size()) - (done - base0), "Rendering playlist tracks"); };
    sub = subFolder(s.playlistFolder);
    renderPlaylistTracks(song, tj, tracks, partSink);
  }
  if (!groups.empty() && !(cancel && cancel->load())) {
    RenderJob gj = job;
    const double base0 = done;
    gj.progress = [&](double f) { report(f * static_cast<double>(groups.size()) - (done - base0), "Rendering playlist track groups"); };
    sub = subFolder(s.groupsFolder);
    renderPlaylistTrackSets(song, gj, groups, partSink);
  }
  if (cancel && cancel->load()) {
    out.cancelled = true;
    return;
  }
  if (s.midi) {
    MidiFileData m = exportScopeMidi(song, s.render);
    const std::string path = (fs::path(s.folder) / (base + ".mid")).string();
    std::string err;
    if (m.tracks.empty())
      out.errors.push_back("MIDI: no notes in the scope");
    else if (writeMidiFile(path, m, &err))
      out.files.push_back(path);
    else
      out.errors.push_back("MIDI: " + err);
  }
  if (progress) progress(1.0, "Done");
}

void runExportInto(ExportOutcome& out, const Song& song, const ExportSettings& s, const std::function<void(double, const std::string&)>& progress,
                   const std::atomic<bool>* cancel) {
  const int nArr = arrangementCount(song);
  if (s.render.scope != RenderScope::Pattern && s.arrangement == -2 && nArr > 1) {
    // K-406: export every arrangement (non-empty ones; if all are empty, report an error).
    std::vector<int> indices;
    for (int i = 0; i < nArr; ++i) {
      if (arrangementLengthTicks(song, i) > 0) indices.push_back(i);
    }
    if (indices.empty()) {
      out.errors.push_back("Nothing to render: every arrangement is empty");
      return;
    }
    const bool multiFolder = s.splitMixer || s.splitPlaylist || s.splitPlaylistGroups;
    for (size_t k = 0; k < indices.size(); ++k) {
      if (cancel && cancel->load()) {
        out.cancelled = true;
        return;
      }
      const int ai = indices[k];
      Song arrSong = songWithArrangement(song, ai);
      const std::string arrName = safeFileName(arrangementName(song, ai));
      ExportSettings sub = s;
      sub.arrangement = -1;
      if (multiFolder) {
        sub.folder = (fs::path(s.folder) / arrName).string();
      } else {
        sub.baseName = s.baseName + " - " + arrName;
      }
      sub.projectTitle = (s.projectTitle.empty() ? s.baseName : s.projectTitle) + " - " + arrName;
      auto subProgress = [&](double f, const std::string& what) {
        if (progress) {
          const double overall = (static_cast<double>(k) + f) / static_cast<double>(indices.size());
          progress(overall, arrName + ": " + what);
        }
      };
      ExportOutcome one;
      runExportOne(one, arrSong, sub, subProgress, cancel);
      if (out.frames == 0 && one.frames > 0) {
        out.frames = one.frames;
        out.peak = one.peak;
        out.lufs = one.lufs;
        out.ceilingLimited = one.ceilingLimited;
      }
      out.files.insert(out.files.end(), one.files.begin(), one.files.end());
      out.errors.insert(out.errors.end(), one.errors.begin(), one.errors.end());
      if (one.cancelled) {
        out.cancelled = true;
        return;
      }
    }
    if (progress) progress(1.0, "Done");
    return;
  }
  const int cur = song.arrangements.empty() ? 0 : song.currentArrangement;
  if (s.render.scope != RenderScope::Pattern && s.arrangement >= 0 && s.arrangement < nArr && s.arrangement != cur) {
    Song arrSong = songWithArrangement(song, s.arrangement);
    ExportSettings single = s;
    single.arrangement = -1;
    runExportOne(out, arrSong, single, progress, cancel);
    return;
  }
  runExportOne(out, song, s, progress, cancel);
}

}  // namespace

ExportOutcome runExport(const Song& song, const ExportSettings& s, const std::function<void(double, const std::string&)>& progress,
                        const std::atomic<bool>* cancel) {
  // An exception must not leave the render's thread (the app would end at once): a project too long
  // for the memory there is, a file system error on a strange name. It comes back as the export's
  // error, and as a crash report (one, here: the callers never see the exception). Short of memory,
  // writing that report can fail as well; the error is in the outcome by then.
  ExportOutcome out;
  auto report = []() noexcept {
    try {
      crash::reportCurrentException("Exporting audio");
    } catch (...) {
    }
  };
  try {
    runExportInto(out, song, s, progress, cancel);
  } catch (const std::bad_alloc&) {
    out.errors.push_back("not enough memory for this render (try a lower sample rate, no oversampling or fewer loops)");
    report();
  } catch (const std::exception& e) {
    out.errors.push_back(std::string("the render stopped: ") + e.what());
    report();
  } catch (...) {
    out.errors.push_back("the render stopped on an unknown error");
    report();
  }
  return out;
}

}  // namespace gl
