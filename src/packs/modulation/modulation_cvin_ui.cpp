// Gridline — "modulation" pack: Voltage In's editor, Voltage Out's interface section, the
// voltage calibration window and clock follow (K-170). See cv.h.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "../../core/I18n.h"
#include "../../core/Params.h"
#include "../edison/edison.h"
#include "cv.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_editors.h"
#include "modulation_skin.h"

namespace gl::packs::modulation {

namespace {

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

const CvInMonitor* monitorOf(const App& a, ObjectId ch) {
  auto rt = a.builder.channelRuntime(ch);
  return rt && rt->gen ? dynamic_cast<const CvInMonitor*>(rt->gen.get()) : nullptr;
}
CvOutTester* testerOf(const App& a, ObjectId ch) {
  auto rt = a.builder.channelRuntime(ch);
  return rt && rt->gen ? dynamic_cast<CvOutTester*>(rt->gen.get()) : nullptr;
}

std::string noteText(double pitch) {
  if (pitch < 0) return "--";
  const int k = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
  const int cents = static_cast<int>(std::lround((pitch - k) * 100.0));
  char buf[32];
  if (cents == 0) std::snprintf(buf, sizeof buf, "%s", w::noteName(k).c_str());
  else std::snprintf(buf, sizeof buf, "%s %+dc", w::noteName(k).c_str(), cents);
  return buf;
}

// Device names the calibration is stored under (this computer only).
std::string outputDeviceName(const App& a) {
  if (a.asio.active) return "asio:" + a.asio.driver;
  return a.audioDevice.empty() ? "default" : a.audioDevice;
}
std::string inputDeviceName(const App& a) {
  if (a.asio.active) return "asio:" + a.asio.driver;
  return a.audioInputDevice.empty() || a.audioInputDevice == "*" ? "default" : a.audioInputDevice;
}

// ------------------------------------------------------------------ calibration state
enum class CalMode { Coupling, RoundTrip, Tuner };
struct CalRun {
  bool open = false;
  ObjectId outCh = kNoId;  // Voltage Out channel
  int outSide = 0;
  ObjectId inCh = kNoId;   // Voltage In channel (reads the device inputs)
  int input = 0;           // device input, 0-based
  bool confirmed = false;  // the user says the output is not patched to speakers
  int tab = 0;
  // running
  bool running = false;
  CalMode mode = CalMode::Coupling;
  double t0 = 0;
  int step = -1;
  std::vector<double> steps;   // volts sent, per step
  std::vector<double> meas;    // measured per step (volts or Hz)
  std::vector<double> ts, vs;  // coupling samples
  double acc = 0;
  int accN = 0;
  size_t tapFrom = 0;
  std::string status;
  // results
  CouplingResult coupling;
  bool haveCoupling = false;
  LineFit round;
  bool haveRound = false;
  TunerFit tuner;
  bool haveTuner = false;
  CvCal oldOut;
};
CalRun& cal() {
  static CalRun r;
  return r;
}
bool storeLoaded = false;
std::string storePath(const App& a) { return a.configDir() + "/cv_calibration.json"; }

void saveStore(App& a, const CalStore& s) {
  setCalStore(s);
  if (!s.save(storePath(a))) a.notify(i18n::tr("Could not save the voltage calibration"), true);
  a.syncEngine();  // the plugins pick up the new numbers
}

void stopRun(App& a) {
  CalRun& r = cal();
  if (CvOutTester* t = testerOf(a, r.outCh)) t->clearTest();
  if (r.mode == CalMode::Tuner) a.engine.setInputTap(-1, false);
  r.running = false;
}

constexpr double kStepSec = 0.6;  // each round-trip / tuner step: settle, then measure the second half

void startRun(App& a, CalMode m) {
  CalRun& r = cal();
  r.running = true;
  r.mode = m;
  r.t0 = nowSec();
  r.step = -1;
  r.meas.clear();
  r.ts.clear();
  r.vs.clear();
  r.acc = 0;
  r.accN = 0;
  r.oldOut = CvCal{};
  const Channel* oc = a.doc.song().channel(r.outCh);
  const int range = oc ? static_cast<int>(std::lround(oc->plugin.params.size() > CvRange ? oc->plugin.params[CvRange] : 0)) : 0;
  const double lo = rangeMin(range), hi = rangeMax(range);
  r.steps.clear();
  if (m == CalMode::RoundTrip) {
    for (double v : {-2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0})
      if (v >= lo && v <= hi) r.steps.push_back(v);
  } else if (m == CalMode::Tuner) {
    const double vpo = oc && oc->plugin.params.size() > CvPitchScale && oc->plugin.params[CvPitchScale] >= 0.5f ? 1.2 : 1.0;
    for (int k = 0; k < 4; ++k)
      if (k * vpo <= hi) r.steps.push_back(k * vpo);
    r.oldOut = calStoreCopy().get(calKey(false, calOutputDevice(), cvOutputChannel(false, 0, r.outSide)));
    a.engine.setInputTap(r.input, false);
    r.tapFrom = a.engine.inputTap().written();
  } else {
    r.steps = {std::min(5.0, hi)};
  }
  r.status = i18n::tr("Running...");
}

// One frame of a running check (UI thread).
void tickRun(App& a) {
  CalRun& r = cal();
  if (!r.running) return;
  CvOutTester* out = testerOf(a, r.outCh);
  const CvInMonitor* in = monitorOf(a, r.inCh);
  if (!out || (r.mode != CalMode::Tuner && !in)) {
    stopRun(a);
    r.status = i18n::tr("Stopped: the Voltage Out or Voltage In channel is gone.");
    return;
  }
  const double t = nowSec() - r.t0;
  if (r.mode == CalMode::Coupling) {
    out->setTest(r.outSide, r.steps[0]);
    if (t > 0.25) {  // the step starts after a quarter second at 0 V
      r.ts.push_back(t - 0.25);
      r.vs.push_back(in->deviceVolts(r.input));
    } else {
      out->setTest(r.outSide, 0.0);
    }
    if (t > 2.75) {
      r.coupling = analyseCoupling(r.ts, r.vs);
      r.haveCoupling = true;
      stopRun(a);
      r.status = !r.coupling.ok ? i18n::tr("Nothing arrived at the input: check the cable and the input choice.")
                 : r.coupling.dcCoupled ? i18n::tr("Steady voltages pass: this path is DC-coupled.")
                                        : i18n::tr("The voltage fades away: this path blocks steady voltages (AC-coupled).");
    }
    return;
  }
  const int step = static_cast<int>(t / kStepSec);
  if (step != r.step) {  // close the previous step
    if (r.step >= 0 && r.step < static_cast<int>(r.steps.size())) {
      if (r.mode == CalMode::RoundTrip) {
        r.meas.push_back(r.accN > 0 ? r.acc / r.accN : 0.0);
      } else {
        // Tuner: the second half of the step's input audio, through the pitch detector.
        const auto& ring = a.engine.inputTap();
        const size_t end = ring.written();
        const size_t half = static_cast<size_t>(kStepSec * 0.5 * a.engine.sampleRate());
        size_t from = end > half ? end - half : 0;
        from = std::max(from, r.tapFrom);
        edison::Audio au = edison::makeAudio(a.engine.sampleRate(), 1, static_cast<int64_t>(end - from));
        std::vector<uint8_t> play(au.L.size());
        size_t got = ring.read(from, au.L.size(), au.L.data(), au.R.data(), play.data());
        au.resize(static_cast<int64_t>(got));
        const edison::PitchInfo pi = edison::detectPitch(au, edison::Range{0, au.frames()});
        r.meas.push_back(pi.confidence > 0.5 ? pi.hz : 0.0);
      }
    }
    r.step = step;
    r.acc = 0;
    r.accN = 0;
    if (step >= static_cast<int>(r.steps.size())) {
      std::vector<double> x(r.steps.begin(), r.steps.begin() + static_cast<long>(std::min(r.steps.size(), r.meas.size())));
      if (r.mode == CalMode::RoundTrip) {
        r.round = fitLine(x, r.meas);
        r.haveRound = r.round.ok;
        r.status = r.round.ok ? i18n::tr("Round trip measured. Apply stores it for this input.") : i18n::tr("No usable readings: check the cable.");
      } else {
        const Channel* oc = a.doc.song().channel(r.outCh);
        const double vpo = oc && oc->plugin.params.size() > CvPitchScale && oc->plugin.params[CvPitchScale] >= 0.5f ? 1.2 : 1.0;
        r.tuner = fitTuner(x, r.meas, vpo);
        r.haveTuner = r.tuner.ok;
        r.status = r.tuner.ok ? i18n::tr("Tuning measured. Apply stores it for this output.")
                              : i18n::tr("No clear pitch: route the oscillator's audio to the chosen input.");
      }
      stopRun(a);
      return;
    }
  }
  out->setTest(r.outSide, r.steps[static_cast<size_t>(step)]);
  if (r.mode == CalMode::RoundTrip && t - step * kStepSec > kStepSec * 0.5) {
    r.acc += in->deviceVolts(r.input);
    ++r.accN;
  }
}

// ------------------------------------------------------------------ calibration window
void channelCombo(App& a, const char* id, const char* type, ObjectId& sel, float width) {
  const Song& s = a.doc.song();
  const Channel* cur = s.channel(sel);
  if (cur && cur->plugin.type != type) cur = nullptr, sel = kNoId;
  if (!cur)
    for (const auto& c : s.channels)
      if (c.plugin.type == type) {
        sel = c.id;
        cur = &c;
        break;
      }
  if (ui::BeginCombo(id, cur ? cur->name.c_str() : i18n::tr("None in this project"), width)) {
    for (const auto& c : s.channels)
      if (c.plugin.type == type && ImGui::Selectable((c.name + "##" + std::to_string(c.id)).c_str(), c.id == sel)) sel = c.id;
    ImGui::EndCombo();
  }
}

void drawCalibration(App& a) {
  CalRun& r = cal();
  if (!r.open) {
    if (r.running) stopRun(a);
    return;
  }
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
  const std::string title = ui::windowTitle("Voltage calibration");
  if (!ImGui::Begin(title.c_str(), &r.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  const theme::Palette& P = theme::pal();
  ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     i18n::tr("Calibration needs a DC-coupled audio interface. It is stored on this computer for this "
                              "audio device, never in the project."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  ui::BeginSection(i18n::tr("Patch"));
  const float colW = 220;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(i18n::tr("Voltage Out"));
  ImGui::SameLine(120);
  channelCombo(a, "##calout", "gl.cvctl", r.outCh, colW);
  ImGui::SameLine();
  {
    const char* sides[2] = {i18n::tr("Left"), i18n::tr("Right")};
    ui::Segmented("##calside", sides, 2, &r.outSide, 70);
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(i18n::tr("Voltage In"));
  ImGui::SameLine(120);
  channelCombo(a, "##calin", "gl.cvin", r.inCh, colW);
  ImGui::SameLine();
  {
    char prev[16];
    std::snprintf(prev, sizeof prev, "In %d", r.input + 1);
    if (ui::BeginCombo("##calinput", prev, 140)) {
      for (int k = 0; k < kCvDeviceIns; ++k) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "In %d", k + 1);
        if (ImGui::Selectable(nm, k == r.input)) r.input = k;
      }
      ImGui::EndCombo();
    }
  }
  ImGui::Checkbox(i18n::tr("The output goes to CV gear or back into an input, never to speakers"), &r.confirmed);
  ui::EndSection();

  const char* tabs[3] = {i18n::tr("Coupling check"), i18n::tr("Round trip"), i18n::tr("Tuner (V/oct)")};
  if (!r.running) ui::TabStrip("##caltabs", tabs, 3, &r.tab);
  else ImGui::TextUnformatted(tabs[r.tab]);
  ImGui::Spacing();
  const bool haveOut = a.doc.song().channel(r.outCh) != nullptr, haveIn = a.doc.song().channel(r.inCh) != nullptr;
  const Channel* oc = a.doc.song().channel(r.outCh);
  const bool outOn = oc && oc->plugin.params.size() > CvEnable && oc->plugin.params[CvEnable] >= 0.5f;
  ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
  if (r.tab == 0)
    ImGui::TextUnformatted(i18n::tr("Patch the output into the input. The output holds 5 V for 2.5 s while Gridline watches the input: "
                                    "a voltage that fades means the output or the input blocks steady voltages."));
  else if (r.tab == 1)
    ImGui::TextUnformatted(i18n::tr("Patch the output into the input. Gridline steps from -2 V to +5 V and fits what comes back, so "
                                    "the input reads what Voltage Out sends."));
  else
    ImGui::TextUnformatted(i18n::tr("Patch the output into an oscillator's pitch input and the oscillator's audio into the chosen "
                                    "input. Gridline sends 0 to 3 octaves and tunes the output's volts per octave."));
  ImGui::PopTextWrapPos();
  const bool needIn = r.tab != 2;
  const bool can = haveOut && outOn && (haveIn || !needIn) && r.confirmed && !r.running;
  ImGui::BeginDisabled(!can);
  if (ui::TextButton(i18n::tr("Run"), false, 90)) startRun(a, r.tab == 0 ? CalMode::Coupling : r.tab == 1 ? CalMode::RoundTrip : CalMode::Tuner);
  ImGui::EndDisabled();
  if (r.running) {
    ImGui::SameLine();
    if (ui::TextButton(i18n::tr("Stop"), false, 90)) {
      stopRun(a);
      r.status = i18n::tr("Stopped.");
    }
  }
  if (!haveOut) ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Add a Voltage Out channel first."));
  else if (!outOn) ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Turn the Voltage Out on first."));
  if (needIn && !haveIn) ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Add a Voltage In channel first."));
  if (!r.status.empty()) ImGui::TextUnformatted(r.status.c_str());

  CalStore store = calStoreCopy();
  const std::string inKey = calKey(true, calInputDevice(), r.input);
  const std::string outKey = calKey(false, calOutputDevice(), cvOutputChannel(false, 0, r.outSide));
  if (r.tab == 0 && r.haveCoupling && r.coupling.ok) {
    ImGui::Text(i18n::tr("Start %.3f V, end %.3f V (%.0f %% held), noise %.1f mV"), r.coupling.startVolts, r.coupling.endVolts,
                r.coupling.held * 100.0, r.coupling.noise * 1000.0);
    if (!r.coupling.dcCoupled)
      ImGui::TextColored(theme::vec(P.danger), "%s", i18n::tr("Gates and held pitches will not work on this path."));
  }
  if (r.tab == 1 && r.haveRound) {
    ImGui::Text(i18n::tr("Gain %.4f, offset %+.1f mV, error %.2f mV"), r.round.gain, r.round.offset * 1000.0, r.round.rmsError * 1000.0);
    if (ui::TextButton(i18n::tr("Apply to this input"), false, 0)) {
      store.set(inKey, CvCal{r.round.gain, r.round.offset, true});
      saveStore(a, store);
      r.haveRound = false;
      r.status = i18n::tr("Input calibration stored.");
    }
  }
  if (r.tab == 2 && r.haveTuner) {
    ImGui::Text(i18n::tr("Measured %.4f octaves per volt"), r.tuner.octavesPerVolt);
    for (size_t k = 0; k < r.tuner.centsBefore.size(); ++k)
      ImGui::Text(i18n::tr("Step %d: %+.1f cents now, %+.1f cents after"), static_cast<int>(k), r.tuner.centsBefore[k], r.tuner.centsAfter[k]);
    if (ui::TextButton(i18n::tr("Apply to this output"), false, 0)) {
      CvCal c = r.oldOut.valid ? r.oldOut : CvCal{};
      c.gain *= r.tuner.gain;
      c.valid = true;
      store.set(outKey, c);
      saveStore(a, store);
      r.haveTuner = false;
      r.status = i18n::tr("Output calibration stored.");
    }
  }
  ImGui::Spacing();
  ui::BeginSection(i18n::tr("Stored on this computer"));
  int shown = 0;
  for (const auto& [k, c] : store.all()) {
    ImGui::PushID(k.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s   %.4f  %+.1f mV", k.c_str(), c.gain, c.offset * 1000.0);
    ImGui::SameLine();
    if (ui::TextButton(i18n::tr("Forget"), false, 0)) {
      CalStore s2 = store;
      s2.erase(k);
      saveStore(a, s2);
      ImGui::PopID();
      break;
    }
    ImGui::PopID();
    ++shown;
  }
  if (shown == 0 && store.all().empty()) ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("Nothing calibrated yet."));
  ui::EndSection();
  ImGui::End();
}

// ------------------------------------------------------------------ Play into
// Channels a Voltage In plays (listening on its MIDI output port).
std::vector<const Channel*> listeners(const Song& s, const Channel& me) {
  std::vector<const Channel*> v;
  if (me.midiOutPort < 0) return v;
  for (const auto& c : s.channels)
    if (c.id != me.id && c.midiInPort == me.midiOutPort) v.push_back(&c);
  return v;
}

void playInto(App& a, ObjectId me, ObjectId target) {
  a.doc.editSong(target == kNoId ? "stop playing Voltage In into a channel" : "play Voltage In into a channel", [&](Song& s) {
    Channel* c = s.channel(me);
    if (!c) return;
    if (c->midiOutPort < 0) {  // the lowest port nobody uses
      for (int p = 0; p < 256 && c->midiOutPort < 0; ++p) {
        bool used = false;
        for (const auto& o : s.channels) used = used || o.midiOutPort == p || o.midiInPort == p;
        if (!used) c->midiOutPort = p;
      }
      if (c->midiOutPort < 0) return;
    }
    for (auto& o : s.channels)
      if (o.id != me && o.midiInPort == c->midiOutPort) o.midiInPort = -1;
    if (Channel* t = s.channel(target)) t->midiInPort = c->midiOutPort;
  });
}

void playIntoBox(EditTarget& e, ImVec2 tl, ImVec2 size, const skin::Skin& k) {
  App& a = *e.a;
  const Song& s = a.doc.song();
  const Channel* me = s.channel(e.owner);
  if (!me) return;
  auto ls = listeners(s, *me);
  std::string cur = ls.empty() ? i18n::tr("Nothing") : ls.size() == 1 ? ls[0]->name : std::to_string(ls.size()) + " " + i18n::tr("channels");
  ImGui::PushID("playinto");
  if (sk::button("##pi", tl, size, k, cur.c_str())) ImGui::OpenPopup("##pip");
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("The channel Voltage In's notes play on (pitch and gate inputs)"));
  if (ImGui::BeginPopup("##pip")) {
    if (ImGui::MenuItem(i18n::tr("Nothing"), nullptr, ls.empty())) playInto(a, me->id, kNoId);
    ImGui::Separator();
    for (const auto& c : s.channels) {
      if (c.id == me->id || c.plugin.type == "gl.cvin") continue;
      const bool on = std::find(ls.begin(), ls.end(), &c) != ls.end();
      if (ImGui::MenuItem((c.name + "##" + std::to_string(c.id)).c_str(), nullptr, on)) playInto(a, me->id, c.id);
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

// Horizontal volt meter: a bar from 0 V within [lo, hi].
void voltBar(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, double v, double lo, double hi) {
  dl->AddRectFilled(p0, p1, k.lcdBg, 2);
  const float w = p1.x - p0.x;
  auto xOf = [&](double x) { return p0.x + 1 + static_cast<float>((std::clamp(x, lo, hi) - lo) / (hi - lo)) * (w - 2); };
  const float z = xOf(0.0), x = xOf(v);
  dl->AddRectFilled(ImVec2(std::min(z, x), p0.y + 2), ImVec2(std::max(z, x) + 1, p1.y - 2), k.fill, 1);
  dl->AddLine(ImVec2(z, p0.y + 1), ImVec2(z, p1.y - 1), theme::withAlpha(k.text, 0.35f));
}

}  // namespace

void openCalibration(App& a, ObjectId channel, bool input) {
  (void)a;
  CalRun& r = cal();
  r.open = true;
  if (input) r.inCh = channel;
  else r.outCh = channel;
}

// ------------------------------------------------------------------ Voltage In editor
// Original layout: the six inputs as columns (source, range, trims, live volts), then the note
// section and the clock section side by side.
void cvInEditor(EditTarget& e) {
  const skin::Skin& K = sk::voltage();
  constexpr float kW = 720, kH = 452;
  skin::Form F = sk::openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  App& a = *e.a;
  const CvInMonitor* mon = monitorOf(a, e.owner);
  const bool on = e.p(CiEnable, 1) >= 0.5f;
  const bool inputOpen = a.engine.inputChannels() > 0 && !(a.audioInputDevice.empty() && !a.asio.active);
  sk::check(e, CiEnable, F.p(14, 6), F.sz(52, 20), K, "ON");
  const char* status = !on ? i18n::tr("INPUT OFF") : !inputOpen ? i18n::tr("NO AUDIO INPUT: CHOOSE ONE IN SETTINGS > AUDIO") : i18n::tr("DC IN: DC-COUPLED INTERFACE ONLY");
  sk::text(dl, F, 72, 10, K, status, 7.0f, false, on && inputOpen ? skin::hex(0xFFC097) : K.textDim);
  sk::text(dl, F, 470, 10, K, i18n::tr("PLAY INTO"), 7.0f);
  playIntoBox(e, F.p(540, 6), F.sz(166, 20), K);
  if (!pui::titleWordmarkIn(dl, "gl.cvin", F.p(14, 34), F.p(300, 58), F.font(10.0f) * 1.4f, 1, K.text))
    skin::pill(dl, F.p(14, 34), F.p(300, 58), K, "VOLTAGE IN", F.font(10.0f));
  sk::text(dl, F, 318, 40, K, i18n::tr("FULL SCALE"), 7.0f);
  sk::digi(e, CiFullScale, F.p(392, 36), F.sz(66, 18), K);
  sk::check(e, CiUseCal, F.p(470, 36), F.sz(100, 18), K, i18n::tr("USE CAL"));
  const bool calClicked = sk::button("##cal", F.p(584, 34), F.sz(122, 22), K, i18n::tr("CALIBRATE"));
  if (calClicked) openCalibration(a, e.owner, true);
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Open the voltage calibration window (coupling check, round trip, tuner)"));

  // Inputs.
  sk::panelBox(dl, F, 6, 64, 708, 196, K, i18n::tr("INPUTS"));
  static const char* const kSlot[kCvInputs] = {"PITCH", "GATE", "CV A", "CV B", "CV C", "CV D"};
  for (int s = 0; s < kCvInputs; ++s) {
    ImGui::PushID(s);
    const float x = 14 + s * 116.0f;
    skin::pill(dl, F.p(x, 80), F.p(x + 108, 98), K, kSlot[s], F.font(8.0f));
    sk::menuBox(e, cvInParam(s, CfSource), F.p(x, 104), F.sz(108, 20), K);
    sk::menuBox(e, cvInParam(s, CfRange), F.p(x, 128), F.sz(108, 20), K);
    sk::text(dl, F, x, 156, K, i18n::tr("TRIM"), 7.0f);
    sk::digi(e, cvInParam(s, CfOffset), F.p(x + 40, 152), F.sz(68, 18), K);
    sk::text(dl, F, x, 180, K, i18n::tr("FINE"), 7.0f);
    sk::digi(e, cvInParam(s, CfScale), F.p(x + 40, 176), F.sz(68, 18), K);
    const int range = std::clamp(e.ip(cvInParam(s, CfRange)), 0, 2);
    const bool used = e.ip(cvInParam(s, CfSource)) > 0;
    const double v = mon && used ? mon->slotVolts(s) : 0.0;
    voltBar(dl, F.p(x, 202), F.p(x + 108, 214), K, v, rangeMin(range), rangeMax(range));
    char buf[24];
    if (used) std::snprintf(buf, sizeof buf, "%+.3f V", v);
    else std::snprintf(buf, sizeof buf, "%s", "--");
    skin::lcd(dl, F.p(x, 220), F.p(x + 108, 240), K, buf, F.font(9.0f));
    ImGui::PopID();
  }

  // Notes.
  sk::panelBox(dl, F, 6, 268, 440, 178, K, i18n::tr("NOTES"));
  sk::text(dl, F, 14, 292, K, i18n::tr("GATE"), 7.0f);
  sk::selector(e, CiGateMode, F.p(60, 288), F.sz(120, 18), K);
  sk::text(dl, F, 190, 292, K, i18n::tr("THRESH"), 7.0f);
  sk::digi(e, CiThreshold, F.p(240, 288), F.sz(64, 18), K);
  sk::text(dl, F, 314, 292, K, i18n::tr("HYST"), 7.0f);
  sk::digi(e, CiHysteresis, F.p(354, 288), F.sz(84, 18), K);
  sk::text(dl, F, 14, 318, K, i18n::tr("LENGTH"), 7.0f);
  sk::digi(e, CiTrigLen, F.p(60, 314), F.sz(70, 18), K);
  sk::selector(e, CiPitchScale, F.p(140, 314), F.sz(164, 18), K);
  sk::text(dl, F, 314, 318, K, i18n::tr("0 V"), 7.0f);
  sk::menuBox(e, CiZeroNote, F.p(354, 314), F.sz(84, 18), K);
  sk::text(dl, F, 14, 344, K, i18n::tr("SNAP"), 7.0f);
  {
    static const std::vector<std::string> q = {"OFF", "SEMI", "SCALE"};
    sk::selector(e, CiQuantise, F.p(60, 340), F.sz(150, 18), K, &q);
  }
  sk::menuBox(e, CiScale, F.p(218, 340), F.sz(156, 18), K);
  sk::menuBox(e, CiRoot, F.p(382, 340), F.sz(56, 18), K);
  sk::text(dl, F, 14, 370, K, i18n::tr("GLIDE"), 7.0f);
  sk::digi(e, CiGlide, F.p(60, 366), F.sz(70, 18), K);
  sk::check(e, CiRetrigger, F.p(140, 366), F.sz(298, 18), K, i18n::tr("NEW NOTE ON PITCH CHANGE"));
  sk::text(dl, F, 14, 396, K, i18n::tr("VEL"), 7.0f);
  sk::menuBox(e, CiVelSource, F.p(60, 392), F.sz(80, 18), K);
  sk::digi(e, CiVelocity, F.p(148, 392), F.sz(56, 18), K);
  {
    const bool g = mon && mon->gate();
    skin::ledDot(dl, F.p(222, 401), F.u(5), g, K);
    std::string nt = std::string(i18n::tr("NOTE")) + " " + noteText(mon ? mon->note() : -1.0);
    skin::lcd(dl, F.p(234, 391), F.p(438, 409), K, nt.c_str(), F.font(9.0f));
  }
  sk::text(dl, F, 14, 424, K, i18n::tr("NOTES PLAY ONE AUDIO BLOCK AFTER THE INPUT, TIMED TO THE SAMPLE"), 6.5f);

  // Clock.
  sk::panelBox(dl, F, 454, 268, 260, 178, K, i18n::tr("CLOCK"));
  sk::text(dl, F, 462, 292, K, i18n::tr("IN"), 7.0f);
  sk::menuBox(e, CiClockIn, F.p(512, 288), F.sz(90, 18), K);
  sk::menuBox(e, CiClockPpq, F.p(610, 288), F.sz(96, 18), K);
  sk::text(dl, F, 462, 318, K, i18n::tr("RUN"), 7.0f);
  sk::menuBox(e, CiRunIn, F.p(512, 314), F.sz(90, 18), K);
  sk::text(dl, F, 462, 344, K, i18n::tr("FOLLOW"), 7.0f);
  {
    static const std::vector<std::string> f = {"OFF", "TEMPO", "+RUN"};
    sk::selector(e, CiFollow, F.p(512, 340), F.sz(194, 18), K, &f);
  }
  {
    const double bpm = mon ? mon->clockBpm() : 0.0;
    char buf[32];
    if (bpm > 0) std::snprintf(buf, sizeof buf, "%.2f BPM", bpm);
    else std::snprintf(buf, sizeof buf, "%s", i18n::tr("NO CLOCK"));
    skin::lcd(dl, F.p(462, 365), F.p(602, 383), K, buf, F.font(9.0f));
    const int run = mon ? mon->run() : -1;
    skin::ledDot(dl, F.p(620, 375), F.u(5), run == 1, K);
    sk::text(dl, F, 632, 370, K, i18n::tr("RUNNING"), 7.0f);
  }
  sk::text(dl, F, 462, 396, K, i18n::tr("SMOOTH"), 7.0f);
  sk::digi(e, CiSmooth, F.p(512, 392), F.sz(90, 18), K);
  sk::text(dl, F, 462, 424, K, i18n::tr("CV TO PARAMETERS: RIGHT-CLICK A KNOB"), 6.5f);
  F.end(kW, kH);
}

// ------------------------------------------------------------------ Voltage Out interface section
// Below the reference columns: output mode, pair, full scale, calibration, fine trims and the value
// sources (form units of the Voltage Out form, from y0).
void cvOutInterface(EditTarget& e, const skin::Form& F, const skin::Skin& K, float y0) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  App& a = *e.a;
  sk::panelBox(dl, F, 8, y0, 286, 156, K, i18n::tr("INTERFACE"));
  sk::selector(e, CvOutMode, F.p(16, y0 + 20), F.sz(270, 18), K, nullptr);
  const bool direct = e.ip(CvOutMode) == CvmDirect;
  if (direct) {
    if (a.asio.active) {
      sk::menuBox(e, CvOutPair, F.p(16, y0 + 44), F.sz(130, 18), K);
      char buf[48];
      std::snprintf(buf, sizeof buf, "%d %s", a.asio.extraOuts, i18n::tr("PAIRS OPEN"));
      sk::text(dl, F, 156, y0 + 48, K, buf, 7.0f);
    } else {
      sk::text(dl, F, 16, y0 + 48, K, i18n::tr("DIRECT NEEDS THE ASIO DRIVER: VIA THE MIXER HERE"), 6.5f, false, skin::hex(0xFFC097));
    }
  } else {
    sk::text(dl, F, 16, y0 + 48, K, i18n::tr("KEEP THE CHANNEL AND MIXER AT UNITY"), 6.5f);
  }
  sk::text(dl, F, 16, y0 + 73.5f, K, i18n::tr("FULL SCALE"), 7.0f);
  sk::digi(e, CvFullScale, F.p(90, y0 + 68), F.sz(64, 18), K);
  sk::check(e, CvUseCal, F.p(162, y0 + 68), F.sz(124, 18), K, i18n::tr("USE CAL"));
  for (int side = 0; side < 2; ++side) {
    ImGui::PushID(side + 40);
    const float x = side ? 152 : 16;
    sk::text(dl, F, x, y0 + 97.5f, K, side ? i18n::tr("R TRIM") : i18n::tr("L TRIM"), 7.0f);
    sk::digi(e, side ? CvRightTrim : CvLeftTrim, F.p(x + 50, y0 + 92), F.sz(84, 18), K);
    sk::text(dl, F, x, y0 + 121.5f, K, side ? i18n::tr("R FINE") : i18n::tr("L FINE"), 7.0f);
    sk::digi(e, side ? CvRightFine : CvLeftFine, F.p(x + 50, y0 + 116), F.sz(84, 18), K);
    ImGui::PopID();
  }
  if (sk::button("##cal", F.p(16, y0 + 138), F.sz(124, 14), K, i18n::tr("CALIBRATE"))) openCalibration(a, e.owner, false);
  if (ImGui::IsItemHovered()) a.setHint(i18n::tr("Open the voltage calibration window (coupling check, round trip, tuner)"));
  // Value sources: the side's Value parameter (link a controller to it).
  for (int side = 0; side < 2; ++side) {
    const int src = e.ip(side ? CvRightSrc : CvLeftSrc);
    if (src != CvsValue) continue;
    ImGui::PushID(side + 50);
    sk::text(dl, F, side ? 222 : 150, y0 + 140, K, side ? "R" : "L", 7.0f);
    sk::digi(e, side ? CvRightValue : CvLeftValue, F.p(side ? 232 : 160, y0 + 138), F.sz(54, 14), K);
    ImGui::PopID();
  }
}

// ------------------------------------------------------------------ per frame
void tickVoltage(App& a) {
  if (!storeLoaded) {
    storeLoaded = true;
    CalStore s;
    s.load(storePath(a));
    setCalStore(s);
    if (std::getenv("GRIDLINE_CV_CALIBRATION")) cal().open = true;  // screenshots / app tests
  }
  const std::string inDev = inputDeviceName(a), outDev = outputDeviceName(a);
  if (inDev != calInputDevice() || outDev != calOutputDevice()) {
    setCalDevices(inDev, outDev);
    a.syncEngine();
  }
  tickRun(a);
  drawCalibration(a);
  // Clock follow: the first Voltage In with follow on drives the tempo (and run / stop).
  static std::map<ObjectId, int> lastRun;
  static double lastTempoSet = -1;
  for (const auto& c : a.doc.song().channels) {
    if (c.plugin.type != "gl.cvin") continue;
    const auto& p = c.plugin.params;
    const int follow = p.size() > CiFollow ? static_cast<int>(std::lround(p[CiFollow])) : 0;
    if (follow == CfwOff || (p.size() > CiEnable && p[CiEnable] < 0.5f)) continue;
    const CvInMonitor* m = monitorOf(a, c.id);
    if (!m) continue;
    const double bpm = m->clockBpm(), now = nowSec();
    if (bpm >= 10 && bpm <= 522 && std::fabs(bpm - a.doc.song().tempo) > 0.05 && now - lastTempoSet > 0.25) {
      a.doc.setParam(songParam(SongParam::Tempo), std::round(bpm * 100) / 100, true);
      lastTempoSet = now;
    }
    if (follow == CfwTempoRun) {
      const int run = m->run();
      auto it = lastRun.find(c.id);
      const int was = it == lastRun.end() ? run : it->second;
      if (run == 1 && was == 0) {
        a.engine.setPosition(0);
        a.engine.play();
      } else if (run == 0 && was == 1) {
        a.engine.pause();
      }
      lastRun[c.id] = run;
    }
    break;
  }
}

}  // namespace gl::packs::modulation
