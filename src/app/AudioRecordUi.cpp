// Gridline — audio input & recording UI (J-8). See AudioRecordUi.h.
#include "ClipEditor.h"
#include "../engine/TimeStretch.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/ClipWindow.h"
#include "../core/I18n.h"
#include "J31Ui.h"
#include "LiveLook.h"
#include "DialogLayout.h"
#include "LiveTakes.h"
#include "AudioRecordUi.h"
#include "ClipRenderUi.h"
#include "SilenceUi.h"
#include "ClipToolsUi.h"
#include "../engine/TakeLanes.h"
#include "LogicPlugins.h"
#include "RecordRowsUi.h"
#include "AsioUi.h"
#include "ChopUi.h"
#include "ContextMenus.h"
#include "FreezeUi.h"
#include "Keys.h"
#include "Windows.h"
#include "../core/MenuParity.h"
#include "ClipMultiUi.h"
#include "StemsUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include "../core/Automation.h"
#include "../core/Meter.h"
#include "../core/PluginParams.h"
#include "../engine/ClipOps.h"
#include "../packs/PacksUi.h"
#include "Theme.h"
#include "UrlImportUi.h"
#include "Widgets.h"
#include "RecordMoreUi.h"
#include "../engine/Loopback.h"

namespace fs = std::filesystem;

