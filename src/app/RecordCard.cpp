// Gridline — K-123: the Recording card, the record strip and the Record window. See RecordCard.h.
// One look for all three: an 8 px grid, a label column and a value column, quiet headings, one
// primary action (Record and play, or Stop), slim continuous level meters and a mono time readout.
#include "RecordCard.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <vector>
#include "../core/I18n.h"
#include "../core/LevelMeter.h"
#include "../core/Meter.h"
#include "../core/MidiRecord.h"
#include "../core/RecordInfo.h"
#include "AudioRecordUi.h"
#include "DialogLayout.h"
#include "J31Ui.h"
#include "LiveLook.h"
#include "LogicPlugins.h"
#include "MenuParityUi.h"
#include "RecordLook.h"
#include "RecordRowsUi.h"
#include "RecordMoreUi.h"
#include "SettingsPages.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "Widgets.h"

namespace fs = std::filesystem;

namespace gl::recordcard {

using theme::pal;

namespace {

struct State {
  bool open = false;
  bool hidden = false;
  bool wasRecording = false;
  bool full = false;  // the Record window (Full options)
  int page = 0;
  std::set<std::string> seenSongs;  // record.window = first time per song: songs the card opened for
  double diskAt = -10;              // when the free space was read last
  double freeBytes = 0;
};
State& st() {
  static State s;
  return s;
}

// The grid: every gap and height is a multiple of 8 (4 for hairline nudges).
using reclook::kG;
using reclook::Kind;
constexpr float kCardW = 400.0f;         // the card's content width
constexpr float kStripH = 40.0f;         // the strip under the top bar
constexpr float kMeterH = 4.0f;          // a slim level meter
constexpr float kDbW = 40.0f;            // the dB readout beside a meter

int form(const App& a) { return std::clamp(static_cast<int>(a.settings.getNumber("record.form")), 0, 2); }

// What the card, the strip and the window show: the armed tracks, the disk time, the take.
struct Live {
  std::vector<const MixerTrack*> armed;
  bool recording = false, punch = false, loop = false;
  bool waiting = false;     // K-123 (0.7.7): Play waits for the first note
  bool quick = false;       // quick punch: Record punches while the transport plays
  bool punchedOut = false;  // quick punch, recording open, Record off
  int pass = 0;
  double minutes = 0, secs = 0;
};
Live gather(App& a, State& S) {
  Live L;
  const Song& s = a.doc.song();
  L.recording = a.audioRecSession && !a.audioRecFinishing;
  for (const auto& m : s.mixer)
    if (m.armed && m.id != s.masterId()) L.armed.push_back(&m);
  // Free disk space, read once a second.
  if (a.now - S.diskAt > 1.0) {
    std::error_code ec;
    fs::path dir = recordedDir(a);
    while (!dir.empty() && !fs::exists(dir, ec)) dir = dir.parent_path();
    const fs::space_info sp = fs::space(dir.empty() ? fs::path(".") : dir, ec);
    S.freeBytes = ec ? 0.0 : static_cast<double>(sp.available);
    S.diskAt = a.now;
  }
  const double sr = a.engine.sampleRate();
  const int bits = static_cast<int>(a.settings.getNumber("audio.recordBits"));
  L.minutes = recinfo::minutesLeft(S.freeBytes, sr, std::max<int>(1, static_cast<int>(L.armed.size())), bits == 24 ? 3 : bits == 16 ? 2 : 4);
  for (const RecordedTake& t : a.audioRec.takes()) L.pass = std::max(L.pass, t.pass + 1);
  Tick ra = 0, rb = 0;  // checkpoint 56: Clips punches at its loop brace
  const bool sel = livelook::recordRange(a, ra, rb);
  L.punch = a.recPunch && sel && a.engine.mode() == PlayMode::Song;
  L.loop = !L.punch && L.pass > 1;  // a second pass: the transport looped
  L.secs = L.recording ? a.audioRec.seconds(sr) : 0.0;
  L.waiting = recmore::waiting();
  L.quick = recmore::quickPunch(a);
  L.punchedOut = L.quick && recmore::punchRolling(a) && !a.recordArmed;
  if (recmore::punchRolling(a) && !L.recording) L.recording = a.recorder.active();  // notes only
  return L;
}

std::string takeLine(const Live& L) {
  return "Take " + std::to_string(std::max(1, L.pass)) + (L.punch ? " of the punch" : L.loop ? " of the loop" : "");
}
std::string diskLine(const Live& L) { return recinfo::durationText(L.minutes) + " of disk left"; }
std::string trackName(const Song& s, const MixerTrack& m) { return m.name.empty() ? "Insert " + std::to_string(s.mixerIndex(m.id)) : m.name; }
std::string songKey(const App& a) { return a.projectPath.empty() ? std::string("(unsaved)") : a.projectPath; }

// ---- drawing helpers ----

float textW(ImFont* f, float size, const char* t) { return f->CalcTextSizeA(size, FLT_MAX, 0, t).x; }

// A slim continuous level meter (the louder side of the pair) with its held peak, -60..0 dBFS.
// The fill takes the meter roles: low up to -6 dB, mid to 0, high above.
void slimMeter(ImDrawList* dl, ImVec2 p, float w, float level, float hold) {
  const auto& P = pal();
  auto pos = [](float g) { return g > 1e-6f ? std::clamp((20.0f * std::log10(g) + 60.0f) / 60.0f, 0.0f, 1.0f) : 0.0f; };
  const float r = kMeterH * 0.5f;
  const ImVec2 b(p.x + w, p.y + kMeterH);
  dl->AddRectFilled(p, b, P.well, r);
  const float v = pos(level), warn = pos(0.5f);
  if (v > 0.0f) {
    const float x = p.x + std::max(kMeterH, v * w);
    dl->AddRectFilled(p, ImVec2(std::min(x, p.x + warn * w), b.y), P.meterLow, r);
    if (v > warn) dl->AddRectFilled(ImVec2(p.x + warn * w, p.y), ImVec2(x, b.y), level >= 1.0f ? P.meterHigh : P.meterMid, r, ImDrawFlags_RoundCornersRight);
  }
  if (hold > 1e-4f) {
    const float hx = std::round(p.x + std::max(1.0f, pos(hold) * w) - 1.0f);
    dl->AddRectFilled(ImVec2(hx, p.y - 1), ImVec2(hx + 2, b.y + 1), hold >= 1.0f ? P.danger : theme::withAlpha(P.text, 0.7f));
  }
}

// One armed track's level: smoothed and held (core/LevelMeter ballistics), louder side.
void trackLevel(App& a, const MixerTrack& m, float& level, float& hold) {
  static std::map<ObjectId, LevelMeter> meters;
  float pl = 0, pr = 0;
  inputPeaks(a, m.id, pl, pr);
  if (m.input < 0) pl = pr = 0;
  LevelMeter& lm = meters[m.id];
  static double at = -1;
  static float dt = 0;
  if (at != a.now) dt = ImGui::GetIO().DeltaTime, at = a.now;
  lm.update(pl, pr, dt);
  level = std::max(lm.level(0), lm.level(1));
  hold = std::max(lm.hold(0), lm.hold(1));
}

std::string dbText(float g) {
  if (g <= 1e-5f) return "-inf";
  char b[16];
  std::snprintf(b, sizeof b, "%.1f", 20.0 * std::log10(g));
  return b;
}

// A track's level row at the cursor, `w` wide: the colour mark, the name, the meter, the dB.
void levelRow(App& a, const Song& s, const MixerTrack& m, float w, float nameW) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = 3 * kG, cy = p.y + h * 0.5f;
  dl->AddRectFilled(ImVec2(p.x, cy - 6), ImVec2(p.x + 3, cy + 6), theme::rgb(m.color), 1.5f);
  const std::string name = trackName(s, m);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontBody - 1, ImVec2(p.x + 12, cy - (theme::kFontBody - 1) * 0.5f - 1), nameW - 12, P.text, name.c_str());
  float level = 0, hold = 0;
  trackLevel(a, m, level, hold);
  const float mx = p.x + nameW + kG, mw = std::max(24.0f, w - nameW - kG - kG - kDbW);
  slimMeter(dl, ImVec2(mx, cy - kMeterH * 0.5f), mw, level, hold);
  const std::string db = dbText(hold);
  const float dw = textW(a.fontMono, theme::kFontLabel, db.c_str());
  dl->AddText(a.fontMono, theme::kFontLabel, ImVec2(p.x + w - dw, cy - theme::kFontLabel * 0.5f - 1), hold >= 1.0f ? P.danger : P.textDim, db.c_str());
  ImGui::Dummy(ImVec2(w, h));
}

