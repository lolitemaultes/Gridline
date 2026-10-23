// Gridline — audio input & recording UI (J-8). See AudioRecordUi.h.
#include "../engine/TimeStretch.h"
#include "../core/I18n.h"
#include "J31Ui.h"
#include "AudioRecordUi.h"
#include "ChopUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "../core/Automation.h"
#include "../core/PluginParams.h"
#include "../engine/ClipOps.h"
#include "../packs/PacksUi.h"
#include "Theme.h"
#include "Widgets.h"

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
      g.startTick = static_cast<double>(2 * s.barTicks());
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
      compTakes(s, g, 2 * s.barTicks() + s.beatTicks() * 2, 2 * s.barTicks() + s.beatTicks() * 4, 0);
      if (mode == "props") propsClip = r.clips[0];
    }
  });
  a.clipProps = propsClip;
  const Song& s = a.doc.song();
  if (s.mixer.size() > 2) a.selMixer = s.mixer[2].id;
  a.playlist.selStart = 2 * s.barTicks();
  a.playlist.selEnd = 4 * s.barTicks();
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
  const Song& s = doc.song();
  // Latency compensation: device buffers + delay compensation + manual offset.
  const double offMs = settings.getNumber("audio.inputOffsetMs");
  const int offset = static_cast<int>(std::lround(offMs * engine.sampleRate() / 1000.0));
  engine.setRecordLatency(std::max(0, audioInputLatency + engine.outputLatency() - offset));

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

  const bool rolling = engine.playing() && engine.mode() != PlayMode::Live;
  const bool want = recordArmed && rolling && armedTracks && recAudio;
  if (want && !audioRecSession) {
    audioRec.begin();
    audioRecSession = true;
    audioRecFinishing = false;
    const bool sel = playlist.selEnd > playlist.selStart;
    if (recPunch && sel && engine.mode() == PlayMode::Song)
      engine.setPunch(static_cast<double>(playlist.selStart), static_cast<double>(playlist.selEnd));
    else
      engine.setPunch(0, 0);
    // Song mode without a time selection: keep recording past the song end.
    if (engine.mode() == PlayMode::Song && !sel) {
      engine.setLooping(false);
      audioRecLoopRestore = 1;
    }
  }
  // Hosted plugins see "recording" whenever the record button is armed and the
  // transport rolls; armed tracks capture while it is set.
  engine.setRecording(recordArmed && rolling && !audioRecFinishing);
  if (!audioRecSession) return;
  if (const auto& p = engine.program()) audioRec.pump(engine, ringsOf(*p));
  if (!want && !audioRecFinishing) {
    engine.setRecording(false);
    audioRecFinishing = true;
    audioRecStopTime = now;
  }
  // Commit once the engine has closed every take (the latency tail), or after a second.
  if (audioRecFinishing && (!audioRec.open() || now - audioRecStopTime > 1.0)) {
    if (const auto& p = engine.program()) audioRec.pump(engine, ringsOf(*p));
    std::vector<RecordedTake> takes = audioRec.release();
    takes.erase(std::remove_if(takes.begin(), takes.end(), [](const RecordedTake& t) { return t.frames() < 64; }), takes.end());
    // J-31: cancelled sessions keep nothing; silent takes can be dropped.
    if (j31::takeCancelRequested()) takes.clear();
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
      TakeCommitResult r;
      doc.editSong("Record audio", [&](Song& x) { r = commitTakes(x, takes, o); });
      if (!r.error.empty()) notify("Recording: " + r.error, true);
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
    audioRecSession = audioRecFinishing = false;
    if (audioRecLoopRestore >= 0) engine.setLooping(audioRecLoopRestore > 0);
    audioRecLoopRestore = -1;
  }
}

// ------------------------------------------------------------------ record filter menu

