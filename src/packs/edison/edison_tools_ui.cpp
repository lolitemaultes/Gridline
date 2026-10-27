// Gridline — "edison" pack: tool dialogs (stretch / pitch, drawn EQ, reverb,
// blur, clean-up, gate, declick, resample, levels, drawn envelope, beat
// slicing, tuning, silence, spectral brush, marker names). Each one runs its
// operation through runEdit, so it lands on the worker thread and becomes one
// undo step.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "../../app/Theme.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace {

const char* toolTitle(Tool t) {
  switch (t) {
    case Tool::Stretch: return "Time stretch / pitch shift";
    case Tool::Equalize: return "Equalize (drawn curve)";
    case Tool::Reverb: return "Convolution reverb";
    case Tool::Blur: return "Blur";
    case Tool::Denoise: return "Clean up (noise reduction)";
    case Tool::Gate: return "Noise gate";
    case Tool::Declick: return "Declick";
    case Tool::Resample: return "Resample";
    case Tool::Amplify: return "Amplify";
    case Tool::Normalize: return "Normalize";
    case Tool::Envelope: return "Amplitude envelope";
    case Tool::Slice: return "Beat detection";
    case Tool::Tune: return "Tune to note";
    case Tool::Silence: return "Insert silence";
    case Tool::Spectral: return "Spectral brush";
    case Tool::Rename: return "Name";
    case Tool::Fade: return "Fade";
    case Tool::Script: return "Script";
    default: return "Tool";
  }
}

bool acceptRow(bool enabled = true) {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::BeginDisabled(!enabled);
  bool ok = ImGui::Button(i18n::tr("Accept"), ImVec2(110, 0));
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(i18n::tr("Cancel"), ImVec2(110, 0))) ImGui::CloseCurrentPopup();
  if (ok) ImGui::CloseCurrentPopup();
  return ok;
}

// Point editor in a graph: x / y mapping functions, drag points, double-click
// adds, right-click removes (first and last stay when `pinEnds`).
template <class P, class ToScreen, class FromScreen>
void pointGraph(const char* id, std::vector<P>& pts, ImVec2 size, ToScreen toS, FromScreen fromS, bool pinEnds, int& drag,
                const std::function<void(ImDrawList*, ImVec2, ImVec2)>& grid) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
  ImGui::InvisibleButton(id, size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  dl->AddRectFilled(p0, p1, C(col::WaveBg), 4);
  grid(dl, p0, p1);
  const ImVec2 m = ImGui::GetIO().MousePos;
  int hot = -1;
  for (size_t i = 0; i < pts.size(); ++i) {
    ImVec2 q = toS(pts[i], p0, p1);
    if (std::fabs(q.x - m.x) < 7 && std::fabs(q.y - m.y) < 7) hot = static_cast<int>(i);
  }
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drag = hot;
  if (ImGui::IsItemActive() && drag >= 0 && drag < static_cast<int>(pts.size())) {
    P np = fromS(ImVec2(std::clamp(m.x, p0.x, p1.x), std::clamp(m.y, p0.y, p1.y)), p0, p1);
    if (pinEnds && (drag == 0 || drag + 1 == static_cast<int>(pts.size()))) np.first = pts[static_cast<size_t>(drag)].first;
    pts[static_cast<size_t>(drag)] = np;
  }
  if (ImGui::IsItemDeactivated()) drag = -1;
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hot < 0)
    pts.push_back(fromS(m, p0, p1));
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hot >= 0 && pts.size() > 2 && !(pinEnds && (hot == 0 || hot + 1 == static_cast<int>(pts.size()))))
    pts.erase(pts.begin() + hot);
  std::stable_sort(pts.begin(), pts.end(), [](const P& a, const P& b) { return a.first < b.first; });
  // Curve.
  std::vector<ImVec2> line;
  for (const auto& q : pts) line.push_back(toS(q, p0, p1));
  if (line.size() > 1) dl->AddPolyline(line.data(), static_cast<int>(line.size()), C(col::Wave), 0, 2.f);
  for (size_t i = 0; i < line.size(); ++i) dl->AddCircleFilled(line[i], static_cast<int>(i) == hot ? 6.f : 4.5f, static_cast<int>(i) == hot ? C(col::WaveSel) : C(col::Wave));
  dl->AddRect(p0, p1, C(col::BtnLo), 4);
}

struct EqPt {
  double first, second;  // hz, db
};

}  // namespace

void openTool(Session& s, Tool t) {
  s.tool = t;
  s.toolOpenReq = true;
}

