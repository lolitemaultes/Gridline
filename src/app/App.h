// Gridline — application state shared by all windows (UI thread only).
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "RackReveal.h"
#include "../core/Timeline.h"
#include "../core/Backup.h"
#include "../core/Document.h"
#include "../core/Launcher.h"
#include "../core/Recents.h"
#include "../core/MidiRecord.h"
#include "../engine/AudioRecord.h"
#include "../core/MidiTemplates.h"
#include "../core/ProjectFiles.h"
#include "../core/Settings.h"
#include "../core/SmoothScroll.h"
#include "../core/Templates.h"
#include "../engine/AudioEngine.h"
#include "../engine/ProjectLoad.h"
#include "../engine/Sample.h"
#include "../engine/UiSounds.h"
#include "Midi.h"
#include "MixerState.h"

struct ImFont;

namespace gl {
namespace collab {
class Controller;
}

// Grid snap choices shared by the piano roll and playlist.

// Editing tools of the piano roll and the playlist. J-11 added Slip (playlist:
// move a clip's content inside it), Play (audition / scrub while held), Zoom
// (drag a range, click in, right-click out) and Detune (drag notes' fine pitch).
enum class Tool { Draw, Paint, Erase, Select, Slice, Mute, Slip, Play, Zoom, Detune };

struct PianoRollState {
  float pxPerTick = 0.25f;
  float rowHeight = 14.0f;
  float scrollX = 0, scrollY = -1;  // -1: centre on C5 at first open
  bool centred = false;             // J-67: first-open centring done (scrollY < 0 is an elastic overscroll now)
  smoothscroll::Axis axX, axY;      // J-67: smooth scrolling / elastic edges
  Tool tool = Tool::Draw;
  Snap snap = Snap::Line;
  Tick effGrid = 0;          // grid the last frame snapped to (for the snap chip)
  float zoomRequest = 1.0f;  // zoom buttons/keys: applied once the canvas size is known
  bool fitRequest = false;   // zoom to fit the pattern
  bool zoomInit = false;     // first frame: default zoom (16th notes ~20 px wide at any PPQ)
  std::set<size_t> selected;  // note indices in the current (pattern, channel) list
  ObjectId selPattern = kNoId, selChannel = kNoId;
  Tick lastLength = 0;  // remembered note length (0 = one step)
  int lastVelocity = 100;
  bool ghostNotes = true;
  std::vector<Note> clipboard;
  // J-11 helpers.
  int stampChord = -1;        // tools::chords() index drawn by Draw / Paint (-1 = single notes)
  int stampInversion = 0;
  bool stampOnce = false;     // J-27: the stamp turns off after one chord
  int stampAuto = 0;          // J-27: chord from the scale: 0 off, 1 bottom up, 2 top down
  int scaleRoot = 0;          // 0..11 (C..B)
  int scale = 0;              // tools::scales() index; 0 = no scale highlighting
  bool snapToScale = false;   // drawn and moved notes land on the scale
  bool keyNames = false;      // note names on every key (else only on C)
  std::set<ObjectId> ghostChannels;  // channels shown as ghost notes (empty = all others)
};

// Channel rack view state (J-11): zipped rows, graph / keyboard editor lane,
// the step under the pointer (insert / delete step).
struct RackState {
  std::set<ObjectId> zipped;
  bool showGraph = false;
  int graphLane = 0;           // tools::StepLane
  ObjectId hoverChannel = kNoId;
  int hoverStep = -1;
  // J-84: the step area scrolls sideways (long patterns, narrow racks).
  float stepScroll = 0;          // px into the step content (drawn, may be elastically past an edge)
  smoothscroll::Axis axStep;     // J-67 smooth scroll / elastic edges
  float lastAvail = 0;           // rows pane inner width last frame (bottom bar reserve)
  float viewX0 = 0, viewX1 = 0;  // step viewport, screen x (time scroller alignment)
  float originX = 0;             // row origin, screen x (bottom bar alignment)
  bool thumbDrag = false;        // time scroller thumb dragged
  float thumbGrab = 0;           // pointer offset into the thumb
  int lastPlayStep = -1;         // page follow while the pattern plays
};

struct PlaylistState {
  float pxPerTick = 0.17f;
  float trackHeight = 46.0f;
  float scrollX = 0, scrollY = 0;
  smoothscroll::Axis axX, axY;  // J-67: smooth scrolling / elastic edges
  Tool tool = Tool::Draw;
  Snap snap = Snap::Line;
  Tick effGrid = 0;              // grid the last frame snapped to (for the snap chip)
  float zoomRequest = 1.0f;      // toolbar zoom buttons: applied once the canvas size is known
  bool fitRequest = false;       // zoom to fit the song
  Tick selStart = 0, selEnd = 0; // time selection (selEnd <= selStart: none)
  std::set<ObjectId> selected;  // clip ids
  std::vector<Clip> clipboard;
  // J-28: FL-style playlist controls.
  ClipKind srcKind = ClipKind::Pattern;  // current clip source (what draw / paint place);
  ObjectId srcId = kNoId;                //   patterns follow App::curPattern
  // J-118: the clip brush: the clip last clicked (kNoId: none, the source's
  // natural clip). Draw / paint place copies of it while it is a clip of the
  // current source; `brush` keeps its settings for when it is deleted.
  ObjectId brushId = kNoId;
  Clip brush;
  int pickerTab = 0;                     // clip source picker: 0 patterns, 1 audio, 2 automation
  bool pickerOpen = true;                // picker panel shown (Alt+P)
  // J-82: picker selection (Ctrl+click adds, Shift+click a range) of the
  // current tab; Delete deletes it while the picker has the focus.
  std::set<ObjectId> pickerSel;
  bool pickerFocus = false;              // last click landed in the picker
  bool stretchResize = false;            // Shift+M: resizing audio clips stretches them
  bool showFades = true;                 // Shift+F: fade handles on audio clips
  int patternPreview = 0;                // J-173: pattern clips show 0 notes and automation, 1 notes, 2 automation
  bool zeroCross = false;                // audio clip edges land on zero crossings
  bool wideHeaders = false;              // track headers: wide / narrow
  Snap lastSnap = Snap::Line;            // snap the magnet restores
  int quickZoom = 0;                     // pending quick zoom (1..3: 4 / 16 / 64 bars fill the view)
  bool zoomSelRequest = false;           // pending zoom on the selection (time selection, else selected clips)
};

struct FileDialogState {
  bool open = false;
  bool save = false;
  std::string title;
  std::string dir;
  std::string name;
  std::vector<std::string> exts;  // accepted extensions (lower case, with dot)
  std::function<void(const std::string&)> onOk;
  // J-52: file types the dialog offers (a type list beside the name); the
  // chosen one sets `exts`. `nameForType` renames the file when a save
  // dialog's type changes. `typeIndex` is read by onOk.
  std::vector<FileType> types;
  int typeIndex = 0;
  std::function<std::string(const std::string& name, int type)> nameForType;
  std::string confirmOverwrite;  // save: the existing file the next OK replaces
  std::string devAccept;  // J-167 dev-input "dialogopen": OK with this path once the dialog is up
};

struct Toast {
  std::string text;     // shown (interface language)
  double until = 0;
  bool error = false;
  // J-78: a click copies the English message + details (toast::clipboardText).
  std::string english;  // untranslated message
  std::string details;  // file path, error code, missing files … ("" = none)
  double copiedUntil = 0;  // "Copied" feedback shows until then
  uint64_t serial = 0;     // stable ImGui id
};

class App {
 public:
  App();
  ~App();

