#include "HostedUi.h"
#include "McpUi.h"
#include "App.h"
#include "../core/NoteTools.h"
#include "../core/flp/Flp.h"
#include "../core/flp/FlpPresets.h"
#include "../core/Presets.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "../core/Demo.h"
#include "../core/I18n.h"
#include "../core/Keymap.h"
#include "../core/Log.h"
#include "ScriptUi.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../core/WavWriter.h"
#include "../engine/Render.h"
#include "ExportUi.h"
#include "../host/HostedPlugins.h"

namespace gl {

namespace fs = std::filesystem;


static App* gApp = nullptr;
App& app() { return *gApp; }

App::App() {
  gApp = this;
  std::string err;
  settings.load(Settings::defaultPath(), &err);
  BackupPolicy pol;
  pol.frequency = static_cast<BackupFrequency>(static_cast<int>(settings.getNumber("backup.frequency")));
  pol.maxCount = static_cast<int>(settings.getNumber("backup.maxCount"));
  pol.inProjectFolder = settings.getString("backup.location") == "project";
  pol.globalFolder = (fs::path(configDir()) / "Backup").string();
  backups = std::make_unique<BackupManager>(pol);
  doc.setMaxUndoLevels(static_cast<size_t>(std::max(1.0, settings.getNumber("undo.maxLevels"))));
  applySettingsHooks();
  // Recent files.
  std::ifstream rf(fs::path(configDir()) / "recent.txt");
  for (std::string line; std::getline(rf, line);)
    if (!line.empty()) recentFiles.push_back(line);
  newProject();
}

App::~App() {
  shutdownControllerScripts();  // onDeinit while the app still exists
  mcpui::shutdown();             // SC-07: stop the assistant server, remove its endpoint file
  presetImport.cancel = true;
  if (presetImport.worker.joinable()) presetImport.worker.join();
  settings.save(Settings::defaultPath());
  gApp = nullptr;
}

std::string App::configDir() const { return fs::path(Settings::defaultPath()).parent_path().string(); }

void App::addRecent(const std::string& path) {
  recentFiles.erase(std::remove(recentFiles.begin(), recentFiles.end(), path), recentFiles.end());
  recentFiles.insert(recentFiles.begin(), path);
  if (recentFiles.size() > 12) recentFiles.resize(12);
  std::error_code ec;
  fs::create_directories(configDir(), ec);
  std::ofstream wf(fs::path(configDir()) / "recent.txt");
  for (const auto& r : recentFiles) wf << r << "\n";
}

int App::processingThreads() const {
  int n = static_cast<int>(settings.getNumber("audio.threads"));
  if (n <= 0) {
    // Automatic: half the hardware threads (the rest serve the UI, hosted
    // plugin editors and the system), at most 8.
    const unsigned hw = std::thread::hardware_concurrency();
    n = std::clamp(static_cast<int>(hw / 2), 1, 8);
  }
  return std::clamp(n, 1, 16);
}

void App::syncEngine() {
  if (const int th = processingThreads(); th != engine.threads()) engine.setThreads(th);
  // Hosted plugins prepare for the engine's sample rate: rebuild when it
  // changes, when delay compensation is switched, and when a hosted plugin
  // reports a new latency.
  const bool pdc = settings.getBool("audio.pdc");
  // Stale hosted instances (restart requests) are polled ten times a second:
  // the check visits every plugin instance (J-25).
  static double staleChecked = -1;
  bool stale = false;
  if (now < staleChecked || now - staleChecked >= 0.1) {
    staleChecked = now;
    stale = builder.staleInstances();
  }
  // J-30: samples decode in the background (program builds never wait for
  // them); rebuild when some have finished — at most four times a second while
  // more are still loading — so they start to play.
  if (trimSamplesAt_ > 0 && now >= trimSamplesAt_) {  // a project was replaced: free the old one's samples
    sampleCache().trim();
    trimSamplesAt_ = -1;
  }
  const uint64_t sampleGen = sampleCache().generation();
  bool samplesReady = false;
  if (sampleGen != builtSampleGen_) {
    static double lastSampleBuild = -1;
    if (sampleCache().pending() == 0 || now < lastSampleBuild || now - lastSampleBuild >= 0.25) {
      samplesReady = true;
      lastSampleBuild = now;
    }
  }
  if (doc.revision() == builtRevision_ && builder.sampleRate() == engine.sampleRate() && builder.pdc() == pdc && !builder.latencyChanged() &&
      !stale && !samplesReady)
    return;
  builder.setSampleRate(engine.sampleRate());
  builder.setPdc(pdc);
  builder.setAsyncSamples(true);
  builtSampleGen_ = sampleGen;
  builtRevision_ = doc.revision();
  const Song& s = doc.song();
  // Keep the selection valid after undo/redo/deletes.
  if (!s.pattern(curPattern)) curPattern = s.patterns.empty() ? kNoId : s.patterns[0].id;
  if (!s.channel(selChannel)) selChannel = s.channels.empty() ? kNoId : s.channels[0].id;
  for (auto it = selChannels.begin(); it != selChannels.end();)
    it = s.channel(*it) ? std::next(it) : selChannels.erase(it);
  if (!s.mixerTrack(selMixer)) selMixer = s.masterId();
  if (!s.channel(settingsChannel)) settingsChannel = kNoId;
  if (fxEditor.first != kNoId) {
    const MixerTrack* t = s.mixerTrack(fxEditor.first);
    if (!t || fxEditor.second < 0 || fxEditor.second >= kEffectSlots || t->slots[static_cast<size_t>(fxEditor.second)].plugin.empty())
      fxEditor = {kNoId, -1};
  }
  for (auto it = playlist.selected.begin(); it != playlist.selected.end();)
    it = s.clip(*it) ? std::next(it) : playlist.selected.erase(it);
  engine.setProgram(builder.build(s));
  engine.setPattern(curPattern);
}

namespace {
// J-11: marker actions during song playback, checked once per frame: the
// first action marker crossed since the last frame pauses, stops, skips to the
// next marker or loops to it (through the time selection loop).
void runMarkerActions(App& a) {
  static double last = -1;
  const Song& s = a.doc.song();
  if (!a.engine.playing() || a.engine.mode() != PlayMode::Song || s.markers.empty()) {
    last = -1;
    return;
  }
  const double pos = a.engine.position();
  if (last >= 0 && pos > last && pos - last < static_cast<double>(s.barTicks()) * 8) {
    for (size_t i = 0; i < s.markers.size(); ++i) {
      const Marker& m = s.markers[i];
      if (m.action == MarkerAction::None || static_cast<double>(m.pos) <= last || static_cast<double>(m.pos) > pos) continue;
      switch (m.action) {
        case MarkerAction::Pause:
          a.engine.pause();
          a.engine.setPosition(m.pos, false);  // an automatic move: Stop still returns to the user's position
          break;
        case MarkerAction::Stop: a.engine.stop(); break;
        case MarkerAction::Skip:
          for (const auto& n : s.markers)
            if (n.pos > m.pos) {
              a.engine.setPosition(n.pos, false);
              break;
            }
          break;
        case MarkerAction::Loop: {
          auto r = ops::markerLoopRange(s, static_cast<int>(i));
          a.playlist.selStart = r.first;
          a.playlist.selEnd = r.second;
          a.engine.setLoopRange(r.first, r.second);
          break;
        }
        default: break;
      }
      break;
    }
  }
  last = a.engine.playing() ? a.engine.position() : -1;
}
}  // namespace

void App::tick(double t) {
  pollPresetImport();
  now = t;
  {  // J-48: recording start / stop and a user-started plugin scan finishing
    const bool rec = recordArmed && engine.playing() && !engine.countingIn() && engine.mode() != PlayMode::Live;
    if (rec != wasRecording_) uiSound(rec ? uisound::Event::RecordStart : uisound::Event::RecordStop);
    wasRecording_ = rec;
    const bool scanning = host::scanStatus().running;
    if (wasScanning_ && !scanning && scanByUser) {
      uiSound(uisound::Event::TaskDone);
      scanByUser = false;
    }
    wasScanning_ = scanning;
  }
  runMarkerActions(*this);
  engine.collectGarbage();
  toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& x) { return x.until < now; }), toasts.end());
  int64_t wall = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (wall - lastBackupCheck_ >= 5) {
    lastBackupCheck_ = wall;
    if (doc.modified() && backups->due(wall)) {
      std::string err;
      if (backups->write(doc.song(), projectPath, wall, &err).empty() && !err.empty()) notify("Backup failed: " + err, true);
    }
    // Autosave (J-24): saved projects are written back to their file.
    // Only Gridline files: an FL Studio project is re-exported on an explicit Save only.
    if (settings.getBool("backup.autosave") && !projectPath.empty() && projectFormat == SaveFormat::Gridline && doc.modified()) {
      if (lastAutosave_ == 0) lastAutosave_ = wall;
      const int64_t every = static_cast<int64_t>(std::clamp(settings.getNumber("backup.autosaveMinutes"), 1.0, 120.0) * 60.0);
      if (wall - lastAutosave_ >= every) {
        lastAutosave_ = wall;
        autosaving_ = true;
        if (saveProject(projectPath)) logging::info("app", "autosaved " + fs::path(projectPath).filename().string());
        autosaving_ = false;
      }
    } else {
      lastAutosave_ = wall;
    }
  }
}

