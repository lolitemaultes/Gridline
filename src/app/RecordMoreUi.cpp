// Gridline - K-123 (0.7.7 recording rows, core A). See RecordMoreUi.h.
#include "RecordMoreUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../core/I18n.h"
#include "../engine/ArmedRing.h"
#include "../engine/AudioRecord.h"
#include "../engine/Loopback.h"
#include "App.h"
#include "AudioRecordUi.h"
#include "DevSignal.h"
#include "LayoutPresets.h"
#include "LiveLook.h"
#include "DialogLayout.h"
#include "SettingsPages.h"
#include "RecordCard.h"
#include "RecordLook.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::recmore {

using theme::pal;

namespace {

struct State {
  // start on the first note
  bool waiting = false, bypass = false;
  int startsSeen = 0;
  std::vector<HeldNote> held;
  // the latency check
  bool open = false;
  int phase = 0;  // 0 ready, 1 measuring, 2 result
  int channel = 0;
  std::vector<float> burst;
  std::vector<loopback::Lag> runs;
  loopback::Summary sum;
  float level = 0;
  bool inverted = false;
};
State& st() {
  static State s;
  return s;
}

constexpr int kRuns = 3;

std::string msText(int frames, double sr) { return latencyText(frames, sr); }

std::string currentSetup(const App& a) {
  if (a.jack.active) {
    const std::string tag = "jack:" + a.jack.clientName;
    return loopback::setupKey(tag, tag, a.engine.sampleRate(), a.audioBuffer);
  }
  return loopback::setupKey(a.audioDevice, a.audioInputDevice, a.engine.sampleRate(), a.audioBuffer);
}

bool punchRange(App& a) {
  Tick ra = 0, rb = 0;
  return a.recPunch && livelook::recordRange(a, ra, rb) && a.engine.mode() == PlayMode::Song;
}

bool audioArmed(const App& a) {
  for (const auto& m : a.doc.song().mixer) {
    if (m.armed && m.input >= 0 && m.recPoint == 0) return true;
  }
  return false;
}

void startProbe(App& a, State& S) {
  if (S.burst.empty()) S.burst = loopback::probeSignal(a.engine.sampleRate());
  if (!a.engine.startLatencyProbe(S.channel, S.burst, loopback::listenFrames(a.engine.sampleRate()))) S.phase = 0;
}

void beginMeasure(App& a, State& S) {
  if (a.engine.playing()) a.engine.stop();
  S.runs.clear();
  S.sum = {};
  S.level = 0;
  S.inverted = false;
  S.burst = loopback::probeSignal(a.engine.sampleRate());
  S.phase = 1;
  a.settings.set("audio.loopbackInput", S.channel);
  startProbe(a, S);
}

// The measuring goes on from frame to frame, whether the window shows or not.
void stepMeasure(App& a, State& S) {
  if (S.phase != 1 || !a.engine.latencyProbeDone()) return;
  const loopback::Lag l = loopback::findLag(S.burst, a.engine.latencyProbeCapture());
  S.runs.push_back(l);
  S.level = std::max(S.level, l.level);
  S.inverted = S.inverted || (l.found && l.inverted);
  if (static_cast<int>(S.runs.size()) < kRuns) {
    startProbe(a, S);
    return;
  }
  a.engine.cancelLatencyProbe();
  S.sum = loopback::summarise(S.runs);
  devsig::reach("latency-measured");  // dev checks (waitfor)
  S.phase = 2;
}

void useMeasured(App& a, State& S) {
  if (!S.sum.ok) return;
  a.settings.set("audio.loopbackLatency", S.sum.frames);
  a.settings.set("audio.loopbackSetup", currentSetup(a));
  a.notify(std::string(i18n::tr("Recordings now use the measured round trip: ")) + msText(S.sum.frames, a.engine.sampleRate()));
}

void clearMeasured(App& a) {
  a.settings.set("audio.loopbackLatency", 0);
  a.settings.set("audio.loopbackSetup", std::string());
}

TakeCommitOptions commitOptions(App& a) {
  TakeCommitOptions o;
  o.dir = recordedDir(a);
  const int rb = static_cast<int>(a.settings.getNumber("audio.recordBits"));
  o.bits = rb == 24 || rb == 16 ? rb : 32;
  o.sampleRate = a.engine.sampleRate();
  o.naming = a.settings.getNumber("record.naming") > 0.5 ? TakeCommitOptions::Naming::Input : TakeCommitOptions::Naming::Insert;
  o.nameTracks = a.settings.getBool("record.nameTracks");
  o.lanes = a.settings.getBool("record.takeLanes");
  return o;
}

}  // namespace