// The record dot: a ring when ready, filled while recording.
void recDot(ImDrawList* dl, ImVec2 c, bool recording) {
  const auto& P = pal();
  if (recording) dl->AddCircleFilled(c, 5.0f, P.danger, 20);
  else dl->AddCircle(c, 4.5f, P.danger, 20, 1.5f);
}

// The fold (H) and close glyph buttons from (x, y), 20 px each, 4 apart.
void foldClose(State& S, ImDrawList* dl, float x, float y) {
  const auto& P = pal();
  const float cy = y + 10;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  if (ImGui::InvisibleButton("##fold", ImVec2(20, 20))) S.hidden = true;
  const bool hf = ImGui::IsItemHovered();
  ui::Hint("Folds to a small REC pill (H)");
  if (hf) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 20, y + 20), theme::withAlpha(P.text, 0.08f), 4);
  dl->AddLine(ImVec2(x + 6, cy), ImVec2(x + 14, cy), hf ? P.text : P.textDim, 1.5f);
  const float bx = x + 24;
  ImGui::SetCursorScreenPos(ImVec2(bx, y));
  if (ImGui::InvisibleButton("##close", ImVec2(20, 20))) S.open = false, S.hidden = false;
  const bool hc = ImGui::IsItemHovered();
  ui::Hint("Closes it (the record button stays on)");
  if (hc) dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + 20, y + 20), theme::withAlpha(P.text, 0.08f), 4);
  dl->AddLine(ImVec2(bx + 6, cy - 4), ImVec2(bx + 14, cy + 4), hc ? P.text : P.textDim, 1.5f);
  dl->AddLine(ImVec2(bx + 14, cy - 4), ImVec2(bx + 6, cy + 4), hc ? P.text : P.textDim, 1.5f);
}

// The main action and the take's secondary one, in a row at the cursor (actionWidth: their width).
// K-325: the layout's own buttons (bevel buttons in the Tracker); the punch button as wide as Stop's pair.
constexpr float kPunchW = 88.0f, kStopW = 72.0f;
void actionButtons(App& a, const Live& Lv, bool withPlay) {
  if (Lv.waiting) {  // K-123 (0.7.7): start on the first note
    if (reclook::rlButton(a, "Start now", Kind::Primary, 0, "Starts playback and recording without waiting for a note")) recmore::startNow(a);
    ImGui::SameLine(0, kG);
    if (reclook::rlButton(a, "Cancel", Kind::Normal, 0, "Stops waiting (Record stays on)")) recmore::cancelWait(a);
    return;
  }
  if (Lv.recording && Lv.quick) {  // K-123 (0.7.7): quick punch (the record key does the same)
    if (reclook::rlButton(a, a.recordArmed ? "Punch out" : "Punch in", Kind::Normal, kPunchW,
                        "Quick punch: stops or starts recording while playback goes on (the record key does the same)", !a.recordArmed)) {
      livelook::toggleArrangementRecord(a);
    }
    ImGui::SameLine(0, kG);
  }
  if (Lv.recording) {
    if (reclook::rlButton(a, "Stop", Kind::Primary, kStopW, "Stops the transport; the take is kept")) menuparity::stopButtonPressed(a);
    ImGui::SameLine(0, kG);
    if (reclook::rlButton(a, "Discard take", Kind::Danger, 0, "Stops and throws away what is being recorded")) j31::cancelRecording(a);
  } else if (!a.audioRecFinishing && withPlay) {
    if (reclook::rlButton(a, "Record and play", Kind::Primary, 0,
                          Lv.armed.empty() ? "Starts playback (nothing records until a track is armed)" : "Starts playback; the armed tracks record (Space does the same)")) {
      menuparity::playPressed(a);
    }
  }
}