void audioRecordFilterMenu(App& a) {
  ImGui::TextDisabled("%s", i18n::tr("What to record"));
  ImGui::MenuItem(i18n::tr("Audio, into the playlist as audio clips"), nullptr, &a.recAudio);
  ImGui::MenuItem(i18n::tr("Audio, into Loupe (armed tracks)"), nullptr, &a.recAudioEditor);
  ImGui::MenuItem(i18n::tr("Notes"), nullptr, &a.recOptions.notes);
  ImGui::MenuItem(i18n::tr("Automation (controller moves)"), nullptr, &a.recOptions.automation);
  if (ImGui::MenuItem(i18n::tr("Everything"))) a.recAudio = a.recOptions.notes = a.recOptions.automation = true;
  ImGui::Separator();
  ImGui::MenuItem(i18n::tr("Punch in/out at the time selection"), nullptr, &a.recPunch);
  if (ImGui::BeginMenu(i18n::tr("Audio file format"))) {
    const int bits = static_cast<int>(a.settings.getNumber("audio.recordBits"));
    if (ImGui::MenuItem(i18n::tr("32-bit float WAV"), nullptr, bits != 24 && bits != 16)) a.settings.set("audio.recordBits", 32);
    if (ImGui::MenuItem(i18n::tr("24-bit WAV"), nullptr, bits == 24)) a.settings.set("audio.recordBits", 24);
    if (ImGui::MenuItem(i18n::tr("16-bit WAV"), nullptr, bits == 16)) a.settings.set("audio.recordBits", 16);
    ImGui::EndMenu();
  }
  const Song& s = a.doc.song();
  int armed = 0;
  for (const auto& m : s.mixer) armed += m.armed && m.input >= 0;
  if (armed == 0)
    ImGui::TextDisabled("%s", i18n::tr("No mixer track is armed for audio"));
  else
    ImGui::TextDisabled(i18n::tr("%d mixer track%s armed for audio"), armed, armed == 1 ? "" : "s");
}

// ------------------------------------------------------------------ mixer

void toggleArm(App& a, ObjectId track) {
  const MixerTrack* t = a.doc.song().mixerTrack(track);
  if (!t || track == a.doc.song().masterId()) return;
  const bool on = !t->armed;
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
  ui::Hint(t.armed ? "Disk record: armed (click to disarm)" : "Disk record: click to arm this track for audio recording");
  ui::Tooltip(t.armed ? "Disarm" : "Arm for recording");
  const ImVec2 c(pos.x + 8, pos.y + 7);
  const bool rolling = a.engine.recordingOn() && t.armed;
  const ImU32 ring = t.armed ? P.danger : hov ? P.text : P.textFaint;
  dl->AddCircleFilled(c, 5.0f, t.armed ? theme::withAlpha(P.danger, rolling ? 0.55f + 0.45f * static_cast<float>(std::fabs(std::sin(a.now * 4))) : 0.85f)
                                       : P.field);
  dl->AddCircle(c, 5.0f, ring, 0, 1.2f);
  if (clicked) toggleArm(a, t.id);
  return clicked;
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
  if (ui::TextButton(t.armed ? "Armed" : "Arm", t.armed, 56, "Disk record: arm this track (the record button then records its input)"))
    toggleArm(a, t.id);
  // Monitoring + input meter.
  const char* mon[] = {"Auto", "On", "Off"};
  const char* monHints[] = {"Hear the input while the track is armed, and whenever the transport is stopped",
                            "Always hear the input through the track", "Never hear the input (it is still recorded)"};
  int m = std::clamp(static_cast<int>(t.monitor), 0, 2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Monitor"));
  ImGui::SameLine(0, theme::kS1);
  if (ui::Segmented("##mon", mon, 3, &m, 40, 0, monHints))
    a.doc.editMixerTrack(t.id, "input monitoring", [m](MixerTrack& x) { x.monitor = static_cast<uint8_t>(m); });
  float pl = 0, pr = 0;
  if (auto rt = a.builder.mixerRuntime(t.id)) {
    pl = rt->inPeakL.exchange(0);
    pr = rt->inPeakR.exchange(0);
  }
  // Input level (horizontal meter on its own row).
  ui::Meter(ImGui::GetID("inmeter"), t.input >= 0 ? pl : 0.f, t.input >= 0 ? pr : 0.f, ImVec2(width, 8), false);
  ui::Hint("Input level (before the track's effects)");
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
  ImGui::EndMenu();
}

}  // namespace