std::string latencyText(int frames, double sampleRate) {
  // Tenths of a millisecond, without a format buffer.
  const long long tenths = std::llround(std::clamp(10000.0 * frames / std::max(1.0, sampleRate), -1e12, 1e12));
  const long long whole = tenths / 10, frac = std::llabs(tenths % 10);
  return std::to_string(frames) + " samples (" + (tenths < 0 && whole == 0 ? "-" : "") + std::to_string(whole) + "." + std::to_string(frac) + " ms)";
}

// ---------------------------------------------------------------- quick punch

bool quickPunch(const App& a) { return a.settings.getBool("record.quickPunch"); }

bool punchRolling(const App& a) {
  return quickPunch(a) && a.engine.playing() && a.engine.mode() != PlayMode::Live && (a.audioRecSession || a.recorder.active());
}

int punches(const App& a) {
  int n = 0;
  ObjectId first = kNoId;
  for (const RecordedTake& t : a.audioRec.takes()) {
    if (first == kNoId) first = t.mixer;
    n += t.mixer == first;
  }
  return std::max(n, a.recorder.active() ? 1 : 0);
}

// ---------------------------------------------------------------- start on the first note

bool holdPlay(App& a) {
  State& S = st();
  if (S.bypass) return false;
  if (S.waiting) {  // Play again while waiting: at once
    S.waiting = false;
    a.engine.armStartOnInput(0);
    return false;
  }
  if (!a.recordArmed || !a.settings.getBool("record.startOnNote") || a.engine.mode() == PlayMode::Live || a.audioRecFinishing) return false;
  S.waiting = true;
  S.held.clear();
  S.startsSeen = a.engine.inputStarts();
  // An armed track's input can start it too: the engine starts on the block that reaches the level,
  // with recording on from that block (the punch range as Play sets it).
  const double db = a.settings.getNumber("record.startLevelDb");
  if (db < 0 && a.recAudio && audioArmed(a)) {
    Tick ra = 0, rb = 0;
    if (a.recPunch && livelook::recordRange(a, ra, rb) && a.engine.mode() == PlayMode::Song) {
      a.engine.setPunch(static_cast<double>(ra), static_cast<double>(rb));
    } else {
      a.engine.setPunch(0, 0);
    }
    a.engine.setRecording(true);
    a.engine.armStartOnInput(static_cast<float>(std::pow(10.0, db / 20.0)));
  }
  return true;
}

bool waiting() { return st().waiting; }

void startNow(App& a) {
  State& S = st();
  if (!S.waiting) return;
  S.waiting = false;
  a.engine.armStartOnInput(0);
  S.bypass = true;
  a.startPlayback();
  S.bypass = false;
}

void cancelWait(App& a) {
  State& S = st();
  if (!S.waiting) return;
  S.waiting = false;
  S.held.clear();
  a.engine.armStartOnInput(0);
  a.engine.setRecording(false);
}

void noteWhileWaiting(App& a, ObjectId channel, int pitch, int velocity) {
  State& S = st();
  if (!S.waiting) return;
  S.held.push_back({channel, pitch, velocity});
  startNow(a);
}

std::vector<HeldNote> takeStartNotes() {
  std::vector<HeldNote> out;
  out.swap(st().held);
  return out;
}

// ---------------------------------------------------------------- capture while armed

double captureSeconds(const App& a) { return std::clamp(a.settings.getNumber("record.captureSeconds"), 0.0, 600.0); }

void captureHeld(App& a, double& seconds, int& tracks) {
  seconds = 0;
  tracks = 0;
  const auto& p = a.engine.program();
  if (!p) return;
  const double sr = std::max(1.0, a.engine.sampleRate());
  for (const auto& [id, ring] : armRingsOf(*p)) {
    size_t first = 0, end = 0;
    if (!ring || !ring->lastRun(static_cast<size_t>(sr * 0.5), first, end)) continue;
    ++tracks;
    seconds = std::max(seconds, static_cast<double>(end - first) / sr);
  }
}

