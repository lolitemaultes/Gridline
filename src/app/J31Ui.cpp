// Gridline — J-31 menu entries and per-frame hooks (see J31Ui.h). The chord
// progression editor window is in ChordEditorUi.cpp. Captions are Gridline's
// own wording; the operations are in core/Grouping, core/SongTools,
// engine/AudioLogger and engine/AudioRecord.
#include "../core/I18n.h"
#include "J31Ui.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <memory>
#include "../../third_party/imgui/imgui.h"
#include "../core/ChordTools.h"
#include "../core/Grouping.h"
#include "../core/NoteTools.h"
#include "../core/SongTools.h"
#include "../engine/AudioLogger.h"
#include "../engine/AudioRecord.h"
#include "../engine/Sample.h"
#include "../packs/PacksUi.h"
#include "../packs/edison/edison.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "ChordEditorUi.h"
#include "ContextMenus.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::j31 {

namespace fs = std::filesystem;
using namespace menus;

namespace {

ObjectId bgChannel = kNoId;  // background waveform source (a channel with a sample)
struct KeyFollow {
  ObjectId pattern = kNoId;
  int marker = -2, root = -1, scale = -1;
} keyFollow;

void report(App& a, int n, const std::string& done, const std::string& none) {
  a.notify(n > 0 ? done + " (" + std::to_string(n) + ")" : none);
}

// The pattern's ticks per step / beat / bar in steps.
int stepsPerBeat(const Song& s, const Pattern& p) {
  const Tick st = s.ticksPerStep(p);
  return st > 0 ? static_cast<int>(std::max<Tick>(1, s.beatTicks() / st)) : 4;
}

// ------------------------------------------------------------------ audio logger

std::string loggerSourceName(int k) {
  return k == 1 ? "The master output (after the effects)" : "Armed inputs (before the effects)";
}

void syncLogger(App& a) {
  const bool on = a.settings.getBool("logger.enabled");
  const double minutes = std::clamp(a.settings.getNumber("logger.minutes"), 1.0, 10.0);
  const auto source = a.settings.getNumber("logger.source") > 0.5 ? AudioLogger::Source::Master : AudioLogger::Source::ArmedInputs;
  const auto& cur = a.engine.logger();
  if (!on) {
    if (cur) a.engine.setLogger(nullptr);
    return;
  }
  if (cur && cur->source() == source && std::fabs(cur->seconds() - minutes * 60.0) < 0.5 &&
      std::fabs(cur->sampleRate() - a.engine.sampleRate()) < 0.5)
    return;
  a.engine.setLogger(std::make_shared<AudioLogger>(a.engine.sampleRate(), minutes * 60.0, source));
}

// Writes the last `minutes` of the logger into the recordings folder; "" on failure.
std::string saveLogger(App& a, double minutes) {
  const auto& lg = a.engine.logger();
  if (!lg || lg->available() <= 0) {
    a.notify("The audio logger has nothing yet", true);
    return "";
  }
  std::vector<float> L, R;
  const int64_t n = lg->copyLast(static_cast<int64_t>(minutes * 60.0 * lg->sampleRate()), L, R);
  packs::edison::Audio au = packs::edison::makeAudio(lg->sampleRate(), 2, n);
  au.L = std::move(L);
  au.R = std::move(R);
  const std::string dir = recordedDir(a);
  std::error_code ec;
  fs::create_directories(dir, ec);
  char stamp[32];
  const std::time_t t = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H%M%S", std::localtime(&t));
  const std::string path = packs::edison::renderPath(dir, std::string("Audio log ") + stamp, packs::edison::audioHash(au));
  std::string err;
  if (!packs::edison::writeWavFile(path, packs::edison::makeContent(std::move(au)), 32, &err)) {
    a.notify("Could not save the audio log: " + err, true);
    return "";
  }
  return path;
}

// ------------------------------------------------------------------ recording extras
bool cancelPending = false;

}  // namespace

// ================================================================ per frame

void drawWindows(App& a) {
  syncLogger(a);
  // Snap incoming MIDI: the engine snaps live notes to the highlighted scale.
  uint16_t mask = 0;
  if (a.settings.getBool("piano.snapIncomingMidi") && a.piano.scale > 0) {
    const auto& sc = tools::scales();
    if (a.piano.scale < static_cast<int>(sc.size()))
      for (int st : sc[static_cast<size_t>(a.piano.scale)].steps) mask = static_cast<uint16_t>(mask | (1u << (st % 12)));
  }
  a.engine.setMidiScaleSnap(a.piano.scaleRoot, mask);
  if (cancelPending && !a.audioRecSession) cancelPending = false;
  drawChordEditor(a);
}