namespace gl {

using theme::pal;

namespace {

std::string projectSubdir(const App& a, const char* name) {
  if (!a.projectPath.empty()) return (fs::path(a.projectPath).parent_path() / name).string();
  return (fs::path(a.configDir()) / name).string();
}

bool anyArmed(const Song& s) {
  for (const auto& m : s.mixer)
    if (m.records()) return true;  // J-31: output taps record without an input
  return false;
}

int deviceInputs(const App& a) { return a.audioInputDevice.empty() ? 0 : a.engine.inputChannels(); }

// Development / screenshots: GRIDLINE_J8_DEMO=1 arms two inserts and commits
// synthetic recordings (a vocal-like line and three loop takes of a plucked
// part, comped) as if they had been recorded; =props also opens the clip
// properties of the first recording.
void devDemo(App& a) {
  static int frames = 0;
  const char* env = std::getenv("GRIDLINE_J8_DEMO");
  if (!env || !*env || ++frames != 3) return;
  const double sr = a.engine.sampleRate();
  const std::string mode = env;
  const std::string dir = recordedDir(a);
  ObjectId propsClip = kNoId;
  a.doc.editSong("J-8 demo", [&](Song& s) {
    s.tempo = 120;
    if (s.mixer.size() < 3) return;
    s.mixer[1].name = "Vocal";
    s.mixer[1].color = 0xC8724E;
    s.mixer[1].input = 0;
    s.mixer[1].inputStereo = false;
    s.mixer[1].armed = true;
    s.mixer[2].name = "Guitar";
    s.mixer[2].color = 0x6E9F4B;
    s.mixer[2].input = 0;
    s.mixer[2].armed = true;
    s.mixer[2].monitor = static_cast<uint8_t>(InputMonitor::On);
    const double spb = 60.0 / s.tempo;
    std::vector<RecordedTake> takes;
    RecordedTake v;  // vocal-like: harmonics through two formants on a slow line
    v.mixer = s.mixer[1].id;
    v.startTick = 0;
    const auto vn = static_cast<size_t>(16 * spb * sr);
    v.L.resize(vn);
    double ph = 0;
    for (size_t i = 0; i < vn; ++i) {
      const double t = static_cast<double>(i) / sr, beat = t / spb;
      const int note = (static_cast<int>(beat / 2) % 4 == 3) ? 64 : 60 + 2 * (static_cast<int>(beat / 2) % 3);
      const double f = 440.0 * std::pow(2.0, (note - 69) / 12.0) * (1 + 0.004 * std::sin(2 * M_PI * 5.5 * t));
      ph += f / sr;
      const double phrase = std::fmod(beat, 4.0);
      const double env2 = phrase < 3.2 ? std::min(1.0, phrase * 6) * std::min(1.0, (3.2 - phrase) * 4) : 0.0;
      double x = 0;
      for (int h = 1; h < 12; ++h)
        x += std::sin(2 * M_PI * ph * h) / h *
             (1.0 / (1 + std::pow((f * h - 700) / 300, 2)) + 0.4 / (1 + std::pow((f * h - 1200) / 400, 2)));
      v.L[i] = static_cast<float>(0.45 * env2 * x);
    }
    v.R = v.L;
    takes.push_back(std::move(v));
    for (int pass = 0; pass < 3; ++pass) {  // plucks: three loop takes over bars 3-4
      RecordedTake g;
      g.mixer = s.mixer[2].id;
      g.pass = pass;
      g.startTick = static_cast<double>(MeterMap(s).barStart(2));  // K-193: bar 3 of the song's meters
      const auto gn = static_cast<size_t>(8 * spb * sr * (pass == 2 ? 0.75 : 1.0));
      g.L.resize(gn);
      g.R.resize(gn);
      for (size_t i = 0; i < gn; ++i) {
        const double t = static_cast<double>(i) / sr, step = t / (spb / 2);
        const int k = static_cast<int>(step);
        const double u = (step - k) * spb / 2;
        const double f = 110.0 * std::pow(2.0, ((k * 5 + pass * 3) % 12) / 12.0);
        const double x = std::exp(-u * (6 + pass)) * (std::sin(2 * M_PI * f * u) + 0.4 * std::sin(4 * M_PI * f * u));
        g.L[i] = static_cast<float>(0.5 * x);
        g.R[i] = static_cast<float>(0.45 * x);
      }
      takes.push_back(std::move(g));
    }
    TakeCommitOptions o;
    o.dir = dir;
    o.sampleRate = sr;
    TakeCommitResult r = commitTakes(s, takes, o);
    if (r.clips.size() == 4) {
      Clip* vc = s.clip(r.clips[0]);
      vc->fadeIn = 0.25, vc->fadeInManual = true;
      vc->fadeOut = 0.6, vc->fadeOutManual = true;
      const ObjectId g = s.clip(r.clips[1])->takeGroup;
      const MeterMap mm(s);  // K-193: beats 3-4 of bar 3 in the meter in force there
      const Tick b3 = mm.barStart(2), beat = mm.beatLenAt(b3);
      compTakes(s, g, b3 + beat * 2, b3 + beat * 4, 0);
      if (mode == "props") propsClip = r.clips[0];
    }
  });
  if (propsClip != kNoId) clipeditor::open(a, propsClip);  // K-249: the audio clip editor
  const Song& s = a.doc.song();
  if (s.mixer.size() > 2) a.selMixer = s.mixer[2].id;
  const MeterMap mm(s);  // K-193: bars 3-4 of the song's meters
  a.playlist.selStart = mm.barStart(2);
  a.playlist.selEnd = mm.barStart(4);
  a.recordArmed = true;
  a.engine.setMode(PlayMode::Song);
}

}  // namespace

std::string recordedDir(const App& a) { return projectSubdir(a, "Recorded"); }
std::string renderedDir(const App& a) { return projectSubdir(a, "Rendered"); }
std::string editedDir(const App& a) { return projectSubdir(a, "Edited audio"); }
std::string seltmannLoopsDir(const App& a) { return projectSubdir(a, "Seltmann loops"); }

std::string inputName(int first, bool stereo) {
  if (first < 0) return "(none)";
  if (stereo) return "In " + std::to_string(first + 1) + " - In " + std::to_string(first + 2);
  return "In " + std::to_string(first + 1);
}

// ------------------------------------------------------------------ recording session

void App::processAudioRecording() {
  devDemo(*this);
  recmore::frame(*this);  // K-123 (0.7.7): quick punch, capture while armed, start on the first note, the latency check
  const Song& s = doc.song();
  // Latency compensation: device buffers (K-123: the measured round trip when one applies) + delay
  // compensation + manual offset.
  const double offMs = settings.getNumber("audio.inputOffsetMs");
  const int offset = static_cast<int>(std::lround(offMs * engine.sampleRate() / 1000.0));
  engine.setRecordLatency(loopback::recordLatency(audioInputLatency, recmore::measuredRoundTrip(*this), engine.outputLatency(), offset));

  const bool armedTracks = anyArmed(s);
  // Record filter "into the audio editor": arm the editors of the armed tracks
  // when the record button is armed (they start with playback).
  if (recordArmed && !audioRecArmedPrev && recAudioEditor) {
    doc.beginGroup("Record into Loupe");
    std::vector<ObjectId> ids;
    for (const auto& m : s.mixer)
      if (m.armed && m.input >= 0) ids.push_back(m.id);
    for (ObjectId id : ids)
      if (packs::armAudioEditorRecording(*this, id) < 0) notify("No free effect slot for Loupe", true);
    doc.endGroup();
  }
  audioRecArmedPrev = recordArmed;

  // K-112c (checkpoint 48, N47-01): Clips' Session record of an armed audio track records in the clip
  // launch mode, into a grid slot (livelook::audioTakesCommitted moves the take there).
  const bool sessAudio = livelook::sessionRecordAudio(*this);
  const bool rolling = engine.playing() && (engine.mode() != PlayMode::Live || sessAudio);
  // K-123 (0.7.7) quick punch: once recording, the session stays open while the transport plays, so
  // Record punches out and in again inside one take (one undo step).
  const bool qpOpen = recmore::quickPunch(*this) && audioRecSession && !audioRecFinishing && recAudio && !sessAudio;
  const bool want = ((recordArmed && recAudio) || sessAudio || qpOpen) && rolling && armedTracks;
  const bool song = engine.mode() == PlayMode::Song;
  // Checkpoint 56 (N55-04): Clips loops (and punches) at its loop brace (the song's cycle range), never at
  // the time selection; the other layouts at the time selection, as before.
  const bool clipsLook = livelook::active(*this);
  Tick punchA = 0, punchB = 0;
  const bool punchRange = livelook::recordRange(*this, punchA, punchB);
  const bool sel = clipsLook ? livelook::loopOn(*this) : punchRange;
  // J-175: what records besides armed tracks: a deck setup's performance passes (Platter)
  const bool packRecords = recordArmed && packs::recordsWithTransport(*this);
  // Song mode without a time selection: keep recording past the song end. Set while the record
  // button is armed and something records, before the transport rolls, so the song never wraps
  // under a recording (J-175: a wrap ends a deck pass there and starts another one); looping
  // comes back once nothing records any more (stopped, disarmed, another mode).
  // Clips: Arrangement Record takes MIDI too, and a take ends where recording stopped (N50-06).
  // R06-01: the Tracks layout loops only inside the cycle; without one playback and recording run
  // on past the song end (a new project's one bar never wraps a take onto itself).
  const bool pastEnd = song && !sel && ((recordArmed && ((armedTracks && recAudio) || packRecords || livelook::active(*this))) || logicplug::active(*this));
  if (pastEnd && audioRecLoopRestore < 0) {
    engine.setLooping(false);
    audioRecLoopRestore = 1;
  } else if (audioRecLoopRestore >= 0 && !pastEnd && !(song && rolling && (recordArmed || audioRecSession))) {
    engine.setLooping(audioRecLoopRestore > 0);
    audioRecLoopRestore = -1;
  }
  // Punch in / out at the time selection (song mode), for everything that records: set as
  // recording starts (J-175: deck passes too, not only audio sessions).
  auto setPunch = [&] {
    if (recPunch && punchRange && song)
      engine.setPunch(static_cast<double>(punchA), static_cast<double>(punchB));
    else
      engine.setPunch(0, 0);
  };
  const bool recRolling = recordArmed && rolling;
  // K-123: the punch range is known before Play (pre-roll starts ahead of it), and the pre / post-roll.
  if (recordArmed && !rolling) setPunch();
  {
    const double beat = static_cast<double>(doc.song().ppq);
    const double pre = settings.getBool("record.preRoll") ? beat * std::max(0.0, settings.getNumber("record.preRollBeats")) : 0.0;
    const double post = settings.getBool("record.postRoll") ? beat * std::max(0.0, settings.getNumber("record.postRollBeats")) : 0.0;
    engine.setRecordRoll(recordArmed && recPunch && punchRange && song, pre, post);
  }
  if (recRolling && !recRollingPrev) setPunch();
  recRollingPrev = recRolling;
  if (want && !audioRecSession) {
    audioRec.begin();
    audioRecSession = true;
    audioRecFinishing = false;
    setPunch();
  }
  // Hosted plugins see "recording" whenever the record button is armed and the
  // transport rolls; armed tracks capture while it is set.
  if (!recmore::waiting()) engine.setRecording((recordArmed || sessAudio) && rolling && !audioRecFinishing);  // (K-123: waiting, the engine may start it)
  if (!audioRecSession) {
    livetake::reset();  // J-217
    return;
  }
  if (const auto& p = engine.program()) audioRec.pump(engine, ringsOf(*p));
  livetake::update(*this);  // J-217: the takes drawn live on the playlist
  if (!want && !audioRecFinishing) {
    engine.setRecording(false);
    audioRecFinishing = true;
    audioRecStopTime = now;
  }
  // Commit once the engine has closed every take (the latency tail) and the
  // playlist has shown them complete for a frame (J-217: the committed clips
  // look exactly like the last live frame), or after a second.
  if (audioRecFinishing && ((!audioRec.open() && livetake::settled()) || now - audioRecStopTime > 1.0)) {
    if (const auto& p = engine.program()) audioRec.pump(engine, ringsOf(*p));
    std::vector<RecordedTake> takes = audioRec.release();
    takes.erase(std::remove_if(takes.begin(), takes.end(), [](const RecordedTake& t) { return t.frames() < 64; }), takes.end());
    // J-31: cancelled sessions keep nothing; silent takes can be dropped.
    if (j31::takeCancelRequested()) takes.clear();
    recmore::beforeCommit(*this, takes);  // K-123 (0.7.7): punched takes reach into the crossfades
    if (settings.getBool("record.ignoreSilent"))
      if (const int n = dropSilentTakes(takes, -60.0f); n > 0) notify(std::to_string(n) + " silent take" + (n == 1 ? "" : "s") + " dropped");
    if (!takes.empty()) {
      TakeCommitOptions o;
      o.dir = recordedDir(*this);
      const int rb = static_cast<int>(settings.getNumber("audio.recordBits"));
      o.bits = rb == 24 || rb == 16 ? rb : 32;  // J-27: 16-bit too
      o.sampleRate = engine.sampleRate();
      o.naming = settings.getNumber("record.naming") > 0.5 ? TakeCommitOptions::Naming::Input : TakeCommitOptions::Naming::Insert;
      o.nameTracks = settings.getBool("record.nameTracks");
      o.lanes = settings.getBool("record.takeLanes");
      o.punchFade = recmore::punchFadeSeconds(*this);  // (only takes still marked as punches crossfade)
      TakeCommitResult r;
      const bool clipsTake = livelook::audioTakePending(*this);  // checkpoint 49: the take and its move into the slot are one undo step
      if (clipsTake) doc.beginGroup("Record audio");
      doc.editSong("Record audio", [&](Song& x) { r = commitTakes(x, takes, o); });
      livetake::handoff(takes, r);  // J-217: the clips draw from the live summaries
      if (!r.error.empty()) notify("Recording: " + r.error, true);
      if (livelook::audioTakesCommitted(*this, r.clips)) r.clips.clear();  // Clips: into the grid slot (it said so)
      if (clipsTake) doc.endGroup();
      char b[128];
      std::snprintf(b, sizeof b, "Recorded %zu audio clip%s into the playlist (%s)", r.clips.size(), r.clips.size() == 1 ? "" : "s",
                    fs::path(o.dir).filename().string().c_str());
      if (!r.clips.empty()) {
        notify(b);
        playlist.selected.clear();
        playlist.selected.insert(r.clips.begin(), r.clips.end());
      }
    }
    if (settings.getBool("record.autoUnarm") && !j31::takeCancelRequested() && anyArmed(doc.song()))  // J-31
      doc.editSong("disarm tracks after recording", [](Song& x) {
        for (auto& m : x.mixer) m.armed = false;
      });
    livetake::reset();
    audioRecSession = audioRecFinishing = false;  // (looping comes back on the next frame, above)
  }
}

// ------------------------------------------------------------------ record filter menu

void audioEditorRecordItem(App& a) { ImGui::MenuItem(i18n::tr("Audio, into Loupe (armed tracks)"), nullptr, &a.recAudioEditor); }

void audioRecordFilterMenu(App& a) {
  ImGui::SeparatorText(i18n::tr("What to record"));  // K-123: quiet group headings
  // N47-13: Clips has no playlist: its arrangement and grid take the clips.
  // R05-18: the Tracks layout words it as its tracks and regions.
  ImGui::MenuItem(livelook::active(a)    ? i18n::tr("Audio, as audio clips")
                  : logicplug::active(a) ? i18n::tr("Audio, into the tracks as audio regions")
                                         : i18n::tr("Audio, into the playlist as audio clips"),
                  nullptr, &a.recAudio);
  if (!livelook::active(a)) {
    audioEditorRecordItem(a);  // N49-12: Studio's editor (K-314: Clips has it under Advanced)
  }
  ImGui::MenuItem(i18n::tr("Notes"), nullptr, &a.recOptions.notes);
  ImGui::MenuItem(i18n::tr("Automation (controller moves)"), nullptr, &a.recOptions.automation);
  recordrows::filterMenuItems(a);  // K-123 (4.1): pitch bend, pressure, program changes
  if (ImGui::MenuItem(i18n::tr("Everything"))) {
    a.recAudio = a.recOptions.notes = a.recOptions.automation = a.recOptions.bend = a.recOptions.pressure = a.recOptions.program = true;
  }
  const Song& s = a.doc.song();
  int armed = 0;
  for (const auto& m : s.mixer) armed += m.armed && m.input >= 0;
  // K-325: a note on the rows' label column (it was loose text at the menu's left edge); not a row.
  const std::string note = armed == 0 ? std::string(i18n::tr("No mixer track is armed for audio"))
                                      : i18n::plural(armed, "%d mixer track armed for audio", "%d mixer tracks armed for audio");
  if (const ImGuiWindow* w = ImGui::GetCurrentWindow()) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w->DC.MenuColumns.OffsetLabel);
  ImGui::TextDisabled("%s", note.c_str());
}