int recoverAudio(App& a) {
  const auto& p = a.engine.program();
  std::vector<RecordedTake> takes = p ? recoverLastPass(armRingsOf(*p), a.engine.sampleRate()) : std::vector<RecordedTake>{};
  if (takes.empty()) {
    a.notify(i18n::tr("Nothing to recover: play with a track armed while the transport runs"), true);
    return 0;
  }
  TakeCommitOptions o = commitOptions(a);
  TakeCommitResult r;
  a.doc.editSong("Recover audio", [&](Song& x) { r = commitTakes(x, takes, o); });
  if (!r.error.empty()) a.notify("Recover: " + r.error, true);
  if (!r.clips.empty()) {
    a.playlist.selected.clear();
    a.playlist.selected.insert(r.clips.begin(), r.clips.end());
    a.notify(std::string(i18n::tr("Recovered the last pass: ")) + std::to_string(r.clips.size()) + (r.clips.size() == 1 ? " audio clip" : " audio clips"));
  }
  return static_cast<int>(r.clips.size());
}

// ---------------------------------------------------------------- punch crossfades

double punchFadeSeconds(const App& a) { return std::clamp(a.settings.getNumber("record.punchFadeMs"), 0.0, 500.0) / 1000.0; }


static bool punching(App& a) { return quickPunch(a) || punchRange(a); }

void beforeCommit(App& a, std::vector<RecordedTake>& takes) {
  // Only punches crossfade: a punch range, or quick punch. Elsewhere a take starts as it always did.
  if (punching(a) && punchFadeSeconds(a) > 0) return;
  for (RecordedTake& t : takes) {
    t.punchIn = t.punchOut = false;
  }
}

// ---------------------------------------------------------------- latency

int measuredRoundTrip(const App& a) {
  const int m = static_cast<int>(a.settings.getNumber("audio.loopbackLatency"));
  if (m <= 0 || a.settings.getString("audio.loopbackSetup") != currentSetup(a)) return 0;
  return m;
}

void openLatencyCheck(App& a) {
  State& S = st();
  S.open = true;
  if (S.phase != 1) S.phase = 0;
  S.channel = std::clamp(static_cast<int>(a.settings.getNumber("audio.loopbackInput")), 0, std::max(0, a.engine.inputChannels() - 1));
}

bool latencyCheckOpen() { return st().open; }

// ---------------------------------------------------------------- per frame

void frame(App& a) {
  State& S = st();
  // Capture while armed: the length goes to the builder (rings are made there); not mid-take.
  const double secs = captureSeconds(a);
  if (secs != a.builder.armedCapture() && !a.audioRecSession) {
    a.builder.setArmedCapture(secs);
    a.forceRebuild();
  }
  a.audioRec.setQuickPunch(quickPunch(a));
  {  // punch crossfades reach into the capture-while-armed rings (only punches: a range or quick punch)
    const auto& p = a.engine.program();
    a.audioRec.setPunchPads(p ? armRingsOf(*p) : ArmRings{}, punching(a) ? punchFadeSeconds(a) : 0.0, a.engine.sampleRate());
  }
  if (S.waiting) {
    if (!a.recordArmed || a.engine.mode() == PlayMode::Live) {
      cancelWait(a);
    } else if (a.engine.inputStarts() != S.startsSeen) {  // a sound started it (the engine plays already)
      S.startsSeen = a.engine.inputStarts();
      S.waiting = false;
    }
  }
  stepMeasure(a, S);
}