bool App::uiSound(uisound::Event e) {
  if (!uiSoundsReady || uiSoundsDisabled) return false;
  uisound::Context c;
  c.enabled = settings.getBool("sounds.enabled");
  c.eventEnabled = settings.getBool(uisound::settingKey(e));
  c.transportPlaying = engine.playing();
  c.recording = recordArmed && engine.playing();
  c.audioInputRecording = audioRecSession;
  if (!uisound::shouldPlay(e, c) || !uiSoundGate.allow(e, now)) return false;
  uiSounds.setVolume(static_cast<float>(settings.getNumber("sounds.volume")));
  return uiSounds.trigger(e);
}

void App::notify(const std::string& text, bool error) {
  toasts.push_back({text, now + (error ? 6.0 : 3.0), error});
  if (error) uiSound(uisound::Event::Error);  // every failure is reported through here
  if (error)
    logging::error("app", text);
  else
    logging::info("app", text);
}

std::string App::windowTitle() const {
  std::string name = projectFile().empty() ? doc.song().info.title : fs::path(projectFile()).filename().string();
  return (doc.modified() ? "• " : "") + name + " — Gridline";
}

void App::newProject() {
  // A default template chosen in Settings > General replaces the starter kit.
  if (std::string key = settings.getString("general.defaultTemplate"); !key.empty()) {
    if (const TemplateInfo* t = findTemplate(key)) {
      Song s;
      std::string err;
      if (loadTemplate(t->path, s, &err)) {
        if (s.info.author.empty()) s.info.author = settings.getString("project.defaultAuthor");
        startProject(std::move(s));
        return;
      }
      notify("Could not open the default template: " + err, true);
    }
  }
  startProject(starterSong());
}