// K-123: the audio file format submenu (Recording options > Audio takes).
void audioFormatMenu(App& a) {
  if (ImGui::BeginMenu(i18n::tr("Audio file format"))) {
    const int bits = static_cast<int>(a.settings.getNumber("audio.recordBits"));
    if (ImGui::MenuItem(i18n::tr("32-bit float WAV"), nullptr, bits != 24 && bits != 16)) a.settings.set("audio.recordBits", 32);
    if (ImGui::MenuItem(i18n::tr("24-bit WAV"), nullptr, bits == 24)) a.settings.set("audio.recordBits", 24);
    if (ImGui::MenuItem(i18n::tr("16-bit WAV"), nullptr, bits == 16)) a.settings.set("audio.recordBits", 16);
    ImGui::EndMenu();
  }
}

// ------------------------------------------------------------------ mixer

// K-123: record safe on a mixer track (it disarms the track); one undo step.
void setRecordSafe(App& a, ObjectId track, bool on) {
  const MixerTrack* t = a.doc.song().mixerTrack(track);
  if (!t || track == a.doc.song().masterId() || t->recordSafe == on) return;
  a.doc.editMixerTrack(track, on ? "record safe on" : "record safe off", [on](MixerTrack& m) {
    m.recordSafe = on;
    if (on) m.armed = false;
  });
}