void drawToolDialogs(App& a, Session& s) {
  if (s.tool == Tool::None) return;
  const char* title = toolTitle(s.tool);
  char id[64];
  std::snprintf(id, sizeof id, "%s###edtool", title);
  if (s.toolOpenReq) {
    ImGui::OpenPopup(id);
    s.toolOpenReq = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!ImGui::IsPopupOpen(id)) s.tool = Tool::None;
    return;
  }
  const Range tr = target(s);
  const bool sel = !s.sel.empty();
  auto scopeLine = [&]() {
    ImGui::TextDisabled(i18n::tr("Applies to: %s (%s)"), sel ? "selection" : "whole file", timeText(a, s, tr.len()).c_str());
  };
  static int drag = -1;
  switch (s.tool) {
    case Tool::Stretch: {
      scopeLine();
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Time"), &s.tStretch, 0.25f, 4.f, "x %.3f", ImGuiSliderFlags_Logarithmic);
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Pitch"), &s.tSemis, -24.f, 24.f, "%.2f semitones");
      const char* modes[] = {i18n::tr(stretchName(Stretch::Elastic)), i18n::tr(stretchName(Stretch::Transient)), i18n::tr(stretchName(Stretch::Tape)),
                             i18n::tr(stretchName(Stretch::Mono)), i18n::tr(stretchName(Stretch::Formant)), i18n::tr(stretchName(Stretch::Voice)),
                             i18n::tr(stretchName(Stretch::Beats))};
      ImGui::SetNextItemWidth(260);
      ImGui::Combo(i18n::tr("Method"), &s.tMode, modes, static_cast<int>(Stretch::Count));
      const char* q[] = {"Fast", "Normal", "Best"};
      ImGui::SetNextItemWidth(260);
      ImGui::Combo(i18n::tr("Quality"), &s.tQuality, q, 3);
      const double newLen = s.tMode == 2 ? tr.len() / std::pow(2.0, s.tSemis / 12.0) : tr.len() * static_cast<double>(s.tStretch);
      ImGui::TextDisabled(i18n::tr("New length %s%s"), timeText(a, s, static_cast<int64_t>(newLen)).c_str(), s.tMode == 2 ? " (tape: pitch sets speed)" : "");
      if (ImGui::SmallButton(i18n::tr("Fit to song bar"))) {
        const double bar = s.rate() * 60.0 / std::max(10.0, a.doc.song().tempo) * a.doc.song().timeSigNum;
        const double bars = std::max(1.0, std::round(tr.len() / bar));
        s.tStretch = static_cast<float>(bars * bar / std::max<double>(1, tr.len()));
      }
      ImGui::SameLine();
      if (ImGui::SmallButton(i18n::tr("Reset"))) s.tStretch = 1, s.tSemis = 0;
      if (acceptRow()) {
        const double f = s.tStretch, st = s.tSemis;
        const Stretch m = static_cast<Stretch>(s.tMode);
        const int qq = s.tQuality;
        runEdit(a, s, s.tMode == 2 ? "Varispeed" : "Time stretch / pitch shift", [tr, f, st, m, qq](const Content& c) { return stretch(c, tr, f, st, m, qq); });
      }
      break;
    }
    case Tool::Equalize: {
      scopeLine();
      std::vector<EqPt> pts;
      for (auto& p : s.eq) pts.push_back({p.hz, p.db});
      auto toS = [](const EqPt& p, ImVec2 p0, ImVec2 p1) {
        float x = p0.x + static_cast<float>(std::log(p.first / 20.0) / std::log(1000.0)) * (p1.x - p0.x);
        float y = (p0.y + p1.y) * 0.5f - static_cast<float>(p.second / 24.0) * (p1.y - p0.y) * 0.5f;
        return ImVec2(x, y);
      };
      auto fromS = [](ImVec2 m, ImVec2 p0, ImVec2 p1) {
        double hz = 20.0 * std::pow(1000.0, (m.x - p0.x) / (p1.x - p0.x));
        double db = -((m.y - (p0.y + p1.y) * 0.5) / ((p1.y - p0.y) * 0.5)) * 24.0;
        return EqPt{hz, std::round(db * 10) / 10};
      };
      pointGraph("eqgraph", pts, ImVec2(480, 190), toS, fromS, false, drag, [](ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
        for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
          float x = p0.x + static_cast<float>(std::log(hz / 20.0) / std::log(1000.0)) * (p1.x - p0.x);
          dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), C(col::WaveGrid));
          char b[12];
          std::snprintf(b, sizeof b, hz >= 1000 ? "%.0fk" : "%.0f", hz >= 1000 ? hz / 1000 : hz);
          dl->AddText(ImVec2(x + 2, p1.y - 15), C(col::TextDim), b);
        }
        for (int db : {-12, 0, 12}) {
          float y = (p0.y + p1.y) * 0.5f - db / 24.f * (p1.y - p0.y) * 0.5f;
          dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), db == 0 ? C(col::WaveGridHi) : C(col::WaveGrid));
        }
      });
      s.eq.clear();
      for (auto& p : pts) s.eq.push_back({p.first, p.second});
      ImGui::TextDisabled("%s", i18n::tr("Drag points; double-click adds, right-click removes. +-24 dB, 20 Hz - 20 kHz."));
      if (ImGui::SmallButton(i18n::tr("Flat"))) s.eq = {{30, 0}, {120, 0}, {500, 0}, {2000, 0}, {8000, 0}, {16000, 0}};
      ImGui::SameLine();
      if (ImGui::SmallButton(i18n::tr("Low cut"))) s.eq = {{20, -24}, {60, -24}, {120, 0}, {16000, 0}};
      ImGui::SameLine();
      if (ImGui::SmallButton(i18n::tr("Telephone"))) s.eq = {{20, -24}, {300, -24}, {500, 0}, {2500, 3}, {3400, -24}, {20000, -24}};
      if (acceptRow()) {
        std::vector<EqPoint> e = s.eq;
        runEdit(a, s, "Equalize", [tr, e](const Content& c) { return equalize(c, tr, e); });
      }
      break;
    }
    case Tool::Reverb: {
      scopeLine();
      float d = static_cast<float>(s.rv.decay), pd = static_cast<float>(s.rv.predelayMs), dm = static_cast<float>(s.rv.damping),
            w = static_cast<float>(s.rv.width), wet = static_cast<float>(s.rv.wet), dry = static_cast<float>(s.rv.dry);
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Decay"), &d, 0.1f, 10.f, "%.2f s", ImGuiSliderFlags_Logarithmic);
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Pre-delay"), &pd, 0.f, 200.f, "%.0f ms");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Damping"), &dm, 0.f, 1.f, "%.2f");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Width"), &w, 0.f, 1.f, "%.2f");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Wet"), &wet, 0.f, 1.f, "%.2f");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Dry"), &dry, 0.f, 1.f, "%.2f");
      s.rv.decay = d, s.rv.predelayMs = pd, s.rv.damping = dm, s.rv.width = w, s.rv.wet = wet, s.rv.dry = dry;
      ImGui::TextDisabled("%s", i18n::tr("The tail rings into the following audio (and past the end)."));
      if (acceptRow()) {
        ReverbSettings rs = s.rv;
        runEdit(a, s, "Reverb", [tr, rs](const Content& c) { return reverb(c, tr, rs); });
      }
      break;
    }
    case Tool::Blur:
      scopeLine();
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Amount"), &s.blurMs, 2.f, 500.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Mix"), &s.blurMix, 0.f, 1.f, "%.2f");
      if (acceptRow()) {
        const double ms = s.blurMs, mix = s.blurMix;
        runEdit(a, s, "Blur", [tr, ms, mix](const Content& c) { return blur(c, tr, ms, mix); });
      }
      break;
    case Tool::Denoise:
      scopeLine();
      if (ImGui::Button(s.noise.valid() ? i18n::tr("Take profile again (from selection)") : i18n::tr("Take noise profile from the selection"))) {
        s.noise = acquireNoise(s.cur().a(), tr);
        if (!s.noise.valid()) say(s, "Select at least 50 ms of noise first");
      }
      ImGui::TextDisabled(s.noise.valid() ? "Noise profile ready." : "No profile yet: select a noise-only part first.");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Reduction"), &s.dnReduce, 3.f, 60.f, "%.0f dB");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Sensitivity"), &s.dnSens, 0.f, 1.f, "%.2f");
      if (acceptRow(s.noise.valid())) {
        NoiseProfile p = s.noise;
        const double r = s.dnReduce, se = s.dnSens;
        runEdit(a, s, "Clean up", [tr, p, r, se](const Content& c) { return denoise(c, tr, p, r, se); });
      }
      break;
    case Tool::Gate:
      scopeLine();
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Threshold"), &s.gThr, -90.f, 0.f, "%.1f dB");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Attack"), &s.gAtt, 0.f, 50.f, "%.1f ms");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Hold"), &s.gHold, 0.f, 500.f, "%.0f ms");
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Release"), &s.gRel, 1.f, 1000.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Floor"), &s.gFloor, -90.f, 0.f, s.gFloor <= -90 ? "silence" : "%.0f dB");
      if (acceptRow()) {
        const double t = s.gThr, at = s.gAtt, h = s.gHold, rl = s.gRel, fl = s.gFloor;
        runEdit(a, s, "Noise gate", [tr, t, at, h, rl, fl](const Content& c) { return gate(c, tr, t, at, h, rl, fl); });
      }
      break;
    case Tool::Declick:
      scopeLine();
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Sensitivity"), &s.dcSens, 0.f, 1.f, "%.2f");
      if (acceptRow()) {
        const double se = s.dcSens;
        runEdit(a, s, "Declick", [tr, se](const Content& c) { return declick(c, tr, se); });
      }
      break;
    case Tool::Resample: {
      const int rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
      ImGui::Text(i18n::tr("Current rate: %.0f Hz"), s.rate());
      int idx = 0;
      for (int i = 0; i < 11; ++i)
        if (rates[i] == s.rsRate) idx = i;
      const char* names[] = {"8000", "11025", "16000", "22050", "32000", "44100", "48000", "88200", "96000", "176400", "192000"};
      ImGui::SetNextItemWidth(200);
      if (ImGui::Combo(i18n::tr("New rate (Hz)"), &idx, names, 11)) s.rsRate = rates[idx];
      const char* q[] = {"Fast (16 taps)", "Normal (64 taps)", "Best (192 taps)"};
      ImGui::SetNextItemWidth(200);
      ImGui::Combo(i18n::tr("Quality"), &s.rsQuality, q, 3);
      ImGui::TextDisabled("%s", i18n::tr("Converts the whole file (markers move with it)."));
      if (acceptRow(std::lround(s.rate()) != s.rsRate)) {
        const double nr = s.rsRate;
        const int qq = s.rsQuality;
        runEdit(a, s, "Resample", [nr, qq](const Content& c) { return resample(c, nr, qq); });
      }
      break;
    }
    case Tool::Amplify:
      scopeLine();
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Gain"), &s.ampDb, -48.f, 24.f, "%+.1f dB");
      if (acceptRow()) {
        const double g = s.ampDb;
        runEdit(a, s, "Amplify", [tr, g](const Content& c) { return gain(c, tr, g); });
      }
      break;
    case Tool::Normalize:
      scopeLine();
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Peak level"), &s.normDb, -24.f, 0.f, "%.1f dBFS");
      if (acceptRow()) {
        const double g = s.normDb;
        runEdit(a, s, "Normalize", [tr, g](const Content& c) { return normalize(c, tr, g); });
      }
      break;
    case Tool::Envelope: {
      scopeLine();
      auto toS = [](const std::pair<double, double>& p, ImVec2 p0, ImVec2 p1) {
        return ImVec2(p0.x + static_cast<float>(p.first) * (p1.x - p0.x), p1.y - static_cast<float>(p.second / 2.0) * (p1.y - p0.y));
      };
      auto fromS = [](ImVec2 m, ImVec2 p0, ImVec2 p1) {
        return std::make_pair(static_cast<double>((m.x - p0.x) / (p1.x - p0.x)), static_cast<double>((p1.y - m.y) / (p1.y - p0.y) * 2.0));
      };
      pointGraph("envgraph", s.env, ImVec2(480, 170), toS, fromS, true, drag, [](ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
        float y1 = p1.y - 0.5f * (p1.y - p0.y);
        dl->AddLine(ImVec2(p0.x, y1), ImVec2(p1.x, y1), C(col::WaveGridHi));
        dl->AddText(ImVec2(p0.x + 3, y1 - 15), C(col::TextDim), "0 dB");
      });
      ImGui::TextDisabled("%s", i18n::tr("Gain across the range (0 to +6 dB). Double-click adds a point, right-click removes."));
      if (ImGui::SmallButton(i18n::tr("Reset"))) s.env = {{0, 1}, {1, 1}};
      ImGui::SameLine();
      if (ImGui::SmallButton(i18n::tr("Swell"))) s.env = {{0, 0}, {0.7, 1}, {1, 1}};
      ImGui::SameLine();
      if (ImGui::SmallButton(i18n::tr("Duck"))) s.env = {{0, 1}, {0.1, 0.3}, {0.6, 0.3}, {1, 1}};
      if (acceptRow()) {
        auto e = s.env;
        runEdit(a, s, "Envelope", [tr, e](const Content& c) { return applyEnvelope(c, tr, e); });
      }
      break;
    }
    case Tool::Slice: {
      scopeLine();
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Sensitivity"), &s.sliceSens, 0.f, 1.f, "%.2f");
      ImGui::TextDisabled("%s", i18n::tr("Places a marker at every detected hit (replaces the markers)."));
      if (acceptRow()) detectSlices(a, s, s.sliceSens);
      break;
    }
    case Tool::Tune: {
      scopeLine();
      static PitchInfo found;
      static uint64_t foundKey = ~0ull;
      const uint64_t k = s.hist.serial() * 31 + static_cast<uint64_t>(tr.a) * 7 + static_cast<uint64_t>(tr.b);
      if (k != foundKey) {
        Range pr{tr.a, std::min(tr.b, tr.a + static_cast<int64_t>(s.rate() * 2))};
        found = detectPitch(s.cur().a(), pr);
        foundKey = k;
      }
      if (found.hz > 0)
        ImGui::Text(i18n::tr("Detected: %.2f Hz = %s %+.0f cents (confidence %.0f%%)"), found.hz, noteName(found.note).c_str(), found.cents, found.confidence * 100);
      else
        ImGui::TextDisabled("%s", i18n::tr("No clear pitch found."));
      std::vector<std::string> names = {"Nearest note"};
      for (int n = 24; n <= 96; ++n) names.push_back(noteName(n));
      int idx = s.tuneNote < 0 ? 0 : s.tuneNote - 23;
      ImGui::SetNextItemWidth(200);
      if (ImGui::BeginCombo("Target", names[static_cast<size_t>(std::clamp(idx, 0, static_cast<int>(names.size()) - 1))].c_str())) {
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
          if (ImGui::Selectable(names[static_cast<size_t>(i)].c_str(), i == idx)) s.tuneNote = i == 0 ? -1 : i + 23;
        ImGui::EndCombo();
      }
      if (acceptRow(found.hz > 0)) {
        const int note = s.tuneNote;
        runEdit(a, s, "Tune to note", [tr, note](const Content& c) { return tuneToNote(c, tr, note); });
      }
      break;
    }
    case Tool::Silence:
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Length"), &s.silenceSec, 0.01f, 30.f, "%.2f s", ImGuiSliderFlags_Logarithmic);
      ImGui::TextDisabled(i18n::tr("Inserted at the cursor (%s)."), timeText(a, s, s.cursor).c_str());
      if (acceptRow()) {
        const int64_t at = s.cursor;
        const double sec = s.silenceSec;
        runEdit(a, s, "Insert silence", [at, sec](const Content& c) {
          Content base = c;
          if (c.frames() == 0 && !c.audio) base = makeContent(makeAudio(44100, 2, 0));
          return insertSilence(base, at, static_cast<int64_t>(sec * base.rate()));
        });
      }
      break;
    case Tool::Spectral:
      ImGui::Text(i18n::tr("%zu painted areas"), s.brush.size());
      ImGui::SetNextItemWidth(260);
      ImGui::SliderFloat(i18n::tr("Gain"), &s.spectralDb, -60.f, 12.f, "%+.0f dB");
      ImGui::TextDisabled("%s", i18n::tr("Attenuate (negative) or boost the painted time / frequency areas."));
      if (acceptRow(!s.brush.empty())) {
        auto rects = s.brush;
        const double g = s.spectralDb;
        runEdit(a, s, "Spectral edit", [rects, g](const Content& c) { return spectralGain(c, rects, g); });
        s.brush.clear();
      }
      break;
    case Tool::Rename: {
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(260);
      const bool enter = ImGui::InputText("##name", s.renameBuf, sizeof s.renameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
      if (acceptRow() || enter) {
        if (enter) ImGui::CloseCurrentPopup();
        if (s.renameMarker >= 0 && static_cast<size_t>(s.renameMarker) < s.cur().markers.size())
          applyNow(a, s, "Rename marker", renameMarker(s.cur(), static_cast<size_t>(s.renameMarker), s.renameBuf));
        else if (s.renameRegion >= 0 && static_cast<size_t>(s.renameRegion) < s.cur().regions.size())
          applyNow(a, s, "Rename region", renameRegion(s.cur(), static_cast<size_t>(s.renameRegion), s.renameBuf));
      }
      break;
    }
    case Tool::Script:
      drawScriptDialog(a, s);
      break;
    default:
      if (acceptRow()) {
      }
      break;
  }
  ImGui::EndPopup();
}

}  // namespace gl::packs::edison