void draw(App& a) {
  State& S = st();
  if (!S.open) return;
  const auto& P = pal();
  const double sr = std::max(1.0, a.engine.sampleRate());
  // K-325 (round 2): laid out as Render and Settings: their padding, the steps as the dim line under
  // the title bar, Settings' sections and form, and Render's footer (buttons from 96 wide, the main
  // one last: Measure, or Use this once a measurement agrees).
  ui::PlaceNextDialog(ImVec2(560, 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
  bool open = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  const bool shown = ImGui::Begin(i18n::tr("Latency check###latencycheck"), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
  ui::KeepWindowInViewport();
  if (shown) ui::SettleDialogCentred();
  ImGui::PopStyleVar();
  if (shown) {
    const float W = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(P.textDim));
    ImGui::TextUnformatted(i18n::tr("1. Connect an output of your audio interface to one of its inputs with a cable."));
    ImGui::TextUnformatted(i18n::tr("2. Turn the speakers down: a short burst of noise plays through the outputs."));
    ImGui::TextUnformatted(i18n::tr("3. Pick the input the cable comes back on and press Measure. Playback stops while it measures."));
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, theme::kS1));
    const float fieldW = settingsFormBegin(a);
    const reclook::FormScope formScope(fieldW);
    reclook::rlSection("SETUP");
    const int inputs = a.engine.inputChannels();
    reclook::rlLabel("Input");
    if (inputs <= 0) {
      reclook::rlValue(i18n::tr("No input device: choose one in Settings > Audio"), P.textFaint);
    } else {
      static const char* kIn[] = {"In 1", "In 2", "In 3", "In 4", "In 5", "In 6", "In 7", "In 8"};
      int ch = std::clamp(S.channel, 0, inputs - 1);
      ImGui::BeginDisabled(S.phase == 1);
      if (reclook::rlChoice(a, "##lbin", kIn, std::min(inputs, 8), &ch, reclook::kShortW)) S.channel = ch;
      ImGui::EndDisabled();
    }
    const int measured = measuredRoundTrip(a);
    reclook::rlLabel("Now in use");
    reclook::rlValue((measured > 0 ? msText(measured, sr) + i18n::tr(", measured") : msText(a.audioInputLatency, sr) + i18n::tr(", estimated from the buffers")).c_str());
    // The result.
    if (S.phase != 0) {
      reclook::rlSection("RESULT");
      reclook::rlLabel("Round trip");
      if (S.phase == 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::vec(P.text), i18n::tr("Measuring... %d of %d"), static_cast<int>(S.runs.size()) + 1, kRuns);
      } else if (S.sum.ok) {
        ImGui::BeginGroup();
        ImGui::PushFont(a.fontMonoBold, theme::kFontLcd);
        ImGui::TextUnformatted(msText(S.sum.frames, sr).c_str());
        ImGui::PopFont();
        ImGui::TextColored(theme::vec(P.textDim), i18n::tr("%d of %d measurements agree"), S.sum.agree, S.sum.total);
        if (S.inverted) ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("The input comes back with its polarity turned over."));
        ImGui::EndGroup();
      } else if (S.level < 1e-4f) {
        reclook::rlValue(i18n::tr("Nothing came back on this input: check the cable and the input."), P.danger);
      } else {
        reclook::rlValue(i18n::tr("The measurements did not agree: turn the input's monitoring off and try again."), P.danger);
      }
    }
    reclook::rlSectionsEnd();
    settingsFormEnd();
    // The footer: the extra actions first, Close, the main action last.
    const bool result = S.phase == 2 && S.sum.ok, back = measured > 0 && S.phase != 1;
    const char* primary = S.phase == 1 ? nullptr : result ? "Use this" : "Measure";
    const float wBack = reclook::footerButtonWidth("Back to the estimate"), wAgain = reclook::footerButtonWidth("Measure again");
    const float wClose = reclook::footerButtonWidth("Close"), wMain = primary ? reclook::footerButtonWidth(primary) : 0.0f;
    float bw = wClose;
    if (back) bw += wBack + theme::kS2;
    if (result) bw += wAgain + theme::kS2;
    if (primary) bw += wMain + theme::kS2;
    ui::DialogFooter(bw);
    if (back) {
      if (reclook::rlButton(a, "Back to the estimate", reclook::Kind::Normal, wBack, "Forgets the measurement: recordings use the estimate from the buffers again")) clearMeasured(a);
      ImGui::SameLine(0, theme::kS2);
    }
    if (result) {
      if (reclook::rlButton(a, "Measure again", reclook::Kind::Normal, wAgain)) beginMeasure(a, S);
      ImGui::SameLine(0, theme::kS2);
    }
    if (reclook::rlButton(a, "Close", reclook::Kind::Normal, wClose)) open = false;
    if (result) {
      ImGui::SameLine(0, theme::kS2);
      if (reclook::rlButton(a, "Use this", reclook::Kind::Primary, wMain, "Recordings are moved earlier by this round trip (plus plugin delay compensation) while these devices, rate and buffer play")) {
        useMeasured(a, S);
        open = false;
      }
    } else if (primary) {
      ImGui::SameLine(0, theme::kS2);
      ImGui::BeginDisabled(inputs <= 0);
      if (reclook::rlButton(a, "Measure", reclook::Kind::Primary, wMain, "Plays the burst three times and listens for it on the input")) beginMeasure(a, S);
      ImGui::EndDisabled();
    }
  }
  ImGui::End();
  if (!open) {
    S.open = false;
    if (S.phase == 1) {
      a.engine.cancelLatencyProbe();
      S.phase = 0;
    }
  }
}

