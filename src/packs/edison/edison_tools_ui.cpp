// Gridline — "edison" pack: tool dialogs (stretch / pitch, drawn EQ, reverb,
// blur, clean-up, gate, declick, resample, levels, drawn envelope, beat
// slicing, tuning, silence, spectral brush, marker names). Each one runs its
// operation through runEdit, so it lands on the worker thread and becomes one
// undo step.
//
// J-207: every dialog has the same frame: what it applies to, one titled
// section with the labels in one column and the controls in the next (one
// width), preset buttons of one width, and the house footer (Cancel, Accept)
// on the right.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>
#include "../../core/I18n.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../engine/TimeStretch.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace {

constexpr float kDlgW = 420;      // section width of the slider dialogs
constexpr float kDlgWideW = 540;  // graph dialogs
constexpr float kBtnW = 96;       // footer and preset buttons (the house dialogs' button width)

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

// The label column of a dialog: wide enough for its widest label.
float column(std::initializer_list<const char*> labels) {
  std::vector<const char*> t;
  for (const char* l : labels) t.push_back(i18n::tr(l));
  return ui::FormColumn(t.data(), static_cast<int>(t.size()), 72.0f, 150.0f, theme::kS3);
}

// A titled section of the given width. The auto-sizing dialog only widens to
// what it holds, so a zero-height spacer of that width comes first.
void BeginSection(const char* caption, float width) {  // (name kept for the i18n extractor)
  ImGui::Dummy(ImVec2(width, 0));
  ui::BeginSection(caption, width);
}

// One form row: the label (dim, on the control's text line), then the next item
// starts on the column and fills the section's width.
void row(const char* label, float col) {
  const float x0 = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
  ImGui::TextUnformatted(i18n::tr(label));
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::SetCursorPosX(x0 + col);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
}