void audioClipMenu(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const Clip* c = s.clip(id);
  if (!c || c->kind != ClipKind::Audio) return;
  const Clip clip = *c;
  if (ImGui::MenuItem(i18n::tr("Clip properties..."))) a.clipProps = id;
  compMenu(a, clip);
  if (ImGui::BeginMenu(i18n::tr("Time stretch"))) {
    const Channel* ch = s.channel(clip.source);
    const bool keep = ch && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode) && ch->plugin.params[pp::SmpStretchMode] >= 0.5f;
    // J-87: every time mode (engine/TimeStretch), the current one checked.
    const int cur = ch && ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode)
                        ? static_cast<int>(stretch::modeOf(ch->plugin.params[pp::SmpStretchMode]))
                        : 0;
    for (int m = 0; m < static_cast<int>(stretch::Mode::Count); ++m) {
      if (ImGui::MenuItem(i18n::tr(stretch::modeName(static_cast<stretch::Mode>(m))), nullptr, m == cur))
        editSongClip(a, "stretch mode", [&](Song& x) { setClipStretchMode(x, id, clip.stretch, m); });
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr(stretch::modeHint(static_cast<stretch::Mode>(m))));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Half as long"))) editSongClip(a, "stretch clip", [&](Song& x) { setClipStretch(x, id, clip.stretch * 0.5, keep); });
    if (ImGui::MenuItem(i18n::tr("Twice as long"))) editSongClip(a, "stretch clip", [&](Song& x) { setClipStretch(x, id, clip.stretch * 2.0, keep); });
    if (ImGui::MenuItem(i18n::tr("Original length"), nullptr, false, clip.stretch != 1.0))
      editSongClip(a, "stretch clip", [&](Song& x) { setClipStretch(x, id, 1.0, keep); });
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Fit to tempo"))) {
    int beats = 0;
    editSongClip(a, "fit clip to tempo", [&](Song& x) { beats = fitClipToTempo(x, id); });
    if (beats > 0) a.notify("Fitted to " + std::to_string(beats) + (beats == 1 ? " beat" : " beats") + "; the clip now follows the tempo");
  }
  if (ImGui::MenuItem(i18n::tr("Normalize"))) editSongClip(a, "normalize clip", [&](Song& x) { normalizeClip(x, id, 0.0); });
  chopui::clipMenu(a, id);  // PL-13: slice at the grid / into parts / at transients, stutter presets
  ImGui::Separator();
  std::set<ObjectId> sel = a.playlist.selected;
  if (!sel.count(id)) sel = {id};
  if (ImGui::MenuItem(sel.size() > 1 ? i18n::tr("Consolidate selected clips to audio") : i18n::tr("Render clip to audio (consolidate)"))) {
    const std::string dir = renderedDir(a);
    const double sr = a.engine.sampleRate();
    ObjectId nid = kNoId;
    editSongClip(a, "consolidate clips", [&](Song& x) { nid = consolidateClips(x, sel, dir, sr, true); });
    if (nid != kNoId) a.playlist.selected = {nid};
    else a.notify("Nothing to render", true);
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

void drawClipProperties(App& a) {
  if (a.clipProps == kNoId) return;
  const Song& s = a.doc.song();
  const Clip* cp = s.clip(a.clipProps);
  const Channel* ch = cp ? s.channel(cp->source) : nullptr;
  if (!cp || cp->kind != ClipKind::Audio || !ch) {
    a.clipProps = kNoId;
    return;
  }
  const Clip c = *cp;
  const ObjectId id = c.id;
  const auto& P = pal();
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
  std::string title = "Audio clip \xC2\xB7 " + ch->name + "###clipprops";
  if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    if (!open) a.clipProps = kNoId;
    return;
  }
  const uint64_t key = 0x434C500000000000ull ^ (static_cast<uint64_t>(id) << 4);
  const float fw = 200;
  auto row = [&](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", label);
    ImGui::SameLine(110);
    ImGui::SetNextItemWidth(fw);
  };
  SamplePtr smp = sampleCache().get(ch->plugin.data);
  ImGui::PushFont(a.fontMono, theme::kFontLabel);
  if (smp)
    ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("%s \xC2\xB7 %.0f Hz \xC2\xB7 %.2f s"), fs::path(ch->plugin.data).filename().string().c_str(),
                       smp->sampleRate, smp->seconds());
  ImGui::PopFont();
  ui::Label("LEVEL");
  float gainDb = c.gain > 0 ? 20.0f * std::log10(c.gain) : -60.0f;
  row("Gain");
  if (ImGui::DragFloat("##gain", &gainDb, 0.1f, -60.0f, 24.0f, "%.1f dB"))
    a.doc.editClip(id, "clip gain", [&](Clip& x) { x.gain = gainDb <= -60.0f ? 0.0f : std::pow(10.0f, gainDb / 20.0f); }, key);
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("Normalize")) a.doc.editSong("normalize clip", [&](Song& x) { normalizeClip(x, id, 0.0); });
  float fi = static_cast<float>(c.fadeIn * 1000.0), fo = static_cast<float>(c.fadeOut * 1000.0);
  row("Fade in");
  if (ImGui::DragFloat("##fi", &fi, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
    a.doc.editClip(id, "clip fade in", [&](Clip& x) { x.fadeIn = std::max(0.0f, fi) / 1000.0, x.fadeInManual = true; }, key + 1);
  row("Fade out");
  if (ImGui::DragFloat("##fo", &fo, 1.0f, 0.0f, 60000.0f, "%.0f ms"))
    a.doc.editClip(id, "clip fade out", [&](Clip& x) { x.fadeOut = std::max(0.0f, fo) / 1000.0, x.fadeOutManual = true; }, key + 2);
  bool rev = c.reverse;
  row("Reverse");
  if (ImGui::Checkbox("##rev", &rev)) a.doc.editClip(id, "reverse clip", [rev](Clip& x) { x.reverse = rev; });

  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::Label("PITCH & TIME");
  float cents = c.pitch;
  row("Pitch");
  if (ImGui::DragFloat("##pitch", &cents, 1.0f, -2400.0f, 2400.0f, "%.0f ct"))
    a.doc.editClip(id, "clip pitch", [&](Clip& x) { x.pitch = cents; }, key + 3);
  const bool keep = ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode) && ch->plugin.params[pp::SmpStretchMode] >= 0.5f;
  // J-87: the channel's time mode (engine/TimeStretch).
  int mode = ch->plugin.params.size() > static_cast<size_t>(pp::SmpStretchMode)
                 ? static_cast<int>(stretch::modeOf(ch->plugin.params[pp::SmpStretchMode]))
                 : 0;
  row("Stretch mode");
  ImGui::SetNextItemWidth(200);
  if (ImGui::BeginCombo("##smode", i18n::tr(stretch::modeName(static_cast<stretch::Mode>(mode))))) {
    for (int m = 0; m < static_cast<int>(stretch::Mode::Count); ++m) {
      if (ImGui::Selectable(i18n::tr(stretch::modeName(static_cast<stretch::Mode>(m))), m == mode)) {
        const int pick = m;
        a.doc.editSong("stretch mode", [&](Song& x) { setClipStretchMode(x, id, c.stretch, pick); });
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr(stretch::modeHint(static_cast<stretch::Mode>(m))));
    }
    ImGui::EndCombo();
  }
  float tf = static_cast<float>(c.stretch);
  row("Time");
  if (ImGui::DragFloat("##tf", &tf, 0.005f, 0.0625f, 16.0f, "x %.3f")) {
    float v = tf;
    a.doc.editSong("stretch clip", [&](Song& x) { setClipStretch(x, id, v, keep); });
  }
  bool follow = c.syncBpm > 0;
  row("Follow tempo");
  if (ImGui::Checkbox("##follow", &follow)) {
    TempoMap tm;
    tm.build(s);
    const double bpm = tm.bpmAt(static_cast<double>(c.start));
    a.doc.editClip(id, "follow tempo", [&](Clip& x) { x.syncBpm = follow ? bpm : 0.0; });
  }
  if (c.syncBpm > 0) {
    ImGui::SameLine(0, theme::kS2);
    ImGui::TextColored(theme::vec(P.textFaint), i18n::tr("at %.2f BPM"), c.syncBpm);
  }
  ImGui::Dummy(ImVec2(110 - ImGui::GetStyle().ItemSpacing.x, 0));
  ImGui::SameLine();
  if (ui::TextButton("Fit to tempo", false, 0, "Guess the number of beats and make the clip follow the tempo"))
    a.doc.editSong("fit clip to tempo", [&](Song& x) { fitClipToTempo(x, id); });

  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::Label("RENDER STRETCH (LOUPE)");
  static float rFactor = 1.0f, rSemis = 0.0f;
  static int rAlgo = 0;
  row("Length");
  ImGui::DragFloat("##rf", &rFactor, 0.005f, 0.25f, 4.0f, "x %.3f");
  row("Pitch");
  ImGui::DragFloat("##rs", &rSemis, 0.05f, -24.0f, 24.0f, "%+.2f st");
  const char* algos[] = {"Elastic", "Transient", "Tape"};
  row("Algorithm");
  ui::Segmented("##ralgo", algos, 3, &rAlgo, 64);
  ImGui::Dummy(ImVec2(110 - ImGui::GetStyle().ItemSpacing.x, 0));
  ImGui::SameLine();
  if (ui::TextButton("Render", false, 0, "Writes a stretched copy of the audio and points the clip at it")) {
    std::string out;
    const std::string dir = renderedDir(a);
    a.doc.editSong("render stretch", [&](Song& x) { out = renderClipStretch(x, id, rFactor, rSemis, rAlgo, dir); });
    a.notify(out.empty() ? std::string("Render failed") : "Rendered " + fs::path(out).filename().string(), out.empty());
  }

  ImGui::Dummy(ImVec2(0, theme::kS1));
  ui::Label("SLICE");
  if (ui::TextButton("At beats")) a.doc.editSong("slice clip", [&](Song& x) { sliceClipAtGrid(x, id, s.beatTicks()); });
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("At bars")) a.doc.editSong("slice clip", [&](Song& x) { sliceClipAtGrid(x, id, s.barTicks()); });
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("At transients")) a.doc.editSong("slice clip at transients", [&](Song& x) { sliceClipAtTransients(x, id, 0.5); });
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("Edit audio")) packs::openAudioEditor(a, ch->plugin.data, ch->id);
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemActive()) a.doc.endGesture();
  ImGui::End();
  if (!open) a.clipProps = kNoId;
}

