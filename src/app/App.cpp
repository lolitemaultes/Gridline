#include "MidiUi.h"
#include "DisplayConfirm.h"
#include "../core/Platform.h"
#include "../core/DawProject.h"
#include "HostedUi.h"
#include "LogicPlugins.h"
#include "McpUi.h"
#include "OscUi.h"
#include "App.h"
#include "AutomationEditor.h"
#include "StageView.h"
#include "RecordRowsUi.h"
#include "../core/ChromeOs.h"
#include "LayoutPresets.h"
#include "LiveLook.h"
#include "WorkspacesUi.h"
#include "../core/SelectionFollow.h"
#include "../core/TrackModes.h"
#include "../core/LayoutShell.h"
#include "../core/Crash.h"
#include "../core/RemoteFs.h"
#include "../core/ProjectArchive.h"
#include "FreezeUi.h"
#include "CacheUi.h"
#include "CollabUi.h"
#include "../collab/Controller.h"
#include "../core/NoteTools.h"
#include "../core/flp/Flp.h"
#include "../core/flp/FlpPresets.h"
#include "../core/PluginStandIn.h"
#include "../core/ExternalDrop.h"
#include "../core/Presets.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "../core/Demo.h"
#include "../core/TrackerDemo.h"
#include "../core/MenuParity.h"
#include "../core/ProjectPreview.h"
#include "../core/Recents.h"
#include "LauncherUi.h"
#include "../core/I18n.h"
#include "../core/Keymap.h"
#include "../core/Log.h"
#include "ScriptUi.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"
#include "../core/WavWriter.h"
#include "../engine/KerfGrid.h"
#include "../engine/Render.h"
#include "../engine/TrackReceiptPass.h"
#include "ExportUi.h"
#include "FlpExportUi.h"
#include "ToastText.h"
#include "../host/HostedPlugins.h"
#include "UndoUi.h"