float actionWidth(const App& a, const Live& Lv) {
  if (Lv.waiting) return reclook::buttonWidth("Start now") + kG + reclook::buttonWidth("Cancel");
  if (Lv.recording) return (Lv.quick ? kPunchW + kG : 0.0f) + kStopW + kG + reclook::buttonWidth("Discard take");
  return a.audioRecFinishing ? 0.0f : reclook::buttonWidth("Record and play");
}

}  // namespace

void open(App&) {
  st().open = true;
  st().hidden = false;
}
void openWindow(App&, int page) {
  st().full = true;
  if (page >= 0 && page < 7) st().page = page;
}
void showPage(int page) { st().page = page; }
void close() { st().open = false, st().hidden = false; }
bool isOpen() { return st().open; }
bool isHidden() { return st().open && st().hidden; }

void recordClicked(App& a, bool shift) {
  if (recmore::punchRolling(a)) return;  // K-123 (0.7.7): a quick punch, the card stays as it is
  if (!a.recordArmed) {
    close();
    return;
  }
  if (shift) return;  // Shift+click records without the card
  const int when = static_cast<int>(a.settings.getNumber("record.window"));
  if (when == 2) return;
  if (when == 1 && !st().seenSongs.insert(songKey(a)).second) return;
  open(a);
}