Song App::starterSong() const {
  Song s = makeEmptySong();
  applyProjectDefaults(s);
  Document doc(s);
  // Starter kit: four drum voices on inserts 1-4 and a synth on insert 5.
  struct Starter { const char* name; const char* type; uint32_t color; };
  const Starter kit[] = {{"Kick", "gl.kick", 0xD8775A}, {"Clap", "gl.clap", 0xC9A43C}, {"Hat", "gl.hat", 0x4FB8B0},
                         {"Snare", "gl.snare", 0x5A8FD8}, {"Synth", "gl.synth", 0xA56CD8}};
  int insert = 1;
  for (const auto& k : kit) {
    ObjectId id = doc.addChannel(k.name, k.type);
    ObjectId mt = doc.song().mixer[static_cast<size_t>(insert)].id;
    doc.editChannel(id, "", [&](Channel& c) {
      c.color = k.color;
      c.mixerTrack = mt;
    });
    doc.editMixerTrack(mt, "", [&](MixerTrack& m) {
      m.name = k.name;
      m.color = k.color;
    });
    ++insert;
  }
  return doc.song();
}

void App::applyProjectDefaults(Song& s) const {
  s.tempo = settings.getNumber("project.defaultTempo") > 0 ? settings.getNumber("project.defaultTempo") : 140.0;
  int ppq = static_cast<int>(settings.getNumber("project.defaultPpq"));
  if (isValidPpq(ppq)) s.ppq = ppq;
  const int num = static_cast<int>(settings.getNumber("project.defaultNum")), den = static_cast<int>(settings.getNumber("project.defaultDen"));
  if (num >= 1 && num <= 16) s.timeSigNum = num;
  if (den == 2 || den == 4 || den == 8 || den == 16) s.timeSigDen = den;
  s.info.author = settings.getString("project.defaultAuthor");
  const int bars = std::clamp(static_cast<int>(settings.getNumber("project.defaultPatternBars")), 1, 64);
  for (auto& p : s.patterns) p.bars = bars;
}