  // ---- document & engine ----
  // PH-09: running as a plugin inside another application (the host owns the
  // project, audio and MIDI): no backups, autosave, devices or plugin scans,
  // one processing thread. Set right after construction.
  bool embedded = false;
  // PH-09: extra entries for a parameter's context menu (plugin build: host macros).
  std::function<void(ParamId)> extraParamMenu;
  // J-131: one built-in plugin on its own (Gridline Suite in another host):
  // plugin menus keep the plugin's own items (no channel / slot operations).
  bool singlePlugin = false;
  Document doc;
  // J-111: collaborative projects (app/CollabUi.h); made on first use.
  std::unique_ptr<collab::Controller> collab;
  bool showCollab = false, showJoin = false, showSyncReview = false;
  AudioEngine engine{44100.0};
  ProgramBuilder builder;
  Settings settings;
  std::unique_ptr<BackupManager> backups;
  std::string projectPath;  // "" = never saved
  SaveFormat projectFormat = SaveFormat::Gridline;  // what Save writes to projectPath (J-52)
  std::string openedFrom;   // J-52: FL project / zip the song was opened from (projectPath "")
  // The file the project is known by: projectPath, else openedFrom.
  const std::string& projectFile() const { return projectPath.empty() ? openedFrom : projectPath; }
  std::vector<std::string> recentFiles;  // most recent first (recent.txt)
  // J-141: the welcome launcher (app/LauncherUi.h): shown at start-up and from the logo.
  launcher::State home;