namespace {

// ---- the Record window ----

enum Page { kReady = 0, kMode, kStart, kTakes, kInputs, kNotes, kFiles, kPages };
const char* pageName(int p) {
  static const char* n[] = {"Ready", "Mode and punch", "Count-in", "Takes", "Inputs and monitoring", "Notes", "Files"};
  return p >= 0 && p < kPages ? n[p] : "";
}
const char* pageCaption(int p) {
  static const char* n[] = {"What the next take does", "Where a take starts and stops", "How a take begins", "What happens to each pass",
                            "Input, trim and listening of the armed tracks", "Recording from MIDI devices and the typing keyboard",
                            "How takes are written"};
  return p >= 0 && p < kPages ? n[p] : "";
}

// K-325 (round 2): under the tabs, as Settings: the page's caption in the dim colour (Clips' list and
// Smooth's tiles name the page, so there it says only that a take is recording).
void pageBlurb(bool tabs, int page, bool recording) {
  const bool busy = recording && (page == kReady || page == kMode || page == kFiles);
  if (!tabs && !busy) return;
  const char* cap = busy ? "Recording now: settings that would change the take wait until it stops" : pageCaption(page);
  ImGui::TextColored(theme::vec(pal().textDim), "%s", i18n::tr(cap));
  ImGui::Dummy(ImVec2(0, theme::kS1));
}

// A labelled value row.
void rowValue(const char* label, const std::string& v, ImU32 col = 0) {
  reclook::rlLabel(label);
  reclook::rlValue(v.c_str(), col);
}
// A labelled switch row bound to a setting.
void boolSetting(App& a, const char* label, const char* key, const char* hint) {
  bool b = a.settings.getBool(key);
  ImGui::PushID(key);
  reclook::rlLabel(label);
  if (reclook::rlToggle(a, "##b", &b, hint)) a.settings.set(key, b);
  ImGui::PopID();
}
// A labelled switch row bound to a flag.
bool boolRow(App& a, const char* label, const char* id, bool& v, const char* hint) {
  reclook::rlLabel(label);
  return reclook::rlToggle(a, id, &v, hint);
}
// A unit after a control, on its baseline (beats).
void unit(const char* text) {
  ImGui::SameLine(0, kG);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(theme::vec(pal().textFaint), "%s", i18n::tr(text));
}

void rollRow(App& a, const char* label, const char* onKey, const char* beatsKey, const char* hint) {
  static const char* kNames[] = {"Off", "1", "2", "4", "8", "16"};
  static const double kBeats[] = {0, 1, 2, 4, 8, 16};
  int cur = 0;
  if (a.settings.getBool(onKey)) {
    for (int i = 1; i < 6; ++i) {
      if (a.settings.getNumber(beatsKey) == kBeats[i]) cur = i;
    }
  }
  reclook::rlLabel(label);
  ImGui::PushID(onKey);
  if (reclook::rlChoice(a, "##roll", kNames, 6, &cur, reclook::kShortW, hint)) {
    a.settings.set(onKey, cur > 0);
    if (cur > 0) a.settings.set(beatsKey, kBeats[cur]);
  }
  unit("beats");
  ImGui::PopID();
}

void drawFull(App& a, State& S, const Live& Lv) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  // K-325 (round 2): the Settings window's frame in each layout (its size, padding, page navigation,
  // Smooth's inset panel and its form), with Render's footer (DialogFooter, buttons from 96 wide).
  ui::PlaceNextDialog(livelook::active(a) ? ImVec2(980, 600) : ImVec2(860, 560), ImGuiCond_Appearing, ImVec2(660, 360));
  bool open = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool shown = ImGui::Begin(i18n::tr("Record###recordwindow"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
  ui::KeepWindowInViewport();
  ImGui::PopStyleVar();
  if (!shown) {
    ImGui::End();
    S.full = open;
    return;
  }
  const bool recording = Lv.recording;
  const int armedN = static_cast<int>(Lv.armed.size());
  const float footH = ui::DialogFooterHeight();
  const SettingsNav nav = settingsNav(a);
  {
    using I = ui::Icon;
    static const I kIcons[kPages] = {I::Record, I::Trim, I::Metronome, I::LoopArrows, I::InputJack, I::CatMidi, I::Folder};
    const char* names[kPages];
    const char* hints[kPages];
    for (int p = 0; p < kPages; ++p) {
      names[p] = pageName(p);
      hints[p] = pageCaption(p);
    }
    settingsPageNav(a, "##recordtabs", names, kIcons, kPages, &S.page, hints, footH);
  }
  const bool panel = nav == SettingsNav::Icons;
  if (panel) {
    ImDrawList* wdl = ImGui::GetWindowDrawList();
    const float barTop = ImGui::GetCursorScreenPos().y;
    wdl->ChannelsSplit(2);
    wdl->ChannelsSetCurrent(1);
    settingsPagePanel(barTop, footH);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS3, 0));
  }
  ImGui::BeginChild("##page", ImVec2(0, -(footH + (panel ? theme::kS2 : 0.0f))), panel ? ImGuiChildFlags_AlwaysUseWindowPadding : ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  if (panel) ImGui::PopStyleVar();
  pageBlurb(nav == SettingsNav::Tabs, S.page, recording);
  const float fieldW = settingsFormBegin(a);
  const reclook::FormScope formScope(fieldW);
  const bool sel = a.playlist.selEnd > a.playlist.selStart;
  const int bits = static_cast<int>(a.settings.getNumber("audio.recordBits"));
  const char* fmt = bits == 24 ? "24-bit WAV" : bits == 16 ? "16-bit WAV" : "32-bit float WAV";
  switch (S.page) {
    case kReady: {
      reclook::rlSection("NEXT TAKE");
      rowValue("Mode", std::string(a.recPunch && sel ? "Punch at the time selection" : "From the playhead") +
                           (recmore::quickPunch(a) ? ", quick punch" : ""));
      if (a.settings.getBool("record.startOnNote")) rowValue("Start", "On the first note");  // K-123 (0.7.7)
      rowValue("Pre-roll", a.settings.getBool("record.preRoll") ? std::to_string(static_cast<int>(a.settings.getNumber("record.preRollBeats"))) + " beats" : "Off");
      rowValue("Count-in", a.countInBars ? std::to_string(a.countInBars) + (a.countInBars == 1 ? " bar" : " bars") : "Off");
      rowValue("Loop passes", a.settings.getBool("record.takeLanes") ? "Take lanes on the track" : "Tracks below");
      rowValue("Armed tracks", armedN ? std::to_string(armedN) : std::string(i18n::tr("None yet: arm tracks on Inputs and monitoring")), armedN ? 0 : P.textFaint);
      reclook::rlSection("FILES");
      rowValue("Format", fmt);
      reclook::rlLabel("Folder");
      ui::PathFit(recordedDir(a).c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.text, true);
      rowValue("Disk", recinfo::durationText(recinfo::minutesLeft(S.freeBytes, a.engine.sampleRate(), std::max(1, armedN), bits == 24 ? 3 : bits == 16 ? 2 : 4)) + " of recording left",
               Lv.minutes < 10 ? P.danger : 0);
      reclook::rlSection("WHEN RECORD IS CLICKED");
      static const char* kWhen[] = {"Every time", "First time per song", "Never"};
      static const char* kForm[] = {"Card", "Strip", "This window"};
      const float segW = 0;  // fitted, as Settings
      reclook::rlLabel("Open");
      int w = std::clamp(static_cast<int>(a.settings.getNumber("record.window")), 0, 2);
      if (reclook::rlChoice(a, "##when", kWhen, 3, &w, segW, "When the record controls open as Record is switched on. Shift+click on Record, and the record key, never open them")) {
        a.settings.set("record.window", w);
      }
      reclook::rlLabel("Show as");
      int f = form(a);
      if (reclook::rlChoice(a, "##form", kForm, 3, &f, segW, "Card: a small card at the bottom right. Strip: one row under the top bar. This window: the Record window stays open while a take records")) {
        a.settings.set("record.form", f);
      }
      break;
    }
    case kMode: {
      reclook::rlSection("PUNCH");
      reclook::rlLabel("Punch");
      int pv = a.recPunch ? 1 : 0;
      static const char* kPunch[] = {"Off", "At the time selection"};
      ImGui::BeginDisabled(recording);
      if (reclook::rlChoice(a, "##punch", kPunch, 2, &pv, 0,
                            logicplug::active(a) ? "In song mode, records only inside the tracks area's time selection"
                                                 : "In song mode, records only inside the playlist's time selection")) {
        a.recPunch = pv == 1;
      }
      ImGui::EndDisabled();
      if (sel) {
        const MeterMap mm(s);
        auto pos = [&](Tick t) {
          const MeterMap::Bbt b = mm.toBbt(t);
          return "bar " + std::to_string(b.bar + 1) + " beat " + std::to_string(b.beat + 1);
        };
        rowValue("Range", pos(a.playlist.selStart) + " to " + pos(a.playlist.selEnd));
      } else {
        // D9 (R14-14): Smooth's word for the arrangement, as its own menus and hints say it
        rowValue("Range", logicplug::active(a) ? i18n::tr("No time selection: drag in the tracks area's ruler")
                                               : i18n::tr("No time selection: drag in the playlist's ruler"),
                 P.textFaint);
      }
      {  // K-123 (0.7.7): quick punch
        bool q = recmore::quickPunch(a);
        ImGui::BeginDisabled(recording);
        if (boolRow(a, "Quick punch", "##quick", q, "While playing, Record (the button or the record key) punches in and out as often as you like; the punches are one take and one undo step")) {
          a.settings.set("record.quickPunch", q);
        }
        ImGui::EndDisabled();
      }
      reclook::rlSection("AROUND THE PUNCH");
      rollRow(a, "Pre-roll", "record.preRoll", "record.preRollBeats", "Playback starts this much before the punch in (the first pass)");
      rollRow(a, "Post-roll", "record.postRoll", "record.postRollBeats", "Playback goes on this long past the punch out, then stops");
      {  // K-123 (0.7.7): the punch crossfade
        reclook::rlLabel("Crossfade");
        static const char* kFade[] = {"Off", "5 ms", "10 ms", "20 ms", "50 ms"};
        static const int kMs[] = {0, 5, 10, 20, 50};
        const int cur = static_cast<int>(std::lround(a.settings.getNumber("record.punchFadeMs")));
        int fi = 2;
        for (int i = 0; i < 5; ++i) {
          if (cur == kMs[i]) fi = i;
        }
        ImGui::BeginDisabled(recording);
        if (reclook::rlChoice(a, "##pfade", kFade, 5, &fi, reclook::kShortW,
                               "At each punch in and out the take and the audio already on the track crossfade over this time, so the joins never click")) {
          a.settings.set("record.punchFadeMs", kMs[fi]);
        }
        ImGui::EndDisabled();
      }
      reclook::rlSection("CAPTURE WHILE ARMED");  // K-123 (0.7.7)
      {
        double held = 0;
        int tracks = 0;
        recmore::captureHeld(a, held, tracks);
        reclook::rlLabel("Kept");
        if (recmore::captureSeconds(a) <= 0) {
          reclook::rlValue(i18n::tr("Off: turn it on in Files"), P.textFaint);
        } else if (tracks == 0) {
          reclook::rlValue(i18n::tr("Nothing yet: armed tracks keep what they hear while the transport plays"), P.textFaint);
        } else {
          ImGui::AlignTextToFramePadding();
          ImGui::TextColored(theme::vec(P.text), "%s", (recinfo::clock(held) + (tracks == 1 ? std::string(" on 1 track") : " on " + std::to_string(tracks) + " tracks")).c_str());
          ImGui::SameLine(0, 2 * kG);
          ImGui::BeginDisabled(recording || a.audioRecFinishing);
          if (reclook::rlButton(a, "Recover the last pass", Kind::Normal, 0, "Places what the armed tracks heard in the last pass the transport played, where it was played (one undo step)")) {
            recmore::recoverAudio(a);
          }
          ImGui::EndDisabled();
        }
      }
      break;
    }
    case kStart: {
      reclook::rlSection("COUNT-IN");
      reclook::rlLabel("Bars");
      static const char* kCount[] = {"Off", "1", "2", "4"};
      static const int kBars[] = {0, 1, 2, 4};
      int ci = 0;
      for (int i = 0; i < 4; ++i) {
        if (a.countInBars == kBars[i]) ci = i;
      }
      if (reclook::rlChoice(a, "##count", kCount, 4, &ci, reclook::kShortW, "Bars of click before recording starts")) {
        a.countInBars = kBars[ci];
        if (kBars[ci] > 0) a.countInLength = kBars[ci];  // N59-10
      }
      reclook::rlSection("START");  // K-123 (0.7.7): start on the first note
      boolSetting(a, "On the first note", "record.startOnNote",
                  "With Record on, Play waits until you play a note (it records at the start), then starts without a count-in. Play again starts at once");
      {
        reclook::rlLabel("Or a sound above");
        static const char* kLv[] = {"Notes only", "-40 dB", "-30 dB", "-20 dB", "-12 dB"};
        static const int kDb[] = {0, -40, -30, -20, -12};
        const int cur = static_cast<int>(std::lround(a.settings.getNumber("record.startLevelDb")));
        int li = 0;
        for (int i = 0; i < 5; ++i) {
          if (cur == kDb[i]) li = i;
        }
        ImGui::BeginDisabled(!a.settings.getBool("record.startOnNote"));
        if (reclook::rlChoice(a, "##slevel", kLv, 5, &li, 0, "An armed track's input this loud starts it too, from the block that reached it")) a.settings.set("record.startLevelDb", kDb[li]);
        ImGui::EndDisabled();
      }
      break;
    }
    case kTakes: {
      reclook::rlSection("PASSES");
      boolSetting(a, "Take lanes", "record.takeLanes", "On: every pass of a loop lands on the track as a lane to comp from. Off: passes stack on the tracks below");
      if (logicplug::active(a)) {  // K-112B (R14-04): Smooth's arrange
        boolSetting(a, "Take folders", "record.midi.takeFolders",
                    "On: notes recorded over regions on the track make a take folder with them (the new take plays, the others stay as lanes). Off: the take stacks on them");
      }
      boolSetting(a, "Drop silent takes", "record.ignoreSilent", "Takes that never rise above -60 dBFS are not placed in the playlist");
      reclook::rlSection("AFTER RECORDING");
      boolSetting(a, "Disarm tracks", "record.autoUnarm", "Disarms every track when recording stops");
      boolSetting(a, "Name empty tracks", "record.nameTracks", "An empty playlist track takes the name of what was recorded on it");
      reclook::rlLabel("Name takes after");
      int nm = static_cast<int>(a.settings.getNumber("record.naming")) > 0 ? 1 : 0;
      static const char* kNames[] = {"Mixer track", "Audio input"};
      if (reclook::rlChoice(a, "##naming", kNames, 2, &nm, 0)) a.settings.set("record.naming", nm);
      recordrows::takesList(a, recording);  // K-123 (2.7): every track's takes, rated, soloed, auditioned
      break;
    }
    case kInputs: {
      reclook::rlSection("MONITORING");
      boolSetting(a, "Input only", "record.monitor.inputOnly", "On: armed tracks in Auto always hear their input. Off: inside the punch range only, and the playback around it");
      boolSetting(a, "Low latency", "record.monitor.lowLatency", "While recording, armed tracks skip their effects that add latency");
      {  // K-123 (0.7.7): the latency check
        reclook::rlSection("LATENCY");
        const double sr = std::max(1.0, a.engine.sampleRate());
        const int measured = recmore::measuredRoundTrip(a);
        const int rt = measured > 0 ? measured : a.audioInputLatency;
        reclook::rlLabel("Round trip");
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(P.text), "%s", recmore::latencyText(rt, sr).c_str());
        ImGui::SameLine(0, kG);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(P.textFaint), "%s", measured > 0 ? i18n::tr("measured") : i18n::tr("estimated from the buffers"));
        reclook::rlLabel("");
        if (reclook::rlButton(a, "Measure with a loopback cable...", Kind::Normal, 0, "A cable from an output back into an input: Gridline measures the round trip, and recordings line up exactly")) {
          recmore::openLatencyCheck(a);
        }
      }
      // The tracks: the name in the label column, the track's input panel (with its meter) on the
      // control column, so its rows line up with the switches above.
      int shown = 0;
      for (const auto& m : s.mixer) {
        if (m.id == s.masterId() || !(m.armed || m.input >= 0)) continue;
        if (!shown++) reclook::rlSection("TRACKS");
        ImGui::PushID(static_cast<int>(m.id));
        ui::FormLabelFit(trackName(s, m).c_str(), reclook::kColumn, P.text);  // a name: not translated
        ImGui::BeginGroup();
        mixerInputPanel(a, m, std::min(420.0f, ImGui::GetContentRegionAvail().x));
        ImGui::EndGroup();
        ImGui::PopID();
      }
      if (!shown) {
        reclook::rlSection("TRACKS");
        reclook::rlLabel("");
        reclook::rlValue(i18n::tr("No mixer track has an input yet: pick one in the mixer's input panel."), P.textFaint);
      }
      break;
    }
    case kNotes: {
      RecordOptions& o = a.recOptions;
      reclook::rlSection("WHAT RECORDS");
      boolRow(a, "Notes", "##notes", o.notes, "Notes played on a MIDI device or the typing keyboard record into the selected channel");
      boolRow(a, "Controller moves", "##ctl", o.automation, "Moves of controls and linked controllers record as automation");
      // K-123 (4.1): the rest of the input filter (yours: kept in this computer's settings).
      boolRow(a, "Pitch bend", "##bend", o.bend, "Pitch bend records: an MPE note's bend with the note, a linked bend as automation");
      boolRow(a, "Pressure", "##press", o.pressure, "Pressure (aftertouch) records: an MPE note's pressure with the note, linked pressure as automation");
      boolRow(a, "Program changes", "##prog", o.program, "Program changes linked to a control record as automation");
      reclook::rlSection("HOW");
      static const char* kOver[] = {"Merge", "Replace"};
      static const char* kQ[] = {"Off", "Starts", "Starts and lengths"};
      static const char* kT[] = {"Automatic", "Pattern events", "Automation clips"};
      const float segW = 0;  // fitted, as Settings
      reclook::rlLabel("Loop passes");
      int ov = o.overdub ? 0 : 1;
      if (reclook::rlChoice(a, "##over", kOver, 2, &ov, segW, "Merge keeps what was there. Replace clears what a pass plays over, from its first note on")) o.overdub = ov == 0;
      ImGui::BeginDisabled(recording);
      boolRow(a, "New take per pass", "##takes", o.takes, "Every loop pass records into a new pattern (a take)");
      ImGui::EndDisabled();
      reclook::rlLabel("Quantize");
      int q = !o.quantize ? 0 : o.quantizeLength ? 2 : 1;
      if (reclook::rlChoice(a, "##quant", kQ, 3, &q, segW, "Snaps recorded notes to the piano roll's grid")) o.quantize = q > 0, o.quantizeLength = q == 2;
      reclook::rlLabel("Controller moves into");
      int t = static_cast<int>(o.autoTarget);
      ImGui::BeginDisabled(recording);
      if (reclook::rlChoice(a, "##target", kT, 3, &t, segW, "Automatic: pattern events in pattern mode, automation clips in song mode")) o.autoTarget = static_cast<AutoRecordTarget>(t);
      ImGui::EndDisabled();
      reclook::rlSection("PLAYED WHILE NOT RECORDING");  // K-123 (4.2)
      reclook::rlLabel("Kept");
      {
        const int kept = a.capture.size();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(kept ? P.text : P.textFaint), "%s",
                           kept ? (std::to_string(kept) + (kept == 1 ? " note" : " notes") + " from the last two minutes").c_str() : i18n::tr("Nothing yet"));
        ImGui::SameLine(0, 2 * kG);
        ImGui::BeginDisabled(recording || kept == 0);
        if (reclook::rlButton(a, "Recover what I just played", Kind::Normal, 0, "The last phrase goes in where it was played (or at the pattern's start when the transport was stopped)")) {
          recordrows::recoverPlayed(a);
        }
        ImGui::EndDisabled();
      }
      break;
    }
    case kFiles: {
      reclook::rlSection("FORMAT");
      reclook::rlLabel("Bit depth");
      static const char* kBits[] = {"32-bit float", "24-bit", "16-bit"};
      int bi = bits == 24 ? 1 : bits == 16 ? 2 : 0;
      ImGui::BeginDisabled(recording);
      if (reclook::rlChoice(a, "##bits", kBits, 3, &bi, 0, "WAV files at the audio device's rate")) a.settings.set("audio.recordBits", bi == 1 ? 24 : bi == 2 ? 16 : 32);
      ImGui::EndDisabled();
      rowValue("Rate", std::to_string(static_cast<long long>(std::lround(a.engine.sampleRate()))) + " Hz");
      reclook::rlSection("WHERE");
      reclook::rlLabel("Folder");
      ui::PathFit(recordedDir(a).c_str(), std::max(80.0f, ImGui::GetContentRegionAvail().x), P.text, true);
      reclook::rlSection("CAPTURE WHILE ARMED");  // K-123 (0.7.7)
      {
        reclook::rlLabel("Keep");
        static const char* kKeep[] = {"Off", "30 s", "1 min", "2 min"};
        static const int kSec[] = {0, 30, 60, 120};
        const int cur = static_cast<int>(std::lround(recmore::captureSeconds(a)));
        int ki = 2;
        for (int i = 0; i < 4; ++i) {
          if (cur == kSec[i]) ki = i;
        }
        ImGui::BeginDisabled(recording);
        if (reclook::rlChoice(a, "##capture", kKeep, 4, &ki, reclook::kShortW,
                               "Every armed track keeps this much of its input while Record is off, so the last pass can be recovered (Mode and punch)")) {
          a.settings.set("record.captureSeconds", kSec[ki]);
        }
        ImGui::EndDisabled();
        const double mb = recmore::captureSeconds(a) * a.engine.sampleRate() * 8.0 / (1024.0 * 1024.0);
        if (mb > 0) rowValue("Memory", std::to_string(static_cast<long long>(std::lround(mb))) + " MB per armed track", P.textFaint);
      }
      break;
    }
    default: break;
  }
  reclook::rlSectionsEnd();
  settingsFormEnd();
  ImGui::EndChild();
  // Footer, as Render's: a rule, then back to the card and the main action at the right; while a
  // take records, its time and size at the left on the buttons' line.
  {
    const char* back = form(a) == recinfo::kStrip ? "Back to the strip" : "Back to the card";
    const float bw = reclook::footerButtonWidth(back);
    const float aw = recording ? actionWidth(a, Lv) : (a.audioRecFinishing ? 0.0f : reclook::footerButtonWidth("Record and play"));
    const float x0 = ImGui::GetCursorScreenPos().x;
    ui::DialogFooter(bw + (aw > 0 ? theme::kS2 + aw : 0.0f));
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    if (recording || a.audioRecFinishing) {
      ImDrawList* wdl = ImGui::GetWindowDrawList();
      const float cy = bp.y + ImGui::GetFrameHeight() * 0.5f;
      recDot(wdl, ImVec2(x0 + 6, cy), recording);
      const std::string t = recinfo::clock(Lv.secs);
      wdl->AddText(a.fontMonoBold, theme::kFontLcd, ImVec2(x0 + 18, std::round(cy - theme::kFontLcd * 0.5f - 1)), P.text, t.c_str());
      const float tw = textW(a.fontMonoBold, theme::kFontLcd, t.c_str());
      const std::string l = recording ? takeLine(Lv) + "  /  " + diskLine(Lv) : std::string(i18n::tr("Saving the take"));
      const float lx = x0 + 18 + tw + theme::kS3;
      wdl->PushClipRect(ImVec2(lx, bp.y), ImVec2(std::max(lx, bp.x - theme::kS3), bp.y + ImGui::GetFrameHeight()), true);
      wdl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(lx, std::round(cy - theme::kFontCaption * 0.5f - 1)), P.textDim, l.c_str());
      wdl->PopClipRect();
    }
    if (reclook::rlButton(a, back, Kind::Normal, bw, "Closes this window; the Recording card or strip stays")) open = false;
    if (aw > 0) {
      ImGui::SameLine(0, theme::kS2);
      if (!recording) {
        if (reclook::rlButton(a, "Record and play", Kind::Primary, aw, "Arms the record button and starts playback")) {
          a.recordArmed = true;
          S.open = true;
          menuparity::playPressed(a);
        }
      } else {
        actionButtons(a, Lv, true);
      }
    }
  }
  ImGui::End();
  S.full = open;
}