// A dialog slider in Gridline's look: a thin recessed rail with a raised handle,
// and the value in its own recessed field to the right, so the number is never
// under the handle. Drag the rail (Shift: 8x finer), wheel steps, double-click
// the value to type one. Same height as the dialog's combos (the frame height).
constexpr float kValueW = 96;  // the value field (the house button width)
bool slider(const char* label, float col, float* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags = 0) {
  row(label, col);
  ImGui::PushID(label);
  const bool logS = (flags & ImGuiSliderFlags_Logarithmic) != 0 && lo > 0;
  auto toN = [&](float x) {
    x = std::clamp(x, lo, hi);
    return logS ? std::log(x / lo) / std::log(hi / lo) : (x - lo) / (hi - lo);
  };
  auto fromN = [&](float n) {
    n = std::clamp(n, 0.0f, 1.0f);
    return logS ? lo * std::pow(hi / lo, n) : lo + (hi - lo) * n;
  };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = ImGui::GetFrameHeight(), w = ImGui::GetContentRegionAvail().x;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float valW = std::min(kValueW, std::floor(w * 0.4f)), gap = theme::kS2;
  const float trackW = std::max(40.0f, w - valW - gap);
  const float handleW = 10, inset = handleW * 0.5f;  // the handle's centre runs inset .. trackW - inset
  bool changed = false;

  // Rail.
  ImGui::InvisibleButton("##track", ImVec2(trackW, h));
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  static float grabDx = 0;  // pointer offset from the handle at the press (one active slider at a time)
  const float x0 = p0.x + inset, len = trackW - 2 * inset;
  float n = toN(*v);
  if (ImGui::IsItemActivated()) {
    const float hx = x0 + n * len;
    const float mx = ImGui::GetIO().MousePos.x;
    grabDx = std::fabs(mx - hx) <= handleW ? mx - hx : 0.0f;  // on the handle: no jump
  }
  if (act) {
    const ImGuiIO& io = ImGui::GetIO();
    float nn = io.KeyShift ? n + io.MouseDelta.x / len / 8.0f : (io.MousePos.x - grabDx - x0) / len;
    nn = std::clamp(nn, 0.0f, 1.0f);
    if (nn != n) {
      *v = fromN(nn), n = nn;
      changed = true;
    }
  }
  if (hov && ImGui::GetIO().MouseWheel != 0) {
    *v = fromN(n + ImGui::GetIO().MouseWheel * (ImGui::GetIO().KeyShift ? 0.002f : 0.02f));
    n = toN(*v);
    changed = true;
  }
  const float cy = std::round(p0.y + h * 0.5f), rh = 4;
  const ImVec2 r0(p0.x, cy - rh * 0.5f), r1(p0.x + trackW, cy + rh * 0.5f);
  skin::insetField(dl, r0, r1, C(col::Lcd), 2.0f);
  // Fill from the low end (or from zero on a range that spans it).
  const float zeroN = lo < 0 && hi > 0 && !logS ? toN(0) : 0.0f;
  const float fa = x0 + std::min(zeroN, n) * len, fb = x0 + std::max(zeroN, n) * len;
  if (fb - fa >= 1) dl->AddRectFilled(ImVec2(fa, r0.y + 1), ImVec2(fb, r1.y - 1), C(col::Wave, act || hov ? 1.0f : 0.8f), 1.0f);
  const float hx = std::round(x0 + n * len);
  const ImVec2 h0(hx - handleW * 0.5f, p0.y + 3), h1(hx + handleW * 0.5f, p0.y + h - 3);
  skin::raisedFace(dl, h0, h1, act ? C(col::BtnHi) : hov ? skin::lighten(C(col::Btn), 0.08f) : C(col::Btn), 2.0f);
  dl->AddLine(ImVec2(hx, h0.y + 4), ImVec2(hx, h1.y - 4), act ? C(col::Lit) : C(col::Glyph, 0.8f), 1.0f);

  // Value field.
  ImGui::SameLine(0, gap);
  const ImVec2 v0 = ImGui::GetCursorScreenPos(), v1(v0.x + valW, v0.y + h);
  ImGui::InvisibleButton("##value", ImVec2(valW, h));
  const bool vhov = ImGui::IsItemHovered();
  if (vhov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ImGui::OpenPopup("##typein");
  if (vhov) ui::Tooltip("Double-click to type a value");
  skin::insetField(dl, v0, v1, vhov ? skin::lighten(C(col::Lcd), 0.06f) : C(col::Lcd), theme::kRadius);
  char b[64];
  std::snprintf(b, sizeof b, fmt, static_cast<double>(*v));
  ImFont* fm = app().fontMono ? app().fontMono : ImGui::GetFont();
  const float fs = theme::kFontNumber;
  const ImVec2 ts = fm->CalcTextSizeA(fs, FLT_MAX, 0, b);
  dl->PushClipRect(v0, v1, true);
  dl->AddText(fm, fs, ImVec2(std::round(v1.x - theme::kS2 - ts.x), std::round(cy - ts.y * 0.5f)), C(col::LcdText), b);
  dl->PopClipRect();
  if (ImGui::BeginPopup("##typein")) {
    static float typed = 0;
    if (ImGui::IsWindowAppearing()) {
      typed = *v;
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(valW);
    if (ImGui::InputFloat("##t", &typed, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
      *v = std::clamp(typed, lo, hi);
      changed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

bool combo(const char* label, float col, int* v, const char* const* items, int n) {
  row(label, col);
  ImGui::PushID(label);
  const bool c = ImGui::Combo("##v", v, items, n);
  ImGui::PopID();
  return c;
}

// A dim note line inside a section (wraps at the section's edge).
void note(const char* text) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
}

// Preset buttons: one width, one gap; returns the clicked index or -1.
int presets(std::initializer_list<const char*> labels) {
  int k = 0, hit = -1;
  for (const char* l : labels) {
    if (k > 0) ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton(l, false, kBtnW)) hit = k;
    ++k;
  }
  return hit;
}

// What the tool applies to, above its section.
void scopeLine(App& a, const Session& s, Range tr) {
  char b[160];
  std::snprintf(b, sizeof b, i18n::tr("Applies to the %s (%s)"), s.sel.empty() ? i18n::tr("whole file") : i18n::tr("selection"),
                timeText(a, s, tr.len()).c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
  ImGui::TextUnformatted(b);
  ImGui::PopStyleColor();
}

// The footer: Cancel and Accept (primary), right-aligned. True on Accept.
bool footer(bool enabled = true) {
  ui::DialogFooter(ui::ButtonRowWidth(2, kBtnW));
  if (ui::TextButton("Cancel", false, kBtnW)) ImGui::CloseCurrentPopup();
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(!enabled);
  const bool ok = ui::TextButton("Accept", true, kBtnW);
  ImGui::EndDisabled();
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
  skin::insetField(dl, p0, p1, C(col::WaveBg), theme::kRadius);
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
}

// Small grid label (mono caption size) at a point.
void gridText(ImDrawList* dl, ImVec2 p, const char* t) {
  ImFont* f = app().fontMono ? app().fontMono : ImGui::GetFont();
  dl->AddText(f, theme::kFontLabel, ImVec2(std::round(p.x), std::round(p.y)), C(col::TextDim), t);
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
  const char* title = i18n::tr(toolTitle(s.tool));
  char id[96];
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
  static int drag = -1;
  switch (s.tool) {
    case Tool::Stretch: {
      scopeLine(a, s, tr);
      const float col = column({"Time", "Pitch", "Method", "Quality"});
      BeginSection("AMOUNT", kDlgW);
      slider("Time", col, &s.tStretch, 0.25f, 4.f, "x %.3f", ImGuiSliderFlags_Logarithmic);
      slider("Pitch", col, &s.tSemis, -24.f, 24.f, "%.2f semitones");
      const char* modes[] = {i18n::tr(stretchName(Stretch::Elastic)), i18n::tr(stretchName(Stretch::Tape)), i18n::tr(stretchName(Stretch::RubberBand))};
      s.tMode = std::clamp(s.tMode, 0, static_cast<int>(Stretch::Count) - 1);
      combo("Method", col, &s.tMode, modes, static_cast<int>(Stretch::Count));
      ui::Tooltip(stretch::modeHint(s.tMode == 1 ? stretch::Mode::Resample : s.tMode == 2 ? stretch::Mode::RubberBand : stretch::kStretch));
      const char* q[] = {i18n::tr("Fast"), i18n::tr("Normal"), i18n::tr("Best")};
      combo("Quality", col, &s.tQuality, q, 3);
      const double newLen = s.tMode == 1 ? tr.len() / std::pow(2.0, s.tSemis / 12.0) : tr.len() * static_cast<double>(s.tStretch);
      char b[160];
      std::snprintf(b, sizeof b, i18n::tr("New length %s%s"), timeText(a, s, static_cast<int64_t>(newLen)).c_str(),
                    s.tMode == 1 ? i18n::tr(" (resample: pitch sets speed)") : "");
      note(b);
      ui::EndSection();
      switch (presets({"Fit to bar", "Reset"})) {
        case 0: {
          const double bar = s.rate() * 60.0 / std::max(10.0, a.doc.song().tempo) * a.doc.song().timeSigNum;
          const double bars = std::max(1.0, std::round(tr.len() / bar));
          s.tStretch = static_cast<float>(bars * bar / std::max<double>(1, tr.len()));
          break;
        }
        case 1: s.tStretch = 1, s.tSemis = 0; break;
        default: break;
      }
      if (footer()) {
        const double f = s.tStretch, st = s.tSemis;
        const Stretch m = static_cast<Stretch>(s.tMode);
        const int qq = s.tQuality;
        runEdit(a, s, s.tMode == 1 ? "Varispeed" : "Time stretch / pitch shift", [tr, f, st, m, qq](const Content& c) { return stretch(c, tr, f, st, m, qq); });
      }
      break;
    }
    case Tool::Equalize: {
      scopeLine(a, s, tr);
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
      BeginSection("CURVE", kDlgWideW);
      pointGraph("eqgraph", pts, ImVec2(ImGui::GetContentRegionAvail().x, 200), toS, fromS, false, drag, [](ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
        for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
          float x = std::round(p0.x + static_cast<float>(std::log(hz / 20.0) / std::log(1000.0)) * (p1.x - p0.x)) + 0.5f;
          dl->AddLine(ImVec2(x, p0.y + 1), ImVec2(x, p1.y - 1), C(col::WaveGrid));
          char b[12];
          std::snprintf(b, sizeof b, hz >= 1000 ? "%.0fk" : "%.0f", hz >= 1000 ? hz / 1000 : hz);
          gridText(dl, ImVec2(x + 3, p1.y - theme::kFontLabel - 4), b);
        }
        for (int db : {-12, 0, 12}) {
          float y = std::round((p0.y + p1.y) * 0.5f - db / 24.f * (p1.y - p0.y) * 0.5f) + 0.5f;
          dl->AddLine(ImVec2(p0.x + 1, y), ImVec2(p1.x - 1, y), db == 0 ? C(col::WaveGridHi) : C(col::WaveGrid));
          char b[12];  // J-205: the gain scale on the left
          std::snprintf(b, sizeof b, "%+d dB", db);
          gridText(dl, ImVec2(p0.x + 5, y - theme::kFontLabel - 3), db == 0 ? "0 dB" : b);
        }
      });
      s.eq.clear();
      for (auto& p : pts) s.eq.push_back({p.first, p.second});
      note(i18n::tr("Drag points; double-click adds, right-click removes. +-24 dB, 20 Hz - 20 kHz."));
      ui::EndSection();
      switch (presets({"Flat", "Low cut", "Telephone"})) {
        case 0: s.eq = {{30, 0}, {120, 0}, {500, 0}, {2000, 0}, {8000, 0}, {16000, 0}}; break;
        case 1: s.eq = {{20, -24}, {60, -24}, {120, 0}, {16000, 0}}; break;
        case 2: s.eq = {{20, -24}, {300, -24}, {500, 0}, {2500, 3}, {3400, -24}, {20000, -24}}; break;
        default: break;
      }
      if (footer()) {
        std::vector<EqPoint> e = s.eq;
        runEdit(a, s, "Equalize", [tr, e](const Content& c) { return equalize(c, tr, e); });
      }
      break;
    }
    case Tool::Reverb: {
      scopeLine(a, s, tr);
      float d = static_cast<float>(s.rv.decay), pd = static_cast<float>(s.rv.predelayMs), dm = static_cast<float>(s.rv.damping),
            w = static_cast<float>(s.rv.width), wet = static_cast<float>(s.rv.wet), dry = static_cast<float>(s.rv.dry);
      const float col = column({"Decay", "Pre-delay", "Damping", "Width", "Wet", "Dry"});
      BeginSection("ROOM", kDlgW);
      slider("Decay", col, &d, 0.1f, 10.f, "%.2f s", ImGuiSliderFlags_Logarithmic);
      slider("Pre-delay", col, &pd, 0.f, 200.f, "%.0f ms");
      slider("Damping", col, &dm, 0.f, 1.f, "%.2f");
      slider("Width", col, &w, 0.f, 1.f, "%.2f");
      ui::EndSection();
      BeginSection("MIX", kDlgW);
      slider("Wet", col, &wet, 0.f, 1.f, "%.2f");
      slider("Dry", col, &dry, 0.f, 1.f, "%.2f");
      note(i18n::tr("The tail rings into the following audio (and past the end)."));
      ui::EndSection();
      s.rv.decay = d, s.rv.predelayMs = pd, s.rv.damping = dm, s.rv.width = w, s.rv.wet = wet, s.rv.dry = dry;
      if (footer()) {
        ReverbSettings rs = s.rv;
        runEdit(a, s, "Reverb", [tr, rs](const Content& c) { return reverb(c, tr, rs); });
      }
      break;
    }
    case Tool::Blur: {
      scopeLine(a, s, tr);
      const float col = column({"Amount", "Mix"});
      BeginSection("OPTIONS", kDlgW);
      slider("Amount", col, &s.blurMs, 2.f, 500.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
      slider("Mix", col, &s.blurMix, 0.f, 1.f, "%.2f");
      ui::EndSection();
      if (footer()) {
        const double ms = s.blurMs, mix = s.blurMix;
        runEdit(a, s, "Blur", [tr, ms, mix](const Content& c) { return blur(c, tr, ms, mix); });
      }
      break;
    }
    case Tool::Denoise: {
      scopeLine(a, s, tr);
      const float col = column({"Noise profile", "Reduction", "Sensitivity"});
      BeginSection("NOISE PROFILE", kDlgW);
      if (ui::TextButton(s.noise.valid() ? "Take again" : "Take from selection", false, ImGui::GetContentRegionAvail().x)) {
        s.noise = acquireNoise(s.cur().a(), tr);
        if (!s.noise.valid()) say(s, "Select at least 50 ms of noise first");
      }
      note(s.noise.valid() ? i18n::tr("Noise profile ready.") : i18n::tr("No profile yet: select a part with only noise first."));
      ui::EndSection();
      BeginSection("REDUCTION", kDlgW);
      slider("Reduction", col, &s.dnReduce, 3.f, 60.f, "%.0f dB");
      slider("Sensitivity", col, &s.dnSens, 0.f, 1.f, "%.2f");
      ui::EndSection();
      if (footer(s.noise.valid())) {
        NoiseProfile p = s.noise;
        const double r = s.dnReduce, se = s.dnSens;
        runEdit(a, s, "Clean up", [tr, p, r, se](const Content& c) { return denoise(c, tr, p, r, se); });
      }
      break;
    }
    case Tool::Gate: {
      scopeLine(a, s, tr);
      const float col = column({"Threshold", "Attack", "Hold", "Release", "Floor"});
      BeginSection("GATE", kDlgW);
      slider("Threshold", col, &s.gThr, -90.f, 0.f, "%.1f dB");
      slider("Attack", col, &s.gAtt, 0.f, 50.f, "%.1f ms");
      slider("Hold", col, &s.gHold, 0.f, 500.f, "%.0f ms");
      slider("Release", col, &s.gRel, 1.f, 1000.f, "%.0f ms", ImGuiSliderFlags_Logarithmic);
      slider("Floor", col, &s.gFloor, -90.f, 0.f, s.gFloor <= -90 ? i18n::tr("silence") : "%.0f dB");
      ui::EndSection();
      if (footer()) {
        const double t = s.gThr, at = s.gAtt, h = s.gHold, rl = s.gRel, fl = s.gFloor;
        runEdit(a, s, "Noise gate", [tr, t, at, h, rl, fl](const Content& c) { return gate(c, tr, t, at, h, rl, fl); });
      }
      break;
    }
    case Tool::Declick: {
      scopeLine(a, s, tr);
      const float col = column({"Sensitivity"});
      BeginSection("OPTIONS", kDlgW);
      slider("Sensitivity", col, &s.dcSens, 0.f, 1.f, "%.2f");
      ui::EndSection();
      if (footer()) {
        const double se = s.dcSens;
        runEdit(a, s, "Declick", [tr, se](const Content& c) { return declick(c, tr, se); });
      }
      break;
    }
    case Tool::Resample: {
      const int rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
      char cur[64];
      std::snprintf(cur, sizeof cur, i18n::tr("Current rate: %.0f Hz"), s.rate());
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
      ImGui::TextUnformatted(cur);
      ImGui::PopStyleColor();
      int idx = 0;
      for (int i = 0; i < 11; ++i)
        if (rates[i] == s.rsRate) idx = i;
      const char* names[] = {"8000 Hz", "11025 Hz", "16000 Hz", "22050 Hz", "32000 Hz", "44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz", "176400 Hz", "192000 Hz"};
      const float col = column({"New rate", "Quality"});
      BeginSection("CONVERSION", kDlgW);
      if (combo("New rate", col, &idx, names, 11)) s.rsRate = rates[idx];
      const char* q[] = {i18n::tr("Fast (16 taps)"), i18n::tr("Normal (64 taps)"), i18n::tr("Best (192 taps)")};
      combo("Quality", col, &s.rsQuality, q, 3);
      note(i18n::tr("Converts the whole file (markers move with it)."));
      ui::EndSection();
      if (footer(std::lround(s.rate()) != s.rsRate)) {
        const double nr = s.rsRate;
        const int qq = s.rsQuality;
        runEdit(a, s, "Resample", [nr, qq](const Content& c) { return resample(c, nr, qq); });
      }
      break;
    }
    case Tool::Amplify: {
      scopeLine(a, s, tr);
      const float col = column({"Gain"});
      BeginSection("LEVEL", kDlgW);
      slider("Gain", col, &s.ampDb, -48.f, 24.f, "%+.1f dB");
      ui::EndSection();
      if (footer()) {
        const double g = s.ampDb;
        runEdit(a, s, "Amplify", [tr, g](const Content& c) { return gain(c, tr, g); });
      }
      break;
    }
    case Tool::Normalize: {
      scopeLine(a, s, tr);
      const float col = column({"Peak level"});
      BeginSection("LEVEL", kDlgW);
      slider("Peak level", col, &s.normDb, -24.f, 0.f, "%.1f dBFS");
      ui::EndSection();
      if (footer()) {
        const double g = s.normDb;
        runEdit(a, s, "Normalize", [tr, g](const Content& c) { return normalize(c, tr, g); });
      }
      break;
    }
    case Tool::Envelope: {
      scopeLine(a, s, tr);
      auto toS = [](const std::pair<double, double>& p, ImVec2 p0, ImVec2 p1) {
        return ImVec2(p0.x + static_cast<float>(p.first) * (p1.x - p0.x), p1.y - static_cast<float>(p.second / 2.0) * (p1.y - p0.y));
      };
      auto fromS = [](ImVec2 m, ImVec2 p0, ImVec2 p1) {
        return std::make_pair(static_cast<double>((m.x - p0.x) / (p1.x - p0.x)), static_cast<double>((p1.y - m.y) / (p1.y - p0.y) * 2.0));
      };
      BeginSection("ENVELOPE", kDlgWideW);
      pointGraph("envgraph", s.env, ImVec2(ImGui::GetContentRegionAvail().x, 180), toS, fromS, true, drag, [](ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
        const float y1 = std::round(p1.y - 0.5f * (p1.y - p0.y)) + 0.5f;
        dl->AddLine(ImVec2(p0.x + 1, y1), ImVec2(p1.x - 1, y1), C(col::WaveGridHi));
        gridText(dl, ImVec2(p0.x + 5, y1 - theme::kFontLabel - 3), "0 dB");
      });
      note(i18n::tr("Gain across the range (0 to +6 dB). Double-click adds a point, right-click removes."));
      ui::EndSection();
      switch (presets({"Reset", "Swell", "Duck"})) {
        case 0: s.env = {{0, 1}, {1, 1}}; break;
        case 1: s.env = {{0, 0}, {0.7, 1}, {1, 1}}; break;
        case 2: s.env = {{0, 1}, {0.1, 0.3}, {0.6, 0.3}, {1, 1}}; break;
        default: break;
      }
      if (footer()) {
        auto e = s.env;
        runEdit(a, s, "Envelope", [tr, e](const Content& c) { return applyEnvelope(c, tr, e); });
      }
      break;
    }
    case Tool::Slice: {
      scopeLine(a, s, tr);
      const float col = column({"Sensitivity"});
      BeginSection("DETECTION", kDlgW);
      slider("Sensitivity", col, &s.sliceSens, 0.f, 1.f, "%.2f");
      note(i18n::tr("Places a marker at every detected hit (replaces the markers)."));
      ui::EndSection();
      if (footer()) detectSlices(a, s, s.sliceSens);
      break;
    }
    case Tool::Tune: {
      scopeLine(a, s, tr);
      static PitchInfo found;
      static uint64_t foundKey = ~0ull;
      const uint64_t k = s.hist.serial() * 31 + static_cast<uint64_t>(tr.a) * 7 + static_cast<uint64_t>(tr.b);
      if (k != foundKey) {
        Range pr{tr.a, std::min(tr.b, tr.a + static_cast<int64_t>(s.rate() * 2))};
        found = detectPitch(s.cur().a(), pr);
        foundKey = k;
      }
      const float col = column({"Detected", "Target"});
      BeginSection("PITCH", kDlgW);
      {
        char b[160];
        if (found.hz > 0)
          std::snprintf(b, sizeof b, i18n::tr("%.2f Hz = %s %+.0f cents (%.0f%%)"), found.hz, noteName(found.note).c_str(), found.cents, found.confidence * 100);
        else
          std::snprintf(b, sizeof b, "%s", i18n::tr("No clear pitch found."));
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
        ImGui::TextUnformatted(i18n::tr("Detected"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + col);
        ImGui::TextUnformatted(b);
      }
      std::vector<std::string> names = {i18n::tr("Nearest note")};
      for (int nn = 24; nn <= 96; ++nn) names.push_back(noteName(nn));
      const int idx = std::clamp(s.tuneNote < 0 ? 0 : s.tuneNote - 23, 0, static_cast<int>(names.size()) - 1);
      row("Target", col);
      if (ImGui::BeginCombo("##target", names[static_cast<size_t>(idx)].c_str())) {
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
          if (ImGui::Selectable(names[static_cast<size_t>(i)].c_str(), i == idx)) s.tuneNote = i == 0 ? -1 : i + 23;
        ImGui::EndCombo();
      }
      ui::EndSection();
      if (footer(found.hz > 0)) {
        const int note2 = s.tuneNote;
        runEdit(a, s, "Tune to note", [tr, note2](const Content& c) { return tuneToNote(c, tr, note2); });
      }
      break;
    }
    case Tool::Silence: {
      char b[120];
      std::snprintf(b, sizeof b, i18n::tr("Inserted at the cursor (%s)"), timeText(a, s, s.cursor).c_str());
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
      ImGui::TextUnformatted(b);
      ImGui::PopStyleColor();
      const float col = column({"Length"});
      BeginSection("SILENCE", kDlgW);
      slider("Length", col, &s.silenceSec, 0.01f, 30.f, "%.2f s", ImGuiSliderFlags_Logarithmic);
      ui::EndSection();
      if (footer()) {
        const int64_t at = s.cursor;
        const double sec = s.silenceSec;
        runEdit(a, s, "Insert silence", [at, sec](const Content& c) {
          Content base = c;
          if (c.frames() == 0 && !c.audio) base = makeContent(makeAudio(44100, 2, 0));
          return insertSilence(base, at, static_cast<int64_t>(sec * base.rate()));
        });
      }
      break;
    }
    case Tool::Spectral: {
      char b[96];
      std::snprintf(b, sizeof b, i18n::tr("%zu painted areas"), s.brush.size());
      ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::pal().textDim));
      ImGui::TextUnformatted(b);
      ImGui::PopStyleColor();
      const float col = column({"Gain"});
      BeginSection("BRUSH", kDlgW);
      slider("Gain", col, &s.spectralDb, -60.f, 12.f, "%+.0f dB");
      note(i18n::tr("Attenuate (negative) or boost the painted time / frequency areas."));
      ui::EndSection();
      if (footer(!s.brush.empty())) {
        auto rects = s.brush;
        const double g = s.spectralDb;
        runEdit(a, s, "Spectral edit", [rects, g](const Content& c) { return spectralGain(c, rects, g); });
        s.brush.clear();
      }
      break;
    }
    case Tool::Rename: {
      const float col = column({"Name"});
      BeginSection(s.renameRegion >= 0 ? "REGION" : "MARKER", kDlgW);
      row("Name", col);
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      const bool enter = ImGui::InputText("##name", s.renameBuf, sizeof s.renameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
      ui::EndSection();
      if (footer() || enter) {
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
      if (footer()) {
      }
      break;
  }
  ImGui::EndPopup();
}

}  // namespace gl::packs::edison