void toggleArm(App& a, ObjectId track) {
  const MixerTrack* t = a.doc.song().mixerTrack(track);
  if (!t || track == a.doc.song().masterId()) return;
  const bool on = !t->armed;
  if (on && t->recordSafe) {  // K-123
    a.notify(i18n::tr("This track is record safe: turn Record safe off (right-click its record button) to arm it."));
    return;
  }
  const int inputs = deviceInputs(a);
  a.doc.editMixerTrack(track, on ? "arm track for recording" : "disarm track", [&](MixerTrack& m) {
    m.armed = on;
    if (on && m.input < 0) {  // no input yet: the first input (pair when there is one)
      m.input = 0;
      m.inputStereo = inputs != 1;
    }
  });
  if (on && a.audioInputDevice.empty()) a.notify("Armed. Choose an input device in Settings \xE2\x86\x92 Audio to record.");
}

bool stripArmButton(App& a, const MixerTrack& t, ImVec2 pos) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(pos);
  const bool clicked = ImGui::InvisibleButton("arm", ImVec2(16, 14));
  const bool hov = ImGui::IsItemHovered();
  ui::Hint(t.recordSafe ? "Record safe: this track can't be armed (right-click to change)"
           : t.armed    ? "Disk record: armed (click to disarm, right-click for record safe)"
                        : "Disk record: click to arm this track for audio recording (right-click for record safe)");
  ui::Tooltip(t.recordSafe ? "Record safe" : t.armed ? "Disarm" : "Arm for recording");
  const ImVec2 c(pos.x + 8, pos.y + 7);
  if (ImGui::BeginPopupContextItem("armctx")) {  // K-123: record safe
    if (ImGui::MenuItem(i18n::tr("Record safe"), nullptr, t.recordSafe)) setRecordSafe(a, t.id, !t.recordSafe);
    ImGui::EndPopup();
  }
  if (t.recordSafe) {
    ui::drawIcon(dl, ui::Icon::Lock, c, 11, hov ? P.text : P.textDim);
    if (clicked) toggleArm(a, t.id);
    return clicked;
  }
  const bool rolling = a.engine.recordingOn() && t.armed;
  const ImU32 ring = t.armed ? P.danger : hov ? P.text : P.textFaint;
  ui::drawIconFitC(dl, ui::Icon::Record, c, ImVec2(10, 10),
                   t.armed ? theme::withAlpha(P.danger, rolling ? 0.55f + 0.45f * static_cast<float>(std::fabs(std::sin(a.now * 4))) : 0.85f) : P.field);
  ui::drawIconFitC(dl, ui::Icon::Ring, c, ImVec2(11.2f, 11.2f), ring);
  if (clicked) toggleArm(a, t.id);
  return clicked;
}

void inputPeaks(App& a, ObjectId track, float& l, float& r) {
  // K-123: read once a frame (the engine's peaks reset on read), so the input panel and the
  // Recording card both see them.
  struct Read {
    int frame = -1;
    float l = 0, r = 0;
  };
  static std::map<ObjectId, Read> cache;
  Read& c = cache[track];
  if (c.frame != ImGui::GetFrameCount()) {
    c.frame = ImGui::GetFrameCount();
    c.l = c.r = 0;
    if (auto rt = a.builder.mixerRuntime(track)) c.l = rt->inPeakL.exchange(0), c.r = rt->inPeakR.exchange(0);
  }
  l = c.l, r = c.r;
}