void App::startProject(Song s) {
  engine.stopToStart();
  engine.panic();
  doc.reset(std::move(s));  // the new project is the "saved" baseline, with no history
  trimSamplesAt_ = now + 1.0;  // J-30: the old project's samples leave the cache once its program is retired
  projectPath.clear();
  projectFormat = SaveFormat::Gridline;
  openedFrom.clear();
  const Song& song = doc.song();
  curPattern = song.patterns.empty() ? kNoId : song.patterns[0].id;
  selChannel = song.channels.empty() ? kNoId : song.channels[0].id;
  selChannels = {selChannel};
  selMixer = song.masterId();
  piano.selPattern = kNoId;
  playlist.selected.clear();
  engine.setMode(song.playlist.empty() ? PlayMode::Pattern : PlayMode::Song);
  syncEngine();
}

std::string App::templatesDir() const { return (fs::path(configDir()) / "templates").string(); }

const std::vector<TemplateInfo>& App::templates(bool rescan) {
  if (!templatesScanned_ || rescan) {
    std::vector<std::pair<std::string, bool>> roots;
    if (!assetsDir.empty()) roots.push_back({(fs::path(assetsDir) / "templates").string(), false});
    roots.push_back({templatesDir(), true});
    templates_ = scanTemplates(roots);
    templatesScanned_ = !assetsDir.empty();
  }
  return templates_;
}

const TemplateInfo* App::findTemplate(const std::string& key) {
  for (const auto& t : templates())
    if (t.category + "/" + t.name == key) return &t;
  return nullptr;
}

void App::applySettingsHooks() {
  logging::setMaxLevel(static_cast<logging::Level>(std::clamp(static_cast<int>(settings.getNumber("debug.logLevel")), 0, 3)));
  logging::setFile(settings.getBool("debug.logToFile") ? (fs::path(configDir()) / "gridline.log").string() : std::string());
  std::string err;
  if (!i18n::setLanguage(settings.getString("general.language"), languageDir(), &err)) logging::warning("app", err);
  if (!keys::keymap().load(settings.getString("keys.bindings"), &err)) logging::warning("app", err);
}

std::string App::languageDir() const { return assetsDir.empty() ? std::string() : (fs::path(assetsDir) / "i18n").string(); }

void App::applyStartup() {
  applySettingsHooks();
  templates(true);
  const std::string mode = settings.getString("general.startup");
  if (mode == "empty") {
    newFromTemplate("");
  } else if (mode == "last" && !recentFiles.empty()) {
    std::error_code ec;
    if (fs::exists(recentFiles.front(), ec)) openProject(recentFiles.front());
  } else if (!settings.getString("general.defaultTemplate").empty()) {
    newProject();
  }
}

bool App::openProject(const std::string& path) {
  // J-52: FL Studio projects and zipped projects open as projects; MIDI and
  // audio files are imported into the open project.
  switch (openKindOf(path)) {
    case OpenKind::FlProject: return openFlpProject(path, path);
    case OpenKind::ZippedProject: return openZippedProject(path);
    case OpenKind::Midi:
    case OpenKind::Audio: return exportui::openAny(*this, path);
    case OpenKind::GridlineProject:
    case OpenKind::Unknown: break;
  }
  Song s;
  std::string err;
  if (!loadProject(path, s, &err)) {
    notify("Could not open project: " + err, true);
    return false;
  }
  engine.stopToStart();
  engine.panic();
  doc.reset(std::move(s));
  trimSamplesAt_ = now + 1.0;  // J-30
  projectPath = path;
  projectFormat = SaveFormat::Gridline;
  openedFrom.clear();
  addRecent(path);
  curPattern = doc.song().patterns.empty() ? kNoId : doc.song().patterns[0].id;
  selChannel = doc.song().channels.empty() ? kNoId : doc.song().channels[0].id;
  selChannels = {selChannel};
  selMixer = doc.song().masterId();
  playlist.selected.clear();
  engine.setMode(doc.song().playlist.empty() ? PlayMode::Pattern : PlayMode::Song);
  syncEngine();
  notify("Opened " + fs::path(path).filename().string());
  uiSound(uisound::Event::Open);
  return true;
}