// ------------------------------------------------------------------ settings

void audioInputSettings(App& a, float fieldW) {
  const auto& P = pal();
  auto label = [](const char* t) { ui::FormLabelFit(i18n::tr(t), 168.0f, pal().textDim); };
  Settings& st = a.settings;
  // J-54: drawn inside the settings section "INPUT & RECORDING" (no second caption).
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
  for (int c : chans) {
    if (c != chans[0]) ImGui::SameLine(0, theme::kS1);
    if (ui::TextButton(std::to_string(c).c_str(), curCh == c)) {
      st.set("audio.inputChannels", c);
      a.audioReopen = true;
    }
  }
  const double sr = std::max(1.0, a.engine.sampleRate());
  label("Recording offset");
  {
    const int autoLat = a.audioInputLatency + a.engine.outputLatency();
    ImGui::PushFont(a.fontMono, theme::kFontNumber + 1);
    ImGui::Text(i18n::tr("%d smp (%.1f ms) auto"), autoLat, 1000.0 * autoLat / sr);
    ImGui::PopFont();
    ui::Hint("Input + output buffers and plugin delay compensation: recorded audio is moved this much earlier");
    ImGui::SameLine(0, theme::kS2);
    float ms = static_cast<float>(st.getNumber("audio.inputOffsetMs"));
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragFloat("##offms", &ms, 0.1f, -200.0f, 200.0f, "%+.1f ms")) st.set("audio.inputOffsetMs", static_cast<double>(ms));
    ui::Hint("Manual offset on top of the automatic one (positive = recordings land later)");
  }
  label("Record format");
  const int bits = static_cast<int>(st.getNumber("audio.recordBits"));
  if (ui::TextButton("32-bit float", bits != 24 && bits != 16)) st.set("audio.recordBits", 32);
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("24-bit", bits == 24)) st.set("audio.recordBits", 24);
  ImGui::SameLine(0, theme::kS1);
  if (ui::TextButton("16-bit", bits == 16)) st.set("audio.recordBits", 16);
  label("Recorded files");
  ImGui::PushFont(a.fontMono, theme::kFontLabel + 1);
  ImGui::TextColored(theme::vec(P.textDim), "%s", recordedDir(a).c_str());
  ImGui::PopFont();
  if (!a.audioInputStatus.empty()) {
    label("Input status");
    ImGui::TextColored(theme::vec(P.textDim), "%s", a.audioInputStatus.c_str());
  }
}

}  // namespace gl