void mixerInputPanel(App& a, const MixerTrack& t, float width) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  if (t.id == s.masterId()) return;
  ImGui::PushID("input");
  ui::Label("INPUT");
  const int devCh = deviceInputs(a);
  const int listCh = std::max(devCh, 2);
  const float comboW = std::max(80.0f, width - 64);
  if (ui::BeginCombo("##in", inputName(t.input, t.inputStereo).c_str(), comboW)) {
    auto pick = [&](int first, bool stereo) {
      a.doc.editMixerTrack(t.id, "mixer track input", [&](MixerTrack& m) {
        m.input = first;
        m.inputStereo = stereo;
        if (first < 0) m.armed = false;
      });
    };
    if (ImGui::Selectable(i18n::tr("(none)"), t.input < 0)) pick(-1, true);
    for (int c = 0; c + 1 < listCh; c += 2) {
      std::string n = inputName(c, true);
      if (ImGui::Selectable(n.c_str(), t.input == c && t.inputStereo)) pick(c, true);
    }
    for (int c = 0; c < listCh; ++c) {
      std::string n = inputName(c, false);
      if (ImGui::Selectable(n.c_str(), t.input == c && !t.inputStereo)) pick(c, false);
    }
    ImGui::EndCombo();
  }
  ui::Hint(devCh > 0 ? "Device input feeding this track (stereo pairs or single inputs)"
                     : "Device input feeding this track \xC2\xB7 no input device open: Settings \xE2\x86\x92 Audio");
  ImGui::SameLine(0, theme::kS1);
  if (t.recordSafe) {  // K-123
    if (ui::TextButton("Safe", t.recordSafe, 56, "Record safe: this track can't be armed. Click to allow recording again"))
      setRecordSafe(a, t.id, false);
  } else if (ui::TextButton(t.armed ? "Armed" : "Arm", t.armed, 56, "Disk record: arm this track (the record button then records its input). Right-click: record safe"))
    toggleArm(a, t.id);
  if (!t.recordSafe && ImGui::BeginPopupContextItem("armsafe")) {
    if (ImGui::MenuItem(i18n::tr("Record safe"))) setRecordSafe(a, t.id, true);
    ImGui::EndPopup();
  }
  // Monitoring + input meter.
  const char* mon[] = {"Auto", "On", "Off"};
  const char* monHints[] = {"Hear the input while the track is armed (around a punch range only inside it, unless Input only monitoring is on), and whenever the transport is stopped",
                            "Always hear the input through the track", "Never hear the input (it is still recorded)"};
  int m = std::clamp(static_cast<int>(t.monitor), 0, 2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Monitor"));
  ImGui::SameLine(0, theme::kS1);
  if (ui::Segmented("##mon", mon, 3, &m, 40, 0, monHints))
    a.doc.editMixerTrack(t.id, "input monitoring", [m](MixerTrack& x) { x.monitor = static_cast<uint8_t>(m); });
  // K-123: input trim and polarity (before the meter, the monitoring and the recording).
  {
    float trim = t.inputTrim;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Trim"));
    ImGui::SameLine(0, theme::kS1);
    ImGui::SetNextItemWidth(std::max(60.0f, width - 110));
    if (ImGui::DragFloat("##trim", &trim, 0.1f, kInputTrimMin, kInputTrimMax, "%+.1f dB", ImGuiSliderFlags_AlwaysClamp))
      a.doc.editMixerTrack(t.id, "input trim", [trim](MixerTrack& x) { x.inputTrim = trim; }, (static_cast<uint64_t>(t.id) << 8) ^ 0x123A);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && t.inputTrim != 0.0f)
      a.doc.editMixerTrack(t.id, "input trim", [](MixerTrack& x) { x.inputTrim = 0.0f; });
    ui::Hint("Input gain before the track's effects, -24 to +24 dB (double-click: 0 dB)");
    ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton("\xC3\x98", t.inputInvert, 30, "Invert the input's polarity (before the effects and the recording)"))
      a.doc.editMixerTrack(t.id, t.inputInvert ? "input polarity normal" : "invert input polarity", [](MixerTrack& x) { x.inputInvert = !x.inputInvert; });
  }
  float pl = 0, pr = 0;
  inputPeaks(a, t.id, pl, pr);
  if (t.input < 0) pl = pr = 0.f;
  // Input level (horizontal meter on its own row), with K-123's peak hold and clip lamp.
  const float lampW = 12.0f;
  const float mw = width - lampW - 4;
  const ImVec2 mp = ImGui::GetCursorScreenPos();
  ui::Meter(ImGui::GetID("inmeter"), pl, pr, ImVec2(mw, 8), false);
  ui::Hint("Input level (after the trim, before the track's effects)");
  {
    struct Hold {
      float peak = 0;
      double at = 0;
      bool clip = false;
    };
    static std::map<ObjectId, Hold> holds;
    Hold& h = holds[t.id];
    const float lvl = std::max(pl, pr);
    if (lvl >= h.peak || a.now - h.at > 1.5) h.peak = lvl, h.at = a.now;
    if (lvl >= 1.0f) h.clip = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (h.peak > 0.001f) {  // the held peak: a tick on a -60..0 dB scale
      const float db = 20.0f * std::log10(h.peak);
      const float x = mp.x + mw * std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
      dl->AddLine(ImVec2(x, mp.y), ImVec2(x, mp.y + 8), h.peak >= 1.0f ? P.danger : P.text, 1.5f);
    }
    const ImVec2 l0(mp.x + mw + 4, mp.y), l1(mp.x + width, mp.y + 8);
    dl->AddRectFilled(l0, l1, h.clip ? P.danger : P.field, 2.0f);
    const ImVec2 back = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(l0);
    if (ImGui::InvisibleButton("##clip", ImVec2(lampW, 8))) h.clip = false;
    ui::Hint(h.clip ? "The input clipped (it reached 0 dBFS): click to clear" : "Clip lamp: lights when the input reaches 0 dBFS");
    ImGui::SetCursorScreenPos(back);
  }
  ImGui::Dummy(ImVec2(0, theme::kS1));
  ImGui::PopID();
}