bool App::openZippedProject(const std::string& zip) {
  // Unpacked beside the archive (not a temporary folder): the samples stay
  // where the opened project points, whatever it is saved as later.
  std::string proj, err;
  const fs::path zp(zip);
  if (!unpackZippedProject(zip, (zp.parent_path() / projectStemOf(zip)).string(), proj, &err)) {
    notify("Cannot open the zipped project: " + err, true);
    return false;
  }
  bool ok;
  if (openKindOf(proj) == OpenKind::FlProject) {
    ok = openFlpProject(proj, zip);
  } else {
    ok = openProject(proj);  // a Gridline zip: the unpacked project is the project file
  }
  if (ok && !missingFiles(doc.song()).empty()) exportui::openMissingSamples(*this);
  return ok;
}

bool App::openFlpProject(const std::string& path, const std::string& shownAs) {
  Song s;
  flp::Report rep;
  std::string err;
  if (!importFlp(path, s, rep, &err, settings.getString("import.flData"))) {
    notify("Cannot open the FL Studio project: " + err, true);
    return false;
  }
  engine.stopToStart();
  engine.panic();
  doc.reset(std::move(s));
  trimSamplesAt_ = now + 1.0;  // J-30
  // Not a Gridline file yet: Save asks where and in which format, so the
  // .flp is never replaced by a different format (or a lossy re-export) unasked.
  projectPath.clear();
  projectFormat = SaveFormat::Gridline;
  openedFrom = shownAs;
  addRecent(shownAs);
  curPattern = doc.song().patterns.empty() ? kNoId : doc.song().patterns[0].id;
  selChannel = doc.song().channels.empty() ? kNoId : doc.song().channels[0].id;
  selChannels = {selChannel};
  selMixer = doc.song().masterId();
  playlist.selected.clear();
  engine.setMode(doc.song().playlist.empty() ? PlayMode::Pattern : PlayMode::Song);
  syncEngine();
  importReport = ImportReport{};
  importReport.source = fs::path(shownAs).filename().string();
  for (const auto& [k, v] : rep.counts) importReport.counts.emplace_back(k, v);
  for (const auto& [k, v] : rep.unmapped) importReport.unmapped.emplace_back(k, v);
  importReport.warnings = rep.warnings;
  showImportReport = true;
  notify("Opened " + importReport.source);
  // J-48: opening a project sounds like opening one; warnings or unmapped plugins: the report needs a look.
  uiSound(importReport.warnings.empty() && importReport.unmapped.empty() ? uisound::Event::Open : uisound::Event::Warning);
  return true;
}

void App::startPresetImport() {
  if (presetImport.running) return;
  if (presetImport.worker.joinable()) presetImport.worker.join();
  presetImport.cancel = false;
  presetImport.finished = false;
  presetImport.done = 0;
  presetImport.total = 0;
  presetImport.running = true;
  const std::string data = settings.getString("import.flData"), root = presetRoot(configDir());
  presetImport.worker = std::thread([this, data, root]() {
    auto rep = flp::importPresetLibrary(data, root, [this](size_t done, size_t total, const std::string&) {
      presetImport.done = done;
      presetImport.total = total;
      return !presetImport.cancel.load();
    });
    std::string s;
    if (!rep.error.empty()) {
      s = rep.error;
    } else {
      s = std::to_string(rep.converted) + " presets converted (" + std::to_string(rep.exact) + " exact), " + std::to_string(rep.chains) +
          " effect chains";
      if (rep.shapes) s += ", " + std::to_string(rep.shapes) + " automation shapes";
      if (rep.failed) s += ", " + std::to_string(rep.failed) + " without a Gridline equivalent yet";
      if (rep.cancelled) s += " (cancelled)";
    }
    presetImport.summary = s;
    presetImport.finished = true;
    presetImport.running = false;
  });
}