  void syncEngine();  // rebuild the engine program after edits
  void forceRebuild() { builtRevision_ = ~0ull; }  // the next syncEngine() rebuilds (PH-09: macro links)
  void tick(double now);  // per-frame housekeeping (backups, toasts, GC)

  // ---- selection ----
  ObjectId curPattern = kNoId;
  ObjectId selChannel = kNoId;
  std::set<ObjectId> selChannels;
  ObjectId selMixer = kNoId;
  MixerViewState mixerView;  // mixer layout, meters, multi-selection, armed tracks (J-12)
  ObjectId settingsChannel = kNoId;        // channel settings window target
  int channelTab = 0;                       // channel settings window tab (0 plugin, 1 misc, 2 envelope)
  std::pair<ObjectId, int> fxEditor{kNoId, -1};  // effect editor window target
  PianoRollState piano;
  PlaylistState playlist;
  RackState rack;

  // ---- windows ----
  bool showBrowser = true, showRack = true, showPiano = true, showPlaylist = true, showMixer = true;
  bool showSettings = false, showInfo = false, showHistory = false, showAbout = false, showExport = false;
  bool showShortcuts = false;
  bool showPluginManager = false;  // Options > Plugin manager
  bool showImportReport = false;
  // FL Studio preset library import (runs on a worker thread).
  struct PresetImport {
    std::thread worker;
    std::atomic<bool> running{false}, cancel{false}, finished{false};
    std::atomic<size_t> done{0}, total{0};
    std::string summary;  // written by the worker before `finished`
  } presetImport;
  void startPresetImport();
  void pollPresetImport();
  bool showLivePads = false;    // live mode pads window
  bool showPerformance = false;  // "Performance" window (CPU per channel / insert / effect)
  bool showMaster = false;       // RN-09 "Master chain" quick panel
  // CPU use per channel, insert and effect slot, sampled from the engine's
  // profiling counters while the mixer or the Performance window is open
  // (Performance.cpp).
  struct PerfStats {
    enum Kind : int { Channel, Insert, Slot };
    struct Key {
      int kind;
      ObjectId id;
      int slot;
      bool operator<(const Key& o) const { return std::tie(kind, id, slot) < std::tie(o.kind, o.id, o.slot); }
    };
    std::map<Key, uint64_t> lastNs;  // counter values at the last sample
    std::map<Key, float> load;       // fraction of one core's real time over the last period
    double lastT = -1;
    float cpuPeak = 0;  // worst block load during the last period
    float get(int kind, ObjectId id, int slot = -1) const {
      auto it = load.find({kind, id, slot});
      return it == load.end() ? 0.f : it->second;
    }
    // An insert: its own processing (inputs, strip, sends) plus its effects.
    float insertTotal(ObjectId id) const {
      float t = get(Insert, id);
      for (auto it = load.lower_bound({Slot, id, -1}); it != load.end() && it->first.kind == Slot && it->first.id == id; ++it) t += it->second;
      return t;
    }
  } perf;
  // Mixer processing threads from the "audio.threads" setting (0 = automatic).
  int processingThreads() const;
  bool typingKeyboard = false;  // computer keyboard plays the selected channel
  bool recordArmed = false;
  bool metronome = false;
  bool resetLayout = false;
  RackReveal rackReveal;  // J-77: choosing PAT shows the channel rack (main loop)
  bool quitRequested = false;
  bool confirmDiscard = false;  // unsaved-changes prompt open
  // J-82: short yes / no prompt ("Delete 'Drum loop' and its 6 clips?"):
  // `action` runs on the confirm button; Cancel / Esc drop it.
  struct ConfirmPrompt {
    bool open = false;
    std::string title, subtitle, okLabel;
    std::function<void()> action;
  } confirm;
  std::function<void()> afterDiscard;