// ------------------------------------------------------------------ audio clips

namespace {

void editSongClip(App& a, const char* label, const std::function<void(Song&)>& fn) { a.doc.editSong(label, fn); }

void compMenu(App& a, const Clip& c) {
  const Song& s = a.doc.song();
  if (c.takeGroup == kNoId) return;
  if (!ImGui::BeginMenu(i18n::tr("Takes"))) return;
  const bool sel = a.playlist.selEnd > a.playlist.selStart;
  const Tick a0 = sel ? a.playlist.selStart : c.start, b0 = sel ? a.playlist.selEnd : c.start + s.clipLength(c);
  ImGui::TextDisabled(sel ? "Play in the time selection" : "Play in this clip's range");
  for (int k : takesInGroup(s, c.takeGroup)) {
    std::string n = "Take " + std::to_string(k + 1);
    if (ImGui::MenuItem(n.c_str(), nullptr, k == c.take && !c.muted)) {
      ObjectId g = c.takeGroup;
      a.doc.editSong("comp take", [&](Song& x) { compTakes(x, g, a0, b0, k); });
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Select all takes"))) {
    a.playlist.selected.clear();
    for (const auto& k : s.playlist)
      if (k.takeGroup == c.takeGroup) a.playlist.selected.insert(k.id);
  }
  // K-123: take lanes.
  const ObjectId g = c.takeGroup;
  if (takes::spreadOverTracks(s, g)) {
    if (ImGui::MenuItem(i18n::tr("Move the takes into lanes"))) a.doc.editSong("move takes into lanes", [&](Song& x) { takes::gatherIntoLanes(x, g); });
    ui::Hint("Puts every take on the first take's track as lanes to comp from");
  } else {
    const int tr = c.track;
    const bool shown = tr >= 0 && static_cast<size_t>(tr) < s.tracks.size() && s.tracks[static_cast<size_t>(tr)].takeLanes;
    if (ImGui::MenuItem(i18n::tr("Show take lanes"), nullptr, shown))
      a.doc.editPlaylistTrack(tr, shown ? "hide take lanes" : "show take lanes", [shown](PlaylistTrack& t) { t.takeLanes = !shown; });
  }
  if (ImGui::MenuItem(i18n::tr("Flatten the comp"))) {
    std::string err;
    ObjectId made = kNoId;
    const std::string dir = recordedDir(a);
    a.doc.editSong("flatten comp", [&](Song& x) { made = takes::flatten(x, g, dir, &err); });
    a.notify(made != kNoId ? std::string("The comp is one clip now (undo brings the takes back)") : "Nothing to flatten: " + err);
  }
  if (ImGui::MenuItem(i18n::tr("Remove unused takes"))) {
    int n = 0;
    a.doc.editSong("remove unused takes", [&](Song& x) { n = takes::removeUnused(x, g); });
    if (n == 0) a.notify("Every take plays somewhere: nothing removed");
  }
  ImGui::EndMenu();
}

}  // namespace

void audioClipTempoItems(App& a, ObjectId id) {
  const Clip* c = a.doc.song().clip(id);
  if (!c || c->kind != ClipKind::Audio) return;
  if (ImGui::MenuItem(i18n::tr("Fit to tempo"))) {
    int beats = 0;
    editSongClip(a, "fit clip to tempo", [&](Song& x) { beats = fitClipToTempo(x, id); });
    if (beats > 0) a.notify("Fitted to " + std::to_string(beats) + (beats == 1 ? " beat" : " beats") + "; the clip now follows the tempo");
    return;
  }
  if (ImGui::MenuItem(i18n::tr("Normalize"))) editSongClip(a, "normalize clip", [&](Song& x) { normalizeClip(x, id, 0.0); });
}

void audioClipMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c || c->kind != ClipKind::Audio) return;
  const Clip clip = *c;
  if (ImGui::MenuItem(i18n::tr("Edit clip..."))) clipeditor::open(a, id, clipwin::entryTab(clipwin::Entry::EditClip));  // the Audio clip window
  ui::Hint("The Audio clip window: every setting of the clip and its sample, its waveform, warp markers and audio edits (also a double-click)");
  compMenu(a, clip);
  if (ImGui::BeginMenu(i18n::tr("Time stretch"))) {
    const Channel* ch = s.channel(clip.source);
    const bool keep = ch && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode) && ch->plugin.params[pp::SmpStretchMode] >= 0.5f;
    const stretch::Mode cur = ch && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode)
                                  ? stretch::choiceOf(stretch::modeOf(ch->plugin.params[pp::SmpStretchMode]))
                                  : stretch::Mode::Resample;
    // J-87 / J-153 / J-157: Stretch (pitch kept), Rubber Band or Resample, the current one checked.
    // Checkpoint 59 (N58-12): not in Clips (one stretch and pitch engine, no per-material modes).
    if (!livelook::active(a)) {
      for (const stretch::Mode m : stretch::kChoices) {
        if (ImGui::MenuItem(i18n::tr(stretch::modeName(m)), nullptr, m == cur))
          editSongClip(a, "stretch mode", [&](Song& x) { setClipStretchMode(x, id, clip.stretch, static_cast<int>(m)); });
        ui::Tooltip(stretch::modeHint(m));
      }
      ImGui::Separator();
    }
    if (ImGui::MenuItem(i18n::tr("Half as long"))) editSongClip(a, "stretch clip", [&](Song& x) { setClipStretch(x, id, clip.stretch * 0.5, keep); });
    if (ImGui::MenuItem(i18n::tr("Twice as long"))) editSongClip(a, "stretch clip", [&](Song& x) { setClipStretch(x, id, clip.stretch * 2.0, keep); });
    if (ImGui::MenuItem(i18n::tr("Original length"), nullptr, false, clip.stretch != 1.0))
      editSongClip(a, "stretch clip", [&](Song& x) { resetClipStretch(x, id); });  // J-193: unstretched: fixed seconds again
    ImGui::EndMenu();
  }
  stemsui::clipMenuItem(a, id);  // J-191: vocals / drums / bass / other onto new tracks
  chopui::clipMenu(a, id);  // PL-13: slice at the grid / into parts / at transients, stutter presets
  if (ImGui::BeginMenu(i18n::tr("Silence"))) {  // K-253: trim the ends / remove every silence, side by side
    const bool done = silenceui::clipItems(a, id);
    ImGui::EndMenu();
    if (done) return;
  }
  cliptoolsui::clipMenu(a, id);  // Loupe's tools on the clip (each clip a new file, one undo step)
  ImGui::Separator();
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(id)) sel = {id};
  if (cliprenderui::menuItems(a, id)) return;  // K-250 / K-251: to audio in place, to a file (each clip its own)
  if (sel.size() > 1 && ImGui::MenuItem(i18n::tr("Consolidate selected clips to audio"))) {  // several clips into one
    const std::string dir = renderedDir(a);
    const double sr = a.engine.sampleRate();
    ObjectId nid = kNoId;
    editSongClip(a, "consolidate clips", [&](Song& x) { nid = consolidateClips(x, sel, dir, sr, true); });
    if (nid != kNoId) a.playlist.selected = {nid};
    else a.notify("Nothing to render", true);
    return;
  }
  if (ImGui::MenuItem(i18n::tr("Bounce this track to audio"))) {
    const std::string dir = renderedDir(a);
    const double sr = a.engine.sampleRate();
    const int tr = clip.track;
    ObjectId nid = kNoId;
    editSongClip(a, "bounce track", [&](Song& x) { nid = bounceTrack(x, tr, dir, sr); });
    if (nid != kNoId) a.notify("Track bounced to audio: the original clips are muted");
  }
}

void clipsAudioClipMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c || c->kind != ClipKind::Audio) return;
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(id)) sel = {id};
  // An item that edits the song returns at once: `s` and `c` point into the song it replaced.
  if (!gClipMenuFromSlot) {
    if (ImGui::MenuItem(i18n::tr("Cut"))) {
      a.playlist.selected = sel;
      menus::editCut(a);
      return;
    }
    if (ImGui::MenuItem(i18n::tr("Copy"), keys::label("playlist.copy"))) {
      a.playlist.selected = sel;
      menus::editCopy(a);
    }
    if (ImGui::MenuItem(i18n::tr("Paste"), keys::label("playlist.paste"), false, menus::canPaste(a))) {
      menus::editPaste(a);
      return;
    }
    if (ImGui::MenuItem(i18n::tr("Duplicate"), keys::label("playlist.duplicate"))) {
      auto ids = ops::duplicateClips(a.doc, sel);
      a.playlist.selected = std::set<ObjectId>(ids.begin(), ids.end());
      return;
    }
    if (ImGui::MenuItem(i18n::tr("Delete"), keys::label("playlist.delete"))) {
      ops::removeClips(a.doc, sel);
      a.playlist.selected.clear();
      return;
    }
    ImGui::Separator();
  }
  // Rename is a command: the clip band shows the clip and its Name field takes the typing.
  if (ImGui::MenuItem(i18n::tr("Rename"), keys::label("edit.rename"))) {
    livelook::renameClip(a, id);
    return;
  }
  if (ImGui::BeginMenu(i18n::tr("Colour"))) {  // an audio clip has its source's colour
    uint32_t col;
    const ObjectId src = c->source;
    const bool picked = menus::colorGrid(&col);
    if (picked) ops::setChannelsColor(a.doc, {src}, col);
    ImGui::EndMenu();
    if (picked) return;
  }
  if (ImGui::MenuItem(i18n::tr("Deactivate"), keys::label("playlist.mute"), c->muted)) {
    a.doc.editClip(id, c->muted ? "activate clip" : "deactivate clip", [](Clip& x) { x.muted = !x.muted; });
    return;
  }
  if (cliprenderui::sendToLoupeItem(a, id)) return;  // K-252
  ImGui::Separator();  // N49-13: no freeze or hibernate rows: those are the track's (its menu has Freeze track)
  if (ImGui::MenuItem(i18n::tr("Consolidate"), keys::label("playlist.merge"))) {  // the clips rendered to one new audio clip
    const std::string dir = renderedDir(a);
    const double sr = a.engine.sampleRate();
    ObjectId nid = kNoId;
    a.doc.editSong("consolidate clips", [&](Song& x) { nid = consolidateClips(x, sel, dir, sr, true); });
    if (nid != kNoId) a.playlist.selected = {nid};
    else a.notify("Nothing to render", true);
    return;
  }
  {
    const Tick pos = static_cast<Tick>(a.engine.position());
    if (ImGui::MenuItem(i18n::tr("Split"), keys::label("playlist.splitAtSongPos"), false, pos > c->start && pos < c->start + s.clipLength(*c))) {
      a.playlist.selected = sel;
      splitClipsAtPlayhead(a);
      return;
    }
  }
  if (ImGui::MenuItem(i18n::tr("Reverse"), nullptr, c->reverse)) {
    ops::reverseAudioClips(a.doc, sel);
    return;
  }
  ImGui::Separator();
  if (silenceui::clipItems(a, id)) return;  // K-253
  cliptoolsui::clipMenu(a, id);  // Loupe's tools on the clip (each clip a new file, one undo step)
  ImGui::Separator();
  if (cliprenderui::menuItems(a, id)) return;  // K-250 / K-251
  stemsui::clipMenuItem(a, id);  // J-191: vocals, drums, bass and the rest onto new audio tracks
}