void App::pollPresetImport() {
  if (presetImport.finished.exchange(false)) {
    if (presetImport.worker.joinable()) presetImport.worker.join();
    notify("FL Studio presets: " + presetImport.summary);
    uiSound(uisound::Event::TaskDone);
  }
}

bool App::saveProject(const std::string& path) { return saveProjectAs(path, saveFormatOf(path)); }

bool App::saveProjectAs(const std::string& path, SaveFormat format) {
  captureHostedStates(*this);  // third-party plugin state into the song
  std::string err;
  std::string p = path;
  if (format != SaveFormat::Gridline || fs::path(p).extension().empty()) p = withSaveExtension(p, format);
  switch (format) {
    case SaveFormat::Gridline:
      if (!gl::saveProject(p, doc.song(), &err)) {
        notify("Save failed: " + err, true);
        return false;
      }
      break;
    case SaveFormat::Flp:
      if (!exportui::writeFlpFile(*this, p)) return false;
      break;
    case SaveFormat::FlZip:
      if (!exportui::writeFlpZip(*this, p)) return false;
      break;
  }
  projectPath = p;
  projectFormat = format;
  openedFrom.clear();
  doc.markSaved();
  addRecent(p);
  notify("Saved " + fs::path(p).filename().string());
  if (!autosaving_) uiSound(uisound::Event::Save);
  return true;
}

static std::string homeDir() {
  const char* h = std::getenv("HOME");
  return h ? h : ".";
}

void App::save() {
  if (projectPath.empty())
    saveAs();
  else
    saveProjectAs(projectPath, projectFormat);  // the format the user chose for this file
}

void App::saveAs(std::function<void()> then) {
  // J-52: one list of formats, like FL's Save as: Gridline project, FL Studio
  // project, FL Studio zipped loop package. A project opened from an FL file
  // starts in its folder, under its name, as a Gridline project.
  const std::string ref = projectFile();
  const SaveFormat fmt = projectPath.empty() ? SaveFormat::Gridline : projectFormat;
  fileDialog = {};
  fileDialog.open = true;
  fileDialog.save = true;
  fileDialog.title = "Save project as";
  fileDialog.dir = ref.empty() ? homeDir() : fs::path(ref).parent_path().string();
  fileDialog.types = saveFileTypes();
  fileDialog.typeIndex = static_cast<int>(fmt);
  fileDialog.exts = fileDialog.types[static_cast<size_t>(fmt)].exts;
  fileDialog.name = withSaveExtension(ref.empty() ? doc.song().info.title : fs::path(ref).filename().string(), fmt);
  fileDialog.nameForType = [](const std::string& n, int t) { return withSaveExtension(n, static_cast<SaveFormat>(t)); };
  fileDialog.onOk = [this, then](const std::string& p) {
    const auto f = static_cast<SaveFormat>(std::clamp(fileDialog.typeIndex, 0, 2));
    if (saveProjectAs(withSaveExtension(p, f), f) && then) then();
  };
}

void App::openDialog() {
  guardUnsaved([this]() {
    fileDialog = {};
    fileDialog.open = true;
    fileDialog.title = "Open project";
    const std::string ref = projectFile();
    fileDialog.dir = ref.empty() ? homeDir() : fs::path(ref).parent_path().string();
    fileDialog.types = openFileTypes();  // all projects, then Gridline / FL Studio / zipped
    fileDialog.exts = fileDialog.types[0].exts;
    fileDialog.onOk = [this](const std::string& p) { openProject(p); };
  });
}

void App::guardUnsaved(std::function<void()> action) {
  if (!doc.modified()) {
    action();
    return;
  }
  afterDiscard = std::move(action);
  confirmDiscard = true;
  uiSound(uisound::Event::Warning);
}

