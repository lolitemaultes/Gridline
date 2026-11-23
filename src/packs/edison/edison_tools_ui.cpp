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
#include "../../app/ProgressUi.h"
#include "../../app/Widgets.h"
#include "../../engine/TimeStretch.h"
#include "edison_dialog.h"
#include "edison_repair_ui.h"
#include "edison_ui.h"

namespace gl::packs::edison {

namespace dlg {

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
    case Tool::Deverb: return i18n::N_("Deverb (reduce room reverb)");
    case Tool::Declip: return i18n::N_("Declip (rebuild clipped peaks)");
    case Tool::DrumStretch: return i18n::N_("Drum loop stretch");
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
bool slider(const char* label, float col, float* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags) {
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

void statusNote(const char* text, int rows) {
  progui::WrappedRows(text, ImGui::GetContentRegionAvail().x, rows, theme::pal().textDim);
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
bool footer(bool enabled) {
  ui::DialogFooter(ui::ButtonRowWidth(2, kBtnW));
  if (ui::TextButton("Cancel", false, kBtnW)) ImGui::CloseCurrentPopup();
  ImGui::SameLine(0, theme::kS2);
  ImGui::BeginDisabled(!enabled);
  const bool ok = ui::PrimaryButton("Accept", kBtnW);
  ImGui::EndDisabled();
  if (ok) ImGui::CloseCurrentPopup();
  return ok;
}

}  // namespace dlg

using namespace dlg;

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
    if (!ImGui::IsPopupOpen(id)) {
      s.tool = Tool::None;
      repairToolClosed(a, s);  // its preview stops
    }
    return;
  }
  const Range tr = target(s);
  if (drawRepairDialog(a, s, tr)) {
    ImGui::EndPopup();
    return;
  }
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
        runEdit(a, s, s.tMode == 1 ? "Varispeed" : "Time stretch / pitch shift", [tr, f, st, m, qq](const Content& c) { return stretch(c, tr, f, st, m, qq); },
                EditMap::replace(tr));
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
        runEdit(a, s, "Resample", [nr, qq](const Content& c) { return resample(c, nr, qq); }, EditMap::scale());
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
        }, EditMap::replace({at, at}));
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
