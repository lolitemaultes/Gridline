// Gridline — complete export. See ExportJob.h.
#include "ExportJob.h"
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <new>
#include "../core/Automation.h"
#include "../core/Crash.h"
#include "../core/MidiFile.h"
#include "Loudness.h"
#include "TrackReceiptPass.h"

namespace fs = std::filesystem;

namespace gl {

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
    if (m.pos < start || m.pos >= end || m.name.empty()) continue;
    out.push_back({static_cast<int64_t>(std::llround(map.samplesBetween(static_cast<double>(start), static_cast<double>(m.pos), job.sampleRate))),
                   m.name});
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

  void write(const std::string& stem, RenderResult&& r) {
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
      meta.meterNum = song.timeSigNum;
      meta.meterDen = song.timeSigDen;
      meta.loop = s.render.tail == TailMode::Wrap;
    }
    for (ExportFormat f : s.formats) {
      AudioFileOptions o;
      o.format = f;
      o.dither = s.dither;
      o.bits = f == ExportFormat::Flac ? s.flacBits : s.wavBits;
      o.bitrate = f == ExportFormat::Mp3 ? s.mp3Bitrate : f == ExportFormat::Ogg ? s.oggBitrate : s.m4aBitrate;
      o.cancel = cancel;
      if (f == ExportFormat::Wav) o.meta = meta;
      const std::string path = (fs::path(s.folder) / (stem + exportFormatExt(f))).string();
      std::string err;
      if (writeAudioFile(path, buf, s.render.sampleRate, o, &err)) {
        out.files.push_back(path);
        receipt::writeRenderOrigin(song, path);  // J-219: a later use of this render maps back (Track Receipt)
      } else {
        out.errors.push_back(fs::path(path).filename().string() + ": " + err);
      }
    }
  }
};

}  // namespace

namespace {

void runExportInto(ExportOutcome& out, const Song& song, const ExportSettings& s, const std::function<void(double, const std::string&)>& progress,
                   const std::atomic<bool>* cancel) {
  std::error_code ec;
  fs::create_directories(s.folder, ec);
  if (ec) {
    out.errors.push_back("cannot create folder " + s.folder);
    return;
  }
  Writer w{song, s, out, cancel};
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
    RenderResult r = renderAudio(song, job);
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
    w.write(expandNamePattern(s.partPattern, t), std::move(r));
    done += 1;
    return !(cancel && cancel->load());
  };
  if (!stems.empty()) {
    RenderJob sj = job;
    const double base0 = done;
    sj.progress = [&](double f) { report(f * static_cast<double>(stems.size()) - (done - base0), "Rendering mixer tracks"); };
    renderStems(song, sj, stems, s.stemsMasterFx, partSink);
  }
  if (!tracks.empty() && !(cancel && cancel->load())) {
    RenderJob tj = job;
    const double base0 = done;
    tj.progress = [&](double f) { report(f * static_cast<double>(tracks.size()) - (done - base0), "Rendering playlist tracks"); };
    renderPlaylistTracks(song, tj, tracks, partSink);
  }
  if (!groups.empty() && !(cancel && cancel->load())) {
    RenderJob gj = job;
    const double base0 = done;
    gj.progress = [&](double f) { report(f * static_cast<double>(groups.size()) - (done - base0), "Rendering playlist track groups"); };
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