void App::exportAudio(const std::string& path, int bits, int sampleRate, bool patternOnly, bool normalize) {
  captureHostedStates(*this);  // offline instances load the plugins' current state
  RenderOptions o;
  o.sampleRate = sampleRate;
  o.tail = doc.song().barTicks();
  o.patternMode = patternOnly;
  o.pattern = curPattern;
  AudioBuffer buf = renderSong(doc.song(), o);
  if (buf.frames == 0) {
    notify("Nothing to export: the " + std::string(patternOnly ? "pattern" : "playlist") + " is empty", true);
    return;
  }
  // Trim trailing silence from the tail.
  size_t last = 0;
  for (size_t i = 0; i < buf.frames; ++i)
    if (std::fabs(buf.at(0, i)) > 1e-5f || std::fabs(buf.at(1, i)) > 1e-5f) last = i;
  float peak = buf.peak();
  if (normalize && peak > 0) {
    float g = 0.98f / peak;
    for (auto& v : buf.data) v *= g;
  }
  AudioBuffer out(buf.channels, std::max(last + 1, static_cast<size_t>(1)));
  for (size_t c = 0; c < buf.channels; ++c) std::copy(buf.plane(c), buf.plane(c) + out.frames, out.plane(c));
  std::string p = path;
  if (fs::path(p).extension().empty()) p += ".wav";
  if (!writeWav(p, out, sampleRate, bits)) {
    notify("Export failed: cannot write " + p, true);
    return;
  }
  char msg[256];
  std::snprintf(msg, sizeof msg, "Exported %s (%.1f s)%s", fs::path(p).filename().string().c_str(),
                static_cast<double>(out.frames) / sampleRate, !normalize && peak > 1.0f ? " — clipped, try Normalize" : "");
  notify(msg, !normalize && peak > 1.0f);
  if (normalize || peak <= 1.0f) uiSound(uisound::Event::Export);
}

ObjectId App::addInstrument(const std::string& pluginType, const std::string& samplePath) {
  const PluginInfo* info = findPlugin(pluginType);
  if (!info) return kNoId;
  std::string name = info->name;
  if (!samplePath.empty()) name = fs::path(samplePath).stem().string();
  int index = doc.song().channelIndex(selChannel);
  doc.beginGroup("add channel");
  ObjectId id = doc.addChannel(name, pluginType, index >= 0 ? index + 1 : -1);
  if (!samplePath.empty()) doc.editChannel(id, "", [&](Channel& c) { c.plugin.data = samplePath; });
  doc.endGroup();
  selectChannel(id);
  return id;
}

void App::selectPattern(ObjectId id) {
  if (!doc.song().pattern(id)) return;
  curPattern = id;
  engine.setPattern(id);
}

void App::selectChannel(ObjectId id, bool additive) {
  if (!doc.song().channel(id)) return;
  selChannel = id;
  if (!additive) selChannels.clear();
  if (additive && selChannels.count(id) && selChannels.size() > 1)
    selChannels.erase(id);
  else
    selChannels.insert(id);
}

void App::openPianoRoll(ObjectId channel) {
  selectChannel(channel);
  showPiano = true;
  focusWindow = "Piano roll";
}

void App::previewSample(const std::string& path) {
  SamplePtr s = sampleCache().get(path);
  if (!s) {
    notify("Cannot decode " + fs::path(path).filename().string(), true);
    return;
  }
  engine.preview(s);
}

void App::stopPreview() { engine.preview(nullptr); }

}  // namespace gl

// ---------------------------------------------------------------- MIDI ----
#include "../core/MidiFile.h"
#include "ExportUi.h"

namespace gl {

void App::importMidi(const std::string& path) { exportui::importMidiFile(*this, path); }

void App::exportMidi(const std::string& path, bool patternOnly) {
  MidiFileData m = patternOnly ? patternToMidi(doc.song(), curPattern) : songToMidi(doc.song());
  if (m.tracks.empty()) {
    notify("Nothing to export: no notes", true);
    return;
  }
  std::string p = path, err;
  if (std::filesystem::path(p).extension().empty()) p += ".mid";
  if (writeMidiFile(p, m, &err)) {
    notify("Exported MIDI " + std::filesystem::path(p).filename().string());
    uiSound(uisound::Event::Export);
  } else {
    notify("MIDI export failed: " + err, true);
  }
}

// ---- menu actions --------------------------------------------------------------

namespace {
// "song.gridline.json" -> "song"; other extensions are stripped once.
std::string projectStem(const fs::path& p) {
  std::string n = p.filename().string();
  const std::string ext = ".gridline.json";
  if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) return n.substr(0, n.size() - ext.size());
  return p.stem().string();
}
}  // namespace