  // Audio device (opened by main).
  std::vector<std::string> audioDevices;
  std::string audioDevice;  // "" = system default
  int audioSampleRate = 44100;
  int audioBuffer = 256;
  bool audioReopen = false;
  std::string audioStatus;
  // Audio input & recording (J-8): input device (Settings → Audio), the
  // recording session (armed mixer tracks → playlist audio clips) and the
  // record filter (what the record button records).
  std::vector<std::string> audioInputDevices;
  std::string audioInputDevice;  // "" = no input, "*" = system default
  int audioInputChannels = 2;    // channels the input stream delivers
  int audioInputLatency = 0;     // samples: capture buffer + jitter buffer + output buffer
  std::string audioInputStatus;
  bool recAudio = true;          // record filter: audio into the playlist (armed tracks)
  bool recAudioEditor = false;   // record filter: audio into the audio editor of armed tracks
  bool recPunch = true;          // punch in/out at the playlist time selection
  AudioRecorder audioRec;
  bool audioRecSession = false, audioRecFinishing = false;
  double audioRecStopTime = 0;
  int audioRecLoopRestore = -1;  // looping to restore once nothing records past the song end (-1 = unchanged)
  bool recRollingPrev = false;   // last frame: the record button armed and the transport rolling (J-175)
  bool audioRecArmedPrev = false;
  ObjectId clipProps = kNoId;    // audio clip properties window
  void processAudioRecording();  // per frame (AudioRecordUi.cpp)