// ---------------------------------------------------------------- menus

void punchMenuItems(App& a) {
  bool q = quickPunch(a);
  if (ImGui::MenuItem(i18n::tr("Quick punch"), nullptr, &q)) a.settings.set("record.quickPunch", q);
  ui::Hint("While playing, Record punches in and out as often as you like: the punches are one take and one undo step");
}

void startMenuItems(App& a) {
  bool w = a.settings.getBool("record.startOnNote");
  if (ImGui::MenuItem(i18n::tr("Start on the first note"), nullptr, &w)) a.settings.set("record.startOnNote", w);
  ui::Hint("With Record on, Play waits until you play a note (or an armed input gets loud) and starts there");
}

void advancedMenuItems(App& a) {
  if (ImGui::BeginMenu(i18n::tr("Punch crossfade"))) {
    const int cur = static_cast<int>(std::lround(a.settings.getNumber("record.punchFadeMs")));
    static const int kMs[] = {0, 5, 10, 20, 50};
    for (int ms : kMs) {
      const std::string n = ms == 0 ? std::string(i18n::tr("Off")) : std::to_string(ms) + " ms";
      if (ImGui::MenuItem(n.c_str(), nullptr, cur == ms)) a.settings.set("record.punchFadeMs", ms);
    }
    ImGui::EndMenu();
  }
  ui::Hint("At each punch in and out the take and the audio already on the track crossfade over this time");
  if (ImGui::BeginMenu(i18n::tr("Capture while armed"))) {
    const int cur = static_cast<int>(std::lround(captureSeconds(a)));
    static const int kS[] = {0, 30, 60, 120};
    static const char* kN[] = {"Off", "30 seconds", "1 minute", "2 minutes"};
    for (int i = 0; i < 4; ++i) {
      if (ImGui::MenuItem(i18n::tr(kN[i]), nullptr, cur == kS[i], !a.audioRecSession)) a.settings.set("record.captureSeconds", kS[i]);
    }
    // integ-1124: Recover sits with the capture it recovers, so the record options menu still fits 1440x900.
    ImGui::Separator();
    double secs = 0;
    int tracks = 0;
    captureHeld(a, secs, tracks);
    if (ImGui::MenuItem(i18n::tr("Recover the last pass"), nullptr, false, tracks > 0 && !a.audioRecSession)) recoverAudio(a);
    ui::Hint("Places what the armed tracks heard in the last pass the transport played, where it was played");
    ImGui::EndMenu();
  }
  ui::Hint("Armed tracks keep this much of their input while Record is off, so a pass can be recovered");
  if (ImGui::MenuItem(i18n::tr("Latency check..."))) openLatencyCheck(a);
  ui::Hint("Measures the round trip through a loopback cable, so recordings line up exactly");
}

void latencySettingsRow(App& a) {
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton(i18n::tr("Measure..."), false, 0, "Measures the round trip through a loopback cable")) openLatencyCheck(a);
}

