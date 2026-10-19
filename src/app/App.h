// Gridline — application state shared by all windows (UI thread only).
#pragma once
#include <atomic>
#include <thread>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "../core/Timeline.h"
#include "../core/Backup.h"
#include "../core/Document.h"
#include "../core/MidiRecord.h"
#include "../engine/AudioRecord.h"
#include "../core/MidiTemplates.h"
#include "../core/Settings.h"
#include "../core/Templates.h"
#include "../engine/AudioEngine.h"
#include "../engine/Sample.h"
#include "Midi.h"
#include "MixerState.h"

struct ImFont;

namespace gl {

// Grid snap choices shared by the piano roll and playlist.

// Editing tools of the piano roll and the playlist. J-11 added Slip (playlist:
// move a clip's content inside it), Play (audition / scrub while held), Zoom
// (drag a range, click in, right-click out) and Detune (drag notes' fine pitch).
enum class Tool { Draw, Paint, Erase, Select, Slice, Mute, Slip, Play, Zoom, Detune };

struct PianoRollState {
  float pxPerTick = 0.25f;
  float rowHeight = 14.0f;
  float scrollX = 0, scrollY = -1;  // -1: centre on C5 at first open
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
};

struct PlaylistState {
  float pxPerTick = 0.17f;
  float trackHeight = 46.0f;
  float scrollX = 0, scrollY = 0;
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
  int pickerTab = 0;                     // clip source picker: 0 patterns, 1 audio, 2 automation
  bool pickerOpen = true;                // picker panel shown (Alt+P)
  bool stretchResize = false;            // Shift+M: resizing audio clips stretches them
  bool showFades = true;                 // Shift+F: fade handles on audio clips
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
};

struct Toast {
  std::string text;
  double until = 0;
  bool error = false;
};

class App {
 public:
  App();
  ~App();

  // ---- document & engine ----
  Document doc;
  AudioEngine engine{44100.0};
  ProgramBuilder builder;
  Settings settings;
  std::unique_ptr<BackupManager> backups;
  std::string projectPath;  // "" = never saved
  std::vector<std::string> recentFiles;

  void syncEngine();  // rebuild the engine program after edits
  void tick(double now);  // per-frame housekeeping (backups, toasts, GC)

  // ---- selection ----
  ObjectId curPattern = kNoId;
  ObjectId selChannel = kNoId;
  std::set<ObjectId> selChannels;
  ObjectId selMixer = kNoId;
  MixerViewState mixerView;  // mixer layout, meters, multi-selection, armed tracks (J-12)
  ObjectId settingsChannel = kNoId;        // channel settings window target
  int channelTab = 0;                       // channel settings window tab (0 plugin, 1 misc)
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
  bool quitRequested = false;
  bool confirmDiscard = false;  // unsaved-changes prompt open
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
  int audioRecLoopRestore = -1;  // looping to restore after a session (-1 = unchanged)
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
  // FL Studio project import (.flp): opens the converted song as a new, unsaved project.
  bool importFlpProject(const std::string& path);
  struct ImportReport {
    std::string source;                   // file name
    std::vector<std::pair<std::string, int>> counts;
    std::vector<std::pair<std::string, int>> unmapped;
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
  bool saveProject(const std::string& path);
  void save();     // to projectPath, or Save As
  void saveAs();
  void openDialog();
  void exportAudio(const std::string& path, int bits, int sampleRate, bool patternOnly, bool normalize);
  void guardUnsaved(std::function<void()> action);  // runs now, or after the user confirms

  ObjectId addInstrument(const std::string& pluginType, const std::string& samplePath = "");
  void selectPattern(ObjectId id);
  void selectChannel(ObjectId id, bool additive = false);
  void openPianoRoll(ObjectId channel);
  void previewSample(const std::string& path);
  void stopPreview();
  void notify(const std::string& text, bool error = false);
  void setHint(const std::string& text) { hint = text; }
  std::string windowTitle() const;
  std::string configDir() const;
  void addRecent(const std::string& path);

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
  // Settings > General > On startup (after main has set assetsDir).
  void applyStartup();
  // Log level / log file and language from the settings.
  void applySettingsHooks();
  std::string templatesDir() const;  // <config>/templates (user templates)
  // Shipped + user templates (cached; rescan after saving or deleting one).
  const std::vector<TemplateInfo>& templates(bool rescan = false);
  const TemplateInfo* findTemplate(const std::string& key);  // key "Category/Name"

 private:
  uint64_t builtRevision_ = ~0ull;
  int64_t lastBackupCheck_ = 0;
  SamplePtr preview_;
  double lastCcTime_ = -1;
  double lastRecPos_ = 0;
  std::map<uint64_t, int> templateKnobDown_;  // template buttons: last pressed state
  int64_t lastAutosave_ = 0;
  std::vector<TemplateInfo> templates_;
  bool templatesScanned_ = false;
};

App& app();  // the single application instance

}  // namespace gl