bool shownOpen(const App& a, const State& S) { return S.open && (a.recordArmed || a.audioRecSession); }

}  // namespace

// ---- the strip ----

float drawStrip(App& a, float top) {
  State& S = st();
  const Live Lv = gather(a, S);
  const recinfo::View v = recinfo::view(form(a), shownOpen(a, S), S.hidden, S.full, Lv.recording, false);
  if (!v.strip) return 0;
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + top));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kStripH));
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface800));
  ImGui::Begin("##recstrip", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 wp = ImGui::GetWindowPos();
  const float W = ImGui::GetWindowWidth();
  const float cy = wp.y + kStripH * 0.5f;
  // A hairline below; while recording, a thin record line along it.
  dl->AddLine(ImVec2(wp.x, wp.y + kStripH - 1), ImVec2(wp.x + W, wp.y + kStripH - 1), P.line);
  if (Lv.recording) dl->AddRectFilled(ImVec2(wp.x, wp.y + kStripH - 2), ImVec2(wp.x + W, wp.y + kStripH), P.danger);
  float x = wp.x + 2 * kG;
  // The dot, the time, the take.
  recDot(dl, ImVec2(x + 5, cy), Lv.recording);
  x += 3 * kG;
  const std::string t = recinfo::clock(Lv.secs);
  dl->AddText(a.fontMonoBold, theme::kFontLcd, ImVec2(x, cy - theme::kFontLcd * 0.5f - 1), Lv.recording ? P.text : P.textFaint, t.c_str());
  x += textW(a.fontMonoBold, theme::kFontLcd, t.c_str()) + 2 * kG;
  std::string what = Lv.punchedOut ? i18n::tr("Punched out")
                     : Lv.recording ? takeLine(Lv)
                     : a.audioRecFinishing ? i18n::tr("Saving the take")
                     : Lv.waiting ? i18n::tr("Waiting for the first note")
                                  : i18n::tr("Ready");
  if (!Lv.recording && Lv.punch) what += ", punch";
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x, cy - theme::kFontCaption * 0.5f - 1), P.textDim, what.c_str());
  x += textW(a.fontRegular, theme::kFontCaption, what.c_str()) + 3 * kG;
  // The right block, laid out from the right edge: disk, actions, Full options, fold and close.
  const float glyphs = 44, right = wp.x + W - 2 * kG - glyphs - 2 * kG;
  const float fullW = reclook::buttonWidth("Full options..."), aw = actionWidth(a, Lv);
  const float bx = right - fullW - (aw > 0 ? kG + aw : 0.0f);
  const std::string disk = diskLine(Lv);
  const float dw = textW(a.fontRegular, theme::kFontCaption, disk.c_str());
  const float diskX = bx - 3 * kG - dw;
  // The armed tracks' levels, as many as fit before the right block.
  constexpr float kCellMeter = 96;
  size_t shown = 0;
  for (const MixerTrack* m : Lv.armed) {
    const std::string name = trackName(s, *m);
    const float nw = std::min(96.0f, textW(a.fontRegular, theme::kFontCaption, name.c_str()));
    const float cellW = 3 + kG + nw + kG + kCellMeter + 3 * kG;
    if (x + cellW > diskX - kG) break;
    dl->AddRectFilled(ImVec2(x, cy - 6), ImVec2(x + 3, cy + 6), theme::rgb(m->color), 1.5f);
    theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(x + 3 + kG, cy - theme::kFontCaption * 0.5f - 1), nw, P.text, name.c_str());
    float level = 0, hold = 0;
    trackLevel(a, *m, level, hold);
    slimMeter(dl, ImVec2(x + 3 + kG + nw + kG, cy - kMeterH * 0.5f), kCellMeter, level, hold);
    x += cellW;
    ++shown;
  }
  if (shown < Lv.armed.size()) {
    const std::string more = "+" + std::to_string(Lv.armed.size() - shown);
    if (x + 24 < diskX) dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x, cy - theme::kFontCaption * 0.5f - 1), P.textDim, more.c_str());
  } else if (Lv.armed.empty() && x + 120 < diskX) {
    dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(x, cy - theme::kFontCaption * 0.5f - 1), P.textFaint, i18n::tr("No track is armed"));
  }
  if (diskX > x) dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(diskX, cy - theme::kFontCaption * 0.5f - 1), Lv.minutes < 10 ? P.danger : P.textFaint, disk.c_str());
  ImGui::SetCursorScreenPos(ImVec2(bx, std::round(cy - reclook::buttonHeight() * 0.5f)));
  if (aw > 0) {
    actionButtons(a, Lv, true);
    ImGui::SameLine(0, kG);
  }
  if (reclook::rlButton(a, "Full options...", Kind::Normal, fullW, "Opens the Record window: mode, punch, count-in, takes, inputs, notes and files", S.full)) S.full = !S.full;
  foldClose(S, dl, right + 2 * kG, cy - 10);
  ImGui::End();
  return kStripH;
}