void App::saveNewVersion() {
  if (projectPath.empty()) {
    saveAs();
    return;
  }
  fs::path cur(projectPath);
  std::string stem = projectStem(cur);
  // Strip a trailing " N" version number.
  int ver = 1;
  size_t sp = stem.find_last_of(' ');
  if (sp != std::string::npos && sp + 1 < stem.size() &&
      stem.find_first_not_of("0123456789", sp + 1) == std::string::npos) {
    ver = std::atoi(stem.c_str() + sp + 1);
    stem = stem.substr(0, sp);
  }
  fs::path next;
  do {
    ++ver;
    next = cur.parent_path() / withSaveExtension(stem + " " + std::to_string(ver), projectFormat);
  } while (fs::exists(next));
  saveProjectAs(next.string(), projectFormat);
}

bool App::revertToLastBackup() {
  auto list = backups->list(projectPath);
  if (list.empty()) {
    notify("No backup of this project yet", true);
    return false;
  }
  Song s;
  std::string err;
  if (!loadProject(list.back(), s, &err)) {
    notify("Could not read the backup: " + err, true);
    return false;
  }
  engine.stopToStart();
  doc.editSong("revert to last backup", [&s](Song& x) { x = s; });
  notify("Reverted to " + fs::path(list.back()).filename().string());
  return true;
}

void App::newFromTemplate(const std::string& which) {
  if (which == "starter") {
    startProject(starterSong());
    return;
  }
  Song s;
  if (which.empty()) {
    s = makeEmptySong();
    applyProjectDefaults(s);
  } else if (which == "demo") {
    s = makeDemoSong();
  } else {
    std::string err;
    if (!loadTemplate(which, s, &err)) {
      notify("Could not open template: " + err, true);
      return;
    }
    if (s.info.author.empty()) s.info.author = settings.getString("project.defaultAuthor");
  }
  startProject(std::move(s));
}

std::vector<std::string> App::templateFiles() const {
  std::vector<std::string> out;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(configDir()) / "templates", ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".json") out.push_back(it->path().string());
  std::sort(out.begin(), out.end());
  return out;
}

bool App::saveAsTemplate(const std::string& name, const std::string& category, const std::string& description) {
  std::string err;
  captureHostedStates(*this);
  if (!saveTemplate(templatesDir(), category, name, description, doc.song(), nullptr, &err)) {
    notify("Could not save template: " + err, true);
    return false;
  }
  templates(true);
  notify("Saved template " + name);
  return true;
}

int App::exportStems(const std::string& dir, int bits, int sampleRate) {
  captureHostedStates(*this);
  const Song& song = doc.song();
  std::error_code ec;
  fs::create_directories(dir, ec);
  int files = 0;
  for (size_t i = 1; i < song.mixer.size(); ++i) {
    ObjectId id = song.mixer[i].id;
    bool fed = false;
    for (const auto& c : song.channels) fed = fed || c.mixerTrack == id;
    for (const auto& m : song.mixer)
      for (const auto& snd : m.sends) fed = fed || snd.to == id;
    if (!fed) continue;
    Song solo = song;
    for (auto& m : solo.mixer) m.solo = m.id == id;
    RenderOptions o;
    o.sampleRate = sampleRate;
    o.tail = song.barTicks();
    AudioBuffer buf = renderSong(solo, o);
    if (buf.frames == 0 || buf.peak() < 1e-6f) continue;
    char num[8];
    std::snprintf(num, sizeof num, "%02zu", i);
    std::string name = song.info.title + " - " + num + " " + mixerTrackName(song, i) + ".wav";
    for (char& c : name)
      if (c == '/' || c == '\\' || c == ':') c = '-';
    if (writeWav((fs::path(dir) / name).string(), buf, sampleRate, bits)) ++files;
  }
  notify(files ? "Exported " + std::to_string(files) + " stem(s) to " + dir : std::string("Nothing to export: no insert track makes sound"),
         files == 0);
  return files;
}

void App::closeAllWindows() {
  showBrowser = showRack = showPiano = showPlaylist = showMixer = false;
  showSettings = showInfo = showHistory = showAbout = showExport = showShortcuts = showPluginManager = showPerformance = false;
  closePluginWindows();
}

void App::closePluginWindows() {
  closeAllHostedEditors(*this);
  settingsChannel = kNoId;
  fxEditor = {kNoId, -1};
}

}  // namespace gl