namespace gl {

namespace fs = std::filesystem;


static App* gApp = nullptr;
App& app() { return *gApp; }
App* setCurrentApp(App* a) {
  App* prev = gApp;
  gApp = a;
  return prev;
}

App::App() {
  gApp = this;
  std::string err;
  {
    std::error_code ec;
    freshInstall_ = !fs::exists(Settings::defaultPath(), ec);
  }
  // K-75: on a Chromebook (or the Android Linux Terminal) a new installation starts with the audio
  // settings its sound server runs at; saved settings still win.
  chromeos::applyDefaults(settings, chromeos::current());
  settings.load(Settings::defaultPath(), &err);
  loadTypingLayout(settings);  // K-113
  typingKeyboard = typingKeyboardAtStart(settings);  // on, off, or as it was left
  loadRecordOptions(settings, recOptions, countInBars, recPunch);  // K-226: as they were left
  bindAutomationArm(&recOptions.automation);  // N58-01: one automation arm, kept with the record options
  recAudio = settings.getBool("record.audio");
  recAudioEditor = settings.getBool("record.audioEditor");
  if (const std::string sys = chromeos::describe(chromeos::current()); !sys.empty()) logging::info("app", "Running in the " + sys);
  // K-112b: the Tracks layout now starts with its library and right pane closed. Someone who has
  // used the layout had them open by the old default without that being stored: keep them open
  // (unless they had closed them), once.
  if (!settings.getBool("layout.logic.panesKept")) {
    if (settings.getNumber("ui.layoutVersion.logic.shell") > 0 || settings.getNumber("ui.layoutVersion.logic") > 0) {
      const layout::Shell& sh = layout::shell(layout::Preset::Logic);
      layout::State st = layout::fromJson(layout::Preset::Logic, settings.getString(layout::stateKey(layout::Preset::Logic)));
      for (const char* r : {"logic.library", "logic.right"})
        if (!st.hidden.count(r)) layout::setVisible(sh, st, r, true);
      st.hidden.erase("logic.library");
      st.hidden.erase("logic.right");
      settings.set(layout::stateKey(layout::Preset::Logic), layout::toJson(st));
    }
    settings.set("layout.logic.panesKept", true);
  }
  // K-112B (checkpoint 40): the Tracks layout now starts with its editors pane closed (the tracks get
  // the full height, as a new project in its reference opens). As above, someone who already used
  // the layout keeps the pane open unless they had closed it, once.
  if (!settings.getBool("layout.logic.lowerKept")) {
    if (settings.getNumber("ui.layoutVersion.logic.shell") > 0 || settings.getNumber("ui.layoutVersion.logic") > 0) {
      const layout::Shell& sh = layout::shell(layout::Preset::Logic);
      layout::State st = layout::fromJson(layout::Preset::Logic, settings.getString(layout::stateKey(layout::Preset::Logic)));
      if (!st.hidden.count("logic.lower")) layout::setVisible(sh, st, "logic.lower", true);
      st.hidden.erase("logic.lower");
      settings.set(layout::stateKey(layout::Preset::Logic), layout::toJson(st));
    }
    settings.set("layout.logic.lowerKept", true);
  }
  {  // K-24: the playlist's vertical zoom as it was left
    const double h = settings.getNumber("ui.playlistTrackHeight");
    if (std::isfinite(h)) playlist.trackHeight = static_cast<float>(std::clamp(h, 24.0, 120.0));
    // K-112b: a height saved before the flag existed was the person's own if it is not the old default.
    if (std::isfinite(h) && std::fabs(h - 46.0) > 1e-3) settings.set("ui.playlistTrackHeightSet", true);
  }
  BackupPolicy pol;
  pol.frequency = static_cast<BackupFrequency>(static_cast<int>(settings.getNumber("backup.frequency")));
  pol.maxCount = static_cast<int>(settings.getNumber("backup.maxCount"));
  pol.inProjectFolder = settings.getString("backup.location") == "project";
  pol.globalFolder = (fs::path(configDir()) / "Backup").string();
  backups = std::make_unique<BackupManager>(pol);
  doc.setMaxUndoLevels(static_cast<size_t>(std::clamp(settings.getNumber("undo.maxLevels"), static_cast<double>(UndoStack::kMinLevels),
                                                       static_cast<double>(UndoStack::kMaxLevels))));
  doc.setMaxUndoBytes(UndoStack::kDefaultMaxBytes);  // K-141
  undoui::install(*this);
  applySettingsHooks();
  // Recent files (J-141: with the times they were last opened, for the launcher).
  {
    recents::List rl = recents::load(configDir());
    putRecents(std::move(rl));
  }
  newProject();
}

App::~App() {
  bindAutomationArm(nullptr);
  launcherui::shutdown();        // J-141: the sketch worker
  cacheui::shutdown(*this);      // K-45: the project's unpacked copy can go
  collabui::closeSession(*this);  // J-111: the log is synced, collaborators are told
  shutdownControllerScripts();  // onDeinit while the app still exists
  mcpui::shutdown();             // SC-07: stop the assistant server, remove its endpoint file
  oscui::shutdown();             // OSC server, notes it holds
  presetImport.cancel = true;
  if (presetImport.worker.joinable()) presetImport.worker.join();
  flpexport::shutdown();         // J-80: a running FL export is cancelled and waited for
  // J-77: a size / theme / language / audio change still waiting for Keep is not saved.
  dispconf::confirm().abandon();
  settings.set("keys.typingOn", typingKeyboard);  // for "as it was left" at the next start
  // K-226; checkpoint 56: Clips' Punch is its own (clips.punch), so leaving from Clips keeps record.punch as it was.
  saveRecordOptions(settings, recOptions, countInBars, livelook::active(*this) ? settings.getBool("record.punch") : recPunch);
  settings.set("record.audio", recAudio);
  settings.set("record.audioEditor", recAudioEditor);
  settings.save(Settings::defaultPath());
  if (gApp == this) gApp = nullptr;
}

std::string App::configDir() const { return fs::path(Settings::defaultPath()).parent_path().string(); }

static int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void App::saveRecents() {
  if (embedded) return;  // PH-09: the host owns the project
  recents::List l;
  l.paths = recentFiles;
  l.opened = recentTimes_;
  l.favourites = recentFavourites_;
  recents::save(configDir(), l);
}

recents::List App::takeRecents() {
  recents::List l;
  l.paths = std::move(recentFiles);
  l.opened = std::move(recentTimes_);
  l.favourites = std::move(recentFavourites_);
  recentFiles.clear(), recentTimes_.clear(), recentFavourites_.clear();
  return l;
}

void App::putRecents(recents::List&& l) {
  recentFiles = std::move(l.paths);
  recentTimes_ = std::move(l.opened);
  recentFavourites_ = std::move(l.favourites);
}

void App::addRecent(const std::string& path) {
  recents::List l = takeRecents();
  l.touch(path, unixNow());
  putRecents(std::move(l));
  saveRecents();
}

bool App::removeRecent(const std::string& path) {
  recents::List l = takeRecents();
  const bool ok = l.remove(path);
  putRecents(std::move(l));
  if (ok) saveRecents();
  return ok;
}

size_t App::removeMissingRecents() {
  recents::List l = takeRecents();
  // K-42: a project on a drive that is offline right now has not gone: it stays listed.
  const size_t n = l.pruneMissing([](const std::string& p) { return remotefs::isRemote(p) || recents::exists(p); });
  putRecents(std::move(l));
  if (n) saveRecents();
  return n;
}

void App::clearRecents() {
  recents::List l = takeRecents();
  l.clearKeepingFavourites();
  putRecents(std::move(l));
  saveRecents();
}

void App::moveRecent(const std::string& oldPath, const std::string& newPath) {
  recents::List l = takeRecents();
  const bool fav = l.isFavourite(oldPath);
  l.remove(oldPath);
  l.touch(newPath, unixNow());
  if (fav) l.setFavourite(newPath, true);
  putRecents(std::move(l));
  saveRecents();
}

bool App::setFavouriteProject(const std::string& path, bool on) {
  recents::List l = takeRecents();
  const bool changed = l.setFavourite(path, on);
  putRecents(std::move(l));
  if (changed) saveRecents();
  return changed;
}

int64_t App::recentOpenedAt(const std::string& path) const {
  const auto it = recentTimes_.find(path);
  return it == recentTimes_.end() ? 0 : it->second;
}

std::string App::sketchCacheDir() const { return (fs::path(configDir()) / "launcher" / "sketches").string(); }

void App::rememberSketch(const std::string& path) {
  if (embedded || path.empty()) return;
  launcherui::rememberSketch(*this, path, preview::make(doc.song()));
}

void App::closeProject() {
  guardUnsaved([this]() {
    newProject();
    home.projectClosed(now);
  });
}

int App::processingThreads() const {
  if (embedded) return 1;  // PH-09: the host spreads its plugins over its own threads
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
  engine.setRestInstruments(settings.getBool("freeze.restSilent"));  // K-277
  {  // K-277: an instrument whose own editor is open never rests (the editor may play notes itself)
    static std::set<ObjectId> was;
    std::set<ObjectId> open;
    for (const PluginTarget& t : openHostedEditorTargets())
      if (t.isChannel()) open.insert(t.owner);
    for (ObjectId id : was)
      if (!open.count(id))
        if (auto rt = builder.channelRuntime(id)) rt->editorOpen.store(false, std::memory_order_relaxed);
    for (ObjectId id : open)  // (every frame: a rebuild may have made a new runtime)
      if (auto rt = builder.channelRuntime(id)) rt->editorOpen.store(true, std::memory_order_relaxed);
    was = std::move(open);
  }
  stretch::setRenderFocus(engine.position());  // K-64: the renders about to play first
  // Hosted plugins prepare for the engine's sample rate: rebuild when it
  // changes, when delay compensation is switched, and when a hosted plugin
  // reports a new latency.
  const bool pdc = settings.getBool("audio.pdc");
  engine.setMonitorInputOnly(settings.getBool("record.monitor.inputOnly"));  // K-123 (relaxed stores)
  engine.setLowLatencyMonitoring(settings.getBool("record.monitor.lowLatency"));
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
  // J-190: a finished beat analysis also rebuilds (Kerf's beat-grid slices were provisional).
  const uint64_t sampleGen = sampleCache().generation() + kerfgrid::analysisGeneration();
  bool samplesReady = false;
  if (sampleGen != builtSampleGen_) {
    static double lastSampleBuild = -1;
    if (sampleCache().pending() == 0 || now < lastSampleBuild || now - lastSampleBuild >= 0.25) {
      samplesReady = true;
      lastSampleBuild = now;
    }
  }
  // J-196: while a pointer gesture edits (a knob, a field or a handle held, a
  // wheel turn) builds start no clip renders; they start once it is over.
  const bool editing = doc.inPointerGesture();
  const bool settled = builder.deferredRenders() && !editing;
  const bool engineChanged = builder.sampleRate() != engine.sampleRate() || builder.pdc() != pdc || builder.latencyChanged() || stale ||
                             samplesReady || builder.voicesPending();
  if (doc.revision() == builtRevision_ && !engineChanged && !settled) return;
  // J-196: only clip levels changed (a volume or fade drag, on one clip or a
  // selection): written into the playing program, no build.
  if (!engineChanged && !settled && doc.clipLevelsOnlySince(builtRevision_) && builder.patchClipLevels(playedSong(doc.song()))) {
    builtRevision_ = doc.revision();
    return;
  }
  builder.setSampleRate(engine.sampleRate());
  builder.setPdc(pdc);
  builder.setAsyncSamples(true);
  builder.setDeferRenders(editing);
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
  engine.setProgram(builder.build(playedSong(s)));  // K-87: frozen and hibernating units
  engine.setPattern(curPattern);
}


// The song the engine plays: frozen and hibernating units (K-87), and the Stage's soloed rooms (K-270)
// (this computer only).
Song App::playedSong(const Song& s) {
  Song p = freezeui::playSong(*this, s);
  stageui::listenFilter(*this, p);
  recordrows::listenFilter(*this, p);  // K-123: a soloed or auditioned take
  return p;
}

void App::tick(double t) {
  pollPresetImport();
  now = t;
  pollProjectLoad();  // J-167: a slice of the project load, the swap once it is ready
  cacheui::tick(*this);  // K-45: what a crash left in the unpacked cache
  freezeui::tick(*this);  // K-87: freeze renders, out-of-date freezes, hibernating plugins waking and sleeping
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
  {  // J-133: sound theme switches take effect at once (clips load on a worker thread)
    if (soundThemeLoad_.valid() && soundThemeLoad_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      std::unique_ptr<uisound::Bank> b = soundThemeLoad_.get();
      if (b && b->theme == uiSoundTheme && b->rate == uiSoundRate) uiSounds.install(std::move(b));  // else stale: reloads below
      else uiSoundTheme.clear();
    }
    if (uiSoundRate > 0 && !soundThemeLoad_.valid() &&
        uisound::resolveTheme(settings.getString(uisound::kThemeKey)) != uiSoundTheme)
      loadSoundTheme(uiSoundRate, false);
    uiSounds.collect();
  }
  pollPreview();
  // Marker actions (J-11) run in the audio engine at the exact sample (J-85).
  engine.collectGarbage();
  toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& x) { return x.until < now; }), toasts.end());
  int64_t wall = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if (wall - lastBackupCheck_ >= 5 && !embedded) {  // PH-09: the host saves an embedded project
    lastBackupCheck_ = wall;
    if (doc.modified() && backups->due(wall)) {
      std::string err;
      if (backups->write(doc.song(), projectPath, wall, &err).empty() && !err.empty()) notify("Backup failed: " + err, true);
    }
    // Autosave (J-24): saved projects are written back to their file.
    // Only Gridline files: an FL Studio project is re-exported on an explicit Save only.
    // K-42: never to a web or network drive (only an explicit Save writes there; the timed backups
    // above stay on this computer).
    if (settings.getBool("backup.autosave") && !projectPath.empty() && isGridlineText(projectFormat) && doc.modified() &&
        !remotefs::isRemote(projectPath)) {
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

void App::loadSoundTheme(double deviceRate, bool sync) {
  if (uiSoundsDisabled || assetsDir.empty() || deviceRate <= 0) return;
  uiSoundRate = deviceRate;
  uiSoundTheme = uisound::resolveTheme(settings.getString(uisound::kThemeKey));
  const std::string dir = (fs::path(assetsDir) / "sounds").string();
  if (sync) {
    uiSounds.install(uisound::loadTheme(dir, uiSoundTheme, deviceRate));
    return;
  }
  soundThemeLoad_ = std::async(std::launch::async, [dir, theme = uiSoundTheme, deviceRate] {
    return uisound::loadTheme(dir, theme, deviceRate);
  });
}

bool App::previewSoundTheme(const std::string& theme, uisound::Event e) {
  if (uiSoundsDisabled || assetsDir.empty() || uiSoundRate <= 0) return false;
  uisound::Clip c;
  if (!uisound::loadThemeClip((fs::path(assetsDir) / "sounds").string(), theme, e, uiSoundRate, c)) return false;
  uiSounds.setVolume(static_cast<float>(settings.getNumber("sounds.volume")));
  return uiSounds.preview(std::move(c));
}

void App::notifyAction(const std::string& english, std::vector<Toast::Action> actions, double seconds) {
  notify(english);
  if (toasts.empty()) {
    return;
  }
  toasts.back().actions = std::move(actions);
  toasts.back().until = now + seconds;
}

bool App::runToastAction(size_t index) {
  for (auto it = toasts.rbegin(); it != toasts.rend(); ++it) {
    if (it->actions.empty()) {
      continue;
    }
    if (index >= it->actions.size()) {
      return false;
    }
    const std::function<void()> run = it->actions[index].run;
    it->until = 0;
    it->actions.clear();
    if (run) {
      run();
    }
    return true;
  }
  return false;
}

void App::notify(const std::string& english, bool error, const std::string& details) {
  // J-61: notifications are shown in the interface language (the log keeps English).
  static uint64_t serial = 0;
  Toast t;
  t.text = i18n::trMessage(english);
  t.until = now + toast::lifetime(error);  // J-78: errors stay up longer (and while hovered)
  t.error = error;
  t.english = english;
  t.details = details;
  t.serial = ++serial;
  toasts.push_back(std::move(t));
  if (error) uiSound(uisound::Event::Error);  // every failure is reported through here
  const std::string logged = details.empty() || english.find(details) != std::string::npos ? english : english + " [" + details + "]";
  if (error)
    logging::error("app", logged);
  else
    logging::info("app", logged);
}

std::string App::windowTitle() const {
  if (home.visible() && !home.projectOpen()) return "Gridline";  // J-141: the launcher, no project open
  std::string name = projectFile().empty() ? doc.song().info.title : fs::path(projectFile()).filename().string();
  if (std::string shared = collabui::projectTitle(*this); !shared.empty()) name = shared;  // J-111f: the collaborative project's name
  return (doc.modified() ? "• " : "") + name + " — Gridline";
}

void App::newProject() {
  // K-112c (round 45): a new set in Clips opens on the clip grid, as the reference's new set does.
  if (layoutui::active() == layout::Preset::Live) {
    livelook::showView(*this, true);
    livelook::setDrawMode(*this, false);  // and in select mode (B switches to drawing)
  }
  livelook::setOpenedNew();  // N47-17: its first track selected (Clips only)
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

static void trackerFirstPattern(Song& s);  // (below)

Song App::starterSong() const {
  Song s = makeEmptySong();
  applyProjectDefaults(s);
  s.panLaw = PanLaw::Stage;  // new projects place each channel on the Stage (older ones keep the Balance law)
  // K-112c (round 45): a new set in Clips is the reference's: its tracks and returns, no instruments
  // (no starter kit), 120 BPM unless Settings has a default tempo other than its own 140.
  if (layoutui::active() == layout::Preset::Live) {
    if (!(settings.getNumber("project.defaultTempo") > 0) || settings.getNumber("project.defaultTempo") == 140.0) s.tempo = 120.0;
    trackmodes::makeClipsStarterTracks(s);
    return s;
  }
  trackerFirstPattern(s);
  return addStarterKit(std::move(s));  // starter kit and master limiter (core/Templates.h)
}

// K-106 cp21: a new song made in the Tracker starts with the classic 64-row pattern (4 bars of 16
// steps) in place of the one-bar one; the other layouts' new songs are unchanged.
static void trackerFirstPattern(Song& s) {
  if (layoutui::active() != layout::Preset::Tracker || s.patterns.size() != 1) return;
  Pattern& p = s.patterns.front();
  if (p.bars == 1 && p.stepsPerBar == 16 && p.notes.empty() && p.steps.empty() && p.automation.empty()) p.bars = 4;
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
  collabui::closeSession(*this);  // J-111
  engine.stopToStart();
  engine.panic(false);  // (the project is replaced: no fade over the new one)
  doc.reset(std::move(s));  // the new project is the "saved" baseline, with no history
  cacheui::projectChanged(*this, nullptr);  // K-45: the closed project's unpacked copy can go
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
  rackReveal.quietSwitch(engine.mode());  // J-77: a new project does not count as choosing PAT
  syncEngine();
  home.projectOpened(now);  // J-141: the launcher gives way to the new project
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
  layout::normalize(settings);    // K-79: layout keys from an older or newer Gridline
  layoutui::applyKeyProfile(*this);  // the shortcut set of the layout preset, with its own overrides
}

std::string App::languageDir() const { return assetsDir.empty() ? std::string() : (fs::path(assetsDir) / "i18n").string(); }

void App::applyStartup(bool plainLaunch) {
  applySettingsHooks();
  templates(true);
  // J-135: the demo song greets a new install, once; J-141: then the welcome launcher
  // (plain launches) or Settings > General > On startup.
  launcher::StartupInputs in;
  in.plainLaunch = plainLaunch;
  in.freshInstall = freshInstall_;
  in.demoShown = settings.getBool("general.demoShown");
  in.launcherOn = settings.getBool("general.launcher");
  in.startupMode = settings.getString("general.startup");
  {
    std::error_code ec;
    // K-42: a project on a web drive is not checked here (a stat can stall a starting app): if the
    // drive is offline, the loading window says so.
    in.lastExists = !recentFiles.empty() && (remotefs::isRemote(recentFiles.front()) || fs::exists(recentFiles.front(), ec));
  }
  in.defaultTemplate = !settings.getString("general.defaultTemplate").empty();
  const launcher::StartupPlan plan = launcher::planStartup(in);
  if (plan.markDemoShown) {
    settings.set("general.demoShown", true);
    settings.save(Settings::defaultPath());
  }
  switch (plan.action) {
    case launcher::StartAction::Demo: newFromTemplate("demo"); break;
    case launcher::StartAction::Empty: newFromTemplate(""); break;
    case launcher::StartAction::Last: openProject(recentFiles.front()); break;
    case launcher::StartAction::Template: newProject(); break;
    case launcher::StartAction::None: break;
  }
  if (plan.showLauncher) home.showAtStartup(now);
}

bool App::openProject(const std::string& pathIn) {
  // J-141: whatever opened (a project, or a file imported into the project), the
  // launcher gives way to it (J-167: a project once its load has finished).
  const bool ok = openProjectFile(pathIn);
  if (ok && !projectLoadBlocking()) home.projectOpened(now);
  return ok;
}

bool App::openProjectFile(const std::string& pathIn) {
  // J-111: a collaborative project folder opens its project (with its session)
  // K-42: on a web drive the project's kind goes by its name and the loader asks the drive on its
  // worker; only a path with no project extension (a folder) needs a quick, bounded look here.
  const bool onDrive = remotefs::isRemote(pathIn);
  const bool lookInFolder = !onDrive || (openKindOf(pathIn) == OpenKind::Unknown && remotefs::probeWithin(pathIn, 1500).isDir);
  const std::string collabFile = lookInFolder ? collabui::projectFileIn(pathIn) : std::string();
  const std::string& path = collabFile.empty() ? pathIn : collabFile;
  // J-52: FL Studio projects and zipped projects open as projects; MIDI and
  // audio files are imported into the open project.
  bool collab = false;
  if (dawproject::isDawProject(path)) return exportui::openAny(*this, path);  // K-89: a new project from it
  switch (openKindOf(path)) {
    case OpenKind::Midi:
    case OpenKind::Audio: return exportui::openAny(*this, path);
    case OpenKind::FlProject:
    case OpenKind::ZippedProject:
    case OpenKind::TrackerModule: break;
    case OpenKind::GridlineProject:
    case OpenKind::Unknown:
      if (onDrive && collabFile.empty()) break;  // K-42: found out on the loader's worker (Request::collabProbe)
      // J-111: a collaborative project opens with its session (or asks for an update first)
      switch (collabui::openGate(*this, path)) {
        case collabui::OpenGate::NotCollab: break;
        case collabui::OpenGate::Blocked: return false;
        case collabui::OpenGate::Open: collab = true; break;
      }
      break;
  }
  // J-167: read and prepared by the project loader (ProjectLoadUi.cpp) while the
  // interface keeps drawing; the open project stays until the new one is ready.
  startProjectLoad(path, collab);
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
      if (rep.grooves) s += ", " + std::to_string(rep.grooves) + " swing presets";
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

bool App::saveProjectAs(const std::string& path, SaveFormat format, std::function<void()> then) {
  crash::crumb("project save", isGridlineText(format) ? "gridline" : isGridlineZip(format) ? "zip" : "fl");
  crash::BusyScope busy("saving");
  captureHostedStates(*this);  // third-party plugin state into the song
  std::string err;
  std::string p = path;
  // A Gridline project keeps the name it was given (K-121: an older ".gridline.json" file is saved
  // in place, under that name); every other format gets its own extension.
  if (!isGridlineText(format) || fs::path(p).extension().empty()) p = withSaveExtension(p, format);
  // K-47: the background saves write the song as it is now. By the time one is done the person may
  // have edited on (those edits stay unsaved) or opened another project (nothing of this applies to it).
  const uint64_t epoch = doc.projectEpoch(), serial = doc.saveSerial();
  // N48-07 (checkpoint 49): the view is noted when the save starts (the view it was saved in), not only
  // once the background write is done: a quit or an open in between lost it.
  if (!collabui::active(*this)) livelook::projectSaved(*this, p);
  {  // K-297 (round 9): the plugin windows open now go in the project (view state: no undo step)
    std::vector<Song::OpenPluginWindow> wins;
    for (const auto& [owner, slot] : openPluginWindowList(*this)) wins.push_back({owner, slot});
    if (doc.song().pluginWindows != wins) doc.patchSong([&wins](Song& s) { s.pluginWindows = wins; });
  }
  workspacesui::refreshProjectCopy(*this);  // K-237: the opt-in copy of the workspaces, as they are now
  auto saved = [this, p, format, epoch, serial]() {
    if (doc.projectEpoch() != epoch) return;
    if (collabui::active(*this)) {  // J-111: a copy; the project stays the collaborative one
      notify("Saved a copy: " + fs::path(p).filename().string());
      return;
    }
    projectPath = p;
    projectFormat = format;
    openedFrom.clear();
    doc.markSavedAt(serial);
    addRecent(p);
    rememberSketch(p);  // J-141: the launcher's tile shows what was saved
    livelook::projectSaved(*this, p);  // N45-10: the view it was saved in
    livelook::rowHeightsSaved(*this, p);  // checkpoint 56 (N55-06): the Set's row heights follow its file
  };
  // What was waiting for this save (closing, opening another project) only goes on if nothing
  // unsaved would be lost by it.
  auto thenWhenSaved = [this, then, epoch]() {
    if (!then || doc.projectEpoch() != epoch) return;
    if (doc.modified()) notify("The project changed while it was saving, so it stayed open. Save it again.", true);
    else then();
  };
  try {  // J-78: an unexpected failure is reported, never a crash that loses the open project
    switch (format) {
      case SaveFormat::Gridline:
      case SaveFormat::GridlineJson:
      {
        // R9-05: samples that live only in the unpack cache (an imported module's, an opened
        // archive's) are copied beside the project ("<name> Samples") and the saved file names the
        // copies, so clearing the cache never leaves a saved project silent. The open song keeps
        // pointing at the cache (no edit is made to it); the next save reuses the copies.
        Song out = doc.song();
        std::string copyErr;
        const fs::path pp = fs::u8path(p);
        std::string stem = pp.filename().u8string();
        for (const char* ext : {".gridline.json", ".gridline", ".json"})
          if (stem.size() > std::strlen(ext) && stem.compare(stem.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
            stem.resize(stem.size() - std::strlen(ext));
            break;
          }
        int adopted = adoptCacheFiles(out, archive::defaultCacheRoot(), (pp.parent_path() / fs::u8path(stem + " Samples")).u8string(), &copyErr,
                                      pp.parent_path().u8string());
        if (!copyErr.empty()) notify("Some samples stay in the cache only: " + copyErr, true);
        // R11-02: the folders Gridline made beside the project (or, before the first save, in the
        // settings folder) go with it: the audio editors' "Edited audio" and the "<name> Samples"
        // copies, copied on Save as and named relative to the project.
        {
          std::vector<std::pair<std::string, std::string>> folders;
          folders.emplace_back((fs::u8path(configDir()) / "Edited audio").u8string(), "Edited audio");
          if (!projectPath.empty()) {
            const fs::path old = fs::u8path(projectPath);
            std::string oldStem = old.filename().u8string();
            for (const char* ext : {".gridline.json", ".gridline", ".json"})
              if (oldStem.size() > std::strlen(ext) && oldStem.compare(oldStem.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
                oldStem.resize(oldStem.size() - std::strlen(ext));
                break;
              }
            folders.emplace_back((old.parent_path() / "Edited audio").u8string(), "Edited audio");
            folders.emplace_back((old.parent_path() / fs::u8path(oldStem + " Samples")).u8string(), stem + " Samples");
          }
          folders.emplace_back((pp.parent_path() / "Edited audio").u8string(), "Edited audio");
          folders.emplace_back((pp.parent_path() / fs::u8path(stem + " Samples")).u8string(), stem + " Samples");
          // R12-01: and wherever else the song's files still live in a Gridline-made folder (the
          // open song keeps the first project's paths: Save as B, then v2, then v3 finds B's).
          for (auto& f : gridlineMadeFolders(out, stem)) folders.push_back(std::move(f));
          std::string moveErr;
          adopted += adoptProjectFolders(out, folders, pp.parent_path().u8string(), &moveErr);
          if (!moveErr.empty()) notify("Some edited audio stays where it was: " + moveErr, true);
        }
        if (!gl::saveProject(p, adopted > 0 ? out : doc.song(), &err)) {
          notify("Could not save the project: " + err, true, p);
          return false;
        }
      }
        archive::pinUnpacked(doc.song());  // K-42: the samples it points at stay in the cache
        break;
      case SaveFormat::Flp:
      case SaveFormat::FlZip: {
        if (format == SaveFormat::Flp) archive::pinUnpacked(doc.song());  // K-45: an .flp keeps full paths too
        // J-80: a background job (instruments rendered to samples, progress,
        // cancel); it refuses a read-only target (J-78) and notifies the summary ("Saved x.flp: ...") or the error
        // itself, and the save completes, and `then` runs, when it is done.
        auto done = [saved, thenWhenSaved](bool ok) {
          if (!ok) return;
          saved();
          thenWhenSaved();
        };
        return format == SaveFormat::Flp ? exportui::writeFlpFile(*this, p, done) : exportui::writeFlpZip(*this, p, done);
      }
      case SaveFormat::GridlineZip:
      case SaveFormat::GridlineZipOld:
        // K-47: a background job as well: a zip of a big project takes a while.
        return exportui::writeGridlineZip(*this, p, [this, p, saved, thenWhenSaved](const std::string& files) {
          saved();
          notify("Saved " + fs::path(p).filename().string() + files, false, p);
          uiSound(uisound::Event::Save);
          thenWhenSaved();
        });
      case SaveFormat::Count: return false;
    }
  } catch (const std::exception& e) {
    crash::reportCurrentException("Saving the project");
    notify(std::string("Could not save the project: internal error (") + e.what() + "). Please report it; try another format meanwhile.", true, p);
    return false;
  }
  saved();
  notify("Saved " + fs::path(p).filename().string(), false, p);
  if (!autosaving_) uiSound(uisound::Event::Save);
  if (then) then();
  return true;
}

static std::string homeDir() {
  const char* h = gl::platform::homeEnv();
  return h ? h : ".";
}

void App::save() {
  if (collabui::active(*this)) {  // J-111: collaborative projects save continuously (op log + project file)
    if (collab::Session* s = collabui::session(*this)) s->saveNow();
    doc.markSaved();
    notify("Saved (collaborative projects save as you work)");
    return;
  }
  if (projectPath.empty())
    saveAs();
  else
    saveProjectAs(projectPath, projectFormat);  // the format the user chose for this file
}

void App::saveAs(std::function<void()> then) {
  // J-52: one list of formats, like FL's Save as: Gridline project, Gridline
  // zipped project (J-78), FL Studio project, FL Studio zipped loop package. A project opened from an FL file
  // starts in its folder, under its name, as a Gridline project.
  const std::string ref = projectFile();
  // K-45: a Gridline zipped project starts as what it was, so Save can replace the archive (K-121:
  // under its own name, older or new). Everything else starts as a project under the new name; the
  // older names stay in the type list.
  const SaveFormat refFormat = saveFormatOf(ref);
  const bool fromGridlineZip = projectPath.empty() && isGridlineZip(refFormat);
  const SaveFormat fmt = fromGridlineZip                        ? refFormat
                         : projectPath.empty() || isGridlineText(projectFormat) ? SaveFormat::Gridline
                                                                : projectFormat;
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
    const auto f = static_cast<SaveFormat>(std::clamp(fileDialog.typeIndex, 0, static_cast<int>(SaveFormat::Count) - 1));
    saveProjectAs(withSaveExtension(p, f), f, then);
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
  receipt::writeRenderOrigin(doc.song(), p);  // J-219 (Track Receipt)
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

bool App::autoOpens(const std::string& type, const std::string& data) const {
  if (addRoute == AddRoute::MixerDrop || type.empty()) return false;
  if (xdrop::sampleSlot(type) == xdrop::SampleSlot::None) return true;
  const std::string file = data.substr(0, data.find('\n'));  // the sample is the data, or its first line
  return file.empty() || browse::kindOf(file) != browse::Kind::Audio;
}

void App::showNewInstrument(ObjectId channel) {
  const Channel* c = doc.song().channel(channel);
  if (!c) return;
  selectChannel(channel);
  if (autoOpens(c->plugin.type, c->plugin.data)) openChannelWindow(channel);
}

void App::openChannelWindow(ObjectId channel) {
  const Channel* c = doc.song().channel(channel);
  if (!c) return;
  selectChannel(channel);
  settingsChannel = channel;  // routePluginWindows opens an installed plugin's own window
  if (!isHostedType(c->plugin.type)) focusWindow = "###channelsettings";  // already open for another channel: to the front
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

void App::followSelection() {
  static selfollow::State st;
  if (!settings.getBool("rack.selectionFollows")) {
    st.channel = selChannel, st.mixer = selMixer;
    return;
  }
  const ObjectId before = selMixer;
  selfollow::step(doc.song(), st, selChannels, selChannel, selMixer);
  if (selMixer != before && !mixerView.sel.empty() && !mixerView.sel.count(selMixer)) mixerView.sel = {selMixer};
}

void App::openPianoRoll(ObjectId channel) {

  selectChannel(channel);
  showPiano = true;
  focusWindow = "Piano roll";
}

void App::previewSample(const std::string& path) {
  if (remotefs::isRemote(path)) {  // K-42: read on a background decoder, and heard when it has arrived
    previewWhenReady(path);
    return;
  }
  SamplePtr s = sampleCache().get(path);
  if (!s) {
    notify("Cannot decode " + fs::path(path).filename().string(), true);
    return;
  }
  engine.preview(s);
}

void App::previewWhenReady(const std::string& path) {
  pendingPreview_ = path;
  pollPreview();
}

void App::pollPreview() {
  if (pendingPreview_.empty()) return;
  SampleCache& cache = sampleCache();
  if (SamplePtr s = cache.request(pendingPreview_)) {
    engine.preview(s);
  } else if (cache.isPending(pendingPreview_)) {
    return;
  } else if (SamplePtr again = cache.request(pendingPreview_)) {  // (finished between the two calls)
    engine.preview(again);
  } else if (!cache.isPending(pendingPreview_)) {
    notify("Cannot decode " + fs::path(pendingPreview_).filename().string(), true);
  } else {
    return;
  }
  pendingPreview_.clear();
}

void App::stopPreview() {
  pendingPreview_.clear();
  engine.preview(nullptr);
}

}  // namespace gl

// ---------------------------------------------------------------- MIDI ----
#include "../core/MidiFile.h"
#include "ExportUi.h"
#include "../core/SafeFs.h"

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

void App::saveNewVersion() {
  if (projectPath.empty()) {
    saveAs();
    return;
  }
  fs::path cur(projectPath);
  std::string stem = projectStemOf(projectPath);  // "song.gridline" / "song.gridline.json" -> "song"
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
  } while (safefs::existsAt(next));
  saveProjectAs(next.string(), projectFormat);
}

bool App::revertToLastBackup() {
  crash::crumb("revert to backup");
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
    trackerFirstPattern(s);
  } else if (which == "tracker") {
    s = makeTrackerDemo();  // K-106 cp15: the Tracker's demo module
  } else if (which == "demo") {
    s = makeDemoTrack();  // J-135: the "Gridline" demo song
    // K-112c (round 45): opened in Clips, the demo starts with its clips in the grid (as the reference's
    // demo sets do): Prepare the live area, made part of the opened song (its saved starting point, so
    // collaborators get the same; Studio's demo is not changed).
    if (layoutui::active() == layout::Preset::Live) {
      Document d(s);
      ops::prepareClipGrid(d, std::max<Tick>(1, d.song().barTicks()) * livelook::kSceneBars);  // C44-07: one clip to a slot
      s = d.song();
    }
  } else {
    std::string err;
    if (!loadTemplate(which, s, &err)) {
      notify("Could not open template: " + err, true);
      return;
    }
    if (s.info.author.empty()) s.info.author = settings.getString("project.defaultAuthor");
  }
  startProject(std::move(s));
  if (livelook::active(*this)) livelook::setDrawMode(*this, false);  // N45-12: Clips opens a set with the pointer
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
    char num[32];
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
  // R02-06: in the Tracks layout the tracks area is the main window and the panes are part of it;
  // closing all windows closes the floating ones only (dialogs, tool windows, plugin windows).
  if (layoutui::active() == layout::Preset::Logic) {
    showSettings = showInfo = showHistory = showAbout = showExport = showShortcuts = showPluginManager = showPerformance = showGroove = false;
    showMaster = false;
    closePluginWindows();
    return;
  }
  showBrowser = showRack = showPiano = showPlaylist = showMixer = false;
  showSettings = showInfo = showHistory = showAbout = showExport = showShortcuts = showPluginManager = showPerformance = showGroove = false;
  closePluginWindows();
}

void App::closePluginWindows() {
  closeAllHostedEditors(*this);
  logicplug::closeMoreFx();  // K-297: Smooth's earlier effect windows too
  settingsChannel = kNoId;
  fxEditor = {kNoId, -1};
}

}  // namespace gl