// ------------------------------------------------------------------ settings

void audioInputSettings(App& a, float fieldW) {
  const auto& P = pal();
  auto label = [](const char* t) { ui::FormLabelFit(i18n::tr(t), 168.0f, pal().textDim); };
  Settings& st = a.settings;
  // J-54: drawn inside the settings section "INPUT & RECORDING" (no second caption).
  if (asioui::inputsFromDriver(a)) {  // K-6: the ASIO driver's inputs feed the mixer
    label("Input device");
    ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%s, %d inputs (ASIO CHANNELS above)"), a.asio.driver.c_str(), a.asio.inputs);
  } else {
    label("Input device");
    const char* cur = a.audioInputDevice.empty() ? "None" : a.audioInputDevice == "*" ? "System default" : a.audioInputDevice.c_str();
    if (ui::BeginCombo("##indev", cur, fieldW)) {
      auto pick = [&](const std::string& d) {
        a.audioInputDevice = d;
        st.set("audio.inputDevice", d);
        a.audioReopen = true;
      };
      if (ImGui::Selectable(i18n::tr("None"), a.audioInputDevice.empty())) pick("");
      if (ImGui::Selectable(i18n::tr("System default"), a.audioInputDevice == "*")) pick("*");
      for (const auto& d : a.audioInputDevices)
        if (ImGui::Selectable(d.c_str(), d == a.audioInputDevice)) pick(d);
      ImGui::EndCombo();
    }
    ui::Hint("Device that mixer track inputs record from (the mixer's INPUT selector picks the channels)");
    label("Input channels");
    const int chans[] = {1, 2, 4, 8};
    const int curCh = static_cast<int>(st.getNumber("audio.inputChannels"));
    {  // K-112b checkpoint 35: an option row (a pop-up menu in the Smooth layout)
      const char* labels[] = {"1", "2", "4", "8"};
      int idx = -1;
      for (int i = 0; i < 4; ++i)
        if (curCh == chans[i]) idx = i;
      if (ui::OptionRow("inch", labels, 4, &idx, 160.0f)) {
        st.set("audio.inputChannels", chans[idx]);
        a.audioReopen = true;
      }
    }
  }
  const double sr = std::max(1.0, a.engine.sampleRate());
  label("Recording offset");
  {
    // K-123 (0.7.7): a measured round trip (the latency check) replaces the estimate from the buffers.
    const int measured = recmore::measuredRoundTrip(a);
    const int autoLat = (measured > 0 ? measured : a.audioInputLatency) + a.engine.outputLatency();
    ImGui::PushFont(livelook::numFont(a), theme::kFontNumber + 1);
    ImGui::Text(measured > 0 ? i18n::tr("%d smp (%.1f ms) measured") : i18n::tr("%d smp (%.1f ms) auto"), autoLat, 1000.0 * autoLat / sr);
    ImGui::PopFont();
    ui::Hint(measured > 0 ? "The round trip measured with a loopback cable + plugin delay compensation: recorded audio is moved this much earlier"
                          : "Input + output buffers and plugin delay compensation: recorded audio is moved this much earlier");
    recmore::latencySettingsRow(a);
    ImGui::SameLine(0, theme::kS2);
    float ms = static_cast<float>(st.getNumber("audio.inputOffsetMs"));
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragFloat("##offms", &ms, 0.1f, -200.0f, 200.0f, "%+.1f ms")) st.set("audio.inputOffsetMs", static_cast<double>(ms));
    ui::Hint("Manual offset on top of the automatic one (positive = recordings land later)");
  }
  // K-123: monitoring of the armed tracks.
  label("Monitoring");
  {
    bool b = st.getBool("record.monitor.inputOnly");
    if (ImGui::Checkbox(i18n::tr("Input only"), &b)) st.set("record.monitor.inputOnly", b);
    ui::Hint("On: armed tracks in Auto always hear their input. Off: they hear it inside the punch range only, and the playback around it");
    ImGui::SameLine(0, theme::kS3);
    b = st.getBool("record.monitor.lowLatency");
    if (ImGui::Checkbox(i18n::tr("Low latency"), &b)) st.set("record.monitor.lowLatency", b);
    ui::Hint("While recording, armed tracks skip their effects that add latency, so the performer hears the input sooner");
  }
  label("Record format");
  const int bits = static_cast<int>(st.getNumber("audio.recordBits"));
  {
    const char* labels[] = {i18n::tr("32-bit float"), i18n::tr("24-bit"), i18n::tr("16-bit")};
    const int values[] = {32, 24, 16};
    int idx = bits == 24 ? 1 : bits == 16 ? 2 : 0;
    if (ui::OptionRow("bits", labels, 3, &idx, 160.0f)) st.set("audio.recordBits", values[idx]);
  }
  label("Recorded files");
  ImGui::PushFont(livelook::numFont(a), theme::kFontLabel + 1);
  // K-112c (round 32): in the Clips layout a long path is cut in the middle to fit the page (it ran
  // past the window's edge); hovering shows it whole.
  if (livelook::active(a)) ui::PathFit(recordedDir(a).c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x - 8), P.textDim);
  else ImGui::TextColored(theme::vec(P.textDim), "%s", recordedDir(a).c_str());
  ImGui::PopFont();
  if (!a.audioInputStatus.empty()) {
    label("Input status");
    ImGui::TextColored(theme::vec(P.textDim), "%s", a.audioInputStatus.c_str());
  }
}

}  // namespace gl