// ================================================================ main menu

void patternsItems(App& a) {
  const Song& s = a.doc.song();
  const ObjectId cur = a.curPattern;
  const int g = ops::patternGroupOf(s, cur);
  if (!ImGui::BeginMenu(i18n::tr("Pattern groups"))) return;
  if (ImGui::MenuItem(i18n::tr("New group with the current pattern..."), nullptr, false, s.pattern(cur) != nullptr))
    openPrompt("Pattern group name", "Group " + std::to_string(s.patternGroups.size() + 1),
               [&a, cur](const std::string& n) { ops::groupPatterns(a.doc, {cur}, n); });
  if (ImGui::BeginMenu(i18n::tr("Add the current pattern to"), !s.patternGroups.empty())) {
    for (size_t k = 0; k < s.patternGroups.size(); ++k)
      if (ImGui::MenuItem((s.patternGroups[k].name + "##pg" + std::to_string(k)).c_str(), nullptr, g == static_cast<int>(k))) {
        auto ids = ops::patternsInGroup(s, static_cast<int>(k));
        if (std::find(ids.begin(), ids.end(), cur) == ids.end()) ids.push_back(cur);
        const std::string name = s.patternGroups[k].name;
        a.doc.beginGroup("add pattern to group");
        ops::deletePatternGroup(a.doc, static_cast<int>(k));
        ops::groupPatterns(a.doc, ids, name);
        a.doc.endGroup();
      }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Take the current pattern out of its group"), nullptr, false, g >= 0)) ops::ungroupPatterns(a.doc, {cur});
  if (!s.patternGroups.empty()) ImGui::Separator();
  for (size_t k = 0; k < s.patternGroups.size(); ++k) {
    const int gi = static_cast<int>(k);
    if (!ImGui::BeginMenu((s.patternGroups[k].name + "##grp" + std::to_string(k)).c_str())) continue;
    const auto ids = ops::patternsInGroup(s, gi);
    for (ObjectId id : ids)
      if (const Pattern* p = s.pattern(id); p && ImGui::MenuItem((p->name + "##p" + std::to_string(id)).c_str(), nullptr, id == cur))
        a.selectPattern(id);
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Play the group"), nullptr, false, !ids.empty())) {
      // Plays the group's first pattern; the next ones follow with Num + / -.
      a.selectPattern(ids.front());
      a.engine.setMode(PlayMode::Pattern);
      if (!a.engine.playing()) a.startPlayback();
    }
    ui::Hint("Selects the group's first pattern and plays it in pattern mode");
    if (ImGui::MenuItem(i18n::tr("Select the next pattern of the group"), nullptr, false, ids.size() > 1)) {
      auto it = std::find(ids.begin(), ids.end(), cur);
      a.selectPattern(it == ids.end() || it + 1 == ids.end() ? ids.front() : *(it + 1));
    }
    if (ImGui::MenuItem(i18n::tr("Rename group...")))
      openPrompt("Rename pattern group", s.patternGroups[k].name, [&a, gi](const std::string& n) { ops::renamePatternGroup(a.doc, gi, n); });
    if (ImGui::MenuItem(i18n::tr("Ungroup"))) ops::deletePatternGroup(a.doc, gi);
    ImGui::EndMenu();
  }
  ImGui::EndMenu();
}