// ---------------------------------------------------------------- dev input

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args, int frame) {
  if (cmd != "recmore" || args.empty()) return false;
  const std::string& w = args[0];
  const std::string v = args.size() > 1 ? args[1] : std::string();
  State& S = st();
  if (w == "show") {
    double secs = 0;
    int tracks = 0;
    captureHeld(a, secs, tracks);
    std::printf("recmore frame %d: quick %d fade %g capture %g startnote %d level %g waiting %d starts %d playing %d recording %d session %d "
                "held %.1f tracks %d measured %d latency %d punches %d\n",
                frame, quickPunch(a) ? 1 : 0, a.settings.getNumber("record.punchFadeMs"), captureSeconds(a), a.settings.getBool("record.startOnNote") ? 1 : 0,
                a.settings.getNumber("record.startLevelDb"), S.waiting ? 1 : 0, a.engine.inputStarts(), a.engine.playing() ? 1 : 0,
                a.engine.recordingOn() ? 1 : 0, a.audioRecSession ? 1 : 0, secs, tracks, measuredRoundTrip(a), a.engine.recordLatency(), punches(a));
  } else if (w == "quick") {
    a.settings.set("record.quickPunch", std::atoi(v.c_str()) != 0);
  } else if (w == "fade") {
    a.settings.set("record.punchFadeMs", std::atof(v.c_str()));
  } else if (w == "capture") {
    a.settings.set("record.captureSeconds", std::atof(v.c_str()));
  } else if (w == "startnote") {
    a.settings.set("record.startOnNote", std::atoi(v.c_str()) != 0);
  } else if (w == "startlevel") {
    a.settings.set("record.startLevelDb", std::atof(v.c_str()));
  } else if (w == "arm") {  // recmore arm <mixer index>: as the strip's record dot
    const auto& mx = a.doc.song().mixer;
    const auto i = static_cast<size_t>(std::max(0, std::atoi(v.c_str())));
    if (i < mx.size() && !mx[i].armed) toggleArm(a, mx[i].id);
  } else if (w == "recover") {
    // K-326: recover first, then read the undo label (as arguments of one call their order is
    // unspecified: GCC read the label before the recovery made its undo step).
    const int recovered = recoverAudio(a);
    std::printf("recmore frame %d: recovered %d undo \"%s\"\n", frame, recovered, a.doc.history().undoLabel().c_str());
  } else if (w == "punch") {  // the record key while playing
    livelook::toggleArrangementRecord(a);
  } else if (w == "clips") {  // recmore clips [<track>]: the audio clips (on a playlist track), with their fades
    const int tr = v.empty() ? -1 : std::atoi(v.c_str());
    std::vector<const Clip*> cs;
    for (const Clip& c : a.doc.song().playlist) {
      if ((tr < 0 || c.track == tr) && c.kind == ClipKind::Audio) cs.push_back(&c);
    }
    std::sort(cs.begin(), cs.end(), [](const Clip* x, const Clip* y) { return x->start < y->start; });
    std::printf("recmore frame %d: track %d clips %zu undo \"%s\":", frame, tr, cs.size(), a.doc.history().undoLabel().c_str());
    for (const Clip* c : cs) {
      std::printf(" [t%d %lld+%lld in %.4f out %.4f%s%s]", c->track, static_cast<long long>(c->start), static_cast<long long>(c->length), c->fadeIn,
                  c->fadeOut, c->muted ? " muted" : "", c->takeGroup != kNoId ? " lane" : "");
    }
    std::printf("\n");
  } else if (w == "window") {  // recmore window <page>: the Record window on that page
    recordcard::openWindow(a);
    recordcard::showPage(std::atoi(v.c_str()));
  } else if (w == "card") {  // the Recording card, as the record button opens it
    recordcard::open(a);
  } else if (w == "cardstate") {  // R14-01: what a click on a record button left: the card, Record, the transport
    std::printf("recmore frame %d: card %d armed %d playing %d\n", frame, recordcard::isOpen() ? 1 : 0, a.recordArmed ? 1 : 0,
                a.engine.playing() ? 1 : 0);
  } else if (w == "layout") {  // recmore layout <id>: as --switch-layout, at a point in the script (after waits)
    layoutui::choose(a, layout::fromId(v), layout::linkedFromSettings(a.settings));
  } else if (w == "latency" && v == "open") {
    openLatencyCheck(a);
  } else if (w == "latency" && v == "measure") {
    openLatencyCheck(a);
    S.channel = args.size() > 2 ? std::max(0, std::atoi(args[2].c_str())) : 0;
    beginMeasure(a, S);
  } else if (w == "latency" && v == "use") {
    useMeasured(a, S);
  } else if (w == "latency" && v == "clear") {
    clearMeasured(a);
  } else if (w == "latency" && v == "close") {
    S.open = false;
  } else if (w == "latency") {
    std::printf("recmore frame %d: latency phase %d ok %d frames %d agree %d of %d level %.3f measured %d auto %d record %d\n", frame, S.phase,
                S.sum.ok ? 1 : 0, S.sum.frames, S.sum.agree, S.sum.total, S.level, measuredRoundTrip(a), a.audioInputLatency, a.engine.recordLatency());
  } else {
    return false;
  }
  std::fflush(stdout);
  return true;
}

}  // namespace gl::recmore