  // MIDI.
  std::unique_ptr<MidiManager> midi;
  bool midiLearning = false;   // the remote control dialog waits for a control (midiLearnTarget)
  ParamId midiLearnTarget;
  void processMidi();          // per frame: recording, learning, controller links, templates, sync
  // ---- J-17 keyboard / MIDI workflow (MidiWorkflow.cpp, MidiUi.cpp) ----
  RemoteMapper remote;                         // link takeover state
  std::vector<ControllerTemplate> controllerTemplates;
  void loadControllerTemplates();              // shipped + user templates
  std::string assetsDir;                       // set by main
  std::string templatesUserDir() const;        // <config>/midi-templates
  // Remote control settings dialog ("Link to controller…"): edits remoteEdit;
  // remoteEditIndex = index in Song::links, -1 = a new link.
  bool showRemoteDialog = false;
  bool remoteListening = false;                // the dialog takes the next control moved
  ControllerLink remoteEdit;
  int remoteEditIndex = -1;
  void openRemoteDialog(ParamId target);
  void openRemoteDialog(RemoteAction action);
  void openRemoteDialogForLink(int index);
  // Multilink: move controls in Gridline, then hardware controls in the same
  // order; each hardware control links to the next touched control.
  bool multilink = false;
  std::vector<ParamId> multilinkQueue;
  uint64_t multilinkSerial_ = 0;
  // Focused plugin window (template knobs drive its first parameters).
  ObjectId focusPluginOwner = kNoId;
  int focusPluginSlot = -1;
  // Recording.
  RecordOptions recOptions;
  int countInBars = 0;                         // count-in before recording starts
  bool stepRecord = false;                     // step editing from the keyboard (stopped transport)
  NoteRecorder recorder;
  StepRecorder stepRecorder;
  TypingKeyboard typing;
  // Starts playback (with the count-in when recording).
  void startPlayback();
  void runAction(RemoteAction action, bool fromMmc = false);
  // Notes played by any source (MIDI, typing keyboard): recording and step record.
  void playedNoteOn(ObjectId channel, int pitch, int velocity);
  void playedNoteOff(ObjectId channel, int pitch);
  void publishMidi();                          // routing / sync settings → engine and drivers
  double tapTimes_[4] = {0, 0, 0, 0};
  int tapCount_ = 0;
  void importMidi(const std::string& path);
  // J-52: FL Studio projects (.flp) open as projects: the converted song,
  // shown under the FL file's name (`openedFrom`) with the import report; Save
  // asks where and in which format (never writes over the .flp unasked).
  // Zipped projects: Gridline zips and FL Studio zipped loop packages are
  // unpacked beside the archive and opened from there.
  // J-167: every project opens through the project loader (openProject).
  projload::Loader projectLoad;
  // A project is being read or prepared: the loading window shows (after a short delay) and
  // blocks every input to the project behind.
  bool projectLoading() const { return projectLoad.active() || projectLoad.phase() == projload::Phase::Ready; }
  // The loading window is up (loading, or showing why a project could not open).
  bool projectLoadBlocking() const { return projectLoading() || projectLoad.phase() == projload::Phase::Failed; }
  // Runs the current load to its end now (no frames): embedded builds, tools, the launcher dev view.
  void finishProjectLoad();
  // Cancel: the open project stays exactly as it was (the launcher comes back if it was up).
  void cancelProjectLoad();
  // The failure window's Close.
  void dismissProjectLoad();
  // Quit: a running load is cancelled and its reader joined.
  void shutdownProjectLoad();
  const std::string& projectLoadName() const { return load_.shownAs; }  // the file being opened
  struct ImportReport {
    std::string source;                   // file name
    std::vector<std::pair<std::string, int>> counts;
    std::vector<std::pair<std::string, int>> unmapped;
    std::vector<std::pair<std::string, int>> mapped;  // J-94: "FL plugin → Gridline plugin"
    std::vector<std::string> warnings;
  } importReport;
  void exportMidi(const std::string& path, bool patternOnly);

  FileDialogState fileDialog;
  std::vector<Toast> toasts;
  std::string hint;  // status/hint bar text for this frame
  std::string focusWindow;  // window to bring to front next frame

  // Fonts (set by main).
  ImFont* fontRegular = nullptr;
  ImFont* fontBold = nullptr;
  ImFont* fontLarge = nullptr;
  ImFont* fontMono = nullptr;
  ImFont* fontMonoBold = nullptr;

  // ---- actions ----
  void newProject();
  bool openProject(const std::string& path);
  // Saves in the format the file name says (.flp, .zip FL package, else Gridline).
  bool saveProject(const std::string& path);
  // J-80: FL Studio formats are written by a background job (instruments
  // rendered to samples): the save completes, and `then` runs, when it ends.
  bool saveProjectAs(const std::string& path, SaveFormat format, std::function<void()> then = {});
  void save();     // to projectPath, or Save As
  // Save dialog with the format list (Gridline, FL Studio project, FL Studio
  // zipped loop package); `then` runs after a successful save.
  void saveAs(std::function<void()> then = {});
  void openDialog();
  void exportAudio(const std::string& path, int bits, int sampleRate, bool patternOnly, bool normalize);
  void guardUnsaved(std::function<void()> action);  // runs now, or after the user confirms