void toolsItems(App& a) {
  if (ImGui::BeginMenu(i18n::tr("Audio logger"))) {
    bool on = a.settings.getBool("logger.enabled");
    if (ImGui::MenuItem(i18n::tr("Keep the last minutes of audio"), nullptr, &on)) a.settings.set("logger.enabled", on);
    ui::Hint("Records continuously into a ring in memory, so a moment can be saved after it happened");
    const int src = a.settings.getNumber("logger.source") > 0.5 ? 1 : 0;
    ImGui::Separator();
    for (int k = 0; k < 2; ++k)
      if (ImGui::MenuItem(loggerSourceName(k).c_str(), nullptr, src == k)) a.settings.set("logger.source", k);
    ImGui::Separator();
    const int mins = static_cast<int>(a.settings.getNumber("logger.minutes"));
    for (int m : {1, 2, 5, 10})
      if (ImGui::MenuItem((std::to_string(m) + " " + (m == 1 ? i18n::tr("minute") : i18n::tr("minutes"))).c_str(), nullptr, mins == m))
        a.settings.set("logger.minutes", m);
    ImGui::Separator();
    const auto& lg = a.engine.logger();
    const double have = lg ? static_cast<double>(lg->available()) / lg->sampleRate() : 0.0;
    char label[96];
    std::snprintf(label, sizeof label, "Save the last %d minute%s (%.0f s kept)", mins, mins == 1 ? "" : "s", have);
    if (ImGui::MenuItem(label, nullptr, false, on && have > 0)) {
      const std::string p = saveLogger(a, mins);
      if (!p.empty()) a.notify("Saved " + fs::path(p).filename().string());
    }
    if (ImGui::MenuItem(i18n::tr("Open the last minutes in Loupe"), nullptr, false, on && have > 0)) {
      const std::string p = saveLogger(a, mins);
      if (!p.empty()) {
        const ObjectId ch = a.addInstrument("gl.sampler", p);
        packs::openAudioEditor(a, p, ch);
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Cancel recording"), nullptr, false, a.audioRecSession)) {
    cancelPending = true;
    a.engine.setRecording(false);
    a.engine.stop();
    a.notify("Recording cancelled: the takes are discarded");
  }
  ui::Hint("Stops the transport and throws away the audio being recorded");
}

void optionsItems(App& a) {
  if (ImGui::BeginMenu(i18n::tr("Song position jumps"))) {
    const int cur = static_cast<int>(a.settings.getNumber("transport.jumpQuantize"));
    for (int k = 0; k < ops::kJumpGridCount; ++k)
      if (ImGui::MenuItem(ops::jumpGridName(k), nullptr, cur == k)) a.settings.set("transport.jumpQuantize", k);
    ImGui::EndMenu();
  }
  ui::Hint("While playing, a click in the ruler or a marker jump waits for the next beat / bar line");
  if (ImGui::BeginMenu(i18n::tr("Recording options"))) {
    bool b = a.settings.getBool("record.autoUnarm");
    if (ImGui::MenuItem(i18n::tr("Disarm tracks after recording"), nullptr, &b)) a.settings.set("record.autoUnarm", b);
    b = a.settings.getBool("record.ignoreSilent");
    if (ImGui::MenuItem(i18n::tr("Drop silent takes"), nullptr, &b)) a.settings.set("record.ignoreSilent", b);
    ui::Hint("Takes that never rise above -60 dBFS are not placed in the playlist");
    b = a.settings.getBool("record.nameTracks");
    if (ImGui::MenuItem(i18n::tr("Name empty playlist tracks after the take"), nullptr, &b)) a.settings.set("record.nameTracks", b);
    const int naming = static_cast<int>(a.settings.getNumber("record.naming"));
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Name takes after the mixer track"), nullptr, naming == 0)) a.settings.set("record.naming", 0);
    if (ImGui::MenuItem(i18n::tr("Name takes after the audio input"), nullptr, naming == 1)) a.settings.set("record.naming", 1);
    ImGui::EndMenu();
  }
}

// ================================================================ context menus

void channelItems(App& a, ObjectId channel) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(channel);
  const Pattern* p = s.pattern(a.curPattern);
  if (!c) return;
  if (p && ImGui::BeginMenu(i18n::tr("Step loop"))) {
    const int total = p->bars * p->stepsPerBar, loop = ops::stepLoop(*p, channel), beat = stepsPerBeat(s, *p);
    const std::vector<ObjectId> ids = {channel};
    if (ImGui::MenuItem(i18n::tr("Don't loop"), nullptr, loop == 0)) ops::setStepLoop(a.doc, a.curPattern, ids, 0);
    for (int n : {1, 2, 3, 4, 6, 8, 12})
      if (n * beat < total) {
        const std::string l = std::to_string(n) + (n == 1 ? " beat" : " beats") + " (" + std::to_string(n * beat) + " steps)";
        if (ImGui::MenuItem(l.c_str(), nullptr, loop == n * beat)) ops::setStepLoop(a.doc, a.curPattern, ids, n * beat);
      }
    if (ImGui::BeginMenu(i18n::tr("Steps"))) {
      for (int n = 1; n < std::min(total, 33); ++n)
        if (ImGui::MenuItem(std::to_string(n).c_str(), nullptr, loop == n)) ops::setStepLoop(a.doc, a.curPattern, ids, n);
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Set to the next beat"))) report(a, ops::loopToContent(a.doc, a.curPattern, ids, beat), "Looping", "The content fills the pattern");
    if (ImGui::MenuItem(i18n::tr("Set to the next bar"))) report(a, ops::loopToContent(a.doc, a.curPattern, ids, p->stepsPerBar), "Looping", "The content fills the pattern");
    if (ImGui::MenuItem(i18n::tr("Burn the loop into the pattern"), nullptr, false, loop > 0)) ops::burnStepLoops(a.doc, a.curPattern, ids);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Key range"))) {
    ImGui::TextDisabled("%s .. %s", tools::pitchName(c->keyLo).c_str(), tools::pitchName(c->keyHi).c_str());
    if (ImGui::MenuItem(i18n::tr("Whole keyboard"), nullptr, c->fullKeyRange())) ops::setKeyRange(a.doc, channel, 0, 127);
    if (ImGui::MenuItem(i18n::tr("Notes below C5 only"))) ops::setKeyRange(a.doc, channel, 0, 59);
    if (ImGui::MenuItem(i18n::tr("C5 and above only"))) ops::setKeyRange(a.doc, channel, 60, 127);
    ImGui::EndMenu();
  }
  ui::Hint("Notes outside the range do not play on this channel (layer splits set it)");
  bool trunc = c->swingTruncate;
  if (ImGui::MenuItem(i18n::tr("Truncate swung steps"), nullptr, &trunc)) ops::setSwingTruncate(a.doc, {channel}, trunc ? 1 : 0);
  ui::Hint("Swung steps end on their grid line instead of overlapping the next step");
  if (c->isLayer()) {
    if (ImGui::MenuItem(i18n::tr("Split the layer's children over the keyboard"))) {
      if (ops::splitLayerChildren(a.doc, channel)) a.notify("Each child plays its own key range");
    }
    if (ImGui::MenuItem(i18n::tr("Reset the children's key ranges"))) ops::resetLayerChildren(a.doc, channel);
  }
}

void rackItems(App& a, const std::vector<ObjectId>& selection) {
  const Song& s = a.doc.song();
  const Pattern* p = s.pattern(a.curPattern);
  std::vector<ObjectId> all, stepChans;
  for (const auto& c : s.channels) {
    all.push_back(c.id);
    if (p)
      if (auto it = p->steps.find(c.id); it != p->steps.end() && std::any_of(it->second.begin(), it->second.end(), [](uint8_t v) { return v; }))
        stepChans.push_back(c.id);
  }
  if (p && ImGui::BeginMenu(i18n::tr("Step loops"))) {
    const int beat = stepsPerBeat(s, *p);
    if (ImGui::MenuItem(i18n::tr("Loop step channels at their content"))) report(a, ops::loopToContent(a.doc, a.curPattern, stepChans, beat), "Looping", "Nothing to loop");
    if (ImGui::MenuItem(i18n::tr("Loop all channels at their content"))) report(a, ops::loopToContent(a.doc, a.curPattern, all, beat), "Looping", "Nothing to loop");
    if (ImGui::MenuItem(i18n::tr("Stop looping the selected channels"), nullptr, false, !selection.empty())) ops::setStepLoop(a.doc, a.curPattern, selection, 0);
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Burn the selected channels' loops"), nullptr, false, !selection.empty()))
      report(a, ops::burnStepLoops(a.doc, a.curPattern, selection), "Loops burned", "No loops in the selection");
    if (ImGui::MenuItem(i18n::tr("Burn every loop"))) report(a, ops::burnStepLoops(a.doc, a.curPattern, all), "Loops burned", "No loops");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Truncate swung steps"), !selection.empty())) {
    if (ImGui::MenuItem(i18n::tr("Turn on for the selected"))) ops::setSwingTruncate(a.doc, selection, 1);
    if (ImGui::MenuItem(i18n::tr("Turn off for the selected"))) ops::setSwingTruncate(a.doc, selection, 0);
    if (ImGui::MenuItem(i18n::tr("Toggle for the selected"))) ops::setSwingTruncate(a.doc, selection, 2);
    ImGui::EndMenu();
  }
}

void pianoToolItems(App& a) {
  const bool ok = pianoHasTarget(a);
  if (ImGui::MenuItem(i18n::tr("Chord progression editor..."), nullptr, false, ok)) openChordEditor(a);
  ui::Hint("Build a progression chord by chord (key, voicing, bass, rhythm), hear it, write it into the pattern");
  const size_t nsel = pianoSelectionCount(a);
  if (ImGui::MenuItem(i18n::tr("Group the selected notes"), nullptr, false, ok && nsel >= 2))
    pianoEdit(a, "group notes", [](auto& n, auto& s) { tools::groupNotes(n, s); });
  if (ImGui::MenuItem(i18n::tr("Ungroup the selected notes"), nullptr, false, ok))
    pianoEdit(a, "ungroup notes", [](auto& n, auto& s) { tools::ungroupNotes(n, s); });
}

void pianoViewItems(App& a) {
  bool b = a.settings.getBool("piano.snapIncomingMidi");
  if (ImGui::MenuItem(i18n::tr("Snap played MIDI notes to the scale"), nullptr, &b, a.piano.scale > 0)) a.settings.set("piano.snapIncomingMidi", b);
  ui::Hint("Notes from a MIDI keyboard sound and record on the highlighted scale");
  b = a.settings.getBool("piano.editableGhosts");
  if (ImGui::MenuItem(i18n::tr("Editable ghost notes"), nullptr, &b)) a.settings.set("piano.editableGhosts", b);
  ui::Hint("A click on another channel's ghost note switches to that channel with the note selected");
  b = a.settings.getBool("piano.followKeyMarkers");
  if (ImGui::MenuItem(i18n::tr("Scale follows key markers"), nullptr, &b)) a.settings.set("piano.followKeyMarkers", b);
  if (ImGui::BeginMenu(i18n::tr("Background waveform"))) {
    if (ImGui::MenuItem(i18n::tr("None"), nullptr, bgChannel == kNoId)) bgChannel = kNoId;
    ImGui::Separator();
    for (const auto& c : a.doc.song().channels)
      if (!c.plugin.data.empty() && isAudioFile(c.plugin.data))
        if (ImGui::MenuItem((c.name + "##bg" + std::to_string(c.id)).c_str(), nullptr, bgChannel == c.id)) bgChannel = c.id;
    ImGui::EndMenu();
  }
}

void playlistItems(App& a) {
  const auto& sel = a.playlist.selected;
  if (ImGui::BeginMenu(i18n::tr("Clip groups"))) {
    if (ImGui::MenuItem(i18n::tr("Group the selected clips"), nullptr, false, sel.size() >= 2))
      if (ops::groupClips(a.doc, sel) != kNoId) a.notify("Grouped clips move and select together");
    if (ImGui::MenuItem(i18n::tr("Ungroup the selected clips"), nullptr, false, !sel.empty())) ops::ungroupClips(a.doc, sel);
    if (ImGui::MenuItem(i18n::tr("Select whole groups"), nullptr, false, !sel.empty())) a.playlist.selected = ops::expandClipGroups(a.doc.song(), sel);
    ImGui::EndMenu();
  }
}

void markerItems(App& a, Tick at) {
  if (!ImGui::BeginMenu(i18n::tr("Add key marker"))) return;
  const auto& sc = tools::scales();
  static int root = 0;
  ImGui::TextDisabled(i18n::tr("Key from %s"), ops::keyName(root, 1).c_str());
  if (ImGui::BeginMenu(i18n::tr("Root note"))) {
    for (int k = 0; k < 12; ++k)
      if (ImGui::MenuItem(chord::noteName(k).c_str(), nullptr, root == k)) root = k;
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(i18n::tr("Piano roll's scale"), nullptr, false, a.piano.scale > 0))
    ops::addKeyMarker(a.doc, at, a.piano.scaleRoot, a.piano.scale);
  for (size_t i = 1; i < sc.size(); ++i)
    if (ImGui::MenuItem(sc[i].name)) ops::addKeyMarker(a.doc, at, root, static_cast<int>(i));
  ImGui::EndMenu();
}

void mixerItems(App& a, ObjectId track) {
  const MixerTrack* m = a.doc.song().mixerTrack(track);
  if (!m) return;
  if (ImGui::BeginMenu(i18n::tr("Record from"))) {
    static const char* const n[] = {"Audio input (before the effects)", "After the effects (before the fader)",
                                    "Track output (after the fader)"};
    for (int k = 0; k < static_cast<int>(RecordPoint::Count); ++k)
      if (ImGui::MenuItem(n[k], nullptr, m->recPoint == k))
        a.doc.editMixerTrack(track, "set recording point", [k](MixerTrack& x) { x.recPoint = static_cast<uint8_t>(k); });
    ImGui::EndMenu();
  }
  ui::Hint("Where an armed track records from; the output taps record without an audio input");
  auto sel = mixerSelection(a);
  if (sel.empty()) sel = {track};
  if (ImGui::MenuItem(i18n::tr("Solo alone (without the tracks feeding it)"), nullptr, m->solo && m->soloAlone))
    ops::altSoloMixerTracks(a.doc, sel);
}

// ================================================================ piano roll hooks

void pianoKey(App& a, int& root, int& scale) {
  if (!a.settings.getBool("piano.followKeyMarkers")) return;
  const auto k = ops::patternKey(a.doc.song(), a.curPattern);
  if (k.marker < 0) {
    keyFollow = {a.curPattern, -1, -1, -1};
    return;
  }
  // Applied when the key in effect changes; a scale picked by hand stays until then.
  if (keyFollow.pattern == a.curPattern && keyFollow.root == k.root && keyFollow.scale == k.scale) return;
  keyFollow = {a.curPattern, k.marker, k.root, k.scale};
  root = k.root;
  scale = k.scale;
}

void drawPianoBackground(App& a, float x0, float y0, float x1, float y1, double tick0, double ticksPerPx) {
  if (bgChannel == kNoId) return;
  const Channel* c = a.doc.song().channel(bgChannel);
  if (!c || c->plugin.data.empty()) {
    bgChannel = kNoId;
    return;
  }
  SamplePtr smp = sampleCache().get(c->plugin.data);
  if (!smp || smp->frames() == 0) return;
  const Song& s = a.doc.song();
  const double secPerTick = 60.0 / (s.tempo * s.ppq);
  const float colW = 2.0f;
  const int cols = static_cast<int>((x1 - x0) / colW) + 1;
  const auto peaks = ops::waveformColumns(smp->left.data(), smp->right.data(), smp->frames(), smp->sampleRate, secPerTick, 0.0,
                                          tick0, ticksPerPx * colW, cols);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float mid = (y0 + y1) * 0.5f, half = (y1 - y0) * 0.45f;
  const ImU32 col = theme::withAlpha(theme::rgb(c->color), 0.16f);
  for (int k = 0; k < cols; ++k) {
    const auto& pk = peaks[static_cast<size_t>(k)];
    if (pk.first == 0.0f && pk.second == 0.0f) continue;
    const float x = x0 + k * colW;
    dl->AddRectFilled(ImVec2(x, mid - pk.second * half), ImVec2(x + colW - 0.5f, mid - pk.first * half + 1.0f), col);
  }
}

bool editGhostAt(App& a, Tick t, int pitch) {
  if (!a.settings.getBool("piano.editableGhosts")) return false;
  const Pattern* p = a.doc.song().pattern(a.curPattern);
  if (!p) return false;
  const auto h = ops::ghostNoteAt(*p, a.selChannel, a.piano.ghostChannels, t, pitch);
  if (h.channel == kNoId) return false;
  a.selectChannel(h.channel);
  a.piano.selPattern = a.curPattern;
  a.piano.selChannel = h.channel;
  a.piano.selected = {static_cast<size_t>(h.index)};
  return true;
}

void expandNoteGroups(App& a) {
  const Pattern* p = a.doc.song().pattern(a.curPattern);
  if (!p) return;
  auto it = p->notes.find(a.selChannel);
  if (it == p->notes.end()) return;
  ops::NoteSel sel(it->second.size(), false);
  for (size_t i : a.piano.selected)
    if (i < sel.size()) sel[i] = true;
  if (tools::selectGroups(it->second, sel) == 0) return;
  for (size_t i = 0; i < sel.size(); ++i)
    if (sel[i]) a.piano.selected.insert(i);
}

// ================================================================ transport / MIDI hooks

void songJump(App& a, Tick tick) {
  const int mode = static_cast<int>(a.settings.getNumber("transport.jumpQuantize"));
  a.engine.jumpTo(tick, ops::jumpGrid(a.doc.song(), mode));
}

int incomingPitch(App& a, int pitch) {
  return ops::snapIncoming(pitch, a.piano.scaleRoot, a.piano.scale, a.settings.getBool("piano.snapIncomingMidi"));
}

bool takeCancelRequested() { return cancelPending; }

}  // namespace gl::j31
