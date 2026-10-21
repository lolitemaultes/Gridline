// Gridline — J-27 menu parity sweep: menu entries (see MenuParityUi.h).
// Captions are Gridline's own wording; the operations live in
// core/MenuParity (undoable, tested) and engine/NoteFxBurn.
#include "../engine/TimeStretch.h"
#include "../core/I18n.h"
#include "MenuParityUi.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../core/MenuOpsExtra.h"
#include "../core/MenuParity.h"
#include "../core/NoteTools.h"
#include "../core/PatternAutomation.h"
#include "../core/PluginParams.h"
#include "../core/ProjectInterop.h"
#include "../engine/NoteFxBurn.h"
#include "App.h"
#include "AutomationEditor.h"
#include "BrowserUi.h"
#include "ContextMenus.h"
#include "ExportUi.h"
#include "Midi.h"
#include "ScriptUi.h"
#include "Widgets.h"
#include "Windows.h"

namespace gl::menuparity {

namespace fs = std::filesystem;
using namespace menus;

namespace {

ops::ScoreLog scoreLog;
std::multiset<int> heldPitches;  // MIDI input notes held now (played chord display)
bool dumpReplaces = false;

std::vector<ObjectId> selectedChannels(const App& a) {
  std::vector<ObjectId> out;
  for (const auto& c : a.doc.song().channels)
    if (a.selChannels.count(c.id) || c.id == a.selChannel) out.push_back(c.id);
  return out;
}

std::set<ObjectId> selectedClipsOfKind(const App& a, ClipKind k) {
  std::set<ObjectId> out;
  for (const auto& c : a.doc.song().playlist)
    if (c.kind == k && a.playlist.selected.count(c.id)) out.insert(c.id);
  return out;
}

void report(App& a, int n, const std::string& done, const std::string& none) {
  a.notify(n > 0 ? done + " (" + std::to_string(n) + ")" : none);
}

const char* const kDeclick[] = {"Off", "Out only", "Transient", "Generic", "Smooth"};

void declickItems(App& a, const std::vector<ObjectId>& chans) {
  for (int k = 0; k < 5; ++k)
    if (ImGui::MenuItem(kDeclick[k], nullptr, false, !chans.empty()))
      report(a, ops::setSamplerDeclick(a.doc, chans, k), "Declicking changed", "No sampler channel to change");
}

// J-87: the sampler's time modes (engine/TimeStretch::modeName / modeHint),
// marked here for the translation catalogue.
[[maybe_unused]] constexpr const char* kTimeModeText[] = {
    i18n::N_("Resample"), i18n::N_("Auto"), i18n::N_("Mono / vocal"), i18n::N_("Transient"), i18n::N_("Formant keep"),
    i18n::N_("Pitch and speed change together, like a turntable (band-limited, no aliasing)"),
    i18n::N_("Pitch and time apart, for full mixes, chords and loops; drum hits stay sharp"),
    i18n::N_("Single voices, vocals, bass and leads: tighter timing"),
    i18n::N_("Drum loops and percussion: every hit re-attacked from the original"),
    i18n::N_("Pitch shift that keeps a voice's character (no chipmunk effect)"),
    i18n::N_("Auto (general)"), i18n::N_("Transient (drums)"), i18n::N_("Tape (varispeed)"),
    i18n::N_("Realtime (band-limited sinc)")};

void timeModeItems(App& a) {
  const auto sel = selectedClipsOfKind(a, ClipKind::Audio);
  for (int group = 0; group < 2; ++group) {
    ImGui::SeparatorText(group == 0 ? i18n::tr("Every audio clip") : i18n::tr("Selected audio clips"));
    ImGui::PushID(group);
    for (int m = 0; m < static_cast<int>(stretch::Mode::Count); ++m) {
      ImGui::PushID(m);
      const auto mode = static_cast<stretch::Mode>(m);
      if (ImGui::MenuItem(i18n::tr(stretch::modeName(mode)), nullptr, false, group == 0 || !sel.empty()))
        report(a, ops::setAudioClipTimeMode(a.doc, group == 0 ? decltype(sel){} : sel, m), "Audio clip channels switched",
               "Nothing to switch");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", i18n::tr(stretch::modeHint(mode)));
      ImGui::PopID();
    }
    ImGui::PopID();
  }
}

void scoreLogItems(App& a) {
  const Song& s = a.doc.song();
  const bool target = s.pattern(a.curPattern) && s.channel(a.selChannel);
  ImGui::TextDisabled(i18n::tr("%d logged note events"), static_cast<int>(scoreLog.events.size()));
  if (ImGui::BeginMenu(i18n::tr("Put the notes played in the last"), target && !scoreLog.events.empty())) {
    for (int m : {1, 2, 5, 10, 20, 30}) {
      std::string l = std::to_string(m) + (m == 1 ? " minute" : " minutes");
      if (ImGui::MenuItem(l.c_str())) {
        int n = ops::dumpScoreLog(a.doc, a.curPattern, a.selChannel, scoreLog, a.now, m * 60.0, dumpReplaces);
        a.notify(n ? std::to_string(n) + " notes put into " + s.pattern(a.curPattern)->name : std::string("No notes in that time"));
        if (n) a.openPianoRoll(a.selChannel);
      }
    }
    ImGui::Separator();
    ImGui::MenuItem(i18n::tr("Replace the channel's notes"), nullptr, &dumpReplaces);
    ImGui::EndMenu();
  }
  if (!target) ImGui::TextDisabled("%s", i18n::tr("Select a pattern and a channel first"));
  if (ImGui::MenuItem(i18n::tr("Clear the log"), nullptr, false, !scoreLog.events.empty())) scoreLog.clear();
  ImGui::MenuItem(i18n::tr("Log MIDI input notes"), nullptr, &scoreLog.enabled);
  if (ImGui::BeginMenu(i18n::tr("Keep notes for"))) {
    for (int m : {5, 10, 20, 30, 60}) {
      std::string l = std::to_string(m) + " minutes";
      if (ImGui::MenuItem(l.c_str(), nullptr, std::fabs(scoreLog.keepSeconds - m * 60.0) < 1)) scoreLog.keepSeconds = m * 60.0;
    }
    ImGui::EndMenu();
  }
}

std::string paramLabel(const App& a, ParamId p) {
  auto d = describeParam(a.doc.song(), p);
  return d ? paramDisplayName(a.doc.song(), p) : std::string();
}

}  // namespace

// ================================================================ main menu ====

void fileItems(App& a) {
  if (ImGui::MenuItem(i18n::tr("Save to a new project folder..."))) {
    std::string title = a.doc.song().info.title.empty() ? std::string("Untitled") : a.doc.song().info.title;
    fileDialog(a, "Save into a new project folder (with its samples)", true, {".json"}, title, [&a](const std::string& p) {
      fs::path in(p);
      std::string name = in.filename().string();
      for (const char* ext : {".json", ".gridline"})
        if (name.size() > std::strlen(ext) && name.compare(name.size() - std::strlen(ext), std::strlen(ext), ext) == 0)
          name.resize(name.size() - std::strlen(ext));
      if (name.empty()) name = "Project";
      const fs::path folder = in.parent_path() / name;
      std::error_code ec;
      fs::create_directories(folder, ec);
      if (ec) {
        a.notify("Could not create " + folder.string(), true);
        return;
      }
      Song s = a.doc.song();
      auto rep = collectFiles(s, (folder / "Samples").string());
      if (!rep.error.empty()) {
        a.notify("Could not copy the samples: " + rep.error, true);
        return;
      }
      if (s != a.doc.song()) a.doc.editSong("save to a new project folder", [&s](Song& x) { x = s; });
      if (a.saveProject((folder / (name + ".gridline.json")).string()))
        a.notify("Saved into " + folder.string() + " (" + std::to_string(rep.copied) + " files copied" +
                 (rep.missing.empty() ? std::string(")") : ", " + std::to_string(rep.missing.size()) + " missing)"));
    });
  }
  ui::Hint("Saves the project into a folder of its own, with copies of every sample it uses");
}

void exportItems(App& a) {
  if (ImGui::MenuItem(i18n::tr("Render again with the last settings..."))) exportui::openRenderAgain(a);
}

void addItems(App& a) {
  const ParamId last = a.doc.lastTouchedParam();
  const std::string name = paramLabel(a, last);
  std::string l = "Automation clip for the last moved control" + (name.empty() ? std::string() : " (" + name + ")");
  if (ImGui::MenuItem(l.c_str(), nullptr, false, !name.empty())) {
    ObjectId id = createAutomationClip(a, last);
    if (id) a.notify("Automation clip for " + name);
  }
}

void viewItems(App& a) {
  if (ImGui::MenuItem(i18n::tr("Project picker"))) browserui::showProjectTab(a);
  ui::Hint("The browser's project tab: patterns, channels, effects, automation and samples of this project");
  {
    const bool on = a.settings.getBool("ui.showPlayedChords");
    if (ImGui::MenuItem(i18n::tr("Show played chords in the hint bar"), nullptr, on)) a.settings.set("ui.showPlayedChords", !on);
    ui::Hint("Names the chord held on the MIDI keyboard (e.g. Am7, C/E) in the hint bar");
  }
}

void optionsItems(App& a) {
  if (!a.midi) return;
  const bool clock = a.midi->syncOut != SyncOut::Off;
  if (ImGui::MenuItem(i18n::tr("Send MIDI clock (master sync)"), nullptr, clock)) {
    a.midi->syncOut = clock ? SyncOut::Off : SyncOut::Clock;
    a.publishMidi();
    a.midi->save((fs::path(a.configDir()) / "midi.json").string());
  }
  ui::Hint("Other devices follow Gridline's tempo and transport (MIDI clock on the MIDI output; timecode in MIDI settings)");
}

void toolsItems(App& a) {
  const ParamId last = a.doc.lastTouchedParam(), prev = a.doc.previousTouchedParam();
  const std::string ln = paramLabel(a, last), pn = paramLabel(a, prev);
  if (ImGui::BeginMenu(("Last moved control" + (ln.empty() ? std::string() : ": " + ln)).c_str(), !ln.empty())) {
    knobContextMenu(a, last);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(("Control moved before it" + (pn.empty() ? std::string() : ": " + pn)).c_str(), !pn.empty())) {
    knobContextMenu(a, prev);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Score logger"))) {
    scoreLogItems(a);
    ImGui::EndMenu();
  }
  const std::string& script = lastPianoRollScript();
  if (ImGui::MenuItem(i18n::tr("Open the last piano roll script again"), nullptr, false, !script.empty())) openPianoRollScript(a, script);
  if (ImGui::MenuItem(i18n::tr("Save the note clipboard as a MIDI file"), nullptr, false, !a.piano.clipboard.empty())) {
    const std::string dir = browserui::contentDir(a, "Clipboard files");
    std::string path;
    for (int k = 1; k < 10000; ++k) {
      path = (fs::path(dir) / ("Notes " + std::to_string(k) + ".mid")).string();
      if (!fs::exists(path)) break;
    }
    std::string err;
    if (writeMidiFile(path, ops::notesToMidiFile(a.piano.clipboard, a.doc.song().ppq, a.doc.song().tempo, "Notes"), &err))
      a.notify("Saved " + fs::path(path).filename().string() + " (browser: clipboard files)");
    else
      a.notify("Could not save the MIDI file: " + err, true);
  }
  ImGui::SeparatorText(i18n::tr("More macros"));
  if (ImGui::MenuItem(i18n::tr("Lock every playlist track"))) ops::setAllTracksLocked(a.doc, true);
  if (ImGui::MenuItem(i18n::tr("Unlock every playlist track"))) ops::setAllTracksLocked(a.doc, false);
  if (ImGui::MenuItem(i18n::tr("Prepare the live area"))) {
    Tick t = ops::prepareLiveArea(a.doc);
    a.notify(t ? std::string("Live area ready: each clip once per track at the song start") : std::string("Nothing to prepare"));
  }
  ui::Hint("Puts every different clip of each track once into a live area at the song start (the song moves right)");
  if (ImGui::BeginMenu(i18n::tr("Audio clip channels"))) {
    timeModeItems(a);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Declicking of the selected channels"))) {
    declickItems(a, selectedChannels(a));
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Delete unused automation clips")))
    report(a, ops::deleteUnusedAutomationClips(a.doc), "Deleted unused automation clips", "No unused automation clips");
}

// ============================================================ context menus ====

void channelItems(App& a, ObjectId channel) {
  const Song& s = a.doc.song();
  const Channel* c = s.channel(channel);
  if (!c) return;
  if (ImGui::MenuItem(i18n::tr("Clone with controller links"))) a.selectChannel(ops::cloneChannelWithLinks(a.doc, channel));
  ui::Hint("The copy follows the same MIDI controls and internal controllers as this channel");
  const Pattern* p = s.pattern(a.curPattern);
  if (p && ImGui::MenuItem(i18n::tr("Repeat the first bar of steps"), nullptr, false, p->bars > 1 && p->steps.count(channel)))
    ops::autoFillSteps(a.doc, p->id, channel);
  if (p && ImGui::MenuItem(i18n::tr("Bake note effects into the pattern"), nullptr, false, !c->noteFx.empty())) {
    if (burnNoteFxToPattern(a.doc, p->id, channel)) a.notify("Note effects baked into " + p->name);
    else a.notify("Nothing to bake: the channel has no notes or steps here");
  }
  ui::Hint("Plays the channel's note effects (arpeggiator, chords, …) and writes their output as notes");
  {  // the channel's sample in a new channel of a sample-based instrument
    const PluginInfo* pi = findPlugin(c->plugin.type);
    const bool hasSample = pi && pi->usesSample && !c->plugin.data.empty() && c->plugin.data[0] != '{' &&
                           c->plugin.data.find('\n') == std::string::npos;
    if (ImGui::BeginMenu(i18n::tr("Open the sample in a new channel"), hasSample)) {
      const std::string path = c->plugin.data;
      for (const char* type : {"gl.sampler", "gl.granular", "gl.slicer"})
        if (const PluginInfo* ti = findPlugin(type); ti && ImGui::MenuItem(ti->name.c_str())) a.addInstrument(type, path);
      ImGui::EndMenu();
    }
  }
  if (c->isLayer() && ImGui::BeginMenu(i18n::tr("Layer"))) {
    if (ImGui::MenuItem(i18n::tr("Put its channels in a new group"))) {
      int g = ops::groupLayerChildren(a.doc, channel, c->name);
      if (g >= 0) a.notify("Group \"" + a.doc.song().channelGroups[static_cast<size_t>(g)] + "\" created");
    }
    if (ImGui::MenuItem(i18n::tr("Delete its channels"))) report(a, ops::deleteLayerChildren(a.doc, channel), "Deleted the layer's channels", "No channels to delete");
    ImGui::EndMenu();
  }
}

void rackItems(App& a, const std::vector<ObjectId>& sel) {
  if (ImGui::MenuItem(i18n::tr("Swing mix for selected (type in)..."), nullptr, false, !sel.empty())) {
    std::vector<ObjectId> ids = sel;
    openPrompt("Swing mix for the selected channels (0-100 %)", "50", [&a, ids](const std::string& t) {
      const double v = std::clamp(std::atof(t.c_str()), 0.0, 100.0);
      ops::setSwingMix(a.doc, ids, static_cast<float>(v / 100.0));
    });
  }
  if (ImGui::BeginMenu(i18n::tr("Declicking of selected"), !sel.empty())) {
    declickItems(a, sel);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(i18n::tr("Audio clip channels"))) {
    timeModeItems(a);
    ImGui::EndMenu();
  }
}

void pianoEditItems(App& a) {
  const bool ok = pianoHasTarget(a);
  if (ImGui::MenuItem(i18n::tr("Delete all notes"), nullptr, false, ok))
    pianoEdit(a, "delete all notes", [](auto& n, auto& sel) { n.clear(), sel.clear(); });
  const Pattern* p = a.doc.song().pattern(a.curPattern);
  if (ImGui::BeginMenu(i18n::tr("Turn events into an automation clip"), p && !p->automation.empty())) {
    for (const auto& lane : p->automation) {
      std::string l = paramDisplayName(a.doc.song(), ParamId{lane.owner, lane.index}) + "##lane" +
                      std::to_string(lane.owner) + "_" + std::to_string(lane.index);
      if (ImGui::MenuItem(l.c_str()) && ops::laneToAutomationClip(a.doc, p->id, lane.owner, lane.index))
        a.notify("Automation clip created in the playlist");
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Save the notes as a MIDI file..."), nullptr, false, ok)) {
    const Pattern* pat = a.doc.song().pattern(a.curPattern);
    std::vector<Note> notes;
    if (auto it = pat->notes.find(a.selChannel); it != pat->notes.end()) notes = it->second;
    const int ppq = a.doc.song().ppq;
    const double bpm = a.doc.song().tempo;
    fileDialog(a, "Save notes as a MIDI file", true, {".mid"}, pat->name + ".mid", [&a, notes, ppq, bpm](const std::string& path) {
      std::string p2 = fs::path(path).extension().empty() ? path + ".mid" : path;
      std::string err;
      if (writeMidiFile(p2, ops::notesToMidiFile(notes, ppq, bpm, "Notes"), &err)) a.notify("Saved " + fs::path(p2).filename().string());
      else a.notify("Could not save: " + err, true);
    });
  }
}

void pianoToolItems(App& a) {
  const bool ok = pianoHasTarget(a);
  static bool removePart = false;
  if (ImGui::BeginMenu(i18n::tr("Keep notes by beat position"), ok)) {
    const Song& s = a.doc.song();
    struct Per {
      const char* name;
      Tick t;
    };
    const Per pers[] = {{"Of every beat", s.beatTicks()}, {"Of every 2 beats", s.beatTicks() * 2}, {"Of every bar", s.barTicks()}};
    for (const auto& per : pers)
      if (ImGui::BeginMenu(per.name)) {
        for (int parts : {2, 3, 4})
          for (int part = 1; part <= parts; ++part) {
            std::string l = (removePart ? "Remove part " : "Keep part ") + std::to_string(part) + " of " + std::to_string(parts);
            if (ImGui::MenuItem(l.c_str())) {
              const Tick period = per.t;
              const bool rm = removePart;
              pianoEdit(a, "keep notes by position", [&](auto& n, auto& sel) { ops::keepTimeSlot(n, sel, part, parts, period, rm); });
            }
          }
        ImGui::EndMenu();
      }
    ImGui::Separator();
    ImGui::MenuItem(i18n::tr("Remove the part instead"), nullptr, &removePart);
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(i18n::tr("Invert chords up"), nullptr, false, ok)) pianoEdit(a, "invert chords", [](auto& n, auto& s) { ops::invertChords(n, s, 1); });
  if (ImGui::MenuItem(i18n::tr("Invert chords down"), nullptr, false, ok)) pianoEdit(a, "invert chords", [](auto& n, auto& s) { ops::invertChords(n, s, -1); });
  const Channel* c = a.doc.song().channel(a.selChannel);
  if (ImGui::MenuItem(i18n::tr("Bake note effects into the notes"), nullptr, false, ok && c && !c->noteFx.empty()))
    burnNoteFxToPattern(a.doc, a.curPattern, a.selChannel);
}

void pianoSelectItems(App& a) {
  if (ImGui::MenuItem(i18n::tr("Stacked notes (chords)"), nullptr, false, pianoHasTarget(a)))
    pianoEdit(a, "", [](auto& n, auto& s) { ops::selectStacked(n, s); });
}

void pianoViewItems(App& a) {
  const bool ok = pianoHasTarget(a);
  std::vector<Note> notes;
  ops::NoteSel sel;
  if (ok) {
    const Pattern* p = a.doc.song().pattern(a.curPattern);
    if (auto it = p->notes.find(a.selChannel); it != p->notes.end()) notes = it->second;
    sel.assign(notes.size(), false);
    if (a.piano.selPattern == a.curPattern && a.piano.selChannel == a.selChannel)
      for (size_t i : a.piano.selected)
        if (i < sel.size()) sel[i] = true;
  }
  if (ImGui::MenuItem(i18n::tr("Scale highlighting from the notes"), nullptr, false, !notes.empty())) {
    auto g = ops::detectScale(notes, sel);
    if (g.scale > 0) {
      static const char* const keys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      a.piano.scaleRoot = g.root;
      a.piano.scale = g.scale;
      a.notify(std::string("Scale: ") + keys[g.root] + " " + tools::scales()[static_cast<size_t>(g.scale)].name + " (" +
               std::to_string(static_cast<int>(std::lround(g.fit * 100))) + "% of the notes)");
    }
  }
  const std::string chord = notes.empty() ? std::string() : ops::chordName(notes, sel);
  ImGui::TextDisabled(i18n::tr("Chord of the %s: %s"), pianoSelectionCount(a) ? "selection" : "notes", chord.empty() ? "\xE2\x80\x94" : chord.c_str());
}

void knobItems(App& a, ParamId pid) {
  if (!(pid == songParam(SongParam::Tempo))) return;
  if (ImGui::BeginMenu(i18n::tr("Tempo presets"))) {
    for (int bpm : {80, 90, 100, 110, 120, 130, 140, 150, 160}) {
      std::string l = std::to_string(bpm) + " BPM";
      if (ImGui::MenuItem(l.c_str(), nullptr, std::fabs(a.doc.song().tempo - bpm) < 0.001))
        a.doc.setParam(pid, bpm, false);
    }
    ImGui::EndMenu();
  }
}

// ================================================================== top bar ====

std::string positionText(App& a, double tick) {
  const int f = std::clamp(static_cast<int>(a.settings.getNumber("ui.timeFormat")), 0, static_cast<int>(ops::TimeFormat::Count) - 1);
  return ops::formatSongTime(a.doc.song(), tick, static_cast<ops::TimeFormat>(f), a.settings.getBool("ui.timeSteps"),
                             static_cast<int>(a.settings.getNumber("ui.smpteRate")));
}

void timeFormatMenu(App& a) {
  const int f = static_cast<int>(a.settings.getNumber("ui.timeFormat"));
  const int rate = static_cast<int>(a.settings.getNumber("ui.smpteRate"));
  ImGui::SeparatorText(i18n::tr("Song position shows"));
  if (ImGui::MenuItem(i18n::tr("Bars : beats : ticks"), nullptr, f == 0 && !a.settings.getBool("ui.timeSteps")))
    a.settings.set("ui.timeFormat", 0), a.settings.set("ui.timeSteps", false);
  if (ImGui::MenuItem(i18n::tr("Bars : steps : ticks"), nullptr, f == 0 && a.settings.getBool("ui.timeSteps")))
    a.settings.set("ui.timeFormat", 0), a.settings.set("ui.timeSteps", true);
  if (ImGui::MenuItem(i18n::tr("Minutes : seconds : hundredths"), nullptr, f == 1)) a.settings.set("ui.timeFormat", 1);
  if (ImGui::BeginMenu(i18n::tr("Timecode (SMPTE)"))) {
    const auto& rates = ops::smpteRates();
    for (size_t k = 0; k < rates.size(); ++k)
      if (ImGui::MenuItem(rates[k].name, nullptr, f == 2 && rate == static_cast<int>(k)))
        a.settings.set("ui.timeFormat", 2), a.settings.set("ui.smpteRate", static_cast<int>(k));
    ImGui::EndMenu();
  }
}

void stopMenuItems(App& a) {
  const bool on = a.settings.getBool("transport.rememberSeek");
  if (ImGui::MenuItem(i18n::tr("Stop returns to the set position"), nullptr, on)) a.settings.set("transport.rememberSeek", !on);
}

void stopPressed(App& a) { a.engine.stop(); }  // J-49: the engine returns to the seek position

void transportTick(App& a) {
  if (heldPitches.size() >= 2 && a.settings.getBool("ui.showPlayedChords")) {
    std::vector<Note> held;
    for (int p : heldPitches) held.push_back(Note(0, 1, p, 100));
    const std::string name = ops::chordName(held, ops::NoteSel(held.size(), false));
    if (!name.empty() && a.hint.empty()) a.setHint("Playing " + name);
  }
}

void logMidi(App& a, const MidiMessage& m) {
  if (m.noteOn()) {
    scoreLog.noteOn(a.now, m.d1, m.d2);
    heldPitches.insert(m.d1);
  } else if (m.noteOff()) {
    scoreLog.noteOff(a.now, m.d1);
    if (auto it = heldPitches.find(m.d1); it != heldPitches.end()) heldPitches.erase(it);
  }
}

}  // namespace gl::menuparity