  ObjectId addInstrument(const std::string& pluginType, const std::string& samplePath = "");
  void selectPattern(ObjectId id);
  void selectChannel(ObjectId id, bool additive = false);
  void openPianoRoll(ObjectId channel);
  void previewSample(const std::string& path);
  void stopPreview();
  // A notification card; `details` (path, code) is not shown but is copied
  // with the message when the card is clicked (J-78).
  void notify(const std::string& text, bool error = false, const std::string& details = "");
  void setHint(const std::string& text) { hint = text; }
  std::string windowTitle() const;
  std::string configDir() const;
  void addRecent(const std::string& path);
  // J-141: recent list management (the launcher): saved at once.
  bool removeRecent(const std::string& path);
  size_t removeMissingRecents();
  void clearRecents();  // J-148: favourites stay
  int64_t recentOpenedAt(const std::string& path) const;  // Unix seconds, 0 = unknown
  // J-148: favourite projects (a heart on their launcher tile; never pushed off the list).
  bool isFavouriteProject(const std::string& path) const { return recentFavourites_.count(path) != 0; }
  bool setFavouriteProject(const std::string& path, bool on);
  // The project moved (renamed folder or file): its entry moves to the front, favourite mark kept.
  void moveRecent(const std::string& oldPath, const std::string& newPath);
  size_t favouriteProjectCount() const { return recentFavourites_.size(); }
  // J-141: Close project (launcher): asks about unsaved changes, then shows the launcher
  // with no project open.
  void closeProject();
  // J-141: the launcher's sketch cache folder (<config>/launcher/sketches).
  std::string sketchCacheDir() const;

  // ---- J-48 interface sounds (engine/UiSounds.h) ----
  uisound::Player uiSounds;          // mixed after the master in the audio callback (main.cpp)
  uisound::Gate uiSoundGate;         // per-event cool-down
  bool uiSoundsDisabled = false;     // --no-ui-sounds, GRIDLINE_NO_UI_SOUNDS, screenshot / profile runs
  bool uiSoundsReady = false;        // set with the startup sound, once the window is up (no sounds while launching)
  bool scanByUser = false;           // the running plugin scan was started from the UI (its end gets a sound)
  // Plays an interface sound if the settings and the muting rules allow it.
  bool uiSound(uisound::Event e);
  // J-133: sound themes (setting "sound.theme", assets/sounds/<theme>/). Called when the
  // device opens (sync: the device is paused) and when the setting changes (async: the
  // clips load on a worker thread and are handed to the player when ready, see tick()).
  void loadSoundTheme(double deviceRate, bool sync);
  // Plays one event of any theme (Settings preview), at the interface-sound volume.
  bool previewSoundTheme(const std::string& theme, uisound::Event e = uisound::Event::Startup);
  std::string uiSoundTheme;          // theme of the clips installed or loading ("" = none yet)
  double uiSoundRate = 0;            // device rate the clips were loaded for

  double now = 0;  // seconds since start (frame time)
  std::string browserSearch;  // text put into the browser's search field once (--browser-search)

  // ---- menu actions (Menus.cpp / ContextMenus.cpp) ----
  // Editor that Edit-menu commands (cut/copy/paste/delete/select) act on: the
  // last focused of the four editors.
  enum class EditFocus { None, Rack, Piano, Playlist, Mixer };
  EditFocus editFocus = EditFocus::None;
  int settingsTab = -1;  // settings tab to bring to front on next open (-1 = keep)
  // Saves next to the current file as "<name> 2", "<name> 3", … (or Save As if never saved).
  void saveNewVersion();
  // Replaces the song with the newest automatic backup of this project (undoable).
  bool revertToLastBackup();
  // Templates: "" = empty project, "starter" = default kit, "demo" = demo song,
  // otherwise a template file path. The result is an unsaved project.
  void newFromTemplate(const std::string& which);
  bool saveAsTemplate(const std::string& name, const std::string& category = "", const std::string& description = "");
  std::vector<std::string> templateFiles() const;  // user templates (full paths)
  // Renders one WAV per used insert track (solo render) into `dir`. Returns the file count.
  int exportStems(const std::string& dir, int bits, int sampleRate);
  void closeAllWindows();
  void closePluginWindows();