// ---- the card, the pill, and the frame ----

void draw(App& a) {
  State& S = st();
  recordrows::frame(a);  // K-123: an audition ends with the transport
  if (!a.recordArmed && !a.audioRecSession && !recmore::punchRolling(a)) S.open = false;
  const Live Lv = gather(a, S);
  const bool started = Lv.recording && !S.wasRecording;
  S.wasRecording = Lv.recording;
  ImGuiIO& io = ImGui::GetIO();
  // H folds the card or the strip to its pill and back (not while typing, nor while the typing keyboard plays notes).
  if (S.open && !io.WantTextInput && !a.typingKeyboard && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && ImGui::IsKeyPressed(ImGuiKey_H, false))
    S.hidden = !S.hidden;
  const recinfo::View v = recinfo::view(form(a), S.open, S.hidden, S.full, Lv.recording, started);
  S.full = v.window;
  if (S.full) drawFull(a, S, Lv);
  if (!v.card && !v.pill) return;
  const auto& P = pal();
  const Song& s = a.doc.song();
  const bool recording = Lv.recording;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 corner(vp->WorkPos.x + vp->WorkSize.x - 3 * kG, vp->WorkPos.y + vp->WorkSize.y - 4 * kG);
  const ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
                              ImGuiWindowFlags_NoFocusOnAppearing;
  ImGui::SetNextWindowPos(corner, ImGuiCond_Always, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface800));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::vec(P.line));
  const bool trk = reclook::tracker();  // K-325: the Tracker's card is a classic square box in a bevel frame
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, trk ? 0.0f : kG);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, trk ? 0.0f : 1.0f);
  if (v.pill) {  // the REC pill
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, trk ? 0.0f : 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 6));
    if (ImGui::Begin("##recpill", nullptr, fl)) {
      reclook::cardFrame();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const std::string t = recording ? recinfo::clock(Lv.secs) : std::string(i18n::tr("Ready"));
      ImFont* f = recording ? a.fontMono : a.fontRegular;
      const float w = 18 + textW(f, theme::kFontBody - 1, t.c_str());
      if (ImGui::InvisibleButton("##pill", ImVec2(w, 16))) S.hidden = false;
      ui::Hint("Shows the record controls again (H)");
      recDot(dl, ImVec2(p.x + 5, p.y + 8), recording);
      dl->AddText(f, theme::kFontBody - 1, ImVec2(p.x + 16, p.y + 8 - (theme::kFontBody - 1) * 0.5f - 1), recording ? P.text : P.textDim, t.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    return;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2 * kG, 2 * kG));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kG, 0));
  if (ImGui::Begin("##reccard", nullptr, fl)) {
    reclook::cardFrame();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Header: the dot, the title and the mode; fold and close at the right.
    {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float cy = p.y + 10;
      recDot(dl, ImVec2(p.x + 5, cy), recording);
      const char* title = Lv.punchedOut ? i18n::tr("Punched out")
                          : recording   ? i18n::tr("Recording")
                          : a.audioRecFinishing ? i18n::tr("Saving the take")
                          : Lv.waiting ? i18n::tr("Waiting for the first note")
                                       : i18n::tr("Ready to record");
      dl->AddText(a.fontBold, theme::kFontBody, ImVec2(p.x + 2 * kG, cy - theme::kFontBody * 0.5f - 1), P.text, title);
      const float tw = textW(a.fontBold, theme::kFontBody, title);
      const std::string mode = recinfo::modeLine(false, Lv.punch, Lv.loop, Lv.pass);
      if (!mode.empty()) dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(p.x + 2 * kG + tw + kG, cy - theme::kFontCaption * 0.5f - 1), P.textDim, mode.c_str());
      foldClose(S, dl, p.x + kCardW - 44, p.y);
      ImGui::SetCursorScreenPos(p);
      ImGui::Dummy(ImVec2(kCardW, 20));
    }
    ImGui::Dummy(ImVec2(0, 2 * kG));
    // The time, and the take (or what is armed) with the disk time beside it.
    {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float big = 28.0f;
      const std::string t = recinfo::clock(Lv.secs);
      dl->AddText(a.fontMonoBold, big, p, recording ? P.text : P.textFaint, t.c_str());
      const float tw = textW(a.fontMonoBold, big, t.c_str());
      std::string l1;
      if (recording) l1 = takeLine(Lv);
      else l1 = Lv.armed.empty() ? std::string(i18n::tr("No track is armed")) : std::to_string(Lv.armed.size()) + (Lv.armed.size() == 1 ? " track armed" : " tracks armed");
      const std::string l2 = diskLine(Lv);
      const float lx = p.x + tw + 2 * kG;
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(lx, p.y + 2), P.textDim, l1.c_str());
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(lx, p.y + 18), Lv.minutes < 10 ? P.danger : P.textFaint, l2.c_str());
      ImGui::Dummy(ImVec2(kCardW, 4 * kG));
    }
    ImGui::Dummy(ImVec2(0, 2 * kG));
    // The armed tracks' levels.
    if (Lv.armed.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kCardW);
      ImGui::TextUnformatted(i18n::tr("Arm a track with the record dot on its mixer strip, or in the mixer's input panel."));
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
    }
    for (const MixerTrack* m : Lv.armed) {
      ImGui::PushID(static_cast<int>(m->id));
      levelRow(a, s, *m, kCardW, 128);
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 2 * kG));
    // Actions: the main one first, Full options quiet at the right.
    {
      const float y0 = ImGui::GetCursorScreenPos().y, x0 = ImGui::GetCursorScreenPos().x;
      actionButtons(a, Lv, true);
      const float fw = reclook::buttonWidth("Full options...");
      ImGui::SetCursorScreenPos(ImVec2(x0 + kCardW - fw, y0));
      if (reclook::rlButton(a, "Full options...", Kind::Normal, fw, "Opens the Record window: mode, punch, count-in, takes, inputs, notes and files", S.full)) S.full = !S.full;
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(2);
}

}  // namespace gl::recordcard