  // ---- J-24: templates, startup, autosave, settings depth ----
  bool showNewProject = false;    // File > New from template... (template browser)
  bool showSaveTemplate = false;  // File > Save as template...
  // Replaces the project with an unsaved one made from `s` (templates, new project).
  void startProject(Song s);
  Song starterSong() const;  // built-in starter kit: four drum voices and a synth
  // New-project defaults from the settings (tempo, PPQ, time signature, author, pattern length).
  void applyProjectDefaults(Song& s) const;
  // Settings > General > On startup (after main has set assetsDir). The very first launch on a
  // machine (no settings file yet) opens the demo song instead, once.
  // J-141: `plainLaunch` (no command-line arguments) lets the welcome launcher show.
  void applyStartup(bool plainLaunch = false);
  // Log level / log file and language from the settings.
  void applySettingsHooks();
  std::string languageDir() const;  // assets/i18n: translation catalogues
  std::string templatesDir() const;  // <config>/templates (user templates)
  // Shipped + user templates (cached; rescan after saving or deleting one).
  const std::vector<TemplateInfo>& templates(bool rescan = false);
  const TemplateInfo* findTemplate(const std::string& key);  // key "Category/Name"

 private:
  uint64_t builtRevision_ = ~0ull;
  uint64_t builtSampleGen_ = 0;   // SampleCache::generation() of the last build (J-30)
  bool freshInstall_ = false;     // no settings file existed when the app started (J-135)
  std::map<std::string, int64_t> recentTimes_;  // J-141: last opened / saved per recent path
  std::set<std::string> recentFavourites_;       // J-148: favourite projects (listed paths)
  void saveRecents();
  // J-148: the recent list as one value (taken from / given back to the members above).
  recents::List takeRecents();
  void putRecents(recents::List&& l);
  bool openProjectFile(const std::string& path);  // openProject without the launcher hand-over
  // J-167: the project loader (ProjectLoadUi.cpp).
  void startProjectLoad(const std::string& path, bool collab);
  void pollProjectLoad();      // per frame (tick): a slice of work, the swap when ready
  void completeProjectLoad();  // the prepared project replaces the open one
  struct LoadContext {
    std::string shownAs;      // the file the user opened (recent list, window title)
    bool collab = false;      // a collaborative project (its session opens at the swap)
    launcher::State homeBefore;  // the launcher as it was (restored on cancel / failure)
    bool failReported = false;
  } load_;
  void rememberSketch(const std::string& path);  // J-141: the open song's sketch for the launcher
  double trimSamplesAt_ = -1;     // drop the replaced project's samples from the cache at this time (J-30)
  int64_t lastBackupCheck_ = 0;
  SamplePtr preview_;
  double lastCcTime_ = -1;
  double lastRecPos_ = 0;
  std::map<uint64_t, int> templateKnobDown_;  // template buttons: last pressed state
  int64_t lastAutosave_ = 0;
  std::vector<TemplateInfo> templates_;
  bool templatesScanned_ = false;
  bool autosaving_ = false;      // J-48: autosave writes quietly
  bool wasRecording_ = false;    // J-48: record start / stop sounds
  bool wasScanning_ = false;     // J-48: plugin scan finished sound
  std::future<std::unique_ptr<uisound::Bank>> soundThemeLoad_;  // J-133: theme loading off the UI thread
};

App& app();  // the single application instance
// PH-09: the instance app() returns (plugin builds run one per plugin
// instance, all on the host's main thread); returns the previous one.
App* setCurrentApp(App* a);

}  // namespace gl
